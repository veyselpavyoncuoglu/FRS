#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Settings store for everything added after v1.2. The old keys (flushSz, autoClean, mcp*...) stay
// where they are. All calls are thread-safe. Values live in settings.ini as key=value next to the
// old keys, App saves the file from the UI thread at most once a second when something changed.
// Prefix keys with your area : mem*, proc*, reg*, pool*, gpu*, tray*...
namespace cfg {

	int geti(const char* k,int def);
	float getf(const char* k,float def);
	bool getb(const char* k,bool def);
	std::string gets(const char* k,const char* def);

	void seti(const char* k,int v);
	void setf(const char* k,float v);
	void setb(const char* k,bool v);
	void sets(const char* k,const std::string& v); // newlines get stripped

	bool takeDirty(); // App::update only
	void loadKV(const char* k,const char* v); // App::load only
	std::string serialize(); // App::serialize only

	// User facing option : shows up on the Settings page, in app_status "options" and can be
	// changed through set_settings "options". Declare in your area's init(), not later.
	enum OptType { OptBool,OptInt,OptFloat,OptEnum,OptColor };
	struct Opt {
		std::string key,group,label,hint;
		OptType type = OptBool;
		float def = 0,lo = 0,hi = 0;
		std::vector<std::string> names; // OptEnum : value is the index
		std::string defHex; // OptColor : "#RRGGBB", or "" for a colour that follows something else until the user picks one
	};
	void optBool(const char* key,const char* group,const char* label,const char* hint,bool def);
	void optInt(const char* key,const char* group,const char* label,const char* hint,int def,int lo,int hi);
	void optFloat(const char* key,const char* group,const char* label,const char* hint,float def,float lo,float hi);
	void optEnum(const char* key,const char* group,const char* label,const char* hint,int def,std::vector<std::string> names);
	void optColor(const char* key,const char* group,const char* label,const char* hint,const char* defHex); // stored as "#RRGGBB"
	const std::vector<Opt>& opts();

	bool parseColor(const std::string& s,uint32_t& rgb); // "#RRGGBB" only, rgb = 0xRRGGBB
	bool getColor(const char* k,uint32_t& rgb); // an optColor's value, false while it is unset and its default is ""
	void resetOpts(); // drops the stored value of every declared option, other keys stay

}
