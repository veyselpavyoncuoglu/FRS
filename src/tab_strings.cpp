#include "vmscan.h"
#include "cfg.h"
#include "ui.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#if __has_include("vmem.h")
#include "vmem.h"
#define HAS_HEX 1
#else
#define HAS_HEX 0
#endif

namespace {

	using namespace vmscan;

	const uint64_t UI_BYTES = (uint64_t)256 << 30;
	const uint64_t UI_MS = 600000;
	const uint64_t UI_TEXT = (uint64_t)1 << 30;
	const size_t UI_HITS = 1000000;
	const size_t SORT_ON_UI = 50000;
	const size_t COPY_CAP = (size_t)64 << 20;
	const uint64_t GONE_MS = 600000; // results of a process window that closed this long ago get freed
	const int MAX_WINS = 4;

	enum { CIdx,CBase,CAddr,CType,CLen,CStr,CProt,CKind,CN };
	const char* const colName[CN] = {"#","Base address","Address","Type","Length","String","Protection","Memory type"};

	using Rows = std::vector<Hit>;
	using Regs = std::vector<Region>;

	struct Filt {
		std::vector<uint32_t> rows;
		size_t end = 0; // rows before this one were looked at
	};

	// a table of hits with its own filter, sort and selection. Filter windows hold a deep copy of the rows.
	struct Res {
		std::shared_ptr<Rows> rows = std::make_shared<Rows>();
		std::shared_ptr<const Regs> regs;
		std::vector<uint8_t> sel;
		std::vector<uint32_t> view;
		size_t done = 0; // rows the filter has seen
		size_t nSel = 0;
		int anchor = -1;
		int hotCol = CStr;
		char filt[256] = "";
		bool regex = false,cased = false,ansi = true,uni = true,zero = false;
		Rx rx;
		std::string rxErr;
		int sortCol = CIdx;
		bool desc = false,needSort = false,inOrder = true;
		uint32_t gen = 0;
		std::shared_ptr<task::T> sortT;
		std::shared_ptr<std::vector<uint32_t>> sorted;
		uint32_t sortGen = 0;
		std::shared_ptr<task::T> filtT;
		std::shared_ptr<Filt> filtOut;
		bool on[CN] = {true,true,true,true,true,true,false,false};
	};

	struct Win {
		std::unique_ptr<Res> res;
		bool open = true;
		std::string title;
	};

	struct St {
		ProcRef r;
		int mode = MStrings,scanMode = MStrings;
		bool ext = false,priv = true,img = false,map = false;
		int minLen = 10,threads = 0;
		char pat[1024] = "";
		char rx[512] = "";
		bool rxCase = false;
		std::string why;
		Res res;
		std::shared_ptr<Scan> scan;
		std::shared_ptr<task::T> t;
		bool scanned = false,restart = false,optDirty = false;
		uint64_t lastPull = 0,goneAt = 0;
		int winN = 0;
		std::deque<Win> wins;
	};

	std::mutex g_m; // the map. UI thread draws under it, the freeing job takes entries out under it
	std::map<std::pair<uint32_t,uint64_t>,std::unique_ptr<St>> g_st;

	std::string addrStr(uint64_t v,bool zero) {
		if (!zero) return util::hex(v);
		char b[32];
		snprintf(b,sizeof(b),"0x%016llx",(unsigned long long)v);
		return b;
	}

	void loadOpts(St& s) {
		int f = cfg::geti("strFlags",0xb);
		s.res.ansi = f & 1;
		s.res.uni = f & 2;
		s.ext = f & 4;
		s.priv = f & 8;
		s.img = f & 16;
		s.map = f & 32;
		s.res.zero = f & 64;
		s.minLen = std::max(1,std::min(cfg::geti("strMinLen",10),100000));
		s.res.regex = cfg::getb("strRegex",false);
		s.res.cased = cfg::getb("strCase",false);
	}

	void saveOpts(const St& s) {
		cfg::seti("strFlags",(s.res.ansi ? 1 : 0) | (s.res.uni ? 2 : 0) | (s.ext ? 4 : 0) | (s.priv ? 8 : 0) | (s.img ? 16 : 0) | (s.map ? 32 : 0) | (s.res.zero ? 64 : 0));
		cfg::seti("strMinLen",s.minLen);
		cfg::setb("strRegex",s.res.regex);
		cfg::setb("strCase",s.res.cased);
	}

