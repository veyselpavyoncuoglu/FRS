#include "app.h"
#include "ram.h"
#include "platform.h"
#include "privacy.h"
#include "mcp.h"
#include "util.h"
#include "cfg.h"
#include "ui.h"
#include "domains.h"
#include "imgui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <memory>
#include <algorithm>
#include <stdexcept>

namespace {

	const char* kMethods[] = {"flush_standby","empty_working_sets","purge_standby","flush_modified"};
	const char* kMethodNames[] = {"Flush Standby","Empty Working Sets","Purge Standby","Flush Modified"};

	// Clean set areas next to the four methods, each one Empty menu command. win : not declared on Linux
	struct Area { const char* key; const char* cmd; const char* label; const char* hint; bool win; };
	const Area kAreas[] = {
		{"acSysCache","file_cache","System file cache","Empty system file cache from the Memory page's Empty menu.",false},
		{"acModCache","modified_file_cache","Modified file cache","Empty modified file cache from the Memory page's Empty menu. A slow or stuck volume makes the clean take longer.",false},
		{"acStandby0","standby_low","Priority 0 standby list","Empty priority 0 standby list from the Memory page's Empty menu.",true},
		{"acRegistry","registry_cache","Registry cache","Empty registry cache from the Memory page's Empty menu, may free little.",true},
		{"acCombine","combine_pages","Combine memory pages","Combine memory pages from the Memory page's Empty menu. Slow on big RAM.",true}
	};
	const int kAreaN = 5;
#ifdef _WIN32
	const bool kWin = true;
#else
	const bool kWin = false;
#endif

	bool areaHere(const Area& a) { return kWin || !a.win; }

	int extraMask() {
		int m = 0;
		for (int i = 0; i < kAreaN; i++) if (areaHere(kAreas[i]) && cfg::getb(kAreas[i].key,false)) m |= 1 << i;
		return m;
	}

	// run order in CleanInfo::set bits : the four keep theirs (DASH-08), the areas sit between them
	const int kOrder[] = {0,1,4,5,2,6,3,7,8};

	std::string stepNames(int m) {
		std::string s;
		for (int b : kOrder) {
			if (!(m & (1 << b))) continue;
			if (!s.empty()) s += ", ";
			s += b < 4 ? kMethodNames[b] : kAreas[b - 4].label;
		}
		return s;
	}

	std::string srcName(const std::string& pfx) {
		std::string s;
		for (char c : pfx) if (c != '[' && c != ']' && c != ' ') s += c;
		return s.empty() ? "dashboard" : s;
	}

	std::string hexCol(uint32_t c) {
		char b[8];
		snprintf(b,sizeof(b),"#%06X",(unsigned)(c & 0xFFFFFF));
		return b;
	}

	const char* kAskClean = "Clean###dashclean";
	const char* kAskReset = "Reset settings###setreset";
	const char* kAskNp = "No-prompt shortcut###npask";

	Json ramJson(const RamStats& r) {
		Json j = Json::obj();
		j.set("total_mb",r.total).set("used_mb",r.used).set("free_mb",r.free);
		j.set("used_pct",r.total ? std::round(r.used * 1000.0 / r.total) / 10.0 : 0.0);
		return j;
	}

	const char* stName(int s) { return s == 2 ? "on" : s == 1 ? "partial" : "off"; }

	// results of one runClean, for whoever started it (MCP waits on it)
	struct CleanRun {
		std::vector<std::string> res;
		RamStats b{},a{}; // RAM at the start and end of the steps, the summary line and MCP clean report the same two
		std::atomic<bool> done{false};
	};

	const cfg::Opt* findOpt(const std::string& k) {
		for (auto& o : cfg::opts()) if (o.key == k) return &o;
		return nullptr;
	}

	float clampf(float x,float lo,float hi) { return x < lo ? lo : x > hi ? hi : x; }

	// a log line's result : the tail after its last " : " outside brackets, so a process name or an error text in brackets can't pick the colour
	const char* logResult(const std::string& e) {
		int d = 0;
		for (size_t i = e.size(); i-- > 2;) {
			if (e[i] == ')') d++;
			else if (e[i] == '(') d--;
			else if (!d && e[i] == ' ' && e[i - 1] == ':' && e[i - 2] == ' ') return e.c_str() + i + 1;
		}
		return e.c_str();
	}

	bool stepFailed(const std::string& r) {
		const char* t = logResult(r);
		return strstr(t,"failed") || strstr(t,"NTSTATUS");
	}

	// one clean step by CleanInfo::set bit. A test exe never cleans the real machine, a stray click on a visible test window once
	// ran two Full Cleans. runStep fakes in it too
	std::string runBit(int i,int sz,bool fake) {
		auto fk = [](const char* n) {
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			return std::string(n) + " : OK (fake, test exe)";
		};
		if (i == 0) return fake ? fk("FlushStandby") : flushStandby(sz);
		if (i == 1) return fake ? fk("EmptyWorkSets") : emptyWorkSets();
		if (i == 2) return fake ? fk("PurgeStandby") : purgeStandby();
		if (i == 3) return fake ? fk("FlushModified") : flushModified();
		return sysmem::runStep(kAreas[i - 4].cmd);
	}

	bool fakeClean() {
		return util::testEnv("FRS_TEST_NOACTIVATE") || util::testEnv("FRS_TEST_FAKECMD");
	}

	std::string sumText(const RamStats& b,const RamStats& a,int nFail,size_t n) {
		char fr[128];
		double d = (double)b.used - (double)a.used;
		std::string av = "(available " + util::size(b.free << 20) + " -> " + util::size(a.free << 20) + ")";
		if (d > 0) snprintf(fr,sizeof(fr),"freed %.0f MB %s",d,av.c_str());
		else if (d < 0) snprintf(fr,sizeof(fr),"freed nothing, in use grew %.0f MB %s",-d,av.c_str());
		else snprintf(fr,sizeof(fr),"freed nothing %s",av.c_str());
		return nFail ? "Clean : failed (" + std::to_string(nFail) + " of " + std::to_string(n) + " steps), " + fr : std::string("Clean : OK, ") + fr;
	}

	// frs.log lines carry the date, the window's log only the time
	std::string day() {
		std::string d = util::timeStr((uint64_t)time(nullptr));
		return d.size() > 11 ? d.substr(0,11) : "";
	}

	const size_t kLogMax = 1 << 20;

	const char* kHelp =
		"FRS switches :\n\n"
		"-clean : runs the clean set (Settings > Auto-Clean) and exits\n"
		"-clean:full : runs the four methods and every extra area, then exits\n"
		"-silent : with -clean, shows no result\n"
		"-minimized : starts minimized\n"
		"-portable : keeps the settings next to FRS\n"
		"-sysinfo : opens the Memory page\n"
		"-help : this list\n\n"
		"-clean exits with 0 when every step worked, 1 otherwise.\n";

	// as double first, a huge number overflows the cast
	int limitArg(const Json& a,int def,int mx) {
		double l = a.num("limit",def);
		return l < 1.0 ? 1 : l > (double)mx ? mx : (int)l;
	}

	Json optsJson() {
		Json j = Json::obj();
		for (auto& o : cfg::opts()) {
			const char* k = o.key.c_str();
			uint32_t c = 0;
			if (o.type == cfg::OptBool) j.set(o.key,cfg::getb(k,o.def != 0.0f));
			else if (o.type == cfg::OptColor) j.set(o.key,cfg::getColor(k,c) ? hexCol(c) : std::string());
			else if (o.type == cfg::OptFloat) j.set(o.key,std::round((double)clampf(cfg::getf(k,o.def),o.lo,o.hi) * 10000.0) / 10000.0);
			else j.set(o.key,(int)clampf((float)cfg::geti(k,(int)o.def),o.lo,o.hi));
		}
		return j;
	}

	// options that only act while another one is on, greyed out until one of the parents is. Parents this build lacks don't count
	const char* kIcons = "trayPhysHist,trayCommitHist,trayPhysText,trayCommitText,trayGpuHist,trayGpuText";
	const char* kTextIcons = "trayPhysText,trayCommitText,trayGpuText";
	const char* kDeps[][2] = {
		{"trayTransparent",kIcons},{"trayHl",kIcons},{"traySingleClick",kIcons},{"trayToggle",kIcons},{"trayMidAct",kIcons},
		{"trayClose",kIcons},{"trayMin",kIcons},{"trayStartHidden",kIcons},{"trayNotify",kIcons},
		{"trayColText",kTextIcons},{"trayColBg",kTextIcons},{"trayNoBg",kTextIcons},{"trayFont",kTextIcons},{"trayAA",kTextIcons},
		{"trayBorder",kTextIcons},{"trayRound",kTextIcons},
		{"trayLvlBg","trayHl"},{"trayColWarn","trayHl"},{"trayColBad","trayHl"},{"trayNotifySound","trayNotify"},
		{"hlWarn","hlMem,trayHl,taskProgress"},{"hlBad","hlMem,trayHl,taskProgress"},{"lowPct","lowOn"},
		{"procHlGrowthMb","procHlGrowth"},{"procHlMs","procHlNew,procHlGrowth"},{"poolHlKb","poolHighlight"},
		{"regAutoSec","regAutoRefresh"},{"gpuPerfCounters","gpuMonitor"}
	};

	const char* depOf(const cfg::Opt& o) {
		if (o.group == "Status bar items") return "sbOn";
		for (auto& d : kDeps) if (o.key == d[0]) return d[1];
		return nullptr;
	}

	bool depOn(const char* ps) {
		bool any = false;
		std::string s = ps;
		for (size_t i = 0; i <= s.size();) {
			size_t e = s.find(',',i);
			if (e == std::string::npos) e = s.size();
			std::string k = s.substr(i,e - i);
			i = e + 1;
			const cfg::Opt* p = findOpt(k);
			if (!p) continue;
			if (cfg::getb(k.c_str(),p->def != 0.0f)) return true;
			any = true;
		}
		return !any;
	}

