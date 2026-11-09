#include "domains.h"
#include "wswatch.h"
#include "gpumem.h"
#include "dotnetmem.h"
#include "cfg.h"
#include "mcp.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace extra { void initTabs(); }

namespace {

	Json by(uint64_t v) {
		return mcp::bytes(v);
	}

	Json txt(const std::string& s) {
		return s.empty() ? Json() : Json(s);
	}

	const char* const kGone = "the process exited, these are the last numbers FRS read";

	// pid and start_time of a process that exited, to look up what FRS kept of it
	bool goneRef(const Json& a,ProcRef& r) {
		const Json* p = a.get("pid");
		const Json* st = a.get("start_time");
		if (!p || p->t != Json::Num || p->n < 0 || p->n > 4294967295.0 || p->n != std::floor(p->n)) return false;
		if (!st || st->t != Json::Str || st->s.empty() || st->s.size() > 20 || st->s.find_first_not_of("0123456789") != std::string::npos) return false;
		r.pid = (uint32_t)p->n;
		r.start = strtoull(st->s.c_str(),nullptr,10);
		return !procs::alive(r);
	}

	Json wsHead(const ProcRef& r,const wswatch::View& v,bool gone) {
		Json j = Json::obj();
		j.set("pid",r.pid).set("name",r.name).set("start_time",std::to_string(r.start));
		j.set("enabled",v.probed ? Json(v.on) : Json());
		j.set("irreversible",wswatch::permanent());
		j.set("exited",gone);
		j.set("total_faults",v.probed ? Json(v.total) : Json()).set("unique_instructions",v.probed ? Json(v.rows.size()) : Json()).set("lost",v.probed ? Json(v.lost) : Json());
		j.set("reason",gone ? Json(kGone) : txt(v.why));
		return j;
	}

	Json wsWatch(const Json& a) {
		std::string act = a.str("action","");
		if (act != "start" && act != "read" && act != "status") throw std::runtime_error("action must be start, read or status");
		ProcRef r;
		std::shared_ptr<const wswatch::View> v;
		bool gone = false;
		try {
			r = procs::target(a,true);
		} catch (std::runtime_error&) {
			if (act == "start" || !goneRef(a,r) || !(v = wswatch::kept(r))) throw;
			gone = true;
		}
		if (!gone) {
			auto l = procs::latest();
			const ProcRow* row = l->find(r);
			if (row && row->sys) throw std::runtime_error("pid " + std::to_string(r.pid) + " is a system process, WS watch is refused for those");
		}
		if (act == "start") {
			if (r.pid == procs::be::selfPid()) throw std::runtime_error("refused, that is FRS itself");
			if (wswatch::permanent() && !a.flag("confirm",false)) throw std::runtime_error("start can't be undone, Windows keeps recording faults in this process until it exits. Pass confirm true only if the user wants that");
			auto before = wswatch::read(r);
			std::string why = wswatch::enable(r,"[mcp] ");
			if (!why.empty()) throw std::runtime_error("WS watch failed : " + why);
			v = wswatch::read(r);
			Json j = wsHead(r,*v,false);
			j.set("was_enabled",before->probed ? Json(before->on) : Json());
			return j;
		}
		if (!gone) v = wswatch::read(r);
		if (act == "status") return wsHead(r,*v,gone);
		std::vector<Json> rows;
		rows.reserve(v->rows.size());
		for (auto& x : v->rows) {
			Json o = Json::obj();
			o.set("pc",util::hex(x.pc)).set("instruction",x.instr).set("file",txt(x.file)).set("count",x.count);
			rows.push_back(std::move(o));
		}
		Json j = mcp::paged(rows,a,200,5000);
		for (auto& kv : wsHead(r,*v,gone).o) j.set(kv.first,kv.second);
		return j;
	}

	Json gpuProcJson(const GpuProc& p) {
		Json j = Json::obj();
		j.set("pid",p.pid).set("name",txt(p.name)).set("start_time",p.start ? Json(std::to_string(p.start)) : Json());
		j.set("dedicated_bytes",by(p.ded)).set("shared_bytes",by(p.shared));
		j.set("dedicated_committed_bytes",by(p.dedCommit)).set("shared_committed_bytes",by(p.sharedCommit)).set("commit_bytes",by(p.commit));
		return j;
	}

