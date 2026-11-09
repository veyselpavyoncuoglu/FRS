#include "vmem.h"
#include "cfg.h"
#include "domains.h"
#include "ui.h"
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <utility>

namespace {

	using namespace vmem;

	struct Slot {
		std::mutex m;
		std::shared_ptr<const RegionList> l;
		uint64_t lGen = 0; // gen when the walk of l started
		std::atomic<uint64_t> gen{0}; // changes FRS made to the memory
		std::atomic<bool> busy{false};
		bool again = false;
		std::atomic<uint64_t> used{0};
	};

	std::mutex g_m;
	std::map<std::pair<uint32_t,uint64_t>,std::shared_ptr<Slot>> g_slots;

	std::shared_ptr<Slot> slotOf(const ProcRef& r) {
		std::lock_guard<std::mutex> lk(g_m);
		auto& s = g_slots[{r.pid,r.start}];
		if (!s) s = std::make_shared<Slot>();
		s->used = util::nowMs();
		return s;
	}

	// a region with no use of its own says what its allocation is, that's why every part of a DLL shows the file
	void inherit(RegionList& l) {
		auto& v = l.rows;
		for (auto& x : v) {
			if (x.kind != KNone || x.state == StFree || x.base == x.allocBase) continue;
			auto it = std::lower_bound(v.begin(),v.end(),x.allocBase,[](const Region& a,uint64_t b) { return a.base < b; });
			// a PEB or TEB row is one split off part of its allocation, it says nothing about the rest
			if (it == v.end() || it->base != x.allocBase || it->kind == KNone || it->kind == KTeb || it->kind == KPeb || it->kind == KPeb32) continue;
			x.kind = it->kind;
			x.use = it->use;
			x.file = it->file;
			x.tid = it->tid;
			x.heapId = it->heapId;
			x.heapClass = it->heapClass;
		}
	}

	std::shared_ptr<const RegionList> runQuery(const ProcRef& r,const std::atomic<bool>* stop) {
		auto l = std::make_shared<RegionList>();
		l->ref = r;
		uint64_t t0 = util::nowMs();
		try {
			be::regions(r,*l,stop);
			inherit(*l);
		} catch (std::exception& e) {
			l->rows.clear();
			l->err = util::safe(e.what());
		}
		l->t = util::nowMs();
		l->ms = l->t - t0;
		return l;
	}

	void put(Slot& s,std::shared_ptr<const RegionList> l,uint64_t g) {
		std::lock_guard<std::mutex> lk(s.m);
		// a walk that started before a change never replaces one from after it
		if (s.l && g < s.lGen) return;
		s.l = std::move(l);
		s.lGen = g;
	}

}

const vmem::Region* vmem::RegionList::at(uint64_t addr) const {
	auto it = std::upper_bound(rows.begin(),rows.end(),addr,[](uint64_t a,const Region& b) { return a < b.base; });
	if (it == rows.begin()) return nullptr;
	--it;
	return addr - it->base < it->size ? &*it : nullptr;
}

namespace vmem {

	const char* stateName(State s) {
		return s == StCommit ? "Commit" : s == StReserve ? "Reserved" : "Free";
	}

	const char* typeName(Type t) {
		return t == TyPrivate ? "Private" : t == TyMapped ? "Mapped" : t == TyImage ? "Image" : "";
	}

	const char* kindToken(Kind k) {
		static const char* t[] = {
			nullptr,"unusable","mapped_file","user_shared_data","hypervisor_shared_data","peb","peb32","teb","stack","stack32","heap","heap32","heap_segment","heap_segment32",
			"cfg_bitmap","cfg_bitmap32","api_set_map","read_only_shared_memory","code_page_data","gdi_shared_handle_table","shim_data","activation_context_data",
			"wer_registration_data","silo_shared_data","telemetry_coverage","process_parameters","leap_second_data","desktop_heap","shadow_stack","other"
		};
		return t[k];
	}

	const char* prioName(int p) {
		static const char* t[] = {"Lowest","Very low","Low","Medium","Below normal","Normal","Above normal","High"};
		return p >= 0 && p < 8 ? t[p] : nullptr;
	}

	const char* signingName(int s) {
		static const char* t[] = {
			"Unchecked","Unsigned","Enterprise","Developer","Authenticode","Custom 2","StoreApp","Antimalware",
			"Microsoft","Custom 4","Custom 5","CodeGen","Windows","Custom 7","WinTcb","Custom 6"
		};
		return s >= 0 && s < 16 ? t[s] : nullptr;
	}

	std::shared_ptr<const RegionList> latest(const ProcRef& r) {
		auto s = slotOf(r);
		std::lock_guard<std::mutex> lk(s->m);
		return s->l;
	}

	bool busy(const ProcRef& r) {
		return slotOf(r)->busy;
	}

	void refresh(const ProcRef& r) {
		auto s = slotOf(r);
		{
			std::lock_guard<std::mutex> lk(s->m);
			if (s->busy) {
				s->again = true; // the running walk may have read memory from before a change
				return;
			}
			s->busy = true;
			s->again = false;
		}
		auto t = task::run([s,r](task::T& t) -> std::string {
			try {
				for (;;) {
					uint64_t g = s->gen;
					auto l = runQuery(r,&t.cancel);
					if (!t.cancel) put(*s,l,g);
					std::lock_guard<std::mutex> lk(s->m);
					if (t.cancel || !s->again) {
						s->busy = false;
						return "";
					}
					s->again = false;
				}
			} catch (...) {
				std::lock_guard<std::mutex> lk(s->m);
				s->busy = false;
				throw;
			}
		},false);
		if (t->done && !t->res.empty()) s->busy = false; // closing, or no thread to be had
	}