	int cmpText(const std::string& a,const std::string& b) {
		size_t n = std::min(a.size(),b.size());
		for (size_t i = 0; i < n; i++) {
			unsigned char x = (unsigned char)a[i],y = (unsigned char)b[i];
			if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + 32);
			if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + 32);
			if (x != y) return x < y ? -1 : 1;
		}
		return a.size() < b.size() ? -1 : a.size() > b.size();
	}

	std::string protOf(const Regs* regs,const Hit& h) {
		return regs && h.reg < regs->size() ? os::protStr((*regs)[h.reg].prot) : "";
	}

	bool hitLess(const Rows& rows,const Regs* regs,int col,bool desc,uint32_t a,uint32_t b) {
		const Hit& x = rows[a];
		const Hit& y = rows[b];
		int c = 0;
		auto num = [&](uint64_t p,uint64_t q) { c = p < q ? -1 : p > q; };
		switch (col) {
			case CBase: if (regs) num((*regs)[x.reg].base,(*regs)[y.reg].base); break;
			case CAddr: num(x.addr,y.addr); break;
			case CType: c = strcmp(encName(x.enc),encName(y.enc)); break;
			case CLen: num(x.chars,y.chars); break;
			case CStr: c = cmpText(x.text,y.text); break;
			case CProt: c = cmpText(protOf(regs,x),protOf(regs,y)); break;
			case CKind: if (regs) c = strcmp(kindName((*regs)[x.reg].kind),kindName((*regs)[y.reg].kind)); break;
			default: break;
		}
		if (!c) c = x.idx < y.idx ? -1 : x.idx > y.idx;
		return desc ? c > 0 : c < 0;
	}

	bool encOn(bool ansi,bool uni,const Hit& h) {
		if ((h.enc == EAnsi || h.enc == EUtf8) && !ansi) return false;
		return h.enc != EUtf16 || uni;
	}

	bool shown(const Res& s,const Hit& h) {
		if (!encOn(s.ansi,s.uni,h)) return false;
		if (!s.filt[0] || s.regex) return true; // only a regex that did not compile gets here, those show everything
		if (s.cased) return h.text.find(s.filt) != std::string::npos;
		return util::has(h.text,s.filt);
	}

	void refilter(Res& s) {
		if (s.filtT) s.filtT->cancel = true;
		s.filtOut.reset();
		s.view.clear();
		s.done = 0;
		s.inOrder = true;
		s.gen++;
		s.needSort = !(s.sortCol == CIdx && !s.desc);
		std::fill(s.sel.begin(),s.sel.end(),(uint8_t)0);
		s.nSel = 0;
		s.anchor = -1;
		s.rxErr.clear();
		if (s.regex && s.filt[0] && !s.rx.compile(s.filt,!s.cased,s.rxErr)) s.rx = Rx();
	}

	// one regex row can cost 100 ms and more, so those run on a task, a batch at a time. pull waits for it, the rows don't move under it
	void startFilter(Res& s) {
		auto rows = s.rows;
		auto rx = std::make_shared<Rx>(s.rx);
		auto out = std::make_shared<Filt>();
		size_t from = s.done;
		bool ansi = s.ansi,uni = s.uni;
		s.filtOut = out;
		s.filtT = task::run([rows,rx,out,from,ansi,uni](task::T& t) {
			uint64_t t0 = util::nowMs();
			size_t i = from;
			while (i < rows->size() && !t.cancel.load() && util::nowMs() - t0 < 200) {
				const Hit& h = (*rows)[i];
				if (encOn(ansi,uni,h) && rx->match(h.text.data(),h.text.size())) out->rows.push_back((uint32_t)i);
				i++;
			}
			out->end = i;
			return std::string();
		},false);
	}

	// a slice per frame, so a million rows never stall the UI
	void stepFilter(Res& s) {
		const Rows& rows = *s.rows;
		bool grew = false;
		if (s.filtT) {
			if (!s.filtT->done) return;
			if (s.filtOut) {
				grew = !s.filtOut->rows.empty();
				s.view.insert(s.view.end(),s.filtOut->rows.begin(),s.filtOut->rows.end());
				s.done = s.filtOut->end;
			}
			s.filtT.reset();
			s.filtOut.reset();
		}
		if (s.regex && s.filt[0] && s.rx.ok()) {
			if (s.done < rows.size()) startFilter(s);
		} else {
			uint64_t t0 = util::nowMs();
			int k = 0;
			while (s.done < rows.size()) {
				if (shown(s,rows[s.done])) {
					s.view.push_back((uint32_t)s.done);
					grew = true;
				}
				s.done++;
				if (++k % 256 == 0 && util::nowMs() - t0 >= 6) break;
			}
		}
		if (grew && !(s.sortCol == CIdx && !s.desc)) s.needSort = true;
		if (s.sel.size() < rows.size()) s.sel.resize(rows.size(),0);
	}

	void stepSort(Res& s,bool scanning) {
		if (s.sortT) {
			if (!s.sortT->done) return;
			if (s.sortGen == s.gen && s.sorted) {
				s.view.swap(*s.sorted);
				s.inOrder = s.sortCol == CIdx && !s.desc;
			}
			s.sortT.reset();
			s.sorted.reset();
		}
		if (!s.needSort || s.done < s.rows->size()) return;
		if (s.sortCol == CIdx && !s.desc && s.inOrder) {
			s.needSort = false;
			return;
		}
		if (s.view.size() <= SORT_ON_UI) {
			const Rows& rows = *s.rows;
			const Regs* rg = s.regs.get();
			int col = s.sortCol;
			bool desc = s.desc;
			std::sort(s.view.begin(),s.view.end(),[&](uint32_t a,uint32_t b) { return hitLess(rows,rg,col,desc,a,b); });
			s.inOrder = col == CIdx && !desc;
			s.needSort = false;
			return;
		}
		if (scanning) return;
		auto out = std::make_shared<std::vector<uint32_t>>(s.view);
		auto rows = s.rows;
		auto regs = s.regs;
		int col = s.sortCol;
		bool desc = s.desc;
		s.sortGen = s.gen;
		s.sorted = out;
		s.needSort = false;
		s.sortT = task::run([out,rows,regs,col,desc](task::T&) {
			std::sort(out->begin(),out->end(),[&](uint32_t a,uint32_t b) { return hitLess(*rows,regs.get(),col,desc,a,b); });
			return std::string();
		},false);
	}

	std::string cellText(const Res& s,const Hit& h,int col) {
		switch (col) {
			case CIdx: return std::to_string(h.idx);
			case CBase: return s.regs ? addrStr((*s.regs)[h.reg].base,s.zero) : "";
			case CAddr: return addrStr(h.addr,s.zero);
			case CType: return encName(h.enc);
			case CLen: return std::to_string(h.chars);
			case CStr: return h.text;
			case CProt: return protOf(s.regs.get(),h);
			case CKind: return s.regs ? kindName((*s.regs)[h.reg].kind) : "";
		}
		return "";
	}

	// tab separated visible columns, or one column
	void copySel(Res& s,int only) {
		std::string o;
		size_t rows = 0;
		bool cut = false;
		for (uint32_t v : s.view) {
			if (!s.sel[v]) continue;
			if (o.size() > COPY_CAP) {
				cut = true;
				break;
			}
			const Hit& h = (*s.rows)[v];
			if (only >= 0) o += cellText(s,h,only);
			else {
				bool f = true;
				for (int c = 0; c < CN; c++) {
					if (!s.on[c]) continue;
					if (!f) o += '\t';
					o += cellText(s,h,c);
					f = false;
				}
			}
			o += '\n';
			rows++;
		}
		if (!rows) return;
		ImGui::SetClipboardText(o.c_str());
		ui::toast(cut ? "Copied the first " + util::count(rows) + " rows, the clipboard limit is 64 MB" : "Copied " + util::count(rows) + (rows == 1 ? " row" : " rows"));
	}

	void openHex(const St& st,const Res& s,uint32_t row) {
		const Hit& h = (*s.rows)[row];
#if HAS_HEX
		vmem::hexAt(st.r,h.addr,h.bytes);
#else
		ImGui::SetClipboardText(util::hex(h.addr).c_str());
		ui::toast("No hex editor here, address " + util::hex(h.addr) + " copied");
#endif
	}

	void clickRow(Res& s,int vi,bool ctrl,bool shift) {
		uint32_t row = s.view[vi];
		if (shift && s.anchor >= 0 && s.anchor < (int)s.view.size()) {
			if (!ctrl) {
				std::fill(s.sel.begin(),s.sel.end(),(uint8_t)0);
				s.nSel = 0;
			}
			for (int i = std::min(vi,s.anchor); i <= std::max(vi,s.anchor); i++) {
				if (!s.sel[s.view[i]]) s.nSel++;
				s.sel[s.view[i]] = 1;
			}
			return;
		}
		if (ctrl) {
			s.nSel += s.sel[row] ? (size_t)-1 : 1;
			s.sel[row] = !s.sel[row];
		} else {
			std::fill(s.sel.begin(),s.sel.end(),(uint8_t)0);
			s.sel[row] = 1;
			s.nSel = 1;
		}
		s.anchor = vi;
	}

	void drawTable(const St& st,Res& s) {
		// own id per address width, so the columns get measured again when Zero pad changes
		ui::pushMono();
		float aw = ImGui::CalcTextSize(s.zero ? "0x0000000000000000" : "0x7ff000000000").x + ui::px(14.0f);
		ui::popFont();
		if (!ui::beginTable(s.zero ? "##strsz" : "##strs",CN)) return;
		ImGui::TableSetupScrollFreeze(0,1);
		// NoHide : the row's Selectable sits in this column and ImGui skips items in a hidden one
		ImGui::TableSetupColumn(colName[CIdx],ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortAscending | ImGuiTableColumnFlags_NoHide,ui::px(60.0f));
		ImGui::TableSetupColumn(colName[CBase],ImGuiTableColumnFlags_WidthFixed,aw);
		ImGui::TableSetupColumn(colName[CAddr],ImGuiTableColumnFlags_WidthFixed,aw);
		ImGui::TableSetupColumn(colName[CType],ImGuiTableColumnFlags_WidthFixed,ui::px(62.0f));
		ImGui::TableSetupColumn(colName[CLen],ImGuiTableColumnFlags_WidthFixed,ui::px(54.0f));
		ImGui::TableSetupColumn(colName[CStr],ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(colName[CProt],ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide,ui::px(70.0f));
		ImGui::TableSetupColumn(colName[CKind],ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide,ui::px(100.0f));
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		for (int c = 0; c < CN; c++) if (ImGui::TableSetColumnIndex(c)) ui::headerCell(colName[c],nullptr,c == CIdx || c == CLen);
		for (int c = 0; c < CN; c++) s.on[c] = (ImGui::TableGetColumnFlags(c) & ImGuiTableColumnFlags_IsEnabled) != 0;
		if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs()) {
			if (sp->SpecsDirty && sp->SpecsCount > 0) {
				int col = sp->Specs[0].ColumnIndex;
				bool desc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
				if (col != s.sortCol || desc != s.desc) {
					s.sortCol = col;
					s.desc = desc;
					s.needSort = true;
					s.gen++;
				}
				sp->SpecsDirty = false;
			}
		}
		ImGuiIO& io = ImGui::GetIO();
		bool menu = false;
		ImGuiListClipper clip;
		clip.Begin((int)s.view.size());
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
				uint32_t row = s.view[i];
				const Hit& h = (*s.rows)[row];
				ImGui::TableNextRow();
				ImGui::PushID(i);
				ImGui::TableSetColumnIndex(CIdx);
				bool isSel = s.sel[row] != 0;
				if (ImGui::Selectable("##r",isSel,ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) clickRow(s,i,io.KeyCtrl,io.KeyShift);
				if (ImGui::IsItemHovered()) {
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openHex(st,s,row);
					if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
						if (!isSel) clickRow(s,i,false,false);
						s.hotCol = ImGui::TableGetHoveredColumn();
						menu = true;
					}
					if (h.text.size() > 90 && ImGui::TableGetHoveredColumn() == CStr) {
						std::string t = h.text.substr(0,1500);
						if (h.chars > t.size()) t += " ...";
						ui::tip(t.c_str());
					}
				}
				ImGui::SameLine(0.0f,0.0f);
				ui::cellRight(std::to_string(h.idx).c_str());
				for (int c : {CBase,CAddr}) if (ImGui::TableSetColumnIndex(c)) {
					ui::pushMonoCell();
					ImGui::TextUnformatted(cellText(s,h,c).c_str());
					ui::popFont();
				}
				if (ImGui::TableSetColumnIndex(CType)) ImGui::TextUnformatted(encName(h.enc));
				if (ImGui::TableSetColumnIndex(CLen)) ui::cellRight(std::to_string(h.chars).c_str());
				if (ImGui::TableSetColumnIndex(CStr)) {
					// long strings stay cheap, the rest is in the tooltip and Copy
					size_t n = std::min(h.text.size(),(size_t)600);
					while (n < h.text.size() && ((unsigned char)h.text[n] & 0xC0) == 0x80) n--;
					ImGui::TextUnformatted(h.text.data(),h.text.data() + n);
				}
				if (ImGui::TableSetColumnIndex(CProt)) ImGui::TextUnformatted(cellText(s,h,CProt).c_str());
				if (ImGui::TableSetColumnIndex(CKind)) ImGui::TextUnformatted(cellText(s,h,CKind).c_str());
				ImGui::PopID();
			}
		}
		if (menu) ImGui::OpenPopup("##sctx");
		if (ImGui::BeginPopup("##sctx")) {
			int one = -1;
			if (s.nSel == 1) for (size_t v = 0; v < s.view.size(); v++) if (s.sel[s.view[v]]) {
				one = (int)s.view[v];
				break;
			}
			if (ImGui::MenuItem(HAS_HEX ? "Read/Write memory" : "Copy address",nullptr,false,one >= 0)) openHex(st,s,(uint32_t)one);
			ImGui::Separator();
			if (ImGui::MenuItem("Copy","Ctrl+C")) copySel(s,-1);
			int hc = s.hotCol >= 0 && s.hotCol < CN ? s.hotCol : CStr;
			std::string lb = std::string("Copy \"") + colName[hc] + "\"";
			if (ImGui::MenuItem(lb.c_str())) copySel(s,hc);
			if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		ui::endTable();
	}

	void startScan(St& s) {
		s.why.clear();
		s.restart = false;
		Cfg c;
		c.mode = (Mode)s.mode;
		c.minLen = s.minLen;
		c.ext = s.ext;
		c.kinds = (s.priv ? KPrivate : 0) | (s.img ? KImage : 0) | (s.map ? KMapped : 0);
		c.threads = s.threads;
		c.maxBytes = UI_BYTES;
		c.maxMs = UI_MS;
		c.maxHits = UI_HITS;
		c.maxText = UI_TEXT;
		if (s.mode == MBytes) {
			if (!parsePat(s.pat,c.pat,s.why)) return;
			if (c.pat.size() > 256) {
				s.why = "pattern is " + std::to_string(c.pat.size()) + " bytes, the limit is 256";
				return;
			}
		} else if (s.mode == MRegex) {
			auto rx = std::make_shared<Rx>();
			if (!s.rx[0]) s.why = "type a regex first";
			else if (!rx->compile(s.rx,!s.rxCase,s.why)) s.why = "regex : " + s.why;
			if (!s.why.empty()) return;
			c.rx = rx;
		}
		if (!c.kinds) {
			s.why = "Pick at least one of Private, Image, Mapped in Options";
			return;
		}
		s.scanMode = s.mode;
		s.res.rows = std::make_shared<Rows>();
		s.res.regs.reset();
		s.res.sel.clear();
		s.res.sortT.reset();
		s.res.sorted.reset();
		refilter(s.res);
		s.scanned = true;
		s.lastPull = 0;
		auto scan = std::make_shared<Scan>();
		s.scan = scan;
		ProcRef r = s.r;
		s.t = task::run([scan,r,c](task::T& t) -> std::string {
			run(r,c,*scan,[&t]() -> const char* {
				if (t.cancel.load()) return "cancelled";
				if (sampler::stopping()) return "FRS is closing";
				return nullptr;
			});
			std::string lb = (c.mode == MStrings ? "Search strings" : "Search memory") + (" (pid " + std::to_string(r.pid) + ")");
			std::lock_guard<std::mutex> lk(scan->m);
			if (!scan->err.empty()) return lb + " : failed (" + util::safe(scan->err) + ")";
			return lb + (scan->complete ? " : OK" : " : stopped (" + util::safe(scan->why) + ")");
		});
	}

	void request(St& s) {
		s.why.clear();
		if (s.t && !s.t->done) {
			s.t->cancel = true;
			s.restart = true;
			return;
		}
		startScan(s);
	}

	void pull(St& s) {
		if (!s.scan || (s.res.filtT && !s.res.filtT->done)) return;
		bool fin = s.scan->done.load();
		size_t n = s.res.rows->size();
		uint64_t now = util::nowMs();
		uint64_t gap = n >= 500000 ? 400 : n >= 150000 ? 250 : 200;
		if (!fin && now - s.lastPull < gap) return;
		s.lastPull = now;
		std::vector<Hit> got;
		std::shared_ptr<const Regs> regs;
		{
			std::lock_guard<std::mutex> lk(s.scan->m);
			got.swap(s.scan->pending);
			regs = s.scan->regs;
		}
		if (regs && !s.res.regs) s.res.regs = regs;
		if (got.empty()) return;
		Rows& rows = *s.res.rows;
		rows.reserve(rows.size() + got.size());
		for (auto& h : got) rows.push_back(std::move(h));
	}

	void drawOptions(St& s,bool alive) {
		if (ImGui::BeginPopup("##sopt")) {
			ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups,false);
			if (ImGui::MenuItem("ANSI",nullptr,&s.res.ansi)) {
				refilter(s.res);
				saveOpts(s);
			}
			if (ImGui::MenuItem("Unicode",nullptr,&s.res.uni)) {
				refilter(s.res);
				saveOpts(s);
			}
			if (ImGui::MenuItem("Extended character set",nullptr,&s.ext)) s.optDirty = true;
			ImGui::Separator();
			if (ImGui::MenuItem("Private",nullptr,&s.priv)) s.optDirty = true;
			if (ImGui::MenuItem("Image",nullptr,&s.img)) s.optDirty = true;
			if (ImGui::MenuItem("Mapped",nullptr,&s.map)) s.optDirty = true;
			ImGui::Separator();
			ImGui::TextDisabled("Minimum length");
			ImGui::SetNextItemWidth(ui::px(150.0f));
			if (ImGui::InputInt("##smin",&s.minLen)) {
				s.minLen = std::max(1,std::min(s.minLen,100000));
				s.optDirty = true;
			}
			ImGui::TextDisabled(s.threads ? "Thread count" : "Thread count (auto)");
			ImGui::SetNextItemWidth(ui::px(150.0f));
			if (ImGui::InputInt("##sthr",&s.threads)) s.threads = std::max(0,std::min(s.threads,64));
			if (ImGui::MenuItem("Zero pad addresses",nullptr,&s.res.zero)) saveOpts(s);
			ImGui::Separator();
			ImGui::PopItemFlag();
			if (ImGui::MenuItem("Refresh","F5",false,alive)) {
				s.optDirty = false;
				saveOpts(s);
				request(s);
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		} else if (s.optDirty) {
			s.optDirty = false;
			saveOpts(s);
			s.why.clear();
			if (s.scanned && alive) request(s);
		}
	}

	void drawStatus(St& s,bool busy,float w) {
		Scan* sc = s.scan.get();
		const Res& rs = s.res;
		if (s.res.sortT && !s.res.sortT->done) {
			ImGui::TextDisabled("Sorting...");
			ImGui::SameLine();
		}
		if (rs.filtT && !rs.filtT->done) {
			ImGui::TextDisabled("Filtering...");
			ImGui::SameLine();
		}
		if (!sc) {
			ImGui::TextDisabled("Nothing scanned yet");
			return;
		}
		uint64_t hits = sc->hitN.load();
		const char* word = s.scanMode == MStrings ? (hits == 1 ? "string" : "strings") : (hits == 1 ? "hit" : "hits");
		std::string cnt = util::count(hits) + " " + word;
		if (rs.view.size() != rs.rows->size()) cnt = util::count(rs.view.size()) + " of " + cnt;
		std::string tail = util::size(sc->scanned.load()) + " of " + util::size(sc->total.load());
		if (busy) {
			uint64_t tot = sc->total.load();
			float f = tot ? (float)((double)sc->scanned.load() / (double)tot) : 0.0f;
			std::string ov = "Searching... " + cnt + "  |  " + tail;
			ui::meter(f,ov.c_str(),ImVec2(w / ui::px(1.0f),0.0f),ui::c.accent);
			return;
		}
		std::string err,why;
		bool comp;
		{
			std::lock_guard<std::mutex> lk(sc->m);
			err = sc->err;
			why = sc->why;
			comp = sc->complete;
		}
		if (!err.empty()) {
			ImGui::TextColored(ui::c.bad,"Scan failed : %s",err.c_str());
			return;
		}
		ImGui::TextUnformatted(cnt.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("%s read",util::size(sc->scanned.load()).c_str());
		int bad = sc->regBad.load(),part = sc->regPartial.load();
		uint64_t un = sc->unread.load();
		if (bad || part || un) {
			ImGui::SameLine();
			ImGui::TextColored(ui::c.warn,"%d unreadable, %d partial regions, %s skipped",bad,part,util::size(un).c_str());
			ui::tip("Pages the OS would not hand over. A hit could be in there, so no hit is not proof it isn't in memory.");
		}
		if (!comp && !why.empty()) {
			ImGui::SameLine();
			ImGui::TextColored(ui::c.warn,"Stopped early : %s, %d of %d regions",why.c_str(),sc->regDone.load(),sc->regTotal.load());
		}
	}

	// rows there, but the filter has looked at all of them and hides every one
	bool noneShown(const Res& rs) {
		if (rs.rows->empty() || !rs.view.empty() || rs.done < rs.rows->size() || (rs.filtT && !rs.filtT->done)) return false;
		ui::empty(rs.filt[0] ? "No row matches the search." : "No row matches the ANSI and Unicode options.");
		return true;
	}

	void drawBody(St& s,Res& rs,bool busy) {
		if (noneShown(rs)) return;
		if (rs.rows->empty()) {
			const char* msg;
			if (busy) msg = s.scanMode == MStrings ? "Loading strings..." : "Searching...";
			else if (!s.scan) msg = "Press Scan to search this process's memory. Reading can fault paged out pages back into its working set.";
			else {
				std::lock_guard<std::mutex> lk(s.scan->m);
				msg = !s.scan->err.empty() ? "The scan could not run, see the line below." : s.scanMode != MStrings ? "No hits." : "There are no strings to display.";
			}
			ui::empty(msg);
			return;
		}
		drawTable(s,rs);
	}

	bool filterRow(Res& rs) {
		float w = ImGui::GetContentRegionAvail().x / ui::px(1.0f) - 205.0f;
		bool ch = ui::search("##sflt",rs.filt,sizeof(rs.filt),"Search strings",std::max(120.0f,w));
		ImGui::SameLine();
		bool r1 = ImGui::Checkbox("Filter is regex##sflt",&rs.regex);
		ImGui::SameLine();
		bool r2 = ImGui::Checkbox("Case##sflt",&rs.cased);
		ui::tip("Match case. Off ignores case for A to Z only");
		if (ch || r1 || r2) refilter(rs);
		return r1 || r2;
	}

	void tableKeys(Res& rs) {
		if (ImGui::GetIO().WantTextInput) return;
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
			for (uint32_t v : rs.view) rs.sel[v] = 1;
			rs.nSel = rs.view.size();
		}
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) copySel(rs,-1);
	}

	void drawWindows(St& s) {
		ImGuiWindowFlags fl = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;
		for (auto& w : s.wins) {
			ImGui::SetNextWindowSize(ImVec2(ui::px(720.0f),ui::px(460.0f)),ImGuiCond_Appearing);
			ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,ImVec2(0.5f,0.5f));
			if (ImGui::Begin(w.title.c_str(),&w.open,fl)) {
				Res& rs = *w.res;
				filterRow(rs);
				stepFilter(rs);
				stepSort(rs,false);
				if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) tableKeys(rs);
				ImGui::BeginChild("##swb",ImVec2(0,-ImGui::GetFrameHeightWithSpacing()));
				if (rs.rows->empty()) ui::empty("No rows.");
				else if (!noneShown(rs)) drawTable(s,rs);
				ImGui::EndChild();
				ImGui::TextDisabled("%s of %s %s, copied when this window opened",util::count(rs.view.size()).c_str(),util::count(rs.rows->size()).c_str(),rs.rows->size() == 1 ? "row" : "rows");
			}
			ImGui::End();
		}
		s.wins.erase(std::remove_if(s.wins.begin(),s.wins.end(),[](const Win& w) { return !w.open; }),s.wins.end());
	}

	void openFilterWin(St& s) {
		if (s.wins.size() >= (size_t)MAX_WINS) {
			ui::toast("Too many windows, close one first");
			return;
		}
		Win w;
		w.res.reset(new Res());
		Res& n = *w.res;
		const Res& o = s.res;
		n.regs = o.regs;
		n.ansi = o.ansi;
		n.uni = o.uni;
		n.zero = o.zero;
		// rows sit in # order, so this is the order the new table's # arrow says
		std::vector<uint32_t> ord(o.view);
		if (!o.inOrder) std::sort(ord.begin(),ord.end());
		n.rows->reserve(ord.size());
		for (uint32_t v : ord) n.rows->push_back((*o.rows)[v]);
		n.needSort = false;
		w.title = "Strings of " + util::safe(s.r.name) + " (" + std::to_string(s.r.pid) + "), filtered##sw" + std::to_string(s.r.pid) + "_" + std::to_string(++s.winN);
		s.wins.push_back(std::move(w));
	}

	void drawTab(const ProcRef& r) {
		std::lock_guard<std::mutex> lk(g_m);
		auto& up = g_st[std::make_pair(r.pid,r.start)];
		if (!up) {
			up.reset(new St());
			up->r = r;
			loadOpts(*up);
			refilter(up->res);
		}
		St& s = *up;
		s.goneAt = 0;
		bool alive = procs::alive(r);
		if (s.restart && (!s.t || s.t->done)) {
			s.restart = false;
			if (alive) startScan(s);
		}
		bool busy = s.t && !s.t->done;
		pull(s);
		stepFilter(s.res);
		stepSort(s.res,busy);

		static const char* const modes[] = {"Strings","Bytes","Regex"};
		const ImVec4 tints[3] = {ui::c.accent,ui::c.accent,ui::c.accent};
		float y0 = ImGui::GetCursorPosY();
		ImGui::SetCursorPosY(y0 + (ImGui::GetFrameHeight() - ImGui::GetFontSize()) * 0.5f);
		if (ui::segmented("##smode",modes,tints,3,s.mode)) s.why.clear();
		ImGui::SameLine();
		ImGui::SetCursorPosY(y0);
		ImGui::BeginDisabled(!busy && !alive);
		if (busy) {
			if (ImGui::Button("Stop##s")) s.t->cancel = true;
		} else if (ImGui::Button("Scan##s")) request(s);
		ImGui::EndDisabled();
		if (!busy) ui::tip("Scan, F5 does it too");
		ImGui::SameLine();
		ImGui::SetCursorPosY(y0);
		if (ImGui::Button("Options##s")) ImGui::OpenPopup("##sopt");
		drawOptions(s,alive);
		ImGui::SameLine();
		ImGui::SetCursorPosY(y0);
		// the region kinds stay in sight in every mode, Options sets them for all three
		std::string km;
		if (s.priv) km += "Private ";
		if (s.img) km += "Image ";
		if (s.map) km += "Mapped ";
		if (!km.empty()) km.pop_back();
		float kw = km.empty() ? 0.0f : ImGui::CalcTextSize(km.c_str()).x + ImGui::GetStyle().ItemSpacing.x;
		if (s.mode == MBytes) {
			ImGui::SetNextItemWidth(kw > 0.0f ? -kw : -FLT_MIN);
			if (ImGui::InputTextWithHint("##spat","Hex bytes, like 4D 5A 90 00. 256 bytes at most",s.pat,sizeof(s.pat))) s.why.clear();
		} else if (s.mode == MRegex) {
			ImGui::SetNextItemWidth(-ui::px(80.0f) - kw);
			if (ImGui::InputTextWithHint("##srx","Regex over raw memory, a match is 4 KB at most",s.rx,sizeof(s.rx))) s.why.clear();
			ImGui::SameLine();
			ImGui::SetCursorPosY(y0);
			ImGui::Checkbox("Case##srx",&s.rxCase);
		}
		if (s.mode != MStrings && kw > 0.0f) {
			ImGui::SameLine();
			ImGui::SetCursorPosY(y0);
			ImGui::TextDisabled("%s",km.c_str());
		} else if (s.mode == MStrings) {
			std::string sm = km;
			if (!sm.empty()) sm += " | ";
			sm += "length " + std::to_string(s.minLen) + "+";
			if (s.ext) sm += " | extended";
			ImGui::SetCursorPosY(y0);
			ImGui::TextDisabled("%s",sm.c_str());
		}
		ImGui::SetCursorPosY(y0 + ImGui::GetFrameHeightWithSpacing());
		if (filterRow(s.res)) saveOpts(s);
		bool focus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (focus && ImGui::IsKeyPressed(ImGuiKey_F5,false) && alive) request(s);
		if (focus) tableKeys(s.res);
		if (!s.why.empty()) ImGui::TextColored(ui::c.bad,"%s",s.why.c_str());
		if (!s.res.rxErr.empty()) ImGui::TextColored(ui::c.bad,"Regex : %s, showing everything",s.res.rxErr.c_str());
		if (s.mode == MStrings && r.pid == procs::be::selfPid()) ImGui::TextDisabled("This is FRS itself, the scan also sees its own results and buffers.");

		float foot = ImGui::GetFrameHeightWithSpacing();
		ImGui::BeginChild("##sbody",ImVec2(0,-foot));
		drawBody(s,s.res,busy);
		ImGui::EndChild();
		float bw = ImGui::CalcTextSize("Open in new window").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		float end = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw;
		ImGui::AlignTextToFramePadding();
		drawStatus(s,busy,end - ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x);
		ImGui::SameLine();
		if (end > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(end);
		ImGui::BeginDisabled(busy || s.res.view.empty());
		if (ImGui::Button("Open in new window##s")) openFilterWin(s);
		ImGui::EndDisabled();
		ui::tip("A copy of the rows shown now, with its own search");
		drawWindows(s);
	}

	// procwin has no close event and a tab that isn't selected isn't drawn, so each frame ask whether the window is still there.
	// No frames while minimized means no stamp, so only a window that really went away ever gets freed
	void frameCheck() {
		{
			std::lock_guard<std::mutex> lk(g_m);
			uint64_t now = util::nowMs();
			for (auto& kv : g_st) {
				St& s = *kv.second;
				if (procwin::isOpen(s.r)) {
					s.goneAt = 0;
					continue;
				}
				if (!s.goneAt) s.goneAt = now;
				if (!s.t || s.t->done) continue;
				s.t->cancel = true;
				s.restart = false;
			}
		}
		dumpWins();
	}

}

namespace vmscan {

	void initStrings() {
		procwin::addTab("Strings",40,drawTab);
		procwin::addFloat(frameCheck);
		sampler::add("vmscan-free",30000,[] {
			std::vector<std::unique_ptr<St>> dead;
			{
				std::lock_guard<std::mutex> lk(g_m);
				uint64_t now = util::nowMs();
				for (auto it = g_st.begin(); it != g_st.end();) {
					St& s = *it->second;
					if (!s.goneAt || now - s.goneAt < GONE_MS) {
						++it;
						continue;
					}
					if (s.t && !s.t->done) {
						s.t->cancel = true;
						++it;
						continue;
					}
					if (s.res.sortT && !s.res.sortT->done) {
						++it;
						continue;
					}
					dead.push_back(std::move(it->second));
					it = g_st.erase(it);
				}
			}
		});
	}

}
