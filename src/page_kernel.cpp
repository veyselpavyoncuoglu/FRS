#include "domains.h"
#include "kmem.h"
#include "cfg.h"
#include "ram.h"
#include "ui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

	using namespace kmem;

	enum { TabPool,TabBig,TabPages,TabComp };
	const char* const kTabs[] = {"Pool tags","Big pools","Page files","Compression"};

	int g_pick = -1; // tab to select once
	uint64_t g_lastDraw = 0;

	float btnW(const char* t) {
		return ImGui::CalcTextSize(t,nullptr,true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	}

	void flow(float w) {
		ImGui::SameLine();
		if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
	}

	float arrowW() {
		return std::floor(ImGui::GetFontSize() * 0.65f + ImGui::GetStyle().FramePadding.x);
	}

	void dimRight(const char* t) {
		ImGui::PushStyleColor(ImGuiCol_Text,ui::c.dim);
		ui::cellRight(t);
		ImGui::PopStyleColor();
	}

	struct Card {
		const char* id;
		const char* label;
		std::string val,sub;
		ImVec4 col;
		const char* tip = nullptr;
	};

	void cards(const Card* cs,int n) {
		float sp = ImGui::GetStyle().ItemSpacing.x;
		float av = ImGui::GetContentRegionAvail().x;
		int cols = std::max(2,std::min(n,(int)(av / ui::px(120.0f))));
		float w = (av - sp * (float)(cols - 1)) / (float)cols;
		for (int i = 0; i < n; i++) {
			if (i % cols) ImGui::SameLine();
			ui::beginCard(cs[i].id,nullptr,ImVec2(w / ui::px(1.0f),0.0f));
			ImGui::TextDisabled("%s",cs[i].label);
			ui::pushBig();
			if (cs[i].val == "N/A") ImGui::TextDisabled("N/A");
			else ImGui::TextColored(cs[i].col,"%s",cs[i].val.c_str());
			ui::popFont();
			ImGui::TextDisabled("%s",cs[i].sub.c_str());
			ui::endCard();
			if (cs[i].tip) ui::tip(cs[i].tip);
		}
	}

	void warnLine(const char* txt) {
		float sz = ImGui::GetFontSize();
		ui::icon(ui::IcoWarn,ImGui::GetCursorScreenPos(),sz,ImGui::GetColorU32(ui::c.warn));
		ImGui::Dummy(ImVec2(sz,sz));
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text,ui::c.warn);
		ImGui::TextWrapped("%s",txt);
		ImGui::PopStyleColor();
	}

	float dotW(const char* t) {
		return std::floor(ImGui::GetFontSize() * 0.6f) + ui::px(5.0f) + ImGui::CalcTextSize(t).x;
	}

	void dot(ImVec4 col,const char* t) {
		float d = std::floor(ImGui::GetFontSize() * 0.6f);
		ImVec2 q = ImGui::GetCursorScreenPos();
		q.y += std::floor((ImGui::GetTextLineHeight() - d) * 0.5f);
		ImGui::GetWindowDrawList()->AddRectFilled(q,ImVec2(q.x + d,q.y + d),ImGui::GetColorU32(col),ui::px(2.0f));
		ImGui::Dummy(ImVec2(d,ImGui::GetTextLineHeight()));
		ImGui::SameLine(0.0f,ui::px(5.0f));
		ImGui::TextDisabled("%s",t);
	}

	// the page still draws behind a process window that has the focus
	bool pageKeys() {
		return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId);
	}

	std::string ago(uint64_t t) {
		uint64_t now = util::nowMs();
		uint64_t s = now > t ? (now - t) / 1000 : 0;
		if (s < 2) return "just now";
		if (s < 120) return std::to_string(s) + " s ago";
		return std::to_string(s / 60) + " min ago";
	}

	uint64_t extraOf(const SysMem& m,const char* k) {
		for (auto& e : m.extra) if (e.first == k) return e.second;
		return NA;
	}

	// ---------- Pool tags ----------

	struct PoolPage {
		std::shared_ptr<const PoolSnap> snap;
		std::vector<int> idx;
		char filter[128] = {};
		std::string low;
		int sortCol = PCVal + VNpBytes;
		bool desc = true,dirty = true,focus = false,menu = false;
		std::set<uint64_t> sel;
		uint64_t menuId = 0;
		int menuCol = 0,preset = 0;
		bool colOn[PCN] = {};
		std::string tot[PCN];
	};
	PoolPage p;

	enum { BLAddr,BLSize,BLPool,BLTag,BLDriver,BLDesc,BLN };
	enum { BGTag,BGDriver,BGDesc,BGBlocks,BGBytes,BGNpN,BGNpBytes,BGPN,BGPBytes,BGMax,BGPool,BGN };

	struct BigPage {
		std::shared_ptr<const BigSnap> snap;
		std::shared_ptr<task::T> job;
		std::vector<int> idx;
		std::vector<int64_t> poolBytes; // per tag, what the pool table says
		char filter[128] = {};
		std::string low,tag;
		uint64_t tagId = 0;
		bool hasTag = false,asked = false,dirty = true,focus = false,menu = false;
		int mode = 0; // 0 list, 1 by tag
		int sortL = 1,sortG = 4;
		bool descL = true,descG = true;
		int sel = -1,menuCol = 0;
		bool onL[BLN] = {},onG[BGN] = {}; // enabled columns, Copy row skips the rest
	};
	BigPage b;

	void bigStart() {
		if (b.job && !b.job->done) return;
		b.job = task::run([](task::T&) {
			bigRead();
			return std::string();
		},false);
	}

	void poolRebuild() {
		p.dirty = false;
		p.idx.clear();
		if (!p.snap) return;
		const auto& rs = p.snap->rows;
		for (int i = 0; i < (int)rs.size(); i++) if (poolMatch(rs[i],p.low)) p.idx.push_back(i);
		int col = p.sortCol < 0 ? PCVal + VNpBytes : p.sortCol;
		bool desc = p.sortCol < 0 ? true : p.desc;
		std::sort(p.idx.begin(),p.idx.end(),[&](int x,int y) { return poolLess(rs[x],rs[y],col,desc); });
		PoolRow sum;
		for (int i = 0; i < VN; i++) sum.v[i] = sum.d[i] = UNK;
		for (int i : p.idx) for (int k = 0; k < VN; k++) {
			if (rs[i].v[k] != UNK) sum.v[k] = (sum.v[k] == UNK ? 0 : sum.v[k]) + rs[i].v[k];
			if (rs[i].d[k] != UNK) sum.d[k] = (sum.d[k] == UNK ? 0 : sum.d[k]) + rs[i].d[k];
		}
		for (int c = PCVal; c < PCN; c++) p.tot[c] = poolVal(sum,c) == UNK ? "" : poolCell(sum,c);
	}

	std::string poolLine(const PoolRow& r) {
		std::string s;
		for (int c = 0; c < PCN; c++) {
			if (!p.colOn[c]) continue;
			if (!s.empty()) s += '\t';
			s += poolCell(r,c);
		}
		return s;
	}

	std::string poolTip(int c,int bits) {
		const PoolCol& d = kPoolCols[c];
		if (!kWin) return d.lxTip ? d.lxTip : d.tip;
		std::string t = d.tip;
		if (c == PCVal + VPagedAlloc || c == PCVal + VPagedFree || c == PCVal + VNpAlloc || c == PCVal + VNpFree) t += bits == 32 ? " 32 bit on this Windows, it wraps on long uptimes." : " 64 bit on this Windows, no wrap.";
		return t;
	}

	void poolCopy() {
		if (!p.snap) return;
		if (p.dirty) poolRebuild(); // a new sample came in this frame, the old index list does not fit its rows
		std::string s;
		for (int i : p.idx) if (p.sel.count(p.snap->rows[i].id)) s += poolLine(p.snap->rows[i]) + "\n";
		if (!s.empty()) ImGui::SetClipboardText(s.c_str());
	}

	void pickTag(const std::string& tag,uint64_t id) {
		b.hasTag = true;
		b.tag = tag;
		b.tagId = id;
		b.mode = 0;
		b.sel = -1;
		b.dirty = true;
		bigStart();
	}

	void showBlocks(const PoolRow& r) {
		pickTag(r.tag,r.id);
		b.filter[0] = 0;
		b.low.clear();
		g_pick = TabBig;
	}

	void poolCellUi(const PoolRow& r,int c) {
		std::string t = poolCell(r,c);
		if (c == PCDriver || c == PCDesc) {
			ui::textEllipsis(t.c_str());
			if (!t.empty()) ui::tip(t.c_str());
			return;
		}
		if (t.empty()) return;
		if (t == "N/A") {
			dimRight("N/A");
			return;
		}
		if (c >= PCDelta && kPoolCols[c].bytes) {
			ImGui::PushStyleColor(ImGuiCol_Text,poolVal(r,c) > 0 ? ui::c.warn : ui::c.good);
			ui::cellRight(t.c_str());
			ImGui::PopStyleColor();
			return;
		}
		ui::cellRight(t.c_str());
	}

	void poolTable() {
		ImGuiTableFlags fl = ImGuiTableFlags_ScrollX | ImGuiTableFlags_SortTristate | ImGuiTableFlags_SizingFixedFit;
		if (!ui::beginTable("##kpool",PCN,ImVec2(0,0),fl)) return;
		const char* titles[PCN];
		for (int c = 0; c < PCN; c++) {
			const PoolCol& d = kPoolCols[c];
			titles[c] = !kWin && d.lxTitle ? d.lxTitle : d.title;
			ImGuiTableColumnFlags f = ImGuiTableColumnFlags_WidthFixed;
			if ((!kWin && !d.lxTitle) || (kWin && !p.snap->db.loaded && (c == PCDriver || c == PCDesc))) f |= ImGuiTableColumnFlags_Disabled;
			if (!d.on) f |= ImGuiTableColumnFlags_DefaultHide;
			if (c == PCTag) f |= ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder;
			if (c == PCVal + VNpBytes) f |= ImGuiTableColumnFlags_DefaultSort;
			if (c >= PCVal) f |= ImGuiTableColumnFlags_PreferSortDescending;
			ImGui::TableSetupColumn(titles[c],f,std::max(ui::px(!kWin && c == PCTag ? 200.0f : d.w),ImGui::CalcTextSize(titles[c]).x + arrowW()));
		}
		ImGui::TableSetupScrollFreeze(1,2);
		if (p.preset) {
			for (int c = 0; c < PCN; c++) ImGui::TableSetColumnEnabled(c,p.preset == 2 || kPoolCols[c].on);
			p.preset = 0;
		}
		if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
			if (sp->SpecsDirty) {
				p.sortCol = sp->SpecsCount > 0 ? sp->Specs[0].ColumnIndex : -1;
				p.desc = sp->SpecsCount > 0 && sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
				sp->SpecsDirty = false;
				p.dirty = true;
			}
		}
		for (int c = 0; c < PCN; c++) p.colOn[c] = (ImGui::TableGetColumnFlags(c) & ImGuiTableColumnFlags_IsEnabled) != 0;
		if (p.dirty) poolRebuild();

		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int c = 0; c < PCN; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			ImGui::PushID(c);
			ui::headerCell(titles[c],poolTip(c,p.snap->bits).c_str(),c >= PCVal);
			ImGui::PopID();
		}
		ImGui::TableNextRow();
		ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,ImGui::GetColorU32(ui::c.panel2));
		ImGui::PushStyleColor(ImGuiCol_Text,ui::c.dim);
		for (int c = 0; c < PCN; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			if (c == PCTag) ImGui::Text("Total");
			else if (c >= PCVal && !p.tot[c].empty()) ui::cellRight(p.tot[c].c_str());
		}
		ImGui::PopStyleColor();

		const auto& rs = p.snap->rows;
		uint64_t now = util::nowMs();
		bool hl = cfg::getb("poolHighlight",true);
		ImVec4 cNew = ui::c.good;
		ImVec4 cHot = ui::c.bad;
		cNew.w = 0.16f;
		cHot.w = 0.20f;
		ImU32 uNew = ImGui::GetColorU32(cNew);
		ImU32 uHot = ImGui::GetColorU32(cHot);
		ImVec4 cSel = ui::c.accent;
		cSel.w = 0.22f;
		ImGui::PushStyleColor(ImGuiCol_Header,cSel);
		ImGuiListClipper clip;
		clip.Begin((int)p.idx.size());
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
				const PoolRow& r = rs[p.idx[i]];
				ImGui::TableNextRow();
				if (hl && r.hotMs && now - r.hotMs < 3000) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,uHot);
				else if (hl && r.seenMs && now - r.seenMs < 3000) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,uNew);
				ImGui::PushID((const void*)(uintptr_t)r.id);
				if (ImGui::TableSetColumnIndex(PCTag)) {
					float x0 = ImGui::GetCursorPosX();
					bool sel = p.sel.count(r.id) != 0;
					bool hit = ImGui::Selectable("##r",sel,ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
					bool hov = ImGui::IsItemHovered();
					if (hit) {
						if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
							if (kWin) showBlocks(r);
						} else if (ImGui::GetIO().KeyCtrl) {
							if (sel) p.sel.erase(r.id);
							else p.sel.insert(r.id);
						} else {
							p.sel.clear();
							p.sel.insert(r.id);
						}
					}
					if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
						if (!sel) {
							p.sel.clear();
							p.sel.insert(r.id);
						}
						p.menu = true;
						p.menuId = r.id;
						p.menuCol = ImGui::TableGetHoveredColumn();
					}
					ImGui::SameLine(0.0f,0.0f);
					ImGui::SetCursorPosX(x0);
					if (kWin) ImGui::TextUnformatted(r.tag.c_str());
					else {
						ui::textEllipsis(r.tag.c_str());
						ui::tip(r.tag.c_str());
					}
					if (r.prot) ui::tip("Protected tag, its high bit is set");
				}
				for (int c = 1; c < PCN; c++) if (ImGui::TableSetColumnIndex(c)) poolCellUi(r,c);
				ImGui::PopID();
			}
		}
		ImGui::PopStyleColor();
		if (p.idx.empty()) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(PCTag);
			ImGui::TextDisabled("No match");
		}
		ui::endTable();
	}

	void poolMenu() {
		if (p.menu) {
			ImGui::OpenPopup("##pmenu");
			p.menu = false;
		}
		if (!ImGui::BeginPopup("##pmenu")) return;
		const PoolRow* r = nullptr;
		if (p.snap) for (auto& x : p.snap->rows) if (x.id == p.menuId) r = &x;
		if (kWin && ImGui::MenuItem("Show allocations",nullptr,false,r != nullptr) && r) showBlocks(*r);
		if (ImGui::MenuItem("Copy rows","Ctrl+C")) poolCopy();
		std::string cell = r && p.menuCol >= 0 && p.menuCol < PCN ? poolCell(*r,p.menuCol) : "";
		if (ImGui::MenuItem("Copy cell",nullptr,false,!cell.empty())) ImGui::SetClipboardText(cell.c_str());
		ImGui::EndPopup();
	}

	void drawPool() {
		auto s = poolLatest();
		if (s != p.snap) {
			p.snap = s;
			p.dirty = true;
		}
		auto m = sysmem::latest();
		int64_t pb = 0,nb = 0;
		if (s) for (auto& r : s->rows) {
			if (r.v[VPagedBytes] != UNK) pb += r.v[VPagedBytes];
			if (r.v[VNpBytes] != UNK) nb += r.v[VNpBytes];
		}
		bool have = s && s->err.empty();

		if (kWin) {
			if (have) {
				Card cs[] = {
					{"##kc1","Non-paged in tags",util::size((uint64_t)nb),"Windows reports " + util::size(m->nonPaged),ui::c.kernel,"Sum of the tags against the system counter. The counter is higher, it also holds pool the allocator keeps but no tag uses right now."},
					{"##kc2","Paged in tags",util::size((uint64_t)pb),"Windows reports " + util::size(m->pagedVs),ui::c.kernel,"Sum of the tags against the system counter, the paged pool virtual size. That also holds address space the allocator keeps but no tag uses right now, so it can be far higher."},
					{"##kc3","Tags",util::count(s->rows.size()),s->bits == 32 ? "Alloc counts can wrap" : "Alloc counts never wrap",ui::c.text,s->bits == 32 ? "The allocation and free counters are 32 bit on this Windows, they wrap on long uptimes. Deltas still come out right." : "The allocation and free counters are 64 bit on this Windows, they never wrap."}
				};
				cards(cs,3);
			}
		} else {
			Card cs[] = {
				{"##kc1","Slab active",have ? util::size((uint64_t)nb) : "N/A",have ? "From /proc/slabinfo" : "Not readable",ui::c.kernel},
				{"##kc2","Reclaimable",util::size(extraOf(*m,"SReclaimable")),"SReclaimable",ui::c.cache},
				{"##kc3","Unreclaimable",util::size(m->nonPaged),"SUnreclaim",ui::c.kernel},
				{"##kc4","Kernel stacks",util::size(m->kernelWs),"KernelStack",ui::c.kernel},
				{"##kc5","Page tables",util::size(extraOf(*m,"PageTables")),"PageTables",ui::c.kernel},
				{"##kc6","Vmalloc",util::size(extraOf(*m,"VmallocUsed")),"VmallocUsed",ui::c.kernel}
			};
			cards(cs,6);
		}

		if (!s) {
			ui::empty("Reading the pool tag table");
			return;
		}
		if (!have) {
			ui::empty(s->err.c_str());
			return;
		}

		ui::beginCard("##kpbar",nullptr);
		if (p.focus) {
			ImGui::SetKeyboardFocusHere();
			p.focus = false;
		}
		if (ui::search("##kpf",p.filter,sizeof(p.filter),kWin ? "Filter : tag, driver, description" : "Filter : cache name",240.0f)) {
			p.low = util::lower(p.filter);
			p.dirty = true;
		}
		ui::tip("Ctrl+F. Ctrl+K stays the command palette.");
		if (*p.filter) {
			ImGui::SameLine();
			if (ImGui::Button("Clear##kp")) {
				p.filter[0] = 0;
				p.low.clear();
				p.dirty = true;
			}
		}
		flow(btnW("Bytes columns"));
		if (ImGui::Button("Bytes columns")) p.preset = 1;
		ui::tip("Bytes and how they change. Right click a header to pick single columns.");
		flow(btnW("All columns"));
		if (ImGui::Button("All columns")) p.preset = 2;
		ui::tip("All counters and all deltas.");

		char line[160];
		if (s->dtMs) snprintf(line,sizeof(line),"%s tags shown. Deltas over %.1f s.",util::count(p.idx.size()).c_str(),(double)s->dtMs / 1000.0);
		else snprintf(line,sizeof(line),"%s tags shown. Deltas start with the next sample.",util::count(p.idx.size()).c_str());
		ImGui::TextDisabled("%s",line);
		if (cfg::getb("memPaused",false)) {
			flow(ImGui::CalcTextSize("Sampling is paused.").x);
			ImGui::TextColored(ui::c.warn,"Sampling is paused.");
		}
		if (cfg::getb("poolHighlight",true)) {
			const char* nw = kWin ? "New tag" : "New cache";
			std::string hot = "Growing past " + util::count((uint64_t)cfg::geti("poolHlKb",256)) + " KB/s";
			flow(dotW(nw));
			dot(ui::c.good,nw);
			flow(dotW(hot.c_str()));
			dot(ui::c.bad,hot.c_str());
			ui::tip("Row tints, they last 3 s. Settings, Kernel memory turns them off and sets the threshold.");
		}
		if (kWin) {
			bool bad = !s->db.loaded && !s->db.why.empty();
			std::string db = s->db.loaded ? "pooltag.txt : " + util::count(s->db.n) + " tags." : bad ? "pooltag.txt not used : " + s->db.why + "." : "Tag names : add pooltag.txt next to FRS.exe";
			flow(ImGui::CalcTextSize(db.c_str()).x);
			if (bad) ImGui::TextColored(ui::c.warn,"%s",db.c_str());
			else ImGui::TextDisabled("%s",db.c_str());
			flow(ImGui::CalcTextSize("(?)").x);
			ui::help("Driver and Description come from a file named pooltag.txt next to the FRS exe, read once at start. It is Microsoft's and FRS does not ship it. Debugging Tools for Windows installs one under Debuggers\\x64\\triage.");
		} else {
			const char* t = "Slab caches from /proc/slabinfo, nearest thing Linux has to pool tags.";
			flow(ImGui::CalcTextSize(t).x);
			ImGui::TextDisabled("%s",t);
		}
		ui::endCard();

		if (!ImGui::GetIO().WantTextInput && !p.sel.empty() && pageKeys() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) poolCopy();
		poolTable();
		poolMenu();
	}

	// ---------- Big pools ----------

	bool bigPass(const BigTag& t) {
		if (b.hasTag && t.id != b.tagId) return false;
		if (b.low.empty()) return true;
		return util::lower(t.tag).find(b.low) != std::string::npos || util::lower(t.driver).find(b.low) != std::string::npos || util::lower(t.desc).find(b.low) != std::string::npos;
	}

	std::string bigCell(const BigSnap& s,const BigRow& r,int c) {
		const BigTag& t = s.tags[r.tag];
		switch (c) {
			case BLAddr: return r.addr ? util::hex(r.addr) : "N/A";
			case BLSize: return util::size(r.size);
			case BLPool: return r.np ? "Non-paged" : "Paged";
			case BLTag: return t.tag;
			case BLDriver: return t.driver;
			case BLDesc: return t.desc;
		}
		return "";
	}

	std::string groupCell(const BigTag& t,int c,size_t i) {
		switch (c) {
			case BGTag: return t.tag;
			case BGDriver: return t.driver;
			case BGDesc: return t.desc;
			case BGBlocks: return util::count(t.n);
			case BGBytes: return util::size(t.bytes);
			case BGNpN: return t.npN ? util::count(t.npN) : "";
			case BGNpBytes: return t.npN ? util::size(t.npBytes) : "";
			case BGPN: return t.n - t.npN ? util::count(t.n - t.npN) : "";
			case BGPBytes: return t.n - t.npN ? util::size(t.bytes - t.npBytes) : "";
			case BGMax: return util::size(t.largest);
			case BGPool: return i < b.poolBytes.size() && b.poolBytes[i] != UNK ? util::size((uint64_t)b.poolBytes[i]) : "N/A";
		}
		return "";
	}

	void bigRebuild() {
		b.dirty = false;
		b.idx.clear();
		if (!b.snap || !b.snap->err.empty()) return;
		const BigSnap& s = *b.snap;
		std::vector<char> pass(s.tags.size());
		for (size_t i = 0; i < s.tags.size(); i++) pass[i] = bigPass(s.tags[i]);

		if (b.mode == 1) {
			b.poolBytes.assign(s.tags.size(),UNK);
			if (kWin) {
				if (auto ps = poolLatest()) {
					std::unordered_map<uint64_t,int64_t> by;
					for (auto& r : ps->rows) by[r.id] = r.v[VBytes];
					for (size_t i = 0; i < s.tags.size(); i++) {
						auto it = by.find(s.tags[i].id);
						if (it != by.end()) b.poolBytes[i] = it->second;
					}
				}
			}
			for (int i = 0; i < (int)s.tags.size(); i++) if (pass[i]) b.idx.push_back(i);
			int c = b.sortG;
			bool d = b.descG;
			auto val = [&](int i) -> int64_t {
				const BigTag& t = s.tags[i];
				switch (c) {
					case BGBlocks: return (int64_t)t.n;
					case BGBytes: return (int64_t)t.bytes;
					case BGNpN: return (int64_t)t.npN;
					case BGNpBytes: return (int64_t)t.npBytes;
					case BGPN: return (int64_t)(t.n - t.npN);
					case BGPBytes: return (int64_t)(t.bytes - t.npBytes);
					case BGMax: return (int64_t)t.largest;
					case BGPool: return b.poolBytes[i];
				}
				return 0;
			};
			std::sort(b.idx.begin(),b.idx.end(),[&](int x,int y) {
				if (c == BGDriver || c == BGDesc) {
					const std::string& p1 = c == BGDriver ? s.tags[x].driver : s.tags[x].desc;
					const std::string& p2 = c == BGDriver ? s.tags[y].driver : s.tags[y].desc;
					if (p1.empty() != p2.empty()) return p2.empty();
					int k = p1.compare(p2);
					if (k) return d ? k > 0 : k < 0;
				} else if (c != BGTag) {
					int64_t v1 = val(x);
					int64_t v2 = val(y);
					if ((v1 == UNK) != (v2 == UNK)) return v2 == UNK;
					if (v1 != v2) return d ? v1 > v2 : v1 < v2;
				} else return d ? x > y : x < y;
				return x < y;
			});
			return;
		}

		const auto& rs = s.rows;
		b.idx.reserve(rs.size());
		for (int i = 0; i < (int)rs.size(); i++) if (pass[rs[i].tag]) b.idx.push_back(i);
		// rank per tag, so driver and description sort without string compares per row
		std::vector<int> rank(s.tags.size());
		int c = b.sortL;
		bool d = b.descL;
		if (c == BLDriver || c == BLDesc) {
			std::vector<int> o(s.tags.size());
			for (size_t i = 0; i < o.size(); i++) o[i] = (int)i;
			std::sort(o.begin(),o.end(),[&](int x,int y) {
				const std::string& p1 = c == BLDriver ? s.tags[x].driver : s.tags[x].desc;
				const std::string& p2 = c == BLDriver ? s.tags[y].driver : s.tags[y].desc;
				return p1 < p2;
			});
			for (size_t i = 0; i < o.size(); i++) rank[o[i]] = (int)i;
		}
		std::sort(b.idx.begin(),b.idx.end(),[&](int x,int y) {
			const BigRow& r1 = rs[x];
			const BigRow& r2 = rs[y];
			switch (c) {
				case BLAddr: if (r1.addr != r2.addr) return d ? r1.addr > r2.addr : r1.addr < r2.addr; break;
				case BLSize: if (r1.size != r2.size) return d ? r1.size > r2.size : r1.size < r2.size; break;
				case BLPool: if (r1.np != r2.np) return d ? r1.np : r2.np; break;
				case BLTag: if (r1.tag != r2.tag) return d ? r1.tag > r2.tag : r1.tag < r2.tag; break;
				default: if (rank[r1.tag] != rank[r2.tag]) return d ? rank[r1.tag] > rank[r2.tag] : rank[r1.tag] < rank[r2.tag]; break;
			}
			return x < y;
		});
	}

	void selRowMenu(int id,bool sel,int& selVar,bool& menu,int& menuCol,bool& dbl,float h = 0.0f) {
		bool hit = ImGui::Selectable("##r",sel,ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,ImVec2(0.0f,h));
		bool hov = ImGui::IsItemHovered();
		if (hit) {
			selVar = id;
			dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
		}
		if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
			selVar = id;
			menu = true;
			menuCol = ImGui::TableGetHoveredColumn();
		}
	}

	void bigList(const BigSnap& s) {
		ImGuiTableFlags fl = ImGuiTableFlags_SizingFixedFit;
		if (!ui::beginTable("##kbigl",BLN,ImVec2(0,0),fl)) return;
		const char* titles[BLN] = {"Address","Size","Pool","Tag","Driver","Description"};
		if (!kWin) {
			titles[BLTag] = "Caller";
		}
		const char* tips[BLN] = {
			"Kernel virtual address. N/A when the OS blanked it.",
			"Size of this one block.",
			"Non-paged is always in RAM. Linux vmalloc is always non-paged.",
			kWin ? "Pool tag of this block." : "Kernel function that asked for it, from /proc/vmallocinfo.",
			"From pooltag.txt next to the exe.",
			"From pooltag.txt next to the exe."
		};
		float w[BLN] = {150,90,84,kWin ? 60.0f : 200.0f,96,0};
		int lead = s.blanked ? BLSize : BLAddr; // all N/A, the warning above already says why
		for (int c = 0; c < BLN; c++) {
			ImGuiTableColumnFlags f = c == BLDesc ? ImGuiTableColumnFlags_WidthStretch : ImGuiTableColumnFlags_WidthFixed;
			if ((!kWin && c == BLPool) || (!s.tagFile && (c == BLDriver || c == BLDesc)) || (s.blanked && c == BLAddr)) f |= ImGuiTableColumnFlags_Disabled;
			if (c == lead) f |= ImGuiTableColumnFlags_NoHide;
			if (c == BLSize) f |= ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending;
			ImGui::TableSetupColumn(titles[c],f,c == BLDesc ? 0.0f : std::max(ui::px(w[c]),ImGui::CalcTextSize(titles[c]).x + arrowW()));
		}
		ImGui::TableSetupScrollFreeze(0,1);
		if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
			if (sp->SpecsDirty && sp->SpecsCount > 0) {
				b.sortL = sp->Specs[0].ColumnIndex;
				b.descL = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
				sp->SpecsDirty = false;
				b.dirty = true;
			}
		}
		for (int c = 0; c < BLN; c++) b.onL[c] = (ImGui::TableGetColumnFlags(c) & ImGuiTableColumnFlags_IsEnabled) != 0;
		if (b.dirty) bigRebuild();
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int c = 0; c < BLN; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			ImGui::PushID(c);
			ui::headerCell(titles[c],tips[c],c == BLSize);
			ImGui::PopID();
		}
		ImVec4 cSel = ui::c.accent;
		cSel.w = 0.22f;
		ImGui::PushStyleColor(ImGuiCol_Header,cSel);
		ImGuiListClipper clip;
		clip.Begin((int)b.idx.size());
		bool dbl = false;
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
				const BigRow& r = s.rows[b.idx[i]];
				const BigTag& t = s.tags[r.tag];
				ImGui::TableNextRow();
				ImGui::PushID(b.idx[i]);
				if (ImGui::TableSetColumnIndex(lead)) {
					float x0 = ImGui::GetCursorPosX();
					selRowMenu(b.idx[i],b.sel == b.idx[i],b.sel,b.menu,b.menuCol,dbl);
					ImGui::SameLine(0.0f,0.0f);
					ImGui::SetCursorPosX(x0);
				}
				if (!s.blanked && ImGui::TableSetColumnIndex(BLAddr)) {
					if (r.addr) {
						ui::pushMonoCell();
						ImGui::Text("%s",util::hex(r.addr).c_str());
						ui::popFont();
					} else ImGui::TextDisabled("N/A");
				}
				if (ImGui::TableSetColumnIndex(BLSize)) ui::cellRight(util::size(r.size).c_str());
				if (ImGui::TableSetColumnIndex(BLPool)) ImGui::TextUnformatted(r.np ? "Non-paged" : "Paged");
				if (ImGui::TableSetColumnIndex(BLTag)) ImGui::TextUnformatted(t.tag.c_str());
				if (ImGui::TableSetColumnIndex(BLDriver)) {
					ui::textEllipsis(t.driver.c_str());
					if (!t.driver.empty()) ui::tip(t.driver.c_str());
				}
				if (ImGui::TableSetColumnIndex(BLDesc)) {
					ui::textEllipsis(t.desc.c_str());
					if (!t.desc.empty()) ui::tip(t.desc.c_str());
				}
				ImGui::PopID();
			}
		}
		ImGui::PopStyleColor();
		if (b.idx.empty()) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(lead);
			ImGui::TextDisabled("%s",!b.hasTag ? "No match" : kWin ? "No big blocks with this tag" : "No blocks from this caller");
		}
		ui::endTable();
	}

	void bigGroup(const BigSnap& s) {
		ImGuiTableFlags fl = ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;
		if (!ui::beginTable("##kbigg",BGN,ImVec2(0,0),fl)) return;
		const char* titles[BGN] = {kWin ? "Tag" : "Caller","Driver","Description","Blocks","Bytes","NP blocks","NP bytes","Paged blocks","Paged bytes","Largest","Tag table bytes"};
		const char* tips[BGN] = {
			kWin ? "Pool tag." : "Kernel function that asked for the blocks.",
			"From pooltag.txt next to the exe.",
			"From pooltag.txt next to the exe.",
			"Big blocks with this tag.",
			"Their sum.",
			"Non-paged blocks.",
			"Non-paged bytes.",
			"Paged blocks.",
			"Paged bytes.",
			"Its largest block.",
			"What the pool tag table reports for this tag. It already includes these big blocks, so do not add the two. Measured on Windows build 26200 : no tag had more big bytes than tag bytes, and for about half of the tags the two were equal."
		};
		float w[BGN] = {kWin ? 60.0f : 200.0f,96,190,70,84,76,84,86,84,84,104};
		for (int c = 0; c < BGN; c++) {
			ImGuiTableColumnFlags f = ImGuiTableColumnFlags_WidthFixed;
			if ((!kWin && (c == BGPool || c == BGNpN || c == BGNpBytes || c == BGPN || c == BGPBytes)) || (!s.tagFile && (c == BGDriver || c == BGDesc))) f |= ImGuiTableColumnFlags_Disabled;
			if (c == BGTag) f |= ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder;
			if (c == BGBytes) f |= ImGuiTableColumnFlags_DefaultSort;
			if (c >= BGBlocks) f |= ImGuiTableColumnFlags_PreferSortDescending;
			ImGui::TableSetupColumn(titles[c],f,std::max(ui::px(w[c]),ImGui::CalcTextSize(titles[c]).x + arrowW()));
		}
		ImGui::TableSetupScrollFreeze(1,2);
		if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
			if (sp->SpecsDirty && sp->SpecsCount > 0) {
				b.sortG = sp->Specs[0].ColumnIndex;
				b.descG = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
				sp->SpecsDirty = false;
				b.dirty = true;
			}
		}
		for (int c = 0; c < BGN; c++) b.onG[c] = (ImGui::TableGetColumnFlags(c) & ImGuiTableColumnFlags_IsEnabled) != 0;
		if (b.dirty) bigRebuild();
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int c = 0; c < BGN; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			ImGui::PushID(c);
			ui::headerCell(titles[c],tips[c],c >= BGBlocks);
			ImGui::PopID();
		}
		BigTag sum;
		for (int i : b.idx) {
			const BigTag& t = s.tags[i];
			sum.n += t.n;
			sum.bytes += t.bytes;
			sum.npN += t.npN;
			sum.npBytes += t.npBytes;
			sum.largest = std::max(sum.largest,t.largest);
		}
		ImGui::TableNextRow();
		ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,ImGui::GetColorU32(ui::c.panel2));
		ImGui::PushStyleColor(ImGuiCol_Text,ui::c.dim);
		for (int c = 0; c < BGN; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			if (c == BGTag) ImGui::Text("Total");
			else if (c >= BGBlocks && c != BGPool && c != BGMax) ui::cellRight(groupCell(sum,c,SIZE_MAX).c_str());
		}
		ImGui::PopStyleColor();
		ImVec4 cSel = ui::c.accent;
		cSel.w = 0.22f;
		ImGui::PushStyleColor(ImGuiCol_Header,cSel);
		ImGuiListClipper clip;
		clip.Begin((int)b.idx.size());
		bool dbl = false;
		int hitTag = -1;
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
				const BigTag& t = s.tags[b.idx[i]];
				ImGui::TableNextRow();
				ImGui::PushID(b.idx[i]);
				if (ImGui::TableSetColumnIndex(BGTag)) {
					float x0 = ImGui::GetCursorPosX();
					dbl = false;
					selRowMenu(b.idx[i],b.sel == b.idx[i],b.sel,b.menu,b.menuCol,dbl);
					if (dbl) hitTag = b.idx[i];
					ImGui::SameLine(0.0f,0.0f);
					ImGui::SetCursorPosX(x0);
					ImGui::TextUnformatted(t.tag.c_str());
				}
				for (int c = 1; c < BGN; c++) {
					if (!ImGui::TableSetColumnIndex(c)) continue;
					std::string v = groupCell(t,c,(size_t)b.idx[i]);
					if (c < BGBlocks) {
						ui::textEllipsis(v.c_str());
						if (!v.empty()) ui::tip(v.c_str());
					} else if (v == "N/A") dimRight("N/A");
					else ui::cellRight(v.c_str());
				}
				ImGui::PopID();
			}
		}
		ImGui::PopStyleColor();
		if (b.idx.empty()) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(BGTag);
			ImGui::TextDisabled("No match");
		}
		ui::endTable();
		if (hitTag >= 0) pickTag(s.tags[hitTag].tag,s.tags[hitTag].id);
	}

	std::string bigLine(const BigSnap& s) {
		std::string o;
		bool first = true;
		if (b.mode == 1) {
			if (b.sel < 0 || b.sel >= (int)s.tags.size()) return o;
			for (int c = 0; c < BGN; c++) {
				if (!b.onG[c]) continue;
				if (!first) o += '\t';
				first = false;
				o += groupCell(s.tags[b.sel],c,(size_t)b.sel);
			}
			return o + "\n";
		}
		if (b.sel < 0 || b.sel >= (int)s.rows.size()) return o;
		for (int c = 0; c < BLN; c++) {
			if (!b.onL[c]) continue;
			if (!first) o += '\t';
			first = false;
			o += bigCell(s,s.rows[b.sel],c);
		}
		return o + "\n";
	}

	void bigMenu(const BigSnap& s) {
		if (b.menu) {
			ImGui::OpenPopup("##bmenu");
			b.menu = false;
		}
		if (!ImGui::BeginPopup("##bmenu")) return;
		bool grp = b.mode == 1;
		bool ok = b.sel >= 0 && b.sel < (int)(grp ? s.tags.size() : s.rows.size());
		if (grp && ImGui::MenuItem("Show blocks",nullptr,false,ok) && ok) {
			pickTag(s.tags[b.sel].tag,s.tags[b.sel].id);
			ok = false;
		}
		if (ImGui::MenuItem("Copy row","Ctrl+C",false,ok) && ok) ImGui::SetClipboardText(bigLine(s).c_str());
		std::string cell = !ok || b.menuCol < 0 ? "" : grp ? groupCell(s.tags[b.sel],b.menuCol,(size_t)b.sel) : bigCell(s,s.rows[b.sel],b.menuCol);
		if (ImGui::MenuItem("Copy cell",nullptr,false,!cell.empty())) ImGui::SetClipboardText(cell.c_str());
		ImGui::EndPopup();
	}

	void drawBig() {
		if (!b.asked) {
			b.asked = true;
			if (!bigLatest()) bigStart();
		}
		auto s = bigLatest();
		if (s != b.snap) {
			b.snap = s;
			b.sel = -1;
			b.dirty = true;
		}
		bool busy = b.job && !b.job->done;

		ui::beginCard("##kbbar",nullptr);
		ImGui::BeginDisabled(busy);
		if (ui::iconBtn("kbref",ui::IcoRefresh,"Read the list again. It can be 100k rows, so it never runs on a timer.")) bigStart();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (b.focus) {
			ImGui::SetKeyboardFocusHere();
			b.focus = false;
		}
		if (ui::search("##kbf",b.filter,sizeof(b.filter),kWin ? "Filter : tag, driver, description" : "Filter : caller",220.0f)) {
			b.low = util::lower(b.filter);
			b.sel = -1;
			b.dirty = true;
		}
		ui::tip("Ctrl+F. Ctrl+K stays the command palette.");
		static const char* const names[] = {"List",kWin ? "By tag" : "By caller"};
		const ImVec4 tints[] = {ui::c.accent,ui::c.accent};
		flow(btnW(names[0]) + btnW(names[1]));
		ImGui::BeginGroup();
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(0.0f,0.0f));
		ImGui::Dummy(ImVec2(1.0f,ImGui::GetStyle().FramePadding.y));
		ImGui::PopStyleVar();
		if (ui::segmented("##kbmode",names,tints,2,b.mode)) {
			b.dirty = true;
			b.sel = -1;
		}
		ImGui::EndGroup();
		if (b.hasTag) {
			std::string chip = "Only " + b.tag + "  x##kbtag";
			flow(btnW(chip.c_str()));
			if (ImGui::Button(chip.c_str())) {
				b.hasTag = false;
				b.sel = -1;
				b.dirty = true;
			}
			ui::tip(kWin ? "Showing one tag, click to show all." : "Showing one caller, click to show all.");
		}
		if (busy) ImGui::TextDisabled("Reading the list...");
		else if (s && s->err.empty()) {
			std::string l = util::count(s->rows.size()) + " blocks, " + util::size(s->bytes);
			if (kWin) l += ", non-paged " + util::size(s->npBytes);
			l += ", read " + ago(s->t) + ". Showing " + util::count(b.idx.size()) + ".";
			ImGui::TextDisabled("%s",l.c_str());
		} else if (s) ImGui::TextColored(ui::c.warn,"Read failed %s.",ago(s->t).c_str());
		else ImGui::TextDisabled("Nothing read yet.");
		if (s && s->blanked) {
			warnLine(!kWin ? "Addresses are hidden : /proc/vmallocinfo shows 0 for every block, the kptr_restrict setting blanks them. Size and caller are real." :
				isElevated() ? "Addresses are hidden : every block came back with address 0 even though FRS is elevated. Size, tag and pool type are real." :
				"Addresses are hidden : Windows returned address 0 for every block, FRS is not elevated. Size, tag and pool type are real. Run FRS as administrator to try to see them.");
		}
		ui::endCard();

		if (!s) {
			ui::empty("Reading the list");
			return;
		}
		if (!s->err.empty()) {
			ui::empty(s->err.c_str());
			return;
		}
		if (!ImGui::GetIO().WantTextInput && b.sel >= 0 && pageKeys() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) {
			std::string l = bigLine(*s);
			if (!l.empty()) ImGui::SetClipboardText(l.c_str());
		}
		if (b.mode == 1) bigGroup(*s);
		else bigList(*s);
		bigMenu(*s);
	}

	// ---------- Page files ----------

	enum { PFName,PFBar,PFUsed,PFPeak,PFSize,PFMin,PFMax,PFN };

	struct PfPage {
		int sortCol = PFName;
		bool desc = false;
		int sel = -1,menuCol = 0;
		bool menu = false;
		bool on[PFN] = {};
	};
	PfPage pf;

	std::string pfCell(const PageFile& r,int c) {
		switch (c) {
			case PFName: return r.path;
			case PFBar: return r.size && r.size != NA && r.used != NA ? util::pct(r.used,r.size) : "N/A";
			case PFUsed: return util::size(r.used);
			case PFPeak: return util::size(r.peak);
			case PFSize: return util::size(r.size);
			case PFMin: return util::size(r.min);
			case PFMax: return util::size(r.max);
		}
		return "";
	}

	void drawPages() {
		auto s = pfLatest();
		if (!s) {
			ui::empty("Reading the page file list");
			return;
		}
		if (!s->err.empty()) {
			ui::empty(s->err.c_str());
			return;
		}
		uint64_t sz = 0,used = 0,peak = 0;
		bool unk = false;
		for (auto& r : s->rows) {
			if (r.size != NA) sz += r.size;
			if (r.used != NA) used += r.used;
			if (r.peak != NA) peak += r.peak;
			if (r.size == NA || r.used == NA) unk = true;
		}
		std::string fr = unk ? "N/A" : util::size(sz > used ? sz - used : 0);
		bool any = !s->rows.empty();
		ImVec4 cUse = sz ? ui::loadColor((float)((double)used / (double)sz)) : ui::c.text;
		ImVec4 cPeak = sz ? ui::loadColor((float)((double)peak / (double)sz)) : ui::c.text;
		Card cs[] = {
			{"##kpf1",kWin ? "In use" : "Swap in use",any ? util::size(used) : "N/A",any && sz ? util::pct(used,sz) + " of " + util::size(sz) + ", " + fr + " free" : "none active",cUse},
			{"##kpf2",s->ownPeak ? "Peak since FRS started" : "Peak",any ? util::size(peak) : "N/A",s->ownPeak ? "tracked by FRS" : "since boot",cPeak},
			{"##kpf3","Total size",any ? util::size(sz) : "N/A",std::to_string(s->rows.size()) + (kWin ? (s->rows.size() == 1 ? " page file" : " page files") : (s->rows.size() == 1 ? " swap area" : " swap areas")),ui::c.text}
		};
		cards(cs,3);

		ui::beginCard("##kpfbar",nullptr);
		if (cfg::getb("memPaused",false)) ImGui::TextColored(ui::c.warn,"Sampling is paused. Refresh or F5 reads it again.");
		else ImGui::TextDisabled("Updates on its own while this page is open.");
		ImGui::SameLine();
		ui::help(kWin ? "Windows lists what it has open as paging files. swapfile.sys, used for suspended store apps, is not in this list." : "From /proc/swaps. The kernel keeps no peak, FRS tracks the highest value it sees while it runs. Swap has no minimum or maximum.");
		ui::endCard();

		if (!any) {
			ui::empty(kWin ? "No page file is active, Windows is running without one." : "No swap is active.");
			return;
		}
		if (!ui::beginTable("##kpft",PFN,ImVec2(0,0),ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit)) return;
		const char* titles[PFN] = {"File name","Used %","Usage",s->ownPeak ? "Peak here" : "Peak usage","Total","Minimum","Maximum"};
		const char* tips[PFN] = {"Full path of the file.","Usage as a share of the current size.","In use now.",s->ownPeak ? "Highest seen since FRS started." : "Highest use since boot.","Current size. It can grow up to Maximum.","Configured minimum size.","Configured maximum size."};
		float w[PFN] = {0,90,78,78,78,78,78};
		float nameW = ui::px(120.0f);
		for (auto& r : s->rows) nameW = std::max(nameW,ImGui::CalcTextSize(r.path.c_str()).x + ui::px(16.0f));
		for (int c = 0; c < PFN; c++) {
			ImGuiTableColumnFlags f = ImGuiTableColumnFlags_WidthFixed;
			if (!kWin && (c == PFMin || c == PFMax)) f |= ImGuiTableColumnFlags_Disabled;
			if (c == PFName) f |= ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_DefaultSort;
			else f |= ImGuiTableColumnFlags_PreferSortDescending;
			ImGui::TableSetupColumn(titles[c],f,c == PFName ? nameW : std::max(ui::px(w[c]),ImGui::CalcTextSize(titles[c]).x + arrowW()));
		}
		ImGui::TableSetupScrollFreeze(0,1);
		if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
			if (sp->SpecsCount > 0) {
				pf.sortCol = sp->Specs[0].ColumnIndex;
				pf.desc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
			}
			sp->SpecsDirty = false;
		}
		for (int c = 0; c < PFN; c++) pf.on[c] = (ImGui::TableGetColumnFlags(c) & ImGuiTableColumnFlags_IsEnabled) != 0;
		std::vector<int> idx(s->rows.size());
		for (size_t i = 0; i < idx.size(); i++) idx[i] = (int)i;
		const auto& rs = s->rows;
		std::sort(idx.begin(),idx.end(),[&](int x,int y) {
			const PageFile& p1 = rs[x];
			const PageFile& p2 = rs[y];
			uint64_t v1 = 0,v2 = 0;
			switch (pf.sortCol) {
				case PFName: {
					int k = p1.path.compare(p2.path);
					if (k) return pf.desc ? k > 0 : k < 0;
					break;
				}
				case PFBar: v1 = p1.size && p1.size != NA && p1.used != NA ? p1.used * 1000 / p1.size : 0; v2 = p2.size && p2.size != NA && p2.used != NA ? p2.used * 1000 / p2.size : 0; break;
				case PFUsed: v1 = p1.used; v2 = p2.used; break;
				case PFPeak: v1 = p1.peak; v2 = p2.peak; break;
				case PFSize: v1 = p1.size; v2 = p2.size; break;
				case PFMin: v1 = p1.min; v2 = p2.min; break;
				case PFMax: v1 = p1.max; v2 = p2.max; break;
			}
			if (v1 != v2) {
				if ((v1 == NA) != (v2 == NA)) return v2 == NA;
				return pf.desc ? v1 > v2 : v1 < v2;
			}
			return x < y;
		});
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int c = 0; c < PFN; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			ImGui::PushID(c);
			ui::headerCell(titles[c],tips[c],c >= PFUsed);
			ImGui::PopID();
		}
		ImVec4 cSel = ui::c.accent;
		cSel.w = 0.22f;
		ImGui::PushStyleColor(ImGuiCol_Header,cSel);
		for (int i : idx) {
			const PageFile& r = rs[i];
			ImGui::TableNextRow();
			ImGui::PushID(i);
			if (ImGui::TableSetColumnIndex(PFName)) {
				float x0 = ImGui::GetCursorPosX();
				bool dbl = false;
				selRowMenu(i,pf.sel == i,pf.sel,pf.menu,pf.menuCol,dbl,ImGui::GetFrameHeight());
				ImGui::SameLine(0.0f,0.0f);
				ImGui::SetCursorPosX(x0);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(r.path.c_str());
			}
			if (ImGui::TableSetColumnIndex(PFBar)) {
				bool known = r.size && r.size != NA && r.used != NA;
				float fr = known ? (float)((double)r.used / (double)r.size) : 0.0f;
				ui::meter(fr,pfCell(r,PFBar).c_str(),ImVec2(0.0f,ImGui::GetFrameHeight() / ui::px(1.0f)),ui::loadColor(fr));
			}
			for (int c = PFUsed; c < PFN; c++) {
				if (!ImGui::TableSetColumnIndex(c)) continue;
				std::string v = pfCell(r,c);
				if (v == "N/A") dimRight("N/A");
				else ui::cellRight(v.c_str());
			}
			ImGui::PopID();
		}
		ImGui::PopStyleColor();
		ui::endTable();
		if (pf.menu) {
			ImGui::OpenPopup("##pfmenu");
			pf.menu = false;
		}
		if (ImGui::BeginPopup("##pfmenu")) {
			bool ok = pf.sel >= 0 && pf.sel < (int)rs.size();
			if (ImGui::MenuItem("Copy row",nullptr,false,ok) && ok) {
				std::string o;
				for (int c = 0; c < PFN; c++) {
					if (c == PFBar || !pf.on[c]) continue;
					if (c != PFName) o += '\t';
					o += pfCell(rs[pf.sel],c);
				}
				ImGui::SetClipboardText(o.c_str());
			}
			std::string cell = ok && pf.menuCol >= 0 ? pfCell(rs[pf.sel],pf.menuCol) : "";
			if (ImGui::MenuItem("Copy cell",nullptr,false,!cell.empty())) ImGui::SetClipboardText(cell.c_str());
			ImGui::EndPopup();
		}
	}

	// ---------- Compression ----------

	void drawComp() {
		auto m = sysmem::latest();
		bool hasRatio = m->compData != NA && m->compSize != NA && m->compData && m->compSize;
		double ratio = hasRatio ? (double)m->compData / (double)m->compSize : 0.0;
		if (m->compWs == NA && !hasRatio) {
			ui::empty(m->compWhy.empty() ? "No compression store numbers on this system." : m->compWhy.c_str());
			return;
		}
		ImGui::BeginChild("##kcomp");
		char rt[32] = "N/A";
		if (hasRatio) snprintf(rt,sizeof(rt),"%.2f : 1",ratio);
		uint64_t holds = hasRatio && m->compWs != NA ? (uint64_t)std::llround((double)m->compWs * ratio) : NA; // rounded, on zswap it must come out as the data exactly
		uint64_t saved = holds == NA ? NA : holds > m->compWs ? holds - m->compWs : 0; // ratio under 1 : the store costs more than it holds
		uint64_t rest = holds != NA && m->compData > holds ? m->compData - holds : 0; // before compression, not in the working set
		Card cs[] = {
			{"##kcc1","Store in RAM",util::size(m->compWs),kWin ? "Its working set" : "zram or zswap",ui::c.kernel,kWin ? "Working set of the Memory Compression process. The store can also sit partly in the page file, that part is not RAM and not in this number." : "Memory the zram device or zswap pool uses."},
			{"##kcc2","Held in RAM",util::size(holds),"before compression",ui::c.text,"Store in RAM times the ratio, the same sum the Memory page uses. Only the part in RAM, Data compressed below also counts what sits outside it."},
			{"##kcc3","Memory saved",util::size(saved),"RAM it spares",ui::c.good,"Held in RAM minus Store in RAM. 0 when the store is bigger than what it holds."},
			{"##kcc4","Ratio",rt,"data : compressed",ui::c.accent}
		};
		cards(cs,4);
		if (!m->compWhy.empty()) warnLine(m->compWhy.c_str());

		ui::beginCard("##kcgraph","Store over time");
		auto h = compHist();
		if (h->ws.size() >= 2) {
			float mx = 1.0f;
			for (float v : h->data) mx = std::max(mx,v);
			for (float v : h->ws) mx = std::max(mx,v);
			ui::Series ss[2] = {
				{h->data.data(),(int)h->data.size(),ui::c.good,"Held in RAM"},
				{h->ws.data(),(int)h->ws.size(),ui::c.kernel,"Store in RAM"}
			};
			auto ph = h;
			ui::graph("##kcg",ss,2,mx * 1.1f,ImVec2(0.0f,120.0f),[ph](int i) {
				char t[96];
				if (i < 0 || i >= (int)ph->ws.size()) return std::string();
				snprintf(t,sizeof(t),"Store in RAM %.0f MB, held in RAM %.0f MB",ph->ws[i],ph->data[i]);
				return std::string(t);
			});
			ui::Seg lg[2] = {{1.0f,ui::c.good,"Held in RAM"},{1.0f,ui::c.kernel,"Store in RAM"}};
			ui::legend(lg,2);
		} else ImGui::TextDisabled("History starts when the first samples are in.");
		ui::endCard();

		if (m->compWs != NA && saved != NA) {
			ui::beginCard("##kcbar","Where the data sits");
			ui::Seg seg[3] = {{(float)m->compWs,ui::c.kernel,"Store in RAM"},{(float)saved,ui::c.good,"Saved by compressing"},{(float)rest,ui::c.modified,"Outside its working set, before compression"}};
			ui::stackBar("##kcsb",seg,3,ImVec2(0.0f,24.0f));
			ui::legend(seg,3);
			if (rest) {
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextDisabled("%s of the %s is outside the store's working set, in the page file or on its way there. Size before compression.",util::size(rest).c_str(),util::size(m->compData).c_str());
				ImGui::PopTextWrapPos();
			}
			ui::endCard();
		}

		ui::beginCard("##kckv","Store counters");
		ui::kvSize("Data compressed",m->compData);
		ui::tip("Data in the store before compression, RAM and page file parts together.");
		ui::kvSize("Compressed size",m->compSize);
		ui::tip("The same data after compression.");
		ui::kvSize("Unique data",m->compUnique);
		ui::tip("Data counted once when pages are identical. Linux has no such number.");
		ui::kv("Process ID",m->compPid ? std::to_string(m->compPid) : "N/A");
		ui::tip(kWin ? "The Memory Compression process. Windows protects it, its memory can't be read or trimmed from user mode." : "zram and zswap live in the kernel, there is no process.");
		ui::endCard();
		ImGui::EndChild();
	}

	void draw() {
		uint64_t now = util::nowMs();
		wantPool(std::max<uint64_t>(2500,2 * (uint64_t)sampler::intervalMs()));
		if (!g_lastDraw || now - g_lastDraw > 2000) {
			task::run([](task::T&) {
				sampleNow();
				return std::string();
			},false);
		}
		g_lastDraw = now;

		static bool first = true;
		if (first) {
			first = false;
			const char* t = util::testEnv("FRS_TEST_TAB");
			if (t && !util::testEnv("FRS_TEST_PROC")) for (int i = 0; i < 4; i++) if (!strcmp(t,kTabs[i])) g_pick = i;
		}

		bool find = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F) && pageKeys();
		if (!ImGui::BeginTabBar("##ktabs")) return;
		int pick = g_pick;
		g_pick = -1;
		if (ImGui::BeginTabItem(kTabs[TabPool],nullptr,pick == TabPool ? ImGuiTabItemFlags_SetSelected : 0)) {
			if (find) p.focus = true;
			drawPool();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(kTabs[TabBig],nullptr,pick == TabBig ? ImGuiTabItemFlags_SetSelected : 0)) {
			if (find) b.focus = true;
			drawBig();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(kTabs[TabPages],nullptr,pick == TabPages ? ImGuiTabItemFlags_SetSelected : 0)) {
			drawPages();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(kTabs[TabComp],nullptr,pick == TabComp ? ImGuiTabItemFlags_SetSelected : 0)) {
			drawComp();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	// ---------- Object charges, process window ----------

	enum { OHandle,OType,OPaged,ONonPaged,ON };

	struct ObjPage {
		Snap<ObjSnap> snap;
		std::shared_ptr<const ObjSnap> seen;
		std::shared_ptr<task::T> job;
		std::vector<int> idx;
		char filter[64] = {};
		bool asked = false,dirty = true;
		int sortCol = ONonPaged;
		bool desc = true;
	};
	std::map<std::pair<uint32_t,uint64_t>,std::shared_ptr<ObjPage>> g_objs;

	void objStart(const std::shared_ptr<ObjPage>& o,const ProcRef& r) {
		if (o->job && !o->job->done) return;
		o->job = task::run([o,r](task::T& t) {
			auto s = std::make_shared<ObjSnap>();
			s->err = osObjects(r,*s,t.cancel);
			s->t = util::nowMs();
			auto old = o->snap.get();
			if (!s->err.empty() && old && old->err.empty()) {
				auto k = std::make_shared<ObjSnap>(*old);
				k->stale = s->err;
				s = k;
			}
			o->snap.put(s);
			return std::string();
		},false);
	}

	void drawObj(const ProcRef& r) {
		if (!kWin) {
			ui::empty("Not available on Linux, the kernel keeps no pool charge per object.");
			return;
		}
		auto key = std::make_pair(r.pid,r.start);
		auto it = g_objs.find(key);
		if (it == g_objs.end()) {
			if (g_objs.size() > 16) g_objs.clear();
			it = g_objs.emplace(key,std::make_shared<ObjPage>()).first;
		}
		auto o = it->second;
		if (!o->asked) {
			o->asked = true;
			objStart(o,r);
		}
		auto s = o->snap.get();
		if (s != o->seen) {
			o->seen = s;
			o->dirty = true;
		}
		bool busy = o->job && !o->job->done;

		ui::beginCard("##kobar",nullptr);
		ImGui::BeginDisabled(busy);
		if (ui::iconBtn("koref",ui::IcoRefresh,"Read the handle list again")) objStart(o,r);
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ui::search("##kof",o->filter,sizeof(o->filter),"Filter : type",180.0f)) o->dirty = true;
		ImGui::SameLine();
		ui::help("Pool quota the kernel charges for the object behind each handle. It is a charge, not the object's real footprint, and two handles to one object both show it.");
		if (busy) ImGui::TextDisabled("Reading the handle list...");
		else if (s && s->err.empty()) {
			uint64_t pg = 0,np = 0;
			for (auto& x : s->rows) {
				if (x.paged != NA) pg += x.paged;
				if (x.nonPaged != NA) np += x.nonPaged;
			}
			std::string l = util::count(s->rows.size()) + " handles, charged paged " + util::size(pg) + ", non-paged " + util::size(np) + ", read " + ago(s->t) + ".";
			if (s->failed) l += " " + util::count(s->failed) + " could not be opened, they show N/A.";
			ImGui::TextDisabled("%s",l.c_str());
			if (!s->stale.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text,ui::c.warn);
				ImGui::TextWrapped("Read failed, these rows are from the last good read. %s",s->stale.c_str());
				ImGui::PopStyleColor();
			}
		}
		ui::endCard();

		if (!s) {
			ui::empty("Reading the handle list");
			return;
		}
		if (!s->err.empty()) {
			ui::empty(s->err.c_str());
			return;
		}
		if (!ui::beginTable("##kobj",ON,ImVec2(0,0),ImGuiTableFlags_SizingFixedFit)) return;
		const char* titles[ON] = {"Handle","Type","Paged charge","Non-paged charge"};
		float w[ON] = {90,0,100,120};
		for (int c = 0; c < ON; c++) {
			ImGuiTableColumnFlags f = c == OType ? ImGuiTableColumnFlags_WidthStretch : ImGuiTableColumnFlags_WidthFixed;
			if (c == OHandle) f |= ImGuiTableColumnFlags_NoHide;
			if (c >= OPaged) f |= ImGuiTableColumnFlags_PreferSortDescending;
			if (c == ONonPaged) f |= ImGuiTableColumnFlags_DefaultSort;
			ImGui::TableSetupColumn(titles[c],f,c == OType ? 0.0f : std::max(ui::px(w[c]),ImGui::CalcTextSize(titles[c]).x + arrowW()));
		}
		ImGui::TableSetupScrollFreeze(0,1);
		if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
			if (sp->SpecsDirty && sp->SpecsCount > 0) {
				o->sortCol = sp->Specs[0].ColumnIndex;
				o->desc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
				sp->SpecsDirty = false;
				o->dirty = true;
			}
		}
		if (o->dirty) {
			o->dirty = false;
			o->idx.clear();
			std::string low = util::lower(o->filter);
			for (int i = 0; i < (int)s->rows.size(); i++) if (low.empty() || util::lower(s->rows[i].type).find(low) != std::string::npos) o->idx.push_back(i);
			int c = o->sortCol;
			bool d = o->desc;
			const auto& rs = s->rows;
			std::sort(o->idx.begin(),o->idx.end(),[&](int x,int y) {
				const ObjRow& a = rs[x];
				const ObjRow& e = rs[y];
				if (c == OType) {
					int k = a.type.compare(e.type);
					if (k) return d ? k > 0 : k < 0;
				} else {
					uint64_t v1 = c == OHandle ? a.handle : c == OPaged ? a.paged : a.nonPaged;
					uint64_t v2 = c == OHandle ? e.handle : c == OPaged ? e.paged : e.nonPaged;
					if ((v1 == NA) != (v2 == NA)) return v2 == NA;
					if (v1 != v2) return d ? v1 > v2 : v1 < v2;
				}
				return a.handle < e.handle;
			});
		}
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int c = 0; c < ON; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			ImGui::PushID(c);
			ui::headerCell(titles[c],nullptr,c >= OPaged);
			ImGui::PopID();
		}
		ImGuiListClipper clip;
		clip.Begin((int)o->idx.size());
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
				const ObjRow& x = s->rows[o->idx[i]];
				ImGui::TableNextRow();
				if (ImGui::TableSetColumnIndex(OHandle)) {
					ui::pushMonoCell();
					ImGui::TextUnformatted(util::hex(x.handle).c_str());
					ui::popFont();
				}
				if (ImGui::TableSetColumnIndex(OType)) ImGui::TextUnformatted(x.type.c_str());
				if (ImGui::TableSetColumnIndex(OPaged)) ui::cellSize(x.paged);
				if (ImGui::TableSetColumnIndex(ONonPaged)) ui::cellSize(x.nonPaged);
			}
		}
		if (o->idx.empty()) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(OHandle);
			ImGui::TextDisabled("No match");
		}
		ui::endTable();
	}

}

namespace kmem {

	void init() {
		initData();
		cfg::optBool("poolHighlight","Kernel memory","Highlight pool tags","Tints a row for a moment when a new tag appears or when a tag grows past the threshold below.",true);
		cfg::optInt("poolHlKb","Kernel memory","Pool growth threshold, KB/s","Growth rate of a tag's bytes that counts as a jump.",256,1,1048576);
		nav::addPage("kernel","Kernel",ui::IcoKernel,nav::GrpInspect,draw);
		procwin::addTab("Object charges",90,drawObj);
	}

}
