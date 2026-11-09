#include "vmscan.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

namespace vmscan {

	const Preset kPresets[] = {
		{"minimal","Minimal","Threads, modules and data segments. Small, no heap.",0x1 | 0x20 | 0x1000 | 0x20000},
		{"limited","Limited","Full memory without handle data.",0x2 | 0x20 | 0x800 | 0x1000 | 0x20000},
		{"normal","Normal","Memory and handles (like Task Manager).",0x2 | 0x4 | 0x20 | 0x800 | 0x1000 | 0x20000 | 0x400000},
		{"full","Full","Normal plus data and code segments, thread, token and module header info, auxiliary state. Can be many GB.",0x1 | 0x2 | 0x4 | 0x20 | 0x40 | 0x100 | 0x200 | 0x800 | 0x2000 | 0x8000 | 0x10000 | 0x20000 | 0x40000 | 0x80000 | 0x200000 | 0x400000},
	};
	const int kPresetN = 4;

	const DFlag kFlags[] = {
		{"normal","Normal",0,true},
		{"data_segments","With data segments",0x1,true},
		{"full_memory","With full memory",0x2,true},
		{"handle_data","With handle data",0x4,true},
		{"unloaded_modules","With unloaded modules",0x20,true},
		{"private_read_write","With private read-write memory",0x200,true},
		{"full_memory_info","With full memory info",0x800,true},
		{"thread_info","With thread info",0x1000,true},
		{"ignore_inaccessible","Ignore inaccessible memory",0x20000,true},
		{"indirect_memory","With indirectly referenced memory",0x40,false},
		{"process_thread_data","With process and thread data",0x100,false},
		{"code_segments","With code segments",0x2000,false},
		{"full_aux_state","With full auxiliary state",0x8000,false},
		{"private_write_copy","With private write-copy memory",0x10000,false},
		{"token","With token information",0x40000,false},
		{"module_headers","With module headers",0x80000,false},
		{"avx_context","With AVX state context registers",0x200000,false},
		{"ipt_trace","With Intel processor trace data",0x400000,false},
	};
	const int kFlagN = sizeof(kFlags) / sizeof(kFlags[0]);

	void Prog::say(const std::string& t,float f) {
		if (f > frac.load()) frac = f;
		std::lock_guard<std::mutex> lk(m);
		msg = t;
	}

	std::string Prog::text() {
		std::lock_guard<std::mutex> lk(m);
		return msg;
	}

	const char* encName(Enc e) {
		switch (e) {
			case EAnsi: return "ANSI";
			case EUtf16: return "Unicode";
			case EUtf8: return "UTF-8";
			case EBytes: return "Bytes";
			default: return "Regex";
		}
	}

	const char* kindName(int k) {
		return k == KImage ? "Image" : k == KMapped ? "Mapped" : "Private";
	}

	bool parsePat(const std::string& txt,std::vector<uint8_t>& out,std::string& err) {
		out.clear();
		int nib = -1;
		for (size_t i = 0; i < txt.size(); i++) {
			char c = txt[i];
			if (c == ' ' || c == '\t' || c == ',') {
				if (nib >= 0) {
					err = "odd number of hex digits";
					return false;
				}
				continue;
			}
			if (c == '0' && i + 1 < txt.size() && (txt[i + 1] == 'x' || txt[i + 1] == 'X') && nib < 0) {
				i++;
				continue;
			}
			int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			if (v < 0) {
				err = "pattern is hex bytes like 4D 5A, no other characters";
				return false;
			}
			if (nib < 0) nib = v;
			else {
				out.push_back((uint8_t)(nib << 4 | v));
				nib = -1;
			}
		}
		if (nib >= 0) {
			err = "odd number of hex digits";
			return false;
		}
		if (out.empty()) {
			err = "pattern is empty";
			return false;
		}
		return true;
	}

	// ---- regex ----

	namespace {

		enum { OpSet,OpSplit,OpJmp,OpMatch,OpBol,OpEol,OpWord,OpNoWord };
		const size_t MAXINS = 4000;
		using Code = std::vector<Rx::Ins>;

		bool isWord(uint8_t c) {
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
		}

		struct Parser {
			const std::string& s;
			bool ic;
			std::vector<std::bitset<256>>& sets;
			size_t i = 0;
			int depth = 0;
			std::string err;

			bool fail(const char* m) {
				if (err.empty()) err = m;
				return false;
			}

			void app(Code& a,const Code& b) {
				if (a.size() + b.size() > MAXINS) {
					fail("pattern is too large");
					return;
				}
				a.insert(a.end(),b.begin(),b.end());
			}

			void fold(std::bitset<256>& b) {
				if (ic) for (int c = 'a'; c <= 'z'; c++) {
					bool on = b[c] || b[c - 32];
					b[c] = on;
					b[c - 32] = on;
				}
			}

			int addSet(std::bitset<256> b) {
				fold(b);
				sets.push_back(b);
				return (int)sets.size() - 1;
			}

