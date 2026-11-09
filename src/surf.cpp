#include "surf.h"
#include "domains.h"
#include "cfg.h"
#include "gpumem.h"
#include "mcp.h"
#include "procdet.h"
#include "ui.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

	const char* kUnits[] = {"B","KB","MB","GB","TB","PB","EB"};
#ifdef _WIN32
	const char* kAvailTip = "Pages an app can get right now : free, zeroed and standby. Standby is cache, not lost memory, and this is not the free list.";
	const char* kCommitNote = "";
	const char* kSumHint = "The header and Live view graphs put this series first, the header keeps only this one when it has room for one, and Taskbar progress fills with it.";
	const char* kUnitHint = "Largest unit in the status bar, tray and Live view. At MB, 64 GB shows as 65,536 MB.";
	const char* kPctHint = "Percents in the status bar, Live view, tray tooltips, tables and bar tooltips. The RAM bar keeps one decimal.";
#else
	const char* kAvailTip = "What the kernel estimates apps can get without swapping (MemAvailable). It includes page cache, which is not lost memory, and is not MemFree.";
	const char* kCommitNote = " The limit only binds under vm.overcommit_memory=2, so this can pass 100%.";
	const char* kSumHint = "The header and Live view graphs put this series first, the header keeps only this one when it has room for one.";
	const char* kUnitHint = "Largest unit in the status bar and Live view. At MB, 64 GB shows as 65,536 MB.";
	const char* kPctHint = "Percents in the status bar, Live view, tables and bar tooltips. The RAM bar keeps one decimal.";
