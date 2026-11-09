#include "vmdiag.h"
#include "cfg.h"
#include "domains.h"
#include "ui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <set>

namespace vmdiag {

	namespace {
		std::mutex g_bm;
		std::set<uint32_t> g_busy;
	}

	bool claim(uint32_t pid) {
		std::lock_guard<std::mutex> lk(g_bm);
		return g_busy.insert(pid).second;
	}

	void unclaim(uint32_t pid) {
		std::lock_guard<std::mutex> lk(g_bm);
		g_busy.erase(pid);
	}

	std::string symAt(const SymTab& t,uint64_t addr) {
		auto it = std::upper_bound(t.begin(),t.end(),addr,[](uint64_t a,const SymMod& m) { return a < m.base; });
		if (it == t.begin()) return "";
		--it;
		if (addr - it->base >= it->size) return "";
		uint32_t rva = (uint32_t)(addr - it->base);
		auto e = std::upper_bound(it->exps.begin(),it->exps.end(),rva,[](uint32_t a,const std::pair<uint32_t,std::string>& x) { return a < x.first; });
		char b[32];
		if (e == it->exps.begin()) {
			snprintf(b,sizeof(b),"+0x%x",rva);
			return it->name + b;
		}
		--e;
		if (e->first == rva) return it->name + "!" + e->second;
		// that far from an export the name says nothing, it's data or a private function
		if (rva - e->first >= 0x10000) {
			snprintf(b,sizeof(b),"+0x%x",rva);
			return it->name + b;
		}
		snprintf(b,sizeof(b),"+0x%x",rva - e->first);
		return it->name + "!" + e->second + b;
	}

	const char* cfgTypeName(int t) {
		switch (t) {
		case CfgAllValid: return "All valid (no CFG)";
		case CfgImage: return "Image CFG";
		case CfgModified: return "Modified/allowed";
		case CfgSuppressed: return "Export suppressed";
		case CfgUnmapped: return "Not mapped here";
		}
		return "";
	}

	// the dispatcher in ntdll wants one bit on a 16 byte boundary and both bits anywhere else
	const char* cfgMeaning(const CfgCheck& k,uint64_t addr) {
		if (k.noBitmap) return "this process has no CFG bitmap";
		if ((addr & 15) && k.val == 1) return "not a valid target, only the aligned start of this slot is";
		if ((addr & 15) && k.val == 2) return "not a valid target, the aligned start of this slot is export suppressed";
		switch (k.val) {
		case 0: return "not a valid target";
		case 1: return "valid target";
		case 2: return "export suppressed";
		case 3: return "valid target, also unaligned";
		}
		return "no target, the bitmap page behind it is no access or not committed";
	}

}

namespace {

	using namespace vmdiag;

	// one background query and its last finished result
	template<class R> struct Job {
		std::shared_ptr<task::T> t;
		std::shared_ptr<std::shared_ptr<const R>> box;
		std::shared_ptr<const R> res;
		std::string err;
		uint64_t t0 = 0;
		bool started = false;

		bool running() const { return t != nullptr; }

		// false when no thread could be started, fn then never runs
		bool start(std::function<std::shared_ptr<const R>(std::atomic<bool>&)> fn,std::function<std::string(const R&)> log = nullptr) {
			auto b = std::make_shared<std::shared_ptr<const R>>();
			auto ran = std::make_shared<std::atomic<bool>>(false);
			box = b;
			started = true;
			t0 = util::nowMs();
			t = task::run([b,fn,log,ran](task::T& tk) {
				*ran = true;
				*b = fn(tk.cancel);
				return log && *b ? log(**b) : std::string();
			},log != nullptr);
			return !t->done || *ran;
		}

		void poll() {
			if (!t || !t->done) return;
			if (*box) {
				res = *box;
				err.clear();
			} else err = t->res;
			t.reset();
			box.reset();
		}

		// nobody looked for a while : what came in meanwhile counts, then a view that only reads may go again
		void stale() {
			poll();
			if (!running()) started = false;
		}
	};

	// fl bits for a column
	const int FR = 1; // right aligned
	const int FH = 2; // off until the user turns it on
	const int FN = 4; // can't sort on it
	const int FM = 8; // address, mono font

	struct ColDef {
		const char* name;
		float w;
		int fl;
	};

	struct List {
		std::vector<int> order;
		std::vector<char> sel;
		const void* src = nullptr;
		int last = -1,sortCol = -1,ctxCol = -1;
		bool desc = false,dirty = true;
	};

	using CellFn = std::function<std::string(int,int)>;
	using KeyFn = std::function<bool(int,int,uint64_t&)>;
	using VisFn = std::function<bool(int)>;

	void rebuild(List& l,int n,const CellFn& cell,const KeyFn& key,const VisFn& vis) {
		l.order.clear();
		for (int i = 0; i < n; i++) if (!vis || vis(i)) l.order.push_back(i);
		if (l.sortCol >= 0) {
			int c = l.sortCol;
			bool d = l.desc;
			std::stable_sort(l.order.begin(),l.order.end(),[&](int a,int b) {
				uint64_t x = 0,y = 0;
				if (key && key(a,c,x) && key(b,c,y)) return d ? y < x : x < y;
				std::string s = util::lower(cell(a,c));
				std::string u = util::lower(cell(b,c));
				return d ? u < s : s < u;
			});
		}
		l.last = -1;
		l.dirty = false;
	}

