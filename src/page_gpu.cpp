#include "gpumem.h"
#include "sysmem.h"
#include "ui.h"
#include "cfg.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

	std::string usedOf(uint64_t a,uint64_t b) {
		return a == NA ? "unknown" : b == NA || !b ? "limit unknown" : util::pct(a,b) + " of " + util::size(b);
	}

	float mbOf(uint64_t v) {
		return v == NA ? 0.0f : (float)((double)v / 1048576.0);
	}

	void statCard(const char* id,const char* label,const std::string& val,const std::string& sub,ImVec4 col,float w,const char* tip) {
		ui::beginCard(id,nullptr,ImVec2(w / ui::px(1.0f),0.0f));
		ImGui::TextDisabled("%s",label);
		ui::tip(tip);
		ui::pushBig();
		if (val == "N/A") ImGui::TextDisabled("N/A");
		else ImGui::TextColored(col,"%s",val.c_str());
		ui::popFont();
		ImGui::TextDisabled("%s",sub.c_str());
		ui::endCard();
	}

	void graphCard(const char* id,const char* cap,const char* helpTxt,const std::shared_ptr<const GpuHist>& h,const std::vector<float>& v,ImVec4 col,float maxMb,const std::string& over,float w) {
		std::string name = cap;
		ui::Series s{v.empty() ? nullptr : v.data(),(int)v.size(),col,cap};
		ui::graphCard(id,cap,helpTxt,over.c_str(),s,maxMb,96.0f,[h,&v,name](int i) {
			if (i >= (int)v.size()) return std::string();
			float x = v[i];
			std::string t = name + " : " + (x < 0.0f ? std::string("N/A") : util::size((uint64_t)((double)x * 1048576.0)));
			if (i < (int)h->unixSec.size() && h->unixSec[i]) t += "\n" + util::timeStr(h->unixSec[i]);
			return t;
		},std::max(60,std::min(3600,cfg::geti("memHistN",600))),w);
	}

	void adapterCard(const GpuAdapter& a,const char* id,float w) {
		ui::beginCard(id,nullptr,ImVec2(w / ui::px(1.0f),0.0f));
		ui::pushBold();
		ui::textEllipsis(a.name.c_str());
		ui::popFont();
		ui::tip(a.id.c_str());
		ui::kv("Dedicated memory",util::size(a.ded) + " / " + util::size(a.dedLimit));
		ui::tip("In use over what the adapter reports. VRAM on a discrete card, not RAM.");
		ui::kv("Shared memory",util::size(a.shared) + " / " + util::size(a.sharedLimit));
		ui::tip("System RAM, already part of used RAM. The limit is what Windows lets the GPU use, not RAM in use.");
		if (a.sharedOs != NA) {
			ui::kvSize("Shared, adapter counter",a.sharedOs);
			ui::tip("The adapter's own Shared Usage counter, the one Task Manager shows. The line above sums what processes have resident, like System Informer, so the two differ.");
		}
		if (a.sysDed != NA && a.sysDed) {
			ui::kvSize("RAM set aside for it",a.sysDed);
			ui::tip("Part of the system's RAM reserved for this adapter.");
		}
		ui::kvSize("Total memory",a.installed);
		ui::tip("The size the driver writes to the registry, N/A when it has none.");
		ui::kvSize("Reserved memory",a.reserved);
		ui::tip("Total memory minus what is visible as dedicated. N/A without both.");
		ui::endCard();
	}

	void topList(const char* id,const char* cap,const GpuSnap& s,bool byShared,float w) {
		ui::beginCard(id,cap,ImVec2(w / ui::px(1.0f),0.0f));
		std::vector<const GpuProc*> v;
		for (auto& p : s.procs) if (byShared ? p.shared != NA && p.shared : p.ded != NA && p.ded) v.push_back(&p);
		std::sort(v.begin(),v.end(),[byShared](const GpuProc* a,const GpuProc* b) { return byShared ? a->shared > b->shared : a->ded > b->ded; });
		if (v.empty()) ImGui::TextDisabled("Nothing right now.");
		for (size_t i = 0; i < v.size() && i < 5; i++) {
			ImGui::PushID((int)i);
			std::string val = util::size(byShared ? v[i]->shared : v[i]->ded);
			float vw = ImGui::CalcTextSize(val.c_str()).x;
			float x = ImGui::GetCursorPosX();
			if (ImGui::Selectable("##top",false,0,ImVec2(0,0))) {
				ProcRef r;
				r.pid = v[i]->pid;
				r.start = v[i]->start;
				r.name = v[i]->name;
				procwin::open(r,"GPU");
			}
			ImGui::SameLine(x);
			float nameW = ImGui::GetContentRegionAvail().x - vw - ui::px(10.0f);
			ImGui::BeginGroup();
			ImVec2 p0 = ImGui::GetCursorScreenPos();
			ImGui::PushClipRect(p0,ImVec2(p0.x + nameW,p0.y + ImGui::GetTextLineHeightWithSpacing()),true);
			ImGui::TextUnformatted(v[i]->name.c_str());
			ImGui::PopClipRect();
			ImGui::EndGroup();
			ImGui::SameLine();
			ui::cellRight(val.c_str());
			ImGui::PopID();
		}
		ui::endCard();
	}

	void drawOverview(const GpuSnap& s,const std::shared_ptr<const GpuHist>& h) {
		auto m = sysmem::latest();
		ui::hint("Shared GPU memory is system RAM and already part of used RAM, never add it on top. Dedicated is VRAM on a discrete card, not RAM. A RAM clean is not known to free GPU memory.");
		ImGui::Spacing();
		float sp = ImGui::GetStyle().ItemSpacing.x;
		float av = ImGui::GetContentRegionAvail().x;
		float w3 = (av - sp * 2.0f) / 3.0f;
		statCard("##cd","Dedicated memory",util::size(s.ded),usedOf(s.ded,s.dedLimit),ui::c.purple,w3,"VRAM in use across adapters, over their limit.");
		ImGui::SameLine();
		statCard("##cs","Shared memory",util::size(s.shared),usedOf(s.shared,s.sharedLimit),ui::c.phys,w3,"System RAM the GPU has resident. The limit is how much Windows lets each adapter use, usually half of RAM, added up across adapters and capped at total RAM.");
		ImGui::SameLine();
		statCard("##cr","Inside used RAM",util::pct(s.shared,m->physUsed),m->physUsed == NA ? "used RAM unknown" : "of " + util::size(m->physUsed) + " used RAM",ui::c.text,w3,"How much of used RAM is the GPU's shared memory. It is a part of that number, not on top of it.");
		if (s.shared != NA && m->physUsed != NA && m->physTotal != NA) {
			uint64_t shr = std::min(s.shared,m->physUsed);
			std::string cap = "Used RAM, the shared GPU memory is the cyan part inside it : " + util::size(shr) + " of " + util::size(m->physUsed);
			ImGui::TextDisabled("%s",cap.c_str());
			ui::Seg seg[3] = {
				{(float)shr,ui::c.phys,"Shared GPU memory"},
				{(float)(m->physUsed - shr),ui::c.dim,"Other used RAM"},
				{(float)(m->physTotal > m->physUsed ? m->physTotal - m->physUsed : 0),ui::c.panel2,"Not in use"}
			};
			ui::stackBar("##rambar",seg,3,ImVec2(0.0f,12.0f));
		}
		ImGui::Spacing();
		float w2 = (av - sp) * 0.5f;
		float mxD = s.dedLimit != NA && s.dedLimit ? mbOf(s.dedLimit) : 0.0f;
		float mxS = m->physTotal != NA ? mbOf(m->physTotal) : 0.0f;
		graphCard("##gd","Dedicated memory","Dedicated use over time, scaled to the limit. VRAM, not RAM.",h,h->ded,ui::c.purple,mxD,util::size(s.ded) + " / " + util::size(s.dedLimit),w2);
		ImGui::SameLine();
		graphCard("##gs","Shared memory","Shared use over time, scaled to total RAM so it shows how small it is against RAM.",h,h->shared,ui::c.phys,mxS,util::size(s.shared) + " / " + util::size(s.sharedLimit),w2);
		ImGui::Spacing();
		int cols = std::max(1,std::min(2,(int)s.adapters.size()));
		float wa = (av - sp * (float)(cols - 1)) / (float)cols;
		for (size_t i = 0; i < s.adapters.size(); i++) {
			if (i % cols) ImGui::SameLine();
			std::string id = "##ad" + std::to_string(i);
			adapterCard(s.adapters[i],id.c_str(),wa);
		}
		ImGui::Spacing();
		topList("##td","Top by dedicated memory (VRAM)",s,false,w2);
		ImGui::SameLine();
		topList("##ts","Top by shared memory (RAM)",s,true,w2);
		std::string src = s.source;
		ImGui::TextDisabled("Source : %s.",src == "performance counters" ? "Windows performance counters, the numbers Task Manager shows" : src == "D3DKMT" ? "the graphics driver, asked per process" : src == "sysfs" ? "the kernel's GPU driver files" : s.source);
		if (!s.why.empty()) ui::hint(util::safe(s.why).c_str());
	}

	// ---- process table ----

	char g_filter[128] = "";
	int g_sortCol = 3;
	bool g_desc = true;

	bool lessCi(const std::string& a,const std::string& b) {
		return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),[](char x,char y) {
			return (x >= 'A' && x <= 'Z' ? x + 32 : (unsigned char)x) < (y >= 'A' && y <= 'Z' ? y + 32 : (unsigned char)y);
		});
	}

	uint64_t procVal(const GpuProc& p,int c) {
		return c == 2 ? p.ded : c == 3 ? p.shared : c == 4 ? p.dedCommit : c == 5 ? p.sharedCommit : p.commit;
	}

	void drawProcs(const GpuSnap& s) {
		ui::search("##gflt",g_filter,sizeof(g_filter),"Filter by name or pid",260.0f);
		ImGui::SameLine();
		ImGui::TextDisabled("%d processes hold GPU memory",(int)s.procs.size());
		if (!s.denied.empty()) {
			ImGui::SameLine();
			ImGui::TextColored(ui::c.warn,"%d could not be read",(int)s.denied.size());
			ui::tip("FRS could not look at these processes, so they are missing here. It needs admin or root for that.");
		}
		std::vector<const GpuProc*> v;
		std::string needle = g_filter;
		for (auto& p : s.procs) if (needle.empty() || util::has(p.name,needle) || util::has(std::to_string(p.pid),needle)) v.push_back(&p);
		if (v.empty()) {
			ui::empty(s.procs.empty() ? "No process holds GPU memory right now." : "Nothing matches the filter.");
			return;
		}
		std::string commit = std::string(s.commitName) + " (?)";
		const char* titles[7] = {"Name","PID","Dedicated (resident)","Shared (resident)","Dedicated (committed)","Shared (committed)",commit.c_str()};
		std::string kmt = gpumem::kmtNote(s);
		std::string tDc = "Windows counter Dedicated Usage, what Task Manager calls dedicated GPU memory. On Linux drm-total-vram." + kmt;
		std::string tSc = "Windows counter Shared Usage, what Task Manager calls shared GPU memory. On Linux drm-total-gtt." + kmt;
		const char* tips[7] = {
			nullptr,nullptr,
			"Windows counter Local Usage, VRAM in use now. On Linux drm-resident-vram.",
			"Windows counter Non Local Usage, system RAM that is part of used RAM. On Linux drm-resident-gtt.",
			tDc.c_str(),
			tSc.c_str(),
			s.hasCommit ? "Windows' own Total Committed counter, or the driver's system memory when reading through D3DKMT. It is not the sum of the other columns, Windows counts it on its own, so it can read below Dedicated (committed)." : "Linux has no such number."
		};
		if (!ui::beginTable("##gprocs",7,ImVec2(0,0),ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit)) return;
		float arrow = std::floor(ImGui::GetFontSize() * 0.65f + ImGui::GetStyle().FramePadding.x);
		for (int c = 0; c < 7; c++) {
			ImGuiTableColumnFlags f = ImGuiTableColumnFlags_WidthFixed;
			if (c == 0) f |= ImGuiTableColumnFlags_NoHide;
			if (c == 3) f |= ImGuiTableColumnFlags_DefaultSort;
			if (c >= 2) f |= ImGuiTableColumnFlags_PreferSortDescending;
			float base = c == 0 ? ui::px(190.0f) : c == 1 ? ui::px(60.0f) : ImGui::CalcTextSize(titles[c]).x + arrow;
			ImGui::TableSetupColumn(titles[c],f,base);
		}
		ImGui::TableSetupScrollFreeze(1,1);
		if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
			if (sp->SpecsDirty && sp->SpecsCount > 0) {
				g_sortCol = sp->Specs[0].ColumnIndex;
				g_desc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
			}
			sp->SpecsDirty = false;
		}
		int sc = g_sortCol;
		bool d = g_desc;
		std::stable_sort(v.begin(),v.end(),[sc,d](const GpuProc* a,const GpuProc* b) {
			if (sc == 0) return d ? lessCi(b->name,a->name) : lessCi(a->name,b->name);
			if (sc == 1) return d ? a->pid > b->pid : a->pid < b->pid;
			uint64_t x = procVal(*a,sc);
			uint64_t y = procVal(*b,sc);
			if (x == NA || y == NA) return x != NA && y == NA;
			return d ? x > y : x < y;
		});
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int c = 0; c < 7; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			ImGui::PushID(c);
			ui::headerCell(titles[c],tips[c],c >= 1);
			ImGui::PopID();
		}
		ImGuiListClipper clip;
		clip.Begin((int)v.size());
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
				const GpuProc& p = *v[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushID((int)p.pid);
				if (ImGui::Selectable("##r",false,ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					ProcRef r;
					r.pid = p.pid;
					r.start = p.start;
					r.name = p.name;
					procwin::open(r,"GPU");
				}
				ui::tip("Double click opens the process window on its GPU tab, with graphs and min and max.");
				ImGui::SameLine(0.0f,0.0f);
				ImGui::TextUnformatted(p.name.c_str());
				ImGui::PopID();
				if (ImGui::TableNextColumn()) ui::cellRight(std::to_string(p.pid).c_str());
				for (int c = 2; c < 7; c++) if (ImGui::TableNextColumn()) ui::cellSize(procVal(p,c));
			}
		}
		ui::endTable();
	}

	void draw() {
		auto s = gpumem::latest();
		if (!s->t) {
			ui::empty("Reading GPU memory...");
			return;
		}
		if (!s->on || s->adapters.empty()) {
			ui::empty(s->why.empty() ? "No GPU found." : util::safe(s->why).c_str());
			return;
		}
		auto h = gpumem::history();
		if (!ImGui::BeginTabBar("##gputabs")) return;
		if (ImGui::BeginTabItem("Overview")) {
			ImGui::BeginChild("##go");
			drawOverview(*s,h);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Processes")) {
			ImGui::BeginChild("##gp");
			drawProcs(*s);
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

}

namespace gpumem {

	void initPage() {
		nav::addPage("gpu","GPU",ui::IcoGpu,nav::GrpInspect,draw);
	}

}