			bool classEsc(char c,std::bitset<256>& b) {
				std::bitset<256> t;
				char l = (char)(c | 32);
				if (l == 'd') for (int x = '0'; x <= '9'; x++) t[x] = true;
				else if (l == 'w') for (int x = 0; x < 256; x++) t[x] = isWord((uint8_t)x);
				else if (l == 's') t[' '] = t['\t'] = t['\n'] = t['\r'] = t['\f'] = t['\v'] = true;
				else return false;
				if (c < 'a') t.flip();
				b |= t;
				return true;
			}

			bool litEsc(int& v) {
				char c = s[i++];
				switch (c) {
					case 'n': v = '\n'; return true;
					case 't': v = '\t'; return true;
					case 'r': v = '\r'; return true;
					case 'f': v = '\f'; return true;
					case 'v': v = '\v'; return true;
					case '0': v = 0; return true;
					case 'x': {
						int n = 0;
						for (int k = 0; k < 2; k++) {
							char h = i < s.size() ? s[i] : 0;
							int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
							if (d < 0) return fail("\\x needs two hex digits");
							n = n * 16 + d;
							i++;
						}
						v = n;
						return true;
					}
				}
				if ((c >= '1' && c <= '9')) return fail("back references are not supported");
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return fail("unknown escape");
				v = (uint8_t)c;
				return true;
			}

			bool cls(std::bitset<256>& b) {
				bool neg = i < s.size() && s[i] == '^';
				if (neg) i++;
				bool first = true;
				while (true) {
					if (i >= s.size()) return fail("missing ]");
					char c = s[i];
					if (c == ']' && !first) {
						i++;
						break;
					}
					first = false;
					int lo;
					i++;
					if (c == '\\') {
						if (i >= s.size()) return fail("trailing backslash");
						if (classEsc(s[i],b)) {
							i++;
							continue;
						}
						if (!litEsc(lo)) return false;
					} else lo = (uint8_t)c;
					int hi = lo;
					if (i + 1 < s.size() && s[i] == '-' && s[i + 1] != ']') {
						i++;
						char d = s[i++];
						if (d == '\\') {
							if (i >= s.size()) return fail("trailing backslash");
							if (!litEsc(hi)) return false;
						} else hi = (uint8_t)d;
						if (hi < lo) return fail("bad range in []");
					}
					for (int x = lo; x <= hi; x++) b[x] = true;
				}
				fold(b); // before the flip, or an excluded letter comes back through its other case
				if (neg) b.flip();
				return true;
			}

			Code one(std::bitset<256> b) {
				return Code{{OpSet,addSet(b),0}};
			}

			Code atom() {
				char c = s[i++];
				std::bitset<256> b;
				switch (c) {
					case '(': {
						if (i + 1 < s.size() && s[i] == '?') {
							if (s[i + 1] != ':') {
								fail("only (?: is supported after (");
								return {};
							}
							i += 2;
						}
						if (++depth > 64) {
							fail("groups nest too deep");
							return {};
						}
						Code r = alt();
						depth--;
						if (i >= s.size() || s[i] != ')') {
							fail("missing )");
							return {};
						}
						i++;
						return r;
					}
					case '[':
						if (!cls(b)) return {};
						return one(b);
					case '.':
						b.set();
						b['\n'] = false;
						return one(b);
					case '^': return Code{{OpBol,0,0}};
					case '$': return Code{{OpEol,0,0}};
					case '\\':
						if (i >= s.size()) {
							fail("trailing backslash");
							return {};
						}
						if (s[i] == 'b' || s[i] == 'B') return Code{{s[i++] == 'b' ? (uint8_t)OpWord : (uint8_t)OpNoWord,0,0}};
						if (classEsc(s[i],b)) {
							i++;
							return one(b);
						}
						{
							int v;
							if (!litEsc(v)) return {};
							b[v] = true;
						}
						return one(b);
					case '*': case '+': case '?':
						fail("nothing to repeat");
						return {};
				}
				b[(uint8_t)c] = true;
				return one(b);
			}

			bool braces(int& lo,int& hi) {
				size_t k = i + 1;
				int a = 0,na = 0,b = -1;
				while (k < s.size() && s[k] >= '0' && s[k] <= '9' && a < 100000) {
					a = a * 10 + (s[k++] - '0');
					na++;
				}
				if (!na) return false;
				if (k < s.size() && s[k] == ',') {
					k++;
					int nb = 0;
					int v = 0;
					while (k < s.size() && s[k] >= '0' && s[k] <= '9' && v < 100000) {
						v = v * 10 + (s[k++] - '0');
						nb++;
					}
					b = nb ? v : -1;
				} else b = a;
				if (k >= s.size() || s[k] != '}') return false;
				i = k + 1;
				lo = a;
				hi = b;
				return true;
			}

			Code star(const Code& e) {
				Code r;
				r.push_back({OpSplit,1,(int)e.size() + 2});
				app(r,e);
				r.push_back({OpJmp,-((int)e.size() + 1),0});
				return r;
			}

