#include "procs.h"
#include "cfg.h"
#include "procdet.h"
#include "ram.h"
#include "sysmem.h"
#include "ui.h"
#include "vmem.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>
#include <utility>

namespace {

	using namespace procs;
	using Key = std::pair<uint32_t,uint64_t>;

	enum Mode { MFlat,MTree,MGroup };

	struct Node {
		const ProcRow* r; // null on a group row
		const ProcRow* d; // what the cells show : r, a group sum or a collapsed total
		int depth;
		int grp;
		bool kids,open;
	};

	struct Page {
		std::shared_ptr<const ProcList> list;
		uint64_t seq = UINT64_MAX;
		bool dirty = true,loaded = false;
		int sortCol = CWs;
		bool desc = true;
		int mode = MFlat;
		bool hideOther = false,hideSys = false;
		char filter[128] = {};
		std::string low;
		std::string me; // Linux under pkexec : who started FRS, empty otherwise
		char path[512] = {};

		std::vector<Node> nodes;
		std::vector<ProcRow> extra;
		std::vector<std::vector<int>> members,kids;
		std::vector<int> order;
		std::vector<char> pass,vis;
		std::set<Key> closed; // tree rows the user collapsed
		std::set<std::string> opened; // group rows the user expanded
		std::string tot[COLS];
		int shown = 0;

		std::vector<ProcRef> sel;
		std::set<Key> selKeys;
		std::string anchor;
		bool colOn[COLS] = {};
		int ord[COLS] = {}; // display order, the user can drag columns around
		int preset = 0;
		bool menu = false;
		bool jump = false; // show the first selected row once
		std::shared_ptr<task::T> exp;
	};

	Page g;

	Key keyOf(const ProcRow& r) {
		return Key(r.pid,r.start);
	}

	const std::vector<ProcRow>& rows() {
		return g.list->rows;
	}

	bool passes(const ProcRow& r,const std::string& me) {
		if (!rowMatch(r,g.low)) return false;
		if (g.hideSys && (r.pseudo || r.sys || r.osProc)) return false;
		if (g.hideOther && (r.user.empty() || r.user != me)) return false;
		return true;
	}

	void sortIdx(std::vector<int>& v) {
		const auto& rs = rows();
		std::sort(v.begin(),v.end(),[&](int a,int b) { return rowLess(rs[a],rs[b],g.sortCol,g.desc); });
	}

	bool calc(int i) {
		bool v = g.pass[i] != 0;
		for (int k : g.kids[i]) if (calc(k)) v = true;
		g.vis[i] = v;
		return v;
	}

	void sumUnder(ProcRow& to,int i) {
		for (int k : g.kids[i]) {
			if (!g.vis[k]) continue;
			if (g.pass[k]) addSum(to,rows()[k]);
			sumUnder(to,k);
		}
	}

	void emit(int i,int depth,bool agg) {
		if (!g.vis[i]) return;
		// a filtered out parent does not take its children along, they move up a level
		if (!g.pass[i]) {
			for (int k : g.kids[i]) emit(k,depth,agg);
			return;
		}
		const ProcRow& r = rows()[i];
		bool kids = false;
		for (int k : g.kids[i]) if (g.vis[k]) kids = true;
		bool open = !kids || !g.closed.count(keyOf(r));
		Node n{&r,&r,depth,-1,kids,open};
		if (kids && !open && agg) {
			g.extra.push_back(r);
			sumUnder(g.extra.back(),i);
			n.d = &g.extra.back();
		}
		g.nodes.push_back(n);
		if (open) for (int k : g.kids[i]) emit(k,depth + 1,agg);
	}

	void buildTree() {
		const auto& rs = rows();
		int n = (int)rs.size();
		g.kids.resize(n);
		for (auto& k : g.kids) k.clear();
		g.vis.assign(n,0);
		g.order.clear();
		for (int i = 0; i < n; i++) {
			const ProcRow* p = parentOf(*g.list,rs[i]);
			if (p) g.kids[p - rs.data()].push_back(i);
			else g.order.push_back(i);
		}
		for (auto& k : g.kids) if (k.size() > 1) sortIdx(k);
		sortIdx(g.order);
		bool agg = cfg::getb("procAggregate",false);
		for (int i : g.order) calc(i);
		for (int i : g.order) emit(i,0,agg);
	}

