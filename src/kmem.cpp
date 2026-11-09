#include "kmem.h"
#include "cfg.h"
#include "sysmem.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>

namespace kmem {

	const PoolCol kPoolCols[PCN] = {
		{"Tag","Cache","tag","Four character tag. A protected tag has its high bit masked, other bytes that don't print show as a dot.",58,true,false,"Slab cache name."},
		{"Driver",nullptr,"driver","Owning binary, from pooltag.txt next to the exe. <unknown> means that file does not name one.",96,true,false},
		{"Description","Object","description","What the tag is for, from pooltag.txt next to the exe.",190,true,false,"Object size and objects per slab."},
		{"Paged allocs",nullptr,"paged_allocs","Paged allocations since boot.",120,false,false},
		{"Paged frees",nullptr,"paged_frees","Paged frees since boot.",120,false,false},
		{"Paged now",nullptr,"paged_current","Outstanding paged allocations, signed so a wrap can't turn it huge.",72,false,false},
		{"Paged bytes",nullptr,"paged_bytes","Paged pool in use by this tag right now.",78,true,true},
		{"NP allocs","Objects","nonpaged_allocs","Non-paged allocations since boot.",120,false,false,"num_objs, allocated object slots right now, not a running total."},
		{"NP frees",nullptr,"nonpaged_frees","Non-paged frees since boot.",120,false,false},
		{"NP now","Active","nonpaged_current","Outstanding non-paged allocations, signed so a wrap can't turn it huge.",72,false,false,"active_objs, objects in use right now."},
		{"NP bytes","Active bytes","nonpaged_bytes","Non-paged pool in use by this tag right now, always physical RAM.",78,true,true,"active_objs times objsize, always physical RAM."},
		{"Allocs",nullptr,"allocs","Paged plus non-paged allocations.",130,false,false},
		{"Frees",nullptr,"frees","Paged plus non-paged frees.",130,false,false},
		{"Now",nullptr,"current","Outstanding allocations, both pools.",64,false,false},
		{"Bytes",nullptr,"bytes","Paged plus non-paged bytes in use.",74,true,true},
		{"Paged allocs delta",nullptr,"paged_allocs_delta","Paged allocations since the last sample.",110,false,false},
		{"Paged frees delta",nullptr,"paged_frees_delta","Paged frees since the last sample.",106,false,false},
		{"Paged now delta",nullptr,"paged_current_delta","Change in outstanding paged allocations.",104,false,false},
		{"Paged bytes delta",nullptr,"paged_bytes_delta","Change in paged bytes since the last sample.",106,true,true},
		{"NP allocs delta","Objects delta","nonpaged_allocs_delta","Non-paged allocations since the last sample.",104,false,false,"Change in num_objs since the last sample."},
		{"NP frees delta",nullptr,"nonpaged_frees_delta","Non-paged frees since the last sample.",100,false,false},
		{"NP now delta","Active delta","nonpaged_current_delta","Change in outstanding non-paged allocations.",96,false,false,"Change in active_objs since the last sample."},
		{"NP bytes delta","Bytes delta","nonpaged_bytes_delta","Change in non-paged bytes since the last sample. The best single leak signal, this memory is always RAM.",100,true,true,"Change in active bytes since the last sample. The best single leak signal, slab memory is always RAM."},
		{"Allocs delta",nullptr,"allocs_delta","Allocations since the last sample.",84,false,false},
		{"Frees delta",nullptr,"frees_delta","Frees since the last sample.",80,false,false},
		{"Now delta",nullptr,"current_delta","Change in outstanding allocations, both pools.",76,false,false},
		{"Bytes delta",nullptr,"bytes_delta","Change in pool bytes since the last sample.",80,false,true}
	};

	namespace {

		std::atomic<uint64_t> g_want{0};
		std::mutex g_poolMu,g_bigMu,g_pfMu,g_compMu;
		Snap<PoolSnap> g_pool;
		Snap<BigSnap> g_big;
		Snap<PageSnap> g_pf;
		Snap<CompHist> g_comp;
		std::map<std::string,uint64_t> g_peak;
		uint64_t g_compT = 0;

		int64_t addU(int64_t a,int64_t b) {
			return a == UNK ? b : b == UNK ? a : a + b;
		}

