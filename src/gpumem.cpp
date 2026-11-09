#include "gpumem.h"
#include "sysmem.h"
#include "cfg.h"
#include <algorithm>
#include <ctime>
#include <map>

namespace {

	using namespace gpumem;
	using Key = std::pair<uint32_t,uint64_t>;

	struct Ring {
		GpuProcHist d;
		uint64_t seq = 0,pubSeq = 0,goneMs = 0;
		std::shared_ptr<const GpuProcHist> pub;
	};

	struct Want {
		uint64_t ms = 0;
		ProcRef r;
		std::shared_ptr<const GpuDetail> det;
	};

	Snap<GpuSnap> g_snap;
	Snap<GpuHist> g_hist;
	std::mutex g_m;
	std::map<Key,Ring> g_rings;
	std::map<Key,Want> g_want;
	std::string g_src; // under g_m, the source the rings were filled from

	uint64_t add(uint64_t a,uint64_t b) {
		if (a == NA) return b;
		return b == NA ? a : a + b;
	}

	size_t capN() {
		return (size_t)std::max(60,std::min(3600,cfg::geti("memHistN",600)));
	}

	template<class V,class X> void push(V& v,X x,size_t cap) {
		v.push_back(x);
		if (v.size() > cap) v.erase(v.begin(),v.end() - cap);
	}

	void range(GpuRange& r,uint64_t v) {
		if (v == NA || !v) return;
		if (!r.lo || v < r.lo) r.lo = v;
		if (v > r.hi) r.hi = v;
	}

	void pushHist(const GpuSnap& s) {
		auto cur = g_hist.get();
		auto h = cur ? std::make_shared<GpuHist>(*cur) : std::make_shared<GpuHist>();
		size_t cap = capN();
		push(h->ded,util::toMbf(s.ded),cap);
		push(h->shared,util::toMbf(s.shared),cap);
		push(h->unixSec,(uint64_t)std::time(nullptr),cap);
		g_hist.put(h);
	}

	void pushRings(const GpuSnap& s,uint64_t now) {
		auto l = procs::latest();
		size_t cap = capN();
		uint64_t ts = (uint64_t)std::time(nullptr);
		std::lock_guard<std::mutex> lk(g_m);
		// commit and the committed pair mean other things on counters and D3DKMT, never mix them in one min, max or graph
		if (g_src != s.source) {
			if (!g_src.empty()) for (auto& kv : g_rings) {
				Ring& r = kv.second;
				std::fill(r.d.commit.begin(),r.d.commit.end(),-1.0f);
				r.d.rCommit = r.d.rDedC = r.d.rSharedC = GpuRange();
				r.seq++;
			}
			g_src = s.source;
		}
		for (auto& p : s.procs) if (p.start) g_rings[Key(p.pid,p.start)];
		for (auto it = g_rings.begin(); it != g_rings.end();) {
			Ring& r = it->second;
			ProcRef pr;
			pr.pid = it->first.first;
			pr.start = it->first.second;
			if (!l->find(pr)) {
				if (!r.goneMs) r.goneMs = now;
				if (now - r.goneMs > 60000 && !g_want.count(it->first)) {
					it = g_rings.erase(it);
					continue;
				}
				++it;
				continue;
			}
			r.goneMs = 0;
			GpuProc p = s.now(pr);
			push(r.d.ded,util::toMbf(p.ded),cap);
			push(r.d.shared,util::toMbf(p.shared),cap);
			push(r.d.commit,util::toMbf(p.commit),cap);
			push(r.d.unixSec,ts,cap);
			range(r.d.rDed,p.ded);
			range(r.d.rShared,p.shared);
			range(r.d.rDedC,p.dedCommit);
			range(r.d.rSharedC,p.sharedCommit);
			range(r.d.rCommit,p.commit);
			range(r.d.rTotal,add(p.ded,p.shared));
			r.seq++;
			++it;
		}
	}

	void pushDetail(uint64_t now) {
		std::vector<ProcRef> l;
		{
			std::lock_guard<std::mutex> lk(g_m);
			for (auto it = g_want.begin(); it != g_want.end();) {
				if (now - std::min(now,it->second.ms) > 5000) it = g_want.erase(it);
				else {
					l.push_back(it->second.r);
					++it;
				}
			}
		}
		bool ad = cfg::getb("gpuAdapterStats",false);
		for (auto& r : l) {
			if (!procs::alive(r)) continue;
			auto d = std::make_shared<GpuDetail>(be::detail(r,ad));
			std::lock_guard<std::mutex> lk(g_m);
			auto it = g_want.find(Key(r.pid,r.start));
			// procs::alive lags a real exit, the last good read is what is known then
			if (it != g_want.end() && !(d->gone && it->second.det && it->second.det->why.empty())) it->second.det = d;
		}
	}

