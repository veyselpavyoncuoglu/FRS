#define IMGUI_DEFINE_MATH_OPERATORS
#include "vmem.h"
#include "cfg.h"
#include "ui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cstdio>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>

namespace {

	using namespace vmem;

	const uint64_t BLK = 0x10000;
	const size_t MAXBLK = 64;
	const uint64_t MAXCOPY = 1 << 20;
	const uint64_t FINDCH = 1 << 20;

	struct Block {
		std::vector<uint8_t> d;
		uint8_t map[16];
		int used = 0;
	};

	// the read cache : a worker fills it, the UI copies what is on screen out of it and never waits
	struct Cache {
		std::mutex m;
		std::map<uint64_t,Block> blocks; // by address, aligned to BLK
		std::deque<uint64_t> want;
		std::set<uint64_t> queued;
		bool running = false,closed = false;
		uint64_t gen = 0;
		std::string err;
	};

	struct Edit {
		uint64_t va;
		bool had;
		uint8_t prev;
		int grp;
	};

	struct View {
		uint64_t from = 0;
		std::vector<uint8_t> d,st; // st : 0 still loading, 1 unreadable, 2 read only, 3 writable
	};

	struct FindOut {
		uint64_t hit = NA;
		uint64_t skipped = 0;
	};

	struct WriteOut {
		std::vector<uint64_t> done;
	};

	struct Win {
		ProcRef r;
		uint64_t base = 0,size = 0;
		bool isSec = false;
		std::string title,id;
		std::shared_ptr<Cache> c = std::make_shared<Cache>();
		bool open = true,focus = true,fresh = true;
		int64_t top = 0;
		int vis = 1;
		uint64_t caret = 0,anchor = 0;
		bool hasSel = false,paneAscii = false,wantCopy = false,caretShow = false;
		int nib = 0,bpr = 16,grp = 0;
		std::map<uint64_t,uint8_t> dirty; // by address, what Write would send
		std::vector<Edit> undo;
		std::string flash;
		uint64_t flashUntil = 0;
		bool findOpen = false,findFocus = false,findHexMode = true;
		char findQ[256] = "";
		std::shared_ptr<task::T> findTask;
		std::shared_ptr<FindOut> findRes;
		size_t findLen = 0;
		std::shared_ptr<task::T> writeTask;
		std::shared_ptr<WriteOut> writeRes;
		char gotoTxt[64] = "";
		std::string gotoErr;
		bool openGoto = false,openSave = false,askWrite = false,askReread = false,askClose = false,gotoFocus = false;
		char path[512] = "";
		std::shared_ptr<task::T> saveTask;
		std::map<uint64_t,uint8_t> saveDirty;
	};

	std::deque<Win> g_wins;

	struct Pend {
		ProcRef r;
		uint64_t base,size,selOff,selLen;
		bool isSec;
		std::string label;
	};
	std::mutex g_pm;
	std::vector<Pend> g_pend;

	// a closed window's save and write still run, their result is reported here and a section view goes once they are done
	struct Orphan {
		std::shared_ptr<task::T> save,write;
		uint32_t pid;
		uint64_t sec;
	};
	std::vector<Orphan> g_orph;

	void pump(const Win& w) {
		auto c = w.c;
		{
			std::lock_guard<std::mutex> lk(c->m);
			if (c->running || c->want.empty()) return;
			c->running = true;
		}
		ProcRef r = w.r;
		uint64_t base = w.base,size = w.size;
		auto t = task::run([c,r,base,size](task::T& k) -> std::string {
			struct Off {
				Cache* c;
				~Off() {
					std::lock_guard<std::mutex> lk(c->m);
					c->running = false;
				}
			} off{c.get()};
			for (;;) {
				uint64_t key,gen;
				{
					std::lock_guard<std::mutex> lk(c->m);
					if (c->closed || c->want.empty() || k.cancel) return "";
					key = c->want.front();
					c->want.pop_front();
					gen = c->gen;
				}
				Block b;
				b.d.assign(BLK,0);
				memset(b.map,0,sizeof(b.map));
				uint64_t lo = std::max(key,base),hi = std::min(key + BLK,base + size);
				std::string err;
				if (hi > lo) {
					uint8_t map[18] = {};
					be::read(r,lo,(size_t)(hi - lo),b.d.data() + (lo - key),map,err);
					uint64_t p0 = lo & ~(uint64_t)0xFFF;
					size_t idx = (size_t)((p0 - key) >> 12);
					size_t pages = (size_t)((hi + 0xFFF - p0) >> 12);
					for (size_t i = 0; i < pages && idx + i < 16; i++) b.map[idx + i] = map[i];
				}
				std::lock_guard<std::mutex> lk(c->m);
				c->queued.erase(key);
				if (gen == c->gen) {
					c->err = err;
					c->blocks[key] = std::move(b);
				}
			}
		},false);
		if (t->done && !t->res.empty()) {
			std::lock_guard<std::mutex> lk(c->m);
			c->running = false;
		}
	}

	void reread(Win& w) {
		std::lock_guard<std::mutex> lk(w.c->m);
		w.c->blocks.clear();
		w.c->want.clear();
		w.c->queued.clear();
		w.c->gen++;
		w.c->err.clear();
	}

	// copies [from,to) out of the cache and asks for what is missing. Edits are not in here, overlay() puts them on.
	void fetch(Win& w,uint64_t from,uint64_t to,View& v) {
		size_t n = (size_t)(to - from);
		v.from = from;
		v.d.assign(n,0);
		v.st.assign(n,0);
		int fr = ImGui::GetFrameCount();
		{
			std::lock_guard<std::mutex> lk(w.c->m);
			Cache& c = *w.c;
			for (uint64_t key = (w.base + from) & ~(BLK - 1); key < w.base + to; key += BLK) {
				auto it = c.blocks.find(key);
				if (it == c.blocks.end()) {
					if (c.queued.insert(key).second) c.want.push_back(key);
					continue;
				}
				Block& b = it->second;
				b.used = fr;
				uint64_t lo = std::max(key,w.base + from),hi = std::min(key + BLK,w.base + to);
				memcpy(v.d.data() + (lo - w.base - from),b.d.data() + (lo - key),(size_t)(hi - lo));
				for (uint64_t a = lo; a < hi; a++) {
					uint8_t m = b.map[(a - key) >> 12];
					v.st[(size_t)(a - w.base - from)] = m + 1;
				}
			}
			while (c.blocks.size() > MAXBLK) {
				auto low = c.blocks.begin();
				for (auto it = c.blocks.begin(); it != c.blocks.end(); ++it) if (it->second.used < low->second.used) low = it;
				if (low->second.used == fr) break;
				c.blocks.erase(low);
			}
		}
		pump(w);
	}