		int64_t diff(int64_t a,int64_t b) {
			return a == UNK || b == UNK ? UNK : a - b;
		}

		// alloc and free counters only count up, so the unsigned difference survives one wrap
		int64_t diffCnt(int64_t a,int64_t b,int bits) {
			if (a == UNK || b == UNK) return UNK;
			return bits == 32 ? (int64_t)(uint32_t)(a - b) : a - b;
		}

		const TagDb& tagDb(TagStat* st = nullptr) {
			static TagDb db;
			static TagStat stat;
			static std::once_flag once;
			std::call_once(once,[] {
				std::string txt;
				if (!osTagFile(txt,stat.path,stat.why)) return;
				parseTags(txt,db);
				stat.n = db.size();
				stat.loaded = stat.n > 0;
				if (!stat.loaded) stat.why = "No tag lines in it";
			});
			if (st) *st = stat;
			return db;
		}

		void compTick() {
			auto m = sysmem::latest();
			std::lock_guard<std::mutex> lk(g_compMu);
			if (m->t == g_compT || m->compWs == NA) return;
			uint64_t cw = m->compWs;
			if (kWin && m->compPid) {
				// the store query's working set is 32 bit, the process row fixes it. The first sysmem sample can come before any process list
				auto pl = procs::latest();
				if (pl->rows.empty()) return;
				const ProcRow* r = pl->find(m->compPid);
				if (r && r->ws != NA) cw = std::max(cw,r->ws);
			}
			g_compT = m->t;
			auto n = std::make_shared<CompHist>(*g_comp.get());
			bool hasRatio = m->compData != NA && m->compSize != NA && m->compData && m->compSize;
			double ws = (double)cw / (1024.0 * 1024.0);
			n->ws.push_back((float)ws);
			n->data.push_back((float)(hasRatio ? ws * (double)m->compData / (double)m->compSize : ws));
			size_t mx = (size_t)std::max(60,cfg::geti("memHistN",600));
			if (n->ws.size() > mx) {
				n->ws.erase(n->ws.begin(),n->ws.end() - mx);
				n->data.erase(n->data.begin(),n->data.end() - mx);
			}
			g_comp.put(n);
		}

		void samplePool() {
			std::lock_guard<std::mutex> lk(g_poolMu);
			auto s = g_pool.get();
			// a tool call just read it, another read now would leave a delta window of a few ms
			if (s && util::nowMs() - s->t < (uint64_t)sampler::intervalMs() / 2) return;
			g_pool.put(poolRead(s.get()));
		}

		void tick() {
			compTick();
			bool w = util::nowMs() < g_want;
			if (w) samplePool();
			if (w || !kWin) pfRead(); // Linux has no peak of its own, it must keep looking
		}

		int icmp(const std::string& a,const std::string& b) {
			size_t n = std::min(a.size(),b.size());
			for (size_t i = 0; i < n; i++) {
				int x = std::tolower((unsigned char)a[i]);
				int y = std::tolower((unsigned char)b[i]);
				if (x != y) return x - y;
			}
			return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
		}

		std::string countCell(int64_t v) {
			if (v == UNK) return "N/A";
			if (!v) return "";
			return v < 0 ? "-" + util::count((uint64_t)-v) : util::count((uint64_t)v);
		}

		std::string countDelta(int64_t v) {
			if (v == UNK) return "N/A";
			if (!v) return "";
			return v < 0 ? "-" + util::count((uint64_t)-v) : "+" + util::count((uint64_t)v);
		}

	}

	int64_t poolVal(const PoolRow& r,int col) {
		if (col < PCVal) return UNK;
		return col < PCDelta ? r.v[col - PCVal] : r.d[col - PCDelta];
	}

	std::string poolCell(const PoolRow& r,int col) {
		if (col == PCTag) return r.tag;
		if (col == PCDriver) return r.driver;
		if (col == PCDesc) return r.desc;
		int64_t v = poolVal(r,col);
		if (kPoolCols[col].bytes) {
			if (v == UNK) return "N/A";
			if (!v) return "";
			return col < PCDelta ? util::size((uint64_t)v) : util::delta(v);
		}
		return col < PCDelta ? countCell(v) : countDelta(v);
	}

