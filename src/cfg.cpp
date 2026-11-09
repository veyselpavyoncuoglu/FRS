#include "cfg.h"
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace {

	std::mutex g_m;
	std::map<std::string,std::string> g_kv;
	std::atomic<bool> g_dirty{false};
	std::vector<cfg::Opt> g_opts;

	bool find(const char* k,std::string& out) {
		std::lock_guard<std::mutex> lk(g_m);
		auto it = g_kv.find(k);
		if (it == g_kv.end()) return false;
		out = it->second;
		return true;
	}

	void put(const char* k,const std::string& v) {
		std::lock_guard<std::mutex> lk(g_m);
		auto it = g_kv.find(k);
		if (it != g_kv.end() && it->second == v) return;
		g_kv[k] = v;
		g_dirty = true;
	}

	cfg::Opt& addOpt(const char* key,const char* group,const char* label,const char* hint,cfg::OptType t,float def,float lo,float hi) {
		cfg::Opt o;
		o.key = key;
		o.group = group;
		o.label = label;
		o.hint = hint ? hint : "";
		o.type = t;
		o.def = def;
		o.lo = lo;
		o.hi = hi;
		g_opts.push_back(std::move(o));
		return g_opts.back();
	}

}

namespace cfg {

	int geti(const char* k,int def) {
		std::string s;
		if (!find(k,s)) return def;
		char* e = nullptr;
		double v = strtod(s.c_str(),&e);
		return e == s.c_str() ? def : (int)v;
	}

	float getf(const char* k,float def) {
		std::string s;
		if (!find(k,s)) return def;
		char* e = nullptr;
		double v = strtod(s.c_str(),&e);
		return e == s.c_str() ? def : (float)v;
	}

	bool getb(const char* k,bool def) {
		return geti(k,def ? 1 : 0) != 0;
	}

	std::string gets(const char* k,const char* def) {
		std::string s;
		return find(k,s) ? s : std::string(def);
	}

	void seti(const char* k,int v) {
		put(k,std::to_string(v));
	}

	void setf(const char* k,float v) {
		char b[32];
		snprintf(b,sizeof(b),"%.6g",v);
		put(k,b);
	}

	void setb(const char* k,bool v) {
		put(k,v ? "1" : "0");
	}

	void sets(const char* k,const std::string& v) {
		std::string s;
		for (char c : v) if (c != '\r' && c != '\n') s += c;
		put(k,s);
	}

	bool takeDirty() {
		return g_dirty.exchange(false);
	}

	void loadKV(const char* k,const char* v) {
		std::lock_guard<std::mutex> lk(g_m);
		g_kv[k] = v;
	}

	std::string serialize() {
		std::lock_guard<std::mutex> lk(g_m);
		std::string out;
		for (auto& kv : g_kv) out += kv.first + "=" + kv.second + "\n";
		return out;
	}

	void optBool(const char* key,const char* group,const char* label,const char* hint,bool def) {
		addOpt(key,group,label,hint,OptBool,def ? 1.0f : 0.0f,0,1);
	}

	void optInt(const char* key,const char* group,const char* label,const char* hint,int def,int lo,int hi) {
		addOpt(key,group,label,hint,OptInt,(float)def,(float)lo,(float)hi);
	}

	void optFloat(const char* key,const char* group,const char* label,const char* hint,float def,float lo,float hi) {
		addOpt(key,group,label,hint,OptFloat,def,lo,hi);
	}

	void optEnum(const char* key,const char* group,const char* label,const char* hint,int def,std::vector<std::string> names) {
		float hi = names.empty() ? 0.0f : (float)names.size() - 1.0f;
		addOpt(key,group,label,hint,OptEnum,(float)def,0,hi).names = std::move(names);
	}

	void optColor(const char* key,const char* group,const char* label,const char* hint,const char* defHex) {
		addOpt(key,group,label,hint,OptColor,0,0,0).defHex = defHex ? defHex : "";
	}

	bool parseColor(const std::string& s,uint32_t& rgb) {
		if (s.size() != 7 || s[0] != '#') return false;
		for (size_t i = 1; i < 7; i++) if (!isxdigit((unsigned char)s[i])) return false;
		rgb = (uint32_t)strtoul(s.c_str() + 1,nullptr,16);
		return true;
	}

	bool getColor(const char* k,uint32_t& rgb) {
		std::string def;
		for (auto& o : g_opts) if (o.key == k) def = o.defHex;
		return parseColor(gets(k,def.c_str()),rgb) || parseColor(def,rgb);
	}

	void resetOpts() {
		std::lock_guard<std::mutex> lk(g_m);
		for (auto& o : g_opts) if (g_kv.erase(o.key)) g_dirty = true;
	}

	const std::vector<Opt>& opts() {
		return g_opts;
	}

}
