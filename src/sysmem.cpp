#include "sysmem.h"
#include "smbios.h"
#include "procs.h"
#include "cfg.h"
#include "ui.h"
#include "mcp.h"
#include "ram.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#ifdef _WIN32
#define OS_TIP(win,lx) win
#else
#define OS_TIP(win,lx) lx
#endif

namespace sysmem {

	const Cmd kCmds[] = {
		{"combine_pages","Combine memory pages","Combining memory pages...",OS_TIP("Finds identical physical pages and shares one copy. Slow on big RAM.","Starts KSM and leaves it on. Only memory apps marked MADV_MERGEABLE merges, it takes a while.")},
		{"compression","Empty compression cache","Emptying compression store...",OS_TIP("Trims the Memory Compression process. It is protected, so without a kernel driver this normally reports not available and says why.","Compacts every zram device. Not available without zram, zswap has no such command.")},
		{"file_cache","Empty system file cache","Emptying system file cache...",OS_TIP("Trims the system cache working set. The pages go to standby, empty standby too to really free them.","Runs sync, then drops the clean page cache. Apps read it back from disk when they need it.")},
		{"registry_cache","Empty registry cache","Emptying registry cache...",OS_TIP("Flushes registry hives to disk, may free little.","Linux has no registry, this reports not available.")},
		{"empty_working_sets","Empty working sets","Emptying working sets...",OS_TIP("Trims every process in one call. Pages move to standby or modified and fault back on use, expect a burst of page faults. Empty Working Sets on the Dashboard does about the same, process by process.","Pages out every process it may touch, needs kernel 5.10. Pages fault back on use, expect a burst of page faults. Same as Empty Working Sets on the Dashboard.")},
		{"modified","Empty modified page list","Emptying modified page list...",OS_TIP("Writes modified pages out, they become standby. Can take a while on a slow disk. Same as Flush Modified on the Dashboard.","Runs sync. Dirty pages get written out and stay as page cache. Can take a while on a slow disk. Same as Flush Modified on the Dashboard.")},
		{"modified_file_cache","Empty modified file cache","Emptying modified file cache...",OS_TIP("Flushes dirty file cache on every volume.","Runs syncfs on every block device and ZFS mount, or plain sync when there is none.")},
		{"standby","Empty standby list","Emptying standby list...",OS_TIP("Discards all standby pages. Standby is cache, not lost memory, apps read it back from disk when they need it. Same as Purge Standby on the Dashboard.","Runs sync, then drops the clean page cache. It is cache, not lost memory, apps read it back from disk when they need it. Same as Purge Standby on the Dashboard.")},
		{"standby_low","Empty priority 0 standby list","Emptying priority 0 standby list...",OS_TIP("Discards only the lowest priority standby pages, the gentlest purge.","Linux has no standby priorities, this reports not available.")}
	};
	const int kCmdN = 9;

	namespace {

		Snap<SysMem> g_mem;
		Snap<SysHist> g_hist;
		std::mutex g_runM;
		std::shared_ptr<Run> g_run;

		uint64_t rate(uint64_t cur,uint64_t prev,double sec) {
			if (cur == NA || prev == NA || cur < prev || sec <= 0.0) return NA;
			return (uint64_t)((double)(cur - prev) / sec + 0.5);
		}

		void fillDeltas(SysMem& m,const SysMem& p) {
			if (!p.t || m.t <= p.t) return;
			double s = (double)(m.t - p.t) / 1000.0;
			m.dPagedAllocs = rate(m.cPagedAllocs,p.cPagedAllocs,s);
			m.dPagedFrees = rate(m.cPagedFrees,p.cPagedFrees,s);
			m.dNonPagedAllocs = rate(m.cNonPagedAllocs,p.cNonPagedAllocs,s);
			m.dNonPagedFrees = rate(m.cNonPagedFrees,p.cNonPagedFrees,s);
			m.dFaults = rate(m.cFaults,p.cFaults,s);
			m.dPageReads = rate(m.cPageReads,p.cPageReads,s);
			m.dPfWrites = rate(m.cPfWrites,p.cPfWrites,s);
			m.dMappedWrites = rate(m.cMappedWrites,p.cMappedWrites,s);
			m.dMappedReadBytes = rate(m.cMappedReadBytes,p.cMappedReadBytes,s);
			m.dMappedWriteBytes = rate(m.cMappedWriteBytes,p.cMappedWriteBytes,s);
			if (m.pagedVs != NA && p.pagedVs != NA) {
				m.dPagedBytes = (int64_t)(((double)m.pagedVs - (double)p.pagedVs) / s);
				m.hasPagedDelta = true;
			}
			if (m.nonPaged != NA && p.nonPaged != NA) {
				m.dNonPagedBytes = (int64_t)(((double)m.nonPaged - (double)p.nonPaged) / s);
				m.hasNonPagedDelta = true;
			}
		}