	void buildGroups() {
		const auto& rs = rows();
		std::unordered_map<std::string,int> idx;
		g.members.clear();
		for (int i = 0; i < (int)rs.size(); i++) {
			if (!g.pass[i] || rs[i].pseudo) continue;
			std::string k = util::lower(rs[i].name);
			auto it = idx.find(k);
			int gi;
			if (it == idx.end()) {
				gi = (int)g.extra.size();
				idx[k] = gi;
				g.extra.emplace_back();
				g.extra.back().name = rs[i].name;
				g.members.emplace_back();
			} else gi = it->second;
			addSum(g.extra[gi],rs[i]);
			g.extra[gi].children++;
			g.members[gi].push_back(i);
		}
		g.order.resize(g.extra.size());
		for (int i = 0; i < (int)g.order.size(); i++) g.order[i] = i;
		std::sort(g.order.begin(),g.order.end(),[&](int a,int b) { return rowLess(g.extra[a],g.extra[b],g.sortCol,g.desc); });
		for (int gi : g.order) {
			const ProcRow& s = g.extra[gi];
			bool open = g.opened.count(s.name) != 0;
			g.nodes.push_back(Node{nullptr,&s,0,gi,true,open});
			if (!open) continue;
			sortIdx(g.members[gi]);
			for (int i : g.members[gi]) g.nodes.push_back(Node{&rs[i],&rs[i],1,gi,false,true});
		}
	}

	void buildTotals() {
		const auto& rs = rows();
		for (int c = 0; c < COLS; c++) {
			g.tot[c].clear();
			if (!cols[c].total) continue;
			int64_t s = 0,v = 0;
			bool any = false;
			for (int i = 0; i < (int)rs.size(); i++) if (g.pass[i] && colNum(rs[i],c,v)) {
				s += v;
				any = true;
			}
			if (!any) continue;
			switch (cols[c].kind) {
				case KSize: g.tot[c] = util::size((uint64_t)s); break;
				case KDelta: g.tot[c] = util::delta(s); break;
				case KCount: g.tot[c] = util::count((uint64_t)s); break;
				case KCountDelta: g.tot[c] = s ? "+" + util::count((uint64_t)s) : ""; break;
				default: break;
			}
		}
	}

	void purgeSel() {
		for (size_t i = 0; i < g.sel.size();) {
			const ProcRow* s = g.list->find(g.sel[i]);
			if (s && g.pass[s - rows().data()]) i++;
			else {
				g.selKeys.erase(Key(g.sel[i].pid,g.sel[i].start));
				g.sel.erase(g.sel.begin() + i);
			}
		}
		for (auto it = g.closed.begin(); it != g.closed.end();) {
			const ProcRow* p = g.list->find(it->first);
			if (p && p->start == it->second) ++it;
			else it = g.closed.erase(it);
		}
	}

	// runs when a tick came in or the view changed, not per frame
	void rebuild() {
		g.list = latest();
		g.seq = g.list->seq;
		g.dirty = false;
		const auto& rs = rows();
		g.nodes.clear();
		g.extra.clear();
		g.extra.reserve(rs.size() + 1); // nodes point into it
		g.pass.resize(rs.size());
		const ProcRow* self = g.list->find(be::selfPid());
		std::string me = !g.me.empty() ? g.me : self ? self->user : "";
		g.shown = 0;
		for (size_t i = 0; i < rs.size(); i++) {
			g.pass[i] = passes(rs[i],me) ? 1 : 0;
			g.shown += g.pass[i];
		}
		if (g.mode == MTree) buildTree();
		else if (g.mode == MGroup) buildGroups();
		else {
			g.order.clear();
			for (int i = 0; i < (int)rs.size(); i++) if (g.pass[i]) g.order.push_back(i);
			sortIdx(g.order);
			for (int i : g.order) g.nodes.push_back(Node{&rs[i],&rs[i],0,-1,false,true});
		}
		buildTotals();
		purgeSel();
	}

	void selAdd(const ProcRow& r) {
		if (g.selKeys.insert(keyOf(r)).second) g.sel.push_back(ref(r));
	}

	void selDel(const ProcRow& r) {
		g.selKeys.erase(keyOf(r));
		g.sel.erase(std::remove_if(g.sel.begin(),g.sel.end(),[&](const ProcRef& x) { return x.pid == r.pid && x.start == r.start; }),g.sel.end());
	}

	void selClear() {
		g.sel.clear();
		g.selKeys.clear();
	}

	bool isSel(const Node& n) {
		if (n.r) return g.selKeys.count(keyOf(*n.r)) != 0;
		for (int i : g.members[n.grp]) if (!g.selKeys.count(keyOf(rows()[i]))) return false;
		return !g.members[n.grp].empty();
	}

	void selNode(const Node& n,bool on) {
		if (n.r) {
			if (on) selAdd(*n.r);
			else selDel(*n.r);
			return;
		}
		for (int i : g.members[n.grp]) {
			if (on) selAdd(rows()[i]);
			else selDel(rows()[i]);
		}
	}

	std::string nodeId(const Node& n) {
		return n.r ? std::to_string(n.r->pid) + "." + std::to_string(n.r->start) : "g." + n.d->name;
	}