	void drawOption(const cfg::Opt& o) {
		const char* k = o.key.c_str();
		const char* dep = depOf(o);
		// kDeps parents are OR'ed, a background colour under "without background" is the one "unless"
		bool off = (dep && !depOn(dep)) || (o.key == "trayColBg" && cfg::getb("trayNoBg",false));
		bool ind = dep && !strchr(dep,',') && o.group != "Status bar items";
		if (ind) ImGui::Indent(ui::px(24.0f));
		if (off) ImGui::BeginDisabled();
		ImGui::PushID(k);
		if (o.type == cfg::OptBool) {
			bool v = cfg::getb(k,o.def != 0.0f);
			if (ImGui::Checkbox(o.label.c_str(),&v)) cfg::setb(k,v);
		} else if (o.key == "memRefreshMs") {
			// stored in ms, shown in sec like Auto-Refresh next to it
			float v = clampf((float)cfg::geti(k,(int)o.def),o.lo,o.hi) / 1000.0f;
			ImGui::SetNextItemWidth(ui::px(130.0f));
			if (ImGui::InputFloat(o.label.c_str(),&v,0.1f,1.0f,"%.1f")) cfg::seti(k,(int)clampf(std::round(v * 1000.0f),o.lo,o.hi));
		} else if (o.type == cfg::OptInt) {
			int v = (int)clampf((float)cfg::geti(k,(int)o.def),o.lo,o.hi);
			float span = o.hi - o.lo;
			ImGui::SetNextItemWidth(ui::px(130.0f));
			if (ImGui::InputInt(o.label.c_str(),&v,span >= 1000.0f ? 100 : span >= 100.0f ? 10 : 1,span >= 1000.0f ? 1000 : 100)) cfg::seti(k,(int)clampf((float)v,o.lo,o.hi));
		} else if (o.type == cfg::OptFloat) {
			float v = clampf(cfg::getf(k,o.def),o.lo,o.hi);
			ImGui::SetNextItemWidth(ui::px(130.0f));
			if (ImGui::InputFloat(o.label.c_str(),&v,0.0f,0.0f,"%.2f")) cfg::setf(k,clampf(v,o.lo,o.hi));
		} else if (o.type == cfg::OptColor) {
			uint32_t c = 0,dc = 0;
			bool set = cfg::getColor(k,c);
			// unset shows the colour the icon uses then
			ImVec4 a = o.key == "trayColWarn" ? ui::c.warn : o.key == "trayColBad" ? ui::c.bad : o.key == "trayColBg" ? ImVec4(0,0,0,1) : o.key == "trayColText" ? ui::c.phys : ui::c.dim;
			float f[3] = {a.x,a.y,a.z};
			if (set) for (int i = 0; i < 3; i++) f[i] = (float)((c >> (16 - 8 * i)) & 255) / 255.0f;
			if (ImGui::ColorEdit3(o.label.c_str(),f,ImGuiColorEditFlags_NoInputs)) {
				uint32_t n = 0;
				for (float x : f) n = (n << 8) | (uint32_t)(clampf(x,0.0f,1.0f) * 255.0f + 0.5f);
				cfg::sets(k,hexCol(n));
			}
			if (!set) {
				ImGui::SameLine();
				ImGui::TextDisabled("automatic");
			} else if (!cfg::parseColor(o.defHex,dc) || dc != c) {
				ImGui::SameLine();
				if (ImGui::SmallButton(o.defHex.empty() ? "Automatic" : "Default")) cfg::sets(k,o.defHex);
			}
		} else {
			int v = (int)clampf((float)cfg::geti(k,(int)o.def),o.lo,o.hi);
			const char* cur = v >= 0 && v < (int)o.names.size() ? o.names[v].c_str() : "";
			ImGui::SetNextItemWidth(ui::px(220.0f));
			if (ImGui::BeginCombo(o.label.c_str(),cur)) {
				for (int i = 0; i < (int)o.names.size(); i++) if (ImGui::Selectable(o.names[i].c_str(),i == v)) cfg::seti(k,i);
				ImGui::EndCombo();
			}
		}
		if (!o.hint.empty()) ui::hint(o.hint.c_str());
		ImGui::PopID();
		if (off) ImGui::EndDisabled();
		if (ind) ImGui::Unindent(ui::px(24.0f));
	}

	// Settings and Credits hold short lines, a card as wide as the window is mostly empty
	ImVec2 textCard() {
		return ImVec2(std::min(ImGui::GetContentRegionAvail().x / ui::px(1.0f),760.0f),0.0f);
	}

	char g_setFlt[64] = "";

	// the Settings page order : cleaning, log, startup and window, tray, display and history, then what one page or tab uses. Groups not named here follow in their own order
	const char* GRP_ORDER[] = {"Clean set","Log","Window","Tray and taskbar","Status bar and header","Status bar items","Graphs and numbers","Memory highlights","Memory sampling","GPU","Processes","Regions","Diagnostics","Kernel memory"};
	const char* kAdv[] = {"GPU","Processes","Regions","Diagnostics","Kernel memory"};

	// Windows declares these three in Tray and taskbar, they are about the window. The long tray group gets three cards
	const char* kWinKeys[] = {"uiOnTop","startMin","multiInst"};
	const char* kLookKeys[] = {"trayTransparent","trayColText","trayColBg","trayNoBg","trayFont","trayAA","trayBorder","trayRound","trayHl","trayLvlBg","trayColWarn","trayColBad"};
	const char* kBarKeys[] = {"taskOverlay","taskProgress"};

	template<size_t N> bool inKeys(const char* const (&ks)[N],const std::string& k) {
		for (auto x : ks) if (k == x) return true;
		return false;
	}

	std::string cardOf(const cfg::Opt& o) {
		if (inKeys(kWinKeys,o.key)) return "Window";
		if (o.group == "Tray and taskbar") return inKeys(kLookKeys,o.key) ? "Icon look" : inKeys(kBarKeys,o.key) ? "Taskbar" : "Tray icons";
		return o.group;
	}

	int grpRank(const std::string& g) {
		int n = (int)(sizeof(GRP_ORDER) / sizeof(GRP_ORDER[0]));
		for (int i = 0; i < n; i++) if (g == GRP_ORDER[i]) return i;
		return n;
	}

	bool setHit(const std::string& s) { return !*g_setFlt || util::has(s,g_setFlt); }

	bool optHit(const cfg::Opt& o) { return setHit(o.group) || setHit(o.label) || setHit(o.hint); }

	std::vector<std::string> optGroups() {
		std::vector<std::string> groups;
		for (auto& o : cfg::opts()) if (std::find(groups.begin(),groups.end(),o.group) == groups.end()) groups.push_back(o.group);
		std::stable_sort(groups.begin(),groups.end(),[](const std::string& a,const std::string& b) { return grpRank(a) < grpRank(b); });
		return groups;
	}

	bool grpHit(const std::string& g) {
		for (auto& o : cfg::opts()) if (o.group == g && optHit(o)) return true;
		return false;
	}

	bool cardHit(const std::string& c) {
		for (auto& o : cfg::opts()) if (cardOf(o) == c && optHit(o)) return true;
		return false;
	}

	int cardN(const std::string& c) {
		int n = 0;
		for (auto& o : cfg::opts()) if (cardOf(o) == c) n++;
		return n;
	}

	// a card of options. Status bar items have no hints, two columns keep them short
	void drawCard(const std::string& c) {
		if (!cardHit(c)) return;
		ui::beginCard(("##opt_" + c).c_str(),c.c_str(),textCard());
		int n = 0;
		for (auto& o : cfg::opts()) {
			if (cardOf(o) != c || !optHit(o)) continue;
			if (c == "Status bar items" && n++ & 1) ImGui::SameLine(ui::px(260.0f));
			drawOption(o);
		}
#ifndef _WIN32
		if (c == "Window") ImGui::TextDisabled("No tray on Linux");
#endif
		ui::endCard();
		ImGui::Spacing();
	}

	// what the last section holds : the per page cards, then any group no section names
	std::vector<std::string> advCards() {
		const char* placed[] = {"Clean set","Log","Window","Tray icons","Icon look","Taskbar","Status bar and header","Status bar items","Graphs and numbers","Memory highlights","Memory sampling"};
		std::vector<std::string> v(std::begin(kAdv),std::end(kAdv));
		for (auto& o : cfg::opts()) {
			std::string c = cardOf(o);
			if (!inKeys(placed,c) && std::find(v.begin(),v.end(),c) == v.end()) v.push_back(c);
		}
		return v;
	}

	// a run of cards under one fold. While filtering it shows open with no chevron, a hit never hides behind a closed one
	bool section(const char* name,bool hit,bool defOpen,int n = 0) {
		if (!hit) return false;
		ImGui::Spacing();
		float x0 = ImGui::GetCursorScreenPos().x;
		float x1 = x0 + ui::px(textCard().x);
		ui::pushBold();
		bool open = true;
		if (*g_setFlt) {
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(name);
		} else open = ui::fold(name,defOpen);
		ui::popFont();
		if (!open && n) {
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%d options",n);
		}
		ImVec2 a = ImGui::GetItemRectMin(),b = ImGui::GetItemRectMax();
		float y = std::floor((a.y + b.y) * 0.5f) + 0.5f;
		if (x1 > b.x + ui::px(24.0f)) ImGui::GetWindowDrawList()->AddLine(ImVec2(b.x + ui::px(12.0f),y),ImVec2(x1,y),ImGui::GetColorU32(ui::c.line));
		return open;
	}

	void intOpt(const char* key,int stp,int fast) {
		const cfg::Opt* o = findOpt(key);
		if (!o) return;
		int v = (int)clampf((float)cfg::geti(key,(int)o->def),o->lo,o->hi);
		ImGui::SetNextItemWidth(ui::px(130.0f));
		if (ImGui::InputInt((o->label + "##" + o->key).c_str(),&v,stp,fast)) cfg::seti(key,(int)clampf((float)v,o->lo,o->hi));
	}

	// a combo with no label, its label is the tooltip
	void enumOpt(const char* key,float w) {
		const cfg::Opt* o = findOpt(key);
		if (!o) return;
		int v = (int)clampf((float)cfg::geti(key,(int)o->def),o->lo,o->hi);
		ImGui::SetNextItemWidth(ui::px(w));
		if (ImGui::BeginCombo(("##" + o->key).c_str(),v >= 0 && v < (int)o->names.size() ? o->names[v].c_str() : "")) {
			for (int i = 0; i < (int)o->names.size(); i++) if (ImGui::Selectable(o->names[i].c_str(),i == v)) cfg::seti(key,i);
			ImGui::EndCombo();
		}
		ui::tip(o->label.c_str());
	}

	bool areaHit() {
		for (auto& a : kAreas) {
			const cfg::Opt* o = findOpt(a.key);
			if (o && optHit(*o)) return true;
		}
		return false;
	}

	// under the Auto-Clean methods : the extra areas in the same two columns. hm : the Methods row matched the filter
	void drawAreas(bool hm) {
		int n = 0;
		for (auto& a : kAreas) {
			const cfg::Opt* o = findOpt(a.key);
			if (!areaHere(a) || !o || (!hm && !optHit(*o))) continue;
			bool v = cfg::getb(a.key,false);
			if (n++ & 1) ImGui::SameLine(ui::px(220.0f));
			if (ImGui::Checkbox((std::string(a.label) + "##cs").c_str(),&v)) cfg::setb(a.key,v);
			ui::tip(a.hint);
		}
		ui::hint("The ticked methods and areas are the clean set. Auto-Clean, Clean every, Run clean set on the Dashboard and MCP clean with method selected run it.");
		ui::hint("Purge Standby and Flush Modified in an automatic run can stall the machine while the cache refills or the disk catches up.");
		if (!kWin) ui::hint("Registry cache, priority 0 standby and combine are not in the clean set on Linux, combine is on the Memory page's Empty menu.");
	}

	// a row matches the filter through any option drawn in it
	bool rowHit(const cfg::Opt& o) {
		std::vector<std::string> ks = {o.key};
		if (o.key == "acEvery") ks.push_back("acEveryMin");
		if (o.key == "hkOn") ks = {"hkOn","hkMods","hkKey"};
		for (auto& k : ks) {
			const cfg::Opt* p = findOpt(k);
			if (p && optHit(*p)) return true;
		}
		return false;
	}