	Json rangeJson(const GpuRange* g) {
		bool has = g && g->hi;
		Json j = Json::obj();
		j.set("min_bytes",has ? Json(g->lo) : Json()).set("max_bytes",has ? Json(g->hi) : Json()).set("diff_bytes",has ? Json(g->hi - g->lo) : Json());
		return j;
	}

#ifdef _WIN32
	const char* const kGpuNote = "Shared GPU memory is system RAM and already part of used RAM, never add it on top. Dedicated is VRAM on a discrete card, not RAM. dedicated_bytes and shared_bytes are resident (Windows Local Usage, Non Local Usage). The committed pair is Windows Dedicated Usage and Shared Usage, what Task Manager shows.";
#else
	const char* const kGpuNote = "Shared GPU memory is system RAM and already part of used RAM, never add it on top. Dedicated is VRAM on a discrete card, not RAM. dedicated_bytes and shared_bytes are resident (drm-resident-*). The committed pair is drm-total-*. commit_bytes and driver_system_memory stay null, Linux has no such numbers.";
#endif

	bool gpuOk(const GpuSnap& s) {
		return s.t && s.on && !s.adapters.empty();
	}

	Json gpuOne(const GpuSnap& s,const Json& a) {
		ProcRef r;
		bool gone = false;
		try {
			r = procs::target(a,false);
		} catch (std::runtime_error&) {
			if (!goneRef(a,r) || !gpumem::procHist(r)) throw;
			gone = true;
		}
		GpuProc p;
		if (gone) {
			p.pid = r.pid;
			p.start = r.start;
		} else p = s.now(r);
		Json j = gpuProcJson(p);
		j.set("exited",gone);
		j.set("holds_gpu_memory",p.ded == NA ? Json() : Json(s.find(r) != nullptr));
		auto h = gpumem::procHist(r);
		Json st = Json::obj();
		st.set("note","since FRS started, zero samples don't count, like System Informer");
		st.set("dedicated",rangeJson(h ? &h->rDed : nullptr)).set("shared",rangeJson(h ? &h->rShared : nullptr));
		st.set("dedicated_committed",rangeJson(h ? &h->rDedC : nullptr)).set("shared_committed",rangeJson(h ? &h->rSharedC : nullptr));
		st.set("commit",rangeJson(h ? &h->rCommit : nullptr)).set("total_resident",rangeJson(h ? &h->rTotal : nullptr));
		j.set("statistics",st);
		GpuDetail d;
		if (gone) d.why = "the process exited, statistics are what FRS kept, the rest is gone with it";
		else if (gpuOk(s)) d = gpumem::readDetail(r);
		else d.why = !s.t ? "no sample yet, try again in a moment" : s.why;
		if (d.why.empty() && p.ded == NA) d.why = "FRS could not read this process, this needs admin or root";
		Json sm = Json::obj();
		sm.set("allocated_bytes",by(d.alloc)).set("reserved_bytes",by(d.reserved));
		sm.set("write_combined_allocated_bytes",by(d.wcAlloc)).set("write_combined_reserved_bytes",by(d.wcReserved));
		sm.set("cached_allocated_bytes",by(d.cachedAlloc)).set("cached_reserved_bytes",by(d.cachedReserved));
		sm.set("section_allocated_bytes",by(d.sectionAlloc)).set("section_reserved_bytes",by(d.sectionReserved));
		j.set("driver_system_memory",sm);
		j.set("adapter_stats_enabled",cfg::getb("gpuAdapterStats",false));
		j.set("virtual_bytes",by(d.virt)).set("evicted_bytes",by(d.evicted));
		j.set("reason",txt(d.why));
		return j;
	}

	uint64_t gpuSortVal(const GpuProc& p,const std::string& k) {
		uint64_t v = k == "dedicated" ? p.ded : k == "dedicated_committed" ? p.dedCommit : k == "shared_committed" ? p.sharedCommit : k == "commit" ? p.commit : p.shared;
		return v == NA ? 0 : v;
	}

