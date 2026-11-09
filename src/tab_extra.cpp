#include "wswatch.h"
#include "gpumem.h"
#include "dotnetmem.h"
#include "ui.h"
#include "cfg.h"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <map>
#include <set>

namespace {

	using Key = std::pair<uint32_t,uint64_t>;

	Key keyOf(const ProcRef& r) {
		return Key(r.pid,r.start);
	}

	// rows of a list : click, ctrl click, right click. Rows are known by a key so a re-sort keeps them.
	struct Sel {
		std::set<uint64_t> on;
		int col = 0;
		bool menu = false;

		// the label can be a module name, so it never goes through a label's ## and stays plain text
		void row(uint64_t k,const char* label,const char* tipTxt = nullptr) {
			bool sel = on.count(k) > 0;
			if (ImGui::Selectable("##r",sel,ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
				bool ctrl = ImGui::GetIO().KeyCtrl;
				if (!ctrl) on.clear();
				if (ctrl && sel) on.erase(k);
				else on.insert(k);
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
				if (!sel) {
					on.clear();
					on.insert(k);
				}
				col = std::max(0,ImGui::TableGetHoveredColumn());
				menu = true;
			}
			if (tipTxt) ui::tip(tipTxt);
			ImGui::SameLine(0.0f,0.0f);
			ImGui::TextUnformatted(label);
		}
	};

	bool ctrlC() {
		return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C);
	}

