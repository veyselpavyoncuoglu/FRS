#include "vmem.h"
#include "cfg.h"
#include "ui.h"
#include "vmscan.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <set>
#include <utility>

namespace {

	using namespace vmem;

	enum Col {
		CBase,CType,CSize,CProt,CUse,CWs,CWsPriv,CWsShareable,CWsShared,CWsLocked,CCommitted,CPriv,CSigning,COrigProt,COrigPages,CModPages,CRegionType,CPrio,COLS
	};

	struct ColDef {
		const char* title;
		const char* tip;
		float w;
		bool on,num;
	};

	const ColDef cols[COLS] = {
		{"Base address","Start of the region, on a parent row the allocation base.",124,true,false},
		{"Type","Private is this process only. Mapped is a view of a file or of shared memory. Image is an executable file. Linux : read from the mapping flags.",100,true,false},
		{"Size","Address space the region covers, not memory in use.",66,true,true},
		{"Protection","Current protection. G guard page, NC no cache, WCM write combine, WC copy on write. On a parent row, the protection it was allocated with.",64,true,false},
		{"Use","What the region is for, from the PEB, the TEBs and the mapped file names. This text comes from the target.",190,true,false},
		{"Total WS","Resident bytes in this region, shared pages included. A trim moves them to standby and they fault back.",64,true,true},
		{"Private WS","Resident pages only this process uses.",66,true,true},
		{"Shareable WS","Resident pages that could be shared.",84,false,true},
		{"Shared WS","Resident pages shared with another process right now.",66,true,true},
		{"Locked WS","Resident pages locked in memory, a trim can't move them.",66,false,true},
		{"Committed","Bytes with commit, derived from the state.",70,false,true},
		{"Private","Private committed bytes. Linux : smaps Anonymous.",60,false,true},
		{"Signing level","Code signing level of the image.",80,false,false},
		{"Original protection","Protection when it was allocated. Linux : the mr mw me flags, the closest there is.",118,false,false},
		{"Original pages","Share of a mapped or image region resident and untouched, then how many pages are not. Non resident pages count as not.",92,false,true},
		{"Modified pages","Share of a mapped or image region with modified copy on write pages, then how many. Shows code patches.",94,true,true},
		{"Region type","Region flags from the kernel. Linux : VmFlags.",140,false,false},
		{"Priority","Highest memory priority of the resident pages. Lower priority pages leave standby first.",96,false,false},
	};

	struct Node {
		const Region* d;
		bool parent;
		int depth;
		bool open;
		int kids;
	};

	struct SecState {
		std::shared_ptr<task::T> task;
		std::shared_ptr<std::vector<Section>> rows;
		std::shared_ptr<std::string> err;
		bool asked = false;
		std::shared_ptr<task::T> findTask;
		std::shared_ptr<std::vector<MapUser>> users;
		std::shared_ptr<int> unread;
		char q[260] = "";
		std::string searched;
		int sel = -1,userSel = -1;
		bool menu = false,userMenu = false;
	};

	struct Tab {
		ProcRef ref;
		uint64_t used = 0;
		std::shared_ptr<const RegionList> list;
		std::vector<Node> nodes;
		std::deque<Region> parents;
		std::set<uint64_t> open;
		std::set<uint64_t> sel;
		uint64_t anchor = NA;
		char q[128] = "";
		bool dirty = true,focusQ = false,resetSort = false,fitBase = false;
		std::string rxErr,rxSlow;
		int sortCol = -1;
		bool desc = false;
		int shown = 0;
		bool menu = false;
		int hoverCol = -1;
		uint64_t cm = 0,pv = 0,ws = 0,img = 0,map = 0,prv = 0,res = 0;
		int view = 0;
		Region act;
		bool askFree = false,askDec = false;
		bool openProt = false,openAddr = false,openSave = false,saveList = false,addrFocus = false;
		char protTxt[32] = "";
		int protIdx = 2;
		bool protG = false,protNc = false,protWc = false;
		std::string protErr;
		std::shared_ptr<task::T> protTask;
		std::shared_ptr<std::string> protRes;
		char addrTxt[64] = "";
		std::string addrErr;
		char path[512] = "";
		std::vector<Range> ranges;
		std::shared_ptr<task::T> save;
		SecState sec;
		bool colOn[COLS];
		Tab() {
			for (int c = 0; c < COLS; c++) colOn[c] = cols[c].on;
		}
	};

	std::map<std::pair<uint32_t,uint64_t>,Tab> g_tabs;

	const char* baseProts[] = {"NA","R","RW","WC","X","RX","RWX","WCX"};

	std::string addrText(uint64_t v) {
		if (!cfg::getb("regZeroPad",false)) return util::hex(v);
		char b[32];
		snprintf(b,sizeof(b),"0x%016llx",(unsigned long long)v);
		return b;
	}

	std::string szBlank(uint64_t v) {
		if (v == NA) return "N/A";
		return v ? util::size(v) : "";
	}

	std::string typeText(const Region& d,bool parent) {
		if (d.state == StFree) return d.unusable ? "Free (Unusable)" : "Free";
		std::string t = typeName(d.type);
		if (parent) return t;
		if (t.empty()) return stateName(d.state);
		return t + " : " + stateName(d.state);
	}

	std::string pct(double frac,uint64_t count,bool always) {
		char b[64];
		int pr = std::max(0,std::min(3,cfg::geti("uiPctPrec",1)));
		if (count || always) snprintf(b,sizeof(b),"%.*f%% (%llu)",pr,frac * 100.0,(unsigned long long)count);
		else snprintf(b,sizeof(b),"%.*f%%",pr,frac * 100.0);
		return b;
	}

	std::string cellText(const Region& d,bool parent,int c) {
		switch (c) {
			case CBase: return addrText(d.base);
			case CType: return typeText(d,parent);
			case CSize: return util::size(d.size);
			case CProt: return parent ? d.allocProt : d.prot;
			case CUse: return d.use;
			case CWs: return szBlank(d.ws);
			case CWsPriv: return szBlank(d.wsPriv);
			case CWsShareable: return szBlank(d.wsShareable);
			case CWsShared: return szBlank(d.wsShared);
			case CWsLocked: return szBlank(d.wsLocked);
			case CCommitted: return szBlank(d.committed);
			case CPriv: return szBlank(d.priv);
			case CSigning: {
				const char* s = signingName(d.signing);
				return s ? s : "";
			}
			case COrigProt: return parent ? "" : d.allocProt;
			case COrigPages: {
				if (parent || d.pagesOrig == NA || !d.size) return "";
				uint64_t all = d.size / 4096;
				return pct((double)d.pagesOrig / (double)all,all - d.pagesOrig,false);
			}
			case CModPages: {
				if (parent || d.pagesMod == NA || !d.committed) return "";
				return pct((double)d.pagesMod / (double)(d.committed / 4096),d.pagesMod,true);
			}
			case CRegionType: return parent ? "" : d.flags;
			case CPrio: {
				const char* p = prioName(d.prio);
				return p ? std::string(p) + " (" + std::to_string(d.prio) + ")" : "";
			}
		}
		return "";
	}