			Code rep() {
				Code e = atom();
				while (err.empty() && i < s.size()) {
					char c = s[i];
					int lo = 0,hi = -1;
					if (c == '*') i++;
					else if (c == '+') {
						lo = 1;
						i++;
					} else if (c == '?') {
						hi = 1;
						i++;
					} else if (c != '{' || !braces(lo,hi)) break;
					if (hi != -1 && hi < lo) {
						fail("bad {} range");
						break;
					}
					if (i < s.size() && s[i] == '?') i++; // lazy makes no difference, matches are longest
					if ((size_t)lo * e.size() > MAXINS || (hi > 0 && (size_t)hi * e.size() > MAXINS)) {
						fail("pattern is too large");
						break;
					}
					Code r;
					for (int k = 0; k < lo; k++) app(r,e);
					if (hi == -1) app(r,star(e));
					else {
						Code tail;
						for (int k = lo; k < hi && err.empty(); k++) {
							Code t;
							t.push_back({OpSplit,1,(int)(e.size() + tail.size()) + 1});
							app(t,e);
							app(t,tail);
							tail.swap(t);
						}
						app(r,tail);
					}
					e.swap(r);
				}
				return e;
			}

			Code seq() {
				Code r;
				while (err.empty() && i < s.size() && s[i] != '|' && s[i] != ')') app(r,rep());
				return r;
			}

			Code alt() {
				Code a = seq();
				while (err.empty() && i < s.size() && s[i] == '|') {
					i++;
					Code b = seq();
					Code r;
					r.push_back({OpSplit,1,(int)a.size() + 2});
					app(r,a);
					r.push_back({OpJmp,(int)b.size() + 1,0});
					app(r,b);
					a.swap(r);
				}
				return a;
			}
		};

		struct Thr {
			int pc;
			size_t start;
		};

		struct Sim {
			std::vector<int> mark,stack;
			std::vector<Thr> c,n;
			int gen = 0;
		};

	}

	bool Rx::compile(const std::string& pat,bool icase,std::string& err) {
		prog.clear();
		sets.clear();
		useFirst = false;
		first.reset();
		std::vector<std::bitset<256>> st;
		Parser p{pat,icase,st};
		Code c = p.alt();
		if (p.err.empty() && p.i < pat.size()) p.fail("unmatched )");
		if (p.err.empty() && c.size() + 1 > MAXINS) p.fail("pattern is too large");
		if (!p.err.empty()) {
			err = p.err;
			return false;
		}
		c.push_back({OpMatch,0,0});
		prog.swap(c);
		sets.swap(st);
		// what a match can start with, lets the scan skip bytes while no thread runs
		std::vector<char> seen(prog.size(),0);
		std::vector<int> stk{0};
		useFirst = true;
		while (!stk.empty()) {
			int pc = stk.back();
			stk.pop_back();
			if (seen[pc]) continue;
			seen[pc] = 1;
			const Ins& in = prog[pc];
			switch (in.op) {
				case OpSet: first |= sets[in.x]; break;
				case OpSplit:
					stk.push_back(pc + in.x);
					stk.push_back(pc + in.y);
					break;
				case OpJmp: stk.push_back(pc + in.x); break;
				case OpMatch: useFirst = false; break;
				default: stk.push_back(pc + 1); break;
			}
		}
		return true;
	}

	bool Rx::run(const uint8_t* s,size_t n,size_t from,size_t maxLen,bool any,size_t& ob,size_t& oe) const {
		// not thread_local : on MinGW its destructor runs after emutls freed the object, a use after free at every thread exit
		Sim m;
		m.mark.assign(prog.size(),0);
		m.gen++;
		auto add = [&](std::vector<Thr>& l,int pc0,size_t st,size_t pos) {
			m.stack.clear();
			m.stack.push_back(pc0);
			while (!m.stack.empty()) {
				int pc = m.stack.back();
				m.stack.pop_back();
				if (m.mark[pc] == m.gen) continue;
				m.mark[pc] = m.gen;
				const Ins& in = prog[pc];
				switch (in.op) {
					case OpJmp: m.stack.push_back(pc + in.x); break;
					case OpSplit:
						m.stack.push_back(pc + in.y);
						m.stack.push_back(pc + in.x);
						break;
					case OpBol: if (pos == 0) m.stack.push_back(pc + 1); break;
					case OpEol: if (pos == n) m.stack.push_back(pc + 1); break;
					case OpWord:
					case OpNoWord: {
						bool a = pos > 0 && isWord(s[pos - 1]);
						bool b = pos < n && isWord(s[pos]);
						if ((a != b) == (in.op == OpWord)) m.stack.push_back(pc + 1);
						break;
					}
					default: l.push_back({pc,st}); break;
				}
			}
		};
		bool best = false;
		size_t bs = 0,be = 0;
		for (size_t pos = from;; pos++) {
			if (!best) {
				if (m.c.empty() && useFirst) {
					while (pos < n && !first[s[pos]]) pos++;
					if (pos >= n) break;
				}
				add(m.c,0,pos,pos);
			}
			m.n.clear();
			m.gen++;
			for (const Thr& t : m.c) {
				const Ins& in = prog[t.pc];
				if (in.op == OpMatch) {
					if (!any && pos == t.start) continue;
					if (!best || t.start < bs || (t.start == bs && pos > be)) {
						best = true;
						bs = t.start;
						be = pos;
					}
					if (any) {
						ob = bs;
						oe = be;
						return true;
					}
					continue;
				}
				if (best && t.start > bs) continue;
				if (pos >= n || (maxLen && pos + 1 - t.start > maxLen)) continue;
				if (sets[in.x][s[pos]]) add(m.n,t.pc + 1,t.start,pos + 1);
			}
			m.c.swap(m.n);
			if (pos >= n || (best && m.c.empty())) break;
		}
		if (best) {
			ob = bs;
			oe = be;
		}
		return best;
	}