	void overlay(const Win& w,View& v) {
		uint64_t a = w.base + v.from;
		for (auto it = w.dirty.lower_bound(a); it != w.dirty.end() && it->first < a + v.d.size(); ++it) v.d[(size_t)(it->first - a)] = it->second;
	}

	void flash(Win& w,const std::string& s) {
		w.flash = s;
		w.flashUntil = util::nowMs() + 4000;
	}

	void showCaret(Win& w) {
		w.caretShow = true;
	}

	void moveTo(Win& w,uint64_t off,bool extend) {
		if (w.size && off >= w.size) off = w.size - 1;
		if (extend) {
			if (!w.hasSel) {
				w.anchor = w.caret;
				w.hasSel = true;
			}
		} else w.hasSel = false;
		w.caret = off;
		w.nib = 0;
		showCaret(w);
	}

	void selRange(const Win& w,uint64_t& lo,uint64_t& hi) {
		if (w.hasSel) {
			lo = std::min(w.anchor,w.caret);
			hi = std::max(w.anchor,w.caret) + 1;
		} else {
			lo = w.caret;
			hi = w.caret + 1;
		}
	}

	bool canWriteAt(Win& w,uint64_t off,bool say) {
		View v;
		fetch(w,off,off + 1,v);
		if (v.st[0] == 3) return true;
		if (say) {
			if (v.st[0] == 0) flash(w,"Still reading this part");
			else if (v.st[0] == 1) flash(w,"This page can't be read, so it can't be edited");
			else flash(w,"Read only : this page is not writable, change its protection first");
		}
		return false;
	}

	void setByte(Win& w,uint64_t off,uint8_t val) {
		uint64_t va = w.base + off;
		auto it = w.dirty.find(va);
		w.undo.push_back({va,it != w.dirty.end(),it != w.dirty.end() ? it->second : (uint8_t)0,w.grp});
		w.dirty[va] = val;
	}

	void undoLast(Win& w) {
		if (w.undo.empty()) return;
		int g = w.undo.back().grp;
		while (!w.undo.empty() && w.undo.back().grp == g) {
			Edit& e = w.undo.back();
			if (e.had) w.dirty[e.va] = e.prev;
			else w.dirty.erase(e.va);
			w.undo.pop_back();
		}
		w.nib = 0;
	}

	void typeHex(Win& w,int d) {
		if (!canWriteAt(w,w.caret,true)) return;
		View v;
		fetch(w,w.caret,w.caret + 1,v);
		overlay(w,v);
		uint8_t cur = v.d[0];
		w.grp++;
		w.hasSel = false;
		if (w.nib == 0) {
			setByte(w,w.caret,(uint8_t)((d << 4) | (cur & 0x0F)));
			w.nib = 1;
		} else {
			setByte(w,w.caret,(uint8_t)((cur & 0xF0) | d));
			w.nib = 0;
			if (w.caret + 1 < w.size) w.caret++;
			showCaret(w);
		}
	}

	void typeAscii(Win& w,unsigned ch) {
		if (!canWriteAt(w,w.caret,true)) return;
		w.grp++;
		w.hasSel = false;
		setByte(w,w.caret,(uint8_t)ch);
		if (w.caret + 1 < w.size) w.caret++;
		showCaret(w);
	}

	void doCopy(Win& w) {
		uint64_t lo,hi;
		selRange(w,lo,hi);
		if (hi - lo > MAXCOPY) {
			flash(w,"That is more than 1 MB, Save writes any size to a file");
			w.wantCopy = false;
			return;
		}
		View v;
		fetch(w,lo,hi,v);
		overlay(w,v);
		for (auto s : v.st) if (!s) {
			w.wantCopy = true; // the worker is on it, finish on a later frame
			return;
		}
		w.wantCopy = false;
		std::string s;
		s.reserve(v.d.size() * 3);
		static const char* h = "0123456789abcdef";
		for (size_t i = 0; i < v.d.size(); i++) {
			bool ok = v.st[i] != 1;
			if (w.paneAscii) {
				s += !ok ? '?' : v.d[i] >= 0x20 && v.d[i] < 0x7F ? (char)v.d[i] : '.';
			} else {
				s += ok ? h[v.d[i] >> 4] : '?';
				s += ok ? h[v.d[i] & 15] : '?';
				s += ' ';
			}
		}
		ImGui::SetClipboardText(s.c_str());
		flash(w,"Copied " + std::to_string(v.d.size()) + " bytes");
	}