	// CSV gets plain numbers like the Processes export, the text list what the table shows
	std::string rawCell(const Region& d,int c) {
		uint64_t v = NA;
		switch (c) {
			case CSize: v = d.size; break;
			case CWs: v = d.ws; break;
			case CWsPriv: v = d.wsPriv; break;
			case CWsShareable: v = d.wsShareable; break;
			case CWsShared: v = d.wsShared; break;
			case CWsLocked: v = d.wsLocked; break;
			case CCommitted: v = d.committed; break;
			case CPriv: v = d.priv; break;
			default: return cellText(d,false,c);
		}
		return v == NA ? "N/A" : std::to_string(v);
	}

	uint64_t keyOf(const Node& n) {
		return (n.d->base << 1) | (n.parent ? 1 : 0);
	}

	void addTo(uint64_t& a,uint64_t b) {
		a = a == NA || b == NA ? NA : a + b;
	}

	struct Search {
		std::string low;
		bool on = false,isNum = false,useCase = false,useRx = false;
		uint64_t num = 0;
		vmscan::Rx rx; // no backtracking, a pattern can't hang the UI thread the way std::regex does
	};

	bool hit(const Search& s,const std::string& t) {
		if (s.useRx) return s.rx.match(t.data(),std::min(t.size(),(size_t)1024));
		if (s.useCase) return t.find(s.low) != std::string::npos;
		return util::lower(t).find(s.low) != std::string::npos;
	}

	bool matches(const Region& x,const Search& s) {
		if (!s.on) return true;
		if (s.isNum && (s.num == x.allocBase || (s.num >= x.base && s.num - x.base < x.size))) return true;
		return hit(s,addrText(x.base)) || hit(s,typeName(x.type)) || hit(s,stateName(x.state)) || hit(s,x.use);
	}

	bool visible(const Region& x) {
		if (x.state == StFree && cfg::getb("regHideFree",true)) return false;
		if (x.state == StReserve && x.type != TyNone && cfg::getb("regHideReserved",true)) return false;
		if (x.guard && cfg::getb("regHideGuard",false)) return false;
		if (x.type == TyMapped && cfg::getb("regHideMapped",false)) return false;
		return true;
	}

	// keys are made once, building cell text inside the comparator is what hitched on big lists
	void sortRows(std::vector<const Region*>& v,int c,bool desc) {
		struct Key {
			uint64_t n,base;
			const Region* d;
			std::string s;
		};
		std::vector<Key> ks;
		ks.reserve(v.size());
		for (auto* d : v) {
			uint64_t n = 0;
			std::string s;
			switch (c) {
				case CBase: n = d->base; break;
				case CSize: n = d->size; break;
				case CWs: n = d->ws; break;
				case CWsPriv: n = d->wsPriv; break;
				case CWsShareable: n = d->wsShareable; break;
				case CWsShared: n = d->wsShared; break;
				case CWsLocked: n = d->wsLocked; break;
				case CCommitted: n = d->committed; break;
				case CPriv: n = d->priv; break;
				case COrigPages: n = d->pagesOrig; break;
				case CModPages: n = d->pagesMod; break;
				case CSigning: n = d->signing < 0 ? NA : (uint64_t)d->signing; break;
				case CPrio: n = d->prio < 0 ? NA : (uint64_t)d->prio; break;
				default: s = cellText(*d,false,c); break;
			}
			ks.push_back({n,d->base,d,std::move(s)});
		}
		std::sort(ks.begin(),ks.end(),[desc](const Key& a,const Key& b) {
			// unknown goes last both ways
			if (a.n != b.n) {
				if (a.n == NA || b.n == NA) return b.n == NA;
				return desc ? a.n > b.n : a.n < b.n;
			}
			int r = a.s.compare(b.s);
			if (r) return desc ? r > 0 : r < 0;
			return a.base < b.base;
		});
		for (size_t i = 0; i < ks.size(); i++) v[i] = ks[i].d;
	}