	bool Rx::match(const char* s,size_t n) const {
		size_t b,e;
		return ok() && run((const uint8_t*)s,n,0,0,true,b,e);
	}

	bool Rx::find(const uint8_t* s,size_t n,size_t from,size_t maxLen,size_t& b,size_t& e) const {
		return ok() && run(s,n,from,maxLen,false,b,e);
	}

	// ---- string scanner ----

	namespace {

		enum { TNone,TPrint,TNull,THigh };
		const size_t CHUNK = (size_t)4 << 20;
		const size_t RXWIN = 4096;
		const size_t RXCHUNK = (size_t)64 << 10; // the worst pattern costs about a second for this much, stop is checked between chunks

		// NtReadVirtualMemory copies 14 pages at a time and gives 0 when the first lot holds a bad page, so the good pages in front of it need their own read
		size_t readOn(os::Proc& p,uint64_t a,void* b,size_t n) {
			size_t got = os::read(p,a,b,n);
			size_t pg = (size_t)(4096 - (a & 4095));
			return !got && n > pg ? os::read(p,a,b,pg) : got;
		}

		bool printable(int b,bool ext) {
			if (b == 9 || b == 10 || b == 13) return true;
			if (b >= 32 && b <= 126) return true;
			return ext && b >= 160;
		}

		struct Table {
			uint8_t t[2][256];
			uint8_t act[64]; // no run open : what the last three byte types start. 1 ASCII, 2 UTF-16, 3 UTF-16 non Latin
			Table() {
				for (int e = 0; e < 2; e++) for (int b = 0; b < 256; b++) {
					if (!b) t[e][b] = TNull;
					else if (printable(b,e != 0)) t[e][b] = TPrint;
					else if (e && !(b >= 0xD8 && b <= 0xDF)) t[e][b] = THigh;
					else t[e][b] = TNone;
				}
				for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int c = 0; c < 4; c++) {
					int r = 0;
					if (a == TPrint && b == TPrint && c == TPrint) r = 1;
					else if (a == TPrint && b == TNull && c == TPrint) r = 2;
					else if ((a == THigh || a == TNull) && b != THigh && c == THigh) r = 3;
					act[a << 4 | b << 2 | c] = (uint8_t)r;
				}
			}
		};

		const Table& table() {
			static const Table tab;
			return tab;
		}

		void putCp(std::string& o,uint32_t c) {
			if (c < 0x80) o += (char)c;
			else if (c < 0x800) {
				o += (char)(0xC0 | c >> 6);
				o += (char)(0x80 | (c & 63));
			} else {
				o += (char)(0xE0 | c >> 12);
				o += (char)(0x80 | ((c >> 6) & 63));
				o += (char)(0x80 | (c & 63));
			}
		}

		// ASCII and UTF-16 state machine from phlib/strsrch.c PhpSearchStrings, and its UTF-8 pass. Both keep their
		// state between chunks, so a string across a chunk edge stays whole, and finish() emits a run that reaches
		// the end of a region. SI loses both of those.
		struct Strs {
			const Cfg& c;
			std::vector<Hit>& out;
			uint32_t reg = 0;
			const uint8_t* tb;
			const uint8_t* tp; // the plain table, the UTF-8 pass ignores extended
			const uint8_t* act;
			uint8_t b1 = 0,b2 = 0;
			uint8_t t1 = TNone,t2 = TNone,t3 = TNone;
			int pat = 0; // 1 ASCII, 2 UTF-16
			uint64_t len = 0,start = 0;
			std::vector<uint16_t> buf;
			size_t nb = 0;
			// UTF-8
			bool uIn = false,uHi = false,uFirst = false;
			uint64_t uStart = 0,uChars = 0,uBytes = 0;
			int uNeed = 0,uSeq = 0;
			uint8_t uLead = 0;
			char uPend[4];
			std::string uTxt;

			Strs(const Cfg& cf,std::vector<Hit>& o) : c(cf),out(o) {
				const Table& tab = table();
				tb = tab.t[c.ext ? 1 : 0];
				tp = tab.t[0];
				act = tab.act;
				buf.resize(c.textCap);
			}

			void add(uint16_t v) {
				if (nb < buf.size()) buf[nb++] = v;
				len++;
			}