	void doPaste(Win& w) {
		const char* cb = ImGui::GetClipboardText();
		if (!cb || !*cb) return;
		std::vector<uint8_t> data;
		bool hexOk = !w.paneAscii;
		if (hexOk) {
			int hi = -1;
			for (const char* c = cb; *c; c++) {
				if (*c == ' ' || *c == '\n' || *c == '\r' || *c == '\t') continue;
				int d = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;
				if (d < 0) {
					hexOk = false;
					break;
				}
				if (hi < 0) hi = d;
				else {
					data.push_back((uint8_t)(hi * 16 + d));
					hi = -1;
				}
			}
			if (hi >= 0) hexOk = false;
		}
		if (!hexOk) {
			data.clear();
			for (const char* c = cb; *c; c++) data.push_back((uint8_t)*c);
		}
		uint64_t lo,hi;
		selRange(w,lo,hi);
		if (data.size() > MAXCOPY) {
			flash(w,"That is more than 1 MB, not pasted");
			return;
		}
		size_t n = (size_t)std::min((uint64_t)data.size(),w.size - lo);
		View v;
		fetch(w,lo,lo + n,v);
		w.grp++;
		size_t put = 0;
		for (size_t i = 0; i < n; i++) {
			if (v.st[i] == 1 || v.st[i] == 2) continue; // a part still loading goes in, Write reports it if the page refuses
			setByte(w,lo + i,data[i]);
			put++;
		}
		flash(w,"Pasted " + std::to_string(put) + " bytes, nothing is written until Write" + (put < data.size() ? ", the rest fell on read only pages or past the end" : ""));
		moveTo(w,std::min(lo + n,w.size - 1),false);
	}

	std::string findWork(const ProcRef& r,uint64_t base,uint64_t size,uint64_t start,const std::vector<uint8_t>& pat,task::T& t,FindOut& out) {
		const size_t CH = (size_t)FINDCH;
		std::vector<uint8_t> buf(CH + pat.size());
		std::vector<uint8_t> map(((CH + pat.size()) >> 12) + 3);
		uint64_t total = size ? size : 1,done = 0;
		for (int pass = 0; pass < 2; pass++) {
			uint64_t from = pass == 0 ? start : 0;
			uint64_t to = pass == 0 ? size : std::min(size,start + pat.size());
			for (uint64_t off = from; off + pat.size() <= to;) {
				if (t.cancel) return "stop";
				size_t n = (size_t)std::min(to - off,(uint64_t)CH + pat.size() - 1);
				std::string e;
				size_t got = be::read(r,base + off,n,buf.data(),map.data(),e);
				if (!e.empty()) return e;
				out.skipped += n - got;
				uint64_t p0 = (base + off) & ~(uint64_t)0xFFF;
				auto sb = buf.begin(),se = buf.begin() + (ptrdiff_t)n;
				for (auto it = std::search(sb,se,pat.begin(),pat.end()); it != se; it = std::search(it + 1,se,pat.begin(),pat.end())) {
					uint64_t a = base + off + (uint64_t)(it - sb);
					bool okp = true;
					for (uint64_t q = a & ~(uint64_t)0xFFF; q < a + pat.size(); q += 0x1000) if (!map[(size_t)((q - p0) >> 12)]) okp = false;
					if (okp) {
						out.hit = a - base;
						return "hit";
					}
				}
				if (off + n >= to) break;
				off += n - (pat.size() - 1);
				done += n - (pat.size() - 1);
				t.prog = (float)((double)done / (double)total);
			}
		}
		return "none";
	}

	void startFind(Win& w) {
		if (w.findTask || !w.findQ[0]) return;
		std::vector<uint8_t> pat;
		if (w.findHexMode) {
			int hi = -1;
			for (const char* c = w.findQ; *c; c++) {
				if (*c == ' ') continue;
				int d = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;
				if (d < 0) {
					flash(w,"That is not hex, pick Text to look for characters");
					return;
				}
				if (hi < 0) hi = d;
				else {
					pat.push_back((uint8_t)(hi * 16 + d));
					hi = -1;
				}
			}
			if (hi >= 0) {
				flash(w,"Odd number of hex digits");
				return;
			}
		} else for (const char* c = w.findQ; *c; c++) pat.push_back((uint8_t)*c);
		if (pat.empty()) return;
		uint64_t lo,hi;
		selRange(w,lo,hi);
		uint64_t start = w.hasSel ? lo + 1 : lo; // a held selection is the last hit, the bare caret byte still counts
		if (start >= w.size) start = 0;
		auto res = std::make_shared<FindOut>();
		w.findRes = res;
		w.findLen = pat.size();
		ProcRef r = w.r;
		uint64_t base = w.base,size = w.size;
		w.findTask = task::run([r,base,size,start,pat,res](task::T& t) -> std::string {
			return findWork(r,base,size,start,pat,t,*res);
		},false);
	}

	void pollFind(Win& w) {
		if (!w.findTask || !w.findTask->done) return;
		std::string s = w.findTask->res;
		w.findTask.reset();
		if (s == "hit") {
			w.anchor = w.findRes->hit;
			w.caret = w.findRes->hit + (w.findLen ? w.findLen - 1 : 0);
			w.hasSel = true;
			w.nib = 0;
			showCaret(w);
			flash(w,"Found at offset " + util::hex(w.findRes->hit));
		} else if (s == "none") {
			std::string m = "Not found";
			if (w.findRes->skipped) m += ", " + util::size(w.findRes->skipped) + " could not be read";
			flash(w,m);
		} else if (s != "stop") flash(w,util::safe(s));
	}

	void startWrite(Win& w) {
		if (w.writeTask || w.dirty.empty()) return;
		if (!w.isSec && w.r.pid == procs::be::selfPid()) {
			flash(w,"Refused, that is FRS itself");
			return;
		}
		auto d = w.dirty;
		auto res = std::make_shared<WriteOut>();
		w.writeRes = res;
		ProcRef r = w.r;
		bool sec = w.isSec;
		w.writeTask = task::run([r,d,res,sec](task::T&) -> std::string {
			std::string where = (sec ? std::string("Write section (") : "Write memory (pid " + std::to_string(r.pid) + ", ") + util::hex(d.begin()->first) + ")";
			std::string why;
			size_t ok = 0,bad = 0;
			for (auto it = d.begin(); it != d.end();) {
				uint64_t a = it->first;
				std::vector<uint8_t> b;
				while (it != d.end() && it->first == a + b.size()) {
					b.push_back(it->second);
					++it;
				}
				std::string e;
				size_t k = be::write(r,a,b.data(),b.size(),e);
				for (size_t i = 0; i < k; i++) res->done.push_back(a + i);
				ok += k;
				if (k < b.size()) {
					bad += b.size() - k;
					why = e;
				}
			}
			std::string s = where + (bad ? " : failed (" + std::to_string(bad) + " bytes, " + util::safe(why) + ")" : " : OK (" + std::to_string(ok) + " bytes)");
			ui::toast(s);
			if (ok && !sec) changed(r);
			return s;
		});
	}