	// the Clean set group past the areas. top : the pause and the timer under the threshold, else the rest under its own heading
	void cleanOpts(bool top) {
		const char* first[] = {"acCooldown","acEvery","cleanConfirm","hkOn","cleanSummary"};
		std::vector<const cfg::Opt*> os;
		for (auto k : first) if (const cfg::Opt* o = findOpt(k)) os.push_back(o);
		for (auto& o : cfg::opts()) if (o.group == "Clean set" && !inKeys(first,o.key)) os.push_back(&o);
		bool head = false;
		for (const cfg::Opt* op : os) {
			const cfg::Opt& o = *op;
			if (o.key == "acEveryMin" || o.key == "hkMods" || o.key == "hkKey" || !rowHit(o)) continue;
			bool area = false;
			for (auto& a : kAreas) if (o.key == a.key) area = true;
			if (area || top != (o.key == "acCooldown" || o.key == "acEvery")) continue;
			if (!top && !head) {
				ImGui::Spacing();
				ui::heading("Dashboard, hotkey and log");
				head = true;
			}
			ImGui::PushID(o.key.c_str());
			if (o.key == "acCooldown") {
				intOpt("acCooldown",10,60);
				ui::hint(o.hint.c_str());
			} else if (o.key == "acEvery") {
				bool on = cfg::getb("acEvery",false);
				if (ImGui::Checkbox(o.label.c_str(),&on)) cfg::setb("acEvery",on);
				ImGui::SameLine();
				intOpt("acEveryMin",5,30);
				ui::hint(o.hint.c_str());
			} else if (o.key == "hkOn") {
				bool on = cfg::getb("hkOn",false);
				if (ImGui::Checkbox(o.label.c_str(),&on)) cfg::setb("hkOn",on);
				for (auto k : {"hkMods","hkKey"}) {
					ImGui::SameLine();
					enumOpt(k,140.0f);
				}
				ui::hint(o.hint.c_str());
			} else drawOption(o);
			ImGui::PopID();
		}
		if (!top && !kWin && setHit("No global hotkey on Linux")) ImGui::TextDisabled("No global hotkey on Linux");
	}

	// one number on a Dashboard card. Unknown values never get a card.
	void memCard(const char* id,const char* label,uint64_t v,const std::string& sub,ImVec4 col,float w) {
		ui::beginCard(id,nullptr,ImVec2(w / ui::px(1.0f),0.0f));
		ImGui::TextDisabled("%s",label);
		ui::pushBig();
		ImGui::TextColored(col,"%s",util::size(v).c_str());
		ui::popFont();
		// short of room "75% of 31 GB" keeps the percent, like the Memory page
		size_t of = sub.find(" of ");
		bool cut = of != std::string::npos && ImGui::CalcTextSize(sub.c_str()).x > ImGui::GetContentRegionAvail().x;
		ImGui::TextDisabled("%s",cut ? sub.substr(0,of).c_str() : sub.c_str());
		ui::endCard();
	}

	void drawMemCards() {
		auto m = sysmem::latest();
		struct C {
			const char* id;
			const char* label;
			uint64_t v;
			std::string sub;
			ImVec4 col;
		};
		auto of = [](uint64_t v,uint64_t t) { return t != NA ? util::pct(v,t) + " of " + util::size(t) : std::string(" "); };
		std::vector<C> cs;
		if (m->physUsed != NA) cs.push_back({"##cphys","Physical",m->physUsed,of(m->physUsed,m->physTotal),ui::c.phys});
		if (m->commit != NA) cs.push_back({"##ccommit","Commit",m->commit,of(m->commit,m->commitLimit),ui::c.commit});
		if (m->standby != NA) cs.push_back({"##cstandby","Standby",m->standby,"cached, reusable",ui::c.standby});
		if (m->modified != NA) cs.push_back({"##cmod","Modified",m->modified,"dirty, to be written",ui::c.modified});
		if (cs.empty()) return;
		float sp = ImGui::GetStyle().ItemSpacing.x;
		float w = (ImGui::GetContentRegionAvail().x - sp * (float)(cs.size() - 1)) / (float)cs.size();
		for (size_t i = 0; i < cs.size(); i++) {
			if (i) ImGui::SameLine();
			memCard(cs[i].id,cs[i].label,cs[i].v,cs[i].sub,cs[i].col,w);
		}
		if (m->physTotal != NA && m->standby != NA && m->modified != NA && m->free != NA) {
			uint64_t z = m->zeroed != NA ? m->zeroed : 0;
			uint64_t modAll = m->modified + (m->modifiedNoWrite != NA ? m->modifiedNoWrite : 0);
			uint64_t lists = modAll + m->standby + m->free + z;
			uint64_t inUse = m->physTotal > lists ? m->physTotal - lists : 0;
			std::string nm[5] = {
				"In use  " + util::size(inUse),"Modified  " + util::size(modAll),"Standby  " + util::size(m->standby),
				"Free list  " + util::size(m->free),"Zeroed  " + util::size(z)
			};
			ui::Seg seg[5] = {
				{(float)inUse,ui::c.phys,nm[0].c_str()},{(float)modAll,ui::c.modified,nm[1].c_str()},{(float)m->standby,ui::c.standby,nm[2].c_str()},
				{(float)m->free,ui::c.freeMem,nm[3].c_str()},{(float)z,ui::c.zeroed,nm[4].c_str()}
			};
			ui::stackBar("##lists",seg,5,ImVec2(0.0f,12.0f));
			bool first = true;
			float dot = std::floor(ImGui::GetFontSize() * 0.6f);
			for (auto& g : seg) {
				if (g.v <= 0.0f) continue;
				if (!first) ImGui::SameLine(0.0f,ui::px(14.0f));
				first = false;
				ImVec2 q = ImGui::GetCursorScreenPos();
				q.y += std::floor((ImGui::GetTextLineHeight() - dot) * 0.5f);
				ImGui::GetWindowDrawList()->AddRectFilled(q,ImVec2(q.x + dot,q.y + dot),ImGui::GetColorU32(g.col),ui::px(2.0f));
				ImGui::Dummy(ImVec2(dot,ImGui::GetTextLineHeight()));
				ImGui::SameLine(0.0f,ui::px(5.0f));
				ImGui::TextDisabled("%s",g.name);
			}
		}
		ImGui::Spacing();
	}

}

// All application state. Kept out of the header so backends only see App's three
// entry points.
struct App::State {
	RamStats stats{};
	bool elev = false;

	int flushSz = 512;
	std::vector<std::string> log;
	bool scrollLog = false;

	bool autoRefresh = true;
	float refreshSec = 1.0f;
	bool autoClean = false;
	int cleanThr = 20;
	bool acFS = false,acEW = true,acPS = false,acFM = false;
	int maxLog = 200;

	uint64_t lastRefresh = 0;
	bool startupEnabled = false;

	std::mutex logMtx;
	std::atomic<bool> cleaning{false};
	std::atomic<float> cleanProg{0.0f};
	std::vector<std::string> pendingLog;

	uint64_t lastCfgSave = 0;
	uint64_t lastIniSave = 0;

	uint64_t lastStart = 0; // nowMs at the start of the last clean of any source
	std::atomic<uint64_t> lastEnd{0}; // and at its end, the worker sets it
	std::atomic<bool> dying{false}; // ~App stopped waiting for the clean
	uint64_t timerOn = 0;
	bool timerWas = false;
	std::mutex infoMtx;
	CleanInfo info;
	int askM = 0;
	bool askSet = false;
	std::string askTxt;

	std::string fileBuf; // log lines on their way to frs.log
	std::string logPath;
	bool logOn = false,logWas = false,logErr = false;
	uint64_t logFail = 0; // nowMs of the first failed write in a row
	bool safeDir = true;
	bool npOn = false;
	std::string npLink;
	std::shared_ptr<task::T> npTask;

	int setMask() const {
		return (int)acFS | (int)acEW << 1 | (int)acPS << 2 | (int)acFM << 3 | extraMask() << 4;
	}

	void trimLog() {
		while ((int)log.size() > maxLog) log.erase(log.begin());
	}
	// logOn is set at the start of the frame, the option itself also counts so the line of the call that turns it on lands too
	void addLog(std::string s) {
		if (logOn || (!logErr && cfg::getb("logFile",false))) fileBuf += day() + s + "\n";
		log.push_back(std::move(s));
		trimLog();
		scrollLog = true;
	}
	void flushPending() {
		std::lock_guard<std::mutex> lk(logMtx);
		for (auto& e : pendingLog) addLog(e);
		pendingLog.clear();
	}

	// one write for what this frame logged, also the lines buffered just before the option went off. A viewer or a virus scan
	// can hold the file a moment, the lines wait for 10 s, then it says so once and stops until the option is toggled
	void writeLog() {
		if (fileBuf.empty()) return;
		if (plat::appendFile("frs.log",fileBuf,kLogMax)) {
			fileBuf.clear();
			logFail = 0;
			return;
		}
		uint64_t now = util::nowMs();
		if (!logFail) logFail = now;
		if (now - logFail < 10000) return;
		fileBuf.clear();
		logFail = 0;
		logOn = false;
		logErr = true;
		addLog(util::ts() + " [!] Log file : failed (can't write " + logPath + ")");
	}

	// Run at startup from the checkbox and set_settings. A task made this way needs no update at the next launch
	bool setStartup(bool on,const char* pfx) {
		std::string why;
		if (!plat::setStartupEnabled(on,&why)) {
			addLog(util::ts() + " " + pfx + "Run at startup : failed (" + why + ")");
			return false;
		}
		startupEnabled = on;
		if (on) cfg::seti("startTaskV",2);
		return true;
	}

	void noPrompt(bool on) {
		npTask = task::run([on](task::T&) {
			std::string why;
			if (plat::setNoPrompt(on,why)) return std::string(on ? "No-prompt shortcut : OK (created)" : "No-prompt shortcut : OK (removed)");
			return "No-prompt shortcut : failed (" + why + ")";
		});
	}

	std::string serialize() const {
		char buf[512];
		snprintf(buf,sizeof(buf),
			"flushSz=%d\n"
			"autoRefresh=%d\n"
			"refreshSec=%.2f\n"
			"autoClean=%d\n"
			"cleanThr=%d\n"
			"acFS=%d\n"
			"acEW=%d\n"
			"acPS=%d\n"
			"acFM=%d\n"
			"maxLog=%d\n",
			flushSz,(int)autoRefresh,refreshSec,(int)autoClean,cleanThr,
			(int)acFS,(int)acEW,(int)acPS,(int)acFM,maxLog);
		return buf + mcp::serialize() + cfg::serialize();
	}

	Json settingsJson() const {
		Json m = Json::arr();
		bool ac[4] = {acFS,acEW,acPS,acFM};
		for (int i = 0; i < 4; i++) if (ac[i]) m.push(kMethods[i]);
		Json j = Json::obj();
		j.set("flush_size_mb",flushSz).set("auto_refresh",autoRefresh).set("refresh_sec",std::round(refreshSec * 10.0) / 10.0);
		j.set("auto_clean",autoClean).set("clean_threshold",cleanThr).set("auto_clean_methods",m);
		j.set("max_log",maxLog).set("run_at_startup",startupEnabled);
		j.set("options",optsJson());
		Json ch = Json::obj();
		for (auto& o : cfg::opts()) {
			if (o.type != cfg::OptEnum) continue;
			Json n = Json::arr();
			for (auto& s : o.names) n.push(s);
			ch.set(o.key,n);
		}
		j.set("option_choices",ch);
		return j;
	}

	void regTools();
	void regPages();
	void drawHeader();
	void drawDash();
	void drawSettings();
	void drawCredits();

	void save() {
		plat::writeSettings(serialize());
	}

	void saveIni() {
		size_t n = 0;
		const char* d = ImGui::SaveIniSettingsToMemory(&n);
		plat::writeFile("ui.ini",std::string(d,n));
		ImGui::GetIO().WantSaveIniSettings = false;
		lastIniSave = util::nowMs();
	}