	bool poolLess(const PoolRow& a,const PoolRow& b,int col,bool desc) {
		if (col < PCVal && col >= 0) {
			const std::string& x = col == PCTag ? a.tag : col == PCDriver ? a.driver : a.desc;
			const std::string& y = col == PCTag ? b.tag : col == PCDriver ? b.driver : b.desc;
			if (x.empty() != y.empty()) return y.empty();
			int c = icmp(x,y);
			if (c) return desc ? c > 0 : c < 0;
		} else if (col >= PCVal) {
			int64_t x = poolVal(a,col);
			int64_t y = poolVal(b,col);
			if ((x == UNK) != (y == UNK)) return y == UNK;
			if (x != y) return desc ? x > y : x < y;
		}
		return a.id < b.id;
	}

	bool poolMatch(const PoolRow& r,const std::string& low) {
		if (low.empty()) return true;
		return util::lower(r.tag).find(low) != std::string::npos || util::lower(r.driver).find(low) != std::string::npos || util::lower(r.desc).find(low) != std::string::npos;
	}

	void wantPool(uint64_t ms) {
		uint64_t t = util::nowMs() + ms;
		uint64_t cur = g_want;
		while (cur < t && !g_want.compare_exchange_weak(cur,t)) {}
	}

	std::shared_ptr<const PoolSnap> poolLatest() {
		return g_pool.get();
	}

	std::shared_ptr<const PoolSnap> poolRead(const PoolSnap* prev) {
		auto s = std::make_shared<PoolSnap>();
		std::vector<PoolRaw> raw;
		s->err = osPool(raw,s->bits,s->src);
		s->t = util::nowMs();
		const TagDb& db = tagDb(&s->db);
		if (!s->err.empty()) return s;

		bool base = prev && prev->err.empty() && !prev->rows.empty();
		s->dtMs = base ? s->t - prev->t : 0;
		std::unordered_map<uint64_t,const PoolRow*> old;
		if (base) {
			old.reserve(prev->rows.size());
			for (auto& r : prev->rows) old[r.id] = &r;
		}
		int64_t thr = (int64_t)cfg::geti("poolHlKb",256) * 1024;
		s->rows.reserve(raw.size());
		for (auto& x : raw) {
			s->rows.emplace_back();
			PoolRow& r = s->rows.back();
			r.id = x.id;
			r.tag = std::move(x.tag);
			r.desc = std::move(x.desc);
			r.prot = x.prot;
			for (int i = 0; i < VAlloc; i++) r.v[i] = x.v[i];
			r.v[VAlloc] = addU(r.v[VPagedAlloc],r.v[VNpAlloc]);
			r.v[VFree] = addU(r.v[VPagedFree],r.v[VNpFree]);
			r.v[VCur] = addU(r.v[VPagedCur],r.v[VNpCur]);
			r.v[VBytes] = addU(r.v[VPagedBytes],r.v[VNpBytes]);

			auto it = old.find(r.id);
			if (it == old.end()) {
				for (int i = 0; i < VN; i++) r.d[i] = UNK;
				r.seenMs = base ? s->t : 0;
				if (const TagInfo* ti = db.find(r.tag)) {
					r.driver = util::safe(ti->driver);
					r.desc = util::safe(ti->desc);
				}
				continue;
			}
			const PoolRow& o = *it->second;
			r.driver = o.driver;
			if (r.desc.empty()) r.desc = o.desc;
			r.seenMs = o.seenMs;
			r.hotMs = o.hotMs;
			r.d[VPagedAlloc] = diffCnt(r.v[VPagedAlloc],o.v[VPagedAlloc],s->bits);
			r.d[VPagedFree] = diffCnt(r.v[VPagedFree],o.v[VPagedFree],s->bits);
			r.d[VNpAlloc] = diffCnt(r.v[VNpAlloc],o.v[VNpAlloc],s->bits);
			r.d[VNpFree] = diffCnt(r.v[VNpFree],o.v[VNpFree],s->bits);
			r.d[VPagedCur] = diff(r.v[VPagedCur],o.v[VPagedCur]);
			r.d[VPagedBytes] = diff(r.v[VPagedBytes],o.v[VPagedBytes]);
			r.d[VNpCur] = diff(r.v[VNpCur],o.v[VNpCur]);
			r.d[VNpBytes] = diff(r.v[VNpBytes],o.v[VNpBytes]);
			r.d[VAlloc] = addU(r.d[VPagedAlloc],r.d[VNpAlloc]);
			r.d[VFree] = addU(r.d[VPagedFree],r.d[VNpFree]);
			r.d[VCur] = addU(r.d[VPagedCur],r.d[VNpCur]);
			r.d[VBytes] = addU(r.d[VPagedBytes],r.d[VNpBytes]);
			if (s->dtMs && r.d[VBytes] != UNK && r.d[VBytes] > 0 && (double)r.d[VBytes] * 1000.0 / (double)std::max<uint64_t>(s->dtMs,250) >= (double)thr) r.hotMs = s->t;
		}
		return s;
	}