	std::shared_ptr<const RegionList> queryNow(const ProcRef& r,uint64_t maxAgeMs) {
		auto s = slotOf(r);
		uint64_t g = s->gen;
		{
			std::lock_guard<std::mutex> lk(s->m);
			auto& l = s->l;
			if (l && l->err.empty() && s->lGen == g && util::nowMs() - l->t <= maxAgeMs) return l;
		}
		auto l = runQuery(r,nullptr);
		put(*s,l,g);
		return l;
	}

	void changed(const ProcRef& r) {
		slotOf(r)->gen++;
		if (latest(r)) refresh(r);
	}

	void prune() {
		static uint64_t last = 0;
		uint64_t now = util::nowMs();
		if (now - last < 10000) return;
		last = now;
		std::lock_guard<std::mutex> lk(g_m);
		for (auto it = g_slots.begin(); it != g_slots.end();) {
			if (!it->second->busy && now - it->second->used > 120000) it = g_slots.erase(it);
			else ++it;
		}
	}

	std::string saveRanges(const ProcRef& r,const std::vector<Range>& rs,const std::string& path,bool over,task::T& t,const std::map<uint64_t,uint8_t>* dirty) {
		std::string lb = "Save memory (pid " + std::to_string(r.pid) + ")";
		const uint64_t MAXSAVE = 8ULL << 30;
		uint64_t want = 0;
		for (auto& x : rs) want += x.size;
		if (want > MAXSAVE) return lb + " : failed (" + util::size(want) + " of address space is too much for one file, unreadable parts would all be zeros. Save the committed regions one by one)";
		int rc = 0;
		std::string err;
		void* f = be::fileOpen(path,over,rc,err,true);
		if (rc == 1) return "EXISTS";
		if (!f) return lb + " : failed (" + util::safe(err) + ")";
		uint64_t total = 0,done = 0,bad = 0;
		for (auto& x : rs) total += x.size;
		const size_t CH = 1 << 20;
		std::vector<uint8_t> buf(CH);
		uint8_t map[(CH >> 12) + 2];
		std::string fail;
		for (auto& x : rs) {
			for (uint64_t off = 0; off < x.size && fail.empty();) {
				if (t.cancel) {
					fail = "cancelled, the file is cut short";
					break;
				}
				size_t n = (size_t)std::min(x.size - off,(uint64_t)CH);
				std::string e;
				size_t got = be::read(r,x.addr + off,n,buf.data(),map,e);
				if (!e.empty()) {
					fail = e;
					break;
				}
				if (dirty) for (auto it = dirty->lower_bound(x.addr + off); it != dirty->end() && it->first < x.addr + off + n; ++it) buf[it->first - x.addr - off] = it->second;
				if (!be::fileWrite(f,buf.data(),n)) {
					fail = "could not write the file, is the disk full?";
					break;
				}
				bad += n - got;
				off += n;
				done += n;
				t.prog = total ? (float)((double)done / (double)total) : 1.0f;
			}
			if (!fail.empty()) break;
		}
		be::fileClose(f);
		if (!fail.empty()) return lb + " : failed (" + util::safe(fail) + ")";
		return lb + " : OK (" + util::size(done) + (bad ? ", " + util::size(bad) + " unreadable as zeros" : "") + ", " + util::safe(path) + ")";
	}

	std::string homePath(const std::string& file) {
		std::string user,home;
		procs::be::invoker(user,home);
		if (home.empty()) home = be::home();
#ifdef _WIN32
		return home + (home.empty() ? "" : "\\") + file;
#else
		return home + (home.empty() ? "" : "/") + file;
#endif
	}

	std::string fileName(const ProcRef& r,const std::string& tail) {
		std::string s = util::safe(r.name);
		for (auto& c : s) {
			bool fine = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_';
			if (!fine) c = '_';
		}
		return (s.empty() ? "process" : s) + "_" + tail;
	}

	bool parseNum(const char* s,uint64_t& out) {
		while (*s == ' ') s++;
		if (!*s) return false;
		bool hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
		for (const char* c = s; *c && !hex; c++) if ((*c >= 'a' && *c <= 'f') || (*c >= 'A' && *c <= 'F')) hex = true;
		char* e = nullptr;
		out = strtoull(s,&e,hex ? 16 : 10);
		return e != s && !*e;
	}

	void init() {
		cfg::optBool("regAutoRefresh","Regions","Auto update the Regions tab","Reads the region list again while the tab is open. Off by default : each pass walks the whole address space of the process.",false);
		cfg::optInt("regAutoSec","Regions","Auto update interval, sec","Time between passes of the Regions tab auto update.",5,1,60);
		procwin::addTab("Regions",30,drawTab);
		// the editor windows live on after their tab or process window goes away, so they need a draw of their own each frame
		procwin::addFloat([] {
			hexDraw();
			pruneTabs();
			prune();
		});
	}

	void regTools() {
		regToolsImpl();
	}

}