	void load() {
		std::string text = plat::readSettings();
		if (text.empty()) return;
		size_t pos = 0;
		while (pos < text.size()) {
			size_t eol = text.find('\n',pos);
			if (eol == std::string::npos) eol = text.size();
			std::string line = text.substr(pos,eol - pos);
			pos = eol + 1;
			size_t eq = line.find('=');
			if (eq == std::string::npos || eq == 0) continue;
			std::string k = line.substr(0,eq);
			std::string v = line.substr(eq + 1);
			v = v.substr(0,v.find('\r'));
			// an empty value keeps the default on the old keys and mcp's, anything else goes to the store as it is
			bool old = false;
			for (auto o : {"flushSz","autoRefresh","refreshSec","autoClean","cleanThr","acFS","acEW","acPS","acFM","maxLog"}) if (k == o) old = true;
			if (old && v.empty()) continue;
			const char* key = k.c_str();
			float fval = (float)atof(v.c_str());
			int ival = (int)fval;
			if (strcmp(key,"flushSz") == 0) flushSz = ival;
			else if (strcmp(key,"autoRefresh") == 0) autoRefresh = ival != 0;
			else if (strcmp(key,"refreshSec") == 0) refreshSec = fval;
			else if (strcmp(key,"autoClean") == 0) autoClean = ival != 0;
			else if (strcmp(key,"cleanThr") == 0) cleanThr = ival;
			else if (strcmp(key,"acFS") == 0) acFS = ival != 0;
			else if (strcmp(key,"acEW") == 0) acEW = ival != 0;
			else if (strcmp(key,"acPS") == 0) acPS = ival != 0;
			else if (strcmp(key,"acFM") == 0) acFM = ival != 0;
			else if (strcmp(key,"maxLog") == 0) maxLog = ival;
			else if (!mcp::loadKV(key,v.c_str())) cfg::loadKV(key,v.c_str());
		}
	}

	// after the steps of a clean, on its worker : summary line, appCleanInfo, toast, tray balloon
	void ended(CleanRun& run,int nFail,const std::string& pre,const std::string& from,bool sumOn,bool notifyOn) {
		const RamStats& b = run.b;
		const auto& res = run.res;
		RamStats a = getRamStats();
		run.a = a;
		double freed = (double)b.used - (double)a.used;
		std::string sum = sumText(b,a,nFail,res.size());
		{
			std::lock_guard<std::mutex> lk(infoMtx);
			info.lastUnix = (uint64_t)time(nullptr);
			info.lastSrc = from;
			info.freedMb = freed;
			info.failed = nFail;
		}
		if (sumOn) {
			std::lock_guard<std::mutex> lk(logMtx);
			pendingLog.push_back(util::ts() + " " + pre + sum);
		}
		if (from == "hotkey" || from == "tray") ui::toast(sum);
		if (notifyOn) {
			std::string txt = sum;
			for (auto& r : res) txt += "\n" + r;
			surf::notify("FRS",txt,nFail > 0);
		}
	}

	// the four methods plus Clean set areas (ex : bits in kAreas order). src names it in appCleanInfo, nullptr takes it from pfx
	std::shared_ptr<CleanRun> runClean(bool fs,bool ew,bool ps,bool fm,const char* pfx = "",int ex = 0,const char* src = nullptr) {
		if (cleaning.load()) return nullptr;
		int total = (int)fs + (int)ew + (int)ps + (int)fm;
		for (int i = 0; i < kAreaN; i++) if (ex & (1 << i)) total++;
		if (total == 0) return nullptr;
		cleaning.store(true);
		cleanProg.store(0.0f);
		lastStart = util::nowMs();
		int sz = flushSz;
		std::string pre = pfx;
		std::string from = src ? src : srcName(pre);
		auto run = std::make_shared<CleanRun>();
		bool fake = fakeClean();
		int mask = (int)fs | (int)ew << 1 | (int)ps << 2 | (int)fm << 3 | ex << 4;
		bool sumOn = cfg::getb("cleanSummary",true);
		bool notifyOn = cfg::getb("trayNotify",false);
		// once ~App gave up waiting nothing past the running step may happen, statics are going away
		auto work = [this,mask,total,sz,pre,from,run,fake,sumOn,notifyOn]() {
			int done = 0;
			int nFail = 0;
			auto step = [&](std::string r) {
				if (stepFailed(r)) nFail++;
				{
					std::lock_guard<std::mutex> lk(logMtx);
					pendingLog.push_back(util::ts() + " " + pre + r);
				}
				run->res.push_back(r);
				++done;
				cleanProg.store((float)done / (float)total);
			};
			try {
				run->b = getRamStats();
				for (int i : kOrder) {
					if (!(mask & (1 << i)) || dying.load()) continue;
					step(runBit(i,sz,fake));
				}
				if (!dying.load()) ended(*run,nFail,pre,from,sumOn,notifyOn);
			} catch (...) {
				try {
					step("Clean : failed (out of memory or threads)");
				} catch (...) {}
			}
			lastEnd.store(util::nowMs());
			cleaning.store(false);
			run->done.store(true);
		};
		try {
			std::thread(work).detach();
		} catch (...) {
			cleaning.store(false);
			run->done.store(true);
			appLog(pre + "Clean : failed (could not start a thread)");
		}
		return run;
	}

	std::shared_ptr<CleanRun> runSet(const char* pfx,const char* src) {
		return runClean(acFS,acEW,acPS,acFM,pfx,extraMask(),src);
	}

	void dashRun(int m,bool set) {
		bool ok = set ? runSet("",nullptr) != nullptr : runClean(m & 1,m & 2,m & 4,m & 8) != nullptr;
		if (!ok) ui::toast(cleaning.load() ? "A clean is already running" : "The clean set is empty, tick methods in Settings > Auto-Clean");
	}

	// Dashboard buttons ask when cleanConfirm is on, the palette always does. m in CleanInfo::set bits
	void dashClean(int m,bool set,bool ask) {
		if (set && !m) {
			ui::toast("The clean set is empty, tick methods in Settings > Auto-Clean");
			return;
		}
		if (!ask && !cfg::getb("cleanConfirm",false)) {
			dashRun(m,set);
			return;
		}
		askM = m;
		askSet = set;
		askTxt = std::string(set ? "Runs the clean set : " : "Runs : ") + stepNames(m) + ".\n\nRun it now?";
		ui::askOpen(kAskClean);
	}

	// B13 : MCP config, the startup task, window size and position, ui.ini and page priority rules stay
	void resetAll() {
		save();
		plat::writeFile("settings.ini.bak",plat::readSettings());
		flushSz = 512;
		autoRefresh = true;
		refreshSec = 1.0f;
		autoClean = false;
		cleanThr = 20;
		acFS = false;
		acEW = true;
		acPS = false;
		acFM = false;
		maxLog = 200;
		cfg::resetOpts();
		save();
		addLog(util::ts() + " Settings : reset to defaults (old file in settings.ini.bak)");
	}
};

namespace {

	App::State* g_st = nullptr;
	std::mutex g_stMtx;

}

void appLog(const std::string& line) {
	std::lock_guard<std::mutex> g(g_stMtx);
	if (!g_st) return;
	std::lock_guard<std::mutex> lk(g_st->logMtx);
	g_st->pendingLog.push_back(util::ts() + " " + line);
}

bool appCleaning() {
	std::lock_guard<std::mutex> g(g_stMtx);
	return g_st && g_st->cleaning.load();
}

bool appRunClean(bool fs,bool ew,bool ps,bool fm,const char* pfx) {
	return g_st && g_st->runClean(fs,ew,ps,fm,pfx) != nullptr;
}

bool appRunSet(const char* pfx) {
	return g_st && g_st->runSet(pfx,nullptr) != nullptr;
}

CleanInfo appCleanInfo() {
	std::lock_guard<std::mutex> g(g_stMtx);
	if (!g_st) return CleanInfo();
	std::lock_guard<std::mutex> lk(g_st->infoMtx);
	return g_st->info;
}

void appSetMethod(int i,bool on) {
	if (!g_st || i < 0 || i > 8) return;
	if (i >= 4) {
		if (areaHere(kAreas[i - 4])) cfg::setb(kAreas[i - 4].key,on);
		return;
	}
	bool* f[4] = {&g_st->acFS,&g_st->acEW,&g_st->acPS,&g_st->acFM};
	*f[i] = on;
	g_st->save();
}

void appSetAuto(int thr) {
	if (!g_st) return;
	g_st->autoClean = thr > 0;
	if (thr > 0) g_st->cleanThr = thr > 100 ? 100 : thr;
	g_st->save();
}