	std::shared_ptr<const PoolSnap> poolFresh() {
		wantPool(15000);
		std::lock_guard<std::mutex> lk(g_poolMu);
		auto s = g_pool.get();
		if (s && util::nowMs() - s->t < (uint64_t)sampler::intervalMs()) return s;
		auto n = poolRead(s.get());
		if (!s || !cfg::getb("memPaused",false)) g_pool.put(n);
		return n;
	}

	void sampleNow() {
		bool paused = cfg::getb("memPaused",false);
		if (!paused || !g_pool.get()) samplePool();
		if (!paused || !g_pf.get()) pfRead();
	}

	std::shared_ptr<const BigSnap> bigLatest() {
		return g_big.get();
	}

	std::shared_ptr<const BigSnap> bigRead() {
		std::lock_guard<std::mutex> lk(g_bigMu);
		auto s = std::make_shared<BigSnap>();
		std::vector<BigRaw> raw;
		std::unordered_map<uint64_t,std::string> names;
		s->err = osBig(raw,names,s->src);
		s->t = util::nowMs();
		if (s->err.empty()) {
			TagStat st;
			const TagDb& db = tagDb(&st);
			s->tagFile = st.loaded;
			s->tags.reserve(names.size());
			for (auto& n : names) {
				s->tags.emplace_back();
				BigTag& t = s->tags.back();
				t.id = n.first;
				t.tag = n.second;
				if (const TagInfo* ti = db.find(t.tag)) {
					t.driver = util::safe(ti->driver);
					t.desc = util::safe(ti->desc);
				}
			}
			std::sort(s->tags.begin(),s->tags.end(),[](const BigTag& a,const BigTag& b) {
				int c = icmp(a.tag,b.tag);
				return c ? c < 0 : a.id < b.id;
			});
			std::unordered_map<uint64_t,uint32_t> idx;
			idx.reserve(s->tags.size());
			for (size_t i = 0; i < s->tags.size(); i++) idx[s->tags[i].id] = (uint32_t)i;
			s->rows.reserve(raw.size());
			s->blanked = !raw.empty();
			for (auto& x : raw) {
				uint32_t ti = idx[x.id];
				s->rows.push_back({x.addr,x.size,ti,x.np});
				if (x.addr) s->blanked = false;
				BigTag& t = s->tags[ti];
				t.n++;
				t.bytes += x.size;
				if (x.np) {
					t.npN++;
					t.npBytes += x.size;
					s->npN++;
					s->npBytes += x.size;
				}
				t.largest = std::max(t.largest,x.size);
				s->bytes += x.size;
			}
		}
		g_big.put(s);
		return s;
	}

	std::shared_ptr<const PageSnap> pfLatest() {
		return g_pf.get();
	}

	std::shared_ptr<const PageSnap> pfRead(bool hold) {
		auto s = std::make_shared<PageSnap>();
		s->err = osPages(*s);
		s->t = util::nowMs();
		for (auto& r : s->rows) r.path = util::safe(r.path);
		if (s->ownPeak) {
			std::lock_guard<std::mutex> lk(g_pfMu);
			for (auto& r : s->rows) {
				if (r.used == NA) continue;
				uint64_t& p = g_peak[r.path];
				p = std::max(p,r.used);
				r.peak = p;
			}
		}
		if (!hold || !g_pf.get() || !cfg::getb("memPaused",false)) g_pf.put(s);
		return s;
	}

	std::shared_ptr<const CompHist> compHist() {
		return g_comp.get();
	}

	void initData() {
		g_comp.put(std::make_shared<CompHist>());
		sampler::add("kmem",0,tick);
	}

}