#endif

	int clampi(int v,int lo,int hi) { return v < lo ? lo : v > hi ? hi : v; }
	float px1() { return ui::px(1.0f); }
	ImU32 u32(ImVec4 c) { return ImGui::GetColorU32(c); }

	uint32_t argb(ImVec4 c) {
		auto b = [](float v) { return (uint32_t)(clampi((int)(v * 255.0f + 0.5f),0,255)); };
		return 0xFF000000u | b(c.x) << 16 | b(c.y) << 8 | b(c.z);
	}

	// 0..1, negative when unknown
	double fracOf(uint64_t part,uint64_t total) {
		if (part == NA || total == NA || !total) return -1.0;
		return (double)part / (double)total;
	}

	std::string shortName(const std::string& s,size_t mx) {
		if (s.size() <= mx) return s;
		size_t n = mx;
		while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
		return s.substr(0,n) + "...";
	}


	ImVec4 g_defPhys,g_defCommit;
	std::atomic<uint32_t> g_colPhys{0},g_colCommit{0},g_colWarn{0},g_colBad{0};

	ImVec4 pickCol(int i,ImVec4 def) {
		const ImVec4 o[] = {def,ui::c.accent,ui::c.good,ui::c.warn,ui::c.bad,ui::c.info,ui::c.purple,ui::c.gold};
		return o[clampi(i,0,7)];
	}

	void applyCols() {
		ui::c.phys = pickCol(cfg::geti("colPhys",0),g_defPhys);
		ui::c.commit = pickCol(cfg::geti("colCommit",0),g_defCommit);
		g_colPhys = argb(ui::c.phys);
		g_colCommit = argb(ui::c.commit);
		g_colWarn = argb(ui::c.warn);
		g_colBad = argb(ui::c.bad);
	}

	ImVec4 loadCol(double fr,ImVec4 def) {
		if (fr < 0.0 || !cfg::getb("hlMem",true)) return def;
		double p = fr * 100.0;
		if (p >= cfg::geti("hlBad",90)) return ui::c.bad;
		if (p >= cfg::geti("hlWarn",80)) return ui::c.warn;
		return def;
	}


	struct Preset { int ms; const char* name; const char* cmd; };
	const Preset kPresets[] = {
		{500,"Fast (0.5 s)","Sampling interval : Fast (0.5 s)"},
		{1000,"Normal (1 s)","Sampling interval : Normal (1 s)"},
		{2000,"Below normal (2 s)","Sampling interval : Below normal (2 s)"},
		{5000,"Slow (5 s)","Sampling interval : Slow (5 s)"},
		{10000,"Very slow (10 s)","Sampling interval : Very slow (10 s)"}
	};

	bool paused() { return cfg::getb("memPaused",false); }

	std::string intervalStr() {
		if (paused()) return "Paused";
		int ms = sampler::intervalMs();
		char b[32];
		snprintf(b,sizeof(b),ms % 1000 ? "%.1f s" : "%.0f s",ms / 1000.0);
		return b;
	}


	struct Group {
		std::string name;
		ProcRef rep;
		std::vector<ProcRef> members;
		uint64_t sum = 0;
	};

	struct Mini {
		std::shared_ptr<const SysMem> m;
		std::shared_ptr<const SysHist> h;
		std::shared_ptr<const ProcList> l;
		std::shared_ptr<const GpuSnap> gpu;
		std::vector<Group> groups;
		uint64_t seq = UINT64_MAX;
		int sec = -1;
		ProcRef sel;
		bool stale = true;
		uint64_t kickAt = 0;
		int pick = -1; // section openMini asked for, the tab bar takes it on its next draw
	} g;

	uint64_t metric(const ProcRow& r,bool commit) { return commit ? r.priv : r.ws; }

	// no user yet (a paused start stops before the info pass reaches a row) or no access : the session stands in for it
	bool sameOwner(const ProcRow& a,const ProcRow& b) {
		if (a.user != b.user || util::lower(a.name) != util::lower(b.name)) return false;
		return !a.user.empty() || a.session == b.session;
	}

	// SI procgrp.c : biggest process first, walk up while the parent has the same name and user, take that
	// root's same named subtree, stop at 15 groups. SI's "a visible window starts its own group" rule is left
	// out, it would enumerate windows on the UI thread every tick and Linux has no such thing anyway.
	std::vector<Group> buildGroups(const ProcList& l,bool commit) {
		auto& rows = l.rows;
		int n = (int)rows.size();
		std::vector<std::vector<int>> kids(n);
		std::vector<int> order;
		for (int i = 0; i < n; i++) {
			if (rows[i].pseudo) continue;
			if (const ProcRow* p = procs::parentOf(l,rows[i])) kids[p - rows.data()].push_back(i);
			if (metric(rows[i],commit) != NA) order.push_back(i);
		}
		std::sort(order.begin(),order.end(),[&](int a,int b) { return metric(rows[a],commit) > metric(rows[b],commit); });
		std::vector<char> used(n,0);
		std::vector<Group> out;
		for (int i : order) {
			if (used[i]) continue;
			if (out.size() >= 15) break;
			int root = i;
			while (const ProcRow* p = procs::parentOf(l,rows[root])) {
				int pi = (int)(p - rows.data());
				if (p->pseudo || used[pi] || !sameOwner(*p,rows[root])) break;
				root = pi;
			}
			Group gr;
			gr.name = rows[root].name;
			gr.rep = procs::ref(rows[root]);
			std::vector<int> st{root};
			while (!st.empty()) {
				int k = st.back();
				st.pop_back();
				if (used[k]) continue;
				used[k] = 1;
				gr.members.push_back(procs::ref(rows[k]));
				uint64_t v = metric(rows[k],commit);
				if (v != NA) gr.sum += v;
				for (int c : kids[k]) if (!used[c] && sameOwner(rows[c],rows[root])) st.push_back(c);
			}
			out.push_back(std::move(gr));
		}
		std::stable_sort(out.begin(),out.end(),[](const Group& a,const Group& b) { return a.sum > b.sum; });
		return out;
	}

	void reduceGroup(const Group& gr) {
		auto refs = gr.members;
		std::string nm = gr.name;
		task::run([refs,nm](task::T&) -> std::string {
			if (refs.size() == 1) {
				std::string s = procs::reduceWs(refs[0]);
				ui::toast(s);
				return s;
			}
			int bad = 0;
			for (auto& r : refs) {
				std::string s = procs::reduceWs(r);
				if (s.find(" : OK") == std::string::npos) {
					bad++;
					appLog(s);
				}
			}
			std::string lb = "Reduce working set (" + nm + ", " + std::to_string(refs.size()) + " processes)";
			std::string s = bad ? lb + " : failed (" + std::to_string(bad) + " of them, see the log)" : lb + " : OK";
			ui::toast(s);
			return s;
		});
	}

	void takeLatest() {
		g.m = sysmem::latest();
		g.h = sysmem::history();
		g.l = procs::latest();
		g.gpu = gpumem::latest();
		// a Refresh stays stale until the sample it asked for is in, Auto off would miss it otherwise. 3 s at most
		g.stale = g.kickAt && (g.m->t < g.kickAt || g.l->t < g.kickAt) && util::nowMs() - g.kickAt < 3000;
	}

	void refreshNow() {
		g.kickAt = util::nowMs();
		g.stale = true;
		sampler::kick();
	}

	// F5 on any page, like SI's View > Refresh. Called from every surf part of the main window, once a frame.
	// A focused process window keeps F5 for its own tab.
	void keys() {
		static int fr = -1;
		if (fr == ImGui::GetFrameCount()) return;
		fr = ImGui::GetFrameCount();
		if (!ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F5,false)) refreshNow();
	}

	// the plot runs under these labels, at high use the line would strike through them
	void boxText(ImDrawList* dl,ImVec2 p,ImVec4 col,const char* t) {
		ImVec2 s = ImGui::CalcTextSize(t);
		float m = ui::px(3.0f);
		dl->AddRectFilled(ImVec2(p.x - m,p.y),ImVec2(p.x + s.x + m,p.y + s.y),u32(ui::c.bg),m);
		dl->AddText(p,u32(col),t);
	}

	std::string cutToWidth(const std::string& s,float w) {
		if (ImGui::CalcTextSize(s.c_str()).x <= w) return s;
		const char* rest = nullptr;
		float room = std::max(w - ImGui::CalcTextSize("...").x,0.0f);
		ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(),room,0.0f,s.c_str(),s.c_str() + s.size(),&rest);
		return std::string(s.c_str(),rest) + "...";
	}

	void miniGraph(bool commit,float rowH) {
		const SysMem& m = *g.m;
		const SysHist& h = *g.h;
		const std::vector<float>& v = commit ? h.commit : h.phys;
		uint64_t total = commit ? m.commitLimit : m.physTotal;
		uint64_t cur = commit ? m.commit : m.physUsed;
		const char* title = commit ? "Commit charge" : "Physical memory";
		const char* totL = commit ? "Limit" : "Total";
		ImVec4 col = commit ? ui::c.commit : ui::c.phys;
		bool scale = cfg::getb("graphMaxScale",false);
		float mx = scale || total == NA ? 0.0f : util::toMbf(total);
		// ui::graph would add 10 % on top of the biggest sample, the Max Y label must sit at the real top
		if (mx <= 0.0f) for (float f : v) mx = std::max(mx,f);
		ui::Series s{v.data(),(int)v.size(),col,title};
		auto tipFn = [&](int i) -> std::string {
			std::string t = std::string("Used : ") + surf::sizeStr(cur) + " (" + surf::pctStr(cur,total) + ")\n" + totL + " : " + surf::sizeStr(total);
			if (i < 0 || i >= (int)v.size()) return t;
			t += "\n\nSample : ";
			t += v[i] < 0.0f ? std::string("N/A") : surf::sizeStr((uint64_t)((double)v[i] * 1048576.0));
			if (i < (int)h.unixSec.size()) t += "\nAt : " + util::timeStr(h.unixSec[i]);
			const auto& tn = commit ? h.topPriv : h.topProc;
			const auto& tb = commit ? h.topPrivBytes : h.topProcBytes;
			if (i < (int)tn.size() && i < (int)tb.size() && !tn[i].empty()) t += "\nBiggest : " + tn[i] + " " + surf::sizeStr(tb[i]);
			t += "\n\nClick for the process list";
			return t;
		};
		ui::graph(commit ? "##mgc" : "##mgp",&s,1,mx,ImVec2(0.0f,rowH / px1()),tipFn,clampi(cfg::geti("memHistN",600),60,3600));
		bool hit = ImGui::IsItemClicked(ImGuiMouseButton_Left);
		ImVec2 a = ImGui::GetItemRectMin();
		ImVec2 b = ImGui::GetItemRectMax();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		float pad = ui::px(8.0f);
		float rnd = ui::px(5.0f);
		// the left of the plot fades to the background so the title stays readable
		ImVec4 bk = ui::c.bg;
		ImVec4 bk0(bk.x,bk.y,bk.z,0.0f);
		float fw = std::min(ui::px(180.0f),(b.x - a.x) * 0.5f);
		dl->AddRectFilledMultiColor(ImVec2(a.x + 1.0f,a.y + rnd),ImVec2(a.x + fw,b.y - rnd),u32(bk),u32(bk0),u32(bk0),u32(bk));
		boxText(dl,ImVec2(a.x + pad,a.y + pad * 0.6f),ui::c.text,title);
		if (cfg::getb("graphText",true)) {
			std::string t = surf::sizeStr(cur) + " (" + surf::pctStr(cur,total) + ")";
			boxText(dl,ImVec2(a.x + pad,a.y + pad * 0.6f + ImGui::GetTextLineHeight()),loadCol(fracOf(cur,total),col),t.c_str());
		}
		if (cfg::getb("graphMaxText",true)) {
			if (mx > 0.0f) {
				std::string t = surf::sizeStr((uint64_t)((double)mx * 1048576.0));
				float tw = ImGui::CalcTextSize(t.c_str()).x;
				boxText(dl,ImVec2(b.x - tw - pad,a.y + pad * 0.6f),ui::c.dim,t.c_str());
			}
		}
		if (v.empty()) {
			const char* t = "Collecting samples";
			ImVec2 ts = ImGui::CalcTextSize(t);
			dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f,(a.y + b.y - ts.y) * 0.5f),u32(ui::c.dim),t);
		}
		if (hit) surf::openMini(commit ? surf::SecCommit : surf::SecPhys);
	}

	void drawGroups(bool commit) {
		const ProcList& l = *g.l;
		// the list stays put while a menu is open, like SI's SuspendUpdate
		bool hold = g.sec == (commit ? 1 : 2) && ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId);
		if (!hold && (g.seq != l.seq || g.sec != (commit ? 1 : 2))) {
			g.seq = l.seq;
			g.sec = commit ? 1 : 2;
			g.groups = buildGroups(l,commit);
		}
		const SysMem& m = *g.m;
		uint64_t cur = commit ? m.commit : m.physUsed;
		uint64_t total = commit ? m.commitLimit : m.physTotal;
		ImVec4 col = commit ? ui::c.commit : ui::c.phys;
		ui::beginCard("##mlist",nullptr,ImVec2(0.0f,-1.0f));
		ImGui::TextColored(col,"%s",commit ? "Commit" : "Physical");
		ImGui::SameLine(0.0f,ui::px(14.0f));
		std::string lbl = surf::sizeStr(cur) + " (" + surf::pctStr(cur,total) + ")";
		ImGui::TextColored(loadCol(fracOf(cur,total),col),"%s",lbl.c_str());
		ImGui::SameLine();
		ui::help(commit ?
			"Commit charge is a system number, these rows only approximate it with private bytes, so they don't add up to it." :
			"A group adds up the working sets of its processes. Working set includes shared pages, so they count more than once and a group can show more than it really holds.");
		ImGui::BeginChild("##mrows",ImVec2(0,0));
		if (g.groups.empty()) {
			std::string e = procs::snapErr();
			ui::empty(!e.empty() ? e.c_str() : paused() ? "Sampling is paused, no process list yet" : "No process list yet");
		}
		uint64_t top = 0;
		for (auto& gr : g.groups) top = std::max(top,gr.sum);
		float lh = ImGui::GetTextLineHeight();
		float rowH = lh * 2.0f + ui::px(6.0f);
		auto live = procs::latest();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		bool escMenu = false; // that Esc closed a menu, it must not clear the selection too
		for (int i = 0; i < (int)g.groups.size(); i++) {
			const Group& gr = g.groups[i];
			ImGui::PushID((int)gr.rep.pid); // a popup stays with its process when the rows move
			bool sel = gr.rep == g.sel;
			if (ImGui::Selectable("##r",sel,ImGuiSelectableFlags_AllowDoubleClick,ImVec2(0.0f,rowH))) {
				g.sel = gr.rep;
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) procwin::open(gr.rep);
			}
			ImVec2 a = ImGui::GetItemRectMin();
			ImVec2 b = ImGui::GetItemRectMax();
			if (top) {
				float w = (b.x - a.x) * (float)((double)gr.sum / (double)top);
				dl->AddRectFilled(ImVec2(a.x,b.y - ui::px(2.0f)),ImVec2(a.x + w,b.y),u32(ImVec4(col.x,col.y,col.z,0.45f)));
			}
			float x = a.x + ui::px(6.0f);
			float y = a.y + ui::px(2.0f);
			bool dead = !live->find(gr.rep);
			if (dead) {
				const char* t = "(Terminated) ";
				dl->AddText(ImVec2(x,y),u32(ui::c.dim),t);
				x += ImGui::CalcTextSize(t).x;
			}
			std::string val = surf::sizeStr(gr.sum);
			float vx = b.x - ImGui::CalcTextSize(val.c_str()).x - ui::px(6.0f);
			std::string nm = cutToWidth(gr.name,vx - ui::px(10.0f) - x);
			dl->AddText(ImVec2(x,y),u32(dead ? ui::c.dim : ui::c.text),nm.c_str());
			if (nm != gr.name) ui::tip(gr.name.c_str());
			std::string sub = gr.members.size() > 1 ? std::to_string(gr.members.size()) + " processes" : "pid " + std::to_string(gr.rep.pid);
			dl->AddText(ImVec2(a.x + ui::px(6.0f),y + lh),u32(ui::c.dim),sub.c_str());
			const char* unit = commit ? "Private bytes" : "Working set";
			dl->AddText(ImVec2(vx,y),u32(ui::c.text),val.c_str());
			dl->AddText(ImVec2(b.x - ImGui::CalcTextSize(unit).x - ui::px(6.0f),y + lh),u32(ui::c.dim),unit);
			if (ImGui::BeginPopupContextItem("##ctx")) {
				g.sel = gr.rep;
				if (ImGui::MenuItem("Open details","Enter")) procwin::open(gr.rep);
				if (ImGui::MenuItem("Show in Processes")) {
					procs::select(gr.members);
					nav::go("processes");
				}
				ImGui::Separator();
				bool canTrim = false;
				for (auto& r : gr.members) {
					const ProcRow* p = live->find(r);
					if (p && !p->pseudo && !procdet::sysLike(*p)) canTrim = true;
				}
				if (ImGui::MenuItem(gr.members.size() > 1 ? "Reduce working set of the group" : "Reduce working set",nullptr,false,canTrim)) reduceGroup(gr);
				// saveMenu draws nothing for these, the menu would end on a separator
				const ProcRow* rp = live->find(gr.rep);
				if (rp && !rp->pseudo && !procdet::sysLike(*rp)) {
					ImGui::Separator();
					procdet::saveMenu(gr.rep);
				}
				// ImGui closes popups on Esc only with keyboard nav, FRS has it off
				if (ImGui::IsKeyPressed(ImGuiKey_Escape,false)) {
					ImGui::CloseCurrentPopup();
					escMenu = true;
				}
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}
		if (g.sel.pid && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter,false)) procwin::open(g.sel);
		// a full list leaves no empty space to click, so Esc does it too
		bool emptyHit = ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
		if (emptyHit || (!escMenu && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape,false))) g.sel = ProcRef();
		ImGui::EndChild();
		ui::endCard();
	}

	void gpuCard(const char* id,const char* cap,const char* tip,const GpuSnap& s,bool shared,ImVec4 col,float w) {
		uint64_t cur = shared ? s.shared : s.ded;
		uint64_t lim = shared ? s.sharedLimit : s.dedLimit;
		std::vector<const GpuProc*> v;
		for (auto& p : s.procs) {
			uint64_t x = shared ? p.shared : p.ded;
			if (x != NA && x) v.push_back(&p);
		}
		std::sort(v.begin(),v.end(),[shared](const GpuProc* a,const GpuProc* b) { return shared ? a->shared > b->shared : a->ded > b->ded; });
		ui::beginCard(id,nullptr,ImVec2(w / px1(),-1.0f));
		ImGui::TextColored(col,"%s",cap);
		ImGui::SameLine(0.0f,ui::px(14.0f));
		// a dedicated limit of 0 is an iGPU, there is nothing to divide by
		std::string lbl = surf::sizeStr(cur);
		if (lim != NA && lim) lbl += " / " + surf::sizeStr(lim) + " (" + surf::pctStr(cur,lim) + ")";
		ImGui::TextUnformatted(lbl.c_str());
		ImGui::SameLine();
		ui::help(tip);
		ImGui::BeginChild("##r",ImVec2(0,0));
		if (v.empty()) ui::empty("Nothing right now");
		ImDrawList* dl = ImGui::GetWindowDrawList();
		for (size_t i = 0; i < v.size() && i < 15; i++) {
			const GpuProc& p = *v[i];
			ImGui::PushID((int)p.pid);
			if (ImGui::Selectable("##g",false,ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				ProcRef r;
				r.pid = p.pid;
				r.start = p.start;
				r.name = p.name;
				procwin::open(r,"GPU");
			}
			ImVec2 a = ImGui::GetItemRectMin();
			ImVec2 b = ImGui::GetItemRectMax();
			std::string val = surf::sizeStr(shared ? p.shared : p.ded);
			float vw = ImGui::CalcTextSize(val.c_str()).x;
			float pad = ui::px(6.0f);
			// same name more than once (frs.exe x6), the pid tells them apart like the Commit charge list
			std::string pid = "pid " + std::to_string(p.pid);
			float pw = ImGui::CalcTextSize(pid.c_str()).x;
			std::string nm = cutToWidth(p.name,b.x - a.x - vw - pw - pad * 4.0f);
			dl->AddText(ImVec2(a.x + pad,a.y),u32(ui::c.text),nm.c_str());
			dl->AddText(ImVec2(a.x + pad * 2.0f + ImGui::CalcTextSize(nm.c_str()).x,a.y),u32(ui::c.dim),pid.c_str());
			dl->AddText(ImVec2(b.x - vw - pad,a.y),u32(ui::c.text),val.c_str());
			ImGui::PopID();
		}
		ImGui::EndChild();
		ui::endCard();
	}

	void drawGpu() {
		const GpuSnap& s = *g.gpu;
		if (!s.t) {
			ui::empty("Reading GPU memory...");
			return;
		}
		if (!s.on || s.adapters.empty()) {
			ui::empty(s.why.empty() ? "No GPU found." : util::safe(s.why).c_str());
			return;
		}
		float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		gpuCard("##gded","Dedicated","VRAM on the card, not system RAM. Double click a row for its GPU tab.",s,false,ui::c.purple,w);
		ImGui::SameLine();
		gpuCard("##gshr","Shared","System RAM the GPU has resident. It is already part of used RAM, never add it on top. Double click a row for its GPU tab.",s,true,ui::c.phys,w);
	}

	void drawMini() {
		bool autoR = cfg::getb("miniAuto",true);
		keys();
		if (!ImGui::GetIO().WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_F6,false) || ImGui::IsKeyPressed(ImGuiKey_Pause,false))) cfg::setb("miniAuto",autoR = !autoR);
		if (autoR || g.stale || !g.m) takeLatest();
		const SysMem& m = *g.m;

		ui::beginCard("##mnums",nullptr);
		float w = ImGui::GetContentRegionAvail().x / 4.0f;
		float x0 = ImGui::GetCursorPosX();
		std::string lb = "Physical  " + surf::pctStr(m.physUsed,m.physTotal);
		ui::stat(lb.c_str(),surf::sizeStr(m.physUsed),loadCol(fracOf(m.physUsed,m.physTotal),ui::c.phys));
		ImGui::SameLine(x0 + w);
		lb = "Available  " + surf::pctStr(m.physAvail,m.physTotal);
		ui::stat(lb.c_str(),surf::sizeStr(m.physAvail));
		ui::tip(kAvailTip);
		ImGui::SameLine(x0 + w * 2.0f);
		lb = "Commit  " + surf::pctStr(m.commit,m.commitLimit);
		ui::stat(lb.c_str(),surf::sizeStr(m.commit),loadCol(fracOf(m.commit,m.commitLimit),ui::c.commit));
		ImGui::SameLine(x0 + w * 3.0f);
		ui::stat("Commit limit",surf::sizeStr(m.commitLimit));
		ui::endCard();
		ImGui::Spacing();

		int sec = clampi(cfg::geti("miniSec",surf::SecGraphs),0,3);
		const char* names[] = {"Graphs","Commit charge","Physical memory","GPU memory"};
		static bool first = true;
		int pick = first ? sec : g.pick;
		first = false;
		g.pick = -1;
		float cy = ImGui::GetCursorPosY();
		if (ImGui::BeginTabBar("##msec")) {
			// a picked tab shows from the next frame, until then the bar still reports the old one
			for (int i = 0; i < 4; i++) if (ImGui::BeginTabItem(names[i],nullptr,i == pick ? ImGuiTabItemFlags_SetSelected : 0)) {
				if (pick < 0 && i != sec) cfg::seti("miniSec",sec = i);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		} else {
			g.pick = pick;
		}
		float by = ImGui::GetCursorPosY();
		const char* note = paused() ? "Sampling paused" : autoR ? nullptr : "Frozen";
		const ImGuiStyle& st = ImGui::GetStyle();
		float box = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x;
		float rw = ImGui::GetFrameHeight() + st.ItemSpacing.x + box + ImGui::CalcTextSize("Auto update").x;
		if (note) rw += ImGui::CalcTextSize(note).x + ui::px(12.0f);
		float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		ImGui::SetCursorPos(ImVec2(right - rw,cy));
		if (note) {
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(ui::c.warn,"%s",note);
			ImGui::SameLine(0.0f,ui::px(12.0f));
		}
		if (ui::iconBtn("minisn",ui::IcoRefresh,paused() ? "Takes one memory sample now, sampling stays paused. F5 on any page" : "Takes a memory sample now. F5 on any page")) refreshNow();
		ImGui::SameLine();
		if (ImGui::Checkbox("Auto update##mini",&autoR)) cfg::setb("miniAuto",autoR);
		ui::tip("Numbers and lists follow every sample. Off freezes this view only. F6 or Pause");
		ImGui::SetCursorPosY(by);

		if (sec == surf::SecGraphs) {
			float gap = st.ItemSpacing.y;
			float rowH = std::max(ui::px(70.0f),(ImGui::GetContentRegionAvail().y - gap) * 0.5f);
			bool lead = cfg::getb("sumCommit",true);
			miniGraph(lead,rowH);
			miniGraph(!lead,rowH);
		} else if (sec == surf::SecGpu) {
			drawGpu();
		} else {
			drawGroups(sec == surf::SecCommit);
		}
	}

	uint64_t selSum(bool priv) {
		std::vector<ProcRef> sel = procs::selected();
		if (sel.empty()) return NA;
		auto l = procs::latest();
		uint64_t s = 0;
		bool any = false;
		for (auto& r : sel) {
			const ProcRow* p = l->find(r);
			uint64_t v = p ? (priv ? p->priv : p->ws) : NA;
			if (v == NA) continue;
			s += v;
			any = true;
		}
		return any ? s : NA;
	}


	enum { SbPhys,SbAvail,SbCommit,SbTop,SbSelWs,SbSelPriv,SbProcs,SbInterval,SbMcp,SbClean,SbN };
	struct SbDef { const char* key; const char* name; bool def; };
	const SbDef kSb[SbN] = {
		{"sbPhys","Physical memory",false},
		{"sbAvail","Available memory",true},
		{"sbCommit","Commit charge",true},
		{"sbTop","Top process",true},
		{"sbSelWs","Selected working set",false},
		{"sbSelPriv","Selected private bytes",false},
		{"sbProcs","Processes",true},
		{"sbInterval","Sampling status",false},
		{"sbMcp","MCP server",true},
		{"sbClean","Cleaning state",true}
	};
	// goes first when the row does not fit. Off by default means the user asked for it, those stay longest
	const int kDrop[] = {SbProcs,SbMcp,SbClean,SbTop,SbCommit,SbAvail,SbInterval,SbSelPriv,SbSelWs,SbPhys};
	bool g_sbCut[SbN] = {}; // turned on but no room, the menu says so

	struct SbItem {
		int id = 0;
		std::string lbl,val,tip;
		ImVec4 col;
		const char* go = nullptr;
		ProcRef proc;
		float lw = 0.0f,w = 0.0f;
	};

	struct Top {
		uint64_t seq = UINT64_MAX;
		std::string name;
		uint64_t ws = NA;
		ProcRef ref;
	};

	const Top& topProc() {
		static Top t;
		auto l = procs::latest();
		if (l->seq == t.seq) return t;
		t = Top();
		t.seq = l->seq;
		for (auto& r : l->rows) {
			if (r.pseudo || r.ws == NA || (t.ws != NA && r.ws <= t.ws)) continue;
			t.ws = r.ws;
			t.name = r.name;
			t.ref = procs::ref(r);
		}
		return t;
	}

	// port and bind come out of mcp::serialize, once a second is plenty. The token in there stays here.
	void mcpState(SbItem& it) {
		static uint64_t next = 0;
		static std::string port = "?";
		static bool on = false,lan = false;
		uint64_t now = util::nowMs();
		if (now >= next) {
			next = now + 1000;
			std::string s = "\n" + mcp::serialize();
			auto val = [&](const char* k) {
				std::string key = std::string("\n") + k + "=";
				size_t p = s.find(key);
				if (p == std::string::npos) return std::string();
				p += key.size();
				return s.substr(p,s.find('\n',p) - p);
			};
			on = val("mcpOn") == "1";
			lan = val("mcpLan") == "1";
			port = val("mcpPort");
		}
		bool run = mcp::alive();
		it.lbl = "MCP";
		it.go = "mcp";
		if (run) {
			it.val = "on :" + port;
			it.col = ui::c.good;
			it.tip = std::string("Server running on port ") + port + (lan ? ", open to all interfaces." : ", this PC only.");
		} else if (on) {
			it.val = "failed";
			it.col = ui::c.bad;
			it.tip = "The server is enabled but not running, see the MCP page.";
		} else {
			it.val = "off";
			it.col = ui::c.dim;
			it.tip = "Server is off.";
		}
		it.tip += " Double click for the MCP page.";
	}

	// last clean and the timer under the Cleaning state, the time alone when it was today
	std::string cleanTip() {
		CleanInfo ci = appCleanInfo();
		std::string s;
		if (ci.lastUnix) {
			std::string t = util::timeStr(ci.lastUnix);
			std::string n = util::timeStr((uint64_t)time(nullptr));
			if (t.size() > 11 && t.compare(0,10,n,0,10) == 0) t = t.substr(11);
			char b[48] = ", freed nothing";
			if (ci.freedMb > 0) snprintf(b,sizeof(b),", freed %.0f MB",ci.freedMb);
			s += "\nLast clean " + t + b;
		}
		if (ci.nextTimerSec >= 0) s += "\nNext timer clean in " + (ci.nextTimerSec < 60 ? std::to_string(ci.nextTimerSec) + " s" : std::to_string((ci.nextTimerSec + 59) / 60) + " min");
		return s;
	}

	std::vector<SbItem> sbItems(const SysMem& m,bool& busy) {
		std::vector<SbItem> v;
		auto on = [](int i) { return cfg::getb(kSb[i].key,kSb[i].def); };
		auto add = [&](int id,const char* lbl) -> SbItem& {
			v.emplace_back();
			SbItem& it = v.back();
			it.id = id;
			it.lbl = lbl;
			it.col = ui::c.text;
			it.go = "memory";
			return it;
		};
		const char* dbl = " Double click for the Memory page.";
		if (on(SbPhys)) {
			SbItem& it = add(SbPhys,"Physical");
			it.val = surf::sizeStr(m.physUsed) + " (" + surf::pctStr(m.physUsed,m.physTotal) + ")";
			it.col = loadCol(fracOf(m.physUsed,m.physTotal),ui::c.text);
			it.tip = std::string("Physical memory in use of ") + surf::sizeStr(m.physTotal) + ", total minus available." + dbl;
		}
		if (on(SbAvail)) {
			SbItem& it = add(SbAvail,"Available");
			it.val = surf::sizeStr(m.physAvail) + " (" + surf::pctStr(m.physAvail,m.physTotal) + ")";
			it.tip = std::string(kAvailTip) + dbl;
		}
		if (on(SbCommit)) {
			SbItem& it = add(SbCommit,"Commit");
			it.val = surf::sizeStr(m.commit) + " (" + surf::pctStr(m.commit,m.commitLimit) + ")";
			it.col = loadCol(fracOf(m.commit,m.commitLimit),ui::c.text);
			it.tip = "Commit charge of a limit of " + surf::sizeStr(m.commitLimit) + "." + kCommitNote + dbl;
		}
		if (on(SbTop)) {
			const Top& t = topProc();
			SbItem& it = add(SbTop,"Top");
			it.go = "processes";
			if (t.ws == NA) {
				it.val = "N/A";
				it.col = ui::c.dim;
				it.tip = "No process list yet.";
			} else {
				it.val = shortName(t.name,20) + " " + surf::sizeStr(t.ws);
				it.tip = t.name + " has the biggest working set, pid " + std::to_string(t.ref.pid) + ". Working set includes shared pages. Double click for its details.";
				it.proc = t.ref;
				it.go = nullptr;
			}
		}
		if (on(SbSelWs)) {
			SbItem& it = add(SbSelWs,"Selected WS");
			it.val = surf::sizeStr(selSum(false));
			it.go = "processes";
			it.tip = "Working set of the rows selected on the Processes page, shared pages count more than once.";
		}
		if (on(SbSelPriv)) {
			SbItem& it = add(SbSelPriv,"Selected private");
			it.val = surf::sizeStr(selSum(true));
			it.go = "processes";
			it.tip = "Private bytes of the rows selected on the Processes page.";
		}
		if (on(SbProcs)) {
			// the list's own count, the OS ProcessCount also holds exited processes still referenced and runs higher
			SbItem& it = add(SbProcs,"Processes");
			auto l = procs::latest();
			it.val = l->t ? util::count(l->rows.size()) : "N/A";
			it.go = "processes";
			it.tip = "Processes in the latest list, the count the Processes page shows. Double click for the list.";
		}
		bool pz = paused();
		if (on(SbInterval) || pz) {
			SbItem& it = add(SbInterval,"Sampling");
			it.val = intervalStr();
			if (pz) it.col = ui::c.warn;
			it.go = "settings";
			it.tip = pz ? "Memory sampling is paused, the numbers stay as they are. F5 takes one sample, right click to resume." : "How often the memory numbers update. Right click to change it.";
		}
		if (on(SbMcp)) mcpState(add(SbMcp,"MCP"));
		bool cl = appCleaning();
		bool cmd = sysmem::busy();
		busy = cl || cmd;
		if (on(SbClean)) {
			SbItem& it = add(SbClean,"");
			it.val = cl ? "Cleaning..." : cmd ? "Emptying..." : "Idle";
			it.col = busy ? ui::c.accent : ui::c.dim;
			it.go = cl ? "dashboard" : cmd ? "memory" : "dashboard";
			it.tip = cl ? "A clean is running. Double click for the Dashboard." : cmd ? "A memory command is running. Double click for the Memory page." : "No clean is running.";
			it.tip += cleanTip();
		}
		return v;
	}

	void drawSbItem(const SbItem& it) {
		ImGui::PushID(it.id);
		ImVec2 p = ImGui::GetCursorScreenPos();
		float h = ImGui::GetTextLineHeight();
		ImGui::InvisibleButton("##i",ImVec2(it.w,h));
		bool hov = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (hov) dl->AddRectFilled(ImVec2(p.x - ui::px(4.0f),p.y - ui::px(1.0f)),ImVec2(p.x + it.w + ui::px(4.0f),p.y + h + ui::px(1.0f)),u32(ui::c.panel2),ui::px(4.0f));
		if (!it.lbl.empty()) dl->AddText(p,u32(ui::c.dim),it.lbl.c_str());
		dl->AddText(ImVec2(p.x + it.lw,p.y),u32(it.col),it.val.c_str());
		if (!it.tip.empty()) ui::tip(it.tip.c_str());
		if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			if (it.proc.pid) procwin::open(it.proc);
			else if (it.go) nav::go(it.go);
		}
		ImGui::PopID();
	}

	void drawSbMenu() {
		if (!ImGui::BeginPopup("##sbmenu")) return;
		ImGui::TextDisabled("Status bar");
		for (int i = 0; i < SbN; i++) {
			const SbDef& s = kSb[i];
			bool on = cfg::getb(s.key,s.def);
			if (ImGui::MenuItem(s.name,g_sbCut[i] ? "no room" : nullptr,on)) cfg::setb(s.key,!on);
		}
		ImGui::Separator();
		if (ImGui::BeginMenu("Sampling interval")) {
			int cur = sampler::intervalMs();
			for (auto& p : kPresets) if (ImGui::MenuItem(p.name,nullptr,p.ms == cur)) cfg::seti("memRefreshMs",p.ms);
			ImGui::EndMenu();
		}
		bool pz = paused();
		if (ImGui::MenuItem("Pause sampling",nullptr,pz)) cfg::setb("memPaused",!pz);
		if (ImGui::MenuItem("Sample now","F5")) refreshNow();
		if (ImGui::IsKeyPressed(ImGuiKey_Escape,false)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}


	void hdrGraph(const char* id,const char* lbl,const char* title,bool commit,float x,float sy,float w,const SysMem& m,const SysHist& h) {
		const std::vector<float>& v = commit ? h.commit : h.phys;
		uint64_t total = commit ? m.commitLimit : m.physTotal;
		ImVec4 col = commit ? ui::c.commit : ui::c.phys;
		float fh = ImGui::GetFrameHeight();
		float lw = ImGui::CalcTextSize(lbl).x + ui::px(5.0f);
		bool showL = w >= lw + ui::px(50.0f);
		if (showL) {
			ImGui::SetCursorScreenPos(ImVec2(x,sy));
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s",lbl);
			x += lw;
			w -= lw;
		}
		ImVec2 a(x,sy + ui::px(2.0f));
		ImVec2 b(x + w,sy + fh - ui::px(2.0f));
		ImGui::SetCursorScreenPos(a);
		ImGui::InvisibleButton(id,ImVec2(b.x - a.x,b.y - a.y));
		bool hov = ImGui::IsItemHovered();
		bool dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		float rnd = ui::px(4.0f);
		dl->AddRectFilled(a,b,u32(ui::c.bg),rnd);
		int n = (int)v.size();
		float step = ui::px(2.0f);
		int k = std::min(n,(int)(w / step) + 1);
		float wd = (float)std::max(k - 1,0) * step;
		float mx = cfg::getb("graphMaxScale",false) || total == NA ? 0.0f : util::toMbf(total);
		if (k >= 2) {
			ImGui::SetCursorScreenPos(ImVec2(b.x - wd,a.y + 1.0f));
			ui::sparkline(id,v.data() + n - k,k,mx,ImVec2(wd / px1(),(b.y - a.y - 2.0f) / px1()),col);
		}
		dl->AddRect(a,b,u32(ui::c.line),rnd);
		if (hov) {
			uint64_t cur = commit ? m.commit : m.physUsed;
			std::string t = std::string(title) + " : " + surf::sizeStr(cur) + " (" + surf::pctStr(cur,total) + ")";
			if (k >= 2) {
				float f = (ImGui::GetIO().MousePos.x - (b.x - wd)) / wd;
				if (f >= 0.0f) {
					int i = n - k + clampi((int)(f * (float)(k - 1) + 0.5f),0,k - 1);
					t += "\nSample : ";
					t += v[i] < 0.0f ? std::string("N/A") : surf::sizeStr((uint64_t)((double)v[i] * 1048576.0));
					if (i < (int)h.unixSec.size()) t += "\nAt : " + util::timeStr(h.unixSec[i]);
				}
			}
			t += "\nDouble click for the Memory page";
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(t.c_str());
			ImGui::EndTooltip();
		}
		if (dbl) nav::go("memory");
	}


	uint64_t g_nextSlow = 0;
	bool g_lowOn = false;
	bool g_autoWas = false;

	void lowCheck() {
		if (!cfg::getb("lowOn",false)) {
			g_lowOn = false;
			return;
		}
		auto m = sysmem::latest();
		double fr = fracOf(m->physAvail,m->physTotal);
		if (fr < 0.0) return;
		double pct = fr * 100.0;
		int thr = clampi(cfg::geti("lowPct",10),1,90);
		if (!g_lowOn && pct < (double)thr) {
			g_lowOn = true;
			std::string s = "Low memory : " + surf::sizeStr(m->physAvail) + " available (" + surf::pctStr(m->physAvail,m->physTotal) + ")";
			appLog("[!] " + s);
			ui::toast(s);
		} else if (g_lowOn && pct > (double)thr + 2.0) {
			g_lowOn = false;
		}
	}

	// SI's Automatic : half the virtual screen width, one sample per 2 pixels. Keeps memHistN in its range.
	void histAuto() {
		bool on = cfg::getb("graphHistAuto",false);
		if (on && !g_autoWas) {
			int w = surf::os::screenW();
			if (w > 0) cfg::seti("memHistN",clampi((w + 1) / 2,60,3600));
		}
		g_autoWas = on;
	}


}

