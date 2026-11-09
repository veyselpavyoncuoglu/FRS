#pragma once
#include <string>
#include <vector>
#include <utility>
#include <type_traits>

// Tiny JSON value for the MCP server. Objects keep insertion order.
struct Json {
	enum Type { Null,Bool,Num,Str,Arr,Obj };
	Type t = Null;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector<Json> a;
	std::vector<std::pair<std::string,Json>> o;

	Json() {}
	Json(bool v) : t(Bool),b(v) {}
	template<class T,class = typename std::enable_if<std::is_arithmetic<T>::value>::type>
	Json(T v) : t(Num),n((double)v) {}
	Json(const char* v) : t(Str),s(v) {}
	Json(std::string v) : t(Str),s(std::move(v)) {}

	static Json arr() { Json j; j.t = Arr; return j; }
	static Json obj() { Json j; j.t = Obj; return j; }

	Json& set(const std::string& k,Json v) { o.emplace_back(k,std::move(v)); return *this; }
	Json& push(Json v) { a.push_back(std::move(v)); return *this; }
	const Json* get(const char* k) const;

	double num(const char* k,double def) const;
	bool flag(const char* k,bool def) const;
	std::string str(const char* k,const std::string& def) const;

	static bool parse(const std::string& src,Json& out);
	std::string dump() const;
};
