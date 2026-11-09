#include "sysmem.h"
#include "smbios.h"
#include "procs.h"
#include "ui.h"
#include "cfg.h"
#include "ram.h"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define OS_TIP(win,lx) win
#else
#define OS_TIP(win,lx) lx
#endif

namespace {

	std::shared_ptr<sysmem::Run> g_dlg;
	bool g_openDlg = false;

	const char* kAskAll = "Empty all";

	std::string sz(uint64_t v) {
		return util::size(v);
	}

	std::string perSec(uint64_t v) {
		return v == NA ? "N/A" : util::count(v) + " /s";
	}

	std::string bytesSec(uint64_t v) {
		return v == NA ? "N/A" : util::size(v) + " /s";
	}

	std::string change(bool known,int64_t v) {
		if (!known) return "N/A";
		return v ? util::delta(v) + " /s" : "0 B /s";
	}

	std::string num(uint32_t v) {
		return v ? util::count(v) : "N/A";
	}

	uint64_t sum2(uint64_t a,uint64_t b) {
		return a == NA ? b : b == NA ? a : a + b;
	}

	// one group, so a ui::tip after it works on the whole row
	void kvRow(const std::string& k,const std::string& v) {
		ImGui::BeginGroup();
		ImGui::TextDisabled("%s",k.c_str());
		ImGui::SameLine();
		float av = ImGui::GetContentRegionAvail().x;
		float w = ImGui::CalcTextSize(v.c_str()).x;
		if (av > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + av - w);
		if (v == "N/A") ImGui::TextDisabled("N/A");
		else ImGui::TextUnformatted(v.c_str());
		ImGui::EndGroup();
	}

	void statCard(const char* id,const char* label,const std::string& val,const std::string& sub,ImVec4 col,float w) {
		ui::beginCard(id,nullptr,ImVec2(w / ui::px(1.0f),0.0f));
		ImGui::TextDisabled("%s",label);
		ui::pushBig();
		if (val == "N/A") ImGui::TextDisabled("N/A");
		else ImGui::TextColored(col,"%s",val.c_str());
		ui::popFont();
		// short of room "75 % of 31 GB" keeps the percent
		size_t of = sub.find(" of ");
		bool cut = of != std::string::npos && ImGui::CalcTextSize(sub.c_str()).x > ImGui::GetContentRegionAvail().x;
		ImGui::TextDisabled("%s",cut ? sub.substr(0,of).c_str() : sub.c_str());
		ui::endCard();
	}

	void drawCards(const SysMem& m) {
		std::string need = m.listsWhy.empty() ? "unknown" : "unavailable";
		std::string freeTxt = sz(sum2(m.free,m.zeroed));
		struct C { const char* id; const char* label; std::string val,sub; ImVec4 col; };
		C cs[] = {
			{"##cphys","Physical",sz(m.physUsed),m.physUsed != NA ? util::pct(m.physUsed,m.physTotal) + " of " + sz(m.physTotal) : "unknown",ui::c.phys},
			{"##ccommit","Commit",sz(m.commit),m.commit != NA ? util::pct(m.commit,m.commitLimit) + " of " + sz(m.commitLimit) : "unknown",ui::c.commit},
			{"##cstandby","Standby",sz(m.standby),m.standby != NA ? "cached, reusable" : need,ui::c.standby},
			{"##cmod","Modified",sz(m.modified),m.modified != NA ? "dirty, to be written" : need,ui::c.modified},
			{"##cfree",m.zeroed != NA ? "Free + zeroed" : "Free list",freeTxt,freeTxt == "N/A" ? need : "cache not counted",ui::c.freeMem}
		};
		int n = 5;
		float sp = ImGui::GetStyle().ItemSpacing.x;
		float av = ImGui::GetContentRegionAvail().x;
		int cols = std::max(2,std::min(n,(int)(av / ui::px(120.0f))));
		float w = (av - sp * (float)(cols - 1)) / (float)cols;
		for (int i = 0; i < n; i++) {
			if (i % cols) ImGui::SameLine();
			statCard(cs[i].id,cs[i].label,cs[i].val,cs[i].sub,cs[i].col,w);
		}
	}

	void memGraph(const char* id,const char* cap,const char* helpTxt,const std::vector<float>& v,ImVec4 col,float maxMb,float h,const std::string& over,const std::function<std::string(int)>& tip) {
		ui::graphHead(cap,helpTxt,over.c_str());
		ui::Series s{v.data(),(int)v.size(),col,cap};
		ui::graph(id,&s,1,maxMb,ImVec2(0.0f,h / ui::px(1.0f)),tip,std::max(60,std::min(3600,cfg::geti("memHistN",600))));
	}

	// ratio under 1 : the store costs more than it holds, nothing is saved
	uint64_t savedBytes(uint64_t ws,double ratio) {
		return (uint64_t)std::max(0.0,(double)ws * ratio - (double)ws);
	}

	std::string timeLine(const SysHist& h,int i) {
		return i < (int)h.unixSec.size() && h.unixSec[i] ? util::timeStr(h.unixSec[i]) : "Unknown time";
	}

	std::string topLine(const std::vector<std::string>& names,const std::vector<uint64_t>& vals,int i,const char* what) {
		if (i >= (int)names.size() || names[i].empty()) return "";
		return std::string("\nTop process : ") + names[i] + " (" + what + " " + util::size(vals[i]) + ")";
	}

	std::string overlay(uint64_t cur,uint64_t lim) {
		if (cur == NA) return "N/A";
		return sz(cur) + " / " + sz(lim) + "   " + util::pct(cur,lim);
	}