	// on holds the columns the user has enabled. Read inside the table, a popup is a window of its own with no current table
	void copyRows(const List& l,const std::vector<char>& on,const CellFn& cell) {
		std::string s;
		for (int r : l.order) if (l.sel[r]) {
			std::string line;
			for (size_t c = 0; c < on.size(); c++) if (on[c]) {
				if (!line.empty()) line += '\t';
				line += cell(r,(int)c);
			}
			s += line + "\n";
		}
		if (!s.empty()) ImGui::SetClipboardText(s.c_str());
	}

	void copyCol(const List& l,int col,const CellFn& cell) {
		std::string s;
		for (int r : l.order) if (l.sel[r]) s += cell(r,col) + "\n";
		if (!s.empty()) ImGui::SetClipboardText(s.c_str());
	}

	// cut with ... at the cell edge, full text as a tip. The item keeps the text's width so Size to fit still sees all of it
	void cellText(const std::string& t) {
		const char* end = t.c_str() + t.size();
		float w = ImGui::GetContentRegionAvail().x;
		ImVec2 ts = ImGui::CalcTextSize(t.c_str(),end);
		if (ts.x <= w) {
			ImGui::TextUnformatted(t.c_str(),end);
			return;
		}
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(),p,ImVec2(p.x + w,p.y + ts.y),p.x + w,t.c_str(),end,&ts);
		ImGui::Dummy(ts);
		ui::tip(t.c_str());
	}

	// sort, select, copy and a clipper around plain text cells. Same table id in every process window, so the
	// layout is one entry in ui.ini and not one per process.
	void drawList(const char* id,const ColDef* cols,int nc,List& l,const void* src,int n,const CellFn& cell,const KeyFn& key,const VisFn& vis,const std::function<bool(int)>& bold) {
		if (l.src != src) {
			l.src = src;
			l.sel.assign((size_t)n,0);
			l.dirty = true;
		}
		ImGuiTableFlags tf = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate |
			ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg;
		if (!ImGui::BeginTableEx(id,ImHashStr(id),nc,tf,ImVec2(0,0))) return;
		float arrow = std::floor(ImGui::GetFontSize() * 0.65f + ImGui::GetStyle().FramePadding.x);
		for (int c = 0; c < nc; c++) {
			ImGuiTableColumnFlags f = ImGuiTableColumnFlags_WidthFixed;
			if (c == 0) f |= ImGuiTableColumnFlags_NoHide;
			if (cols[c].fl & FH) f |= ImGuiTableColumnFlags_DefaultHide;
			if (cols[c].fl & FN) f |= ImGuiTableColumnFlags_NoSort;
			ImGui::TableSetupColumn(cols[c].name,f,std::max(ui::px(cols[c].w),ImGui::CalcTextSize(cols[c].name).x + arrow));
		}
		ImGui::TableSetupScrollFreeze(0,1);
		// the table is shared between windows, so SpecsDirty only reaches the first one
		if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
			int col = sp->SpecsCount > 0 ? sp->Specs[0].ColumnIndex : -1;
			bool d = sp->SpecsCount > 0 && sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
			if (col != l.sortCol || d != l.desc) {
				l.sortCol = col;
				l.desc = d;
				l.dirty = true;
			}
			sp->SpecsDirty = false;
		}
		if (l.dirty) rebuild(l,n,cell,key,vis);
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int c = 0; c < nc; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			ImGui::PushID(c);
			ui::headerCell(cols[c].name,nullptr,(cols[c].fl & FR) != 0);
			ImGui::PopID();
		}
		ImVec4 cSel = ui::c.accent;
		cSel.w = 0.22f;
		ImGui::PushStyleColor(ImGuiCol_Header,cSel);
		bool ctx = false;
		ImGuiListClipper clip;
		clip.Begin((int)l.order.size());
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
				int row = l.order[i];
				ImGui::TableNextRow();
				ImGui::PushID(row);
				bool b = bold && bold(row);
				if (b) ui::pushBold();
				// it spans the row from any column, and the first ones can be scrolled away
				bool hasSel = false;
				for (int c = 0; c < nc; c++) {
					if (!ImGui::TableSetColumnIndex(c)) continue;
					if (!hasSel) {
						hasSel = true;
						ImVec2 p = ImGui::GetCursorPos();
						if (ImGui::Selectable("##r",l.sel[row] != 0,ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
							ImGuiIO& io = ImGui::GetIO();
							if (io.KeyShift && l.last >= 0 && l.last < (int)l.order.size()) {
								std::fill(l.sel.begin(),l.sel.end(),(char)0);
								for (int k = std::min(l.last,i); k <= std::max(l.last,i); k++) l.sel[l.order[k]] = 1;
							} else if (io.KeyCtrl) {
								l.sel[row] = !l.sel[row];
								l.last = i;
							} else {
								std::fill(l.sel.begin(),l.sel.end(),(char)0);
								l.sel[row] = 1;
								l.last = i;
							}
						}
						if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
							if (!l.sel[row]) {
								std::fill(l.sel.begin(),l.sel.end(),(char)0);
								l.sel[row] = 1;
								l.last = i;
							}
							l.ctxCol = ImGui::TableGetHoveredColumn();
							ctx = true;
						}
						ImGui::SetCursorPos(p);
					}
					std::string t = cell(row,c);
					bool na = t == "N/A";
					bool mono = (cols[c].fl & FM) != 0;
					if (na) ImGui::PushStyleColor(ImGuiCol_Text,ui::c.dim);
					if (mono) ui::pushMonoCell();
					if (cols[c].fl & FR) ui::cellRight(t.c_str());
					else cellText(t);
					if (mono) ui::popFont();
					if (na) ImGui::PopStyleColor();
				}
				if (b) ui::popFont();
				ImGui::PopID();
			}
		}
		ImGui::PopStyleColor();
		std::vector<char> on((size_t)nc);
		for (int c = 0; c < nc; c++) on[c] = (ImGui::TableGetColumnFlags(c) & ImGuiTableColumnFlags_IsEnabled) != 0;
		if (ctx) ImGui::OpenPopup("##lctx");
		if (ImGui::BeginPopup("##lctx")) {
			if (ImGui::MenuItem("Copy","Ctrl+C")) copyRows(l,on,cell);
			if (l.ctxCol >= 0 && l.ctxCol < nc) {
				std::string lab = std::string("Copy \"") + cols[l.ctxCol].name + "\"";
				if (ImGui::MenuItem(lab.c_str())) copyCol(l,l.ctxCol,cell);
			}
			ImGui::EndPopup();
		}
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) copyRows(l,on,cell);
		ImGui::EndTable();
	}

	struct PS {
		ProcRef r;
		uint64_t seen = 0;
		Job<HeapRes> heaps;
		Job<LockRes> locks;
		Job<ModRes> mod;
		Job<CfgRes> cfgJ;
		Job<GdiRes> gdi;
		std::shared_ptr<task::T> flushT;
		std::shared_ptr<FlushRes> flushOut;
		std::string flushMsg;
		bool flushBad = false;
		std::string askId;
		List heapL,lockL,modL,cfgL,gdiL;
		bool showImage = false,showMod = true,showAll = false,showUnm = false;
		char addr[32] = "";
		std::string addrTxt;
		bool addrBad = false;
	};

	std::map<std::pair<uint32_t,uint64_t>,std::shared_ptr<PS>> g_ps;

	PS& stateOf(const ProcRef& r) {
		auto key = std::make_pair(r.pid,r.start);
		auto it = g_ps.find(key);
		if (it == g_ps.end()) {
			// a result can be big, keep the last few processes only. A running job holds what it needs by itself
			if (g_ps.size() >= 12) {
				auto old = g_ps.begin();
				for (auto i = g_ps.begin(); i != g_ps.end(); ++i) if (i->second->seen < old->second->seen) old = i;
				g_ps.erase(old);
			}
			auto s = std::make_shared<PS>();
			s->r = r;
			s->askId = "Flush heaps?##" + std::to_string(r.pid) + "_" + std::to_string(r.start);
			it = g_ps.emplace(key,s).first;
		}
		PS& s = *it->second;
		uint64_t now = util::nowMs();
		// not drawn for a while : the window or the tab was closed and came back, so what it holds is old. An exited process keeps what was last known
		if (s.seen && now - s.seen > 2000 && procs::alive(s.r)) {
			s.heaps.stale();
			s.locks.stale();
			s.mod.stale();
			s.cfgJ.stale();
			s.gdi.stale();
			s.flushMsg.clear();
			s.addrTxt.clear();
		}
		s.seen = now;
		return s;
	}

	std::string pidTxt(const ProcRef& r) {
		return "(pid " + std::to_string(r.pid) + ")";
	}

	std::string readAt(const Res& x) {
		uint64_t now = (uint64_t)time(nullptr);
		uint64_t s = now > x.unixSec ? now - x.unixSec : 0;
		return s < 120 ? "read " + std::to_string(s) + " s ago" : "read " + std::to_string(s / 60) + " min ago";
	}

	std::string doneLine(const char* what,const ProcRef& r,const Res& x) {
		if (x.ok) return std::string(what) + " " + pidTxt(r) + " : OK";
		if (x.na) return std::string(what) + " " + pidTxt(r) + " : not available (" + x.why + ")";
		return std::string(what) + " " + pidTxt(r) + " : failed (" + x.why + ")";
	}

	void warnCard(const char* id,const char* txt) {
		ui::beginCard(id,nullptr);
		float sz = ImGui::GetFontSize();
		ui::icon(ui::IcoWarn,ImGui::GetCursorScreenPos(),sz,ImGui::GetColorU32(ui::c.warn));
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + sz + ui::px(6.0f));
		ImGui::PushStyleColor(ImGuiCol_Text,ui::c.warn);
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(txt);
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ui::endCard();
	}

	bool sysProc(const ProcRef& r) {
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		return row && (row->sys || row->pseudo);
	}

	// empty state or error for a job with no rows to show. true when the caller should stop.
	template<class R> bool jobState(const Job<R>& j,const char* idle,const char* wait) {
		if (j.running()) {
			uint64_t sec = (util::nowMs() - j.t0) / 1000;
			std::string s = wait;
			if (sec >= 15) s += " " + std::to_string(sec) + " s. The target may be frozen or deadlocked, this can't be cancelled.";
			// the rows of the last query stay up while the next one runs
			if (j.res && j.res->ok) {
				ImGui::TextDisabled("%s",s.c_str());
				return false;
			}
			ui::empty(s.c_str());
			return true;
		}
		if (!j.err.empty()) {
			ui::empty(j.err.c_str());
			return true;
		}
		if (!j.res) {
			ui::empty(idle);
			return true;
		}
		if (!j.res->ok) {
			std::string s = j.res->na ? j.res->why : "Failed : " + j.res->why;
			ui::empty(s.c_str());
			return true;
		}
		return false;
	}

	void startHeaps(PS& s) {
		if (!claim(s.r.pid)) {
			ui::toast("Another query is still running in this process");
			return;
		}
		// a failed flush is over once the process takes queries again
		if (s.flushBad) s.flushMsg.clear();
		ProcRef r = s.r;
		bool clone = cfg::getb("diagHeapClone",false);
		bool ok = s.heaps.start([r,clone](std::atomic<bool>&) {
			std::shared_ptr<const HeapRes> o;
			try {
				o = be::heaps(r,clone);
			} catch (...) {
				unclaim(r.pid);
				throw;
			}
			unclaim(r.pid);
			return o;
		},[r](const HeapRes& x) {
			std::string l = doneLine("Heaps query",r,x);
			ui::toast(l);
			return l;
		});
		if (!ok) unclaim(r.pid);
	}

	void startLocks(PS& s) {
		if (!claim(s.r.pid)) {
			ui::toast("Another query is still running in this process");
			return;
		}
		ProcRef r = s.r;
		bool ok = s.locks.start([r](std::atomic<bool>&) {
			std::shared_ptr<const LockRes> o;
			try {
				o = be::locks(r);
			} catch (...) {
				unclaim(r.pid);
				throw;
			}
			unclaim(r.pid);
			return o;
		},[r](const LockRes& x) {
			std::string l = doneLine("Locks query",r,x);
			ui::toast(l);
			return l;
		});
		if (!ok) unclaim(r.pid);
	}

	void startFlush(PS& s) {
		if (!claim(s.r.pid)) {
			ui::toast("Another query is still running in this process");
			return;
		}
		ProcRef r = s.r;
		auto out = std::make_shared<FlushRes>();
		s.flushOut = out;
		s.flushMsg.clear();
		auto ran = std::make_shared<std::atomic<bool>>(false);
		s.flushT = task::run([r,out,ran](task::T&) {
			*ran = true;
			try {
				*out = be::flush(r);
			} catch (...) {
				unclaim(r.pid);
				throw;
			}
			if (!out->held) unclaim(r.pid);
			std::string l = out->ok ? "Flush heaps " + pidTxt(r) + " : OK" : "Flush heaps " + pidTxt(r) + " : failed (" + out->why + ")";
			ui::toast(l);
			return l;
		});
		// done without having run : no thread could be started, nothing else lets the process go
		if (s.flushT->done && !*ran) unclaim(r.pid);
	}

	void pollFlush(PS& s) {
		if (!s.flushT || !s.flushT->done) return;
		const FlushRes& o = *s.flushOut;
		s.flushBad = !o.ok;
		if (o.ok) s.flushMsg = "Flushed. Private " + util::size(o.privBefore) + " -> " + util::size(o.privAfter) + ", working set " + util::size(o.wsBefore) + " -> " + util::size(o.wsAfter) + ". Query again to see the heaps.";
		else if (!o.why.empty()) s.flushMsg = "Flush failed : " + o.why;
		else s.flushMsg = s.flushT->res;
		s.flushT.reset();
		s.flushOut.reset();
	}

	std::string sizeOr(uint64_t v,bool bytes) {
		if (v == NA) return "N/A";
		return bytes ? util::count(v) : util::size(v);
	}

	const ColDef kHeapCols[] = {
		{"#",30,FR},
		{"Address",124,FM},
		{"Used",76,FR},
		{"Committed",76,FR},
		{"Entries",56,FR},
		{"Flags",150,0},
		{"Class",100,0},
		{"Type",110,0}
	};

	void tabHeaps(const ProcRef& r) {
		PS& s = stateOf(r);
		s.heaps.poll();
		pollFlush(s);
		if (sysProc(r)) {
			ui::empty("System processes have no user heaps.");
			return;
		}
		bool rem = be::remote();
		if (!rem && !s.heaps.started) {
			s.heaps.started = true;
			startHeaps(s);
		}
		if (rem) warnCard("##hnote","Query heaps and Flush heaps start a thread inside this process. In rare cases that stalls or deadlocks it, a flush can also crash it, and anti-cheat or EDR may react. Nothing runs until you press a button.");
		ui::beginCard("##hbar",nullptr);
		bool busy = s.heaps.running() || s.flushT;
		if (busy) ImGui::BeginDisabled();
		if (rem ? ImGui::Button("Query heaps") : ui::iconBtn("dhref",ui::IcoRefresh,"Read the heaps again")) startHeaps(s);
		if (rem) {
			ImGui::SameLine();
			ui::pushBtnColor(ui::c.bad);
			if (ImGui::Button("Flush heaps...")) ui::askOpen(s.askId.c_str());
			ui::popBtnColor();
		}
		if (busy) ImGui::EndDisabled();
		// a safety choice, so not an option : set_settings must not be able to turn it off
		if (rem) {
			ImGui::SameLine();
			bool cl = cfg::getb("diagHeapClone",false);
			if (ImGui::Checkbox("Query on a clone",&cl)) cfg::setb("diagHeapClone",cl);
			ui::tip("The heap query starts a thread in the target. This makes it run in a throwaway clone of the process instead, for the UI and MCP. Safer, a little slower, needs more rights. Only this checkbox changes it, MCP can't.");
		}
		ImGui::SameLine();
		bool inBytes = cfg::getb("diagSizesBytes",false);
		if (ImGui::Checkbox("Sizes in bytes",&inBytes)) cfg::setb("diagSizesBytes",inBytes);
		if (s.heaps.res && s.heaps.res->ok) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s",readAt(*s.heaps.res).c_str());
		}
		if (s.heaps.res && s.heaps.res->ok && !s.heaps.res->rows.empty()) {
			uint64_t used = 0,comm = 0,ents = 0;
			bool haveUsed = false,haveEnts = false;
			for (auto& h : s.heaps.res->rows) {
				if (h.used != NA) {
					used += h.used;
					haveUsed = true;
				}
				if (h.committed != NA) comm += h.committed;
				if (h.entries != NA) {
					ents += h.entries;
					haveEnts = true;
				}
			}
			ImGui::Spacing();
			float gap = ui::px(28.0f);
			ui::stat("Heaps",std::to_string(s.heaps.res->rows.size()));
			ImGui::SameLine(0.0f,gap);
			ui::stat("Used",haveUsed ? sizeOr(used,false) : "N/A");
			ImGui::SameLine(0.0f,gap);
			ui::stat("Committed",sizeOr(comm,false));
			ImGui::SameLine(0.0f,gap);
			ui::stat("Entries",haveEnts ? util::count(ents) : "N/A");
			if (haveUsed && comm) {
				ImGui::SameLine(0.0f,gap);
				ImGui::BeginGroup();
				ImGui::TextDisabled("Used of committed");
				ui::Seg seg[2] = {{(float)used,ui::c.priv,"Used"},{(float)(comm - std::min(comm,used)),ui::c.freeMem,"Committed but free"}};
				ui::stackBar("##hbarc",seg,2,ImVec2(std::max(ImGui::GetContentRegionAvail().x / ui::px(1.0f),80.0f),16.0f));
				ImGui::EndGroup();
			}
			if (s.heaps.res->clone) ImGui::TextDisabled("Read from a clone of the process, Query on a clone turns this off.");
		}
		if (!s.flushMsg.empty()) {
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(s.flushBad ? ui::c.bad : ui::c.good,"%s",s.flushMsg.c_str());
			ImGui::PopTextWrapPos();
		}
		if (s.heaps.res && !s.heaps.res->note.empty()) ui::hint(s.heaps.res->note.c_str());
		if (rem && s.heaps.res && s.heaps.res->ok) ui::hint("Used is the allocated blocks with their headers and rounding, so it reads above what the program asked for when blocks are small.");
		ui::endCard();
		std::string ask = "Flush heaps of " + util::safe(r.name) + " " + pidTxt(r) + " ?\n\nThis runs RtlFlushHeaps inside the process through a remote thread, so its heaps give free pages back. If a heap lock is held the thread blocks and the process can stall or deadlock, in rare cases it crashes. Anti-cheat or EDR may react. Most programs get little out of it.";
		if (ui::askDraw(s.askId.c_str(),ask.c_str(),"Flush heaps",true)) startFlush(s);
		ui::beginCard("##hlist",nullptr,ImVec2(0.0f,-1.0f));
		if (!jobState(s.heaps,"Nothing queried yet. Press Query heaps.","Querying heaps...")) {
			auto res = s.heaps.res;
			if (res->rows.empty()) ui::empty("This process has no heaps to show.");
			else drawList("##diagheaps",kHeapCols,8,s.heapL,res.get(),(int)res->rows.size(),[&](int i,int c) -> std::string {
				const Heap& h = res->rows[i];
				switch (c) {
				case 0: return std::to_string(i + 1);
				case 1: return util::hex(h.base);
				case 2: return sizeOr(h.used,inBytes);
				case 3: return sizeOr(h.committed,inBytes);
				case 4: return h.entries == NA ? "N/A" : util::count(h.entries);
				case 5: return h.flags == NA ? "N/A" : h.flagTxt;
				case 6: return h.cls.empty() ? "N/A" : h.cls;
				case 7: return h.type.empty() ? "N/A" : h.type;
				}
				return "";
			},[&](int i,int c,uint64_t& o) {
				const Heap& h = res->rows[i];
				switch (c) {
				case 0: o = (uint64_t)i; return true;
				case 1: o = h.base; return true;
				case 2: o = h.used; return true;
				case 3: o = h.committed; return true;
				case 4: o = h.entries; return true;
				}
				return false;
			},nullptr,[&](int i) { return res->rows[i].def; });
		}
		ui::endCard();
	}

	const ColDef kModCols[] = {
		{"#",40,FR},
		{"File",130,0},
		{"Base address",124,FM},
		{"Offset",76,FM},
		{"Address",124,FM},
		{"Symbol",216,0},
		{"Resident",56,0}
	};

	void subMod(PS& s) {
		s.mod.poll();
		if (!s.mod.started && !s.mod.running()) {
			ProcRef r = s.r;
			s.mod.start([r](std::atomic<bool>& stop) { return be::modPages(r,stop); });
		}
		auto res = s.mod.res;
		ui::beginCard("##mbar",nullptr);
		bool busy = s.mod.running();
		if (busy) ImGui::BeginDisabled();
		if (ui::iconBtn("dmref",ui::IcoRefresh,"Scan the images again")) {
			ProcRef r = s.r;
			s.mod.start([r](std::atomic<bool>& stop) { return be::modPages(r,stop); });
		}
		if (busy) ImGui::EndDisabled();
		ImGui::SameLine();
		if (res && res->ok) {
			std::string t = util::count(res->rows.size()) + " pages in " + util::count(res->images) + " images, " + util::count(res->residentN) + " resident";
			if (res->skipped) t += ", " + util::count(res->skipped) + " data file maps skipped";
			if (res->fails) t += ", " + util::count(res->fails) + " images could not be read";
			t += ", " + readAt(*res);
			if (!res->complete) t += ", cut short : " + res->reason;
			ui::hint(t.c_str());
		} else ImGui::TextDisabled("Image pages that no longer are the shared copy from disk");
		ui::hint("Relocations, written .data, hooks and patches all land here. The kernel's copy-on-write state says the page was written, nobody compares bytes with the file, so a hit is a candidate and not proof of tampering.");
		if (be::remote()) ui::hint("Symbols are the nearest export, no PDBs. Offsets far from any export show as file+offset.");
		if (res && !res->note.empty()) ui::hint(res->note.c_str());
		ui::endCard();
		ui::beginCard("##mlist",nullptr,ImVec2(0.0f,-1.0f));
		if (!jobState(s.mod,"","Scanning images...")) {
			if (res->rows.empty()) ui::empty("No modified image pages.");
			else drawList("##diagmod",kModCols,7,s.modL,res.get(),(int)res->rows.size(),[&](int i,int c) -> std::string {
				const ModPage& p = res->rows[i];
				switch (c) {
				case 0: return std::to_string(i + 1);
				case 1: return p.file;
				case 2: return util::hex(p.base);
				case 3: return util::hex(p.addr - p.base);
				case 4: return util::hex(p.addr);
				case 5: return p.sym;
				case 6: return p.resident ? "Yes" : "No";
				}
				return "";
			},[&](int i,int c,uint64_t& o) {
				const ModPage& p = res->rows[i];
				switch (c) {
				case 0: o = (uint64_t)i; return true;
				case 2: o = p.base; return true;
				case 3: o = p.addr - p.base; return true;
				case 4: o = p.addr; return true;
				}
				return false;
			},nullptr,nullptr);
		}
		ui::endCard();
	}

	const ColDef kCfgCols[] = {
		{"#",54,FR},
		{"Bitmap",54,0},
		{"Type",120,0},
		{"Address",124,FM},
		{"End",124,FM},
		{"Value",70,FR},
		{"Symbol",260,FN},
		{"Allocation base",124,FM | FH},
		{"Region base",124,FM | FH},
		{"Region size",80,FR | FH},
		{"Memory type",70,FH},
		{"State",60,FH},
		{"Protection",70,FH},
		{"Mapped file",260,FN}
	};

	void subCfg(PS& s) {
		s.cfgJ.poll();
		if (!s.cfgJ.started && !s.cfgJ.running()) {
			ProcRef r = s.r;
			s.cfgJ.start([r](std::atomic<bool>& stop) { return be::cfg(r,stop); });
		}
		auto res = s.cfgJ.res;
		ui::beginCard("##cbar",nullptr);
		bool busy = s.cfgJ.running();
		if (busy) ImGui::BeginDisabled();
		if (ui::iconBtn("dcref",ui::IcoRefresh,"Decode the bitmap again")) {
			ProcRef r = s.r;
			s.cfgJ.start([r](std::atomic<bool>& stop) { return be::cfg(r,stop); });
		}
		if (busy) ImGui::EndDisabled();
		if (be::remote()) {
			ImGui::SameLine();
			if (ImGui::Button("Options##c")) ImGui::OpenPopup("##copts");
			if (ImGui::BeginPopup("##copts")) {
				bool a = ImGui::MenuItem("Show image CFG targets",nullptr,&s.showImage);
				bool b = ImGui::MenuItem("Show modified/allowed targets",nullptr,&s.showMod);
				bool c = ImGui::MenuItem("Show all-valid (no CFG) ranges",nullptr,&s.showAll);
				bool d = ImGui::MenuItem("Show targets in memory not mapped here",nullptr,&s.showUnm);
				if (a || b || c || d) s.cfgL.dirty = true;
				ImGui::EndPopup();
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ui::px(150.0f));
			bool go = ImGui::InputTextWithHint("##caddr","Address to check",s.addr,sizeof(s.addr),ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();
			go |= ImGui::Button("Check##c");
			if (go) {
				uint64_t a = 0;
				if (!util::parseHex(s.addr,a)) {
					s.addrTxt = "Not a hex address";
					s.addrBad = true;
				} else {
					CfgCheck k = be::cfgCheck(s.r,a);
					s.addrBad = !k.ok;
					if (!k.ok) s.addrTxt = "Failed : " + k.why;
					else {
						s.addrTxt = util::hex(a) + " : " + cfgMeaning(k,a);
						if (k.enabled == 0) s.addrTxt += ", the process policy has CFG off";
					}
				}
			}
			if (res && res->ok) {
				ImGui::SameLine();
				ImGui::TextDisabled("%s",readAt(*res).c_str());
			}
			if (!s.addrTxt.empty()) {
				ImGui::PushTextWrapPos(0.0f);
				if (s.addrBad) ImGui::TextColored(ui::c.bad,"%s",s.addrTxt.c_str());
				else ImGui::TextUnformatted(s.addrTxt.c_str());
				ImGui::PopTextWrapPos();
			}
		}
		if (res && res->ok) {
			for (auto& l : res->summary) ImGui::TextDisabled("%s",l.c_str());
			std::string st = util::count(s.cfgL.order.size()) + " of " + util::count(res->rows.size()) + " entries shown";
			st += "  (image " + util::count(res->counts[CfgImage]) + ", modified " + util::count(res->counts[CfgModified]) + ", export suppressed " + util::count(res->counts[CfgSuppressed]) + ", all valid " + util::count(res->counts[CfgAllValid]) + ", not mapped " + util::count(res->counts[CfgUnmapped]) + ")";
			if (!res->complete) st += ", cut short : " + res->reason;
			ImGui::TextUnformatted(st.c_str());
		}
		if (be::remote()) ui::hint("Modified/allowed : valid in the bitmap but not in the image's guard table. JIT code, SetProcessValidCallTargets and the few thunks Windows appends to each image land here. Not mapped here : bitmap pages are shared, so they also hold targets of images this process did not load.");
		ui::endCard();
		ui::beginCard("##clist",nullptr,ImVec2(0.0f,-1.0f));
		if (!jobState(s.cfgJ,"","Decoding the bitmap...")) {
			auto reg = [res](const CfgEnt& e) -> const CfgReg* { return e.reg >= 0 ? &res->regs[e.reg] : nullptr; };
			if (res->rows.empty()) ui::empty("Nothing in the bitmap to list, see the line above.");
			else if (s.cfgL.src == res.get() && !s.cfgL.dirty && s.cfgL.order.empty()) ui::empty("No entry of the kinds Options has turned on.");
			else drawList("##diagcfg",kCfgCols,14,s.cfgL,res.get(),(int)res->rows.size(),[&](int i,int c) -> std::string {
				const CfgEnt& e = res->rows[i];
				const CfgReg* g = reg(e);
				switch (c) {
				case 0: return std::to_string(i + 1);
				case 1: return e.wow ? "32-bit" : "Native";
				case 2: return cfgTypeName(e.type);
				case 3: return util::hex(e.addr);
				case 4: return e.end ? util::hex(e.end) : "";
				case 5: return e.end ? util::size(e.end - e.addr) : std::to_string((int)e.val);
				case 6: return symAt(res->syms,e.addr);
				case 7: return g ? util::hex(g->alloc) : "";
				case 8: return g ? util::hex(g->base) : "";
				case 9: return g ? util::size(g->size) : "";
				case 10: return g ? g->typeTxt : "";
				case 11: return g ? g->stateTxt : "";
				case 12: return g ? g->protTxt : "";
				case 13: return g ? g->file : "";
				}
				return "";
			},[&](int i,int c,uint64_t& o) {
				const CfgEnt& e = res->rows[i];
				const CfgReg* g = reg(e);
				switch (c) {
				case 0: o = (uint64_t)i; return true;
				case 1: o = e.wow; return true;
				case 2: o = e.type; return true;
				case 3: o = e.addr; return true;
				case 4: o = e.end; return true;
				case 5: o = e.end ? e.end - e.addr : e.val; return true;
				case 7: o = g ? g->alloc : 0; return true;
				case 8: o = g ? g->base : 0; return true;
				case 9: o = g ? g->size : 0; return true;
				case 10: o = g ? g->type : 0; return true;
				case 11: o = g ? g->state : 0; return true;
				case 12: o = g ? g->prot : 0; return true;
				}
				return false;
			},[&](int i) {
				int t = res->rows[i].type;
				return t == CfgAllValid ? s.showAll : t == CfgModified ? s.showMod : t == CfgUnmapped ? s.showUnm : s.showImage;
			},nullptr);
		}
		ui::endCard();
	}

	const ColDef kLockCols[] = {
		{"#",40,FR},
		{"Type",100,0},
		{"Address",124,FM},
		{"Filename",200,0},
		{"Thread",60,FR},
		{"Lock count",70,FR},
		{"Contention count",100,FR | FH},
		{"Entry count",70,FR},
		{"Recursion count",90,FR},
		{"Waiting shared count",110,FR | FH},
		{"Waiting exclusive count",120,FR | FH}
	};

	void subLocks(PS& s) {
		s.locks.poll();
		bool rem = be::remote();
		if (!rem && !s.locks.started) {
			s.locks.started = true;
			startLocks(s);
		}
		if (rem) warnCard("##lnote","Query locks starts a thread inside this process that walks every lock. In rare cases that stalls or deadlocks it. Nothing runs until you press the button.");
		auto res = s.locks.res;
		ui::beginCard("##lbar",nullptr);
		if (rem) {
			bool busy = s.locks.running();
			if (busy) ImGui::BeginDisabled();
			if (ImGui::Button("Query locks")) startLocks(s);
			if (busy) ImGui::EndDisabled();
			ImGui::SameLine();
		}
		if (res && res->ok) {
			uint64_t held = 0;
			for (auto& l : res->rows) if (l.owner) held++;
			ImGui::TextDisabled("%s locks, %s held, %s",util::count(res->rows.size()).c_str(),util::count(held).c_str(),readAt(*res).c_str());
		} else ImGui::TextDisabled("Critical sections and resources of this process");
		if (rem) ui::hint("Windows only lists locks with debug info, most critical sections have none since Windows 8. So this is a part of them, not all.");
		ui::endCard();
		ui::beginCard("##llist",nullptr,ImVec2(0.0f,-1.0f));
		if (!jobState(s.locks,"Nothing queried yet. Press Query locks.","Querying locks...")) {
			if (res->rows.empty()) ui::empty("This process has no locks to show.");
			else drawList("##diaglocks",kLockCols,11,s.lockL,res.get(),(int)res->rows.size(),[&](int i,int c) -> std::string {
				const Lock& l = res->rows[i];
				switch (c) {
				case 0: return std::to_string(i + 1);
				case 1: return l.type;
				case 2: return util::hex(l.addr);
				case 3: return l.file;
				case 4: return l.owner ? std::to_string(l.owner) : "";
				case 5: return l.lockCount == -1 ? "" : std::to_string(l.lockCount);
				case 6: return util::count(l.contention);
				case 7: return util::count(l.entry);
				case 8: return std::to_string(l.recursion);
				case 9: return util::count(l.waitShared);
				case 10: return util::count(l.waitExcl);
				}
				return "";
			},[&](int i,int c,uint64_t& o) {
				const Lock& l = res->rows[i];
				switch (c) {
				case 0: o = (uint64_t)i; return true;
				case 2: o = l.addr; return true;
				case 4: o = l.owner; return true;
				case 5: o = (uint64_t)(l.lockCount + 1); return true;
				case 6: o = l.contention; return true;
				case 7: o = l.entry; return true;
				case 8: o = (uint64_t)(l.recursion + ((int64_t)1 << 32)); return true;
				case 9: o = l.waitShared; return true;
				case 10: o = l.waitExcl; return true;
				}
				return false;
			},nullptr,nullptr);
		}
		ui::endCard();
	}

	const ColDef kGdiCols[] = {
		{"Type",110,0},
		{"Handle",96,FM},
		{"Object",160,FM},
		{"Information",400,0}
	};

	void subGdi(PS& s) {
		s.gdi.poll();
		if (!s.gdi.started && !s.gdi.running()) {
			ProcRef r = s.r;
			s.gdi.start([r](std::atomic<bool>&) { return be::gdi(r); });
		}
		auto res = s.gdi.res;
		if (res && !res->note.empty()) warnCard("##gnote",res->note.c_str());
		ui::beginCard("##gbar",nullptr);
		bool busy = s.gdi.running();
		if (busy) ImGui::BeginDisabled();
		if (ui::iconBtn("dgref",ui::IcoRefresh,"Read the GDI table again")) {
			ProcRef r = s.r;
			s.gdi.start([r](std::atomic<bool>&) { return be::gdi(r); });
		}
		if (busy) ImGui::EndDisabled();
		ImGui::SameLine();
		if (res && res->ok) {
			std::string t = util::count(res->rows.size()) + " objects in the table, the OS counts " + util::count(res->osCount) + ", peak " + util::count(res->osPeak);
			if (res->other) t += ", " + util::count(res->other) + " of other types left out";
			t += ", " + readAt(*res);
			ui::hint(t.c_str());
		} else ImGui::TextDisabled("GDI objects this process owns");
		if (be::remote()) ui::hint("GDI objects live in session pool, not in the process's RAM. Information is empty when Windows won't answer for a handle of another process. The OS count can be higher than the table, most of its entries carry no owner.");
		ui::endCard();
		ui::beginCard("##glist",nullptr,ImVec2(0.0f,-1.0f));
		if (!jobState(s.gdi,"","Reading the table...")) {
			if (res->rows.empty()) ui::empty("The table lists no GDI objects for this process.");
			else drawList("##diaggdi",kGdiCols,4,s.gdiL,res.get(),(int)res->rows.size(),[&](int i,int c) -> std::string {
				const GdiObj& o = res->rows[i];
				char b[16];
				switch (c) {
				case 0: return o.type;
				case 1:
					snprintf(b,sizeof(b),"0x%08x",o.handle);
					return b;
				case 2: return util::hex(o.object);
				case 3: return o.info;
				}
				return "";
			},[&](int i,int c,uint64_t& k) {
				const GdiObj& o = res->rows[i];
				if (c == 1) k = o.handle;
				else if (c == 2) k = o.object;
				else return false;
				return true;
			},nullptr,nullptr);
		}
		ui::endCard();
	}

	void tabDiag(const ProcRef& r) {
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		if (row && row->pseudo) {
			ui::empty("This is a pseudo process, it has no memory of its own.");
			return;
		}
		PS& s = stateOf(r);
		if (ImGui::BeginTabBar("##dtabs")) {
			if (ImGui::BeginTabItem("Modified pages")) {
				subMod(s);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("CFG")) {
				subCfg(s);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Locks")) {
				if (sysProc(r)) ui::empty("System processes have no user locks.");
				else subLocks(s);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("GDI")) {
				subGdi(s);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}

}

namespace vmdiag {

	void init() {
		procwin::addTab("Heaps",60,tabHeaps);
		procwin::addTab("Diagnostics",70,tabDiag);
		sampler::add("diagCache",30000,cacheTrim);
	}

}