// MCP tools. Handlers run on connection threads, several at once : OS queries go direct, app state goes through mcp::onUi.
void App::State::regTools() {
	State& st = *this;
	const char* noArgs = R"j({"type":"object","properties":{}})j";

	mcp::addTool("ram_status",
		"Live RAM usage plus where memory sits, sizes in MB. Windows : standby_mb = cached pages purge_standby can drop, "
		"modified_mb = dirty pages flush_modified writes out, free_list_mb = truly empty pages, plus commit and kernel pools. "
		"Linux : cached/buffers/dirty/anon/slab/swap from /proc/meminfo.",
		noArgs,true,
		[](const Json&) {
			Json j = ramJson(getRamStats());
			Json d = Json::obj();
			for (auto& kv : getMemInfo()) d.set(kv.first,kv.second);
			j.set("detail",d);
			return j;
		});

	mcp::addTool("top_processes",
		"Which processes use the most RAM. ws_mb = working set / resident (includes shared pages), "
		"private_mb = private bytes (Linux : anonymous resident). group=true merges same-name processes (e.g. all chrome.exe).",
		R"j({"type":"object","properties":{
			"limit":{"type":"integer","description":"rows to return (default 20)"},
			"sort":{"type":"string","enum":["ws","private"],"description":"sort key (default ws)"},
			"group":{"type":"boolean","description":"merge processes by name (default false)"}}})j",true,
		[](const Json& a) {
			int lim = limitArg(a,20,500);
			bool priv = a.str("sort","ws") == "private";
			bool grp = a.flag("group",false);
			struct Row { ProcMem p; int n; };
			std::vector<Row> rows;
			uint64_t tot = 0;
			for (auto& p : getProcs()) {
				tot += p.ws;
				Row* r = nullptr;
				if (grp) for (auto& x : rows) if (x.p.name == p.name) r = &x;
				if (r) {
					r->p.ws += p.ws;
					r->p.priv += p.priv;
					r->n++;
				} else rows.push_back({p,1});
			}
			std::sort(rows.begin(),rows.end(),[priv](const Row& x,const Row& y) {
				return priv ? x.p.priv > y.p.priv : x.p.ws > y.p.ws;
			});
			Json arr = Json::arr();
			for (size_t i = 0; i < rows.size() && (int)i < lim; i++) {
				Json r = Json::obj();
				if (grp) r.set("name",rows[i].p.name).set("count",rows[i].n);
				else r.set("pid",rows[i].p.pid).set("name",rows[i].p.name);
				r.set("ws_mb",util::mb(rows[i].p.ws)).set("private_mb",util::mb(rows[i].p.priv));
				arr.push(r);
			}
			Json j = Json::obj();
			j.set("rows_total",rows.size()).set("sum_ws_mb",util::mb(tot)).set("top",arr);
			return j;
		});

	mcp::addTool("app_status",
		"What the user sees in the FRS window : OS, admin/root state, the RAM bar, whether a clean is running, and all settings.",
		noArgs,true,
		[&st](const Json&) {
			return mcp::onUi([&st] {
				Json j = Json::obj();
				j.set("os",plat::osName()).set("elevated",st.elev).set("ram",ramJson(getRamStats()));
				j.set("cleaning",st.cleaning.load()).set("clean_progress_pct",std::round(st.cleanProg.load() * 100.0));
				CleanInfo ci;
				{
					std::lock_guard<std::mutex> lk(st.infoMtx);
					ci = st.info;
				}
				Json lc;
				if (ci.lastUnix) {
					lc = Json::obj();
					lc.set("time",util::timeStr(ci.lastUnix)).set("source",ci.lastSrc).set("freed_mb",ci.freedMb).set("failed_steps",ci.failed);
				}
				j.set("last_clean",lc).set("next_timer_clean_s",ci.nextTimerSec < 0 ? Json() : Json(ci.nextTimerSec));
				// null too once writing gave up, until the option is toggled
				j.set("log_file",cfg::getb("logFile",false) && !st.logErr ? Json(st.logPath) : Json());
				j.set("settings",st.settingsJson());
				return j;
			});
		});

	mcp::addTool("get_log",
		"Recent lines from the FRS log (cleans, auto-cleans, errors), newest last. Your own actions are tagged [mcp].",
		R"j({"type":"object","properties":{"limit":{"type":"integer","description":"max lines (default 50)"}}})j",true,
		[&st](const Json& a) {
			size_t lim = (size_t)limitArg(a,50,10000);
			return mcp::onUi([&] {
				st.flushPending();
				Json arr = Json::arr();
				for (size_t i = st.log.size() > lim ? st.log.size() - lim : 0; i < st.log.size(); i++) arr.push(st.log[i]);
				Json j = Json::obj();
				j.set("lines",arr);
				return j;
			});
		});

	mcp::addTool("clean",
		"Free RAM with the FRS cleaning methods, same as the Dashboard buttons. "
		"empty_working_sets : trims every process working set (gentlest, pages come back on demand). "
		"purge_standby : drops the standby list / file cache (needs admin/root). "
		"flush_modified : writes modified (dirty) pages to disk. "
		"flush_standby : briefly allocates flush_size_mb of RAM to push cache out (heavier, can cause paging). "
		"all : every method in order (Full Clean). selected runs the user's clean set, the Auto-Clean methods plus the extra areas. "
		"Waits until done by default and returns RAM before/after.",
		R"j({"type":"object","properties":{
			"methods":{"type":"array","items":{"type":"string","enum":["empty_working_sets","purge_standby","flush_modified","flush_standby","all","selected"]}},
			"wait":{"type":"boolean","description":"wait for the clean to finish (default true)"}},"required":["methods"]})j",false,
		[&st](const Json& a) {
			bool m[4] = {};
			bool sel = false;
			const Json* ms = a.get("methods");
			if (!ms || ms->t != Json::Arr || ms->a.empty()) throw std::runtime_error("methods must be a non-empty array");
			for (auto& v : ms->a) {
				int hit = v.s == "selected";
				sel |= hit != 0;
				for (int i = 0; i < 4; i++) if (v.s == "all" || v.s == kMethods[i]) { m[i] = true; hit++; }
				if (!hit) throw std::runtime_error("unknown method : " + util::safe(v.s));
			}
			auto run = mcp::onUi([&] {
				int ex = 0;
				if (sel) {
					bool ac[4] = {st.acFS,st.acEW,st.acPS,st.acFM};
					for (int i = 0; i < 4; i++) m[i] |= ac[i];
					ex = extraMask();
					if (!m[0] && !m[1] && !m[2] && !m[3] && !ex) throw std::runtime_error("the clean set is empty, tick methods in Settings > Auto-Clean or pass them");
				}
				return st.runClean(m[0],m[1],m[2],m[3],"[mcp] ",ex);
			});
			if (!run) throw std::runtime_error("a clean is already running, try again in a moment");
			Json j = Json::obj();
			if (!a.flag("wait",true)) {
				j.set("started",true);
				return j;
			}
			auto t0 = std::chrono::steady_clock::now();
			while (!run->done.load()) {
				if (!mcp::alive()) throw std::runtime_error("MCP server is stopping");
				if (std::chrono::steady_clock::now() - t0 > std::chrono::minutes(2)) {
					j.set("started",true).set("note","still running after 2 min, check app_status / get_log");
					return j;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			// the worker's two samples, so freed_mb is the number the summary line and app_status last_clean show
			RamStats after = run->a.total ? run->a : getRamStats();
			RamStats before = run->b.total ? run->b : after;
			Json res = Json::arr();
			for (auto& r : run->res) res.push(r);
			j.set("results",res).set("before",ramJson(before)).set("after",ramJson(after));
			j.set("freed_mb",(double)before.used - (double)after.used);
			return j;
		});

	mcp::addTool("set_settings",
		"Change FRS settings like the Settings tab does, only the fields you pass change. Returns the new settings, unknown fields are skipped and listed in ignored.",
		R"j({"type":"object","properties":{
			"flush_size_mb":{"type":"integer","description":"Flush Standby block size, 64..total RAM"},
			"auto_refresh":{"type":"boolean","description":"poll RAM stats every refresh_sec"},
			"refresh_sec":{"type":"number","description":"0.1..60"},
			"auto_clean":{"type":"boolean","description":"clean automatically when RAM usage >= clean_threshold"},
			"clean_threshold":{"type":"integer","description":"percent, 1..100"},
			"auto_clean_methods":{"type":"array","items":{"type":"string","enum":["flush_standby","empty_working_sets","purge_standby","flush_modified"]}},
			"max_log":{"type":"integer","description":"log lines kept, 10..9999"},
			"run_at_startup":{"type":"boolean","description":"launch FRS at login"},
			"options":{"type":"object","description":"other options by key, app_status settings.options lists them. Numbers are clamped, a choice takes its name or index in option_choices, a colour is \"#RRGGBB\" or \"\" for its default"}}})j",false,
		[&st](const Json& a) {
			return mcp::onUi([&] {
				auto get = [&](const char* k,Json::Type t) {
					const Json* v = a.get(k);
					if (v && v->t != t) throw std::runtime_error(std::string(k) + " has the wrong type");
					return v;
				};
				auto clampd = [](double x,double lo,double hi) { return x < lo ? lo : x > hi ? hi : x; };
				// a field this tool doesn't know is skipped like v1.2 did, the answer names it so a misspelt one can't read as done
				const char* known[] = {"flush_size_mb","auto_refresh","refresh_sec","auto_clean","clean_threshold","auto_clean_methods","max_log","run_at_startup","options"};
				Json ign = Json::arr();
				for (auto& kv : a.o) {
					bool ok = false;
					for (auto k : known) if (kv.first == k) ok = true;
					if (!ok) ign.push(util::safe(kv.first));
				}
				const Json* fs = get("flush_size_mb",Json::Num);
				const Json* ar = get("auto_refresh",Json::Bool);
				const Json* rs = get("refresh_sec",Json::Num);
				const Json* ac = get("auto_clean",Json::Bool);
				const Json* th = get("clean_threshold",Json::Num);
				const Json* am = get("auto_clean_methods",Json::Arr);
				const Json* ml = get("max_log",Json::Num);
				const Json* su = get("run_at_startup",Json::Bool);
				const Json* op = get("options",Json::Obj);
				bool acm[4] = {};
				if (am) for (auto& x : am->a) {
					int hit = 0;
					for (int i = 0; i < 4; i++) if (x.s == kMethods[i]) { acm[i] = true; hit++; }
					if (!hit) throw std::runtime_error("unknown method : " + util::safe(x.s));
				}
				// a choice by its name in option_choices or its index there
				auto choice = [](const cfg::Opt* o,const Json& v) {
					if (v.t == Json::Str) {
						for (int i = 0; i < (int)o->names.size(); i++) if (util::lower(o->names[i]) == util::lower(v.s)) return i;
					} else if (v.t == Json::Num && v.n >= 0 && v.n < (double)o->names.size()) return (int)v.n;
					throw std::runtime_error(o->key + " must be an index 0.." + std::to_string(o->names.size() - 1) + " into option_choices." + o->key + " or one of its names");
				};
				if (op) for (auto& kv : op->o) {
					const cfg::Opt* o = findOpt(kv.first);
					if (!o) throw std::runtime_error("unknown option : " + util::safe(kv.first));
					if (o->type == cfg::OptEnum) {
						choice(o,kv.second);
						continue;
					}
					if (kv.second.t != (o->type == cfg::OptBool ? Json::Bool : o->type == cfg::OptColor ? Json::Str : Json::Num)) throw std::runtime_error(util::safe(kv.first) + " has the wrong type");
					uint32_t c = 0;
					if (o->type == cfg::OptColor && !kv.second.s.empty() && !cfg::parseColor(kv.second.s,c)) throw std::runtime_error(util::safe(kv.first) + " must be a colour as \"#RRGGBB\", or \"\" for its default");
				}
				// the pair tray_win.cpp hotkey() refuses to register : Ctrl or Alt alone would take Ctrl+C, Alt+F4 from every program
				const Json* hm = op ? op->get("hkMods") : nullptr;
				const Json* hk = op ? op->get("hkKey") : nullptr;
				if (hm || hk) {
					int md = hm ? choice(findOpt("hkMods"),*hm) : cfg::geti("hkMods",0);
					int k = hk ? choice(findOpt("hkKey"),*hk) : cfg::geti("hkKey",11);
					if (md >= 4 && (k >= 12 || k == 3)) throw std::runtime_error("hkMods with hkKey : Ctrl or Alt alone only with F1 to F12, not F4");
				}

				RamStats r = getRamStats();
				if (fs) st.flushSz = (int)clampd(fs->n,64,r.total > 64 ? (double)r.total : 64);
				if (ar) st.autoRefresh = ar->b;
				if (rs) st.refreshSec = (float)clampd(rs->n,0.1,60);
				if (ac) st.autoClean = ac->b;
				if (th) st.cleanThr = (int)clampd(th->n,1,100);
				if (am) {
					st.acFS = acm[0];
					st.acEW = acm[1];
					st.acPS = acm[2];
					st.acFM = acm[3];
				}
				if (ml) {
					st.maxLog = (int)clampd(ml->n,10,9999);
					st.trimLog();
				}
				if (op) for (auto& kv : op->o) {
					const cfg::Opt* o = findOpt(kv.first);
					const char* k = o->key.c_str();
					uint32_t c = 0;
					if (o->type == cfg::OptBool) cfg::setb(k,kv.second.b);
					else if (o->type == cfg::OptColor) cfg::sets(k,cfg::parseColor(kv.second.s,c) ? hexCol(c) : o->defHex);
					else if (o->type == cfg::OptFloat) cfg::setf(k,(float)clampd(kv.second.n,o->lo,o->hi));
					else if (o->type == cfg::OptEnum) cfg::seti(k,choice(o,kv.second));
					else cfg::seti(k,(int)clampd(kv.second.n,o->lo,o->hi));
				}
				std::string warn;
				if (su && su->b != st.startupEnabled && !st.setStartup(su->b,"[mcp] ")) warn = "could not change run_at_startup";
				st.save();
				st.addLog(util::ts() + " [mcp] settings changed");
				Json j = st.settingsJson();
				if (!warn.empty()) j.set("warning",warn);
				if (!ign.a.empty()) j.set("ignored",ign).set("ignored_note","not fields of set_settings, nothing changed for them. Other options go inside options, app_status lists them");
				return j;
			});
		});

	mcp::addTool("clear_log","Clear the FRS log, same as the Clear button.",noArgs,false,
		[&st](const Json&) {
			return mcp::onUi([&st] {
				st.log.clear();
				Json j = Json::obj();
				j.set("cleared",true);
				return j;
			});
		});

	if (!privacy::backend::supported()) return;

	auto tweaks = [] {
		Json arr = Json::arr();
		for (int i = 0; i < privacy::count(); i++) {
			Json t = Json::obj();
			t.set("id",i).set("name",privacy::name(i)).set("desc",privacy::desc(i));
			t.set("keys",privacy::keys(i)).set("status",stName(privacy::status(i)));
			arr.push(t);
		}
		return arr;
	};

	mcp::addTool("privacy_status",
		"Windows privacy/telemetry registry tweaks from the Privacy tab with their live status (on/partial/off).",
		noArgs,true,
		[tweaks](const Json&) {
			return mcp::onUi([&] {
				Json j = Json::obj();
				j.set("elevated",isElevated()).set("tweaks",tweaks());
				return j;
			});
		});

	for (int ap = 1; ap >= 0; ap--) {
		mcp::addTool(ap ? "privacy_apply" : "privacy_revert",
			ap ? "Apply privacy tweaks like the Privacy tab Apply button (writes the registry values, machine-wide ones need admin)."
				: "Revert privacy tweaks like the Privacy tab Revert button (deletes the values so Windows defaults come back).",
			R"j({"type":"object","properties":{"tweak":{"type":"string","description":"id from privacy_status, the exact name, or \"all\""}},"required":["tweak"]})j",false,
			[ap](const Json& a) {
				return mcp::onUi([&] {
					const Json* tv = a.get("tweak");
					std::string w = !tv ? "" : tv->t == Json::Num ? std::to_string((int)tv->n) : util::lower(tv->s);
					Json res = Json::arr();
					for (int i = 0; i < privacy::count(); i++) {
						if (w != "all" && w != std::to_string(i) && w != util::lower(privacy::name(i))) continue;
						int n = ap ? privacy::apply(i) : privacy::revert(i);
						Json r = Json::obj();
						r.set("id",i).set("name",privacy::name(i)).set(ap ? "written" : "cleared",n);
						r.set("keys",privacy::keys(i)).set("status",stName(privacy::status(i)));
						res.push(r);
					}
					if (res.a.empty()) throw std::runtime_error("unknown tweak '" + util::safe(w) + "', see privacy_status");
					Json j = Json::obj();
					j.set("results",res);
					if (!isElevated()) j.set("warning","FRS is not elevated, machine-wide (HKLM) values fail");
					return j;
				});
			});
	}
}