namespace surf {

	uint32_t physCol() { return g_colPhys.load(); }
	uint32_t commitCol() { return g_colCommit.load(); }
	uint32_t warnCol() { return g_colWarn.load(); }
	uint32_t badCol() { return g_colBad.load(); }

	std::string sizeStr(uint64_t b) {
		int mx = clampi(cfg::geti("uiMaxUnit",6),0,6);
		if (b == NA || mx == 6) return util::size(b);
		int u = 0;
		double v = (double)b;
		while (v >= 1024.0 && u < mx) {
			v /= 1024.0;
			u++;
		}
		if (v >= 1000.0) return util::count((uint64_t)(v + 0.5)) + " " + kUnits[u];
		char t[48];
		snprintf(t,sizeof(t),u ? "%.2f %s" : "%.0f %s",v,kUnits[u]);
		return t;
	}

	std::string pctStr(uint64_t part,uint64_t total) {
		return util::pct(part,total);
	}

	void openMini(int sec) {
		cfg::seti("miniSec",sec);
		g.pick = sec;
		nav::go("mini");
	}

	void rasterGraph(uint32_t* px,int w,int h,const std::vector<float>& v,float maxv,uint32_t col,bool clear) {
		uint32_t line = col | 0xFF000000u;
		uint32_t fill = 0xFF000000u | ((col >> 1) & 0x7F7F7Fu);
		std::fill(px,px + w * h,clear ? 0u : 0xFF000000u);
		if (maxv <= 0.0f || v.empty() || w <= 0 || h <= 0) return;
		int n = (int)v.size();
		int last = -1;
		for (int x = w - 1,k = 0; x >= 0; x--,k++) {
			int i = n - 1 - k / 2;
			if (i < 0 || ((k & 1) && i == 0)) break;
			float s = v[i];
			if (k & 1) {
				float o = v[i - 1];
				s = s < 0.0f || o < 0.0f ? -1.0f : (s + o) * 0.5f;
			}
			if (s < 0.0f) {
				last = -1;
				continue;
			}
			float fr = s / maxv;
			fr = fr < 0.0f ? 0.0f : fr > 1.0f ? 1.0f : fr;
			int y = h - 1 - (int)(fr * (float)(h - 1) + 0.5f);
			for (int yy = y + 1; yy < h; yy++) px[yy * w + x] = fill;
			int y0 = last < 0 ? y : std::min(y,last);
			int y1 = last < 0 ? y : std::max(y,last);
			for (int yy = y0; yy <= y1; yy++) px[yy * w + x] = line;
			last = y;
		}
	}

