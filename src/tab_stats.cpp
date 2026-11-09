#include "procdet.h"
#include "ui.h"
#include "cfg.h"
#include "ram.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace {

	using namespace procdet;
	using Key = std::pair<uint32_t,uint64_t>;

#ifdef _WIN32
	const bool WIN = true;
#else
	const bool WIN = false;
#endif

	enum Kind { KSz,KDl,KCnt,KCntD,KLim,KPrio };

	enum {
		RPriv,RDPriv,RPeakPriv,RVirt,RPeakVirt,RFaults,RDFaults,RHard,RDHard,RWs,RPeakWs,RPrivWs,RShareable,RShared,
		RPaged,RPeakPaged,RNonPaged,RPeakNonPaged,RSharedCommit,RPrivCommit,RPeakCommit,RPrivLimit,RTotalLimit,RPrio,NROWS
	};

	struct RowDef {
		const char* name;
		const char* lxName;
		Kind k;
		const char* tip;
	};

	const RowDef ROWS[NROWS] = {
		{"Private bytes",nullptr,KSz,"Private commit. Includes pages not in RAM, so a trim does not lower it. Linux : RssAnon + VmSwap."},
		{"Private bytes delta",nullptr,KDl,"Change per sample, negative when released."},
		{"Peak private bytes",nullptr,KSz,"Highest private bytes so far. Linux : highest FRS saw."},
		{"Virtual size",nullptr,KSz,"Address space size, not memory in use."},
		{"Peak virtual size",nullptr,KSz,"Largest address space size so far."},
		{"Page faults",nullptr,KCnt,"Total faults since the process started. Most are soft, the page was still in RAM."},
		{"Page faults delta",nullptr,KCntD,"Faults per sample. A spike after a trim is pages faulting back in."},
		{"Hard faults",nullptr,KCnt,"Total faults that read from disk."},
		{"Hard faults delta",nullptr,KCntD,"Hard faults per sample, the best live measure of what a trim cost."},
		{"Working set",nullptr,KSz,"Resident memory, includes shared pages. A trim lowers this, pages move to standby and fault back when used. Linux : file pages are dropped and anonymous ones go to swap."},
		{"Peak working set",nullptr,KSz,"Highest working set so far."},
		{"Private working set",nullptr,KSz,"Resident pages only this process uses. From the page walk when FRS can read it, else from the system snapshot."},
		{"Shareable working set",nullptr,KSz,"Resident pages that could be shared. Page walk, N/A when FRS can't read it. Linux : RssFile + RssShmem."},
		{"Shared working set",nullptr,KSz,"Resident pages actually shared right now. Page walk, N/A when FRS can't read it."},
		{"Paged pool bytes",nullptr,KSz,"Paged pool quota charged. Not on Linux."},
		{"Peak paged pool bytes",nullptr,KSz,"Highest paged pool quota. Not on Linux."},
		{"Nonpaged pool bytes",nullptr,KSz,"Non-paged pool quota charged. A trim can't touch it. Not on Linux."},
		{"Peak nonpaged pool bytes",nullptr,KSz,"Highest non-paged pool quota. Not on Linux."},
		{"Shared commit",nullptr,KSz,"Commit charged for shared memory. Linux : RssShmem."},
		{"Private commit","Cgroup memory",KSz,"Windows reports this for the whole job when the process is in one, so it can be far above the process's private bytes. Linux : memory.current of its cgroup, everything in there together."},
		{"Peak private commit","Cgroup peak",KSz,"Highest of the same, whole job when in one. Linux : memory.peak of its cgroup, 5.19 or newer."},
		{"Private commit limit","Cgroup memory.max",KLim,"For a process in a job this is the job memory limit, read without a job handle. Linux : memory.max of its cgroup."},
		{"Total commit limit","Cgroup memory.swap.max",KLim,"Same for total commit. Linux : memory.swap.max of its cgroup."},
		{"Page priority",nullptr,KPrio,"Memory priority of new pages. Low priority pages leave standby first. Not on Linux."},
	};

	const int RING = 512;

	struct Met {
		bool any = false;
		double mn = 0,mx = 0,avg = 0,med = 0;
		std::vector<double> ring;
		size_t pos = 0;
	};

	struct St {
		bool hasRow = false;
		ProcRow row;
		uint64_t seq = 0,drawMs = 0;
		Met m[NROWS];
		double val[NROWS] = {};
		bool known[NROWS] = {};
		bool noLim[NROWS] = {};
		std::shared_ptr<const ProcHist> hist;
		std::shared_ptr<const Det> det; // the worker forgets a process soon after it exits, this keeps the last read on screen
		int pick = 3; // Linux : the priority a saved rule gets, index in PRIO
		char key[512] = "";
		int addKind = 0,addPrio = 2;
		int ifeoPick = 4;
		bool ifeoAsk = false; // saveMenu asked for the IFEO dialog, it lives in drawActions
		uint32_t menuTid = 0;
		uint64_t menuStart = 0;
		int menuPrio = -1;
		double qMin = 0,qMax = 0;
		bool qFill = false;
	};

	std::map<Key,St> g_st; // UI thread only

	const int PRIO[] = {1,2,3,4,5};
	const char* const PRIO_NAMES[] = {"Very low","Low","Medium","Below normal","Normal"};

	St& stFor(const ProcRef& r) {
		uint64_t now = util::nowMs();
		for (auto it = g_st.begin(); it != g_st.end();) {
			if (now > it->second.drawMs + 120000) it = g_st.erase(it);
			else ++it;
		}
		St& s = g_st[Key(r.pid,r.start)];
		s.drawMs = now;
		return s;
	}

	void addSample(Met& m,double v) {
		if (!m.any) {
			m.mn = m.mx = v;
			m.any = true;
		}
		// compares as a signed double and keeps a real 0, SI's unsigned compare and zero skip get both wrong
		m.mn = std::min(m.mn,v);
		m.mx = std::max(m.mx,v);
		if ((int)m.ring.size() < RING) m.ring.push_back(v);
		else {
			m.ring[m.pos] = v;
			m.pos = (m.pos + 1) % RING;
		}
		double sum = 0;
		for (double x : m.ring) sum += x;
		m.avg = sum / (double)m.ring.size();
		std::vector<double> t = m.ring;
		std::sort(t.begin(),t.end());
		size_t n = t.size();
		m.med = n & 1 ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) / 2.0;
	}

	void setU(St& s,int i,uint64_t v) {
		s.known[i] = v != NA;
		s.noLim[i] = v == NOLIM;
		s.val[i] = v == NA || v == NOLIM ? 0.0 : (double)v;
	}

	void setD(St& s,int i,bool ok,int64_t v) {
		s.known[i] = ok;
		s.val[i] = ok ? (double)v : 0.0;
	}

	uint64_t pick2(uint64_t a,uint64_t b) {
		return a != NA ? a : b;
	}

	void update(const ProcRef& r,St& s,const Det* d) {
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		auto h = procs::history(r);
		if (h) s.hist = h;
		if (!row) return;
		if (s.hasRow && s.seq == l->seq) return;
		s.row = *row;
		s.hasRow = true;
		bool fresh = s.seq != l->seq;
		s.seq = l->seq;
		const ProcRow& o = *row;
		setU(s,RPriv,o.priv);
		setD(s,RDPriv,!o.noPrev && o.priv != NA,o.dPriv);
		setU(s,RPeakPriv,o.privPeak);
		setU(s,RVirt,o.virt);
		setU(s,RPeakVirt,o.virtPeak);
		setU(s,RFaults,o.faults);
		setD(s,RDFaults,!o.noPrev && o.faults != NA,o.dFaults);
		setU(s,RHard,o.hardFaults);
		setD(s,RDHard,!o.noPrev && o.hardFaults != NA,o.dHard);
		setU(s,RWs,o.ws);
		setU(s,RPeakWs,o.wsPeak);
		setU(s,RPrivWs,pick2(d ? d->ws.priv : NA,o.wsPriv));
		setU(s,RShareable,pick2(d ? d->ws.shareable : NA,o.wsShareable));
		setU(s,RShared,pick2(d ? d->ws.shared : NA,o.wsShared));
		setU(s,RPaged,o.paged);
		setU(s,RPeakPaged,o.pagedPeak);
		setU(s,RNonPaged,o.nonPaged);
		setU(s,RPeakNonPaged,o.nonPagedPeak);
		setU(s,RSharedCommit,o.sharedCommit);
		setU(s,RPrivCommit,d ? d->commit.priv : NA);
		setU(s,RPeakCommit,d ? d->commit.peak : NA);
		setU(s,RPrivLimit,d ? d->commit.privLimit : NA);
		setU(s,RTotalLimit,d ? d->commit.totalLimit : NA);
		s.known[RPrio] = o.pagePrio >= 0;
		s.val[RPrio] = o.pagePrio;
		if (!fresh) return;
		for (int i = 0; i < NROWS; i++) if (s.known[i] && ROWS[i].k != KLim && ROWS[i].k != KPrio) addSample(s.m[i],s.val[i]);
	}

	std::string fmt(Kind k,double v) {
		switch (k) {
			case KSz: return util::size((uint64_t)v);
			case KDl: {
				std::string t = util::delta((int64_t)v);
				return t.empty() ? "0 B" : t;
			}
			case KCnt: case KCntD: return util::count((uint64_t)(v + 0.5));
			case KLim: return util::size((uint64_t)v);
			default: return "";
		}
	}

	std::string limText(uint64_t v) {
		return v == NA ? "N/A" : v == NOLIM ? "No limit" : util::size(v);
	}

	void kvRow(const char* k,const std::string& v,const char* tip = nullptr,ImVec4 col = ImVec4(0,0,0,0)) {
		ImGui::BeginGroup();
		ImGui::TextDisabled("%s",k);
		ImGui::SameLine();
		// a gap before the next pair, or this value runs into its label
		float g = ImGui::TableGetColumnIndex() + 1 < ImGui::TableGetColumnCount() ? ui::px(24.0f) : 0.0f;
		float av = ImGui::GetContentRegionAvail().x - g;
		float w = ImGui::CalcTextSize(v.c_str()).x;
		if (av > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + av - w);
		if (v == "N/A") ImGui::TextDisabled("N/A");
		else if (col.w > 0.0f) ImGui::TextColored(col,"%s",v.c_str());
		else ImGui::TextUnformatted(v.c_str());
		ImGui::EndGroup();
		if (tip) ui::tip(tip);
	}

	void headers(int cols,int leftCols) {
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int i = 0; i < cols; i++) if (ImGui::TableSetColumnIndex(i)) ui::headerCell(ImGui::TableGetColumnName(i),nullptr,i >= leftCols);
	}

	float tableH(int rows) {
		return ((float)rows * (ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f) + ui::px(4.0f)) / ui::px(1.0f);
	}

	void graphCard(const char* id,const char* cap,const char* helpTxt,const std::vector<float>* v,const std::vector<uint64_t>* t,ImVec4 col,uint64_t cur,uint64_t peak,float w) {
		const float M = 1048576.0f;
		float mx = !cfg::getb("procGraphFit",false) && peak != NA ? (float)peak / M : 0.0f;
		int n = v ? (int)v->size() : 0;
		for (int i = 0; i < n; i++) mx = std::max(mx,(*v)[i]);
		if (mx <= 0.0f) mx = 1.0f;
		ui::Series sr{n ? v->data() : nullptr,n,col,cap};
		std::string top = "max " + util::size((uint64_t)((double)mx * 1048576.0));
		ui::graphCard(id,cap,helpTxt,util::size(cur).c_str(),sr,mx,104.0f,[&](int i) -> std::string {
			if (i < 0 || i >= n) return "";
			if ((*v)[i] < 0.0f) return std::string(cap) + " : N/A";
			std::string s = std::string(cap) + " : " + util::size((uint64_t)((double)(*v)[i] * 1048576.0));
			if (t && i < (int)t->size()) s += "\n" + util::timeStr((*t)[i]);
			return s;
		},std::max(60,std::min(300,cfg::geti("memHistN",600))),w,top.c_str());
	}

	void drawGraphs(const St& s) {
		float sp = ImGui::GetStyle().ItemSpacing.x;
		float av = ImGui::GetContentRegionAvail().x;
		bool side = av >= ui::px(520.0f);
		float w = side ? (av - sp) * 0.5f : av;
		const ProcHist* h = s.hist.get();
		graphCard("##gpriv","Private bytes","Private commit over time, newest on the right. Scale runs up to the process's peak unless the option on the Settings page says otherwise.",h ? &h->privMb : nullptr,h ? &h->unixSec : nullptr,ui::c.priv,s.row.priv,s.row.privPeak,w);
		if (side) ImGui::SameLine();
		graphCard("##gws","Working set","Resident memory over time, includes shared pages. System Informer has no such graph, this one is new.",h ? &h->wsMb : nullptr,h ? &h->unixSec : nullptr,ui::c.ws,s.row.ws,s.row.wsPeak,w);
	}

	void runRule(const ProcRef& r,int kind,const std::string& key,int prio,bool on) {
		task::run([=](task::T&) -> std::string {
			std::string lb = std::string("Page priority rule (") + ruleKind(kind) + ")";
			std::string why = on ? addRule(kind,key,prio) : delRule(kind,key);
			std::string res = lb + (why.empty() ? " : OK" : " : failed (" + why + ")");
			ui::toast(res);
			poke(r);
			return res;
		});
	}

	bool hasRule(const std::vector<Rule>& rs,int kind,const std::string& key) {
		std::string low = util::lower(key);
		for (auto& x : rs) if (x.kind == kind && (kind == RuleCmd ? x.key == key : util::lower(x.key) == low)) return true;
		return false;
	}

	// a long name would push the next tick off the card, the tip has it in full
	std::string lblName(std::string s) {
		size_t n = 24;
		if (s.size() > n) {
			while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
			s = s.substr(0,n) + "...";
		}
		std::replace(s.begin(),s.end(),'#','_');
		return s;
	}

	const char* NOPRIO = "FRS can't read this process's page priority, so there is nothing to save. Add a rule with a priority in the form below.";

	void drawIfeoDialog(const ProcRef& r,St& s,const char* id,const std::string& exe,int cur) {
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,ImVec2(0.5f,0.5f));
		if (!ImGui::BeginPopupModal(id,nullptr,ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
		ui::pushBold();
		ImGui::TextUnformatted(exe.c_str());
		ui::popFont();
		ImGui::TextUnformatted("Select the default process page priority.");
		ImGui::PushTextWrapPos(ui::px(420.0f));
		ImGui::TextDisabled("Windows applies it when the process starts, even when FRS isn't running. Only the PagePriority value is written, anything else under this program's key stays.");
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		for (int i = 0; i < 5; i++) ImGui::RadioButton(PRIO_NAMES[i],&s.ifeoPick,i);
		ImGui::Spacing();
		float bw = ui::px(90.0f);
		if (ImGui::Button("Save",ImVec2(bw,0))) {
			runRule(r,RuleIfeo,exe,PRIO[s.ifeoPick],true);
			ImGui::CloseCurrentPopup();
		}
		if (cur >= 0) {
			ImGui::SameLine();
			if (ImGui::Button("Remove",ImVec2(bw,0))) {
				runRule(r,RuleIfeo,exe,0,false);
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel",ImVec2(bw,0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	void drawActions(const ProcRef& r,St& s,const Det* d,bool live,const std::vector<Rule>& rs) {
		const ProcRow& o = s.row;
		bool can = live && !o.pseudo && !sysLike(o);
		ui::beginCard("##act","Actions");
		ImGui::BeginDisabled(!can);
		if (ImGui::Button("Reduce working set")) {
			task::run([r](task::T&) -> std::string {
				Trim t;
				std::string m = trim(r,&t);
				std::string res = t.before != NA && t.after != NA ? m + " (" + util::size(t.before) + " to " + util::size(t.after) + ")" : m;
				ui::toast(res);
				return res;
			});
		}
		ImGui::EndDisabled();
		ui::tip(WIN ? "Moves resident pages to the standby list. Nothing is lost, they fault back when used, so hard faults can rise after this. Private bytes does not change."
			: "Drops file pages and moves anonymous pages to swap or zram, without swap they stay in RAM. Nothing is lost, they fault back when used, so hard faults can rise after this. Private bytes does not change.");
		if (!can) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s",!live ? "Process has exited" : o.pseudo ? "Pseudo process, it has no memory of its own" : "System process, refused");
		}
		ImGui::TextDisabled("Page priority");
		ImGui::SameLine();
		ImVec4 cols[5];
		for (auto& c : cols) c = ui::c.accent;
		int cur = -1;
		for (int i = 0; i < 5; i++) if (PRIO[i] == o.pagePrio) cur = i;
		if (!WIN) cur = s.pick;
		ImGui::BeginDisabled(!can);
		if (ui::segmented("##pp",PRIO_NAMES,cols,5,cur)) {
			if (!WIN) s.pick = cur;
			else {
				int v = PRIO[cur];
				task::run([r,v](task::T&) -> std::string {
					bool fol = false;
					std::string m = setPrio(r,v,nullptr,&fol);
					if (fol) m += " (saved rule updated)";
					ui::toast(m);
					return m;
				});
			}
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ui::help(WIN ? "Memory priority of the pages this process faults in from now on. Low priority pages leave standby first, no trim needed." : "Linux has no page priority. This picks what a saved rule gets : below Normal marks the process's pages cold once a minute (MADV_COLD), so reclaim takes them first.");
		if (WIN && cur < 0) {
			ImGui::SameLine();
			ImGui::TextDisabled("Now : %s",procs::pagePrioName(o.pagePrio));
		}
		int prio = WIN ? o.pagePrio : PRIO[s.pick];
		bool known = prio >= 1 && prio <= 5;
		std::string nm = lblName(o.name);
		bool on = hasRule(rs,RuleName,o.name);
		bool was = on;
		ImGui::BeginDisabled(!can || (!on && !known));
		ImGui::Checkbox(("Save for " + nm + "##rn").c_str(),&on);
		ImGui::EndDisabled();
		std::string t = known ? o.name + " : FRS keeps every process with this name at the page priority it has now, for as long as FRS runs. Set the priority first, then tick this." : o.name + " : " + NOPRIO;
		ui::tip(t.c_str());
		if (on != was) runRule(r,RuleName,o.name,prio,on);
		std::string cmd = d ? d->cmd : "";
		on = !cmd.empty() && hasRule(rs,RuleCmd,cmd);
		was = on;
		ImGui::SameLine();
		ImGui::BeginDisabled(!can || cmd.empty() || (!on && !known));
		ImGui::Checkbox("Save for this command line##rc",&on);
		ImGui::EndDisabled();
		ui::tip(known ? "Same, for this exact command line, other arguments don't match. Wins over a rule by name." : NOPRIO);
		if (on != was) runRule(r,RuleCmd,cmd,prio,on);
		std::string dlg = "Default page priority##if" + std::to_string(r.pid) + "_" + std::to_string(r.start);
		if (ifeoSupported()) {
			int ifv = d ? d->ifeo : -1;
			on = ifv >= 0;
			was = on;
			ImGui::SameLine();
			ImGui::BeginDisabled(!can || !ifeoWritable());
			ImGui::Checkbox(("Save for " + nm + " (IFEO)...##ri").c_str(),&on);
			ImGui::EndDisabled();
			t = o.name + (ifeoWritable() ? " : Writes the page priority into the registry, Windows applies it at process start even when FRS isn't running." : " : Writes the page priority into the registry, so it needs FRS running as admin.");
			ui::tip(t.c_str());
			if (on != was || s.ifeoAsk) {
				s.ifeoAsk = false;
				s.ifeoPick = 4;
				for (int i = 0; i < 5; i++) if (PRIO[i] == (ifv >= 0 ? ifv : o.pagePrio)) s.ifeoPick = i;
				ImGui::OpenPopup(dlg.c_str());
			}
			drawIfeoDialog(r,s,dlg.c_str(),o.name,ifv);
		}
		if (live && !o.pseudo && (o.noHandle || (d && d->denied))) ui::hint("FRS can't open this process, so these rows and actions can fail. The rest comes from the system snapshot.");
		ui::endCard();
	}

	// a pseudo row never gets these reads, the worker says why instead
	const char* notYet(const St& s,const Det* d,bool live) {
		if (s.row.pseudo && d) return d->why.c_str();
		return live ? "Reading..." : "Not read before the process exited";
	}

	void drawWs(const St& s,const Det* d,bool live) {
		ui::beginCard("##wsc","Working set counters");
		const Ws w = d ? d->ws : Ws();
		if (!d || !d->wsRead) {
			ImGui::TextDisabled("%s",notYet(s,d,live));
			ui::endCard();
			return;
		}
		if (!w.why.empty()) {
			std::string t = "Page walk failed : " + w.why + ". Private working set below comes from the system snapshot, the rest stays unknown.";
			ui::hint(t.c_str());
		} else if (w.total != NA) {
			float pv = (float)(w.priv == NA ? 0 : w.priv);
			float sh = (float)(w.shared == NA ? 0 : w.shared);
			float sa = (float)(w.shareable == NA || w.shareable < (w.shared == NA ? 0 : w.shared) ? 0 : w.shareable - w.shared);
			ui::Seg seg[] = {{pv,ui::c.priv,"Private"},{sa,ui::c.cache,"Shareable, not shared now"},{sh,ui::c.ws,"Shared with another process"}};
			ui::stackBar("##wsbar",seg,3,ImVec2(0.0f,14.0f));
			ui::legend(seg,3);
		}
		if (ImGui::BeginTable("##wsk",2,ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
			ImGui::TableNextColumn();
			kvRow("Total working set",util::size(w.total),"Pages in the working set list. Matches Working set above, give or take a sample.");
			ImGui::TableNextColumn();
			kvRow("Locked working set",util::size(w.locked),"Pages locked in RAM, a trim can't move them. Windows : from the extended page query.");
			ImGui::TableNextColumn();
			kvRow("Private working set",util::size(w.priv != NA ? w.priv : s.row.wsPriv),"Pages only this process uses.");
			ImGui::TableNextColumn();
			kvRow("Highest page priority",w.prio >= 0 ? procs::pagePrioName(w.prio) : "N/A","Highest memory priority among the resident pages. Not on Linux.");
			ImGui::TableNextColumn();
			kvRow("Shareable working set",util::size(w.shareable),"Pages that could be shared, shared now or not.");
			ImGui::TableNextColumn();
			if (WIN) kvRow("Modified pages",util::size(w.modified),"Mapped and image pages changed in this process, copy on write. These count as private.");
			else kvRow("Anonymous",util::size(w.anon),"Anonymous resident memory from smaps_rollup.");
			ImGui::TableNextColumn();
			kvRow("Shared working set",util::size(w.shared),"Pages mapped by more than one process right now.");
			ImGui::TableNextColumn();
			kvRow("Original pages",util::size(w.original),"Mapped and image pages still as they came from the file. Not on Linux.");
			ImGui::EndTable();
		}
		ui::endCard();
	}

	void drawStats(const St& s,const Det* d) {
		bool inJob = WIN && d && d->jobs.in == 1;
		ui::beginCard("##stat","Statistics");
		ImGui::SameLine();
		ui::help("Min and max start the first time this process's Memory or Limits tab opens. Average and median use the last 512 samples. All of them sample while either tab is on screen and pause when neither is, the graphs above keep going. After 2 minutes off screen they start over.");
		if (ui::beginTable("##stt",7,ImVec2(0.0f,tableH(NROWS + 1)))) {
			ImGui::TableSetupColumn("Property",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoHide,ui::px(166.0f));
			const char* names[] = {"Value","Min","Max","Difference","Average","Median"};
			for (auto n : names) ImGui::TableSetupColumn(n,ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
			ImGui::TableSetupScrollFreeze(0,1);
			headers(7,1);
			for (int i = 0; i < NROWS; i++) {
				const RowDef& rd = ROWS[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				bool jobRow = inJob && i >= RPrivCommit && i <= RTotalLimit;
				ImGui::Text("%s%s",WIN || !rd.lxName ? rd.name : rd.lxName,jobRow ? " (job)" : "");
				ui::tip(rd.tip);
				ImGui::TableNextColumn();
				if (!s.known[i]) ui::cellSize(NA);
				else if (rd.k == KPrio) ui::cellRight(procs::pagePrioName((int)s.val[i]));
				else if (s.noLim[i]) ui::cellRight("No limit");
				else ui::cellRight(fmt(rd.k,s.val[i]).c_str());
				if (rd.k == KPrio || rd.k == KLim) continue;
				const Met& m = s.m[i];
				if (!m.any) continue;
				double diff = m.mx - m.mn;
				ImGui::TableNextColumn();
				ui::cellRight(fmt(rd.k,m.mn).c_str());
				ImGui::TableNextColumn();
				ui::cellRight(fmt(rd.k,m.mx).c_str());
				ImGui::TableNextColumn();
				ui::cellRight(fmt(rd.k == KDl ? KSz : rd.k,diff).c_str());
				ImGui::TableNextColumn();
				ui::cellRight(fmt(rd.k,m.avg).c_str());
				ImGui::TableNextColumn();
				ui::cellRight(fmt(rd.k,m.med).c_str());
			}
			ui::endTable();
		}
		ui::endCard();
	}

	const char* kindLabel(int k) {
		return k == RuleCmd ? "Command line" : k == RuleIfeo ? "IFEO" : "Name";
	}

	void drawRules(const ProcRef& r,St& s,const std::vector<Rule>& rs,bool live) {
		if (live) watch(r,WIfeo);
		std::vector<Rule> all = rs;
		std::vector<Rule> i = ifeoCached();
		all.insert(all.end(),i.begin(),i.end());
		ui::beginCard("##rules",nullptr);
		ui::hint(WIN ? "FRS keeps matching processes at the saved priority while it runs, on its next sample after they start. A command line rule wins over a name. IFEO rules live in the registry and Windows applies them without FRS."
			: "Linux has no page priority. A rule below Normal marks the matching process's pages cold once a minute, so reclaim takes them first. Normal does nothing.");
		if (all.empty()) {
			ImGui::TextDisabled("No saved rules. Pick a priority above and tick Save, or add one here.");
		} else if (ui::beginTable("##rt",4,ImVec2(0.0f,tableH(std::min((int)all.size(),8) + 1)))) {
			ImGui::TableSetupColumn("Kind",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(92.0f));
			ImGui::TableSetupColumn("Match",ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
			ImGui::TableSetupColumn("Page priority",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(92.0f));
			ImGui::TableSetupColumn("##act",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoHide,ui::px(104.0f));
			ImGui::TableSetupScrollFreeze(0,1);
			ImGui::TableHeadersRow();
			ImGuiListClipper clip;
			clip.Begin((int)all.size());
			while (clip.Step()) for (int n = clip.DisplayStart; n < clip.DisplayEnd; n++) {
				const Rule& x = all[n];
				ImGui::PushID(n);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s",kindLabel(x.kind));
				ImGui::TableNextColumn();
				ui::textEllipsis(x.key.c_str());
				ui::tip(x.key.c_str());
				ImGui::TableNextColumn();
				if (x.kind == RuleIfeo && !WIN) ImGui::TextDisabled("N/A");
				else ImGui::TextUnformatted(procs::pagePrioName(x.prio));
				ImGui::TableNextColumn();
				if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText((std::string(kindLabel(x.kind)) + "\t" + x.key + "\t" + procs::pagePrioName(x.prio)).c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton("Delete")) runRule(r,x.kind,x.key,0,false);
				ImGui::PopID();
			}
			ui::endTable();
		}
		const char* kinds[] = {"Name","Command line","IFEO"};
		int nk = ifeoSupported() ? 3 : 2;
		if (s.addKind >= nk) s.addKind = 0;
		ImGui::SetNextItemWidth(ui::px(112.0f));
		ImGui::Combo("##rk",&s.addKind,kinds,nk);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(std::max(ui::px(120.0f),ImGui::GetContentRegionAvail().x - ui::px(224.0f)));
		ImGui::InputTextWithHint("##rm",s.addKind == RuleCmd ? "exact command line" : "program name, like notepad.exe",s.key,sizeof(s.key));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ui::px(112.0f));
		ImGui::Combo("##rp",&s.addPrio,PRIO_NAMES,5);
		ImGui::SameLine();
		if (s.addPrio < 0 || s.addPrio > 4) s.addPrio = 2;
		bool noWrite = s.addKind == RuleIfeo && !ifeoWritable();
		ImGui::BeginDisabled(!s.key[0] || noWrite);
		if (ImGui::Button("Add##ra")) {
			runRule(r,s.addKind,s.key,PRIO[s.addPrio],true);
			s.key[0] = 0;
		}
		ImGui::EndDisabled();
		if (noWrite) ui::tip("IFEO rules are written to the registry, so they need FRS running as admin.");
		ui::endCard();
	}

	void drawThreads(const ProcRef& r,St& s,const Det* d,bool live) {
		if (live) watch(r,WThr);
		ui::beginCard("##thr",nullptr);
		if (!d || !d->thr.read) {
			ImGui::TextDisabled("%s",notYet(s,d,live));
			ui::endCard();
			return;
		}
		const ThrView& t = d->thr;
		if (!t.why.empty()) ui::hint(t.why.c_str());
		if (t.v.empty()) {
			if (t.why.empty()) ImGui::TextDisabled("No threads to show");
			ui::endCard();
			return;
		}
		ImGui::Text("%d threads, %d waiting on the memory manager",(int)t.v.size(),t.memWaits);
		ImGui::SameLine();
		ui::help(WIN ? "A thread's page priority applies to the pages it faults in. Right click a thread to change it. Memory wait shows a thread blocked on a page read, a free page, pool or the virtual memory lock, a sign RAM is short when it stays. Stack is committed of reserved." : "Linux has no thread page priority. Memory wait shows a thread in state D with its wait channel. Stack is known for the main thread only, VmStk of the stack limit.");
		bool canMenu = WIN && live && !s.row.pseudo && !s.row.sys;
		const char* pop = "##thrmenu";
		ImGuiID popId = ImGui::GetID(pop); // the rows sit deeper in the id stack than BeginPopup below
		bool popOpen = ImGui::IsPopupOpen(pop);
		if (ui::beginTable("##tt",4,ImVec2(0.0f,tableH(std::min((int)t.v.size(),12) + 1)))) {
			ImGui::TableSetupColumn("TID",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(64.0f));
			ImGui::TableSetupColumn("Page priority",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(100.0f));
			ImGui::TableSetupColumn("Memory wait",ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
			ImGui::TableSetupColumn("Stack",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(200.0f));
			ImGui::TableSetupScrollFreeze(0,1);
			headers(4,3);
			ImGuiListClipper clip;
			clip.Begin((int)t.v.size());
			while (clip.Step()) for (int n = clip.DisplayStart; n < clip.DisplayEnd; n++) {
				const Thr& x = t.v[n];
				ImGui::PushID((int)x.tid);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				char b[32];
				snprintf(b,sizeof(b),"%u",x.tid);
				ImGui::Selectable(b,popOpen && s.menuTid == x.tid,ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
				if (canMenu && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
					s.menuTid = x.tid;
					s.menuStart = x.start;
					s.menuPrio = x.prio;
					ImGui::OpenPopup(popId);
				}
				ImGui::TableNextColumn();
				if (x.prio >= 0) ImGui::TextUnformatted(procs::pagePrioName(x.prio));
				else ImGui::TextDisabled("N/A");
				ImGui::TableNextColumn();
				if (!x.wait.empty()) ImGui::TextColored(ui::c.warn,"%s",x.wait.c_str());
				ImGui::TableNextColumn();
				std::string st = "N/A";
				if (x.stackUsed != NA) {
					st = util::size(x.stackUsed);
					if (x.stackLimit != NA && x.stackLimit) st += " of " + util::size(x.stackLimit) + " (" + util::pct(x.stackUsed,x.stackLimit) + ")";
				}
				if (st == "N/A") ui::cellSize(NA);
				else ui::cellRight(st.c_str());
				ImGui::PopID();
			}
			ui::endTable();
		}
		if (ImGui::BeginPopup(pop)) {
			ImGui::TextDisabled("Thread %u",s.menuTid);
			ImGui::Separator();
			for (int i = 4; i >= 0; i--) {
				if (ImGui::MenuItem(PRIO_NAMES[i],nullptr,s.menuPrio == PRIO[i])) {
					uint32_t tid = s.menuTid;
					uint64_t ts = s.menuStart;
					int v = PRIO[i];
					task::run([r,tid,ts,v](task::T&) -> std::string {
						std::string m = setThrPrio(r,tid,ts,v,nullptr);
						ui::toast(m);
						return m;
					});
				}
			}
			ImGui::EndPopup();
		}
		ui::endCard();
	}

	void drawMemory(const ProcRef& r) {
		St& s = stFor(r);
		bool live = procs::alive(r);
		if (live) watch(r,WWs | WIfeo);
		if (auto dp = det(r)) s.det = dp;
		const Det* d = s.det.get();
		update(r,s,d);
		if (!s.hasRow) {
			ui::empty("This process is gone and FRS has nothing recorded for it.");
			return;
		}
		std::vector<Rule> rs = rules();
		ImGui::BeginChild("##memtab",ImVec2(0.0f,0.0f));
		drawGraphs(s);
		drawActions(r,s,d,live,rs);
		drawWs(s,d,live);
		drawStats(s,d);
		if (ui::fold("Page priority rules##prf",!rs.empty())) drawRules(r,s,rs,live);
		if (ui::fold("Threads##pth",false)) drawThreads(r,s,d,live);
		ImGui::EndChild();
	}

	std::string yesNo(int v,const char* yes,const char* no) {
		return v < 0 ? "N/A" : v ? yes : no;
	}

	void drawJob(int idx,const Job& j) {
		std::string id = "##job" + std::to_string(idx);
		std::string title = j.name.empty() ? (WIN ? "Unnamed job" : "Cgroup") : j.name;
		ui::beginCard(id.c_str(),nullptr);
		ui::pushBold();
		ui::textEllipsis(title.c_str());
		ui::popFont();
		ui::tip(title.c_str());
		if (!j.via.empty()) {
			std::string v = "Found through : " + j.via;
			ImGui::TextDisabled("%s",v.c_str());
		}
		if (!j.why.empty()) ui::hint(j.why.c_str());
		std::string tb = "##jk" + std::to_string(idx);
		if (ImGui::BeginTable(tb.c_str(),2,ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
			ImGui::TableNextColumn();
			kvRow(WIN ? "Process memory limit" : "Max address space",limText(j.procMem),WIN ? "Commit limit per process in the job. A trim does not help a process near it, the limit counts commit, not working set." : "RLIMIT_AS of this process, address space, not commit.");
			ImGui::TableNextColumn();
			kvRow(WIN ? "Job memory limit" : "memory.max",limText(j.jobMem),WIN ? "Commit limit for the whole job." : "Hard limit of the cgroup.");
			ImGui::TableNextColumn();
			kvRow(WIN ? "Working set minimum" : "memory.min",limText(j.wsMin),WIN ? "A process in such a job may refuse a trim or regain it fast, this can explain a trim that did nothing." : "Protected amount of the cgroup.");
			ImGui::TableNextColumn();
			kvRow(WIN ? "Working set maximum" : "memory.high",limText(j.wsMax),WIN ? "Job wide working set ceiling." : "Throttle limit of the cgroup.");
			ImGui::TableNextColumn();
			kvRow(WIN ? "Peak process usage" : "Peak process",util::size(j.peakProc),"Highest commit of any member. Not on Linux.");
			ImGui::TableNextColumn();
			kvRow(WIN ? "Peak job usage" : "memory.peak",util::size(j.peakJob),WIN ? "Highest commit of the whole job." : "Linux 5.19 or newer.");
			ImGui::TableNextColumn();
			kvRow("Job page faults",util::count(j.faults),WIN ? "Faults of all members so far, includes those that exited." : "pgfault in memory.stat.");
			ImGui::TableNextColumn();
			kvRow("Active processes",util::count(j.active),"Not on Linux.");
			ImGui::TableNextColumn();
			kvRow("Total processes",util::count(j.total),"Not on Linux.");
			ImGui::TableNextColumn();
			kvRow("Terminated processes",util::count(j.term),"Not on Linux.");
			ImGui::EndTable();
		}
		ui::endCard();
	}

	void drawLimits(const ProcRef& r) {
		St& s = stFor(r);
		bool live = procs::alive(r);
		if (live) watch(r,WJobs);
		if (auto dp = det(r)) s.det = dp;
		const Det* d = s.det.get();
		update(r,s,d);
		if (!s.hasRow) {
			ui::empty("This process is gone and FRS has nothing recorded for it.");
			return;
		}
		const ProcRow& o = s.row;
		ImGui::BeginChild("##limtab",ImVec2(0.0f,0.0f));

		ui::beginCard("##jobc",WIN ? "Job" : "Cgroup");
		const JobView* j = d ? &d->jobs : nullptr;
		int in = j ? j->in : -1;
		if (ImGui::BeginTable("##ji",2,ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
			ImGui::TableNextColumn();
			kvRow(WIN ? "In a job" : "In a cgroup",yesNo(in,"Yes","No"));
			ImGui::TableNextColumn();
			if (WIN) kvRow("Job ID",o.jobId == NA ? "N/A" : o.jobId ? std::to_string(o.jobId) : "None","Kernel id of the innermost job, from the process snapshot.");
			else kvRow("Cgroup inode",o.jobId == NA ? "N/A" : std::to_string(o.jobId));
			ImGui::EndTable();
		}
		if (!d || !j || !j->read) {
			if (in != 0 || o.pseudo) ImGui::TextDisabled("%s",notYet(s,d,live));
		} else if (in == 0) {
			ui::hint(WIN ? "This process is not in a job, so no job limit applies to it." : "No cgroup to read.");
		} else if (j->jobs.empty()) {
			if (in == 1) {
				// wraps short of the (?) so a narrow window doesn't clip either
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("(?)").x - ImGui::GetStyle().ItemSpacing.x);
				ImGui::TextDisabled("FRS can't open this job. Job memory limit and peak come from the process, the other limits are unknown.");
				ImGui::PopTextWrapPos();
				ImGui::SameLine();
				std::string t = "The other rows need a job handle and stay N/A. System Informer opens jobs with its kernel driver, FRS has no driver. " + j->why;
				ui::help(t.c_str());
			} else if (!j->why.empty()) ui::hint(j->why.c_str());
			if (WIN && in == 1 && ImGui::BeginTable("##jn",2,ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
				const char* fromProc = "Read from the process, this one needs no job handle.";
				ImGui::TableNextColumn();
				kvRow("Job memory limit",limText(d->commit.privLimit),fromProc);
				ImGui::TableNextColumn();
				kvRow("Peak job usage",util::size(d->commit.peak),fromProc);
				const char* names[] = {"Process memory limit","Peak process usage","Working set minimum","Working set maximum","Job page faults","Active processes"};
				for (auto n : names) {
					ImGui::TableNextColumn();
					kvRow(n,"N/A");
				}
				ImGui::EndTable();
			}
		} else if (!j->why.empty()) ui::hint(j->why.c_str());
		ui::endCard();
		if (j) for (size_t i = 0; i < j->jobs.size(); i++) drawJob((int)i,j->jobs[i]);

		ui::beginCard("##cmc",WIN ? (in == 1 ? "Commit of the job" : "Commit") : "Cgroup memory");
		ImGui::SameLine();
		ui::help(WIN ? "Asked of the process, no job handle needed. For a process in a job Windows answers for the whole job, so this can be far above the process's own private bytes. Private commit limit is then the job memory limit." : "memory.* files of the cgroup, everything in that cgroup together, not this process alone.");
		if (d && !d->commit.why.empty()) ui::hint(d->commit.why.c_str());
		if (ImGui::BeginTable("##cmk",2,ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
			const Commit c = d ? d->commit : Commit();
			ImGui::TableNextColumn();
			kvRow(WIN ? "Private commit" : "memory.current",util::size(c.priv));
			ImGui::TableNextColumn();
			kvRow(WIN ? "Private commit limit" : "memory.max",limText(c.privLimit));
			ImGui::TableNextColumn();
			kvRow(WIN ? "Peak private commit" : "memory.peak",util::size(c.peak));
			ImGui::TableNextColumn();
			kvRow(WIN ? "Total commit limit" : "memory.swap.max",limText(c.totalLimit));
			ImGui::TableNextColumn();
			kvRow(WIN ? "Shared commit" : "shmem",util::size(c.shared),WIN ? nullptr : "shmem in memory.stat : tmpfs, shm and shared anonymous memory of the whole cgroup.");
			ImGui::TableNextColumn();
			if (WIN) kvRow("Page combining",yesNo(d ? d->combine : -1,"Restricted","Allowed"),"This process opted out of memory combining, its pages are skipped by Combine memory pages.");
			else kvRow("Merged by KSM",util::size(d ? d->ksm : NA),"Pages merged by KSM, from ksm_stat, Linux 5.19 or newer.");
			ImGui::EndTable();
		}
		ui::endCard();

		ui::beginCard("##qc",WIN ? "Quota" : "Pool and quota");
		const Quota q = d ? d->quota : Quota();
		if (!q.why.empty()) ui::hint(q.why.c_str());
		if (ImGui::BeginTable("##qk",2,ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
			ImGui::TableNextColumn();
			kvRow("Working set minimum",util::size(q.minWs != NA ? q.minWs : o.minWs),"Soft floor Windows tries to keep. Not on Linux.");
			ImGui::TableNextColumn();
			kvRow("Working set maximum",util::size(q.maxWs != NA ? q.maxWs : o.maxWs),"Soft ceiling, a hint unless the hard flag is on. Not on Linux.");
			ImGui::TableNextColumn();
			kvRow("Hard minimum",yesNo(q.hardMin,"Yes","No"));
			ImGui::TableNextColumn();
			kvRow("Hard maximum",yesNo(q.hardMax,"Yes","No"));
			ImGui::TableNextColumn();
			kvRow("Paged pool",util::size(o.paged),"Paged pool charged now. Not on Linux.");
			ImGui::TableNextColumn();
			kvRow("Paged pool limit",limText(q.paged));
			ImGui::TableNextColumn();
			kvRow("Non-paged pool",util::size(o.nonPaged),"Non-paged pool charged now. Not on Linux.");
			ImGui::TableNextColumn();
			kvRow("Non-paged pool limit",limText(q.nonPaged));
			ImGui::TableNextColumn();
			kvRow("Page file",util::size(WIN ? o.priv : NA),"Page file charge, the same number as private bytes. Not on Linux.");
			ImGui::TableNextColumn();
			kvRow("Page file limit",limText(q.pagefile));
			ImGui::EndTable();
		}
		uint64_t qmn = WIN ? pick2(q.minWs,o.minWs) : NA;
		uint64_t qmx = WIN ? pick2(q.maxWs,o.maxWs) : NA;
		bool qOk = !WIN || (qmn != NA && qmx != NA);
		bool can = live && !o.pseudo && !sysLike(o);
		if (d && !s.qFill) {
			s.qMin = qmn == NA ? 0.0 : (double)qmn / 1048576.0;
			s.qMax = qmx == NA ? 0.0 : (double)qmx / 1048576.0;
			s.qFill = qOk;
		}
		ImGui::Spacing();
		if (!qOk) {
			ImGui::TextDisabled("Set working set quota : %s",!d ? notYet(s,d,live) : "FRS can't read this process's quota, so it can't set one either");
			ui::endCard();
			ImGui::EndChild();
			return;
		}
		ImGui::BeginDisabled(!can);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled(WIN ? "Set working set quota" : "Set cgroup memory.min and memory.high");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ui::px(92.0f));
		ImGui::InputDouble("Min MB##qm",&s.qMin,0.0,0.0,"%.2f");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ui::px(92.0f));
		ImGui::InputDouble("Max MB##qx",&s.qMax,0.0,0.0,"%.2f");
		ImGui::SameLine();
		std::string ask = "Change working set quota ?##q" + std::to_string(r.pid) + "_" + std::to_string(r.start);
		if (ImGui::Button("Apply##qa")) ui::askOpen(ask.c_str());
		ImGui::EndDisabled();
		ui::tip(WIN ? "Soft limits, Windows may go past them. They reset when the process exits. Needs a privilege FRS only has as admin when raising the minimum." : "This changes the whole cgroup, not just this process, and needs root.");
		if (ui::askDraw(ask.c_str(),WIN ? "Set this process's soft working set minimum and maximum? Windows may trim it to the new maximum." : "Set memory.min and memory.high of this process's cgroup? This changes everything in that cgroup.","Apply")) {
			double a = s.qMin;
			double b = s.qMax;
			task::run([r,a,b](task::T&) -> std::string {
				std::string m = a < 0.0 || b <= 0.0 ? "Working set quota (pid " + std::to_string(r.pid) + ") : failed (give sizes above 0)" : setQuota(r,(uint64_t)(a * 1048576.0),(uint64_t)(b * 1048576.0));
				ui::toast(m);
				return m;
			});
		}
		ui::endCard();

		ImGui::EndChild();
	}

}

namespace procdet {

	void initTabs() {
		procwin::addTab("Memory",10,drawMemory);
		procwin::addTab("Limits",20,drawLimits);
	}

	void saveMenu(const ProcRef& r) {
		auto l = procs::latest();
		const ProcRow* o = l->find(r);
		if (!o || o->pseudo || sysLike(*o)) return;
		watch(r,WIfeo);
		auto d = det(r);
		St& s = stFor(r);
		std::vector<Rule> rs = rules();
		int prio = WIN ? o->pagePrio : PRIO[s.pick];
		bool known = prio >= 1 && prio <= 5;
		std::string nm = lblName(o->name);
		bool on = hasRule(rs,RuleName,o->name);
		if (ImGui::MenuItem(("Save for " + nm).c_str(),nullptr,on,on || known)) runRule(r,RuleName,o->name,prio,!on);
		std::string cmd = d ? d->cmd : "";
		on = !cmd.empty() && hasRule(rs,RuleCmd,cmd);
		if (ImGui::MenuItem("Save for this command line",nullptr,on,!cmd.empty() && (on || known))) runRule(r,RuleCmd,cmd,prio,!on);
		if (!ifeoSupported()) return;
		// the dialog can't outlive this popup, so it opens with the process window
		if (ImGui::MenuItem(("Save for " + nm + " (IFEO)...").c_str(),nullptr,d && d->ifeo >= 0,ifeoWritable())) {
			s.ifeoAsk = true;
			procwin::open(r);
		}
	}

}