		uint64_t sumKnown(std::initializer_list<uint64_t> l) {
			uint64_t s = NA;
			for (uint64_t v : l) if (v != NA) s = (s == NA ? 0 : s) + v;
			return s;
		}

		// The store query gives a ULONG working set and needs rights, the process row has neither problem
		void fillComp(SysMem& m,const ProcList& pl) {
			if (!m.compPid) for (auto& r : pl.rows) if (r.memCompression) m.compPid = r.pid;
			const ProcRow* r = m.compPid ? pl.find(m.compPid) : nullptr;
			if (r && r->ws != NA && (m.compWs == NA || r->ws > m.compWs)) m.compWs = r->ws;
			else if (m.compPid && pl.rows.empty()) m.compWs = NA; // 32 bit store working set, no process row yet to fix it
		}

		void pushHist(const SysMem& m,const ProcList& pl) {
			auto cur = g_hist.get();
			auto h = cur ? std::make_shared<SysHist>(*cur) : std::make_shared<SysHist>();
			size_t cap = (size_t)std::max(60,std::min(3600,cfg::geti("memHistN",600)));
			auto add = [cap](auto& v,auto x) {
				v.push_back(std::move(x));
				if (v.size() > cap) v.erase(v.begin(),v.end() - cap);
			};
			add(h->commit,util::toMbf(m.commit));
			add(h->phys,util::toMbf(m.physUsed));
			add(h->cache,util::toMbf(m.cacheWs));
			add(h->kernel,util::toMbf(sumKnown({m.pagedWs,m.nonPaged,m.kernelWs,m.driverWs})));
			add(h->standby,util::toMbf(m.standby));
			add(h->modified,util::toMbf(m.modified));
			add(h->freeMem,util::toMbf(sumKnown({m.free,m.zeroed})));
			add(h->unixSec,(uint64_t)std::time(nullptr));
			const ProcRow* tw = nullptr;
			const ProcRow* tp = nullptr;
			for (auto& r : pl.rows) {
				if (r.pseudo) continue;
				if (r.ws != NA && (!tw || r.ws > tw->ws)) tw = &r;
				if (r.priv != NA && (!tp || r.priv > tp->priv)) tp = &r;
			}
			add(h->topProc,tw ? tw->name : std::string());
			add(h->topProcBytes,tw ? tw->ws : NA);
			add(h->topPriv,tp ? tp->name : std::string());
			add(h->topPrivBytes,tp ? tp->priv : NA);
			g_hist.put(h);
		}

		// fills what osRead does not : used, reserved, compression fallback. Deltas when prev is given.
		std::shared_ptr<const SysMem> readNow(const SysMem* prev,const ProcList& pl) {
			auto m = std::make_shared<SysMem>();
			m->t = util::nowMs();
			osRead(*m);
			if (m->physTotal != NA && m->physAvail != NA) m->physUsed = m->physTotal > m->physAvail ? m->physTotal - m->physAvail : 0;
			if (m->physInstalled != NA && m->physTotal != NA) m->reserved = m->physInstalled > m->physTotal ? m->physInstalled - m->physTotal : 0;
			fillComp(*m,pl);
			if (prev) fillDeltas(*m,*prev);
			return m;
		}

		void tick() {
			smbios::load();
			auto pl = procs::latest();
			auto prev = g_mem.get();
			auto m = readNow(prev.get(),*pl);
			g_mem.put(m);
			pushHist(*m,*pl);
		}

		// FRS_TEST_FAKECMD : ok, slow, fail, na, cleaning. Commands then never reach the OS, for test runs.
		// Only in an unelevated Windows exe, that is a private test build. The shipped one has the admin manifest, so it can't fake a result.
		// FRS_TEST_NOACTIVATE counts as ok, a stray click in a visible test window can't run a real system wide command
		const char* fakeMode() {
#ifdef _WIN32
			static const char* e = isElevated() ? nullptr : util::testEnv("FRS_TEST_FAKECMD") ? util::testEnv("FRS_TEST_FAKECMD") : util::testEnv("FRS_TEST_NOACTIVATE") ? "ok" : nullptr;
			return e && *e ? e : nullptr;
#else
			return nullptr;
#endif
		}

