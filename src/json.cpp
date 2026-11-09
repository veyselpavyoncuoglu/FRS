#include "json.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace {

	// a 4 MB body of [{},{},...] is a million values, about 100 bytes each once parsed. No MCP request comes near the cap
	const size_t MAXVALS = 20000;

	struct P {
		const char* p;
		const char* e;
		size_t n = 0;

		void ws() {
			while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
		}

		bool lit(const char* w) {
			size_t n = strlen(w);
			if ((size_t)(e - p) < n || strncmp(p,w,n) != 0) return false;
			p += n;
			return true;
		}

		bool hex4(unsigned& cp) {
			if (e - p < 4) return false;
			cp = 0;
			for (int i = 0; i < 4; i++) {
				char c = *p++;
				cp <<= 4;
				if (c >= '0' && c <= '9') cp |= c - '0';
				else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
				else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
				else return false;
			}
			return true;
		}

		static void utf8(std::string& out,unsigned cp) {
			if (cp < 0x80) out += (char)cp;
			else if (cp < 0x800) {
				out += (char)(0xC0 | (cp >> 6));
				out += (char)(0x80 | (cp & 0x3F));
			} else if (cp < 0x10000) {
				out += (char)(0xE0 | (cp >> 12));
				out += (char)(0x80 | ((cp >> 6) & 0x3F));
				out += (char)(0x80 | (cp & 0x3F));
			} else {
				out += (char)(0xF0 | (cp >> 18));
				out += (char)(0x80 | ((cp >> 12) & 0x3F));
				out += (char)(0x80 | ((cp >> 6) & 0x3F));
				out += (char)(0x80 | (cp & 0x3F));
			}
		}

		bool str(std::string& out) {
			if (p >= e || *p != '"') return false;
			p++;
			while (p < e && *p != '"') {
				char c = *p++;
				if (c != '\\') { out += c; continue; }
				if (p >= e) return false;
				c = *p++;
				switch (c) {
					case '"': case '\\': case '/': out += c; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case 'u': {
						unsigned cp;
						if (!hex4(cp)) return false;
						if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
							const char* save = p;
							p += 2;
							unsigned lo;
							if (hex4(lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
							else p = save;
						}
						utf8(out,cp);
						break;
					}
					default: return false;
				}
			}
			if (p >= e) return false;
			p++;
			return true;
		}

		bool val(Json& j,int depth) {
			if (depth > 64 || ++n > MAXVALS) return false;
			ws();
			if (p >= e) return false;
			char c = *p;
			if (c == '{') {
				p++;
				j = Json::obj();
				ws();
				if (p < e && *p == '}') { p++; return true; }
				while (true) {
					ws();
					std::string k;
					if (!str(k)) return false;
					ws();
					if (p >= e || *p != ':') return false;
					p++;
					Json v;
					if (!val(v,depth + 1)) return false;
					j.set(k,std::move(v));
					ws();
					if (p < e && *p == ',') { p++; continue; }
					if (p < e && *p == '}') { p++; return true; }
					return false;
				}
			}
			if (c == '[') {
				p++;
				j = Json::arr();
				ws();
				if (p < e && *p == ']') { p++; return true; }
				while (true) {
					Json v;
					if (!val(v,depth + 1)) return false;
					j.push(std::move(v));
					ws();
					if (p < e && *p == ',') { p++; continue; }
					if (p < e && *p == ']') { p++; return true; }
					return false;
				}
			}
			if (c == '"') {
				j = Json("");
				return str(j.s);
			}
			if (lit("true")) { j = Json(true); return true; }
			if (lit("false")) { j = Json(false); return true; }
			if (lit("null")) { j = Json(); return true; }
			if (c == '-' || dig(c)) {
				// JSON's own grammar first : strtod alone also reads 0x7c, inf and nan
				const char* q = p;
				if (*q == '-') q++;
				if (q >= e || !dig(*q)) return false;
				if (*q == '0') q++;
				else while (q < e && dig(*q)) q++;
				if (q < e && *q == '.') {
					if (++q >= e || !dig(*q)) return false;
					while (q < e && dig(*q)) q++;
				}
				if (q < e && (*q == 'e' || *q == 'E')) {
					if (++q < e && (*q == '+' || *q == '-')) q++;
					if (q >= e || !dig(*q)) return false;
					while (q < e && dig(*q)) q++;
				}
				j = Json(strtod(std::string(p,q).c_str(),nullptr));
				p = q;
				return true;
			}
			return false;
		}

		static bool dig(char c) { return c >= '0' && c <= '9'; }
	};

	// bytes of the valid UTF-8 char at s[i], 0 when they are junk
	size_t u8(const std::string& s,size_t i) {
		unsigned char c = (unsigned char)s[i];
		size_t n = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
		if (!n || i + n > s.size()) return 0;
		for (size_t k = 1; k < n; k++) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
		unsigned char d = (unsigned char)s[i + 1];
		if ((c == 0xE0 && d < 0xA0) || (c == 0xED && d > 0x9F) || (c == 0xF0 && d < 0x90) || (c == 0xF4 && d > 0x8F)) return 0;
		return n;
	}

	// JSON text is UTF-8 : bytes that aren't (a non UTF-8 name, a client's junk echoed in an error) become U+FFFD
	void esc(std::string& out,const std::string& s) {
		out += '"';
		for (size_t i = 0; i < s.size(); i++) {
			unsigned char c = (unsigned char)s[i];
			if (c >= 0x80) {
				size_t n = u8(s,i);
				if (n) {
					out.append(s,i,n);
					i += n - 1;
				} else out += "\\ufffd";
				continue;
			}
			switch (c) {
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (c < 0x20) {
						char b[8];
						snprintf(b,sizeof(b),"\\u%04x",c);
						out += b;
					} else out += (char)c;
			}
		}
		out += '"';
	}

	void dumpTo(std::string& out,const Json& j) {
		switch (j.t) {
			case Json::Null: out += "null"; break;
			case Json::Bool: out += j.b ? "true" : "false"; break;
			case Json::Num: {
				char b[32];
				if (!std::isfinite(j.n)) snprintf(b,sizeof(b),"null");
				else if (j.n == std::floor(j.n) && std::fabs(j.n) < 1e15) snprintf(b,sizeof(b),"%lld",(long long)j.n);
				else snprintf(b,sizeof(b),"%.10g",j.n);
				out += b;
				break;
			}
			case Json::Str: esc(out,j.s); break;
			case Json::Arr:
				out += '[';
				for (size_t i = 0; i < j.a.size(); i++) {
					if (i) out += ',';
					dumpTo(out,j.a[i]);
				}
				out += ']';
				break;
			case Json::Obj:
				out += '{';
				for (size_t i = 0; i < j.o.size(); i++) {
					if (i) out += ',';
					esc(out,j.o[i].first);
					out += ':';
					dumpTo(out,j.o[i].second);
				}
				out += '}';
				break;
		}
	}

}

const Json* Json::get(const char* k) const {
	if (t != Obj) return nullptr;
	for (auto& kv : o) if (kv.first == k) return &kv.second;
	return nullptr;
}

double Json::num(const char* k,double def) const {
	const Json* v = get(k);
	return (v && v->t == Num) ? v->n : def;
}

bool Json::flag(const char* k,bool def) const {
	const Json* v = get(k);
	return (v && v->t == Bool) ? v->b : def;
}

std::string Json::str(const char* k,const std::string& def) const {
	const Json* v = get(k);
	return (v && v->t == Str) ? v->s : def;
}

bool Json::parse(const std::string& src,Json& out) {
	P ps{src.c_str(),src.c_str() + src.size()};
	if (!ps.val(out,0)) return false;
	ps.ws();
	return ps.p == ps.e;
}

std::string Json::dump() const {
	std::string out;
	dumpTo(out,*this);
	return out;
}