	// ui::empty that wraps, a long reason must not run off a narrow window
	void emptyWrap(const std::string& txt) {
		ImVec2 av = ImGui::GetContentRegionAvail();
		float w = std::min(av.x,ui::px(520.0f));
		ImVec2 ts = ImGui::CalcTextSize(txt.c_str(),nullptr,false,w);
		ImVec2 p = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(p.x + std::max(0.0f,(av.x - ts.x) * 0.5f),p.y + std::max(0.0f,(av.y - ts.y) * 0.5f)));
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
		ImGui::TextDisabled("%s",txt.c_str());
		ImGui::PopTextWrapPos();
	}

	void statusLine(ImVec4 col,const char* txt) {
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(col,"%s",txt);
	}

	// ---------- WS Watch ----------

	struct WsTab {
		Sel sel;
		std::shared_ptr<task::T> job;
		std::shared_ptr<const wswatch::View> built;
		std::vector<int> order;
		int sortCol = 2;
		bool desc = true;
	};

	std::map<Key,WsTab> g_ws;

	const char* const kWsCols[3] = {"Instruction","Filename","Count"};

	std::string wsCell(const wswatch::Row& r,int c) {
		return c == 0 ? r.instr : c == 1 ? r.file : std::to_string(r.count);
	}

	void wsCopy(const WsTab& t,const wswatch::View& v,int only) {
		std::string s;
		for (int i : t.order) {
			const auto& r = v.rows[i];
			if (!t.sel.on.count(r.pc)) continue;
			if (only >= 0) s += wsCell(r,only);
			else s += r.instr + "\t" + r.file + "\t" + std::to_string(r.count);
			s += "\n";
		}
		if (!s.empty()) ImGui::SetClipboardText(s.c_str());
	}

	void wsSort(WsTab& t,const wswatch::View& v) {
		t.order.resize(v.rows.size());
		for (size_t i = 0; i < t.order.size(); i++) t.order[i] = (int)i;
		int c = t.sortCol;
		bool d = t.desc;
		std::stable_sort(t.order.begin(),t.order.end(),[&](int a,int b) {
			const auto& x = v.rows[a];
			const auto& y = v.rows[b];
			if (c == 2) return d ? x.count > y.count : x.count < y.count;
			const std::string& p = c == 0 ? x.instr : x.file;
			const std::string& q = c == 0 ? y.instr : y.file;
			return d ? q < p : p < q;
		});
	}

	void wsMenu(WsTab& t,const wswatch::View& v) {
		if (t.sel.menu) {
			ImGui::OpenPopup("##wsmenu");
			t.sel.menu = false;
		}
		if (!ImGui::BeginPopup("##wsmenu")) return;
		const wswatch::Row* one = nullptr;
		if (t.sel.on.size() == 1) for (auto& r : v.rows) if (t.sel.on.count(r.pc)) one = &r;
		if (ImGui::MenuItem("Open file location",nullptr,false,one && !one->file.empty())) {
			std::string f = one->file;
			task::run([f](task::T&) {
				wswatch::openLoc(f);
				return std::string();
			},false);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Copy","Ctrl+C")) wsCopy(t,v,-1);
		std::string cc = std::string("Copy \"") + kWsCols[t.sel.col % 3] + "\"";
		if (ImGui::MenuItem(cc.c_str())) wsCopy(t,v,t.sel.col % 3);
		ImGui::EndPopup();
	}

	void wsTable(WsTab& t,const std::shared_ptr<const wswatch::View>& vp) {
		const wswatch::View& v = *vp;
		if (!ui::beginTable("##wslist",3,ImVec2(0,0))) return;
		ImGui::TableSetupColumn(kWsCols[0],ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide,3.0f);
		ImGui::TableSetupColumn(kWsCols[1],ImGuiTableColumnFlags_WidthStretch,4.0f);
		ImGui::TableSetupColumn(kWsCols[2],ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending,ui::px(90.0f));
		ImGui::TableSetupScrollFreeze(0,1);
		bool resort = t.built != vp;
		if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
			if (sp->SpecsDirty && sp->SpecsCount > 0) {
				t.sortCol = sp->Specs[0].ColumnIndex;
				t.desc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
				resort = true;
			}
			sp->SpecsDirty = false;
		}
		if (resort) {
			wsSort(t,v);
			t.built = vp;
		}
		ImGui::TableHeadersRow();
		ImGuiListClipper clip;
		clip.Begin((int)t.order.size());
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
				const auto& r = v.rows[t.order[i]];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushID(i);
				t.sel.row(r.pc,r.instr.c_str());
				ImGui::PopID();
				if (ImGui::TableNextColumn()) {
					ImGui::TextUnformatted(r.file.c_str());
					if (!r.file.empty()) ui::tip(r.file.c_str());
				}
				if (ImGui::TableNextColumn()) ui::cellRight(util::count(r.count).c_str());
			}
		}
		ui::endTable();
		if (ctrlC()) wsCopy(t,v,-1);
		wsMenu(t,v);
	}

	void tabWs(const ProcRef& r) {
		if (g_ws.size() > 64) g_ws.clear();
		WsTab& t = g_ws[keyOf(r)];
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		bool blocked = row && (row->pseudo || row->sys);
		if (!blocked) wswatch::want(r);
		auto v = blocked ? nullptr : wswatch::view(r);
		bool perm = wswatch::permanent();
		ui::hint(perm ?
			"Working set watch shows which instructions fault pages into this process. You have to turn it on first. Once on it can't be turned off, it stays until the process exits. Closing this window does not stop it. Reading drains the kernel's list, so another tool reading the same process splits the counts. One entry can stand for several pages the kernel faulted in together, so the count reads below the process's page fault counter." :
			"Working set watch shows which instructions fault pages into this process, through a perf event per thread. You have to turn it on first. It stops with the Stop button, when FRS exits or when the process exits, and closing this window does not stop it.");
		ImGui::Spacing();
		std::string ask = "Enable WS Watch##" + std::to_string(r.pid);
		bool on = v && v->on;
		ImGui::BeginDisabled(on || t.job || blocked || !procs::alive(r));
		if (ImGui::Button("Enable")) ui::askOpen(ask.c_str());
		ImGui::EndDisabled();
		std::string q = "Turn on WS Watch for " + util::safe(r.name) + " (" + std::to_string(r.pid) + ")?\n\n";
		q += perm ?
			"This can't be undone. Windows keeps recording page faults in this process until it exits, there is no off switch, and reading the results drains its list." :
			"This opens a perf event on each thread. It stops with the Stop button or when FRS exits.";
		if (ui::askDraw(ask.c_str(),q.c_str(),"Enable",perm)) {
			ProcRef rr = r;
			t.job = task::run([rr](task::T&) {
				std::string why = wswatch::enable(rr,"");
				return why.empty() ? std::string("WS Watch is on for pid ") + std::to_string(rr.pid) : "WS Watch : failed (" + why + ")";
			},false);
		}
		if (t.job && t.job->done) {
			ui::toast(t.job->res);
			t.job.reset();
		}
		if (!perm && on) {
			ImGui::SameLine();
			ImGui::BeginDisabled(t.job != nullptr);
			if (ImGui::Button("Stop")) {
				ProcRef rr = r;
				t.job = task::run([rr](task::T&) {
					std::string why = wswatch::stop(rr,"");
					return why.empty() ? std::string("WS Watch is off for pid ") + std::to_string(rr.pid) : "WS Watch : failed (" + why + ")";
				},false);
			}
			ImGui::EndDisabled();
		}
		bool gone = !procs::alive(r);
		if (t.job) statusLine(ui::c.dim,"Working...");
		else if (gone) statusLine(ui::c.dim,on ? "WS Watch was on until the process exited." : "The process exited.");
		else if (blocked) statusLine(ui::c.dim,"Not for system and pseudo processes.");
		else if (!v) statusLine(ui::c.dim,"Checking...");
		else if (!v->why.empty()) statusLine(on && !perm ? ui::c.warn : ui::c.bad,on && !perm ? "WS Watch is on, but not for every thread." : "Can't read it.");
		else if (on) statusLine(ui::c.good,"WS Watch is enabled.");
		else statusLine(ui::c.dim,"WS Watch is off for this process.");
		if (v && !v->why.empty() && !gone) ui::hint(util::safe(v->why).c_str());
		ImGui::TextDisabled("Page faults :");
		ImGui::SameLine();
		if (v && v->probed && (v->on || v->total)) ImGui::Text("%s since FRS started reading, at %s instructions",util::count(v->total).c_str(),util::count(v->rows.size()).c_str());
		else ImGui::TextDisabled("N/A");
		if (v && v->lost) {
			ImGui::SameLine();
			ImGui::TextColored(ui::c.warn,"(%s lost)",util::count(v->lost).c_str());
			ui::tip("The kernel keeps about 1,000 faults between two reads and only counts the rest, so these never got an instruction.");
		}
		if (!v || (v->rows.empty())) {
			ui::empty(blocked ? "Nothing to show." : !v ? (gone ? "The process exited before this was read." : "Reading...") : gone || (!v->why.empty() && (perm || !on)) ? "Nothing to show." : on ? "No page faults since it was turned on, faults show up here as they happen." : "Nothing is being tracked. Enable starts it.");
			return;
		}
		wsTable(t,v);
	}

	// ---------- GPU ----------

	std::string rangeTxt(uint64_t v) {
		return v ? util::size(v) : "-";
	}

	void statRow(const char* name,const char* tip,uint64_t cur,const GpuRange* g) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(name);
		ui::tip(tip);
		ImGui::TableNextColumn();
		ui::cellSize(cur);
		ImGui::TableNextColumn();
		ui::cellRight(g ? rangeTxt(g->lo).c_str() : "-");
		ImGui::TableNextColumn();
		ui::cellRight(g ? rangeTxt(g->hi).c_str() : "-");
		ImGui::TableNextColumn();
		ui::cellRight(g && g->hi ? util::size(g->hi - g->lo).c_str() : "-");
	}

	void gpuGraph(const char* id,const char* cap,const std::vector<float>& v,const std::vector<uint64_t>& ts,uint64_t cur,ImVec4 col,float w) {
		ui::beginCard(id,nullptr,ImVec2(w / ui::px(1.0f),0.0f));
		ImGui::TextUnformatted(cap);
		ImGui::SameLine();
		std::string cs = util::size(cur);
		ui::cellRight(cs.c_str());
		std::string gid = std::string(id) + "g";
		std::string name = cap;
		float mx = 16.0f;
		for (float x : v) if (x > mx) mx = x;
		ui::Series s{v.empty() ? nullptr : v.data(),(int)v.size(),col,cap};
		ui::graph(gid.c_str(),&s,1,mx * 1.1f,ImVec2(0.0f,84.0f),[&v,&ts,name](int i) {
			if (i >= (int)v.size()) return std::string();
			std::string t = name + " : " + (v[i] < 0.0f ? std::string("N/A") : util::size((uint64_t)((double)v[i] * 1048576.0)));
			if (i < (int)ts.size() && ts[i]) t += "\n" + util::timeStr(ts[i]);
			return t;
		});
		ui::endCard();
	}

	void tabGpu(const ProcRef& r) {
		gpumem::want(r);
		auto s = gpumem::latest();
		if (!s->t) {
			ui::empty("Reading GPU memory...");
			return;
		}
		if (!s->on || s->adapters.empty()) {
			ui::empty(s->why.empty() ? "No GPU data." : util::safe(s->why).c_str());
			return;
		}
		auto h = gpumem::procHist(r);
		auto d = gpumem::detail(r);
		const GpuProc* p = s->find(r);
		bool gone = !procs::alive(r);
		GpuProc cur = gone ? GpuProc() : s->now(r);
		bool unread = !gone && cur.ded == NA;
		const char* unreadTxt = "FRS can't read this process's GPU memory, this needs admin or root.";
		if (!p && !h) {
			ui::empty(gone ? "The process exited, FRS has no GPU history left for it." : unread ? unreadTxt : "This process holds no GPU memory and has not since FRS started.");
			return;
		}
		ImGui::BeginChild("##gpubody");
		ui::hint("Shared GPU memory is system RAM and already part of used RAM, don't add it on top. Dedicated is VRAM on a discrete card, not RAM.");
		if (unread) ImGui::TextDisabled("%s",unreadTxt);
		else if (!p) ImGui::TextDisabled("%s",gone ? "The process exited, min and max are what FRS kept." : "This process holds no GPU memory right now.");
		ImGui::Spacing();
		uint64_t ded = cur.ded;
		uint64_t shr = cur.shared;
		uint64_t com = cur.commit;
		float rowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
		if (ui::beginTable("##gpustat",5,ImVec2(0.0f,(rowH * 7.0f + ui::px(3.0f)) / ui::px(1.0f)))) {
			ImGui::TableSetupColumn("Memory",ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoHide,3.0f);
			ImGui::TableSetupColumn("Current",ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
			ImGui::TableSetupColumn("Min",ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
			ImGui::TableSetupColumn("Max",ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
			ImGui::TableSetupColumn("Diff",ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
			ImGui::TableHeadersRow();
			const GpuRange* none = nullptr;
			statRow("Dedicated (resident)","Windows counter Local Usage, VRAM in use now. Not RAM on a discrete card. Zero samples don't count for min and max. On Linux drm-resident-vram.",ded,h ? &h->rDed : none);
			statRow("Shared (resident)","Windows counter Non Local Usage. This is system RAM, already part of used RAM. On Linux drm-resident-gtt.",shr,h ? &h->rShared : none);
			std::string kmt = gpumem::kmtNote(*s);
			statRow("Dedicated (committed)",("Windows counter Dedicated Usage, the number Task Manager calls dedicated GPU memory. On Linux drm-total-vram." + kmt).c_str(),cur.dedCommit,h ? &h->rDedC : none);
			statRow("Shared (committed)",("Windows counter Shared Usage, the number Task Manager calls shared GPU memory. On Linux drm-total-gtt." + kmt).c_str(),cur.sharedCommit,h ? &h->rSharedC : none);
			statRow(s->commitName,s->hasCommit ? "Windows counter Total Committed, or the system memory the driver allocated when reading through D3DKMT. Not the sum of the rows above, Windows counts it on its own, so it can read below Dedicated (committed)." : "Linux has no such number.",com,h ? &h->rCommit : none);
			statRow("Total (resident)","Dedicated plus shared, resident. Unlike System Informer it leaves committed out, adding those would count the same bytes twice.",ded == NA ? NA : ded + shr,h ? &h->rTotal : none);
			ui::endTable();
		}
		ImGui::Spacing();
		static const std::vector<float> noV;
		static const std::vector<uint64_t> noT;
		const auto& ts = h ? h->unixSec : noT;
		float sp = ImGui::GetStyle().ItemSpacing.x;
		int n = s->hasCommit ? 3 : 2;
		float w = (ImGui::GetContentRegionAvail().x - sp * (float)(n - 1)) / (float)n;
		gpuGraph("##gd","Dedicated",h ? h->ded : noV,ts,ded,ui::c.purple,w);
		ImGui::SameLine();
		gpuGraph("##gs","Shared",h ? h->shared : noV,ts,shr,ui::c.phys,w);
		if (s->hasCommit) {
			ImGui::SameLine();
			gpuGraph("##gc",s->commitName,h ? h->commit : noV,ts,com,ui::c.commit,w);
		}
		ImGui::Spacing();
		w = (ImGui::GetContentRegionAvail().x - sp) * 0.5f;
		ui::beginCard("##gdet",nullptr,ImVec2(w / ui::px(1.0f),0.0f));
		ImGui::TextUnformatted("System memory the GPU driver holds for it");
		ImGui::SameLine();
		ui::help("Read per process, all of it is RAM and already in used RAM.");
		const char* noDet = gone ? "The process exited before this was read." : cfg::getb("memPaused",false) ? "Sampling is paused in Settings." : "Reading...";
		if (!s->hasCommit) ImGui::TextDisabled("Not available on Linux, the kernel has no such numbers.");
		else if (!d) ImGui::TextDisabled("%s",noDet);
		else if (!d->why.empty()) ImGui::TextDisabled("%s",util::safe(d->why).c_str());
		else {
			ui::kvSize("Allocated",d->alloc);
			ui::kvSize("Reserved",d->reserved);
			ui::kvSize("Write combined allocated",d->wcAlloc);
			ui::kvSize("Write combined reserved",d->wcReserved);
			ui::kvSize("Cached allocated",d->cachedAlloc);
			ui::kvSize("Cached reserved",d->cachedReserved);
			ui::kvSize("Section allocated",d->sectionAlloc);
			ui::kvSize("Section reserved",d->sectionReserved);
		}
		ui::endCard();
		ImGui::SameLine();
		ui::beginCard("##gadp",nullptr,ImVec2(w / ui::px(1.0f),0.0f));
		ImGui::TextUnformatted("Adapter statistics");
		if (!cfg::getb("gpuAdapterStats",false)) ui::hint("Off. Settings, GPU, Per process adapter statistics turns these on.");
		else if (!d) ImGui::TextDisabled("%s",noDet);
		else if (!d->why.empty()) ImGui::TextDisabled("%s",util::safe(d->why).c_str());
		else {
			if (s->hasCommit) {
				ui::kvSize("GPU virtual memory",d->virt);
				ui::tip("Virtual address space the video memory manager uses for this process.");
			}
			ui::kvSize("Evicted bytes",d->evicted);
			ui::tip("Bytes pushed out of its video memory, a sign of VRAM pressure.");
		}
		ui::endCard();
		ImGui::EndChild();
	}

	// ---------- .NET ----------

	struct NetRow {
		const char* group;
		const char* label;
		std::string val;
		const char* tip;
	};

	struct NetTab {
		Sel sel;
		std::vector<NetRow> rows;
	};

	std::map<Key,NetTab> g_net;

	std::string bps(uint64_t v) {
		return v == NA ? "N/A" : util::size(v) + " /s";
	}

	void netRows(const ClrMem& m,std::vector<NetRow>& o) {
		o.clear();
		auto sz = [&o](const char* g,const char* l,uint64_t v,const char* t = nullptr) { o.push_back({g,l,util::size(v),t}); };
		auto ct = [&o](const char* g,const char* l,uint64_t v,const char* t = nullptr) { o.push_back({g,l,util::count(v),t}); };
		auto rt = [&o](const char* g,const char* l,uint64_t v,const char* t = nullptr) { o.push_back({g,l,bps(v),t}); };
		const char* loose = "Change of a value the GC sets at the end of a collection, so it shows jumps, not a steady rate.";
		const char* g = "Heap";
		sz(g,"Gen 0 budget (not usage)",m.genSize[0],"Allocation budget that triggers the next GC, not bytes in Gen 0. It can be far above the total commit, so it is left out of every total.");
		sz(g,"Gen 1 Heap Size",m.genSize[1],"Bytes in Gen 1 at the last GC.");
		sz(g,"Gen 2 Heap Size",m.genSize[2],"Bytes in Gen 2 at the last GC.");
		sz(g,"Large Object Heap Size",m.loh,"Bytes in the large object heap at the last GC.");
		sz(g,"# Bytes in all Heaps",m.allHeaps,"Gen 1 + Gen 2 + large object heap. Gen 0 is left out on purpose, same as the Windows counter.");
		sz(g,"# Total Committed Bytes",m.committed,"Virtual memory the GC has committed. Compare with private bytes to split managed from native.");
		sz(g,"# Total Reserved Bytes",m.reserved,"Virtual memory the GC has reserved.");
		sz(g,"Bytes in Loader Heap",m.loader,"Memory committed by the class loader.");
		g = "Collections";
		ct(g,"# Gen 0 Collections",m.genCount[0],"Includes the higher generation GCs.");
		ct(g,"# Gen 1 Collections",m.genCount[1]);
		ct(g,"# Gen 2 Collections",m.genCount[2],"Full GCs.");
		ct(g,"# Induced GC",m.induced,"Explicit GC.Collect calls.");
		g = "Promotion";
		sz(g,"Promoted Memory from Gen 0",m.promoted[0],"Bytes that survived Gen 0 at the last GC, not cumulative.");
		sz(g,"Promoted Memory from Gen 1",m.promoted[1],"Bytes that survived Gen 1 at the last GC. Back to 0 after a Gen 0 only GC.");
		sz(g,"Promoted Finalization-Memory from Gen 0",m.promotedFinal,"Bytes kept alive only for finalization.");
		rt(g,"Gen 0 Promoted Bytes / sec",m.rGen0,loose);
		rt(g,"Gen 1 Promoted Bytes / sec",m.rGen1,loose);
		rt(g,"Promoted Finalization-Memory / sec",m.rFinal,loose);
		g = "Allocation";
		sz(g,"Total Bytes Allocated (since start)",m.alloc);
		sz(g,"Total Bytes Allocated for Large Objects (since start)",m.allocLoh);
		rt(g,"Allocated Bytes / sec",m.rAlloc,"The CLR updates this at each GC, so expect steps.");
		g = "Objects";
		ct(g,"Finalization Survivors",m.survivors,"Objects kept alive for their finalizers.");
		ct(g,"# GC Handles",m.handles);
		ct(g,"# of Pinned Objects",m.pinned,"Seen in the last GC.");
		ct(g,"# of Sink Blocks in use",m.sink);
	}

	void netCopy(const NetTab& t,int only) {
		std::string s;
		for (size_t i = 0; i < t.rows.size(); i++) {
			if (!t.sel.on.count(i)) continue;
			if (only == 0) s += t.rows[i].label;
			else if (only == 1) s += t.rows[i].val;
			else s += std::string(t.rows[i].label) + "\t" + t.rows[i].val;
			s += "\n";
		}
		if (!s.empty()) ImGui::SetClipboardText(s.c_str());
	}

	void netStat(const char* id,const char* label,uint64_t v,float w,const char* tip) {
		ui::beginCard(id,nullptr,ImVec2(w / ui::px(1.0f),0.0f));
		ImGui::TextDisabled("%s",label);
		ui::tip(tip);
		ui::pushBig();
		ImGui::TextUnformatted(util::size(v).c_str());
		ui::popFont();
		ui::endCard();
	}

	void tabNet(const ProcRef& r) {
		if (g_net.size() > 64) g_net.clear();
		NetTab& t = g_net[keyOf(r)];
		dotnetmem::want(r);
		auto m = dotnetmem::view(r);
		if (!m) {
			ui::empty(procs::alive(r) ? "Reading the CLR block..." : "The process exited before this was read.");
			return;
		}
		if (!m->block) {
			emptyWrap(m->kind == ClrMem::None ? "Not a .NET Framework process, it has no CLR v2 or v4 block." : m->why.empty() ? "No numbers." : util::safe(m->why));
			return;
		}
		const char* kn = dotnetmem::kindName(m->kind);
		std::string badge = std::string("CLR ") + (kn ? kn : "") + (m->wow64 ? ", 32 bit" : ", 64 bit");
		ui::badge(badge.c_str(),ui::c.accent);
		ImGui::SameLine();
		ImGui::TextDisabled("Read from the CLR's public memory block, values change when a GC ends.");
		float sp = ImGui::GetStyle().ItemSpacing.x;
		float w = (ImGui::GetContentRegionAvail().x - sp * 3.0f) * 0.25f;
		netStat("##nh","Bytes in all heaps",m->allHeaps,w,"Gen 1 + Gen 2 + large object heap, the best single managed heap number.");
		ImGui::SameLine();
		netStat("##nc","Committed",m->committed,w,"Virtual memory the GC has committed.");
		ImGui::SameLine();
		netStat("##nr","Reserved",m->reserved,w,"Virtual memory the GC has reserved.");
		ImGui::SameLine();
		netStat("##nb","Gen 0 budget (not usage)",m->genSize[0],w,"Allocation budget that triggers the next GC, not bytes in Gen 0. Never add it to a total.");
		ui::Seg seg[3] = {
			{(float)m->genSize[1],ui::c.commit,"Gen 1"},
			{(float)m->genSize[2],ui::c.phys,"Gen 2"},
			{(float)m->loh,ui::c.purple,"Large object heap"}
		};
		ui::stackBar("##nbar",seg,3,ImVec2(0.0f,12.0f));
		ui::tip("Managed heap at the last GC : Gen 1, Gen 2 and large object heap. Gen 0 is a budget and is not in here.");
		ImGui::Spacing();
		netRows(*m,t.rows);
		if (ui::beginTable("##nettbl",2,ImVec2(0,0))) {
			ImGui::TableSetupColumn("Counter",ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoHide,3.0f);
			ImGui::TableSetupColumn("Value",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(140.0f));
			ImGui::TableSetupScrollFreeze(0,1);
			ImGui::TableHeadersRow();
			const char* last = nullptr;
			for (size_t i = 0; i < t.rows.size(); i++) {
				const NetRow& n = t.rows[i];
				if (n.group != last) {
					last = n.group;
					ImGui::TableNextRow();
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,ImGui::GetColorU32(ui::c.panel2));
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s",n.group);
				}
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushID((int)i);
				t.sel.row(i,n.label,n.tip);
				ImGui::PopID();
				ImGui::TableNextColumn();
				ui::cellRight(n.val.c_str());
			}
			ui::endTable();
			if (ctrlC()) netCopy(t,-1);
			if (t.sel.menu) {
				ImGui::OpenPopup("##netmenu");
				t.sel.menu = false;
			}
			if (ImGui::BeginPopup("##netmenu")) {
				if (ImGui::MenuItem("Copy","Ctrl+C")) netCopy(t,-1);
				int c = t.sel.col % 2;
				if (ImGui::MenuItem(c ? "Copy \"Value\"" : "Copy \"Counter\"")) netCopy(t,c);
				ImGui::EndPopup();
			}
		}
	}

}

namespace extra {

	void initTabs() {
		procwin::addTab("WS Watch",80,tabWs);
		procwin::addTab("GPU",90,tabGpu);
		procwin::addTab(".NET",100,tabNet);
	}

}