		// the clean set fakes in every test exe, like the Dashboard methods it runs next to (app.cpp runClean), elevated or not
		const char* stepFake() {
			if (const char* m = fakeMode()) return m;
			const char* e = util::testEnv("FRS_TEST_FAKECMD");
			if (e && *e) return e;
			return util::testEnv("FRS_TEST_NOACTIVATE") ? "ok" : nullptr;
		}

		CmdRes fakeCmd(int i,const std::string& mode) {
			std::this_thread::sleep_for(std::chrono::milliseconds(mode == "slow" ? 1500 : 150));
			CmdRes r;
			r.na = mode == "na" && (i == 3 || i == 8);
			r.ok = mode != "fail" && !r.na;
			r.why = r.ok ? "" : "fake : A required privilege is not held by the client.";
			r.msg = r.ok ? std::string("(fake) ") + kCmds[i].label + " done." : std::string("(fake) ") + kCmds[i].label + " : unable. " + r.why;
			if (i == 6) {
				r.items.push_back({"C:\\","Flushed"});
				r.items.push_back({"E:\\",r.ok ? "Skipped, no media" : "Access is denied."});
			}
			return r;
		}

		void setStep(Run& run,int i,int st,const CmdRes* res = nullptr) {
			std::lock_guard<std::mutex> lk(run.m);
			run.steps[i].st = st;
			if (res) run.steps[i].res = *res;
		}

		// something threw : nothing below keeps running, so no step may stay in running or waiting
		void abortSteps(Run& run,const std::string& why) {
			std::lock_guard<std::mutex> lk(run.m);
			for (auto& s : run.steps) {
				if (s.st > 1) continue;
				s.st = 3;
				s.res.why = why;
				s.res.msg = s.label + " did not run. " + why;
			}
		}

		int stepSt(const CmdRes& r) {
			return r.ok ? 2 : r.na ? 5 : 3;
		}

		std::string logTail(const CmdRes& r) {
			return r.ok ? " : OK" : (r.na ? " : not available (" : " : failed (") + util::safe(r.why) + ")";
		}

		void body(const std::shared_ptr<Run>& run,const std::string& pfx,task::T& t) {
			auto pl = procs::latest();
			run->before = readNow(nullptr,*pl);
			int nFail = 0;
			int nNa = 0;
			int nDone = 0;
			bool cancelled = false;
			int n = (int)run->steps.size();
			for (int i = 0; i < n; i++) {
				if (run->cancel || t.cancel) {
					cancelled = true;
					for (int k = i; k < n; k++) setStep(*run,k,4);
					break;
				}
				int idx = -1;
				for (int k = 0; k < kCmdN; k++) if (run->steps[i].id == kCmds[k].id) idx = k;
				setStep(*run,i,1);
				CmdRes r = fakeMode() ? fakeCmd(idx,fakeMode()) : osCmd(idx);
				setStep(*run,i,stepSt(r),&r);
				nDone++;
				run->nDone = nDone;
				if (r.na) nNa++;
				else if (!r.ok) nFail++;
				appLog(pfx + run->steps[i].label + logTail(r));
			}
			if (run->id == "all" && !cancelled && !fakeMode()) {
				CmdRes r;
				if (osCompact(r)) {
					std::lock_guard<std::mutex> lk(run->m);
					Step s;
					s.id = "compact_memory";
					s.label = "Compact memory";
					s.st = stepSt(r);
					s.res = r;
					run->steps.push_back(s);
					if (r.na) nNa++;
					else if (!r.ok) nFail++;
					appLog(pfx + s.label + logTail(r));
				}
			}
			run->after = readNow(nullptr,*procs::latest());
			if (run->id == "all") {
				std::string line = "Empty all : ";
				line += cancelled ? "cancelled after " + std::to_string(nDone) + " commands" : nFail ? "failed (" + std::to_string(nFail) + " of " + std::to_string(nDone) + " commands, see above)" : nNa ? "OK, " + std::to_string(nNa) + " not available" : "OK";
				appLog(pfx + line);
				ui::toast(line);
			} else {
				std::lock_guard<std::mutex> lk(run->m);
				const Step& s = run->steps[0];
				ui::toast(s.label + (s.st == 2 ? " : OK" : s.st == 5 ? " : not available" : " : failed"));
			}
		}

	}