void App::State::regPages() {
	nav::setHeader([] { g_st->drawHeader(); });
	nav::addPage("dashboard","Dashboard",ui::IcoDash,nav::GrpMain,[] { g_st->drawDash(); });
	nav::addPage("settings","Settings",ui::IcoSettings,nav::GrpApp,[] { g_st->drawSettings(); });
	nav::addPage("privacy","Privacy",ui::IcoPrivacy,nav::GrpApp,[] { privacy::drawTab(); });
	nav::addPage("mcp","MCP",ui::IcoMcp,nav::GrpApp,[] { if (mcp::drawTab()) g_st->save(); });
	nav::addPage("credits","Credits",ui::IcoCredits,nav::GrpApp,[] { g_st->drawCredits(); });

	cfg::optInt("memRefreshMs","Memory sampling","Sampling interval, sec","How often the memory numbers and graphs update. The RAM bar follows Auto-Refresh instead.",1000,100,10000);
	cfg::optBool("memPaused","Memory sampling","Pause sampling","Stops the background memory samplers, the numbers and graphs stay as they are. An open process window's Memory, WS Watch and .NET tabs keep reading their process.",false);
	cfg::optInt("memHistN","Memory sampling","History samples","How many samples the memory graphs keep.",600,60,3600);

	for (auto& a : kAreas) if (areaHere(a)) cfg::optBool(a.key,"Clean set",a.label,a.hint,false);
	cfg::optInt("acCooldown","Clean set","Auto-Clean pause after a clean, sec","0 keeps today's behaviour : a machine still above the threshold cleans again right away. 30 stops back to back cleans.",0,0,3600);
	cfg::optBool("acEvery","Clean set","Clean every","Runs the clean set every N minutes, counted from the start of the last clean, no prompt. Works with Auto-Refresh off.",false);
	cfg::optInt("acEveryMin","Clean set","minutes","Minutes between two timed cleans.",30,1,1440);
	cfg::optBool("cleanSummary","Clean set","Summary line after a clean",kWin ? "One log line with what the clean freed. Available counts standby, so a standby purge moves this number little. Free and zeroed on the Memory page shows it. Tray balloon : Notify after a clean in Tray and taskbar."
		: "One log line with what the clean freed. Available (MemAvailable) counts page cache, so dropping the cache moves this number little. The Free list on the Memory page shows it.",true);
	cfg::optBool("cleanConfirm","Clean set","Ask before a Dashboard clean","The Dashboard clean buttons and Run clean set list the steps and ask first. Auto-Clean, Clean every and MCP never ask.",false);
}

void App::State::drawHeader() {
	State& st = *this;
	char sub[64];
	snprintf(sub,sizeof(sub),"Free RAM System  (%s)",plat::osName());
	float x0 = ImGui::GetCursorPosX();
	ui::title(nav::currentTitle());
	ImGui::SameLine(x0 + nav::titleColW() + ui::px(14.0f));
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s",sub);
	ImGui::SameLine(0.0f,ui::px(14.0f));
	if (ImGui::Button("Refresh")) {
		st.stats = getRamStats();
		st.elev = isElevated();
		st.lastRefresh = util::nowMs();
		sampler::kick();
	}
	ui::tip("Re-reads the RAM bar and takes one memory sample now, also while sampling is paused. F5 takes a sample alone.");
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	if (st.autoRefresh) {
		ImGui::TextColored(ui::c.good,"AUTO");
	} else {
		ImGui::TextDisabled("AUTO  [off]");
	}
	ui::tip("Auto-Refresh : the RAM bar and the Auto-Clean check. Graphs and the other numbers follow Memory sampling in Settings.");
	ImGui::SameLine(0.0f,ui::px(14.0f));
	float elevW = ImGui::CalcTextSize(st.elev ? "[ADMIN]" : "[NOT ELEVATED]").x;
	float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	float gw = ImGui::GetContentRegionAvail().x - elevW - ui::px(10.0f);
	if (gw > ui::px(40.0f)) {
		ImGui::BeginChild("##hgraphs",ImVec2(gw,ImGui::GetFrameHeight()),ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		surf::drawHeaderGraphs();
		ImGui::EndChild();
	}
	ImGui::SameLine(right - elevW);
	ImGui::AlignTextToFramePadding();
	if (st.elev) {
		ImGui::TextColored(ui::c.good,"[ADMIN]");
	} else {
		ImGui::TextColored(ui::c.bad,"[NOT ELEVATED]");
	}

	float used = (float)st.stats.used;
	float total = (float)st.stats.total;
	float frac = (total > 0.0f) ? used / total : 0.0f;
	char ovl[80];
	snprintf(ovl,sizeof(ovl),"%.0f / %.0f MB   (%.0f MB free | %.1f%%)",used,total,(float)st.stats.free,frac * 100.0f);
	ImGui::AlignTextToFramePadding();
	ImGui::Text("RAM :");
	ImGui::SameLine();
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - ui::px(20.0f)) * 0.5f);
	ui::meter(frac,ovl,ImVec2(0.0f,20.0f),ui::loadColor(frac));
#ifdef _WIN32
	ui::tip("free here means available : free and zeroed lists plus standby cache. Free + zeroed on the Memory page leaves the cache out.");
#else
	ui::tip("free here means available (MemAvailable) : it includes page cache the kernel can drop. The free list on the Memory page leaves the cache out.");
#endif
}

void App::State::drawDash() {
	State& st = *this;
	drawMemCards();

	ui::beginCard("##clean",nullptr);
	ImGui::AlignTextToFramePadding();
	ImGui::Text("Flush block (MB) :");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ui::px(130.0f));
	if (ImGui::InputInt("##fsz",&st.flushSz,64,512)) st.save();
	ImGui::SameLine();
	if (ImGui::Button("Max")) {
		st.stats = getRamStats();
		st.flushSz = (int)st.stats.total;
	}
	bool busy = st.cleaning.load();
	int set = st.setMask();
	const char* rl = "Run clean set";
	float bw = ImGui::CalcTextSize(rl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	ImGui::SameLine();
	float av = ImGui::GetContentRegionAvail().x;
	if (av > bw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + av - bw);
	if (busy || !set) ImGui::BeginDisabled();
	if (ImGui::Button(rl)) st.dashClean(set,true,false);
	if (busy || !set) ImGui::EndDisabled();
	std::string rt = busy ? "A clean is running." : !set ? "The clean set is empty, tick methods in Settings > Auto-Clean." : "Runs the clean set : " + stepNames(set) + ". Change it in Settings > Auto-Clean.";
	ui::tip(rt.c_str());
	if (st.flushSz < 64) st.flushSz = 64;
	if (st.flushSz > (int)st.stats.total && st.stats.total > 0) st.flushSz = (int)st.stats.total;

	ImGui::Spacing();

	if (busy) ImGui::BeginDisabled();

	ImVec2 bsz((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3.0f) * 0.25f,0.0f);
	if (ImGui::Button("Flush Standby",bsz)) st.dashClean(1,false,false);
	ui::tip("Briefly allocates the Flush block size of RAM to push cache out. Heavier, can cause paging. No admin needed.");
	ImGui::SameLine();
	if (ImGui::Button("Empty Working Sets",bsz)) st.dashClean(2,false,false);
	ui::tip("Trims every process it can open, pages fault back on use. Without admin it only reaches some processes. Memory page twin : Empty working sets.");
	ImGui::SameLine();
	if (ImGui::Button("Purge Standby",bsz)) st.dashClean(4,false,false);
	ui::tip("Drops the standby list, cache and not lost memory. Needs admin. Same as Empty standby list on the Memory page.");
	ImGui::SameLine();
	if (ImGui::Button("Flush Modified",bsz)) st.dashClean(8,false,false);
	ui::tip("Writes modified pages to disk, they become standby. Same as Empty modified page list on the Memory page.");

	ImGui::Spacing();
	ui::pushBtnColor(ui::c.good);
	if (ImGui::Button("Full Clean  (all methods)",ImVec2(-1.0f,ui::px(28.0f)))) {
		st.dashClean(15,false,false);
	}
	ui::tip("All four methods in order. Purge Standby needs admin.");
	ui::popBtnColor();

	if (busy) ImGui::EndDisabled();

	ImGui::Spacing();
	if (busy) {
		float p = st.cleanProg.load();
		char pl[32];
		snprintf(pl,sizeof(pl),"Cleaning... %.0f%%",p * 100.0f);
		ui::meter(p,pl,ImVec2(0.0f,18.0f),ui::c.accent);
	} else {
		ui::meter(0.0f,"",ImVec2(0.0f,18.0f),ui::c.accent);
		ImVec2 a = ImGui::GetItemRectMin();
		ImVec2 b = ImGui::GetItemRectMax();
		ImVec2 ts = ImGui::CalcTextSize("Idle");
		ImGui::GetWindowDrawList()->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f,(a.y + b.y - ts.y) * 0.5f),ImGui::GetColorU32(ui::c.dim),"Idle");
	}
	ui::endCard();

	ImGui::Spacing();
	ui::beginCard("##logcard",nullptr,ImVec2(0.0f,-1.0f));
	ImGui::AlignTextToFramePadding();
	ImGui::Text("Log :");
	ImGui::SameLine();
	if (ImGui::SmallButton("Clear")) st.log.clear();

	ImGui::BeginChild("##log",ImVec2(0,0));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(ImGui::GetStyle().ItemSpacing.x,ui::px(2.0f)));
	if (st.log.empty()) ui::empty("No log entries yet");
	for (auto& e : st.log) {
		const char* s = e.c_str();
		const char* tag = strchr(s,' ');
		tag = tag ? tag + 1 : s;
		const char* res = logResult(e);
		ImVec4 col;
		if (strstr(res,"failed") || strstr(res,"NTSTATUS") || !strncmp(tag,"[!]",3))
			col = ui::c.bad;
		else if (!strncmp(tag,"[mcp]",5))
			col = ui::c.purple;
		else if (!strncmp(tag,"[auto]",6))
			col = ui::c.warn;
		else if (strstr(res,"OK") || strstr(res,"flushed") || strstr(res,"trimmed") || strstr(res,"purge"))
			col = ui::c.good;
		else
			col = ui::c.dim;
		ImGui::TextColored(col,"%s",s);
	}
	if (st.scrollLog) {
		ImGui::SetScrollHereY(1.0f);
		st.scrollLog = false;
	}
	ImGui::PopStyleVar();
	ImGui::EndChild();
	ui::endCard();
	if (ui::askDraw(kAskClean,st.askTxt.c_str(),"Run")) st.dashRun(st.askM,st.askSet);
}