			void flushRun() {
				if (pat && (int64_t)len >= c.minLen) {
					Hit h;
					h.addr = start;
					h.chars = len;
					h.bytes = pat == 1 ? len : len * 2;
					h.enc = pat == 1 ? EAnsi : EUtf16;
					h.reg = reg;
					std::string t;
					t.reserve(nb);
					for (size_t i = 0; i < nb; i++) putCp(t,buf[i]);
					h.text = util::safe(t);
					out.push_back(std::move(h));
				}
				pat = 0;
				len = 0;
				nb = 0;
			}

			void flushU8() {
				if (uIn && uHi && (int64_t)uChars >= c.minLen) {
					Hit h;
					h.addr = uStart;
					h.chars = uChars;
					h.bytes = uBytes;
					h.enc = EUtf8;
					h.reg = reg;
					h.text = util::safe(uTxt);
					out.push_back(std::move(h));
				}
				uIn = uHi = false;
				uNeed = 0;
				uChars = uBytes = 0;
				uTxt.clear();
			}

			void step(uint8_t b,uint64_t pos) {
				uint8_t t = tb[b];
				if (t == THigh && (t1 == THigh || t1 == TNull)) t = TNone;
				if (!pat) {
					switch (act[t2 << 4 | t1 << 2 | t]) {
						case 1:
							pat = 1;
							start = pos - 2;
							add(b2);
							add(b1);
							add(b);
							break;
						case 2:
							pat = 2;
							start = pos - 2;
							add(b2);
							add(b);
							break;
						case 3:
							pat = 2;
							start = pos - 1;
							add((uint16_t)(b1 | b << 8));
							break;
					}
				} else if (t2 == TPrint && t1 == TPrint && t == TPrint) {
					if (pat == 1) add(b);
					else flushRun();
				} else if (t2 == TPrint && t1 == TNull && t == TPrint) {
					if (pat == 2) add(b);
					else flushRun();
				} else if (pat == 2 && t2 == TNull && t1 == TPrint && t == TNull) {
				} else if (pat == 2 && t3 == TNull && t2 == TPrint && t1 == TNull && t == TNone) {
				} else if ((t2 == THigh || t2 == TNull) && t1 != THigh && t == THigh) {
					if (pat == 2) add((uint16_t)(b1 | b << 8));
					else flushRun();
				} else if (pat == 2 && (t3 == THigh || t3 == TNull) && t2 != THigh && t1 == THigh && t != THigh) {
				} else if (pat == 2 && t3 != THigh && t2 == THigh && t1 == TPrint && t == TNull) {
					add(b1);
				} else flushRun();
				b2 = b1;
				b1 = b;
				t3 = t2;
				t2 = t1;
				t1 = t;
			}

			void stepU8(uint8_t b,uint64_t pos) {
				if (uNeed) {
					bool ok = (b & 0xC0) == 0x80;
					if (ok && uFirst) {
						if ((uLead == 0xE0 && b < 0xA0) || (uLead == 0xED && b >= 0xA0) || (uLead == 0xF0 && b < 0x90) || (uLead == 0xF4 && b >= 0x90)) ok = false;
					}
					if (ok) {
						uPend[uSeq++] = (char)b;
						uFirst = false;
						if (--uNeed == 0) {
							if (uChars < c.textCap) uTxt.append(uPend,(size_t)uSeq);
							uChars++;
							uBytes += (uint64_t)uSeq;
							uHi = true;
						}
						return;
					}
					flushU8();
				}
				if (b < 0x80) {
					if (tp[b] == TPrint) {
						if (!uIn) {
							uIn = true;
							uStart = pos;
						}
						if (uChars < c.textCap) uTxt += (char)b;
						uChars++;
						uBytes++;
					} else if (uIn) flushU8();
					return;
				}
				if (b >= 0xC2 && b <= 0xF4) {
					if (!uIn) {
						uIn = true;
						uStart = pos;
					}
					uLead = b;
					uFirst = true;
					uSeq = 1;
					uPend[0] = (char)b;
					uNeed = b <= 0xDF ? 1 : b <= 0xEF ? 2 : 3;
					return;
				}
				if (uIn) flushU8();
			}

			void feed(const uint8_t* p,size_t n,uint64_t addr) {
				for (size_t i = 0; i < n;) {
					// most memory is zero, skip it fast while nothing is open
					if (!pat && !uIn && t1 == TNull && t2 == TNull && t3 == TNull && n - i >= 8) {
						uint64_t v;
						memcpy(&v,p + i,8);
						if (!v) {
							i += 8;
							continue;
						}
					}
					step(p[i],addr + i);
					stepU8(p[i],addr + i);
					i++;
				}
			}

			void finish() {
				flushRun();
				flushU8();
				b1 = b2 = 0;
				t1 = t2 = t3 = TNone;
			}

			// a scan from here finds what is open now whole. Non Latin UTF-16 only starts after the byte in front of it
			uint64_t openAt() const {
				uint64_t o = UINT64_MAX;
				if (pat) o = start - (pat == 2 && c.ext ? 1 : 0);
				if (uIn) o = std::min(o,uStart);
				return o;
			}
		};