	Json gpuMemory(const Json& a) {
		const Json* sbj = a.get("sort_by");
		if (sbj && sbj->t != Json::Null && sbj->t != Json::Str) throw std::runtime_error("sort_by must be a string : shared, dedicated, dedicated_committed, shared_committed, commit, name or pid");
		std::string sb = a.str("sort_by","shared");
		if (sb != "shared" && sb != "dedicated" && sb != "dedicated_committed" && sb != "shared_committed" && sb != "commit" && sb != "name" && sb != "pid") throw std::runtime_error("sort_by must be shared, dedicated, dedicated_committed, shared_committed, commit, name or pid");
		auto s = gpumem::latest();
		for (int i = 0; i < 150 && !s->t && mcp::alive(); i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			s = gpumem::latest();
		}
		std::vector<const GpuProc*> v;
		for (auto& p : s->procs) v.push_back(&p);
		std::stable_sort(v.begin(),v.end(),[&sb](const GpuProc* x,const GpuProc* y) {
			if (sb == "name") return util::lower(x->name) < util::lower(y->name);
			if (sb == "pid") return x->pid < y->pid;
			return gpuSortVal(*x,sb) > gpuSortVal(*y,sb);
		});
		std::vector<Json> rows;
		rows.reserve(v.size());
		for (auto* p : v) rows.push_back(gpuProcJson(*p));
		Json j = mcp::paged(rows,a,100,1000,"processes");
		bool ok = gpuOk(*s);
		j.set("available",ok).set("reason",txt(!s->t ? "no sample yet, try again in a moment" : s->why));
		j.set("note",kGpuNote + gpumem::kmtNote(*s));
		j.set("paused",cfg::getb("memPaused",false)).set("sample_age_ms",s->t ? Json(util::nowMs() - s->t) : Json());
		j.set("source",*s->source ? Json(s->source) : Json());
		j.set("commit_meaning",*s->source && s->hasCommit ? Json(s->commitName) : Json());
		j.set("processes_unreadable",ok ? Json(s->denied.size()) : Json());
		Json t = Json::obj();
		t.set("dedicated_bytes",by(s->ded)).set("dedicated_limit_bytes",by(s->dedLimit));
		t.set("shared_bytes",by(s->shared)).set("shared_limit_bytes",by(s->sharedLimit));
		t.set("shared_adapter_counter_bytes",by(s->sharedOs));
		j.set("total",t);
		auto m = sysmem::latest();
		Json ram = Json::obj();
		ram.set("used_bytes",by(m->physUsed)).set("total_bytes",by(m->physTotal));
		j.set("ram",ram);
		Json ad = Json::arr();
		for (auto& x : s->adapters) {
			Json o = Json::obj();
			o.set("name",x.name).set("id",x.id);
			o.set("dedicated_bytes",by(x.ded)).set("dedicated_limit_bytes",by(x.dedLimit));
			o.set("shared_bytes",by(x.shared)).set("shared_limit_bytes",by(x.sharedLimit));
			o.set("shared_adapter_counter_bytes",by(x.sharedOs));
			o.set("ram_set_aside_bytes",by(x.sysDed)).set("total_memory_bytes",by(x.installed)).set("reserved_bytes",by(x.reserved));
			ad.push(o);
		}
		j.set("adapters",ad);
		const Json* pid = a.get("pid");
		j.set("process",pid && pid->t != Json::Null ? gpuOne(*s,a) : Json());
		return j;
	}