	void init() {
		g_defPhys = ui::c.phys;
		g_defCommit = ui::c.commit;
		g_colPhys = argb(g_defPhys);
		g_colCommit = argb(g_defCommit);
		g_colWarn = argb(ui::c.warn);
		g_colBad = argb(ui::c.bad);

		const char* gs = "Status bar and header";
		cfg::optBool("sbOn",gs,"Status bar","One row of memory numbers at the bottom. Right click it to pick what it shows, double click a number to open its page.",true);
		cfg::optBool("hdrPhys",gs,"Physical memory graph in the header","Double click it for the Memory page.",true);
		cfg::optBool("hdrCommit",gs,"Commit charge graph in the header","Double click it for the Memory page.",true);
		cfg::optBool("sumCommit",gs,"Show commit charge before physical memory in the summary",kSumHint,true);
		// the right click menu sets the same keys, as options the AI can set them too
		for (auto& s : kSb) cfg::optBool(s.key,"Status bar items",s.name,"",s.def);

		const std::vector<std::string> cols = {"Default","Cyan","Green","Orange","Red","Blue","Purple","Gold"};
		const char* gg = "Graphs and numbers";
		cfg::optEnum("colPhys",gg,"Physical memory color","Every physical memory graph and icon.",0,cols);
		cfg::optEnum("colCommit",gg,"Commit charge color","Every commit charge graph and icon.",0,cols);
		cfg::optBool("graphText",gg,"Show text","Current value over the Live view graphs.",true);
		cfg::optBool("graphMaxText",gg,"Max Y label","Size at the top of the Live view graphs.",true);
		cfg::optBool("graphMaxScale",gg,"Scale to the visible maximum","Live view and header graphs scale to the biggest sample shown instead of the total or the limit.",false);
		cfg::optBool("graphHistAuto",gg,"History length from the screen width","Sets History samples to one sample per 2 pixels of all your screens side by side, within its range.",false);
		cfg::optEnum("uiMaxUnit",gg,"Max size unit",kUnitHint,6,{"B","KB","MB","GB","TB","PB","Auto"});
		cfg::optInt("uiPctPrec",gg,"Percent decimals",kPctHint,1,0,3);

		const char* gh = "Memory highlights";
		cfg::optBool("hlMem",gh,"Color memory numbers by load","In the status bar and Live view, physical and commit numbers turn orange, then red, past the thresholds below.",true);
		cfg::optInt("hlWarn",gh,"Warning at, %","",80,1,100);
		cfg::optInt("hlBad",gh,"Critical at, %","",90,1,100);
		cfg::optBool("lowOn",gh,"Low memory notice","A toast and a log line when available memory drops under the threshold. Once per drop.",false);
		cfg::optInt("lowPct",gh,"Low memory under, % available","",10,1,90);

		// the Regions tab reads these keys and has the same switches in its Options menu
		const char* gr = "Regions";
		cfg::optBool("regHlExec",gr,"Highlight executable pages","First match wins : executable, CFG, system, modified, mapped, private.",false);
#ifdef _WIN32
		cfg::optBool("regHlCfg",gr,"Highlight CFG pages","",false);
#endif
		cfg::optBool("regHlSystem",gr,"Highlight system pages","All image memory, not just system files. Linux : file backed code.",false);
		cfg::optBool("regHlModified",gr,"Highlight modified pages","Mapped or image regions with copy on write pages. Shows code patches.",false);
		cfg::optBool("regHlMapped",gr,"Highlight mapped views","",false);
		cfg::optBool("regHlPrivate",gr,"Highlight private pages","",false);

		os::opts();

		nav::addPage("mini","Live view",ui::IcoGraph,nav::GrpMain,drawMini);
		for (auto& p : kPresets) {
			int ms = p.ms;
			nav::addCmd(p.cmd,[ms] { cfg::seti("memRefreshMs",ms); });
		}
		nav::addCmd("Pause sampling",[] { cfg::setb("memPaused",true); });
		nav::addCmd("Resume sampling",[] { cfg::setb("memPaused",false); });
		nav::addCmd("Sample now (F5)",refreshNow);
	}