		std::string hexView(const uint8_t* p,size_t n) {
			static const char* d = "0123456789ABCDEF";
			std::string o;
			o.reserve(n * 3 + n + 2);
			for (size_t i = 0; i < n; i++) {
				if (i) o += ' ';
				o += d[p[i] >> 4];
				o += d[p[i] & 15];
			}
			o += "   ";
			for (size_t i = 0; i < n; i++) o += p[i] >= 32 && p[i] < 127 ? (char)p[i] : '.';
			return o;
		}

		std::string rawText(const uint8_t* p,size_t n,size_t cap) {
			std::string o;
			n = std::min(n,cap);
			o.reserve(n);
			for (size_t i = 0; i < n; i++) o += p[i] >= 32 && p[i] < 127 ? (char)p[i] : '.';
			return o;
		}

		struct Worker {
			os::Proc& p;
			const Cfg& c;
			Scan& s;
			const StopFn& stop;
			const std::vector<Region>& regs;
			uint64_t t0;
			std::vector<uint8_t> buf;
			std::vector<Hit> local;
			Strs st;
			uint32_t curReg = 0;
			size_t keep = 0; // bytes of the last chunk still at the front of buf
			size_t rxFrom = 0;
			uint64_t lastAddr = 0;

			Worker(os::Proc& pr,const Cfg& cf,Scan& sc,const StopFn& sf,const std::vector<Region>& rg,uint64_t t) : p(pr),c(cf),s(sc),stop(sf),regs(rg),t0(t),st(cf,local) {}

			void halt(const char* why,uint64_t at) {
				std::lock_guard<std::mutex> lk(s.m);
				if (s.why.empty()) {
					s.why = why;
					s.next = at;
				}
				s.stop = true;
			}

			bool halted(uint64_t at) {
				if (s.stop.load()) return true;
				const char* why = stop ? stop() : nullptr;
				if (!why && c.maxMs && util::nowMs() - t0 >= c.maxMs) why = "deadline";
				if (!why && c.maxBytes && s.scanned.load() >= c.maxBytes) why = "byte budget";
				if (!why) return false;
				halt(why,at);
				return true;
			}

			void flush() {
				if (local.empty()) return;
				auto byAddr = [](const Hit& a,const Hit& b) { return a.addr < b.addr; };
				if (!std::is_sorted(local.begin(),local.end(),byAddr)) std::stable_sort(local.begin(),local.end(),byAddr);
				if (!c.contains.empty() || c.encMask != 31) {
					local.erase(std::remove_if(local.begin(),local.end(),[&](const Hit& h) {
						return !(c.encMask >> h.enc & 1) || (!c.contains.empty() && !util::has(h.text,c.contains));
					}),local.end());
				}
				// a hit waits for every run that started before it : a stop drops that run and next goes back to it, the hit comes again
				uint64_t lim = c.mode == MStrings ? st.openAt() : UINT64_MAX;
				std::lock_guard<std::mutex> lk(s.m);
				size_t i = 0;
				for (; i < local.size() && local[i].addr < lim; i++) {
					Hit& h = local[i];
					// hits at one address stay together (an ASCII piece and the UTF-8 run around it), or a resume could never get past them
					if (h.addr != lastAddr && ((c.maxHits && s.hitN.load() >= c.maxHits) || (c.maxText && s.textN.load() >= c.maxText))) {
						if (s.why.empty()) {
							s.why = "result limit";
							s.next = h.addr - (h.enc == EUtf16 && c.ext ? 1 : 0);
						}
						s.stop = true;
						local.clear();
						return;
					}
					lastAddr = h.addr;
					h.idx = ++s.idxN;
					s.textN += h.text.size();
					s.pending.push_back(std::move(h));
					s.hitN++;
				}
				local.erase(local.begin(),local.begin() + (std::ptrdiff_t)i);
			}

			void addHit(uint64_t addr,uint64_t len,Enc e,std::string text) {
				Hit h;
				h.addr = addr;
				h.bytes = h.chars = len;
				h.enc = e;
				h.reg = curReg;
				h.text = util::safe(text);
				local.push_back(std::move(h));
			}

			void bytes(uint64_t a,size_t got) {
				size_t tot = keep + got;
				size_t ov = c.pat.size() - 1;
				uint8_t* b = buf.data();
				std::boyer_moore_horspool_searcher<std::vector<uint8_t>::const_iterator> sr(c.pat.begin(),c.pat.end());
				uint8_t* cur = b;
				uint8_t* end = b + tot;
				while (cur < end) {
					auto m = sr(cur,end);
					if (m.first == end) break;
					size_t off = (size_t)(m.first - b);
					size_t av = std::min((size_t)(end - m.first),(size_t)32);
					addHit(a - keep + off,c.pat.size(),EBytes,c.preview ? hexView(m.first,av) : std::string());
					cur = m.first + 1;
				}
				size_t nk = std::min(ov,tot);
				memmove(b,b + tot - nk,nk);
				keep = nk;
			}