	void clickRow(int idx) {
		const ImGuiIO& io = ImGui::GetIO();
		const Node& n = g.nodes[idx];
		if (io.KeyShift && !g.anchor.empty()) {
			int a = -1;
			for (int i = 0; i < (int)g.nodes.size(); i++) if (nodeId(g.nodes[i]) == g.anchor) a = i;
			if (a < 0) a = idx;
			if (!io.KeyCtrl) selClear();
			for (int i = std::min(a,idx); i <= std::max(a,idx); i++) selNode(g.nodes[i],true);
			return;
		}
		if (io.KeyCtrl) selNode(n,!isSel(n));
		else {
			selClear();
			selNode(n,true);
		}
		g.anchor = nodeId(n);
	}

	void selSubtree(int i) {
		if (g.pass[i]) selAdd(rows()[i]);
		if (i < (int)g.kids.size()) for (int k : g.kids[i]) selSubtree(k);
	}

	// middle click, SI's way to grab an app : the subtree, or every row of that name in a flat list
	void middleClick(const Node& n) {
		if (!ImGui::GetIO().KeyCtrl) selClear();
		if (!n.r) {
			selNode(n,true);
			return;
		}
		if (g.mode == MTree) {
			selSubtree((int)(n.r - rows().data()));
			return;
		}
		std::string low = util::lower(n.r->name);
		const auto& rs = rows();
		for (size_t i = 0; i < rs.size(); i++) if (g.pass[i] && util::lower(rs[i].name) == low) selAdd(rs[i]);
	}

	std::string cellOf(const Node& n,int c) {
		if (!n.r && !cols[c].sum && c != CName) return "";
		return colText(*n.d,c);
	}

	std::string rowLine(const Node& n) {
		std::string s;
		for (int k = 0; k < COLS; k++) {
			int c = g.ord[k];
			if (!g.colOn[c] || c == CHist) continue;
			if (!s.empty()) s += '\t';
			s += cellOf(n,c);
		}
		return s;
	}

	void copyRows() {
		std::string s;
		for (auto& n : g.nodes) if (isSel(n)) s += rowLine(n) + "\n";
		if (!s.empty()) ImGui::SetClipboardText(s.c_str());
	}

	void copyPids() {
		std::string s;
		for (auto& r : g.sel) {
			if (!s.empty()) s += ",";
			s += std::to_string(r.pid);
		}
		if (!s.empty()) ImGui::SetClipboardText(s.c_str());
	}

	// names are not ours : a cell a spreadsheet would run as a formula gets a quote in front
	std::string csvCell(const std::string& v) {
		std::string s = v;
		if (!s.empty() && strchr("=+-@",s[0])) s.insert(0,1,'\'');
		if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
		std::string o = "\"";
		for (char ch : s) {
			if (ch == '"') o += '"';
			o += ch;
		}
		return o + "\"";
	}

	std::string csvRows() {
		std::string s;
		bool first = true;
		for (int k = 0; k < COLS; k++) {
			int c = g.ord[k];
			if (!g.colOn[c] || c == CHist) continue;
			if (!first) s += ',';
			s += cols[c].title;
			first = false;
		}
		s += '\n';
		for (auto& n : g.nodes) {
			first = true;
			for (int k = 0; k < COLS; k++) {
				int c = g.ord[k];
				if (!g.colOn[c] || c == CHist) continue;
				if (!first) s += ',';
				first = false;
				if (!n.r && !cols[c].sum && c != CName) continue;
				int64_t v = 0;
				if (cols[c].kind == KText) s += csvCell(colText(*n.d,c));
				else if (c == CPrio) s += colText(*n.d,c);
				else if (colNum(*n.d,c,v)) s += std::to_string(v);
				else s += "N/A";
			}
			s += '\n';
		}
		return s;
	}

	void startExport(bool over) {
		std::string path = g.path;
		std::string data = csvRows();
		g.exp = task::run([path,data,over](task::T&) -> std::string {
			if (path.empty()) return "Export CSV : failed (no path)";
			std::string err;
			int rc = be::writeFile(path,data,over,err);
			if (rc == 1) return "EXISTS";
			if (rc < 0) return "Export CSV : failed (" + util::safe(err) + ")";
			return "Export CSV : OK (" + util::safe(path) + ")";
		},false);
	}

	void pollExport() {
		if (g.exp && g.exp->done) {
			std::string r = g.exp->res;
			g.exp.reset();
			if (r == "EXISTS") ui::askOpen("Overwrite file ?##pexp");
			else {
				appLog(r);
				ui::toast(r);
			}
		}
		if (ui::askDraw("Overwrite file ?##pexp","That file exists. Replace it with this export?","Overwrite",true)) startExport(true);
	}