	void pollTasks(Win& w) {
		if (w.writeTask && w.writeTask->done) {
			for (uint64_t a : w.writeRes->done) w.dirty.erase(a);
			w.undo.clear();
			w.writeTask.reset();
			reread(w);
		}
		pollFind(w);
		std::string askId = "Overwrite file ?##hexover" + w.id;
		if (w.saveTask && w.saveTask->done) {
			std::string s = w.saveTask->res;
			w.saveTask.reset();
			if (s == "EXISTS") ui::askOpen(askId.c_str());
			else {
				appLog(s);
				ui::toast(s);
			}
		}
		if (ui::askDraw(askId.c_str(),"That file exists. Replace it?","Overwrite",true)) {
			auto d = w.saveDirty;
			ProcRef r = w.r;
			std::string path = w.path;
			uint64_t base = w.base,size = w.size;
			w.saveTask = task::run([r,d,path,base,size](task::T& t) -> std::string {
				return saveRanges(r,{{base,size}},path,true,t,&d);
			},false);
		}
	}

	void closeWin(Win& w) {
		w.open = false;
	}

	void keys(Win& w) {
		ImGuiIO& io = ImGui::GetIO();
		bool sh = io.KeyShift,ct = io.KeyCtrl;
		uint64_t last = w.size ? w.size - 1 : 0;
		uint64_t row = (uint64_t)w.bpr;
		auto press = [](ImGuiKey k) { return ImGui::IsKeyPressed(k,true); };
		if (press(ImGuiKey_LeftArrow)) moveTo(w,w.caret ? w.caret - 1 : 0,sh);
		if (press(ImGuiKey_RightArrow)) moveTo(w,w.caret + 1,sh);
		if (press(ImGuiKey_UpArrow)) moveTo(w,w.caret >= row ? w.caret - row : w.caret,sh);
		if (press(ImGuiKey_DownArrow)) moveTo(w,w.caret + row <= last ? w.caret + row : w.caret,sh);
		uint64_t page = row * (uint64_t)std::max(1,w.vis - 1);
		if (press(ImGuiKey_PageUp)) moveTo(w,w.caret >= page ? w.caret - page : w.caret % row,sh);
		if (press(ImGuiKey_PageDown)) moveTo(w,w.caret + page <= last ? w.caret + page : last,sh);
		if (press(ImGuiKey_Home)) moveTo(w,ct ? 0 : w.caret - w.caret % row,sh);
		if (press(ImGuiKey_End)) moveTo(w,ct ? last : std::min(w.caret - w.caret % row + row - 1,last),sh);
		if (ImGui::IsKeyPressed(ImGuiKey_Tab,false)) {
			w.paneAscii = !w.paneAscii;
			w.nib = 0;
		}
		if (press(ImGuiKey_Backspace)) moveTo(w,w.caret ? w.caret - 1 : 0,false);
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) doCopy(w);
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) doPaste(w);
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undoLast(w);
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G)) w.openGoto = true;
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) {
			w.findOpen = true;
			w.findFocus = true;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_F3,false)) startFind(w);
		if (ImGui::IsKeyPressed(ImGuiKey_F5,false)) {
			if (w.dirty.empty()) reread(w);
			else w.askReread = true;
		}
		if (ct || io.KeyAlt) return;
		for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
			unsigned ch = io.InputQueueCharacters[i];
			if (ch < 0x20 || ch >= 0x7F) continue;
			if (w.paneAscii) typeAscii(w,ch);
			else {
				int d = ch >= '0' && ch <= '9' ? (int)ch - '0' : ch >= 'a' && ch <= 'f' ? (int)ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? (int)ch - 'A' + 10 : -1;
				if (d >= 0) typeHex(w,d);
			}
		}
	}

	void drawBody(Win& w,float bodyH) {
		ui::pushMono();
		float cw = ImGui::CalcTextSize("0").x;
		float lineH = ImGui::GetTextLineHeight() + ui::px(2.0f);
		int digits = w.size > 0xFFFFFFFFULL ? 12 : 8;
		float xHex = cw * (float)(digits + 2);
		auto hexX = [&](int i) { return xHex + (float)i * cw * 3.0f + (float)(i / 8) * cw; };
		float xAsc = hexX(w.bpr) + cw;
		float totalW = xAsc + (float)w.bpr * cw + cw;
		ui::popFont();

		float sbW = ImGui::GetStyle().ScrollbarSize;
		ImGui::SetNextWindowContentSize(ImVec2(totalW,0.0f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg,ui::c.panel);
		ImGui::BeginChild("##hexbody",ImVec2(-sbW,bodyH),ImGuiChildFlags_None,ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
		ImGui::PopStyleColor();
		ui::pushMono();
		ImGuiIO& io = ImGui::GetIO();
		ImVec2 org = ImGui::GetCursorScreenPos();
		float availH = ImGui::GetContentRegionAvail().y;
		float headH = lineH + ui::px(2.0f);
		int vis = std::max(1,(int)((availH - headH) / lineH));
		w.vis = vis;
		int64_t rows = (int64_t)((w.size + (uint64_t)w.bpr - 1) / (uint64_t)w.bpr);
		if (w.caretShow) {
			w.caretShow = false;
			int64_t cr = (int64_t)(w.caret / (uint64_t)w.bpr);
			if (cr < w.top) w.top = cr;
			else if (cr >= w.top + vis) w.top = cr - vis + 1;
			int cc = (int)(w.caret % (uint64_t)w.bpr);
			float cx = w.paneAscii ? xAsc + (float)cc * cw : hexX(cc);
			float sx = ImGui::GetScrollX();
			float vw = ImGui::GetWindowWidth();
			if (cx < sx) ImGui::SetScrollX(std::max(0.0f,cx - cw));
			else if (cx + cw * 3.0f > sx + vw) ImGui::SetScrollX(cx + cw * 3.0f - vw);
		}
		if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f) w.top -= (int64_t)(io.MouseWheel * 3.0f);
		int64_t maxTop = std::max((int64_t)0,rows - vis);
		w.top = std::min(std::max(w.top,(int64_t)0),maxTop);

		ImGui::InvisibleButton("##hexhit",ImVec2(totalW,std::max(1.0f,availH)));
		bool active = ImGui::IsItemActive();
		bool down = ImGui::IsItemActivated();
		auto hitTest = [&](uint64_t& off,int& nib,bool& asc) {
			ImVec2 m = io.MousePos;
			int64_t rw = w.top + (int64_t)std::floor((m.y - org.y - headH) / lineH);
			rw = std::min(std::max(rw,(int64_t)0),std::max((int64_t)0,rows - 1));
			float x = m.x - org.x;
			asc = x >= xAsc - cw * 0.5f;
			int col;
			nib = 0;
			if (asc) col = (int)((x - xAsc) / cw);
			else {
				col = 0;
				for (int i = 0; i < w.bpr; i++) if (x >= hexX(i) - cw * 0.5f) col = i;
				nib = x - hexX(col) >= cw ? 1 : 0;
			}
			col = std::min(std::max(col,0),w.bpr - 1);
			off = (uint64_t)rw * (uint64_t)w.bpr + (uint64_t)col;
			if (off >= w.size) off = w.size ? w.size - 1 : 0;
		};
		if (down) {
			uint64_t off;
			int nb;
			bool asc;
			hitTest(off,nb,asc);
			moveTo(w,off,io.KeyShift);
			w.paneAscii = asc;
			w.nib = asc ? 0 : nb;
		} else if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
			uint64_t off;
			int nb;
			bool asc;
			hitTest(off,nb,asc);
			if (off != w.caret) moveTo(w,off,true);
			if (io.MousePos.y < org.y + headH) w.top--;
			else if (io.MousePos.y > org.y + availH) w.top++;
			w.top = std::min(std::max(w.top,(int64_t)0),maxTop);
		}

		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImU32 cDim = ImGui::GetColorU32(ui::c.dim);
		ImU32 cText = ImGui::GetColorU32(ui::c.text);
		ImU32 cWarn = ImGui::GetColorU32(ui::c.warn);
		ImU32 cAcc = ImGui::GetColorU32(ui::c.accent);
		ImU32 cBg = ImGui::GetColorU32(ui::c.bg);
		ImVec4 sv = ui::c.accent;
		sv.w = 0.28f;
		ImU32 cSel = ImGui::GetColorU32(sv);
		sv.w = 0.5f;
		ImU32 cCaret2 = ImGui::GetColorU32(sv);

		char tx[32];
		for (int i = 0; i < w.bpr; i++) {
			snprintf(tx,sizeof(tx),"%02X",i);
			dl->AddText(ImVec2(org.x + hexX(i),org.y),cDim,tx);
		}
		dl->AddText(ImVec2(org.x,org.y),cDim,"Offset");
		dl->AddLine(ImVec2(org.x,org.y + lineH),ImVec2(org.x + totalW,org.y + lineH),ImGui::GetColorU32(ui::c.line));

		uint64_t off0 = (uint64_t)w.top * (uint64_t)w.bpr;
		uint64_t off1 = std::min(w.size,off0 + (uint64_t)(vis + 1) * (uint64_t)w.bpr);
		View v;
		if (off1 > off0) {
			fetch(w,off0,off1,v);
			overlay(w,v);
		}
		uint64_t sLo = 0,sHi = 0;
		if (w.hasSel) selRange(w,sLo,sHi);
		bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		for (int r = 0; r <= vis; r++) {
			uint64_t ro = off0 + (uint64_t)r * (uint64_t)w.bpr;
			if (ro >= off1) break;
			float y = org.y + headH + (float)r * lineH;
			if (digits == 12) snprintf(tx,sizeof(tx),"%012llx",(unsigned long long)ro);
			else snprintf(tx,sizeof(tx),"%08llx",(unsigned long long)ro);
			dl->AddText(ImVec2(org.x,y),cDim,tx);
			for (int i = 0; i < w.bpr; i++) {
				uint64_t o = ro + (uint64_t)i;
				if (o >= off1) break;
				size_t k = (size_t)(o - off0);
				uint8_t b = v.d[k];
				uint8_t st = v.st[k];
				bool dirty = w.dirty.count(w.base + o) != 0;
				float hx = org.x + hexX(i);
				float ax = org.x + xAsc + (float)i * cw;
				if (w.hasSel && o >= sLo && o < sHi) {
					dl->AddRectFilled(ImVec2(hx,y),ImVec2(hx + cw * 2.0f + (i + 1 < w.bpr ? cw : 0.0f),y + lineH),cSel);
					dl->AddRectFilled(ImVec2(ax,y),ImVec2(ax + cw,y + lineH),cSel);
				}
				bool isCaret = o == w.caret;
				if (isCaret) {
					float cx = hx + (w.paneAscii ? 0.0f : (float)w.nib * cw);
					float cwd = w.paneAscii ? cw * 2.0f : cw;
					dl->AddRectFilled(ImVec2(cx,y),ImVec2(cx + cwd,y + lineH),w.paneAscii ? cCaret2 : focused ? cAcc : cCaret2);
					dl->AddRectFilled(ImVec2(ax,y),ImVec2(ax + cw,y + lineH),w.paneAscii ? (focused ? cAcc : cCaret2) : cCaret2);
				}
				ImU32 col = dirty ? cWarn : st <= 1 ? cDim : b == 0 ? cDim : cText;
				const char* txt = st == 0 ? ".." : st == 1 ? "??" : nullptr;
				if (txt) {
					dl->AddText(ImVec2(hx,y),col,txt);
				} else {
					static const char* hd = "0123456789ABCDEF";
					char two[3] = {hd[b >> 4],hd[b & 15],0};
					if (isCaret && !w.paneAscii && focused) {
						char c1[2] = {two[0],0};
						char c2[2] = {two[1],0};
						dl->AddText(ImVec2(hx,y),w.nib == 0 ? cBg : col,c1);
						dl->AddText(ImVec2(hx + cw,y),w.nib == 1 ? cBg : col,c2);
					} else dl->AddText(ImVec2(hx,y),col,two);
				}
				char a[2] = {st == 0 ? '.' : st == 1 ? '?' : b >= 0x20 && b < 0x7F ? (char)b : '.',0};
				bool printable = st > 1 && b >= 0x20 && b < 0x7F;
				dl->AddText(ImVec2(ax,y),isCaret && w.paneAscii && focused ? cBg : dirty ? cWarn : printable ? cText : cDim,a);
			}
		}
		ui::popFont();
		ImGui::EndChild();
		ImGui::SameLine(0.0f,0.0f);
		ImVec2 sp = ImGui::GetCursorScreenPos();
		ImS64 tp = (ImS64)w.top;
		ImRect bb(sp,ImVec2(sp.x + sbW,sp.y + bodyH));
		ImGui::ScrollbarEx(bb,ImGui::GetID("##hexsb"),ImGuiAxis_Y,&tp,(ImS64)vis,std::max((ImS64)rows,(ImS64)1));
		w.top = (int64_t)tp;
		ImGui::ItemSize(ImVec2(sbW,bodyH));
	}

	void openSaveDlg(Win& w) {
		std::string tail = w.isSec ? std::string("section.bin") : util::hex(w.base) + "-" + util::hex(w.base + w.size) + ".bin";
		snprintf(w.path,sizeof(w.path),"%s",homePath(fileName(w.r,tail)).c_str());
		w.openSave = true;
	}

	void drawDialogs(Win& w) {
		std::string writeId = "Write process memory ?##hexwr" + w.id;
		std::string rereadId = "Re-read ?##hexrr" + w.id;
		std::string closeId = "Close editor ?##hexcl" + w.id;
		std::string gotoId = "Go to##hexgo" + w.id;
		std::string saveId = "Save##hexsv" + w.id;
		if (w.askWrite) {
			w.askWrite = false;
			ui::askOpen(writeId.c_str());
		}
		if (w.askReread) {
			w.askReread = false;
			ui::askOpen(rereadId.c_str());
		}
		if (w.askClose) {
			w.askClose = false;
			ui::askOpen(closeId.c_str());
		}
		std::string q = "Do you want to write " + std::to_string(w.dirty.size()) + (w.dirty.size() == 1 ? " changed byte" : " changed bytes") + " to process memory? Only what changed is sent, not the whole region.\n\nSome programs may restrict access or ban your account when editing the memory of the process.\n\nAre you sure you want to continue?";
		if (ui::askDraw(writeId.c_str(),q.c_str(),"Write",true)) startWrite(w);
		q = "This drops " + std::to_string(w.dirty.size()) + (w.dirty.size() == 1 ? " changed byte that was not written." : " changed bytes that were not written.");
		if (ui::askDraw(rereadId.c_str(),q.c_str(),"Re-read",true)) {
			w.dirty.clear();
			w.undo.clear();
			reread(w);
		}
		if (ui::askDraw(closeId.c_str(),q.c_str(),"Close",true)) closeWin(w);

		if (w.openGoto) {
			w.openGoto = false;
			w.gotoErr.clear();
			ImGui::OpenPopup(gotoId.c_str());
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,ImVec2(0.5f,0.5f));
		if (ImGui::BeginPopupModal(gotoId.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::TextDisabled("Offset in this region, or an address inside it. Hex needs 0x.");
			if (ImGui::IsWindowAppearing() || w.gotoFocus) ImGui::SetKeyboardFocusHere();
			w.gotoFocus = false;
			ImGui::SetNextItemWidth(ui::px(260.0f));
			bool go = ImGui::InputText("##hexgoto",w.gotoTxt,sizeof(w.gotoTxt),ImGuiInputTextFlags_EnterReturnsTrue);
			if (!w.gotoErr.empty()) ImGui::TextColored(ui::c.bad,"%s",w.gotoErr.c_str());
			if (ImGui::Button("Go",ImVec2(ui::px(110.0f),0))) go = true;
			if (go) {
				uint64_t v = 0;
				if (!parseNum(w.gotoTxt,v)) {
					w.gotoErr = "That is not a number.";
					w.gotoFocus = true;
				}
				else {
					if (v >= w.base && v - w.base < w.size) v -= w.base;
					if (v >= w.size) {
						w.gotoErr = "That is outside this region.";
						w.gotoFocus = true;
					}
					else {
						moveTo(w,v,false);
						ImGui::CloseCurrentPopup();
					}
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel",ImVec2(ui::px(110.0f),0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (w.openSave) {
			w.openSave = false;
			ImGui::OpenPopup(saveId.c_str());
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,ImVec2(0.5f,0.5f));
		if (ImGui::BeginPopupModal(saveId.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::TextDisabled("Saves the region with your unwritten edits. Unreadable pages become zeros.");
			ImGui::SetNextItemWidth(ui::px(460.0f));
			ImGui::InputText("##hexpath",w.path,sizeof(w.path));
			ImGui::BeginDisabled(w.saveTask != nullptr || !w.path[0]);
			if (ImGui::Button("Save",ImVec2(ui::px(110.0f),0))) {
				w.saveDirty = w.dirty;
				auto d = w.saveDirty;
				ProcRef r = w.r;
				std::string path = w.path;
				uint64_t base = w.base,size = w.size;
				w.saveTask = task::run([r,d,path,base,size](task::T& t) -> std::string {
					return saveRanges(r,{{base,size}},path,false,t,&d);
				},false);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel",ImVec2(ui::px(110.0f),0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	void drawWin(Win& w) {
		ImGuiIO& io = ImGui::GetIO();
		if (w.fresh) {
			w.fresh = false;
			ImVec2 sz(std::min(ui::px(760.0f),io.DisplaySize.x * 0.95f),std::min(ui::px(520.0f),io.DisplaySize.y * 0.95f));
			float off = ui::px(26.0f) * (float)(g_wins.size() % 6);
			ImGui::SetNextWindowSize(sz,ImGuiCond_Always);
			ImGui::SetNextWindowPos(ImVec2(std::max(0.0f,(io.DisplaySize.x - sz.x) * 0.5f) + off,std::max(0.0f,(io.DisplaySize.y - sz.y) * 0.5f) + off),ImGuiCond_Always);
		}
		ImGui::SetNextWindowSizeConstraints(ImVec2(ui::px(480.0f),ui::px(240.0f)),ImVec2(FLT_MAX,FLT_MAX));
		if (w.focus) {
			ImGui::SetNextWindowFocus();
			w.focus = false;
		}
		bool keep = true;
		bool shown = ImGui::Begin(w.title.c_str(),&keep,ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		if (!keep) {
			if (w.dirty.empty()) closeWin(w);
			else w.askClose = true;
		}
		if (shown) {
			pollTasks(w);
			bool busyW = w.writeTask != nullptr;
			if (ui::iconBtn("hexre",ui::IcoRefresh,"Read the bytes from the process again (F5).")) {
				if (w.dirty.empty()) reread(w);
				else w.askReread = true;
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(w.dirty.empty() || busyW);
			char wl[48];
			snprintf(wl,sizeof(wl),w.dirty.empty() ? "Write###hexw" : "Write (%d)###hexw",(int)w.dirty.size());
			ui::pushBtnColor(ui::c.warn);
			if (ImGui::Button(wl)) w.askWrite = true;
			ui::popBtnColor();
			ImGui::EndDisabled();
			ui::tip("Sends only the bytes you changed, shown in orange. Nothing reaches the process before this.");
			ImGui::SameLine();
			if (ImGui::Button("Go to...")) w.openGoto = true;
			ImGui::SameLine();
			if (ImGui::Button("Find...")) {
				w.findOpen = !w.findOpen;
				w.findFocus = w.findOpen;
			}
			ImGui::SameLine();
			if (ImGui::Button("Save...")) openSaveDlg(w);
			ImGui::SameLine();
			if (ImGui::Button("Close")) {
				if (w.dirty.empty()) closeWin(w);
				else w.askClose = true;
			}
			ImGui::SameLine();
			static const char* bprs[] = {"4","8","16","32","64","128","256"};
			int idx = 0;
			for (int i = 0; i < 7; i++) if (w.bpr == atoi(bprs[i])) idx = i;
			ImGui::SetNextItemWidth(ui::px(58.0f));
			if (ImGui::Combo("##hexbpr",&idx,bprs,7)) {
				w.bpr = atoi(bprs[idx]);
				cfg::seti("hexBytesPerRow",w.bpr);
				showCaret(w);
			}
			ui::tip("Bytes per row.");
			ImGui::SameLine();
			View cv;
			if (w.size) fetch(w,w.caret,w.caret + 1,cv);
			uint8_t cst = w.size ? cv.st[0] : 0;
			if (cst == 2) {
				ImGui::AlignTextToFramePadding();
				ui::badge("Read only",ui::c.warn);
				ui::tip("This page is not writable. Change its protection in the Regions tab to edit it.");
			} else if (cst == 1) {
				ImGui::AlignTextToFramePadding();
				ui::badge("Unreadable",ui::c.bad);
				ui::tip("This page is free, reserved, a guard page or no access. Guard pages are not touched on purpose, a read would use the guard up.");
			} else if (cst == 3) {
				ImGui::AlignTextToFramePadding();
				ui::badge("Writable",ui::c.good);
			} else ImGui::NewLine();

			std::string err;
			{
				std::lock_guard<std::mutex> lk(w.c->m);
				err = w.c->err;
			}
			if (!err.empty()) {
				ui::icon(ui::IcoWarn,ImGui::GetCursorScreenPos(),ImGui::GetFontSize(),ImGui::GetColorU32(ui::c.warn));
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetFontSize() + ui::px(6.0f));
				ImGui::TextColored(ui::c.warn,"%s",util::safe(err).c_str());
			} else if (!w.isSec && !procs::alive(w.r)) {
				ImGui::TextColored(ui::c.warn,"This process has exited. Showing what was last read.");
			}

			if (w.findOpen && ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)) w.findOpen = false;
			if (w.findOpen) {
				ImGui::SetNextItemWidth(ui::px(260.0f));
				if (w.findFocus) {
					ImGui::SetKeyboardFocusHere();
					w.findFocus = false;
				}
				bool go = ImGui::InputTextWithHint("##hexfind",w.findHexMode ? "Bytes, like 4D 5A" : "Text",w.findQ,sizeof(w.findQ),ImGuiInputTextFlags_EnterReturnsTrue);
				ImGui::SameLine();
				if (ImGui::RadioButton("Hex",w.findHexMode)) w.findHexMode = true;
				ImGui::SameLine();
				if (ImGui::RadioButton("Text",!w.findHexMode)) w.findHexMode = false;
				ImGui::SameLine();
				if (w.findTask) {
					if (ImGui::Button("Stop")) w.findTask->cancel = true;
					ImGui::SameLine();
					ImGui::TextDisabled("%d%%",(int)(w.findTask->prog * 100.0f));
				} else {
					if (ImGui::Button("Find next") || go) startFind(w);
					ui::tip("From the caret on, then around from the start (F3). Unreadable pages are skipped and counted.");
				}
			}

			float statusH = ImGui::GetFrameHeightWithSpacing();
			float bodyH = std::max(ui::px(80.0f),ImGui::GetContentRegionAvail().y - statusH);
			drawBody(w,bodyH);

			char st[160];
			uint64_t sl = 0,sh = 0;
			selRange(w,sl,sh);
			if (w.hasSel) snprintf(st,sizeof(st),"Offset 0x%llx   Address 0x%llx   Selected %llu bytes",(unsigned long long)w.caret,(unsigned long long)(w.base + w.caret),(unsigned long long)(sh - sl));
			else snprintf(st,sizeof(st),"Offset 0x%llx   Address 0x%llx",(unsigned long long)w.caret,(unsigned long long)(w.base + w.caret));
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s",st);
			if (!w.flash.empty() && util::nowMs() < w.flashUntil) {
				ImGui::SameLine(0.0f,ui::px(14.0f));
				ImGui::TextColored(ui::c.accent,"%s",w.flash.c_str());
			} else if (w.saveTask) {
				ImGui::SameLine(0.0f,ui::px(14.0f));
				ImGui::TextDisabled("Saving %d%%",(int)(w.saveTask->prog * 100.0f));
				ImGui::SameLine();
				if (ImGui::SmallButton("Stop##hexsv")) w.saveTask->cancel = true;
			}

			if (w.wantCopy) doCopy(w);
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)) keys(w);
			drawDialogs(w);
		}
		ImGui::End();
	}

	void addWin(const Pend& p) {
		for (auto& w : g_wins) if (w.r == p.r && w.base == p.base && w.size == p.size) {
			w.focus = true;
			if (p.selOff != NA) {
				w.anchor = p.selOff;
				w.caret = p.selOff + (p.selLen ? p.selLen - 1 : 0);
				w.hasSel = p.selLen > 0;
				w.paneAscii = p.selLen > 1;
				showCaret(w);
			}
			if (p.isSec) be::unmapSection(p.base);
			return;
		}
		if (g_wins.size() >= 16) {
			ui::toast("Too many hex editors, close one first");
			if (p.isSec) be::unmapSection(p.base);
			return;
		}
		g_wins.emplace_back();
		Win& w = g_wins.back();
		w.r = p.r;
		w.base = p.base;
		w.size = p.size;
		w.isSec = p.isSec;
		w.bpr = cfg::geti("hexBytesPerRow",16);
		if (w.bpr != 4 && w.bpr != 8 && w.bpr != 16 && w.bpr != 32 && w.bpr != 64 && w.bpr != 128 && w.bpr != 256) w.bpr = 16;
		static int seq = 0;
		w.id = std::to_string(++seq);
		std::string nm = util::safe(p.isSec ? p.label : p.r.name);
		std::replace(nm.begin(),nm.end(),'#','_');
		char t[96];
		// a section name is often a long path, so its size goes before it
		if (p.isSec) snprintf(t,sizeof(t),"Section (0x%llx bytes) - ",(unsigned long long)p.size);
		else snprintf(t,sizeof(t)," (%u) (0x%llx - 0x%llx)",p.r.pid,(unsigned long long)p.base,(unsigned long long)(p.base + p.size));
		w.title = (p.isSec ? t + nm : nm + t) + "##hex" + w.id;
		if (p.selOff != NA && p.selOff < p.size) {
			w.anchor = p.selOff;
			w.caret = std::min(p.selOff + (p.selLen ? p.selLen - 1 : 0),p.size - 1);
			w.hasSel = p.selLen > 0;
			w.paneAscii = p.selLen > 1; // a string hit shows in the ASCII pane
			showCaret(w);
		}
	}

}

namespace vmem {

	void hexOpen(const ProcRef& r,uint64_t base,uint64_t size,uint64_t selOff,uint64_t selLen) {
		if (!size) return;
		addWin({r,base,size,selOff,selLen,false,""});
	}

	void hexAt(const ProcRef& r,uint64_t addr,uint64_t len) {
		task::run([r,addr,len](task::T&) -> std::string {
			Region x;
			std::string err;
			if (!be::regionAt(r,addr,x,err) || x.state == StFree) {
				ui::toast("Unable to find the memory region for that address");
				return "";
			}
			std::lock_guard<std::mutex> lk(g_pm);
			g_pend.push_back({r,x.base,x.size,addr - x.base,len,false,""});
			return "";
		},false);
	}

	void hexSection(const ProcRef& owner,uint64_t handle,const std::string& name) {
		task::run([owner,handle,name](task::T&) -> std::string {
			uint64_t a = 0,sz = 0;
			bool rw = false;
			std::string err;
			if (!be::mapSection(owner,handle,a,sz,rw,err)) {
				ui::toast("Map section : failed (" + util::safe(err) + ")");
				return "";
			}
			std::lock_guard<std::mutex> lk(g_pm);
			g_pend.push_back({be::self(),a,sz,NA,0,true,name});
			return "";
		},false);
	}

	void hexDraw() {
		{
			std::vector<Pend> p;
			{
				std::lock_guard<std::mutex> lk(g_pm);
				p.swap(g_pend);
			}
			for (auto& x : p) addWin(x);
		}
		for (size_t i = 0; i < g_wins.size(); i++) drawWin(g_wins[i]);
		for (auto it = g_wins.begin(); it != g_wins.end();) {
			if (it->open) {
				++it;
				continue;
			}
			{
				std::lock_guard<std::mutex> lk(it->c->m);
				it->c->closed = true;
			}
			if (it->findTask) it->findTask->cancel = true;
			if (it->saveTask || it->writeTask) g_orph.push_back({it->saveTask,it->writeTask,it->r.pid,it->isSec ? it->base : 0});
			else if (it->isSec) be::unmapSection(it->base);
			it = g_wins.erase(it);
		}
		for (auto it = g_orph.begin(); it != g_orph.end();) {
			if ((it->save && !it->save->done) || (it->write && !it->write->done)) {
				++it;
				continue;
			}
			if (it->save) {
				std::string s = it->save->res == "EXISTS" ? "Save memory (pid " + std::to_string(it->pid) + ") : not saved, the file exists" : it->save->res;
				appLog(s);
				ui::toast(s);
			}
			if (it->sec) be::unmapSection(it->sec);
			it = g_orph.erase(it);
		}
	}

}
