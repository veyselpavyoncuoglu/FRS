#include "vmdiag.h"
#include "cfg.h"
#include "domains.h"
#include "mcp.h"
#include "vmem.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

	using namespace vmdiag;
	using mcp::numArg;
	using mcp::textArg;

	Json txt(const std::string& s) {
		return s.empty() ? Json() : Json(s);
	}

	Json count(uint64_t v) {
		return v == NA ? Json() : Json(v);
	}

	// on a task : a stuck target must not hold this connection forever or stop the server from closing
	template<class R> std::shared_ptr<const R> await(std::function<std::shared_ptr<const R>(std::atomic<bool>&)> fn,int ms,const std::string& what) {
		auto box = std::make_shared<std::shared_ptr<const R>>();
		auto t = task::run([box,fn](task::T& tk) {
			*box = fn(tk.cancel);
			return std::string();
		},false);
		uint64_t end = util::nowMs() + (uint64_t)ms;
		while (!t->done) {
			if (!mcp::alive()) {
				t->cancel = true;
				throw std::runtime_error("MCP server is stopping");
			}
			if (util::nowMs() > end) {
				t->cancel = true;
				throw std::runtime_error(what + " : no answer in " + std::to_string(ms / 1000) + " s. The target may be frozen or deadlocked, the call can't be cancelled");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		if (!*box) throw std::runtime_error(what + " : " + (t->res.empty() ? std::string("failed") : util::safe(t->res)));
		return *box;
	}

	// a query that runs in the target holds the process until it comes back, even when this call gave up on it
	template<class R> std::shared_ptr<const R> remoteCall(const ProcRef& r,const std::function<std::shared_ptr<const R>()>& fn,const std::string& what,const std::string& line) {
		if (!claim(r.pid)) throw std::runtime_error("another query is still running in pid " + std::to_string(r.pid) + ", try again when it is done");
		uint32_t pid = r.pid;
		auto gate = std::make_shared<std::atomic<int>>(0); // 1 the task got there first, 2 this call gave up first
		try {
			return await<R>([fn,pid,line,gate](std::atomic<bool>& gaveUp) {
				std::shared_ptr<const R> o;
				if (gate->exchange(1) == 2) return o;
				try {
					o = fn();
				} catch (...) {
					unclaim(pid);
					throw;
				}
				unclaim(pid);
				if (gaveUp) appLog(line + " : the thread in the target came back after the call gave up");
				return o;
			},45000,what);
		} catch (const std::exception& e) {
			// the task never ran, so nothing else lets the process go
			if (gate->exchange(2) == 0) unclaim(pid);
			appLog(line + " : failed (" + e.what() + ")");
			throw;
		}
	}

	// paging through a big answer must not start one thread per page in the target
	struct Cached {
		uint32_t pid = 0;
		uint64_t start = 0,t = 0;
		std::string view;
		std::shared_ptr<const void> p;
	};
	std::mutex g_cm;
	std::vector<Cached> g_cache;

	void dropOld() {
		uint64_t now = util::nowMs();
		for (size_t i = 0; i < g_cache.size();) {
			if (now - g_cache[i].t >= 120000) g_cache.erase(g_cache.begin() + i);
			else i++;
		}
	}

	std::shared_ptr<const void> cacheGet(const ProcRef& r,const std::string& view,uint64_t& ageMs) {
		std::lock_guard<std::mutex> lk(g_cm);
		dropOld();
		for (auto& c : g_cache) if (c.pid == r.pid && c.start == r.start && c.view == view) {
			ageMs = util::nowMs() - c.t;
			return c.p;
		}
		return nullptr;
	}

	void cachePut(const ProcRef& r,const std::string& view,std::shared_ptr<const void> p) {
		std::lock_guard<std::mutex> lk(g_cm);
		dropOld();
		for (size_t i = 0; i < g_cache.size();) {
			if (g_cache[i].pid == r.pid && g_cache[i].view == view) g_cache.erase(g_cache.begin() + i);
			else i++;
		}
		if (g_cache.size() >= 4) g_cache.erase(g_cache.begin());
		Cached c;
		c.pid = r.pid;
		c.start = r.start;
		c.t = util::nowMs();
		c.view = view;
		c.p = std::move(p);
		g_cache.push_back(std::move(c));
	}

	Json head(const ProcRef& r,const char* view,const Res& x,uint64_t ageMs) {
		Json j = procs::refJson(r);
		j.set("view",view).set("available",!x.na).set("complete",x.complete);
		j.set("reason",x.na ? txt(x.why) : txt(x.reason)).set("result_age_ms",ageMs).set("result_time",x.unixSec ? Json(util::timeStr(x.unixSec)) : Json());
		return j;
	}

	Json merge(Json j,const Json& paged) {
		for (auto& kv : paged.o) j.set(kv.first,kv.second);
		return j;
	}

	// mcp::paged's result, but a Json only for the rows that go out : a 1 row request must not pay for the whole list
	Json pageOf(size_t total,const Json& a,int def,int mx,const char* key,const std::function<Json(size_t)>& row) {
		double l = a.num("limit",def);
		double o = a.num("offset",0);
		int lim = l < 1.0 ? 1 : l > (double)mx ? mx : (int)l;
		size_t off = o <= 0.0 ? 0 : o >= (double)total ? total : (size_t)o;
		size_t end = std::min(total,off + (size_t)lim);
		Json arr = Json::arr();
		for (size_t i = off; i < end; i++) arr.push(row(i));
		Json j = Json::obj();
		j.set("count",end - off).set("total_count",total).set("offset",off).set("limit",lim);
		j.set("truncated",end < total).set(key,arr);
		return j;
	}

	void failIf(const Res& x,const char* what,const ProcRef& r) {
		if (x.ok || x.na) return;
		throw std::runtime_error(std::string(what) + " failed for pid " + std::to_string(r.pid) + " : " + util::safe(x.why));
	}

	Json heapsView(const ProcRef& r,const Json& a) {
		double off = numArg(a,"offset",0);
		uint64_t age = 0;
		auto res = std::static_pointer_cast<const HeapRes>(off > 0 ? cacheGet(r,"heaps",age) : nullptr);
		if (!res) {
			bool clone = cfg::getb("diagHeapClone",false);
			ProcRef rc = r;
			std::string line = "[mcp] Heaps query (pid " + std::to_string(r.pid) + ")";
			res = remoteCall<HeapRes>(r,[rc,clone] { return be::heaps(rc,clone); },"heaps",line);
			if (res->ok) appLog(line + " : OK");
			else if (!res->na) appLog(line + " : failed (" + util::safe(res->why) + ")");
			failIf(*res,"heaps",r);
			if (res->ok) cachePut(r,"heaps",res);
		}
		uint64_t used = 0,comm = 0,ents = 0;
		bool hu = false,he = false;
		std::vector<Json> rows;
		std::string def;
		for (size_t i = 0; i < res->rows.size(); i++) {
			const Heap& h = res->rows[i];
			if (h.used != NA) {
				used += h.used;
				hu = true;
			}
			if (h.committed != NA) comm += h.committed;
			if (h.entries != NA) {
				ents += h.entries;
				he = true;
			}
			if (h.def) def = util::hex(h.base);
			Json o = Json::obj();
			o.set("index",i + 1).set("address",util::hex(h.base));
			o.set("used_bytes",mcp::bytes(h.used)).set("committed_bytes",mcp::bytes(h.committed)).set("entries",count(h.entries));
			o.set("flags",h.flags == NA ? Json() : Json(util::hex(h.flags))).set("flags_text",h.flags == NA ? Json() : Json(h.flagTxt));
			o.set("class",txt(h.cls)).set("type",txt(h.type)).set("default_heap",h.def);
			rows.push_back(std::move(o));
		}
		Json j = head(r,"heaps",*res,age);
		j.set("remote_thread",be::remote() && !res->na).set("read_from_clone",res->clone).set("approximate",res->approx).set("note",txt(res->note));
		j.set("default_heap",txt(def)).set("heap_count",res->ok ? Json(res->rows.size()) : Json());
		j.set("total_used_bytes",hu ? Json(used) : Json()).set("total_committed_bytes",res->ok ? Json(comm) : Json()).set("total_entries",he ? Json(ents) : Json());
		return merge(j,mcp::paged(rows,a,100,1000,"heaps"));
	}

	Json locksView(const ProcRef& r,const Json& a) {
		double off = numArg(a,"offset",0);
		uint64_t age = 0;
		auto res = std::static_pointer_cast<const LockRes>(off > 0 ? cacheGet(r,"locks",age) : nullptr);
		if (!res) {
			ProcRef rc = r;
			std::string line = "[mcp] Locks query (pid " + std::to_string(r.pid) + ")";
			res = remoteCall<LockRes>(r,[rc] { return be::locks(rc); },"locks",line);
			if (res->ok) appLog(line + " : OK");
			else if (!res->na) appLog(line + " : failed (" + util::safe(res->why) + ")");
			failIf(*res,"locks",r);
			if (res->ok) cachePut(r,"locks",res);
		}
		uint64_t held = 0;
		for (auto& l : res->rows) if (l.owner) held++;
		Json j = head(r,"locks",*res,age);
		j.set("remote_thread",be::remote() && !res->na).set("held_now",res->ok ? Json(held) : Json());
		return merge(j,pageOf(res->rows.size(),a,200,2000,"locks",[&](size_t i) {
			const Lock& l = res->rows[i];
			Json o = Json::obj();
			o.set("index",i + 1).set("type",l.type).set("address",util::hex(l.addr)).set("file",txt(l.file));
			o.set("owner_thread",l.owner ? Json(l.owner) : Json()).set("lock_count",l.lockCount == -1 ? Json() : Json(l.lockCount));
			o.set("contention_count",l.contention).set("entry_count",l.entry).set("recursion_count",l.recursion);
			o.set("waiting_shared",l.waitShared).set("waiting_exclusive",l.waitExcl);
			return o;
		}));
	}

	Json modView(const ProcRef& r,const Json& a) {
		double off = numArg(a,"offset",0);
		uint64_t age = 0;
		auto res = std::static_pointer_cast<const ModRes>(off > 0 ? cacheGet(r,"modified_pages",age) : nullptr);
		if (!res) {
			ProcRef rc = r;
			res = await<ModRes>([rc](std::atomic<bool>& stop) { return be::modPages(rc,stop); },75000,"modified_pages");
			failIf(*res,"modified_pages",r);
			if (res->ok) cachePut(r,"modified_pages",res);
		}
		Json j = head(r,"modified_pages",*res,age);
		j.set("images_scanned",res->images).set("images_skipped",res->skipped).set("images_failed",res->fails).set("resident_pages",res->residentN).set("scanned_bytes",res->scanned).set("note",txt(res->note));
		return merge(j,pageOf(res->rows.size(),a,200,2000,"pages",[&](size_t i) {
			const ModPage& p = res->rows[i];
			Json o = Json::obj();
			o.set("index",i + 1).set("file",txt(p.file)).set("image_base",util::hex(p.base));
			o.set("offset",util::hex(p.addr - p.base)).set("address",util::hex(p.addr)).set("symbol",txt(p.sym)).set("resident",p.resident);
			return o;
		}));
	}

	Json cfgCheckView(const ProcRef& r,const std::string& s) {
		uint64_t addr = 0;
		if (!util::parseHex(s,addr)) throw std::runtime_error("address must be a hex string like 0x7ff6a0000000");
		CfgCheck c = be::cfgCheck(r,addr);
		if (!c.ok) throw std::runtime_error("cfg address check failed for pid " + std::to_string(r.pid) + " : " + util::safe(c.why));
		Json j = procs::refJson(r);
		j.set("view","cfg").set("available",true).set("address",util::hex(addr)).set("bitmap",c.wow ? "32-bit" : "native");
		j.set("cfg_enabled",c.enabled < 0 ? Json() : Json(c.enabled == 1));
		j.set("value",c.val < 0 ? Json() : Json(c.val)).set("meaning",cfgMeaning(c,addr));
		return j;
	}

	Json cfgView(const ProcRef& r,const Json& a) {
		std::string show = textArg(a,"cfg_show","modified");
		bool si = show == "image",sm = show == "modified",sv = show == "all_valid",su = show == "unmapped",all = show == "all";
		if (!si && !sm && !sv && !su && !all) throw std::runtime_error("cfg_show must be modified, image, all_valid, unmapped or all");
		double off = numArg(a,"offset",0);
		uint64_t age = 0;
		auto res = std::static_pointer_cast<const CfgRes>(off > 0 ? cacheGet(r,"cfg",age) : nullptr);
		if (!res) {
			ProcRef rc = r;
			res = await<CfgRes>([rc](std::atomic<bool>& stop) { return be::cfg(rc,stop); },75000,"cfg");
			failIf(*res,"cfg",r);
			if (res->ok) cachePut(r,"cfg",res);
		}
		std::vector<uint32_t> vis;
		for (size_t i = 0; i < res->rows.size(); i++) {
			const CfgEnt& e = res->rows[i];
			if (all || (e.type == CfgModified ? sm : e.type == CfgAllValid ? sv : e.type == CfgUnmapped ? su : si)) vis.push_back((uint32_t)i);
		}
		Json sum = Json::arr();
		for (auto& l : res->summary) sum.push(l);
		Json j = head(r,"cfg",*res,age);
		auto n = [&](uint64_t v) { return res->ok ? Json(v) : Json(); };
		j.set("cfg_enabled",res->enabled < 0 ? Json() : Json(res->enabled == 1)).set("summary",sum).set("truncated_at_limit",res->truncated).set("unread_bitmap_bytes",n(res->unread)).set("scanned_bytes",n(res->scanned));
		Json cn = Json::obj();
		cn.set("image",n(res->counts[CfgImage])).set("modified",n(res->counts[CfgModified])).set("export_suppressed",n(res->counts[CfgSuppressed])).set("all_valid",n(res->counts[CfgAllValid])).set("not_mapped_here",n(res->counts[CfgUnmapped]));
		j.set("counts",cn).set("show",show);
		return merge(j,pageOf(vis.size(),a,200,2000,"targets",[&](size_t k) {
			size_t i = vis[k];
			const CfgEnt& e = res->rows[i];
			const CfgReg* g = e.reg >= 0 ? &res->regs[e.reg] : nullptr;
			Json o = Json::obj();
			o.set("index",i + 1).set("bitmap",e.wow ? "32-bit" : "native").set("type",cfgTypeName(e.type)).set("address",util::hex(e.addr));
			o.set("end",e.end ? Json(util::hex(e.end)) : Json()).set("range_bytes",e.end ? Json(e.end - e.addr) : Json()).set("value",e.end ? Json() : Json((int)e.val));
			o.set("symbol",txt(symAt(res->syms,e.addr)));
			o.set("allocation_base",g ? Json(util::hex(g->alloc)) : Json()).set("region_base",g ? Json(util::hex(g->base)) : Json()).set("region_bytes",g ? Json(g->size) : Json());
			o.set("memory_type",g ? txt(g->typeTxt) : Json()).set("state",g ? txt(g->stateTxt) : Json()).set("protection",g ? txt(g->protTxt) : Json()).set("mapped_file",g ? txt(g->file) : Json());
			return o;
		}));
	}

	Json gdiView(const ProcRef& r,const Json& a) {
		double off = numArg(a,"offset",0);
		uint64_t age = 0;
		auto res = std::static_pointer_cast<const GdiRes>(off > 0 ? cacheGet(r,"gdi",age) : nullptr);
		if (!res) {
			ProcRef rc = r;
			res = await<GdiRes>([rc](std::atomic<bool>&) { return be::gdi(rc); },30000,"gdi");
			failIf(*res,"gdi",r);
			if (res->ok) cachePut(r,"gdi",res);
		}
		Json j = head(r,"gdi",*res,age);
		j.set("os_gdi_objects",count(res->osCount)).set("os_gdi_peak",count(res->osPeak)).set("other_types_left_out",res->ok ? Json(res->other) : Json()).set("note",txt(res->note));
		return merge(j,pageOf(res->rows.size(),a,500,5000,"objects",[&](size_t i) {
			const GdiObj& o = res->rows[i];
			char b[16];
			snprintf(b,sizeof(b),"0x%08x",o.handle);
			Json x = Json::obj();
			x.set("type",o.type).set("handle",b).set("object",util::hex(o.object)).set("info",txt(o.info));
			return x;
		}));
	}

	void refuseSystem(const ProcRef& r,const char* what) {
		if (r.pid == procs::be::selfPid()) throw std::runtime_error("refused, that is FRS itself");
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		if (row && row->sys) throw std::runtime_error(std::string("pid ") + std::to_string(r.pid) + " is a system process, it has no " + what);
		// heaps, locks and flush start a thread inside the target, one that crashes in winlogon or lsass bugchecks Windows
		std::string c = be::remote() ? vmem::be::critical(r) : "";
		if (!c.empty()) throw std::runtime_error("pid " + std::to_string(r.pid) + " : " + c);
	}

	Json heapDiag(const Json& a) {
		std::string view = textArg(a,"view","");
		if (view != "heaps" && view != "locks") throw std::runtime_error("view must be heaps or locks");
		numArg(a,"limit",0);
		ProcRef r = procs::target(a,true);
		refuseSystem(r,view == "heaps" ? "user heaps" : "user locks");
		return view == "heaps" ? heapsView(r,a) : locksView(r,a);
	}

	Json processDiag(const Json& a) {
		std::string view = textArg(a,"view","");
		if (view == "heaps" || view == "locks") throw std::runtime_error("view " + view + " moved to the heap_diag tool, it starts a thread inside the target. The user has to turn heap_diag on in the FRS MCP tab");
		if (view != "modified_pages" && view != "cfg" && view != "gdi") throw std::runtime_error("view must be modified_pages, cfg or gdi. Heaps and locks are in heap_diag");
		numArg(a,"limit",0);
		ProcRef r = procs::target(a);
		if (view == "modified_pages") return modView(r,a);
		if (view == "gdi") return gdiView(r,a);
		std::string addr = textArg(a,"address","");
		if (!addr.empty()) return cfgCheckView(r,addr);
		return cfgView(r,a);
	}

	Json flushHeaps(const Json& a) {
		ProcRef r = procs::target(a,true);
		refuseSystem(r,"user heaps");
		if (!claim(r.pid)) throw std::runtime_error("another query is still running in pid " + std::to_string(r.pid) + ", try again when it is done");
		auto out = std::make_shared<FlushRes>();
		uint32_t pid = r.pid;
		ProcRef rc = r;
		auto ran = std::make_shared<std::atomic<bool>>(false);
		auto t = task::run([out,rc,pid,ran](task::T&) {
			*ran = true;
			try {
				*out = be::flush(rc);
			} catch (...) {
				unclaim(pid);
				throw;
			}
			if (!out->held) unclaim(pid);
			return std::string();
		},false);
		// done without having run : no thread could be started, nothing else lets the process go
		if (t->done && !*ran) unclaim(pid);
		uint64_t end = util::nowMs() + 45000;
		while (!t->done) {
			if (!mcp::alive()) throw std::runtime_error("MCP server is stopping");
			if (util::nowMs() > end) {
				appLog("[mcp] Flush heaps (pid " + std::to_string(r.pid) + ") : failed (no answer in 45 s)");
				throw std::runtime_error("flush_heaps : no answer in 45 s, the target may be frozen");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		std::string line = "[mcp] Flush heaps (pid " + std::to_string(r.pid) + ")";
		if (out->na) {
			Json j = procs::refJson(r);
			j.set("ok",false).set("available",false).set("reason",out->why);
			j.set("private_before_bytes",Json()).set("private_after_bytes",Json()).set("working_set_before_bytes",Json()).set("working_set_after_bytes",Json());
			return j;
		}
		if (!out->ok) {
			std::string why = !out->why.empty() ? util::safe(out->why) : t->res.empty() ? std::string("unknown error") : util::safe(t->res);
			appLog(line + " : failed (" + why + ")");
			throw std::runtime_error("flush_heaps failed for pid " + std::to_string(r.pid) + " : " + why);
		}
		appLog(line + " : OK");
		Json j = procs::refJson(r);
		j.set("ok",true).set("available",true).set("reason",Json());
		j.set("private_before_bytes",mcp::bytes(out->privBefore)).set("private_after_bytes",mcp::bytes(out->privAfter));
		j.set("working_set_before_bytes",mcp::bytes(out->wsBefore)).set("working_set_after_bytes",mcp::bytes(out->wsAfter));
		return j;
	}

}

namespace vmdiag {

	void cacheTrim() {
		std::lock_guard<std::mutex> lk(g_cm);
		dropOld();
	}

	void regTools() {
		mcp::add("Virtual memory","process_diag",
			"Per process diagnostics : sizes, counts and addresses, never memory contents, nothing runs inside the target. Heaps and locks are in heap_diag. "
			"modified_pages lists image pages that no longer are the shared disk copy, a candidate and not proof of tampering. cfg decodes valid call targets, or checks one address. "
			"gdi objects live in session pool, not in process RAM, and os_gdi_objects can be above the rows. Paging with offset above 0 reuses the last answer for 2 min.",
			R"j({"type":"object","properties":{
				"pid":{"type":"integer","description":"target process"},
				"start_time":{"type":"string","description":"from list_processes, refuses a reused pid"},
				"view":{"type":"string","enum":["modified_pages","cfg","gdi"],"description":"what to report"},
				"cfg_show":{"type":"string","enum":["modified","image","all_valid","unmapped","all"],"description":"cfg : targets to list, default modified. image includes export suppressed ones, unmapped : bits for images not loaded here"},
				"address":{"type":"string","description":"cfg : check this one hex address instead of listing"},
				"limit":{"type":"integer","description":"rows per page"},
				"offset":{"type":"integer","description":"rows to skip"}},"required":["pid","view"]})j",mcp::Read,processDiag);
		mcp::add("Virtual memory","heap_diag",
			"Heaps or locks of one process : sizes, counts and addresses, no memory contents. On Windows it starts a thread inside the target, which can stall or in rare cases deadlock it, so only ask when you need it. "
			"Heap used is the blocks the program holds with headers and rounding, committed also holds free blocks. Locks only lists those with debug info. Paging with offset above 0 reuses the last answer for 2 min.",
			R"j({"type":"object","properties":{
				"pid":{"type":"integer","description":"target process"},
				"start_time":{"type":"string","description":"from list_processes, refuses a reused pid"},
				"view":{"type":"string","enum":["heaps","locks"],"description":"what to report"},
				"limit":{"type":"integer","description":"rows per page"},
				"offset":{"type":"integer","description":"rows to skip"}},"required":["pid","start_time","view"]})j",mcp::Danger,heapDiag);
		mcp::add("Virtual memory","flush_heaps",
			"Runs RtlFlushHeaps inside the target through a remote thread, so its heaps give free pages back. If a heap lock is held the target can stall or deadlock, in rare cases it crashes, "
			"anti-cheat or EDR may react. Most programs get little out of it. Returns private bytes and working set before and after.",
			R"j({"type":"object","properties":{
				"pid":{"type":"integer","description":"target process"},
				"start_time":{"type":"string","description":"from list_processes, refuses a reused pid"}},"required":["pid","start_time"]})j",mcp::Danger,flushHeaps);
	}

}