void App::State::drawSettings() {
	State& st = *this;
	bool changed = false;
	bool any = false;

	ui::search("##sflt",g_setFlt,sizeof(g_setFlt),"Filter settings",260.0f);
	ImGui::Spacing();

	// what people change most comes first, the long tray list and what one page uses start folded
	bool hThr = setHit("Auto-Clean threshold Triggers clean when RAM usage >= threshold.");
	bool hMeth = setHit("Clean set Methods Flush Standby Empty Working Sets Purge Standby Flush Modified");
	bool hClean = hThr || hMeth || grpHit("Clean set");
	bool hStart = plat::startupSupported() && setHit("Startup Run at startup (no prompt) Launches FRS automatically when you log in. no-prompt shortcut UAC");
	bool hRef = setHit("Auto-Refresh Polls RAM stats every N seconds.");
	bool hLog = setHit("Log max entries Clear Log Oldest entries removed when limit is exceeded.") || grpHit("Log");
	const char* tray[] = {"Tray icons","Icon look","Taskbar"};
	const char* disp[] = {"Status bar and header","Status bar items","Graphs and numbers","Memory highlights"};
	bool hTray = false,hDisp = false,hAdv = false;
	int nTray = 0,nAdv = 0,nDisp = cardN("Memory sampling");
	for (auto c : tray) {
		hTray |= cardHit(c);
		nTray += cardN(c);
	}
	for (auto c : disp) {
		hDisp |= cardHit(c);
		nDisp += cardN(c);
	}
	std::vector<std::string> adv = advCards();
	for (auto& c : adv) {
		hAdv |= cardHit(c);
		nAdv += cardN(c);
	}
	any = hClean || hStart || hRef || hLog || hTray || hDisp || hAdv || cardHit("Window") || cardHit("Memory sampling");

	// Auto-Refresh first like v1.2, threshold Auto-Clean only checks on its ticks
	if (section("Cleaning",hClean || hRef,true,8 + cardN("Clean set"))) {
		if (hRef) {
			ui::beginCard("##sar",nullptr,textCard());
			ui::heading("Auto-Refresh");
			changed |= ImGui::Checkbox("Enabled##ar",&st.autoRefresh);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ui::px(130.0f));
			changed |= ImGui::InputFloat("sec##ri",&st.refreshSec,0.1f,1.0f,"%.1f");
			if (st.refreshSec < 0.1f) st.refreshSec = 0.1f;
			if (st.refreshSec > 60.0f) st.refreshSec = 60.0f;
			ImGui::TextDisabled("Polls RAM stats every N seconds.");
			ui::hint("Only the RAM bar and the Auto-Clean check. Graphs and the other numbers follow Memory sampling below.");
			ui::endCard();
			ImGui::Spacing();
		}
		if (hClean) {
			ui::beginCard("##sac",nullptr,textCard());
			ui::heading("Auto-Clean");
			if (hThr) {
				changed |= ImGui::Checkbox("Enabled##ac",&st.autoClean);
				ImGui::SameLine();
				ImGui::SetNextItemWidth(ui::px(130.0f));
				changed |= ImGui::InputInt("% threshold##ct",&st.cleanThr,1,5);
				if (st.cleanThr < 1) st.cleanThr = 1;
				if (st.cleanThr > 100) st.cleanThr = 100;
				ImGui::TextDisabled("Triggers clean when RAM usage >= threshold.");
			}
			cleanOpts(true);
			if (hMeth || areaHit()) {
				ImGui::Spacing();
				ui::heading("Clean set");
				ImGui::Text("Methods :");
				if (hMeth) {
					changed |= ImGui::Checkbox("Flush Standby##acm",&st.acFS);
					ImGui::SameLine(ui::px(220.0f));
					changed |= ImGui::Checkbox("Empty Working Sets##acm",&st.acEW);
					ui::tip(kWin ? "The Dashboard's Empty Working Sets, process by process, protected processes are skipped. Empty working sets on the Memory page is the one system call."
						: "The Dashboard's Empty Working Sets, same as Empty working sets on the Memory page.");
					changed |= ImGui::Checkbox("Purge Standby##acm",&st.acPS);
					ImGui::SameLine(ui::px(220.0f));
					changed |= ImGui::Checkbox("Flush Modified##acm",&st.acFM);
				}
				drawAreas(hMeth);
			}
			cleanOpts(false);
			ui::endCard();
			ImGui::Spacing();
		}
	}

	// one card, the fold carries its name
	if (section("Log",hLog,true,1 + cardN("Log"))) {
		ui::beginCard("##slog",nullptr,textCard());
		ImGui::SetNextItemWidth(ui::px(130.0f));
		changed |= ImGui::InputInt("max entries##ml",&st.maxLog,10,50);
		if (st.maxLog < 10) st.maxLog = 10;
		if (st.maxLog > 9999) st.maxLog = 9999;
		ImGui::SameLine();
		if (ImGui::Button("Clear Log")) st.log.clear();
		ImGui::TextDisabled("Oldest entries removed when limit is exceeded.");
		if (const cfg::Opt* o = findOpt("logFile")) {
			ImGui::Spacing();
			drawOption(*o);
			if (cfg::getb("logFile",false) && st.logErr) {
				ImGui::PushStyleColor(ImGuiCol_Text,ui::c.bad);
				ImGui::TextWrapped("File : failed (can't write %s), turn it off and on to try again",st.logPath.c_str());
				ImGui::PopStyleColor();
			} else if (cfg::getb("logFile",false)) ui::hint(("File : " + st.logPath).c_str());
		}
		ui::endCard();
		ImGui::Spacing();
	}

	if (section("Startup and window",hStart || cardHit("Window"),true,cardN("Window") + (int)plat::startupSupported() + (int)plat::noPromptSupported())) {
		if (hStart) {
			ui::beginCard("##sst",nullptr,textCard());
			ui::heading("Startup");
			bool startupCopy = st.startupEnabled;
			if (ImGui::Checkbox("Run at startup (no prompt)##st",&startupCopy)) st.setStartup(startupCopy,"");
			ImGui::TextDisabled("Launches FRS automatically when you log in.");
			if (!kWin) ui::hint("(no prompt) is about the Windows UAC dialog. Here pkexec asks for your password at each login.");
			if (!st.safeDir) ui::hint(kWin ? "FRS.exe is in a folder your account can write to. Anything running as you can replace it and get admin rights at your next logon. Program Files closes that."
				: "FRS is in a folder your account can write to. Anything running as you can replace it and get root at the next pkexec prompt. A root owned folder like /usr/local/bin closes that.");
			ImGui::Spacing();
			if (plat::noPromptSupported()) {
				bool busy = st.npTask && !st.npTask->done.load();
				if (st.npTask && !busy) {
					st.npOn = plat::noPromptOn();
					ui::toast(st.npTask->res);
					st.npTask.reset();
				}
				if (busy) ImGui::BeginDisabled();
				if (ImGui::Button(st.npOn ? "Remove no-prompt shortcut" : "Create no-prompt shortcut")) {
					if (st.npOn || st.safeDir) st.noPrompt(!st.npOn);
					else ui::askOpen(kAskNp);
				}
				if (busy) ImGui::EndDisabled();
				ui::tip("A desktop shortcut \"FRS (no prompt)\" that starts FRS as admin without the UAC prompt, through a scheduled task. Remove deletes both. Nothing is made until you click.");
				ui::hint(((st.npOn ? "Shortcut : " : "Would go to : ") + st.npLink).c_str());
			} else ImGui::TextDisabled("No-prompt shortcut : not on Linux, a polkit rule for FRS is the admin's choice.");
			ui::endCard();
			ImGui::Spacing();
		}
		drawCard("Window");
	}

	if (section("Tray and taskbar",hTray,false,nTray)) for (auto c : tray) drawCard(c);

	if (section("Display and history",hDisp || cardHit("Memory sampling"),true,nDisp)) {
		for (auto c : disp) drawCard(c);
		drawCard("Memory sampling");
	}

	if (changed) st.save();

	if (section("Advanced and per page",hAdv,false,nAdv)) for (auto& c : adv) drawCard(c);

	if (setHit("Reset settings to defaults settings.ini.bak")) {
		any = true;
		ImGui::Spacing();
		if (ImGui::Button("Reset settings...")) ui::askOpen(kAskReset);
		ui::tip("Every setting back to its default, the old file is kept as settings.ini.bak.");
	}
	if (!any) ui::empty("No setting matches");
	if (ui::askDraw(kAskNp,"FRS.exe is in a folder your account can write to. With a no-prompt shortcut anything running as you can replace FRS.exe and have it started as admin, no prompt asked. Program Files closes that.\n\nCreate the shortcut anyway?","Create",true)) st.noPrompt(true);
	if (ui::askDraw(kAskReset,kWin ? "Every setting goes back to its default. settings.ini is copied to settings.ini.bak first.\n\nKept : MCP server settings and tool modes, Run at startup, the no-prompt shortcut, window size and position, table layouts and page priority rules."
		: "Every setting goes back to its default. settings.ini is copied to settings.ini.bak first.\n\nKept : MCP server settings and tool modes, Run at startup, window size and position, table layouts and page priority rules.","Reset",true)) st.resetAll();
}