	void tick() {
		static uint64_t last = 0;
		uint64_t now = util::nowMs();
		if (last && now - last < (procs::unseen() ? 4500u : 900u)) return; // nobody looks : one PDH sample per 5 s
		last = now;
		auto s = std::make_shared<GpuSnap>();
		s->t = now;
		s->on = cfg::getb("gpuMonitor",true);
		if (!s->on) {
			s->why = "GPU monitoring is off, it can be turned on in Settings";
			g_snap.put(s);
			return;
		}
		be::read(*s,cfg::getb("gpuPerfCounters",true));
		for (auto& a : s->adapters) {
			s->ded = add(s->ded,a.ded);
			s->dedLimit = add(s->dedLimit,a.dedLimit);
			s->shared = add(s->shared,a.shared);
			s->sharedLimit = add(s->sharedLimit,a.sharedLimit);
			s->sharedOs = add(s->sharedOs,a.sharedOs);
		}
		// each adapter may use about half of RAM, the sum must not claim more RAM than there is
		uint64_t ram = sysmem::latest()->physTotal;
		if (s->sharedLimit != NA && ram != NA && s->sharedLimit > ram) s->sharedLimit = ram;
		auto l = procs::latest();
		for (auto& p : s->procs) {
			const ProcRow* r = l->find(p.pid);
			if (!r) {
				p.name = "pid " + std::to_string(p.pid);
				continue;
			}
			p.name = r->name;
			p.start = r->start;
		}
		std::sort(s->procs.begin(),s->procs.end(),[](const GpuProc& a,const GpuProc& b) { return a.pid < b.pid; });
		pushHist(*s);
		if (!s->adapters.empty()) pushRings(*s,now);
		g_snap.put(s);
		pushDetail(now);
	}

}

const GpuProc* GpuSnap::find(const ProcRef& r) const {
	auto it = std::lower_bound(procs.begin(),procs.end(),r.pid,[](const GpuProc& a,uint32_t pid) { return a.pid < pid; });
	if (it == procs.end() || it->pid != r.pid) return nullptr;
	return !it->start || !r.start || it->start == r.start ? &*it : nullptr;
}

GpuProc GpuSnap::now(const ProcRef& r) const {
	if (const GpuProc* p = find(r)) return *p;
	GpuProc z;
	z.pid = r.pid;
	z.start = r.start;
	z.name = r.name;
	if (!t || !on || adapters.empty() || denied.count(r.pid)) return z;
	z.ded = z.shared = z.dedCommit = z.sharedCommit = 0;
	if (hasCommit) z.commit = 0;
	return z;
}

namespace gpumem {

	void init() {
		g_snap.put(std::make_shared<const GpuSnap>());
		g_hist.put(std::make_shared<const GpuHist>());
		cfg::optBool("gpuMonitor","GPU","Enable GPU monitoring","Reads adapter and per process GPU memory in the background. Off, the GPU page says so and nothing is queried.",true);
#ifdef _WIN32
		cfg::optBool("gpuPerfCounters","GPU","Use performance counters","Read the GPU counters, which needs no process handles. Off, FRS asks the graphics driver per process, that needs admin for other users' processes.",true);
#endif
		cfg::optBool("gpuAdapterStats","GPU","Per process adapter statistics","Adds GPU virtual memory and evicted bytes to a process's GPU tab. Queries that process per adapter while the tab is open.",false);
		sampler::add("gpumem",0,tick);
		initPage();
	}

	std::shared_ptr<const GpuSnap> latest() {
		return g_snap.get();
	}

	std::shared_ptr<const GpuHist> history() {
		return g_hist.get();
	}

	std::shared_ptr<const GpuProcHist> procHist(const ProcRef& r) {
		std::lock_guard<std::mutex> lk(g_m);
		auto it = g_rings.find(Key(r.pid,r.start));
		if (it == g_rings.end()) return nullptr;
		Ring& x = it->second;
		if (!x.pub || x.pubSeq != x.seq) {
			x.pub = std::make_shared<const GpuProcHist>(x.d);
			x.pubSeq = x.seq;
		}
		return x.pub;
	}

	void want(const ProcRef& r) {
		std::lock_guard<std::mutex> lk(g_m);
		Want& w = g_want[Key(r.pid,r.start)];
		w.ms = util::nowMs();
		w.r = r;
	}

	std::shared_ptr<const GpuDetail> detail(const ProcRef& r) {
		std::lock_guard<std::mutex> lk(g_m);
		auto it = g_want.find(Key(r.pid,r.start));
		return it == g_want.end() ? nullptr : it->second.det;
	}

	GpuDetail readDetail(const ProcRef& r) {
		return be::detail(r,cfg::getb("gpuAdapterStats",false));
	}

	std::string kmtNote(const GpuSnap& s) {
		return std::string(s.source) == "D3DKMT" ? " Read through D3DKMT here, so it is the driver's committed bytes per segment, a different number from the counters." : "";
	}

}