	std::shared_ptr<const SysMem> latest() {
		static auto first = std::make_shared<const SysMem>();
		auto p = g_mem.get();
		return p ? p : first;
	}

	std::shared_ptr<const SysHist> history() {
		static auto first = std::make_shared<const SysHist>();
		auto p = g_hist.get();
		return p ? p : first;
	}

	std::shared_ptr<const SysMem> sampleNow() {
		return readNow(nullptr,*procs::latest());
	}

	std::shared_ptr<Run> lastRun() {
		std::lock_guard<std::mutex> lk(g_runM);
		return g_run;
	}

	bool busy() {
		auto r = lastRun();
		return r && !r->done;
	}

	std::string runStep(const char* id) {
		int idx = -1;
		for (int i = 0; i < kCmdN; i++) if (!strcmp(id,kCmds[i].id)) idx = i;
		if (idx < 0) return util::safe(id) + " : failed (unknown command)";
		std::string lbl = kCmds[idx].label;
		try {
			const char* f = stepFake();
			return lbl + logTail(f ? fakeCmd(idx,f) : osCmd(idx));
		} catch (std::exception& e) {
			return lbl + " : failed (" + util::safe(e.what()) + ")";
		} catch (...) {
			return lbl + " : failed (unknown error)";
		}
	}

	std::string start(const std::string& id,bool fromMcp,std::shared_ptr<Run>* out) {
		const char* fake = fakeMode();
		if (appCleaning() || (fake && !strcmp(fake,"cleaning"))) return "a clean is running, try again when it ends";
		int idx = -1;
		for (int i = 0; i < kCmdN; i++) if (id == kCmds[i].id) idx = i;
		if (idx < 0 && id != "all") return "unknown command";
#ifdef _WIN32
		if (id == "all" && !fake && !isElevated()) return "Empty all needs admin, nothing was run";
#else
		if (id == "all" && !fake && !isElevated()) return "Empty all needs root, nothing was run";
#endif
		std::lock_guard<std::mutex> lk(g_runM);
		if (g_run && !g_run->done) return fromMcp ? "another memory command is still running, memory_details last_command shows its progress, try again when it ends" : "another memory command is still running, try again when it ends";
		auto run = std::make_shared<Run>();
		run->id = id;
		for (int i = 0; i < kCmdN; i++) {
			if (idx >= 0 && i != idx) continue;
			Step s;
			s.id = kCmds[i].id;
			s.label = kCmds[i].label;
			run->steps.push_back(s);
		}
		std::string pfx = fromMcp ? "[mcp] " : "";
		auto t = task::run([run,pfx](task::T& tk) {
			run->began = true;
			try {
				body(run,pfx,tk);
			} catch (std::exception& e) {
				appLog(pfx + "Memory command : failed (" + util::safe(e.what()) + ")");
				abortSteps(*run,util::safe(e.what()));
			} catch (...) {
				appLog(pfx + "Memory command : failed (unknown error)");
				abortSteps(*run,"unknown error");
			}
			run->done = true;
			return std::string();
		},false);
		if (t->done && !run->began) {
			run->done = true;
			return t->res.empty() ? "could not start the command" : t->res;
		}
		g_run = run;
		if (out) *out = run;
		return "";
	}

	bool bounded(std::function<std::string()> fn,int ms,std::string& out) {
		struct S {
			std::atomic<bool> done{false};
			std::string res;
		};
		auto s = std::make_shared<S>();
		std::thread th([s,fn] {
			try {
				s->res = fn();
			} catch (...) {
				s->res = "failed";
			}
			s->done = true;
		});
		uint64_t end = util::nowMs() + (uint64_t)ms;
		while (!s->done && util::nowMs() < end) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if (!s->done) {
			th.detach();
			return false;
		}
		th.join();
		out = s->res;
		return true;
	}

	void init() {
		g_mem.put(std::make_shared<const SysMem>());
		g_hist.put(std::make_shared<const SysHist>());
		nav::addPage("memory","Memory",ui::IcoMem,nav::GrpMain,drawPage);
		sampler::add("sysmem",0,tick);
		regPalette();
		if (osArgSysinfo()) mcp::post([] { nav::go("memory"); });
	}

}