void App::State::drawCredits() {
	const float ind = ui::px(12.0f);

	ui::beginCard("##cfrs",nullptr,textCard());
	ui::heading("FRS | Free RAM System");
	ImGui::Indent(ind);
	ImGui::TextDisabled("A lightweight cross-platform RAM cleaner with a native ImGui UI.");
	ImGui::TextDisabled("Windows (Win32 + DirectX 11) and Linux (GLFW + OpenGL 3).");
	ImGui::Unindent(ind);
	ImGui::Spacing();

	ui::heading("Author");
	ImGui::TextColored(ui::c.gold,"Nems1337");
	ImGui::Indent(ind);
	ImGui::TextDisabled("Original author and developer of FRS.");
	ImGui::Unindent(ind);
	ui::endCard();
	ImGui::Spacing();

	ui::beginCard("##clibs",nullptr,textCard());
	ui::heading("Third-Party Libraries");
	ImGui::Spacing();

	ImGui::TextColored(ui::c.gold,"Dear ImGui");
	ImGui::SameLine();
	ImGui::TextDisabled("by Omar Cornut");
	ImGui::Indent(ind);
	ImGui::TextDisabled("Immediate-mode GUI library used for the entire UI.");
	ImGui::TextDisabled("https://github.com/ocornut/imgui  \xE2\x80\x94  MIT License");
	ImGui::Unindent(ind);
	ImGui::Spacing();

	ImGui::TextColored(ui::c.gold,"System Informer");
	ImGui::SameLine();
	ImGui::TextDisabled("by Winsider Seminars & Solutions");
	ImGui::Indent(ind);
	ImGui::PushStyleColor(ImGuiCol_Text,ui::c.dim);
	ImGui::TextWrapped("Memory features (page lists, process memory, regions, hex editor, pool tags, working set watch and more) are ported or adapted from it.");
	ImGui::TextWrapped("Struct layouts follow its phnt headers and the D3DKMT and CLR headers it bundles. License texts : THIRD_PARTY_NOTICES.md.");
	ImGui::PopStyleColor();
	ImGui::TextDisabled("https://github.com/winsiderss/systeminformer  \xE2\x80\x94  MIT License");
	ImGui::Unindent(ind);
	ui::endCard();
	ImGui::Spacing();

	// ideas only, no code of it is in FRS
	ui::beginCard("##cidea",nullptr,textCard());
	ui::heading("Inspiration");
	ImGui::Spacing();
	ImGui::TextColored(ui::c.gold,"Mem Reduct");
	ImGui::SameLine();
	ImGui::TextDisabled("by Henry++");
	ImGui::Indent(ind);
	ImGui::TextDisabled("Feature ideas : clean set, timer, hotkey, tray menu, command line. No code taken.");
	ImGui::TextDisabled("https://github.com/henrypp/memreduct  \xE2\x80\x94  GPL-3.0 License");
	ImGui::Unindent(ind);
	ui::endCard();
	ImGui::Spacing();

	ui::beginCard("##cpriv",nullptr,textCard());
	ui::heading("Privacy Tweaks");
	ImGui::Spacing();

	ImGui::TextColored(ui::c.gold,"hellzerg / Optimizer");
	ImGui::Indent(ind);
	ImGui::TextDisabled("Registry-based privacy and telemetry tweaks ported from");
	ImGui::TextDisabled("the Optimizer project's EnhancePrivacy / CompromisePrivacy");
	ImGui::TextDisabled("routines (C# -> C++).");
	ImGui::TextDisabled("https://github.com/hellzerg/optimizer  \xE2\x80\x94  GPL-3.0 License");
	ImGui::Unindent(ind);
	ui::endCard();
	ImGui::Spacing();

	ui::beginCard("##clic",nullptr,textCard());
	ui::heading("License");
	ImGui::Indent(ind);
	ImGui::TextDisabled("FRS is released under the MIT License.");
	ImGui::TextDisabled("See LICENSE in the project root for the full text.");
	ImGui::Unindent(ind);
	ui::endCard();
}

bool App::sizeOk = true;

App::App() : s(new State()) {
	std::lock_guard<std::mutex> g(g_stMtx);
	g_st = s;
}

App::~App() {
	// a clean step can't be stopped, it gets 5 s while everything it calls still runs. One still running after that keeps writing to State, so State stays, the process ends anyway
	for (int i = 0; i < 100 && s->cleaning.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
	s->dying = s->cleaning.load();
	mcp::shutdown();
	surf::shutdown();
	task::stopAll();
	sampler::stop();
	if (ImGui::GetCurrentContext() && ImGui::GetFrameCount() > 0) s->saveIni();
	if (cfg::takeDirty()) s->save();
	s->flushPending();
	s->writeLog();
	{
		std::lock_guard<std::mutex> g(g_stMtx);
		g_st = nullptr;
	}
	if (!s->cleaning.load()) delete s;
}

void App::init() {
	s->stats = getRamStats();
	s->elev = isElevated();
	s->lastRefresh = util::nowMs();
	s->regPages();
	domains::initAll();
	// after the Memory page's Clean entries, the palette lists it next to them
	nav::addCmd("Clean : Run clean set",[] {
		nav::go("dashboard");
		g_st->dashClean(g_st->setMask(),true,true);
	},"selected areas");
	cfg::optBool("logFile","Log","Write the log to a file","frs.log next to settings.ini, rotated at 1 MB. MCP lines in it carry process names and pids.",false);
	cfg::optBool("multiInst",kWin ? "Tray and taskbar" : "Window","Allow more than one FRS",kWin
		? "Off : starting FRS again brings this window forward. Two on one settings folder overwrite each other's settings. From the next start."
		: "Off : starting FRS again only says it already runs. Two on one settings folder overwrite each other's settings. From the next start.",false);
	for (auto& g : optGroups()) nav::addCmd(("Settings : " + g).c_str(),[g] {
		nav::go("settings");
		snprintf(g_setFlt,sizeof(g_setFlt),"%s",g.c_str());
	});
	s->regTools();
	domains::regToolsAll();
	s->load();
	std::string ini = plat::readFile("ui.ini");
	if (!ini.empty()) ImGui::LoadIniSettingsFromMemory(ini.data(),ini.size());
	s->startupEnabled = plat::startupEnabled();
	// tasks from older FRS stop on battery and after 3 days, their settings change once. What the task runs and when stays
	// a failed one is logged once (startTaskV 1), later launches try again quietly and leave the old task
	int tv = cfg::geti("startTaskV",0);
	if (kWin && s->startupEnabled && tv < 2) task::run([tv](task::T&) {
		std::string why;
		int r = plat::upgradeStartup(why);
		if (r < 0) {
			if (tv == 1) return std::string();
			cfg::seti("startTaskV",1);
			return "Run at startup : failed (" + why + ")";
		}
		cfg::seti("startTaskV",2);
		return r ? std::string("Run at startup : task updated (battery and 3 day limits removed)") : "Run at startup : task left as it is (" + why + ")";
	});
	s->safeDir = plat::exeInSafeDir();
	s->npOn = plat::noPromptOn();
	s->npLink = plat::noPromptLink();
	s->logPath = plat::filePath("frs.log");
	std::string note = plat::dirNote();
	if (!note.empty()) appLog(note);
	if (mcp::init()) s->save();
	sampler::start();
}

void App::update() {
	bool lf = cfg::getb("logFile",false);
	if (lf != s->logWas) s->logErr = false;
	s->logWas = lf;
	s->logOn = lf && !s->logErr;
	s->flushPending();
	mcp::pump();
	uint64_t now = util::nowMs();
	if (s->autoRefresh && (now - s->lastRefresh) >= (uint64_t)(s->refreshSec * 1000.0f)) {
		s->lastRefresh = now;
		s->stats = getRamStats();
		// the worker can set lastEnd after now was read
		uint64_t end = s->lastEnd.load();
		bool cool = !end || (now >= end && now - end >= (uint64_t)clampf((float)cfg::geti("acCooldown",0),0.0f,3600.0f) * 1000);
		if (s->autoClean && !s->cleaning.load() && cool) {
			float pct = (s->stats.total > 0) ? (float)s->stats.used / (float)s->stats.total * 100.0f : 0.0f;
			if (pct >= (float)s->cleanThr) {
				s->runSet("[auto] ","auto");
			}
		}
	}
	// Clean every N minutes, outside the Auto-Refresh branch so it runs without it
	bool ev = cfg::getb("acEvery",false);
	int evMin = (int)clampf((float)cfg::geti("acEveryMin",30),1.0f,1440.0f);
	if (ev && !s->timerWas) s->timerOn = now;
	s->timerWas = ev;
	int64_t next = -1;
	int set = s->setMask();
	if (ev) {
		uint64_t per = (uint64_t)evMin * 60000;
		if (now >= std::max(s->timerOn,s->lastStart) + per && !s->cleaning.load()) s->runSet("[auto] ","timer");
		uint64_t due = std::max(s->timerOn,s->lastStart) + per;
		// an empty set never cleans, no countdown either
		if (set) next = now >= due ? 0 : (int64_t)((due - now + 999) / 1000);
	}
	{
		std::lock_guard<std::mutex> lk(s->infoMtx);
		s->info.nextTimerSec = next;
		s->info.set = set;
		s->info.autoOn = s->autoClean;
		s->info.thr = s->cleanThr;
		s->info.every = ev;
		s->info.everyMin = evMin;
	}
	surf::update();

	if (ImGui::GetCurrentContext() && ImGui::GetFrameCount() > 0) {
		ImGuiIO& io = ImGui::GetIO();
		int w = (int)io.DisplaySize.x;
		int h = (int)io.DisplaySize.y;
		if (w > 0 && h > 0 && sizeOk) {
			cfg::seti("winW",w);
			cfg::seti("winH",h);
		}
		if (io.WantSaveIniSettings && now - s->lastIniSave >= 5000) s->saveIni();
	}
	if (now - s->lastCfgSave >= 1000 && cfg::takeDirty()) {
		s->lastCfgSave = now;
		s->save();
	}
	s->writeLog();
}

int App::cli(const std::vector<std::string>& args,std::string& out,bool helpOnly) {
	bool clean = false,full = false,silent = false,help = false;
	for (auto& a : args) {
		std::string w = util::lower(a);
		if (w.size() > 1 && w[0] == '/') w[0] = '-';
		if (w == "-clean") clean = true;
		else if (w == "-clean:full") clean = full = true;
		else if (w == "-silent") silent = true;
		else if (w == "-help" || w == "--help" || w == "-h" || w == "-?") help = true;
	}
	out.clear();
	if (help) {
		out = kHelp;
		return 0;
	}
	if (!clean || helpOnly) return -1;
	State st;
	st.load();
	int m = st.setMask();
	if (full) {
		m = 15;
		for (int i = 0; i < kAreaN; i++) if (areaHere(kAreas[i])) m |= 1 << (i + 4);
	}
	if (!m) {
		if (!silent) out = "The clean set is empty, tick methods in Settings > Auto-Clean.\n";
		return 1;
	}
	bool fake = fakeClean();
	RamStats b = getRamStats();
	std::vector<std::string> res;
	int nFail = 0;
	for (int i : kOrder) {
		if (!(m & (1 << i))) continue;
		res.push_back(runBit(i,st.flushSz,fake));
		if (stepFailed(res.back())) nFail++;
	}
	if (cfg::getb("cleanSummary",true)) res.push_back(sumText(b,getRamStats(),nFail,res.size()));
	std::string txt,file;
	for (auto& r : res) {
		txt += r + "\n";
		file += day() + util::ts() + " [cli] " + r + "\n";
	}
	// a viewer or a virus scan can hold the file a moment
	bool lf = cfg::getb("logFile",false);
	for (int i = 0; lf && i < 10; i++) {
		if (plat::appendFile("frs.log",file,kLogMax)) lf = false;
		else std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
	if (lf) txt += "Log file : failed (can't write " + plat::filePath("frs.log") + ")\n";
	if (!silent) out = txt;
	return nFail ? 1 : 0;
}

void App::drawUI() {
	nav::drawShell();
	procwin::draw();
	if (mcp::drawConsent()) s->save();
	ui::drawToasts();
}