	void runReduce() {
		auto refs = g.sel;
		task::run([refs](task::T&) -> std::string {
			if (refs.size() == 1) {
				std::string s = reduceWs(refs[0]);
				ui::toast(s);
				return s;
			}
			int bad = 0;
			for (auto& r : refs) {
				std::string s = reduceWs(r);
				if (s.find(" : OK") == std::string::npos) {
					bad++;
					appLog(s);
				}
			}
			std::string lb = "Reduce working set (" + std::to_string(refs.size()) + " processes)";
			std::string s = bad ? lb + " : failed (" + std::to_string(bad) + " of them, see the log)" : lb + " : OK";
			ui::toast(s);
			return s;
		});
	}

	void runPrio(const ProcRef& r,int v) {
		task::run([r,v](task::T&) -> std::string {
			std::string s = procdet::setPrio(r,v,nullptr);
			ui::toast(s);
			return s;
		});
	}

	void drawMenu() {
		if (g.menu) {
			ImGui::OpenPopup("##pmenu");
			g.menu = false;
		}
		if (!ImGui::BeginPopup("##pmenu")) return;
		if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
		bool one = g.sel.size() == 1;
		const ProcRow* r = one ? g.list->find(g.sel[0]) : nullptr;
		bool canTrim = false;
		for (auto& x : g.sel) {
			const ProcRow* p = g.list->find(x);
			if (p && !p->pseudo && !procdet::sysLike(*p)) canTrim = true;
		}
		if (ImGui::MenuItem("Open details",nullptr,false,one)) procwin::open(g.sel[0]);
		if (ImGui::MenuItem("Reduce working set",nullptr,false,canTrim)) runReduce();
#ifdef _WIN32
		ui::tip("Moves resident pages to the standby list. Nothing is lost, they fault back when used. System and pseudo processes are refused.");
		bool prioOn = r && !r->pseudo && !r->sys;
#else
		ui::tip("Pages the process out : file pages are dropped, anonymous ones go to swap when there is any. Nothing is lost, they fault back when used. Kernel threads and init are refused.");
		bool prioOn = false;
#endif
		if (ImGui::BeginMenu("Page priority",prioOn)) {
			static const int vals[] = {5,4,3,2,1};
			for (int v : vals) if (ImGui::MenuItem(pagePrioName(v),nullptr,r && r->pagePrio == v)) runPrio(g.sel[0],v);
			ImGui::EndMenu();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Copy row","Ctrl+C",false,!g.sel.empty())) copyRows();
		if (ImGui::MenuItem("Copy pid",nullptr,false,!g.sel.empty())) copyPids();
		ImGui::EndPopup();
	}

	void tipRow(const char* k,const std::string& v) {
		ImGui::TextDisabled("%s",k);
		ImGui::SameLine(ui::px(120.0f));
		ImGui::TextUnformatted(v.c_str());
	}

	void rowTooltip(const Node& n) {
		const ProcRow& r = *n.d;
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ui::px(430.0f));
		ui::pushBold();
		if (n.r) ImGui::Text("%s (%u)",r.name.c_str(),r.pid);
		else ImGui::Text("%s (%d processes)",r.name.c_str(),r.children);
		ui::popFont();
		if (!r.path.empty()) ImGui::TextDisabled("%s",r.path.c_str());
		if (!r.user.empty()) ImGui::TextDisabled("%s",r.user.c_str());
		ImGui::Separator();
		tipRow("Working set",util::size(r.ws));
		tipRow("Private WS",util::size(r.wsPriv));
		tipRow("Private bytes",util::size(r.priv));
		tipRow("Virtual",util::size(r.virt));
		tipRow("Page faults",util::count(r.faults));
		tipRow("Hard faults",util::count(r.hardFaults));
		if (n.r) {
			tipRow("Peak WS",util::size(r.wsPeak));
			tipRow("Peak private",util::size(r.privPeak));
			tipRow("Shared commit",util::size(r.sharedCommit));
			tipRow("Paged pool",util::size(r.paged));
			tipRow("Non-paged pool",util::size(r.nonPaged));
			tipRow("Page priority",pagePrioName(r.pagePrio));
			tipRow("Min / max WS",util::size(r.minWs) + " / " + util::size(r.maxWs));
			tipRow("Threads",std::to_string(r.threads));
			if (r.pseudo) ui::hint("Pseudo process, it has no memory of its own.");
			else if (r.memCompression) ui::hint("Holds the compressed pages of the compression store. A trim is refused.");
			else if (r.registry) ui::hint("Its working set is registry hive memory. A trim is refused.");
			else if (r.sys) ui::hint("System process. Its numbers are not all kernel memory. A trim is refused.");
			if (r.jobId != NA && r.jobId != 0 && r.cgroup.empty()) ImGui::TextDisabled("Process is in a job");
			if (!r.cgroup.empty()) ImGui::TextDisabled("Cgroup : %s",r.cgroup.c_str());
			if (r.filtered) ui::hint("Something stripped the rights of FRS's handle, so Shared WS and some details stay N/A.");
			else if (r.noHandle) ui::hint("FRS can't open this process, so handle columns stay N/A. The rest comes from the system snapshot.");
			else if (r.noFull) ui::hint("Limited access only, Shared WS and Shareable WS stay N/A.");
		} else {
			ui::hint("Sums of the group. Working set sums double count shared pages.");
		}
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

	void chevron(ImVec2 a,float w,float h,bool open,ImU32 col) {
		ImDrawList* dl = ImGui::GetWindowDrawList();
		float cx = a.x + w * 0.5f;
		float cy = a.y + h * 0.5f;
		float d = ui::px(3.5f);
		if (open) {
			dl->PathLineTo(ImVec2(cx - d,cy - d * 0.5f));
			dl->PathLineTo(ImVec2(cx,cy + d * 0.5f));
			dl->PathLineTo(ImVec2(cx + d,cy - d * 0.5f));
		} else {
			dl->PathLineTo(ImVec2(cx - d * 0.5f,cy - d));
			dl->PathLineTo(ImVec2(cx + d * 0.5f,cy));
			dl->PathLineTo(ImVec2(cx - d * 0.5f,cy + d));
		}
		dl->PathStroke(col,ImDrawFlags_None,ui::px(1.5f));
	}

	void toggle(const Node& n) {
		if (n.r) {
			if (n.open) g.closed.insert(keyOf(*n.r));
			else g.closed.erase(keyOf(*n.r));
		} else {
			if (n.open) g.opened.erase(n.d->name);
			else g.opened.insert(n.d->name);
		}
		g.dirty = true;
	}

	void nameCell(int i) {
		const Node& n = g.nodes[i];
		const ProcRow& r = *n.d;
		float x0 = ImGui::GetCursorPosX();
		bool sel = isSel(n);
		bool hit = ImGui::Selectable("##r",sel,ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick);
		bool hov = ImGui::IsItemHovered();
		bool tipOn = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
		if (hit) {
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				if (n.r) procwin::open(ref(*n.r));
				else toggle(n);
			} else clickRow(i);
		}
		if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
			if (!sel) {
				selClear();
				selNode(n,true);
				g.anchor = nodeId(n);
			}
			g.menu = true;
		}
		if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) middleClick(n);

		float h = ImGui::GetTextLineHeight();
		float ind = ui::px(14.0f);
		ImGui::SameLine(x0 + ind * (float)n.depth,0.0f);
		if (g.mode != MFlat) {
			if (n.kids) {
				ImVec2 p = ImGui::GetCursorScreenPos();
				if (ImGui::InvisibleButton("##t",ImVec2(ind,h))) toggle(n);
				chevron(p,ind,h,n.open,ImGui::GetColorU32(ImGui::IsItemHovered() ? ui::c.text : ui::c.dim));
			} else ImGui::Dummy(ImVec2(ind,h));
			ImGui::SameLine(0.0f,ui::px(2.0f));
		}
		// cut with ... in what the indent leaves, a group row keeps room for its count
		char cnt[16] = "";
		if (!n.r) snprintf(cnt,sizeof(cnt),"%d",r.children);
		float w = ImGui::GetContentRegionAvail().x - (n.r ? 0.0f : ImGui::CalcTextSize(cnt).x + ui::px(4.0f));
		const char* t = r.name.c_str();
		const char* e = t + r.name.size();
		ImVec2 ts = ImGui::CalcTextSize(t,e,false);
		ImVec2 p = ImGui::GetCursorScreenPos();
		w = std::max(w,1.0f);
		ImGui::Dummy(ImVec2(std::min(w,ts.x),ts.y));
		bool dimIt = r.pseudo || r.sys;
		if (dimIt) ImGui::PushStyleColor(ImGuiCol_Text,ui::c.dim);
		ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(),p,ImVec2(p.x + w,p.y + ts.y),p.x + w,t,e,&ts);
		if (dimIt) ImGui::PopStyleColor();
		if (!n.r) {
			ImGui::SameLine(0.0f,ui::px(4.0f));
			ImGui::TextDisabled("%s",cnt);
		}
		if (tipOn) rowTooltip(n);
	}

	void cell(const Node& n,int c) {
		if (c == CHist) {
			if (!n.r) return;
			auto h = histOf(n.r->pid,n.r->start);
			if (!h || h->privMb.size() < 2) return;
			float mx = 1.0f;
			for (float v : h->privMb) if (v > mx) mx = v;
			ui::sparkline("##h",h->privMb.data(),(int)h->privMb.size(),mx,ImVec2(0.0f,ImGui::GetTextLineHeight() / ui::px(1.0f)),ui::c.priv);
			return;
		}
		std::string t = cellOf(n,c);
		if (t.empty()) return;
		ImVec4 col = ui::c.text;
		if (t == "N/A") col = ui::c.dim;
		else if (c == CDPriv || c == CDWs) col = t[0] == '+' ? ui::c.warn : ui::c.good;
		else if (cols[c].kind == KCountDelta) col = ui::c.dim;
		ImGui::PushStyleColor(ImGuiCol_Text,col);
		if (cols[c].kind == KText) ui::textEllipsis(t.c_str());
		else ui::cellRight(t.c_str());
		ImGui::PopStyleColor();
	}

	void drawTable() {
		bool totals = cfg::getb("procTotals",true);
		ImGuiTableFlags fl = ImGuiTableFlags_ScrollX | ImGuiTableFlags_SortTristate | ImGuiTableFlags_SizingFixedFit;
		if (!ui::beginTable("##plist",COLS,ImVec2(0,0),fl)) return;
		// room TableHeader keeps for the sort arrow on every sortable column
		float arrow = std::floor(ImGui::GetFontSize() * 0.65f + ImGui::GetStyle().FramePadding.x);
		for (int c = 0; c < COLS; c++) {
			ImGuiTableColumnFlags f = ImGuiTableColumnFlags_WidthFixed;
			if (!cols[c].mem) f |= ImGuiTableColumnFlags_DefaultHide;
			if (c == CName) f |= ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder;
			if (c == CWs) f |= ImGuiTableColumnFlags_DefaultSort;
			if (cols[c].kind != KText && c != CPid) f |= ImGuiTableColumnFlags_PreferSortDescending;
			ImGui::TableSetupColumn(cols[c].title,f,std::max(ui::px(cols[c].w),ImGui::CalcTextSize(cols[c].title).x + arrow));
		}
		ImGui::TableSetupScrollFreeze(1,totals ? 2 : 1);
		if (g.preset) {
			for (int c = 0; c < COLS; c++) ImGui::TableSetColumnEnabled(c,g.preset == 2 || cols[c].mem);
			g.preset = 0;
		}
		if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
			if (sp->SpecsDirty) {
				g.sortCol = sp->SpecsCount > 0 ? sp->Specs[0].ColumnIndex : -1;
				g.desc = sp->SpecsCount > 0 && sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
				sp->SpecsDirty = false;
				g.dirty = true;
			}
		}
		for (int c = 0; c < COLS; c++) g.colOn[c] = (ImGui::TableGetColumnFlags(c) & ImGuiTableColumnFlags_IsEnabled) != 0;
		ImGuiTable* tb = ImGui::GetCurrentTable();
		for (int k = 0; k < COLS; k++) g.ord[k] = tb->DisplayOrderToIndex[k];
		if (g.colOn[CWsShared] || g.colOn[CWsShareable]) wantWs(2000);
		if (g.dirty) rebuild();

		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int c = 0; c < COLS; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			ImGui::PushID(c);
			ui::headerCell(cols[c].title,cols[c].tip,cols[c].kind != KText && c != CHist);
			ImGui::PopID();
		}
		if (totals) {
			ImGui::TableNextRow();
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,ImGui::GetColorU32(ui::c.panel2));
			ImGui::PushStyleColor(ImGuiCol_Text,ui::c.dim);
			for (int c = 0; c < COLS; c++) {
				if (!ImGui::TableSetColumnIndex(c)) continue;
				if (c == CName) ImGui::Text("Total of %d",g.shown);
				else if (!g.tot[c].empty()) ui::cellRight(g.tot[c].c_str());
			}
			ImGui::PopStyleColor();
		}
		if (g.nodes.empty()) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(CName);
			ImGui::TextDisabled("No process matches");
		}

		uint64_t now = util::nowMs();
		bool hlNew = cfg::getb("procHlNew",true);
		bool hlHot = cfg::getb("procHlGrowth",true);
		uint64_t hlMs = (uint64_t)cfg::geti("procHlMs",3000);
		ImVec4 cNew = ui::c.good;
		ImVec4 cHot = ui::c.bad;
		cNew.w = 0.16f;
		cHot.w = 0.20f;
		ImU32 uNew = ImGui::GetColorU32(cNew);
		ImU32 uHot = ImGui::GetColorU32(cHot);

		ImVec4 cSel = ui::c.accent;
		cSel.w = 0.22f;
		ImGui::PushStyleColor(ImGuiCol_Header,cSel);
		int jump = -1;
		if (g.jump) {
			g.jump = false;
			for (int i = 0; i < (int)g.nodes.size() && jump < 0; i++) if (isSel(g.nodes[i])) jump = i;
		}
		ImGuiListClipper clip;
		clip.Begin((int)g.nodes.size());
		if (jump >= 0) clip.IncludeItemByIndex(jump);
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
				const Node& n = g.nodes[i];
				ImGui::TableNextRow();
				if (n.r) {
					if (hlHot && n.r->hotMs && now - n.r->hotMs < hlMs) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,uHot);
					else if (hlNew && n.r->seenMs && now - n.r->seenMs < hlMs) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,uNew);
				}
				if (n.r) ImGui::PushID((int)n.r->pid);
				else ImGui::PushID(n.d->name.c_str());
				if (ImGui::TableSetColumnIndex(CName)) nameCell(i);
				for (int c = 1; c < COLS; c++) if (ImGui::TableSetColumnIndex(c)) cell(n,c);
				if (i == jump) ImGui::SetScrollHereY();
				ImGui::PopID();
			}
		}
		ImGui::PopStyleColor();
		ui::endTable();
	}

	// next control on this line when it fits, else on a new one
	void flow(float w) {
		ImGui::SameLine();
		if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
	}

	float btnW(const char* t) {
		return ImGui::CalcTextSize(t,nullptr,true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	}

	float chkW(const char* t) {
		return ImGui::CalcTextSize(t,nullptr,true).x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
	}

	void collapseAll(bool open) {
		const auto& rs = rows();
		if (g.mode == MTree) {
			g.closed.clear();
			if (!open) for (auto& r : rs) if (r.children) g.closed.insert(keyOf(r));
		} else {
			g.opened.clear();
			if (open) for (auto& r : g.extra) g.opened.insert(r.name);
		}
		g.dirty = true;
	}

	void drawBar() {
		ui::beginCard("##pbar",nullptr);
		if (ui::search("##pfilter",g.filter,sizeof(g.filter),"Filter : name, pid, user, path",220.0f)) {
			g.low = util::lower(g.filter);
			g.dirty = true;
		}
		static const char* const names[] = {"Flat","Tree","Group"};
		const ImVec4 tints[] = {ui::c.accent,ui::c.accent,ui::c.accent};
		float segW = btnW("Flat") + btnW("Tree") + btnW("Group");
		flow(segW);
		ImGui::BeginGroup();
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(0.0f,0.0f));
		ImGui::Dummy(ImVec2(1.0f,ImGui::GetStyle().FramePadding.y));
		ImGui::PopStyleVar();
		if (ui::segmented("##pmode",names,tints,3,g.mode)) {
			cfg::seti("procMode",g.mode);
			g.dirty = true;
		}
		ImGui::EndGroup();
		flow(chkW("Hide other users"));
		if (ImGui::Checkbox("Hide other users",&g.hideOther)) {
			cfg::setb("procHideOther",g.hideOther);
			g.dirty = true;
		}
		flow(chkW("Hide system"));
		if (ImGui::Checkbox("Hide system",&g.hideSys)) {
			cfg::setb("procHideSys",g.hideSys);
			g.dirty = true;
		}
		ui::tip("Hides pseudo and system processes, service accounts and images under the Windows folder. Linux : kernel threads.");

		if (ImGui::Button("Memory columns")) g.preset = 1;
		ui::tip("The memory view : working set, private WS, private bytes, deltas and history.");
		flow(btnW("All columns"));
		if (ImGui::Button("All columns")) g.preset = 2;
		ui::tip("Everything, Shared WS and Shareable WS included, those start a page walk per process. Right click a header to pick single columns.");
		flow(btnW("Export CSV"));
		if (ImGui::Button("Export CSV")) ImGui::OpenPopup("##pexport");
		ui::tip("Writes the rows and columns shown now, sizes in plain bytes.");
		if (ImGui::BeginPopup("##pexport")) {
			if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
			ImGui::TextDisabled("Save to");
			ImGui::SetNextItemWidth(ui::px(420.0f));
			if (ImGui::InputText("##ppath",g.path,sizeof(g.path))) cfg::sets("procExport",g.path);
			ImGui::BeginDisabled(g.exp != nullptr);
			if (ImGui::Button("Export")) {
				startExport(false);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled("Asks first when the file exists.");
			ImGui::EndPopup();
		}
		if (cfg::getb("memPaused",false)) {
			flow(ImGui::CalcTextSize("Paused").x + ui::px(12.0f));
			ui::badge("Paused",ui::c.warn);
			ui::tip("Sampling is paused in Settings, these numbers and deltas are not moving. F5 takes one sample.");
		}
		std::string se = snapErr();
		if (!se.empty()) {
			flow(ImGui::CalcTextSize("Snapshot failing").x + ui::px(12.0f));
			ui::badge("Snapshot failing",ui::c.bad);
			ui::tip((se + ". The list shows the last good snapshot, its numbers are not moving.").c_str());
		}
		bool hlNew = cfg::getb("procHlNew",true);
		bool hlHot = cfg::getb("procHlGrowth",true);
		if (hlNew || hlHot) {
			std::string hot = "Growing past " + util::count((uint64_t)cfg::geti("procHlGrowthMb",100)) + " MB/s";
			ui::Seg lg[] = {{hlNew ? 1.0f : 0.0f,ui::c.good,"New"},{hlHot ? 1.0f : 0.0f,ui::c.bad,hot.c_str()}};
			flow(ImGui::CalcTextSize(hlNew ? "New" : hot.c_str()).x + ImGui::GetFontSize());
			ImGui::BeginGroup();
			ImGui::AlignTextToFramePadding();
			ui::legend(lg,2);
			ImGui::EndGroup();
			char t[256];
			snprintf(t,sizeof(t),"Row tints, they last %.1f s. Red : private bytes or working set grew past the threshold between two samples, so the deltas shown can already be the next sample's or the other of the two. Settings, Processes sets them.",cfg::geti("procHlMs",3000) / 1000.0);
			ui::tip(t);
		}
		// last, so switching Flat, Tree, Group moves nothing before them
		if (g.mode != MFlat) {
			flow(btnW("Collapse all"));
			if (ImGui::Button("Collapse all")) collapseAll(false);
			flow(btnW("Expand all"));
			if (ImGui::Button("Expand all")) collapseAll(true);
		}

		char cnt[64];
		int total = (int)g.list->rows.size();
		if (g.shown != total) snprintf(cnt,sizeof(cnt),"%d of %d",g.shown,total);
		else snprintf(cnt,sizeof(cnt),"%d processes",total);
		float cw = ImGui::CalcTextSize(cnt).x;
		static const bool elev = isElevated();
		flow(cw + (elev ? 0.0f : ui::px(30.0f)));
		float av = ImGui::GetContentRegionAvail().x;
		float need = cw + (elev ? 0.0f : ui::px(30.0f));
		if (av > need) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + av - need);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s",cnt);
		if (!elev) {
			ImGui::SameLine();
#ifdef _WIN32
			ui::help("FRS is not elevated, so handle columns of other users' processes show N/A : path, page priority, min and max WS, bits, Shared WS. The numbers from the system snapshot are all there.");
#else
			ui::help("FRS is not root, so path, bits and Shared WS of other users' processes show N/A. Page priority, min and max WS never exist on Linux. The numbers from /proc are all there.");
#endif
		}
		ui::endCard();
	}

	void draw() {
		if (!g.loaded) {
			g.loaded = true;
			g.mode = std::max(0,std::min(2,cfg::geti("procMode",MFlat)));
			g.hideOther = cfg::getb("procHideOther",false);
			g.hideSys = cfg::getb("procHideSys",false);
			for (int k = 0; k < COLS; k++) g.ord[k] = k;
			std::string home;
			be::invoker(g.me,home);
			std::string p = cfg::gets("procExport","");
			if (p.empty()) p = vmem::homePath("frs_processes.csv");
			snprintf(g.path,sizeof(g.path),"%s",p.c_str());
		}
		if (!g.list || g.dirty || g.seq != latest()->seq) rebuild();
		drawBar();
		ImGui::Spacing();
		ui::beginCard("##plistc",nullptr,ImVec2(0.0f,-1.0f));
		if (g.list->rows.empty()) {
			std::string e = snapErr();
			ui::empty(e.empty() ? "Waiting for the first process snapshot" : e.c_str());
		} else drawTable();
		ui::endCard();
		drawMenu();
		pollExport();

		ImGuiIO& io = ImGui::GetIO();
		if (!io.WantTextInput && !g.sel.empty() && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)) {
			if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) copyRows();
			if (ImGui::IsKeyPressed(ImGuiKey_Enter,false) && g.sel.size() == 1) procwin::open(g.sel[0]);
		}
	}

}

namespace procs {

	void initPage() {
		nav::addPage("processes","Processes",ui::IcoProc,nav::GrpMain,draw);
	}

	std::vector<ProcRef> selected() {
		return g.sel;
	}

	void select(const std::vector<ProcRef>& v) {
		selClear();
		auto l = latest();
		for (auto& x : v) {
			const ProcRow* p = l->find(x);
			if (!p) continue;
			// a jump that lands on nothing helps nobody : the text filter and a collapsed parent make way
			if (!rowMatch(*p,g.low)) {
				g.filter[0] = 0;
				g.low.clear();
			}
			for (const ProcRow* q = parentOf(*l,*p); q; q = parentOf(*l,*q)) g.closed.erase(keyOf(*q));
			selAdd(*p);
		}
		g.anchor.clear();
		g.jump = true;
		g.dirty = true;
	}

}