			// only hits that start before the last RXWIN bytes count, a later one could still grow in the next chunk
			void regex(uint64_t a,size_t got,bool last) {
				size_t tot = keep + got;
				size_t lim = last ? tot : (tot > RXWIN ? tot - RXWIN : 0);
				uint8_t* b_ = buf.data();
				uint64_t base = a - keep;
				size_t pos = rxFrom;
				size_t mb,me;
				while (pos < tot && c.rx->find(b_,tot,pos,RXWIN,mb,me) && (last || mb < lim)) {
					addHit(base + mb,me - mb,ERegex,rawText(b_ + mb,me - mb,256));
					pos = me;
				}
				if (last) {
					keep = 0;
					rxFrom = 0;
					return;
				}
				// one byte stays in front of where the next pass starts, so ^ and \b know this is the middle of the memory
				size_t cut = lim ? lim - 1 : 0;
				size_t nk = tot - cut;
				memmove(b_,b_ + cut,nk);
				keep = nk;
				rxFrom = std::max(pos,lim) - cut;
			}

			void endRun(uint64_t a) {
				if (c.mode == MStrings) st.finish();
				else if (c.mode == MRegex && keep) regex(a,0,true);
				keep = 0;
				rxFrom = 0;
			}

			// where a resume starts so nothing still open gets lost. A run that began where this scan began would stop
			// the same way again, that one comes out cut and the resume goes on behind it
			uint64_t resumeAt(uint64_t a) const {
				if (c.mode == MBytes) return a - keep;
				if (c.mode == MRegex) return a - keep + rxFrom;
				// a run only opens on its third byte, the last two can still be where one starts
				uint64_t o = std::min(st.openAt(),st.t1 == TNone && st.t2 == TNone ? a : a - 2);
				return o > c.from && o < a ? o : a;
			}

			void region(const Region& g,uint32_t idx) {
				curReg = idx;
				st.reg = idx;
				uint64_t a = std::max(g.base,c.from),left = g.base + g.size - a;
				int fails = 0;
				bool any = false,bad = false;
				size_t extra = c.mode == MBytes ? c.pat.size() : c.mode == MRegex ? RXWIN + 1 : 0;
				size_t chunk = c.mode == MRegex ? RXCHUNK : CHUNK;
				size_t want = (size_t)std::min(left,(uint64_t)chunk) + extra;
				if (buf.size() < want) buf.resize(want);
				while (left) {
					if (halted(resumeAt(a))) break;
					size_t n = (size_t)std::min(left,(uint64_t)chunk);
					size_t got = readOn(p,a,buf.data() + keep,n);
					if (!got) {
						// skip the bad page and try on, a region that keeps failing is given up
						bad = true;
						endRun(a);
						uint64_t step = std::min(left,(uint64_t)(4096 - (a & 4095)));
						s.unread += step;
						a += step;
						left -= step;
						if (++fails >= 16) {
							s.unread += left;
							left = 0;
						}
						continue;
					}
					fails = 0;
					any = true;
					s.scanned += got;
					if (c.mode == MStrings) st.feed(buf.data(),got,a);
					else if (c.mode == MBytes) bytes(a,got);
					else regex(a,got,left == got);
					a += got;
					left -= got;
					flush();
				}
				// a stop is not the end of the region, what is still open never ended
				uint64_t at = left ? resumeAt(a) : a;
				if (at < a) {
					st.len = st.uChars = 0;
					keep = 0;
					local.erase(std::remove_if(local.begin(),local.end(),[at](const Hit& h) { return h.addr >= at; }),local.end());
				}
				endRun(a);
				flush();
				if (left) return;
				s.regDone++;
				if (!any) s.regBad++;
				else if (bad) s.regPartial++;
			}

			void loop() {
				while (!s.stop.load()) {
					size_t i = s.nextReg++;
					if (i >= regs.size()) break;
					if (halted(std::max(regs[i].base,c.from))) break;
					region(regs[i],(uint32_t)i);
				}
			}
		};