	void regTools() {}

	void update() {
		uint64_t now = util::nowMs();
		if (now >= g_nextSlow) {
			g_nextSlow = now + 250;
			applyCols();
			lowCheck();
			histAuto();
			os::setOnTop(cfg::getb("uiOnTop",false));
		}
		os::update();
	}

	void drawStatusBar() {
		keys();
		auto m = sysmem::latest();
		bool busy = false;
		std::vector<SbItem> v = sbItems(*m,busy);
		float sp = ui::px(5.0f);
		// a part only grows, reset now and then, so digits changing don't make the row jitter
		static float wd[SbN] = {};
		static uint64_t reset = 0;
		uint64_t now = util::nowMs();
		if (now >= reset) {
			reset = now + 10000;
			memset(wd,0,sizeof(wd));
		}
		for (auto& it : v) {
			it.lw = it.lbl.empty() ? 0.0f : ImGui::CalcTextSize(it.lbl.c_str()).x + sp;
			wd[it.id] = std::max(wd[it.id],it.lw + ImGui::CalcTextSize(it.val.c_str()).x);
			it.w = wd[it.id];
		}
		float gap = ui::px(22.0f);
		float av = ImGui::GetContentRegionAvail().x;
		auto total = [&] {
			float t = 0.0f;
			for (auto& it : v) t += it.w;
			return t + gap * (float)std::max((int)v.size() - 1,0);
		};
		bool pz = paused();
		memset(g_sbCut,0,sizeof(g_sbCut));
		for (int d : kDrop) {
			if (total() <= av) break;
			if ((d == SbInterval && pz) || (d == SbClean && busy)) continue;
			auto e = std::remove_if(v.begin(),v.end(),[&](const SbItem& it) { return it.id == d; });
			g_sbCut[d] = e != v.end();
			v.erase(e,v.end());
		}
		float rw = 0.0f;
		int nr = 0;
		for (auto& it : v) if (it.id >= SbMcp) {
			rw += it.w + (nr ? gap : 0.0f);
			nr++;
		}
		float x0 = ImGui::GetCursorPosX();
		float y0 = ImGui::GetCursorPosY();
		float x = x0;
		float h = ImGui::GetTextLineHeight();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		bool first = true;
		bool right = false;
		for (auto& it : v) {
			if (it.id >= SbMcp && !right) {
				right = true;
				x = std::max(x,x0 + av - rw);
				first = true;
			}
			ImGui::SetCursorPos(ImVec2(x,y0));
			if (!first) {
				ImVec2 q = ImGui::GetCursorScreenPos();
				dl->AddLine(ImVec2(q.x - gap * 0.5f,q.y + ui::px(2.0f)),ImVec2(q.x - gap * 0.5f,q.y + h - ui::px(2.0f)),u32(ui::c.line));
			}
			first = false;
			drawSbItem(it);
			x += it.w + gap;
		}
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##sbmenu");
		drawSbMenu();
	}