	void rebuild(Tab& t) {
		t.dirty = false;
		t.nodes.clear();
		t.parents.clear();
		t.shown = 0;
		t.rxErr.clear();
		t.cm = t.pv = t.ws = t.img = t.map = t.prv = t.res = 0;
		if (!t.list) return;
		auto& rows = t.list->rows;
		for (auto& x : rows) {
			t.cm += x.committed;
			if (x.priv != NA) t.pv += x.priv;
			if (x.ws != NA) t.ws += x.ws;
			if (x.state == StCommit) {
				if (x.type == TyImage) t.img += x.size;
				else if (x.type == TyMapped) t.map += x.size;
				else t.prv += x.size;
			} else if (x.state == StReserve) t.res += x.size;
		}

		Search s;
		s.on = t.q[0] != 0;
		s.useCase = cfg::getb("regSearchCase",false);
		s.useRx = cfg::getb("regSearchRegex",false);
		if (s.on) {
			s.low = s.useCase ? std::string(t.q) : util::lower(t.q);
			s.isNum = util::parseHex(t.q,s.num);
			if (s.useRx && !s.rx.compile(t.q,!s.useCase,t.rxErr)) {
				s.on = false;
				s.useRx = false;
			}
		}
		// no backtracking means no hang, but a huge pattern on a huge list can still take a minute. Give up once and remember the pattern
		std::vector<char> rxHit;
		if (s.on && s.useRx) {
			bool slow = t.rxSlow == t.q;
			uint64_t until = util::nowMs() + 500;
			if (!slow) rxHit.resize(rows.size());
			for (size_t i = 0; i < rxHit.size() && !slow; i++) {
				rxHit[i] = visible(rows[i]) && matches(rows[i],s);
				slow = util::nowMs() > until;
			}
			if (slow) {
				rxHit.clear();
				t.rxSlow = t.q;
				t.rxErr = "too slow for this many regions";
				s.on = false;
				s.useRx = false;
			}
		}
		auto pass = [&](const Region& x) { return rxHit.empty() ? visible(x) && matches(x,s) : rxHit[&x - rows.data()] != 0; };

		if (t.sortCol >= 0) {
			std::vector<const Region*> v;
			for (auto& x : rows) if (pass(x)) v.push_back(&x);
			sortRows(v,t.sortCol,t.desc);
			t.nodes.reserve(v.size());
			for (auto* x : v) t.nodes.push_back({x,false,0,false,0});
			t.shown = (int)v.size();
			return;
		}

		for (size_t i = 0; i < rows.size();) {
			size_t j = i + 1;
			while (j < rows.size() && rows[j].allocBase == rows[i].allocBase) j++;
			std::vector<const Region*> vis;
			for (size_t k = i; k < j; k++) if (pass(rows[k])) vis.push_back(&rows[k]);
			if (!vis.empty()) {
				if (j - i == 1) t.nodes.push_back({vis[0],false,0,false,0}); // the region row has what the parent would not
				else {
					Region p;
					const Region& f = rows[i];
					p.base = f.allocBase;
					p.allocBase = f.allocBase;
					p.state = f.state;
					p.type = f.type;
					p.allocProt = f.allocProt;
					p.ex = f.allocEx;
					p.kind = f.kind;
					p.use = f.use;
					p.file = f.file;
					p.signing = f.signing;
					p.ws = p.wsPriv = p.wsShareable = p.wsShared = p.wsLocked = 0;
					for (size_t k = i; k < j; k++) {
						const Region& x = rows[k];
						p.size += x.size;
						p.committed += x.committed;
						addTo(p.priv,x.priv);
						addTo(p.ws,x.ws);
						addTo(p.wsPriv,x.wsPriv);
						addTo(p.wsShareable,x.wsShareable);
						addTo(p.wsShared,x.wsShared);
						addTo(p.wsLocked,x.wsLocked);
						p.ex = p.ex || x.ex;
						if (x.pagesMod != NA) p.pagesMod = (p.pagesMod == NA ? 0 : p.pagesMod) + x.pagesMod; // for the tint, the cell stays blank
					}
					t.parents.push_back(std::move(p));
					bool open = s.on || t.open.count(f.allocBase);
					t.nodes.push_back({&t.parents.back(),true,0,open,(int)vis.size()});
					if (open) for (auto* x : vis) t.nodes.push_back({x,false,1,false,0});
				}
				t.shown += (int)vis.size();
			}
			i = j;
		}
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

	std::vector<const Node*> selected(const Tab& t) {
		std::vector<const Node*> v;
		for (auto& n : t.nodes) if (t.sel.count(keyOf(n))) v.push_back(&n);
		return v;
	}

	void toggle(Tab& t,const Node& n) {
		if (n.open) t.open.erase(n.d->base);
		else t.open.insert(n.d->base);
		t.dirty = true;
	}

	void clickRow(Tab& t,int i) {
		const Node& n = t.nodes[i];
		uint64_t k = keyOf(n);
		ImGuiIO& io = ImGui::GetIO();
		if (io.KeyShift && t.anchor != NA) {
			int a = -1;
			for (int j = 0; j < (int)t.nodes.size(); j++) if (keyOf(t.nodes[j]) == t.anchor) a = j;
			if (a >= 0) {
				if (!io.KeyCtrl) t.sel.clear();
				for (int j = std::min(a,i); j <= std::max(a,i); j++) t.sel.insert(keyOf(t.nodes[j]));
				return;
			}
		}
		if (io.KeyCtrl) {
			if (!t.sel.erase(k)) t.sel.insert(k);
		} else {
			t.sel.clear();
			t.sel.insert(k);
		}
		t.anchor = k;
	}

	void openEditor(const ProcRef& r,const Region& x) {
		hexOpen(r,x.base,x.size);
	}

	void baseCell(Tab& t,int i,const ProcRef& r,bool tree) {
		const Node n = t.nodes[i];
		float x0 = ImGui::GetCursorPosX();
		bool sel = t.sel.count(keyOf(n)) != 0;
		bool click = ImGui::Selectable("##r",sel,ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick);
		bool hov = ImGui::IsItemHovered();
		if (click) {
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				if (n.parent) toggle(t,n);
				else if (n.d->state != StFree && t.list->prot.empty()) openEditor(r,*n.d);
			} else clickRow(t,i);
		}
		if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
			if (!sel) {
				t.sel.clear();
				t.sel.insert(keyOf(n));
				t.anchor = keyOf(n);
			}
			t.hoverCol = ImGui::TableGetHoveredColumn();
			t.menu = true;
		}
		float h = ImGui::GetTextLineHeight();
		float ind = ui::px(14.0f);
		ImGui::SameLine(x0 + ind * (float)n.depth,0.0f);
		if (tree) {
			if (n.parent) {
				ImVec2 p = ImGui::GetCursorScreenPos();
				if (ImGui::InvisibleButton("##t",ImVec2(ind,h))) toggle(t,n);
				chevron(p,ind,h,n.open,ImGui::GetColorU32(ImGui::IsItemHovered() ? ui::c.text : ui::c.dim));
			} else ImGui::Dummy(ImVec2(ind,h));
			ImGui::SameLine(0.0f,ui::px(2.0f));
		}
		ui::pushMonoCell();
		std::string s = addrText(n.d->base);
		ImGui::TextUnformatted(s.c_str());
		ui::popFont();
		if (n.parent) {
			ImGui::SameLine(0.0f,ui::px(4.0f));
			ImGui::TextDisabled("%d",n.kids);
		}
	}

	void plainCell(const Node& n,int c) {
		std::string s = cellText(*n.d,n.parent,c);
		if (s.empty()) return;
		if (s == "N/A") ImGui::PushStyleColor(ImGuiCol_Text,ui::c.dim);
		if (cols[c].num) ui::cellRight(s.c_str());
		else {
			ui::textEllipsis(s.c_str());
			if (c == CUse || c == CRegionType) ui::tip(s.c_str());
		}
		if (s == "N/A") ImGui::PopStyleColor();
	}

	ImU32 rowTint(const Region& d) {
		ImVec4 c(0,0,0,0);
		// first match wins, same order as System Informer with mapped added before private
		if (d.ex && cfg::getb("regHlExec",false)) c = ui::c.purple;
		else if ((d.kind == KCfg || d.kind == KCfg32) && cfg::getb("regHlCfg",false)) c = ui::c.bad;
		else if (d.type == TyImage && cfg::getb("regHlSystem",false)) c = ui::c.info;
		else if (d.pagesMod != NA && d.pagesMod > 0 && cfg::getb("regHlModified",false)) c = ui::c.warn;
		else if (d.type == TyMapped && cfg::getb("regHlMapped",false)) c = ui::c.gold;
		else if (d.type == TyPrivate && cfg::getb("regHlPrivate",false)) c = ui::c.priv;
		if (c.w == 0.0f) return 0;
		c.w = 0.16f;
		return ImGui::GetColorU32(c);
	}

	void copyRows(const Tab& t,int only) {
		std::string s;
		for (auto* n : selected(t)) {
			bool first = true;
			for (int c = 0; c < COLS; c++) {
				if (only >= 0 && c != only) continue;
				if (only < 0 && !t.colOn[c]) continue;
				if (!first) s += '\t';
				first = false;
				s += cellText(*n->d,n->parent,c);
			}
			s += '\n';
		}
		if (!s.empty()) ImGui::SetClipboardText(s.c_str());
	}

	bool beginRegTable() {
		// one id for all processes, so the layout is the same everywhere
		ImGuiTableFlags fl = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate |
			ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
		return ImGui::BeginTableEx("##regs",ImHashStr("frsRegionsTable"),COLS,fl,ImVec2(0,0));
	}

	void drawTable(Tab& t,const ProcRef& r) {
		if (!beginRegTable()) return;
		float arrow = std::floor(ImGui::GetFontSize() * 0.65f + ImGui::GetStyle().FramePadding.x);
		ui::pushMono();
		float baseW = ImGui::CalcTextSize(cfg::getb("regZeroPad",false) ? "0x0000000000000000" : "0x7fffffffffff").x + ui::px(44.0f);
		ui::popFont();
		for (int c = 0; c < COLS; c++) {
			ImGuiTableColumnFlags f = ImGuiTableColumnFlags_WidthFixed;
			if (!cols[c].on) f |= ImGuiTableColumnFlags_DefaultHide;
			if (c == CBase) f |= ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder;
			if (cols[c].num) f |= ImGuiTableColumnFlags_PreferSortDescending;
			ImGui::TableSetupColumn(cols[c].title,f,c == CBase ? baseW : std::max(ui::px(cols[c].w),ImGui::CalcTextSize(cols[c].title).x + arrow));
		}
		ImGui::TableSetupScrollFreeze(1,1);
		if (t.fitBase) {
			t.fitBase = false;
			ImGui::TableSetColumnWidth(CBase,baseW);
		}
		if (t.resetSort) {
			t.resetSort = false;
			if (t.sortCol >= 0) ImGui::TableSetColumnSortDirection(t.sortCol,ImGuiSortDirection_None,false);
		}
		if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
			int sc = sp->SpecsCount > 0 ? sp->Specs[0].ColumnIndex : -1;
			bool d = sp->SpecsCount > 0 && sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
			if (sc != t.sortCol || d != t.desc) {
				t.sortCol = sc;
				t.desc = d;
				t.dirty = true;
			}
		}
		for (int c = 0; c < COLS; c++) {
			t.colOn[c] = (ImGui::TableGetColumnFlags(c) & ImGuiTableColumnFlags_IsEnabled) != 0;
		}
		if (t.dirty) rebuild(t);

		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int c = 0; c < COLS; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			ImGui::PushID(c);
			ui::headerCell(cols[c].title,cols[c].tip,cols[c].num);
			ImGui::PopID();
		}
		bool tree = t.sortCol < 0;
		ImVec4 cSel = ui::c.accent;
		cSel.w = 0.22f;
		ImGui::PushStyleColor(ImGuiCol_Header,cSel);
		ImGuiListClipper clip;
		clip.Begin((int)t.nodes.size());
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
				ImGui::TableNextRow();
				Node n = t.nodes[i];
				ImU32 tint = rowTint(*n.d);
				if (tint) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,tint);
				ImGui::PushID(i);
				for (int c = 0; c < COLS; c++) {
					if (!ImGui::TableSetColumnIndex(c)) continue;
					if (c == CBase) baseCell(t,i,r,tree);
					else plainCell(n,c);
				}
				ImGui::PopID();
			}
		}
		ImGui::PopStyleColor();
		ImGui::EndTable();
	}

	void runAction(const ProcRef& r,const Region& x,int what) {
		task::run([r,x,what](task::T&) -> std::string {
			std::string where = " (pid " + std::to_string(r.pid) + ", " + util::hex(x.base) + ")";
			std::string err,did,note;
			std::string s;
			if (what == 0 || what == 1) {
				bool dec = what == 1;
				bool ok = r.pid != procs::be::selfPid() && be::freeMem(r,x.base,dec ? x.size : 0,dec,did,err);
				if (r.pid == procs::be::selfPid()) err = "that is FRS itself";
				const char* lb = dec ? "Decommit memory" : x.type == TyPrivate ? "Free memory" : "Unmap view";
				s = lb + where + (ok ? " : OK" : " : failed (" + util::safe(err) + ")");
			} else {
				bool ok = be::emptyWs(r,x.base,x.size,note,err);
				s = "Reduce region working set" + where + (ok ? " : OK" + (note.empty() ? "" : " (" + note + ")") : " : failed (" + util::safe(err) + ")");
			}
			ui::toast(s);
			changed(r);
			return s;
		});
	}

	void startSave(Tab& t,const ProcRef& r,bool over) {
		std::string path = t.path;
		if (t.saveList) {
			auto l = t.list;
			t.save = task::run([r,l,path,over](task::T&) -> std::string {
				std::string lb = "Save region list (pid " + std::to_string(r.pid) + ")";
				bool csv = path.size() > 4 && util::lower(path.substr(path.size() - 4)) == ".csv";
				std::string s = "\xEF\xBB\xBF";
				char sep = csv ? ',' : '\t';
				auto cell = [&](std::string v) {
					if (csv && v.find_first_of(",\"\n") != std::string::npos) {
						std::string o = "\"";
						for (char ch : v) {
							if (ch == '"') o += '"';
							o += ch;
						}
						return o + "\"";
					}
					return v;
				};
				for (int c = 0; c < COLS; c++) {
					if (c) s += sep;
					s += cols[c].title;
				}
				s += '\n';
				for (auto& x : l->rows) {
					for (int c = 0; c < COLS; c++) {
						if (c) s += sep;
						s += csv ? cell(rawCell(x,c)) : cellText(x,false,c);
					}
					s += '\n';
				}
				int rc = 0;
				std::string err;
				void* f = be::fileOpen(path,over,rc,err);
				if (rc == 1) return "EXISTS";
				if (!f) return lb + " : failed (" + util::safe(err) + ")";
				bool ok = be::fileWrite(f,s.data(),s.size());
				be::fileClose(f);
				return lb + (ok ? " : OK (" + util::safe(path) + ")" : " : failed (could not write the file)");
			},false);
			return;
		}
		auto rs = t.ranges;
		t.save = task::run([r,rs,path,over](task::T& k) -> std::string {
			return saveRanges(r,rs,path,over,k);
		},false);
	}

	void pollSave(Tab& t,const ProcRef& r) {
		std::string askId = "Overwrite file ?##regover" + std::to_string(r.pid);
		if (t.save && t.save->done) {
			std::string s = t.save->res;
			t.save.reset();
			if (s == "EXISTS") ui::askOpen(askId.c_str());
			else {
				appLog(s);
				ui::toast(s);
			}
		}
		if (ui::askDraw(askId.c_str(),"That file exists. Replace it?","Overwrite",true)) startSave(t,r,true);
	}

	void drawMenu(Tab& t,const ProcRef& r) {
		if (t.menu) {
			ImGui::OpenPopup("##regmenu");
			t.menu = false;
		}
		if (!ImGui::BeginPopup("##regmenu")) return;
		if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
		auto sel = selected(t);
		bool one = sel.size() == 1 && !sel[0]->parent;
		const Region* x = one ? sel[0]->d : nullptr;
		bool isFree = x && x->state == StFree;
		bool mapped = x && (x->type == TyMapped || x->type == TyImage);
		bool allParents = !sel.empty(),allRegions = !sel.empty(),hasData = false;
		for (auto* n : sel) {
			if (n->parent) allRegions = false;
			else allParents = false;
			if (n->d->committed) hasData = true;
		}
		bool prot = !t.list->prot.empty();
		bool chg = be::canChange();
		auto why = [](const char* s) {
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s",s);
		};
		if (ImGui::MenuItem("Read/Write memory...","Enter",false,one && !isFree && !prot)) openEditor(r,*x);
		if (prot) why("Protected process, memory contents need the signed kernel driver FRS does not have.");
		if (ImGui::MenuItem("Change protection...",nullptr,false,one && !isFree && chg && !prot)) {
			t.act = *x;
			t.openProt = true;
		}
		if (!chg) why("Not available on Linux, there is no remote mprotect without injecting code.");
		if (ImGui::MenuItem("Reduce working set",nullptr,false,one && !isFree && !prot)) runAction(r,*x,2);
		why("Moves the resident pages of this region to standby. Nothing is lost, they fault back when used.");
		ImGui::Separator();
		if (ImGui::MenuItem(mapped ? "Unmap..." : "Free...",nullptr,false,one && !isFree && chg && !prot)) {
			t.act = *x;
			t.askFree = true;
		}
		if (!chg) why("Not available on Linux, there is no remote munmap without injecting code.");
		if (ImGui::MenuItem("Decommit...",nullptr,false,one && !isFree && !mapped && chg && !prot)) {
			t.act = *x;
			t.askDec = true;
		}
		if (!chg) why("Not available on Linux, the kernel refuses MADV_DONTNEED from another process.");
		ImGui::Separator();
		if (ImGui::MenuItem("Save...",nullptr,false,(allParents || allRegions) && hasData && !prot)) {
			t.ranges.clear();
			for (auto* n : sel) {
				if (n->d->committed) t.ranges.push_back({n->d->base,n->d->size});
			}
			std::string tail;
			if (t.ranges.size() == 1) tail = util::hex(t.ranges[0].addr) + "-" + util::hex(t.ranges[0].addr + t.ranges[0].size) + ".bin";
			else tail = "regions.bin";
			snprintf(t.path,sizeof(t.path),"%s",homePath(fileName(r,tail)).c_str());
			t.saveList = false;
			t.openSave = true;
		}
		why(hasData || sel.empty() ? "Writes the bytes of the selected regions to one file, unreadable pages as zeros so the offsets stay true." : "Nothing in the selection is committed. Reserved and free memory hold no bytes to save.");
		if (ImGui::MenuItem("Copy","Ctrl+C",false,!sel.empty())) copyRows(t,-1);
		if (t.hoverCol >= 0 && t.hoverCol < COLS) {
			std::string cc = std::string("Copy \"") + cols[t.hoverCol].title + "\"";
			if (ImGui::MenuItem(cc.c_str(),nullptr,false,!sel.empty())) copyRows(t,t.hoverCol);
		}
		ImGui::EndPopup();
	}

	void optToggle(Tab& t,const char* label,const char* key,bool def,const char* tip = nullptr) {
		bool on = cfg::getb(key,def);
		if (ImGui::MenuItem(label,nullptr,on)) {
			cfg::setb(key,!on);
			t.dirty = true;
		}
		if (tip) ui::tip(tip);
	}

	void drawOptions(Tab& t,const ProcRef& r) {
		if (!ImGui::BeginPopup("##regopt")) return;
		if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
		optToggle(t,"Hide free pages","regHideFree",true);
		optToggle(t,"Hide reserved pages","regHideReserved",true);
		optToggle(t,"Hide guard pages","regHideGuard",false);
		optToggle(t,"Hide mapped views","regHideMapped",false);
		ImGui::Separator();
		optToggle(t,"Highlight executable pages","regHlExec",false);
#ifdef _WIN32
		optToggle(t,"Highlight CFG pages","regHlCfg",false);
#endif
		optToggle(t,"Highlight system pages","regHlSystem",false,"Image memory. Linux : file backed code.");
		optToggle(t,"Highlight modified pages","regHlModified",false,"Mapped or image regions with copy on write pages. Shows code patches.");
		optToggle(t,"Highlight mapped views","regHlMapped",false);
		optToggle(t,"Highlight private pages","regHlPrivate",false);
		ImGui::Separator();
		bool zp = cfg::getb("regZeroPad",false);
		optToggle(t,"Zero pad addresses","regZeroPad",false);
		if (zp != cfg::getb("regZeroPad",false)) t.fitBase = true;
		optToggle(t,"Search : case sensitive","regSearchCase",false);
		optToggle(t,"Search : regular expression","regSearchRegex",false);
		if (ImGui::MenuItem("Reset sort",nullptr,false,t.sortCol >= 0)) t.resetSort = true;
		ui::tip("Back to the tree by allocation base.");
		ImGui::Separator();
		if (ImGui::MenuItem("Read/Write address...",nullptr,false,t.list && t.list->prot.empty())) t.openAddr = true;
		if (ImGui::MenuItem("Heaps...")) procwin::select(r,"Heaps");
		if (ImGui::MenuItem("Modified...")) procwin::select(r,"Diagnostics");
		if (ImGui::MenuItem("Strings...")) procwin::select(r,"Strings");
		if (ImGui::MenuItem("CFG bitmap...")) procwin::select(r,"Diagnostics");
		if (ImGui::MenuItem("Save list...",nullptr,false,t.list && !t.list->rows.empty())) {
			snprintf(t.path,sizeof(t.path),"%s",homePath(fileName(r,"regions.csv")).c_str());
			t.saveList = true;
			t.openSave = true;
		}
		ui::tip("All regions, flat, whatever the filters show. .csv gets commas, anything else tabs.");
		ImGui::EndPopup();
	}

	void statPair(const char* k,const std::string& v) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s",k);
		ImGui::SameLine(0.0f,ui::px(4.0f));
		ImGui::TextUnformatted(v.c_str());
		ImGui::SameLine(0.0f,ui::px(14.0f));
	}

	void drawDialogs(Tab& t,const ProcRef& r) {
		std::string pid = std::to_string(r.pid);
		std::string freeId = "Free memory region ?##regfree" + pid;
		std::string unmapId = "Unmap section view ?##regunmap" + pid;
		std::string decId = "Decommit memory region ?##regdec" + pid;
		if (t.askFree) {
			t.askFree = false;
			ui::askOpen(t.act.type == TyPrivate ? freeId.c_str() : unmapId.c_str());
		}
		if (t.askDec) {
			t.askDec = false;
			ui::askOpen(decId.c_str());
		}
		std::string what = "Region " + util::hex(t.act.base) + ", " + util::size(t.act.size) + ".\n\n";
		std::string txt = what + "Freeing memory regions may cause the process to crash.\n\nSome programs may also restrict access or ban your account when freeing the memory of the process.\n\nAre you sure you want to continue?";
		if (ui::askDraw(freeId.c_str(),txt.c_str(),"Free",true)) runAction(r,t.act,0);
		txt = what + "Unmapping a section view may cause the process to crash.\n\nSome programs may also restrict access or ban your account when unmapping the memory of the process.\n\nAre you sure you want to continue?";
		if (ui::askDraw(unmapId.c_str(),txt.c_str(),"Unmap",true)) runAction(r,t.act,0);
		txt = what + "Decommitting memory regions may cause the process to crash.\n\nSome programs may also restrict access or ban your account when decommitting the memory of the process.\n\nAre you sure you want to continue?";
		if (ui::askDraw(decId.c_str(),txt.c_str(),"Decommit",true)) runAction(r,t.act,1);

		if (t.openProt) {
			t.openProt = false;
			t.protErr.clear();
			t.protTxt[0] = 0;
			t.protIdx = 2;
			t.protG = t.protNc = t.protWc = false;
			ImGui::OpenPopup("Change protection##regprot");
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,ImVec2(0.5f,0.5f));
		if (ImGui::BeginPopupModal("Change protection##regprot",nullptr,ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::Text("Region %s, %s, now %s",util::hex(t.act.base).c_str(),util::size(t.act.size).c_str(),t.act.prot.empty() ? "unset" : t.act.prot.c_str());
			ImGui::PushTextWrapPos(ui::px(420.0f));
			ImGui::TextDisabled("Applies to the whole region. Changing it can crash the process.");
			ImGui::PopTextWrapPos();
			ImGui::Spacing();
			ImGui::SetNextItemWidth(ui::px(120.0f));
			ImGui::Combo("Protection",&t.protIdx,baseProts,8);
			ui::tip("NA no access, R read, RW read write, WC copy on write, X execute, RX, RWX, WCX.");
			ImGui::Checkbox("Guard",&t.protG);
			ImGui::SameLine();
			ImGui::Checkbox("No cache",&t.protNc);
			ImGui::SameLine();
			ImGui::Checkbox("Write combine",&t.protWc);
			ImGui::SetNextItemWidth(ui::px(120.0f));
			ImGui::InputTextWithHint("New value","0x04",t.protTxt,sizeof(t.protTxt));
			ui::tip("Takes over from the choices above when filled. 0x01 NOACCESS, 0x02 READONLY, 0x04 READWRITE, 0x08 WRITECOPY, 0x10 EXECUTE, 0x20 EXECUTE_READ, 0x40 EXECUTE_READWRITE, 0x80 EXECUTE_WRITECOPY. Modifiers 0x100 GUARD, 0x200 NOCACHE, 0x400 WRITECOMBINE.");
			if (!t.protErr.empty()) {
				ImGui::PushTextWrapPos(ui::px(420.0f));
				ImGui::TextColored(ui::c.bad,"%s",t.protErr.c_str());
				ImGui::PopTextWrapPos();
			}
			if (t.protTask && t.protTask->done) {
				std::string s = *t.protRes;
				t.protTask.reset();
				if (s.find(" : OK") != std::string::npos) ImGui::CloseCurrentPopup();
				else t.protErr = s;
			}
			ImGui::Spacing();
			ImGui::BeginDisabled(t.protTask != nullptr);
			if (ImGui::Button("Change",ImVec2(ui::px(110.0f),0))) {
				std::string v = t.protTxt;
				if (v.empty()) {
					v = baseProts[t.protIdx];
					if (t.protG) v += "+G";
					if (t.protNc) v += "+NC";
					if (t.protWc) v += "+WCM";
				}
				uint32_t pv = 0;
				if (r.pid == procs::be::selfPid()) t.protErr = "Refused, that is FRS itself.";
				else if (!be::parseProt(v,pv)) t.protErr = std::string("Not a valid protection. It takes ") + be::protHelp();
				else {
					t.protErr.clear();
					auto res = std::make_shared<std::string>();
					t.protRes = res;
					Region x = t.act;
					t.protTask = task::run([r,x,pv,res](task::T&) -> std::string {
						uint64_t sz = x.size;
						std::string old,err;
						bool ok = be::protect(r,x.base,sz,pv,old,err);
						std::string s = "Change protection (pid " + std::to_string(r.pid) + ", " + util::hex(x.base) + ")" + (ok ? " : OK (was " + old + ")" : " : failed (" + util::safe(err) + ")");
						ui::toast(s);
						if (ok) changed(r);
						*res = s;
						return s;
					});
				}
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel",ImVec2(ui::px(110.0f),0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (t.openAddr) {
			t.openAddr = false;
			t.addrErr.clear();
			ImGui::OpenPopup("Read/Write address##regaddr");
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,ImVec2(0.5f,0.5f));
		if (ImGui::BeginPopupModal("Read/Write address##regaddr",nullptr,ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			if (ImGui::IsWindowAppearing() || t.addrFocus) ImGui::SetKeyboardFocusHere();
			t.addrFocus = false;
			ImGui::SetNextItemWidth(ui::px(260.0f));
			bool go = ImGui::InputTextWithHint("##addr","0x7ff6a0000000",t.addrTxt,sizeof(t.addrTxt),ImGuiInputTextFlags_EnterReturnsTrue);
			if (!t.addrErr.empty()) ImGui::TextColored(ui::c.bad,"%s",t.addrErr.c_str());
			if (ImGui::Button("Open",ImVec2(ui::px(110.0f),0))) go = true;
			if (go) {
				uint64_t v = 0;
				const Region* x = parseNum(t.addrTxt,v) && t.list ? t.list->at(v) : nullptr;
				if (!x || x->state == StFree) {
					t.addrErr = "Unable to find the memory region for that address.";
					t.addrFocus = true;
				}
				else {
					hexOpen(r,x->base,x->size,v - x->base,1);
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel",ImVec2(ui::px(110.0f),0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (t.openSave) {
			t.openSave = false;
			ImGui::OpenPopup("Save##regsave");
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,ImVec2(0.5f,0.5f));
		if (ImGui::BeginPopupModal("Save##regsave",nullptr,ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::TextDisabled(t.saveList ? "Save the region list to" : "Save the bytes to");
			ImGui::SetNextItemWidth(ui::px(460.0f));
			ImGui::InputText("##rpath",t.path,sizeof(t.path));
			ImGui::BeginDisabled(t.save != nullptr || !t.path[0]);
			if (ImGui::Button("Save",ImVec2(ui::px(110.0f),0))) {
				startSave(t,r,false);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel",ImVec2(ui::px(110.0f),0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		pollSave(t,r);
	}

	void startSections(Tab& t,const ProcRef& r) {
		auto rows = std::make_shared<std::vector<Section>>();
		auto err = std::make_shared<std::string>();
		t.sec.rows = rows;
		t.sec.err = err;
		t.sec.sel = -1;
		t.sec.task = task::run([r,rows,err](task::T& k) -> std::string {
			std::string e;
			if (!be::sections(r,*rows,e,&k.cancel)) *err = e.empty() ? "stopped" : e;
			return "";
		},false);
	}

	void startFind(Tab& t,const std::string& needle) {
		auto users = std::make_shared<std::vector<MapUser>>();
		auto bad = std::make_shared<int>(0);
		t.sec.users = users;
		t.sec.unread = bad;
		t.sec.userSel = -1;
		t.sec.searched = needle;
		t.sec.findTask = task::run([needle,users,bad](task::T& k) -> std::string {
			be::findMapped(needle,*users,*bad,&k.cancel);
			return "";
		},false);
	}

	void drawUsers(Tab& t) {
		SecState& s = t.sec;
		ImGui::Spacing();
		bool run = s.findTask && !s.findTask->done;
		if (run) {
			ImGui::TextDisabled("Walking every process for \"%s\"...",s.searched.c_str());
			return;
		}
		// the counts go first, a full path would push them out
		std::string head = "Mapped by : " + std::to_string(s.users->size()) + " views, " + std::to_string(*s.unread) + " processes could not be read, for \"" + s.searched + "\"";
		ImGui::PushStyleColor(ImGuiCol_Text,ui::c.dim);
		ui::textEllipsis(head.c_str());
		ImGui::PopStyleColor();
		ui::tip(head.c_str());
		if (!ui::beginTable("##regusers",7,ImVec2(0.0f,0.0f),ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit)) return;
		ImGui::TableSetupColumn("Process",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(170.0f));
		ImGui::TableSetupColumn("PID",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(54.0f));
		ImGui::TableSetupColumn("View",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(56.0f));
		ImGui::TableSetupColumn("Start",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(112.0f));
		ImGui::TableSetupColumn("End",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(112.0f));
		ImGui::TableSetupColumn("Size",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(70.0f));
		ImGui::TableSetupColumn("File",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(420.0f));
		ImGui::TableSetupScrollFreeze(0,1);
		ImGui::TableHeadersRow();
		ImGuiListClipper clip;
		clip.Begin((int)s.users->size());
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
				const MapUser& u = (*s.users)[i];
				ImGui::TableNextRow();
				ImGui::PushID(i);
				ImGui::TableSetColumnIndex(0);
				if (ImGui::Selectable(u.name.c_str(),s.userSel == i,ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
					s.userSel = i;
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) procwin::openPid(u.pid);
				}
				if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
					s.userSel = i;
					s.userMenu = true;
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%u",u.pid);
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(u.view.c_str());
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(util::hex(u.lo).c_str());
				ImGui::TableSetColumnIndex(4);
				ImGui::TextUnformatted(util::hex(u.hi).c_str());
				ImGui::TableSetColumnIndex(5);
				ui::cellRight(util::size(u.hi - u.lo).c_str());
				ImGui::TableSetColumnIndex(6);
				ui::textEllipsis(u.path.c_str());
				ui::tip(u.path.c_str());
				ImGui::PopID();
			}
		}
		ui::endTable();
		if (s.userMenu) {
			ImGui::OpenPopup("##regusermenu");
			s.userMenu = false;
		}
		if (ImGui::BeginPopup("##regusermenu")) {
			if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
			bool ok = s.userSel >= 0 && s.userSel < (int)s.users->size();
			if (ImGui::MenuItem("Go to process",nullptr,false,ok)) procwin::openPid((*s.users)[s.userSel].pid);
			if (ImGui::MenuItem("Copy",nullptr,false,ok)) {
				const MapUser& u = (*s.users)[s.userSel];
				std::string o = u.name + "\t" + std::to_string(u.pid) + "\t" + u.view + "\t" + util::hex(u.lo) + "\t" + util::hex(u.hi) + "\t" + util::size(u.hi - u.lo) + "\t" + u.path + "\n";
				ImGui::SetClipboardText(o.c_str());
			}
			ImGui::EndPopup();
		}
	}

	void drawSections(Tab& t,const ProcRef& r) {
		SecState& s = t.sec;
		if (!s.asked) {
			s.asked = true;
			startSections(t,r);
		}
		bool busyNow = s.task && !s.task->done;
		bool f5 = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId) && ImGui::IsKeyPressed(ImGuiKey_F5,false);
		if (ui::iconBtn("regsecref",ui::IcoRefresh,"Read the section handles again (F5)") || f5) {
			if (!busyNow) startSections(t,r);
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ui::px(300.0f));
		bool go = ImGui::InputTextWithHint("##regfind","Who maps a file? Part of its path",s.q,sizeof(s.q),ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		bool finding = s.findTask && !s.findTask->done;
		ImGui::BeginDisabled(finding || !s.q[0]);
		if (ImGui::Button("Find")) go = true;
		ImGui::EndDisabled();
		ImGui::SameLine();
		ui::help("Which processes have a file mapped, from a walk over every process. The list of all views of one section needs the kernel driver, so pagefile backed sections can't be followed.");
		if (go && !finding && s.q[0]) startFind(t,s.q);

		float usersH = s.users ? ImGui::GetContentRegionAvail().y * 0.45f : 0.0f;
		ImGui::BeginChild("##regsecs",ImVec2(0.0f,s.users ? -usersH : 0.0f));
		if (busyNow) ui::empty("Reading the handle table...");
		else if (s.err && !s.err->empty()) ui::empty(s.err->c_str());
		else if (s.rows && s.rows->empty()) ui::empty("This process holds no section handles.");
		else if (s.rows && ui::beginTable("##regsec",4,ImVec2(0.0f,0.0f),ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn(
#ifdef _WIN32
				"Handle",
#else
				"Inode",
#endif
				ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(80.0f));
			ImGui::TableSetupColumn("Type",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(90.0f));
			ImGui::TableSetupColumn("Size",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(72.0f));
			ImGui::TableSetupColumn("Name",ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,ui::px(560.0f));
			ImGui::TableSetupScrollFreeze(0,1);
			ImGui::TableHeadersRow();
			ImGuiListClipper clip;
			clip.Begin((int)s.rows->size());
			while (clip.Step()) {
				for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
					const Section& x = (*s.rows)[i];
					ImGui::TableNextRow();
					ImGui::PushID(i);
					ImGui::TableSetColumnIndex(0);
					ui::pushMonoCell();
					std::string hx = util::hex(x.handle);
					if (ImGui::Selectable(hx.c_str(),s.sel == i,ImGuiSelectableFlags_SpanAllColumns)) s.sel = i;
					ui::popFont();
					if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
						s.sel = i;
						s.menu = true;
					}
					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(x.type.c_str());
					ImGui::TableSetColumnIndex(2);
					ui::cellSize(x.size);
					ImGui::TableSetColumnIndex(3);
					ui::textEllipsis(x.best.c_str());
					ui::tip(x.file.empty() ? x.best.c_str() : x.file.c_str());
					ImGui::PopID();
				}
			}
			ui::endTable();
		}
		if (s.menu) {
			ImGui::OpenPopup("##regsecmenu");
			s.menu = false;
		}
		if (ImGui::BeginPopup("##regsecmenu")) {
			if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
			bool ok = s.rows && s.sel >= 0 && s.sel < (int)s.rows->size();
			const Section* x = ok ? &(*s.rows)[s.sel] : nullptr;
			if (ImGui::MenuItem("Read/Write memory...",nullptr,false,ok)) {
				if (x->addr) hexOpen(r,x->addr,x->view);
				else hexSection(r,x->handle,x->best);
			}
			if (ImGui::MenuItem("Find who maps this file",nullptr,false,ok && !x->file.empty() && !finding)) startFind(t,x->file);
			if (ImGui::MenuItem("Copy",nullptr,false,ok)) {
				std::string o = util::hex(x->handle) + "\t" + x->type + "\t" + util::size(x->size) + "\t" + x->best + "\n";
				ImGui::SetClipboardText(o.c_str());
			}
			ImGui::EndPopup();
		}
		ImGui::EndChild();
		if (s.users) drawUsers(t);
	}

}

namespace vmem {

	void pruneTabs() {
		uint64_t now = util::nowMs();
		for (auto it = g_tabs.begin(); it != g_tabs.end();) {
			Tab& t = it->second;
			bool shut = !procwin::isOpen(t.ref);
			// a closed window's save is polled from here, nobody else would report it
			if (shut && t.save && t.save->done) {
				std::string s = t.save->res == "EXISTS" ? "Save (pid " + std::to_string(t.ref.pid) + ") : not saved, the file exists" : t.save->res;
				t.save.reset();
				appLog(s);
				ui::toast(s);
			}
			if (!t.save && (shut || now - t.used > 120000)) {
				if (t.sec.task) t.sec.task->cancel = true;
				if (t.sec.findTask) t.sec.findTask->cancel = true;
				it = g_tabs.erase(it);
			} else ++it;
		}
	}

	void drawTab(const ProcRef& r) {
		uint64_t now = util::nowMs();
		Tab& t = g_tabs[{r.pid,r.start}];
		t.ref = r;
		t.used = now;
		auto l = latest(r);
		bool run = busy(r);
		if (!l && !run) {
			refresh(r);
			run = true;
		}
		if (l != t.list) {
			t.list = l;
			rebuild(t);
		}
		if (l && !run && cfg::getb("regAutoRefresh",false) && now - l->t >= (uint64_t)cfg::geti("regAutoSec",5) * 1000) {
			refresh(r);
			run = true;
		}
		bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId);
		ImGuiIO& io = ImGui::GetIO();

		if (ImGui::BeginTabBar("##regview")) {
			const char* views[] = {"Regions","Sections"};
			for (int i = 0; i < 2; i++) if (ImGui::BeginTabItem(views[i])) {
				t.view = i;
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		if (t.view == 1) {
			drawSections(t,r);
			drawDialogs(t,r);
			return;
		}

		if (ui::iconBtn("regref",ui::IcoRefresh,"Read the regions again (F5)") && !run) {
			refresh(r);
			run = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Options")) ImGui::OpenPopup("##regopt");
		drawOptions(t,r);
		ImGui::SameLine();
		bool au = cfg::getb("regAutoRefresh",false);
		if (ImGui::Checkbox("Auto update",&au)) cfg::setb("regAutoRefresh",au);
		ui::tip("Read the list again every few seconds, the interval is in Settings. Each pass walks the whole address space of the process.");
		ImGui::SameLine();
		if (t.focusQ) {
			ImGui::SetKeyboardFocusHere();
			t.focusQ = false;
		}
		if (ui::search("##regq",t.q,sizeof(t.q),"Search memory (Ctrl+F)")) t.dirty = true;

		if (focused) {
			if (ImGui::IsKeyPressed(ImGuiKey_F5,false) && !run) {
				refresh(r);
				run = true;
			}
			if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) t.focusQ = true;
			if (!io.WantTextInput && t.list) {
				auto sel = selected(t);
				if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) copyRows(t,-1);
				if (ImGui::IsKeyPressed(ImGuiKey_Enter,false) && sel.size() == 1 && !sel[0]->parent && sel[0]->d->state != StFree && t.list->prot.empty()) openEditor(r,*sel[0]->d);
			}
		}

		if (l && !l->rows.empty()) {
			if (!l->prot.empty()) {
				ui::icon(ui::IcoWarn,ImGui::GetCursorScreenPos(),ImGui::GetFontSize(),ImGui::GetColorU32(ui::c.warn));
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetFontSize() + ui::px(6.0f));
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextColored(ui::c.warn,"%s",l->note.c_str());
				ImGui::PopTextWrapPos();
			} else if (!l->note.empty()) ui::hint(l->note.c_str());
			if (t.dirty) rebuild(t);
			statPair("Committed",util::size(t.cm));
			statPair("Private",util::size(t.pv));
			statPair("Working set",l->wsOk ? util::size(t.ws) : "N/A");
			char cnt[64];
			snprintf(cnt,sizeof(cnt),t.shown != (int)l->rows.size() ? "%d of %d" : "%d",t.shown,(int)l->rows.size());
			statPair("Regions",cnt);
			if (!t.rxErr.empty()) {
				ImGui::TextColored(ui::c.bad,"Bad regular expression : %s",t.rxErr.c_str());
				ImGui::SameLine(0.0f,ui::px(14.0f));
			}
			char upd[64];
			if (run) snprintf(upd,sizeof(upd),"reading...");
			else snprintf(upd,sizeof(upd),"read %llu s ago in %llu ms",(unsigned long long)((now - l->t) / 1000),(unsigned long long)l->ms);
			float w = ImGui::CalcTextSize(upd).x;
			float av = ImGui::GetContentRegionAvail().x;
			if (av > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + av - w);
			ImGui::TextDisabled("%s",upd);
			ui::Seg sg[3] = {
				{(float)t.img,ui::c.purple,"Image"},
				{(float)t.map,ui::c.cache,"Mapped"},
				{(float)t.prv,ui::c.priv,"Private"},
			};
			if (t.img + t.map + t.prv) {
				ui::stackBar("##regbar",sg,3,ImVec2(0.0f,6.0f));
				ui::legend(sg,3);
			}
			ImGui::Spacing();
		}

		if (t.save && !t.save->done) {
			char ov[64];
			snprintf(ov,sizeof(ov),"Saving %d%%",(int)(t.save->prog * 100.0f));
			ui::meter(t.save->prog,ov,ImVec2(240.0f,18.0f),ui::c.accent);
			ImGui::SameLine();
			if (ImGui::SmallButton("Stop##regsv")) t.save->cancel = true;
		}

		if (!l) {
			ui::empty("Loading...");
		} else if (!l->err.empty()) {
			std::string m = "Unable to query memory information : " + l->err;
			ui::empty(m.c_str());
		} else if (l->rows.empty()) {
			ui::empty(l->note.empty() ? "There are no memory regions to display." : l->note.c_str());
		} else {
			drawTable(t,r);
			if (t.nodes.empty()) {
				// the table keeps its header, say why it has no rows
				ImVec2 p = ImGui::GetItemRectMin();
				ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + ui::px(12.0f),p.y + ImGui::GetFrameHeight() + ui::px(8.0f)),ImGui::GetColorU32(ui::c.dim),"There are no memory regions to display. The search or the Options filters hide them all.");
			}
		}
		drawMenu(t,r);
		drawDialogs(t,r);
	}

}
