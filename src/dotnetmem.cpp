#include "dotnetmem.h"
#include <algorithm>
#include <map>

namespace {

	using namespace dotnetmem;
	using Key = std::pair<uint32_t,uint64_t>;

	struct D {
		ProcRef r;
		std::atomic<uint64_t> wantMs{0},useMs{0}; // useMs : the tab or MCP looked at it, keeps it a minute after an exit
		Snap<ClrMem> pub;
		std::mutex m; // readers one at a time, so the rates see their own previous sample
		std::shared_ptr<const ClrMem> prev;
	};

	std::mutex g_m;
	std::map<Key,std::shared_ptr<D>> g_d;

	std::shared_ptr<D> get(const ProcRef& r) {
		std::lock_guard<std::mutex> lk(g_m);
		auto& d = g_d[Key(r.pid,r.start)];
		if (!d) {
			d = std::make_shared<D>();
			d->r = r;
		}
		d->useMs = util::nowMs();
		return d;
	}

	uint64_t perSec(uint64_t cur,uint64_t prev,uint64_t ms) {
		if (cur == NA || prev == NA || !ms) return NA;
		return cur > prev ? (uint64_t)((double)(cur - prev) * 1000.0 / (double)ms) : 0;
	}

	std::shared_ptr<const ClrMem> readD(D& d) {
		std::lock_guard<std::mutex> lk(d.m);
		auto m = std::make_shared<ClrMem>();
		be::read(d.r,*m);
		m->t = util::nowMs();
		// procs::alive lags a real exit, the block goes away first and the last numbers are what is known
		if (m->kind == ClrMem::Gone && d.prev && d.prev->block) return d.pub.get();
		bool keepPrev = false;
		if (m->block) {
			m->allHeaps = m->genSize[1] + m->genSize[2] + m->loh;
			if (d.prev && d.prev->block) {
				uint64_t ms = m->t - d.prev->t;
				// the tab and MCP can read right after each other, a tiny window says nothing
				keepPrev = ms < 500;
				if (keepPrev) {
					m->rGen0 = d.prev->rGen0;
					m->rGen1 = d.prev->rGen1;
					m->rFinal = d.prev->rFinal;
					m->rAlloc = d.prev->rAlloc;
				} else {
					m->rGen0 = perSec(m->promoted[0],d.prev->promoted[0],ms);
					m->rGen1 = perSec(m->promoted[1],d.prev->promoted[1],ms);
					m->rFinal = perSec(m->promotedFinal,d.prev->promotedFinal,ms);
					m->rAlloc = perSec(m->alloc,d.prev->alloc,ms);
				}
			}
		}
		if (!keepPrev) d.prev = m;
		std::shared_ptr<const ClrMem> c = m;
		d.pub.put(c);
		return c;
	}

	void tick() {
		std::vector<std::shared_ptr<D>> l;
		{
			std::lock_guard<std::mutex> lk(g_m);
			for (auto& kv : g_d) l.push_back(kv.second);
		}
		uint64_t now = util::nowMs();
		for (auto& d : l) {
			uint64_t age = now - std::min(now,d->wantMs.load());
			uint64_t idle = now - std::min(now,d->useMs.load());
			bool up = procs::alive(d->r);
			if (up && age < 3000) readD(*d);
			else if (!up && idle > 60000) {
				std::lock_guard<std::mutex> lk(g_m);
				g_d.erase(Key(d->r.pid,d->r.start));
			}
		}
	}

}

namespace dotnetmem {

	void init() {
		sampler::add("dotnetmem",1000,tick);
	}

	void want(const ProcRef& r) {
		get(r)->wantMs = util::nowMs();
	}

	std::shared_ptr<const ClrMem> view(const ProcRef& r) {
		return get(r)->pub.get();
	}

	std::shared_ptr<const ClrMem> read(const ProcRef& r) {
		return readD(*get(r));
	}

	std::shared_ptr<const ClrMem> kept(ProcRef& r) {
		std::shared_ptr<D> d;
		{
			std::lock_guard<std::mutex> lk(g_m);
			auto it = g_d.find(Key(r.pid,r.start));
			if (it == g_d.end()) return nullptr;
			d = it->second;
		}
		d->useMs = util::nowMs();
		r.name = d->r.name;
		return d->pub.get();
	}

	const char* kindName(ClrMem::Kind k) {
		return k == ClrMem::V2 ? "v2" : k == ClrMem::V4 ? "v4" : k == ClrMem::Core ? "core" : nullptr;
	}

}