	void drawGraphs(const SysMem& m,const std::shared_ptr<const SysHist>& h,float avail) {
		float each = (avail - ImGui::GetTextLineHeightWithSpacing() * 2.0f - ImGui::GetStyle().ItemSpacing.y * 2.0f) * 0.5f;
		each = std::max(ui::px(50.0f),std::min(ui::px(240.0f),each));
		float mxC = m.commitLimit != NA ? (float)((double)m.commitLimit / 1048576.0) : 0.0f;
		float mxP = m.physTotal != NA ? (float)((double)m.physTotal / 1048576.0) : 0.0f;
		memGraph("##gcommit","Commit charge",OS_TIP("Memory the system promised to back with RAM or the pagefile. The limit is RAM plus pagefile.","Memory the system promised to back with RAM or swap. The limit is swap plus vm.overcommit_ratio percent of RAM, only enforced with vm.overcommit_memory=2."),
			h->commit,ui::c.commit,mxC,each,overlay(m.commit,m.commitLimit),
			[h](int i) {
				std::string t = "Commit charge : " + (h->commit[i] < 0.0f ? std::string("N/A") : util::size((uint64_t)((double)h->commit[i] * 1048576.0)));
				return t + "\n" + timeLine(*h,i) + topLine(h->topPriv,h->topPrivBytes,i,"private");
			});
		bool comp = m.compWs != NA && m.compData != NA && m.compSize != NA && m.compData && m.compSize;
		double ratio = comp ? (double)m.compData / (double)m.compSize : 0.0;
		uint64_t cWs = m.compWs;
		memGraph("##gphys","Physical memory","Used means not available, and available includes standby. So a standby purge barely moves this graph, the bar below shows it.",
			h->phys,ui::c.phys,mxP,each,overlay(m.physUsed,m.physTotal),
			[h,comp,ratio,cWs](int i) {
				std::string t = "Physical memory : " + (h->phys[i] < 0.0f ? std::string("N/A") : util::size((uint64_t)((double)h->phys[i] * 1048576.0)));
				if (comp) {
					t += "\nCompressed memory, now : " + util::size(cWs);
					t += "\nHeld in RAM, now : " + util::size((uint64_t)((double)cWs * ratio));
					t += "\nTotal memory saved, now : " + util::size(savedBytes(cWs,ratio));
				}
				return t + "\n" + timeLine(*h,i) + topLine(h->topProc,h->topProcBytes,i,"working set");
			});
	}