	void drawHeaderGraphs() {
		keys();
		bool ph = cfg::getb("hdrPhys",true);
		bool co = cfg::getb("hdrCommit",true);
		float av = ImGui::GetContentRegionAvail().x;
		float gap = ui::px(10.0f);
		if (ph && co && av < ui::px(180.0f)) {
			if (cfg::getb("sumCommit",true)) ph = false;
			else co = false;
		}
		int n = (int)ph + (int)co;
		if (!n) return;
		auto m = sysmem::latest();
		auto h = sysmem::history();
		float w = std::min((av - gap * (float)(n - 1)) / (float)n,ui::px(190.0f));
		float x = ImGui::GetCursorScreenPos().x + av - (w * (float)n + gap * (float)(n - 1));
		float sy = ImGui::GetCursorScreenPos().y;
		bool lead = cfg::getb("sumCommit",true);
		for (int i = 0; i < 2; i++) {
			bool commit = (i == 0) == lead;
			if (!(commit ? co : ph)) continue;
			hdrGraph(commit ? "##hgc" : "##hgp",commit ? "Commit" : "Physical",commit ? "Commit charge" : "Physical memory",commit,x,sy,w,*m,*h);
			x += w + gap;
		}
	}

	void shutdown() {
		os::shutdown();
	}

}