		void guarded(Worker& w,Scan& s) {
			try {
				w.loop();
			} catch (std::exception& e) {
				std::lock_guard<std::mutex> lk(s.m);
				if (s.why.empty()) s.why = std::string("worker failed : ") + e.what();
				s.stop = true;
			}
		}

	}

	void run(const ProcRef& r,const Cfg& c,Scan& s,const StopFn& stop) {
		try {
			std::string err;
			auto p = os::open(r,err);
			if (!p) {
				std::lock_guard<std::mutex> lk(s.m);
				s.err = err;
				s.done = true;
				return;
			}
			auto all = std::make_shared<std::vector<Region>>();
			int skip = 0;
			if (!os::regions(*p,*all,skip,err)) {
				std::lock_guard<std::mutex> lk(s.m);
				s.err = err;
				s.done = true;
				return;
			}
			auto regs = std::make_shared<std::vector<Region>>();
			uint64_t tot = 0;
			for (auto& g : *all) {
				if (!(g.kind & c.kinds)) continue;
				if (g.base + g.size <= c.from) continue;
				tot += g.base + g.size - std::max(g.base,c.from);
				regs->push_back(g);
			}
			s.total = tot;
			s.regTotal = (int)regs->size();
			s.regSkipped = skip;
			{
				std::lock_guard<std::mutex> lk(s.m);
				s.regs = regs;
			}
			uint64_t t0 = util::nowMs();
			int nt = c.threads > 0 ? c.threads : (int)std::thread::hardware_concurrency();
			nt = std::max(1,std::min(std::min(nt,64),std::max(1,(int)regs->size())));
			std::vector<std::unique_ptr<Worker>> ws;
			for (int i = 0; i < nt; i++) ws.emplace_back(new Worker(*p,c,s,stop,*regs,t0));
			std::vector<std::thread> th;
			try {
				for (int i = 1; i < nt; i++) {
					Worker* w = ws[i].get();
					th.emplace_back([w,&s] { guarded(*w,s); });
				}
			} catch (std::exception&) {}
			guarded(*ws[0],s);
			for (auto& t : th) t.join();
			std::lock_guard<std::mutex> lk(s.m);
			if (s.why.empty() && os::exited(*p)) s.why = "process exited";
			s.complete = s.why.empty();
		} catch (std::exception& e) {
			std::lock_guard<std::mutex> lk(s.m);
			if (s.err.empty()) s.err = e.what();
		}
		s.done = true;
	}

	DumpRes saveRegion(const ProcRef& r,uint64_t addr,uint64_t size,const std::string& path,Prog& pg,const StopFn& stop,uint64_t cap) {
		DumpRes res;
		res.path = path;
		std::string err;
		if (!os::pathOk(path,err)) {
			res.err = err;
			return res;
		}
		auto p = os::open(r,err);
		if (!p) {
			res.err = err;
			return res;
		}
		Region g;
		if (!os::regionAt(*p,addr,g,err)) {
			res.err = err;
			return res;
		}
		uint64_t end = size ? addr + size : g.base + g.size;
		if (end <= addr) {
			res.err = "size runs past the end of the address space";
			return res;
		}
		if (cap && end - addr > cap) {
			res.err = "that is " + util::size(end - addr) + ", the limit is " + util::size(cap) + ". Give a smaller size. Nothing was written";
			return res;
		}
		uint64_t fr = os::freeDisk(path);
		if (fr != NA && end - addr > fr) {
			res.err = "that is " + util::size(end - addr) + " and that disk has " + util::size(fr) + " free. Nothing was written";
			return res;
		}
		auto f = os::create(path,err);
		if (!f) {
			res.err = err;
			return res;
		}
		std::vector<uint8_t> buf((size_t)1 << 20);
		uint64_t a = addr,total = end - addr,good = 0;
		pg.say("Saving region...",0.0f);
		while (a < end) {
			const char* why = stop ? stop() : nullptr;
			if (why) {
				os::discard(*f);
				res.err = why;
				res.cancelled = true;
				return res;
			}
			size_t n = (size_t)std::min(end - a,(uint64_t)buf.size());
			size_t got = readOn(*p,a,buf.data(),n);
			if (!got) {
				got = (size_t)std::min((uint64_t)n,(uint64_t)(4096 - (a & 4095)));
				memset(buf.data(),0,got);
				res.unread += got;
			} else good += got;
			if (!os::write(*f,buf.data(),got,err)) {
				os::discard(*f);
				res.err = err;
				return res;
			}
			a += got;
			pg.frac = (float)((double)(a - addr) / (double)total);
		}
		if (!good) {
			os::discard(*f);
			res.err = os::exited(*p) ? "the process exited" : "nothing at this address could be read";
			return res;
		}
		if (res.unread && os::exited(*p)) res.note = "The process exited during the save, what was left is zeros";
		err.clear();
		res.size = os::close(*f,err);
		if (!err.empty()) {
			res.err = err;
			return res;
		}
		res.ok = true;
		pg.say("Saved.",1.0f);
		return res;
	}

	uint64_t readableBytes(const ProcRef& r) {
		std::string err;
		auto p = os::open(r,err);
		if (!p) return NA;
		std::vector<Region> v;
		int skip = 0;
		if (!os::regions(*p,v,skip,err)) return NA;
		uint64_t n = 0;
		for (auto& g : v) n += g.size;
		return n;
	}

	std::string newName(const ProcRef& r,const char* ext) {
		std::string nm;
		for (char ch : r.name) {
			if (nm.size() >= 64) break;
			bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
			nm += ok ? ch : '_';
		}
		if (nm.empty()) nm = "process";
		std::string ts = util::timeStr((uint64_t)time(nullptr));
		for (auto& ch : ts) if (ch == ':') ch = '-';
		std::replace(ts.begin(),ts.end(),' ','_');
		std::string d = os::defaultDir();
		if (!d.empty() && d.back() != os::sep()) d += os::sep();
		// the clock has 1 s steps and HTTP threads ask at the same time, a name handed out may not be a file yet
		static std::mutex m;
		static std::string last;
		static int lastN = 0;
		std::string stem = d + nm + "_" + ts,tail = std::string(".") + ext,out;
		std::lock_guard<std::mutex> lk(m);
		int n = stem + tail == last ? lastN + 1 : 1;
		for (;; n++) {
			out = n > 1 ? stem + "_" + std::to_string(n) + tail : stem + tail;
			if (!os::exists(out)) break;
		}
		last = stem + tail;
		lastN = n;
		return out;
	}

}