	void legend(const std::string* names,const ImVec4* cols,int n) {
		float box = ImGui::GetTextLineHeight() * 0.7f;
		float gap = ui::px(14.0f);
		float x0 = ImGui::GetCursorPosX();
		float right = x0 + ImGui::GetContentRegionAvail().x;
		float x = x0;
		for (int i = 0; i < n; i++) {
			float w = box + ui::px(5.0f) + ImGui::CalcTextSize(names[i].c_str()).x;
			if (i && x + w <= right) ImGui::SameLine(x);
			else if (i) x = x0;
			ImVec2 p = ImGui::GetCursorScreenPos();
			float off = (ImGui::GetTextLineHeight() - box) * 0.5f;
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x,p.y + off),ImVec2(p.x + box,p.y + off + box),ImGui::GetColorU32(cols[i]),ui::px(2.0f));
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + box + ui::px(5.0f));
			ImGui::TextDisabled("%s",names[i].c_str());
			x += w + gap;
		}
	}

	const char* adminNote(const SysMem& m) {
		static bool el = isElevated();
		return m.peakSinceStart || el ? "" : " FRS is not running as admin, some Windows builds need it for these.";
	}

	void drawBar(const SysMem& m) {
		ImGui::TextUnformatted("Where the physical pages are");
		ImGui::SameLine();
		ui::help("In use is what no list holds. Physical on the cards and used in the header are total minus available, so they also count Modified and read a little higher. Free list is only the free list, the header's free means available, which also counts standby cache.");
		bool known = m.physTotal != NA && m.free != NA && m.modified != NA && m.standby != NA;
		if (!known) {
			if (m.physUsed != NA) {
				std::string nm[2] = {"In use  " + sz(m.physUsed),"Available  " + sz(m.physAvail)};
				ui::Seg seg[2] = {{(float)m.physUsed,ui::c.phys,nm[0].c_str()},{(float)m.physAvail,ui::c.freeMem,nm[1].c_str()}};
				ui::stackBar("##comp",seg,2,ImVec2(0.0f,26.0f));
				ImVec4 cols[2] = {ui::c.phys,ui::c.freeMem};
				legend(nm,cols,2);
			}
			std::string t = "Page lists are not available" + (m.listsWhy.empty() ? std::string(".") : " : " + m.listsWhy) + adminNote(m);
			ui::hint(t.c_str());
			return;
		}
		// Linux has no zeroed or no-write list, those parts stay out instead of showing as 0
		uint64_t onLists = 0;
		for (uint64_t v : {m.zeroed,m.free,m.modified,m.modifiedNoWrite,m.standby}) if (v != NA) onLists += v;
		uint64_t inUse = m.physTotal > onLists ? m.physTotal - onLists : 0;
		struct P { const char* name; uint64_t v; ImVec4 col; };
		const P ps[] = {
			{"In use",inUse,ui::c.phys},{"Modified",m.modified,ui::c.modified},{"Modified no-write",m.modifiedNoWrite,ui::c.gold},
			{"Standby",m.standby,ui::c.standby},{"Free list",m.free,ui::c.freeMem},{"Zeroed",m.zeroed,ui::c.zeroed}
		};
		std::string nm[6];
		ImVec4 cols[6];
		ui::Seg seg[6];
		int n = 0;
		for (auto& p : ps) {
			if (p.v == NA) continue;
			nm[n] = std::string(p.name) + "  " + sz(p.v);
			cols[n] = p.col;
			n++;
		}
		n = 0;
		for (auto& p : ps) {
			if (p.v == NA) continue;
			seg[n] = {(float)p.v,cols[n],nm[n].c_str()};
			n++;
		}
		ui::stackBar("##comp",seg,n,ImVec2(0.0f,26.0f));
		legend(nm,cols,n);
	}

	void drawOverview(const SysMem& m,const std::shared_ptr<const SysHist>& h) {
		drawCards(m);
		ImGui::Spacing();
		// measured last frame, the legend and the hints wrap with the window
		static float barH = 0.0f;
		drawGraphs(m,h,ImGui::GetContentRegionAvail().y - barH - ImGui::GetStyle().ItemSpacing.y);
		ImGui::Spacing();
		float y0 = ImGui::GetCursorPosY();
		drawBar(m);
		if (!m.perfWhy.empty()) ui::hint(("Commit and physical query failed : " + m.perfWhy).c_str());
		barH = ImGui::GetCursorPosY() - y0;
	}

	struct Row { std::string k,v,tip; };
	struct Panel { const char* title; std::vector<Row> rows; };

	void drawPanels(const std::vector<Panel>& ps) {
		float av = ImGui::GetContentRegionAvail().x;
		int cols = av >= ui::px(780.0f) ? 3 : av >= ui::px(460.0f) ? 2 : 1;
		ImGuiStyle& st = ImGui::GetStyle();
		float w = (av - st.ItemSpacing.x * (float)(cols - 1)) / (float)cols;
		float line = ImGui::GetTextLineHeightWithSpacing();
		for (size_t i = 0; i < ps.size(); i += cols) {
			size_t rows = 0;
			for (size_t k = i; k < i + cols && k < ps.size(); k++) rows = std::max(rows,ps[k].rows.size());
			float h = st.WindowPadding.y * 2.0f + ImGui::GetFontSize() * 1.08f + st.ItemSpacing.y + line * (float)rows + ui::px(2.0f);
			for (size_t k = i; k < i + cols && k < ps.size(); k++) {
				if (k > i) ImGui::SameLine();
				ImGui::PushID(ps[k].title);
				ui::beginCard("##p",ps[k].title,ImVec2(w / ui::px(1.0f),h / ui::px(1.0f)));
				for (auto& r : ps[k].rows) {
					kvRow(r.k,r.v);
					if (!r.tip.empty()) ui::tip(r.tip.c_str());
				}
				ui::endCard();
				ImGui::PopID();
			}
		}
	}

	void drawDetails(const SysMem& m) {
		bool lx = m.peakSinceStart;
		bool hasRatio = m.compData != NA && m.compSize != NA && m.compData && m.compSize;
		double ratio = hasRatio ? (double)m.compData / (double)m.compSize : 0.0;
		char rt[32] = "N/A";
		if (hasRatio) snprintf(rt,sizeof(rt),"%.2f : 1",ratio);
		std::string rsv = m.reserved == NA ? "N/A" : m.reserved ? sz(m.reserved) : "None";
		std::string pnaTip = lx ? "Linux has no paged pool. Reclaimable slab under More counters is the nearest thing." : "";
		const char* naTip = lx ? "This kernel does not report it." : "Windows does not keep this on this build, or the query failed.";
		std::vector<Panel> ps;
		ps.push_back({"Commit",{
			{"Current",sz(m.commit),""},
			{lx ? "Peak since FRS started" : "Peak",sz(m.commitPeak),lx ? "Linux keeps no kernel peak, FRS tracks it while it runs." : "Highest commit since boot."},
			{"Limit",sz(m.commitLimit),lx ? "Only enforced with vm.overcommit_memory=2, so the percent can pass 100." : "RAM plus pagefile."}
		}});
		ps.push_back({"Physical",{
			{"Current",sz(m.physUsed),"Total minus available."},
			{"Total",sz(m.physTotal),"RAM the OS can use."},
			{"Available",sz(m.physAvail),"Includes standby."},
			{"Reserved",rsv,"Installed minus total, firmware and hardware keep it."},
			{"Cache WS",sz(m.cacheWs),lx ? "Cached plus Buffers." : "System cache working set. Not the standby cache."},
			{"Kernel WS",sz(m.kernelWs),lx ? "KernelStack, the nearest Linux number." : "Resident kernel code."},
			{"Driver WS",sz(m.driverWs),lx ? "Loaded size of the modules in /proc/modules, not resident." : "Resident driver code."}
		}});
		ps.push_back({"Paged pool",{
			{"Working set",sz(m.pagedWs),pnaTip},
			{"Virtual size",sz(m.pagedVs),pnaTip},
			{"Limit","N/A",lx ? pnaTip : "A kernel global, only System Informer's driver can read it. FRS has no driver."},
			{"Allocs",perSec(m.dPagedAllocs),lx ? pnaTip : m.cPagedAllocs == NA ? naTip : ""},
			{"Frees",perSec(m.dPagedFrees),lx ? pnaTip : m.cPagedFrees == NA ? naTip : ""},
			{"Size change",change(m.hasPagedDelta,m.dPagedBytes),"System Informer labels this one Allocs delta."}
		}});
		ps.push_back({"Non-paged pool",{
			{"Usage",sz(m.nonPaged),lx ? "SUnreclaim, the nearest Linux number." : ""},
			{"Limit",sz(m.nonPagedLimit),lx ? "" : "Documented limit, RAM or 16 TB. Not read from the kernel."},
			{"Allocs",perSec(m.dNonPagedAllocs),m.cNonPagedAllocs == NA ? naTip : ""},
			{"Frees",perSec(m.dNonPagedFrees),m.cNonPagedFrees == NA ? naTip : ""},
			{"Size change",change(m.hasNonPagedDelta,m.dNonPagedBytes),"System Informer labels this one Allocs delta."}
		}});
		ps.push_back({"Paging",{
			{"Page faults",perSec(m.dFaults),"Spikes right after a clean, that is the cost of cleaning."},
			{lx ? "Major faults" : "Page reads",perSec(m.dPageReads),lx ? "pgmajfault, events and not pages." : "Pages read from disk."},
			{lx ? "Swap writes" : "Pagefile writes",perSec(m.dPfWrites),lx ? "pswpout." : "Dirty pages written."},
			{"Mapped writes",perSec(m.dMappedWrites),lx ? "nr_written, all dirty writeback and not only mapped." : "Mapped file pages written."}
		}});
		ps.push_back({"Mapped IO",{
			{"Reads",bytesSec(m.dMappedReadBytes),lx ? "pgpgin." : "Read through paging."},
			{"Writes",bytesSec(m.dMappedWriteBytes),lx ? "pgpgout." : "Mapped, dirty and lazy writers."}
		}});
		ps.push_back({"File cache",{
			{"Current",sz(m.fileCache),m.cacheWhy},
			{"Peak",sz(m.fileCachePeak),m.cacheWhy},
			{"Minimum",sz(m.fileCacheMin),m.cacheWhy},
			{"Maximum",sz(m.fileCacheMax),m.cacheWhy}
		}});
		ps.push_back({"Compression",{
			{"Store in RAM",sz(m.compWs),m.compWhy},
			{"Held in RAM",hasRatio && m.compWs != NA ? sz((uint64_t)((double)m.compWs * ratio)) : "N/A",m.compWhy.empty() ? "Store in RAM times the ratio. Only the part in RAM, the Kernel page also counts what sits outside it." : m.compWhy},
			{"Memory saved",hasRatio && m.compWs != NA ? sz(savedBytes(m.compWs,ratio)) : "N/A",m.compWhy.empty() ? "Held in RAM minus Store in RAM. 0 when the store is bigger than what it holds." : m.compWhy},
			{"Ratio",rt,m.compWhy},
			{"Process ID",m.compPid ? std::to_string(m.compPid) : "N/A",m.compWhy}
		}});
		ps.push_back({"System",{
			{"Processes",num(m.processes),""},
			{"Threads",num(m.threads),""},
			{"Handles",num(m.handles),lx ? "Allocated file handles." : ""}
		}});
		drawPanels(ps);
		if (!m.perfWhy.empty()) ui::hint(("Query failed : " + m.perfWhy).c_str());
		if (ui::fold("More counters##mc",lx)) {
			std::vector<Panel> more;
			Panel a{"Sizes",{}};
			for (auto& e : m.extra) a.rows.push_back({e.first,sz(e.second),e.second == NA ? naTip : ""});
			Panel b{"Counters",{}};
			for (auto& e : m.counts) b.rows.push_back({e.first,util::count(e.second),e.second == NA ? naTip : ""});
			if (!a.rows.empty()) more.push_back(a);
			if (!b.rows.empty()) more.push_back(b);
			if (more.empty()) ui::hint("No extra counters on this OS.");
			else drawPanels(more);
		}
	}

	// plain header row, right aligned over the number columns. cols[0] stays left.
	void headRow(const char* const* names,int n,int tipCol = -1,const char* tipTxt = nullptr) {
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int i = 0; i < n; i++) if (ImGui::TableSetColumnIndex(i)) ui::headerCell(names[i],i == tipCol ? tipTxt : nullptr,i > 0);
	}

	// firmware text can be long : cut with ... at the column edge, full text on hover
	void cellText(const std::string& v) {
		if (v == "N/A") {
			ImGui::TextDisabled("N/A");
			return;
		}
		bool cut = ImGui::CalcTextSize(v.c_str()).x > ImGui::GetContentRegionAvail().x;
		ui::textEllipsis(v.c_str());
		if (cut) ui::tip(v.c_str());
	}

	void numCell(const std::string& v) {
		if (v == "N/A") ImGui::PushStyleColor(ImGuiCol_Text,ui::c.dim);
		ui::cellRight(v.c_str());
		if (v == "N/A") ImGui::PopStyleColor();
	}

	void listRow(const char* name,uint64_t v,uint64_t total,const char* tip,bool warn = false) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(name);
		if (tip) ui::tip(tip);
		ImGui::TableNextColumn();
		if (warn) ImGui::PushStyleColor(ImGuiCol_Text,ui::c.bad);
		numCell(sz(v));
		if (warn) ImGui::PopStyleColor();
		ImGui::TableNextColumn();
		numCell(util::pct(v,total));
	}

	void drawLists(const SysMem& m) {
		if (!m.listsWhy.empty()) {
			ui::hint(("Page lists are not available : " + m.listsWhy + adminNote(m)).c_str());
			ImGui::Spacing();
		}
		float av = ImGui::GetContentRegionAvail().x;
		bool wide = av >= ui::px(620.0f);
		float sp = ImGui::GetStyle().ItemSpacing.x;
		float w = wide ? (av - sp) * 0.5f : av;
		ImGuiTableFlags fl = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

		ui::beginCard("##lists","Page lists",ImVec2(w / ui::px(1.0f),0.0f));
		if (ImGui::BeginTable("##tl",3,fl)) {
			ImGui::TableSetupColumn("List",ImGuiTableColumnFlags_WidthStretch,1.6f);
			ImGui::TableSetupColumn("Size",ImGuiTableColumnFlags_WidthStretch,1.0f);
			ImGui::TableSetupColumn("of RAM",ImGuiTableColumnFlags_WidthStretch,0.8f);
			const char* const hd[] = {"List","Size","of RAM"};
			headRow(hd,3);
			listRow("Modified",m.modified,m.physTotal,"Changed pages that must reach disk before reuse.");
			listRow("Modified no-write",m.modifiedNoWrite,m.physTotal,"Modified pages the system won't write out for now.");
			listRow("Modified pagefile",m.modifiedPf,m.physTotal,"The part of Modified bound for the pagefile.");
			listRow("Standby",m.standby,m.physTotal,"Cache. Reusable at once, not lost memory.");
			listRow("Free",m.free,m.physTotal,"Free pages waiting to be zeroed.");
			listRow("Zeroed",m.zeroed,m.physTotal,"Free pages already wiped, first to be handed out.");
			listRow("Bad",m.bad,m.physTotal,"Pages the system took out of use after hardware errors.",m.bad != NA && m.bad);
			ImGui::EndTable();
		}
		ui::endCard();
		if (wide) ImGui::SameLine();

		ui::beginCard("##prio","Standby by priority",ImVec2(w / ui::px(1.0f),0.0f));
		bool anyPrio = false;
		for (uint64_t v : m.standbyPrio) if (v != NA) anyPrio = true;
		if (!anyPrio && m.peakSinceStart) {
			ui::hint("Linux has no standby priorities, the page cache is one pool.");
			kvRow("Reclaimed from cache since boot",sz(m.repurposedTotal));
		} else if (ImGui::BeginTable("##tp",3,fl)) {
			ImGui::TableSetupColumn("Priority",ImGuiTableColumnFlags_WidthStretch,1.0f);
			ImGui::TableSetupColumn("Standby",ImGuiTableColumnFlags_WidthStretch,1.0f);
			ImGui::TableSetupColumn("Repurposed",ImGuiTableColumnFlags_WidthStretch,1.0f);
			const char* const hd[] = {"Priority","Standby","Repurposed"};
			headRow(hd,3,2,"Standby pages taken for new use since boot, by priority. A running total, a high low-priority count is normal.");
			for (int i = 0; i < 8; i++) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("%d",i);
				if (!i) ui::tip("What Empty priority 0 standby list discards.");
				ImGui::TableNextColumn();
				numCell(sz(m.standbyPrio[i]));
				ImGui::TableNextColumn();
				numCell(sz(m.repurposed[i]));
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("Total");
			ImGui::TableNextColumn();
			numCell(sz(m.standby));
			ImGui::TableNextColumn();
			numCell(sz(m.repurposedTotal));
			ImGui::EndTable();
		}
		ui::endCard();
	}

	void drawRec(const smbios::Rec& r) {
		ImGuiTableFlags fl = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
		if (!ImGui::BeginTable("##kv",2,fl)) return;
		ImGui::TableSetupColumn("Name",ImGuiTableColumnFlags_WidthStretch,1.0f);
		ImGui::TableSetupColumn("Value",ImGuiTableColumnFlags_WidthStretch,1.2f);
		for (auto& f : r.f) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("%s",f.label.c_str());
			ImGui::TableNextColumn();
			cellText(f.val.empty() ? "N/A" : f.val);
		}
		ImGui::EndTable();
	}

	void recGroup(const char* heading,const std::vector<smbios::Rec>& v,bool open) {
		if (v.empty()) return;
		ImGui::PushID(heading);
		ImGui::Spacing();
		ImGui::TextUnformatted(heading);
		for (size_t i = 0; i < v.size(); i++) {
			ImGui::PushID((int)i);
			std::string lb = v[i].title + "##f";
			if (ui::fold(lb.c_str(),open)) drawRec(v[i]);
			ImGui::PopID();
		}
		ImGui::PopID();
	}

	std::string recsText(const std::vector<smbios::Rec>& v) {
		std::string s;
		for (auto& r : v) {
			s += r.title + "\n";
			for (auto& f : r.f) s += "\t" + f.label + "\t" + (f.val.empty() ? "N/A" : f.val) + "\n";
		}
		return s;
	}

	std::string dashIf(const std::string& s) {
		return s.empty() ? "N/A" : s;
	}

	void drawSticks(const smbios::Table& t) {
		ImGuiTableFlags fl = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
		if (!ImGui::BeginTable("##sticks",7,fl)) return;
		const char* const hd[] = {"Slot","Size","Type","Speed","Configured","Manufacturer","Part number"};
		// set weights : right aligned cells don't report a width, and only ImGui's own header row counts the header text
		const float wt[] = {2.45f,0.8f,0.65f,1.0f,1.05f,1.2f,2.1f};
		for (int i = 0; i < 7; i++) ImGui::TableSetupColumn(hd[i],ImGuiTableColumnFlags_WidthStretch,wt[i]);
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int i = 0; i < 7; i++) {
			ImGui::TableSetColumnIndex(i);
			if (i == 1 || i == 3 || i == 4) ui::cellRight(hd[i]);
			else ImGui::TextUnformatted(hd[i]);
		}
		for (auto& d : t.devices) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			cellText(d.title);
			ImGui::TableNextColumn();
			uint64_t s = d.num("size_bytes");
			if (s == 0) {
				ImGui::PushStyleColor(ImGuiCol_Text,ui::c.dim);
				ui::cellRight("Empty");
				ImGui::PopStyleColor();
				for (int i = 0; i < 5; i++) ImGui::TableNextColumn();
				continue;
			}
			numCell(util::size(s));
			ImGui::TableNextColumn();
			cellText(dashIf(d.str("memory_type")));
			ImGui::TableNextColumn();
			numCell(dashIf(d.str("speed_mts")));
			ImGui::TableNextColumn();
			numCell(dashIf(d.str("configured_speed_mts")));
			ImGui::TableNextColumn();
			cellText(dashIf(d.str("manufacturer")));
			ImGui::TableNextColumn();
			cellText(dashIf(d.str("part_number")));
		}
		ImGui::EndTable();
	}

	void drawHardware() {
		auto t = smbios::latest();
		if (!t->loaded) {
			ui::empty("Reading the SMBIOS table...");
			return;
		}
		if (!t->ok) {
			ui::empty(("No RAM modules : " + t->err).c_str());
			return;
		}
		float av = ImGui::GetContentRegionAvail().x;
		bool side = av >= ui::px(940.0f) && t->devices.size() <= 6; // below that the sticks table clips next to the summary
		float sumW = side ? ui::px(300.0f) : av;
		float lineH = ImGui::GetTextLineHeightWithSpacing();
		ImGuiStyle& st = ImGui::GetStyle();
		float sumH = side ? st.WindowPadding.y * 2.0f + ImGui::GetFontSize() * 1.08f + st.ItemSpacing.y + lineH * (t->major ? 7.0f : 6.0f) + ui::px(2.0f) : 0.0f;
		ui::beginCard("##hwsum","Hardware",ImVec2(sumW / ui::px(1.0f),sumH / ui::px(1.0f)));
		char slots[32];
		snprintf(slots,sizeof(slots),"%d of %d",t->slotsUsed,t->slotsTotal);
		std::vector<std::pair<std::string,std::string>> rows = {
			{"Slots used",t->slotsTotal ? slots : "N/A"},{"Memory type",dashIf(t->memType)},
			{"Installed",sz(t->installed)},{"Form factor",dashIf(t->formFactor)},
			{"Speed",t->speed != NA ? std::to_string(t->speed) + " MT/s" : "N/A"},{"Technology",dashIf(t->tech)}
		};
		if (t->major) rows.push_back({"SMBIOS version",std::to_string(t->major) + "." + std::to_string(t->minor)});
		if (ImGui::BeginTable("##hs",side ? 1 : 2,ImGuiTableFlags_SizingStretchSame)) {
			for (auto& r : rows) {
				ImGui::TableNextColumn();
				kvRow(r.first,r.second);
			}
			ImGui::EndTable();
		}
		ui::endCard();
		if (side) ImGui::SameLine();
		else ImGui::Spacing();
		ui::beginCard("##hwst","RAM modules",ImVec2(0.0f,sumH / ui::px(1.0f)));
		if (t->devices.empty()) ui::hint("The firmware lists no memory devices.");
		else drawSticks(*t);
		ui::endCard();
		ImGui::Spacing();
		ui::beginCard("##hwrec","SMBIOS records");
		if (ImGui::SmallButton("Copy all")) {
			std::string s = recsText(t->arrays) + recsText(t->devices) + recsText(t->errors) + recsText(t->arrayMaps) + recsText(t->deviceMaps);
			ImGui::SetClipboardText(s.c_str());
			ui::toast("SMBIOS records copied");
		}
		recGroup("Physical memory arrays (type 16)",t->arrays,false);
		recGroup("Memory devices (type 17)",t->devices,false);
		recGroup("32-bit memory errors (type 18)",t->errors,false);
		recGroup("Memory array mapped addresses (type 19)",t->arrayMaps,false);
		recGroup("Memory device mapped addresses (type 20)",t->deviceMaps,false);
		ui::endCard();
	}

	void fire(const std::string& id) {
		std::shared_ptr<sysmem::Run> r;
		std::string why = sysmem::start(id,false,&r);
		if (!why.empty()) {
			std::string label = id == "all" ? "Empty all" : id;
			for (int i = 0; i < sysmem::kCmdN; i++) if (id == sysmem::kCmds[i].id) label = sysmem::kCmds[i].label;
			appLog(label + " : failed (" + why + ")");
			ui::toast(label + " : " + why);
			return;
		}
		g_dlg = r;
		g_openDlg = true;
	}

	// palette entries run on Enter, so they ask first. ### keeps one popup id while the title follows the command
	std::string g_palAsk,g_palTxt;
	std::function<void()> g_palFn;

	void palAsk(const char* title,const std::string& txt,std::function<void()> fn) {
		g_palAsk = std::string(title) + "###palask";
		g_palTxt = txt + "\n\nRun it now?";
		g_palFn = std::move(fn);
		ui::askOpen(g_palAsk.c_str());
	}

	void drawPalAsk() {
		if (!g_palAsk.empty() && ui::askDraw(g_palAsk.c_str(),g_palTxt.c_str(),"Run",true) && g_palFn) g_palFn();
	}

	struct CleanCmd { const char* title; const char* txt; bool fs,ew,ps,fm; };
	const CleanCmd kClean[] = {
		{"Full Clean",OS_TIP("All four Dashboard methods in order : Flush Standby, Empty Working Sets, Purge Standby, Flush Modified. Apps fault their pages back in, expect a slow moment. Purge Standby needs admin.","All four Dashboard methods in order : Flush Standby, Empty Working Sets, Purge Standby, Flush Modified. Apps fault their pages back in, expect a slow moment. Purge Standby needs root, without root Empty Working Sets reaches FRS itself at most."),true,true,true,true},
		{"Flush Standby",OS_TIP("Briefly allocates the Dashboard's Flush block size of RAM to push cache out. Heavier, can cause paging. No admin needed.","Briefly allocates the Dashboard's Flush block size of RAM to push cache out. Heavier, can push pages to swap. No root needed."),true,false,false,false},
		{"Empty Working Sets",OS_TIP("Trims every process it can open, pages fault back on use. Without admin it only reaches some processes.","Pages out every process it can reach, pages fault back on use. File pages are dropped, anonymous pages only leave RAM with swap or zram. Without root it reaches FRS itself at most."),false,true,false,false},
		{"Purge Standby",OS_TIP("Drops the standby list. It is cache, not lost memory, apps read it back from disk. Needs admin. Same as Empty standby list on the Memory page.","Runs sync, then drops the clean page cache. It is cache, not lost memory, apps read it back from disk. Needs root. Same as Empty standby list on the Memory page."),false,false,true,false},
		{"Flush Modified",OS_TIP("Writes modified pages to disk, they become standby. Same as Empty modified page list on the Memory page.","Runs sync, dirty pages get written out and stay as page cache. Same as Empty modified page list on the Memory page."),false,false,false,true}
	};

	float emptyW() {
		return ImGui::CalcTextSize("Empty").x + ImGui::GetStyle().FramePadding.x * 2.0f + ui::px(16.0f);
	}

	// plain on purpose, the Dashboard's Clean is the main action. The chevron says it opens a menu
	void drawEmptyMenu() {
		bool cleaning = appCleaning();
		bool off = cleaning || sysmem::busy();
		if (off) ImGui::BeginDisabled();
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,ImVec2(0.0f,0.5f));
		bool hit = ImGui::Button("Empty##menu",ImVec2(emptyW(),0.0f));
		ImGui::PopStyleVar();
		ImVec2 a = ImGui::GetItemRectMin();
		ImVec2 b = ImGui::GetItemRectMax();
		float d = ui::px(3.5f);
		float cx = b.x - ImGui::GetStyle().FramePadding.x - d;
		float cy = (a.y + b.y) * 0.5f;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->PathLineTo(ImVec2(cx - d,cy - d * 0.5f));
		dl->PathLineTo(ImVec2(cx,cy + d * 0.5f));
		dl->PathLineTo(ImVec2(cx + d,cy - d * 0.5f));
		dl->PathStroke(ImGui::GetColorU32(ImGuiCol_Text),ImDrawFlags_None,ui::px(1.5f));
		if (off) ImGui::EndDisabled();
		if (off) ui::tip(cleaning ? "A clean is running." : "A memory command is running.");
		else ui::tip("Holds the calls behind Purge Standby and Flush Modified on the Dashboard, plus seven more. Runs at once, only Empty all asks.");
		if (hit) ImGui::OpenPopup("##emptymenu");
		ImGui::SetNextWindowPos(b,ImGuiCond_Appearing,ImVec2(1.0f,0.0f));
		if (ImGui::BeginPopup("##emptymenu")) {
			for (int i = 0; i < sysmem::kCmdN; i++) {
				const char* id = sysmem::kCmds[i].id;
				// the Dashboard button that makes the same call, dim on the right
				const char* twin = !strcmp(id,"standby") ? "Purge Standby" : !strcmp(id,"modified") ? "Flush Modified" : nullptr;
				if (ImGui::MenuItem(sysmem::kCmds[i].label,twin)) fire(id);
				ui::tip(sysmem::kCmds[i].tip);
			}
			ImGui::Separator();
			if (ImGui::Selectable("Empty all...")) ui::askOpen(kAskAll);
			ui::tip("Runs all of the above in order : working sets feed modified, modified feeds standby, standby goes last.");
			ImGui::EndPopup();
		}
	}

	void drawStrip(const SysMem& m) {
		std::string inst = m.physInstalled != NA ? sz(m.physInstalled) + " installed" : m.physTotal != NA ? sz(m.physTotal) + " total" : "";
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s",inst.c_str());
		if (cfg::getb("memPaused",false)) {
			ImGui::SameLine();
			ui::badge("Paused",ui::c.warn);
			ui::tip("Sampling is paused in Settings, these numbers are not moving. F5 takes one sample.");
		}
		auto r = sysmem::lastRun();
		std::string line;
		bool bad = false;
		if (r) {
			std::lock_guard<std::mutex> lk(r->m);
			bool done = r->done;
			int nF = 0;
			int nC = 0;
			int nU = 0;
			const sysmem::Step* cur = nullptr;
			for (auto& s : r->steps) {
				if (s.st == 1) cur = &s;
				if (s.st == 3) nF++;
				if (s.st == 4) nC++;
				if (s.st == 5) nU++;
			}
			if (!done && cur) line = cur->label + "...";
			else if (!done) line = "Starting memory command...";
			else if (r->id == "all") line = "Last : Empty all, " + (nF ? std::to_string(nF) + " failed" : nC ? std::string("cancelled") : nU ? "OK, " + std::to_string(nU) + " not available" : std::string("OK"));
			else if (!r->steps.empty()) line = "Last : " + r->steps[0].res.msg;
			bad = done && nF;
		}
		float bw = emptyW();
		float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		if (!line.empty()) {
			ImGui::SameLine(0.0f,ui::px(14.0f));
			float x = ImGui::GetCursorPosX();
			float w = right - bw - ui::px(12.0f) - x;
			if (w > ui::px(40.0f)) {
				ImGui::PushStyleColor(ImGuiCol_Text,bad ? ui::c.bad : ui::c.dim);
				ImGui::BeginChild("##last",ImVec2(w,ImGui::GetFrameHeight()),ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
				ImGui::SetCursorPosY((ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
				ui::textEllipsis(line.c_str());
				ImGui::PopStyleColor();
				ui::tip(line.c_str());
				ImGui::EndChild();
			}
		}
		ImGui::SameLine(right - bw);
		drawEmptyMenu();
	}

	void drawDlg() {
		const char* title = "Memory command##memdlg";
		if (g_openDlg) {
			ImGui::OpenPopup(title);
			g_openDlg = false;
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Always,ImVec2(0.5f,0.5f)); // it grows while steps finish, Close must stay on screen
		ImGui::SetNextWindowSizeConstraints(ImVec2(ui::px(480.0f),0.0f),ImVec2(ui::px(640.0f),ImGui::GetMainViewport()->Size.y * 0.9f));
		if (!ImGui::BeginPopupModal(title,nullptr,ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
		if (!g_dlg) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		auto run = g_dlg;
		std::vector<sysmem::Step> steps;
		{
			std::lock_guard<std::mutex> lk(run->m);
			steps = run->steps;
		}
		bool done = run->done;
		int n = (int)steps.size();
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ui::px(600.0f));
		ui::pushBold();
		ImGui::TextUnformatted(done ? "Finished" : n > 1 ? "Executing memory commands..." : "Executing memory command...");
		ui::popFont();
		for (auto& s : steps) {
			if (s.st == 0) ImGui::TextDisabled("%s",s.label.c_str());
			else if (s.st == 4) ImGui::TextDisabled("%s : cancelled",s.label.c_str());
			else if (s.st == 1) {
				const char* w = s.label.c_str();
				for (int i = 0; i < sysmem::kCmdN; i++) if (s.id == sysmem::kCmds[i].id) w = sysmem::kCmds[i].working;
				ImGui::TextColored(ui::c.accent,"%s",w);
			} else {
				ImGui::TextColored(s.st == 2 ? ui::c.good : s.st == 5 ? ui::c.dim : ui::c.bad,"%s",s.res.msg.c_str());
				for (auto& it : s.res.items) ImGui::TextDisabled("   %s : %s",it.first.c_str(),it.second.c_str());
			}
		}
		if (done && run->before && run->after) {
			std::string fb = sz(sum2(run->before->free,run->before->zeroed));
			std::string fa = sz(sum2(run->after->free,run->after->zeroed));
			ImGui::Spacing();
			ImGui::TextDisabled("Free and zeroed : %s before, %s after",fb.c_str(),fa.c_str());
			ImGui::TextDisabled("Available : %s before, %s after",sz(run->before->physAvail).c_str(),sz(run->after->physAvail).c_str());
		}
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		if (!done) {
			if (n > 1) {
				char ov[32];
				int k = run->nDone;
				snprintf(ov,sizeof(ov),"%d of %d",k,n);
				ui::meter((float)k / (float)n,ov,ImVec2(0.0f,18.0f),ui::c.accent);
			} else {
				ImGui::PushStyleColor(ImGuiCol_PlotHistogram,ui::c.accent);
				ImGui::ProgressBar(-(float)ImGui::GetTime(),ImVec2(-1.0f,ui::px(18.0f)),"");
				ImGui::PopStyleColor();
			}
			ImGui::Spacing();
			bool can = !run->cancel;
			if (!can) ImGui::BeginDisabled();
			if (ImGui::Button(can ? "Cancel##memdlg" : "Cancelling##memdlg",ImVec2(ui::px(110.0f),0.0f))) run->cancel = true;
			if (!can) ImGui::EndDisabled();
			ui::tip("Stops between commands. A running command can't be interrupted, this stays open until it ends.");
		} else if (ImGui::Button("Close##memdlg",ImVec2(ui::px(110.0f),0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			ImGui::CloseCurrentPopup();
			g_dlg = nullptr;
		}
		ImGui::EndPopup();
	}

}

namespace sysmem {

	void regPalette() {
		for (int i = 0; i < kCmdN; i++) {
			nav::addCmd(("Memory : " + std::string(kCmds[i].label)).c_str(),[i] {
				nav::go("memory");
				std::string id = kCmds[i].id;
				palAsk(kCmds[i].label,kCmds[i].tip,[id] { fire(id); });
			},std::string(kCmds[i].id) == "empty_working_sets" ? "trim free clean purge flush" : "free clean purge flush");
		}
		nav::addCmd("Memory : Empty all",[] {
			nav::go("memory");
			ui::askOpen(kAskAll);
		},"free clean purge flush");
		for (auto& c : kClean) {
			const CleanCmd* p = &c;
			nav::addCmd(("Clean : " + std::string(c.title)).c_str(),[p] {
				nav::go("dashboard");
				palAsk(p->title,p->txt,[p] {
					if (!appRunClean(p->fs,p->ew,p->ps,p->fm,"")) ui::toast("A clean is already running");
				});
			},std::string(c.title) == "Empty Working Sets" ? "trim free ram memory" : "free ram memory");
		}
		procwin::addFloat(drawPalAsk);
	}

	void drawPage() {
		auto m = latest();
		auto h = history();
		drawStrip(*m);
		const char* askTxt = m->peakSinceStart ?
			"Run all nine Empty commands now, in order? Apps will fault their pages back in, expect the system to feel slow for a moment. Registry cache and priority 0 standby don't exist on Linux, those two report not available. Combine memory pages turns KSM on, it stays on until reboot." :
			"Run all nine Empty commands now, in order? Apps will fault their pages back in, expect the system to feel slow for a moment. Compression normally reports not available, that process is protected.";
		if (ui::askDraw(kAskAll,askTxt,"Empty all",true)) fire("all");
		drawDlg();
		if (!ImGui::BeginTabBar("##memtabs")) return;
		if (ImGui::BeginTabItem("Overview")) {
			ImGui::BeginChild("##b1");
			drawOverview(*m,h);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Details")) {
			ImGui::BeginChild("##b2");
			drawDetails(*m);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Memory lists")) {
			ImGui::BeginChild("##b3");
			drawLists(*m);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Hardware")) {
			ImGui::BeginChild("##b4");
			drawHardware();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

}