	Json dotnetMemory(const Json& a) {
		ProcRef r;
		std::shared_ptr<const ClrMem> m;
		bool gone = false;
		try {
			r = procs::target(a,false);
		} catch (std::runtime_error&) {
			if (!goneRef(a,r) || !(m = dotnetmem::kept(r)) || !m->block) throw;
			gone = true;
		}
		if (!gone) m = dotnetmem::read(r);
		const char* kn = dotnetmem::kindName(m->kind);
		Json j = Json::obj();
		j.set("pid",r.pid).set("name",r.name).set("start_time",std::to_string(r.start));
		j.set("exited",gone);
		j.set("is_dotnet",m->kind == ClrMem::NotHere || m->kind == ClrMem::Gone ? Json() : Json(kn != nullptr));
		j.set("clr",kn ? Json(kn) : Json());
		j.set("wow64",m->block ? Json(m->wow64) : Json());
		j.set("has_numbers",m->block).set("reason",gone ? Json(kGone) : txt(m->why));
		j.set("note","gen0_budget_bytes is the budget that triggers the next GC, not bytes in use, all_heaps_bytes leaves it out on purpose. Values change when a GC ends");
		j.set("gen0_budget_bytes",by(m->genSize[0])).set("gen1_bytes",by(m->genSize[1])).set("gen2_bytes",by(m->genSize[2]));
		j.set("loh_bytes",by(m->loh)).set("all_heaps_bytes",by(m->allHeaps));
		j.set("committed_bytes",by(m->committed)).set("reserved_bytes",by(m->reserved)).set("loader_heap_bytes",by(m->loader));
		j.set("gen0_collections",by(m->genCount[0])).set("gen1_collections",by(m->genCount[1])).set("gen2_collections",by(m->genCount[2]));
		j.set("induced_gcs",by(m->induced));
		j.set("promoted_gen0_bytes",by(m->promoted[0])).set("promoted_gen1_bytes",by(m->promoted[1])).set("promoted_finalization_bytes",by(m->promotedFinal));
		j.set("finalization_survivors",by(m->survivors)).set("gc_handles",by(m->handles));
		j.set("pinned_objects",by(m->pinned)).set("sink_blocks",by(m->sink));
		j.set("allocated_bytes",by(m->alloc)).set("allocated_loh_bytes",by(m->allocLoh));
		j.set("allocated_bytes_per_sec",by(m->rAlloc));
		j.set("promoted_gen0_change_per_sec",by(m->rGen0)).set("promoted_gen1_change_per_sec",by(m->rGen1)).set("promoted_finalization_change_per_sec",by(m->rFinal));
		return j;
	}

}

namespace extra {

	void init() {
		wswatch::init();
		gpumem::init();
		dotnetmem::init();
		initTabs();
	}

	void regTools() {
		mcp::add("Inspect","ws_watch",
			"Page faults per faulting instruction in one process. start turns it on and on Windows that can NOT be undone, it stays until the process exits, so it needs confirm. read returns the counts, status only the state. Both drain the OS list into FRS's running totals, so another tool reading the same process splits the data. One Windows entry can stand for several pages faulted in together, so total_faults reads below the OS page fault counter.",
			R"j({"type":"object","properties":{
				"action":{"type":"string","enum":["start","read","status"],"description":"what to do"},
				"pid":{"type":"integer","description":"Target process"},
				"start_time":{"type":"string","description":"From list_processes, refuses a reused pid. read and status still answer about a minute after an exit, with exited true"},
				"confirm":{"type":"boolean","description":"start only : must be true, the user accepts that it can't be undone"},
				"limit":{"type":"integer","description":"read : rows per page, default 200, max 5000"},
				"offset":{"type":"integer","description":"read : rows to skip"}},
				"required":["action","pid","start_time"]})j",
			mcp::Danger,wsWatch);
		mcp::add("Inspect","gpu_memory",
			"GPU adapters plus per process dedicated and shared memory. Shared GPU memory is system RAM and already part of used RAM, never add it on top. Pass pid for one process in depth.",
			R"j({"type":"object","properties":{
				"pid":{"type":"integer","description":"One process in depth, under the result's process key"},
				"start_time":{"type":"string","description":"With pid, from list_processes, refuses a reused pid. With it an exited process still answers about a minute, with exited true"},
				"sort_by":{"type":"string","enum":["shared","dedicated","dedicated_committed","shared_committed","commit","name","pid"],"description":"Default shared, biggest first"},
				"limit":{"type":"integer","description":"Processes per page, default 100, max 1000"},
				"offset":{"type":"integer","description":"Processes to skip"}}})j",
			mcp::Read,gpuMemory);
		mcp::add("Inspect","dotnet_memory",
			"GC heap sizes of one .NET Framework process (CLR v2 or v4), from a read only memory block. Fields are null when it is not .NET or when it is .NET 5+ or Core, which has no such block. Gen 0 is a budget, not usage.",
			R"j({"type":"object","properties":{
				"pid":{"type":"integer","description":"Target process"},
				"start_time":{"type":"string","description":"From list_processes, refuses a reused pid. With it an exited process still answers about a minute, with exited true"}},
				"required":["pid"]})j",
			mcp::Read,dotnetMemory);
	}

}
