#include "procs.h"
#include "cfg.h"
#include "gpumem.h"
#include "mcp.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

const ProcRow* ProcList::find(uint32_t pid) const {
	auto it = std::lower_bound(rows.begin(),rows.end(),pid,[](const ProcRow& r,uint32_t p) { return r.pid < p; });
	return it != rows.end() && it->pid == pid ? &*it : nullptr;
}

const ProcRow* ProcList::find(const ProcRef& r) const {
	const ProcRow* p = find(r.pid);
	return p && p->start == r.start ? p : nullptr;
}

namespace procs {

	const ColDef cols[COLS] = {
		{"Name","name","Process name.",KText,190,true,false,false},
		{"PID","pid","Process id. Gets reused, start_time tells two apart.",KOther,50,true,false,false},
		{"User","user","Account the process runs as.",KText,120,false,false,false},
#ifdef _WIN32
		{"Working set","working_set_bytes","Resident memory, includes shared pages, so a sum double counts them. A trim lowers this, the pages move to standby and fault back when used.",KSize,78,true,true,true},
#else
		{"Working set","working_set_bytes","Resident memory, includes shared pages, so a sum double counts them. A trim lowers this, file pages are dropped and anonymous ones go to swap, they fault back when used.",KSize,78,true,true,true},
#endif
		{"Private WS","private_working_set_bytes","Resident pages only this process uses.",KSize,74,true,true,true},
		{"Private bytes","private_bytes","Private commit. Includes pages not in RAM, so a trim does not lower it. Linux : RssAnon + VmSwap.",KSize,82,true,true,true},
		{"Private delta","private_delta_bytes","Private bytes change since the last sample, per tick, not per second.",KDelta,74,true,true,true},
		{"WS delta","working_set_delta_bytes","Working set change since the last sample, per tick.",KDelta,66,false,true,true},
		{"Faults delta","page_faults_delta","Page faults this tick. A spike after a trim is pages faulting back in.",KCountDelta,70,true,true,true},
		{"Hard delta","hard_faults_delta","Faults this tick that had to read from disk. Early warning of paging pressure.",KCountDelta,66,true,true,true},
		{"History",nullptr,"Private bytes over the last samples. Sorts by current private bytes.",KOther,84,true,false,false},
		{"Peak private","peak_private_bytes","Highest private bytes so far. The total adds peaks of different moments. Linux : highest FRS saw.",KSize,78,false,false,true},
		{"Peak WS","peak_working_set_bytes","Highest working set so far.",KSize,70,false,false,true},
		{"Shared WS","shared_working_set_bytes","Resident pages actually shared. Needs a page walk per process, runs in the background only while this column is on.",KSize,72,false,false,false},
		{"Shareable WS","shareable_working_set_bytes","Resident pages that could be shared. Page walk on Windows, like Shared WS. Linux : RssFile + RssShmem.",KSize,84,false,false,false},
		{"Virtual","virtual_bytes","Address space size, not memory in use.",KSize,70,false,true,false},
		{"Peak virtual","peak_virtual_bytes","Largest address space size so far.",KSize,78,false,false,false},
		{"Min WS","min_working_set_bytes","Soft minimum working set quota. Not on Linux.",KSize,62,false,false,false},
		{"Max WS","max_working_set_bytes","Soft maximum working set quota. Not on Linux.",KSize,62,false,false,false},
		{"Page faults","page_faults","Total faults since the process started.",KCount,76,false,true,true},
		{"Hard faults","hard_faults","Total faults that read from disk.",KCount,74,false,true,true},
		{"Page priority","page_priority","Memory priority of new pages. Low priority pages leave standby first. Not on Linux.",KOther,84,false,false,false},
		{"Paged pool","paged_pool_bytes","Paged pool quota charged, not all pool its kernel objects use. Not on Linux.",KSize,70,false,true,true},
		{"Peak paged","peak_paged_pool_bytes","Highest paged pool quota. Not on Linux.",KSize,70,false,false,true},
		{"Non-paged","nonpaged_pool_bytes","Non-paged pool quota charged. Handle leaks show here and a trim can't touch it. Not on Linux.",KSize,70,false,true,true},
		{"Peak non-paged","peak_nonpaged_pool_bytes","Highest non-paged pool quota. Not on Linux.",KSize,92,false,false,true},
		{"Shared commit","shared_commit_bytes","Commit charged for shared memory. Linux : RssShmem.",KSize,86,false,false,true},
		{"Job ID","job_id","Job object the process is in, blank when none. Linux : cgroup inode.",KOther,58,false,false,false},
		{"Subprocesses","children","Direct child processes.",KOther,80,false,false,false},
		{"Bits","bits","32 or 64 bit process.",KOther,40,false,false,false},
		{"GPU dedicated",nullptr,"VRAM resident now, not system RAM. N/A while GPU monitoring is off.",KSize,84,false,true,true},
		{"GPU shared",nullptr,"System RAM the GPU has resident. Already part of used RAM, never add it on top. N/A while GPU monitoring is off.",KSize,70,false,true,true},
		{"GPU dedicated committed",nullptr,"What Task Manager calls dedicated GPU memory, a different number from resident. N/A while GPU monitoring is off.",KSize,140,false,true,true},
		{"GPU shared committed",nullptr,"What Task Manager calls shared GPU memory, a different number from resident. N/A while GPU monitoring is off.",KSize,126,false,true,true},
		{"Path","path","Image file.",KText,260,false,false,false},
	};

	bool colNum(const ProcRow& r,int c,int64_t& v) {
		uint64_t u = NA;
		switch (c) {
			case CPid: v = r.pid; return true;
			case CWs: u = r.ws; break;
			case CWsPriv: u = r.wsPriv; break;
			case CPriv: case CHist: u = r.priv; break;
			case CDPriv: if (r.noPrev || r.priv == NA) return false; v = r.dPriv; return true;
			case CDWs: if (r.noPrev || r.ws == NA) return false; v = r.dWs; return true;
			case CDFaults: if (r.noPrev || r.faults == NA) return false; v = r.dFaults; return true;
			case CDHard: if (r.noPrev || r.hardFaults == NA) return false; v = r.dHard; return true;
			case CPrivPeak: u = r.privPeak; break;
			case CWsPeak: u = r.wsPeak; break;
			case CWsShared: u = r.wsShared; break;
			case CWsShareable: u = r.wsShareable; break;
			case CVirt: u = r.virt; break;
			case CVirtPeak: u = r.virtPeak; break;
			case CMinWs: u = r.minWs; break;
			case CMaxWs: u = r.maxWs; break;
			case CFaults: u = r.faults; break;
			case CHard: u = r.hardFaults; break;
			case CPrio: if (r.pagePrio < 0) return false; v = r.pagePrio; return true;
			case CPaged: u = r.paged; break;
			case CPagedPeak: u = r.pagedPeak; break;
			case CNonPaged: u = r.nonPaged; break;
			case CNonPagedPeak: u = r.nonPagedPeak; break;
			case CSharedCommit: u = r.sharedCommit; break;
			case CJob: u = r.jobId; break;
			case CKids: v = r.children; return true;
			case CBits: if (!r.bits) return false; v = r.bits; return true;
			case CGpuDed: u = r.gpuDed; break;
			case CGpuShared: u = r.gpuShared; break;
			case CGpuDedCommit: u = r.gpuDedCommit; break;
			case CGpuSharedCommit: u = r.gpuSharedCommit; break;
			default: return false;
		}
		if (u == NA) return false;
		v = (int64_t)u;
		return true;
	}

	std::string colText(const ProcRow& r,int c) {
		int64_t v = 0;
		bool ok = colNum(r,c,v);
		switch (c) {
			case CName: return r.name;
			case CUser: return r.user.empty() ? "N/A" : r.user;
			case CPath: return r.path.empty() ? "N/A" : r.path;
			case CPrio: return pagePrioName(r.pagePrio);
			case CJob: return !ok ? "N/A" : v ? util::count((uint64_t)v) : "";
			case CKids: return v ? std::to_string(v) : "";
			case CPid: case CBits: return ok ? std::to_string(v) : "N/A";
			case CHist: return "";
		}
		if (!ok) return "N/A";
		switch (cols[c].kind) {
			case KSize: return util::size((uint64_t)v);
			case KDelta: return util::delta(v);
			case KCount: return util::count((uint64_t)v);
			case KCountDelta: return v ? "+" + util::count((uint64_t)v) : "";
			default: return "";
		}
	}

	static int cmpText(const std::string& a,const std::string& b) {
		size_t n = std::min(a.size(),b.size());
		for (size_t i = 0; i < n; i++) {
			int x = (unsigned char)a[i];
			int y = (unsigned char)b[i];
			if (x >= 'A' && x <= 'Z') x += 32;
			if (y >= 'A' && y <= 'Z') y += 32;
			if (x != y) return x - y;
		}
		return (int)a.size() - (int)b.size();
	}

	static bool tieLess(const ProcRow& a,const ProcRow& b) {
		if (a.start != b.start) return a.start < b.start;
		if (a.pid != b.pid) return a.pid < b.pid;
		return a.name < b.name;
	}

	bool rowLess(const ProcRow& a,const ProcRow& b,int c,bool desc) {
		if (c == CName || c == CUser || c == CPath) {
			const std::string& x = c == CName ? a.name : c == CUser ? a.user : a.path;
			const std::string& y = c == CName ? b.name : c == CUser ? b.user : b.path;
			if (x.empty() != y.empty()) return y.empty();
			int d = cmpText(x,y);
			if (d) return desc ? d > 0 : d < 0;
			return tieLess(a,b);
		}
		if (c < 0) return a.pid != b.pid ? a.pid < b.pid : tieLess(a,b);
		int64_t x = 0,y = 0;
		bool kx = colNum(a,c,x);
		bool ky = colNum(b,c,y);
		if (kx != ky) return kx;
		if (kx && x != y) {
			// a zero delta says nothing, keep those last either way
			if (cols[c].kind == KDelta && (x == 0) != (y == 0)) return y == 0;
			return desc ? x > y : x < y;
		}
		return tieLess(a,b);
	}

	static bool ciHas(const std::string& hay,const std::string& low) {
		auto it = std::search(hay.begin(),hay.end(),low.begin(),low.end(),[](char p,char q) {
			if (p >= 'A' && p <= 'Z') p = (char)(p + 32);
			return p == q;
		});
		return it != hay.end();
	}

	bool rowMatch(const ProcRow& r,const std::string& low) {
		if (low.empty() || ciHas(r.name,low) || ciHas(r.user,low) || ciHas(r.path,low)) return true;
		return std::to_string(r.pid).find(low) != std::string::npos;
	}

	static void addU(uint64_t& a,uint64_t b) {
		if (b != NA) a = a == NA ? b : a + b;
	}

	void addSum(ProcRow& g,const ProcRow& r) {
		addU(g.priv,r.priv);
		addU(g.ws,r.ws);
		addU(g.wsPriv,r.wsPriv);
		addU(g.virt,r.virt);
		addU(g.faults,r.faults);
		addU(g.hardFaults,r.hardFaults);
		addU(g.paged,r.paged);
		addU(g.nonPaged,r.nonPaged);
		addU(g.gpuDed,r.gpuDed);
		addU(g.gpuShared,r.gpuShared);
		addU(g.gpuDedCommit,r.gpuDedCommit);
		addU(g.gpuSharedCommit,r.gpuSharedCommit);
		if (r.noPrev) return;
		g.noPrev = false;
		g.dPriv += r.dPriv;
		g.dWs += r.dWs;
		g.dFaults += r.dFaults;
		g.dHard += r.dHard;
	}

	const ProcRow* parentOf(const ProcList& l,const ProcRow& r) {
		if (r.pid == r.ppid) return nullptr;
		const ProcRow* p = l.find(r.ppid);
		// a younger process can't be the parent, that pid was reused. Strict order also rules out cycles.
		return p && (p->start < r.start || (p->start == r.start && p->pid < r.pid)) ? p : nullptr;
	}

}

namespace {

	using namespace procs;
	using Key = std::pair<uint32_t,uint64_t>;

	struct St {
		std::vector<float> priv,ws;
		int pos = 0; // oldest sample once the ring is full
		uint64_t seq = 0;
		uint64_t lastPriv = NA,lastWs = NA,lastFaults = NA,lastHard = NA;
		uint64_t peak = 0,seen = 0,hot = 0;
		std::shared_ptr<const ProcHist> hist;
		uint64_t histSeq = 0;
	};

	struct WsC {
		uint64_t shared,shareable;
	};

	std::mutex g_m; // everything below
	std::map<Key,St> g_st;
	std::map<Key,Info> g_info;
	std::map<Key,WsC> g_ws;
	std::vector<uint64_t> g_times;
	int g_tpos = 0;
	int g_cap = 0;
	uint64_t g_seq = 0;
	uint64_t g_lastMs = 0;
	std::string g_err;

	Snap<ProcList> g_list;
	std::atomic<uint64_t> g_wsT{0}; // walk until this util::nowMs
	std::atomic<uint64_t> g_fullT{0}; // full pace until this util::nowMs : a frame drew or an MCP call read processes
	std::atomic<bool> g_wsIn{false};
	uint64_t g_lastInfo = 0,g_infoSeq = 0; // info job only
	uint64_t g_wsSeq = 0; // ws job only

	template<class T> void ringPut(std::vector<T>& v,int pos,T x,int cap) {
		if ((int)v.size() < cap) v.push_back(x);
		else v[pos] = x;
	}

	template<class T> void ringFix(std::vector<T>& v,int pos,int cap) {
		std::rotate(v.begin(),v.begin() + pos,v.end());
		if ((int)v.size() > cap) v.erase(v.begin(),v.end() - cap);
	}

	uint64_t timeBack(int k) {
		int n = (int)g_times.size();
		return g_times[(g_tpos + n - 1 - k) % n];
	}

	void capChanged(int cap) {
		ringFix(g_times,g_tpos,cap);
		g_tpos = 0;
		for (auto& kv : g_st) {
			ringFix(kv.second.priv,kv.second.pos,cap);
			ringFix(kv.second.ws,kv.second.pos,cap);
			kv.second.pos = 0;
		}
		g_cap = cap;
	}

	int64_t diff(uint64_t now,uint64_t old) {
		return now == NA || old == NA ? 0 : (int64_t)now - (int64_t)old;
	}

	// Windows keeps fault counts in 32 bits, the low bits give the right delta across a wrap
	int64_t diffCount(uint64_t now,uint64_t old) {
		return now == NA || old == NA ? 0 : (int64_t)(uint32_t)(now - old);
	}

	void merge(ProcRow& r,const Info& i) {
		r.user = i.user;
		r.path = i.path;
		r.cgroup = i.cgroup;
		r.bits = i.bits;
		r.pagePrio = i.pagePrio;
		r.minWs = i.minWs;
		r.maxWs = i.maxWs;
		if (r.sharedCommit == NA) r.sharedCommit = i.sharedCommit;
		if (r.jobId == NA) r.jobId = i.jobId;
		r.noHandle = i.noHandle;
		r.noFull = i.noFull;
		r.filtered = i.filtered;
		r.osProc = i.osProc;
	}

	void until(std::atomic<uint64_t>& t,uint64_t ms) {
		uint64_t u = util::nowMs() + ms;
		uint64_t cur = t.load();
		while (cur < u && !t.compare_exchange_weak(cur,u)) {}
	}

	template<class M> void sweep(M& m) {
		for (auto it = m.begin(); it != m.end();) {
			if (g_st.count(it->first)) ++it;
			else it = m.erase(it);
		}
	}

	void tick() {
		if (g_seq && unseen() && util::nowMs() - g_lastMs < 4500) return;
		auto l = std::make_shared<ProcList>();
		std::string err;
		if (!be::snapshot(l->rows,err)) {
			std::lock_guard<std::mutex> lk(g_m);
			if (g_err.empty()) appLog("[!] Process snapshot : failed (" + err + ")");
			g_err = "Process snapshot : failed (" + err + ")";
			return;
		}
		auto& rows = l->rows;
		std::sort(rows.begin(),rows.end(),[](const ProcRow& a,const ProcRow& b) { return a.pid < b.pid; });
		uint64_t now = util::nowMs();
		int cap = std::max(60,std::min(300,cfg::geti("memHistN",600)));
		double hotBps = cfg::geti("procHlGrowthMb",100) * 1048576.0;
		auto gpu = gpumem::latest();

		std::lock_guard<std::mutex> lk(g_m);
		g_err.clear();
		bool first = g_seq == 0;
		l->dtMs = first ? 0 : now - g_lastMs;
		g_lastMs = now;
		if (g_cap != cap) capChanged(cap);
		g_seq++;
		bool tFull = (int)g_times.size() == cap;
		ringPut<uint64_t>(g_times,g_tpos,(uint64_t)time(nullptr),cap);
		if (tFull) g_tpos = (g_tpos + 1) % cap;

		for (auto& r : rows) {
			Key k(r.pid,r.start);
			auto it = g_st.find(k);
			bool fresh = it == g_st.end();
			if (fresh) it = g_st.emplace(k,St()).first;
			St& s = it->second;
			s.seq = g_seq;
			if (fresh) {
				s.seen = first ? 0 : now;
				r.isNew = !first;
			} else {
				r.noPrev = false;
				r.dPriv = diff(r.priv,s.lastPriv);
				r.dWs = diff(r.ws,s.lastWs);
				r.dFaults = diffCount(r.faults,s.lastFaults);
				r.dHard = diffCount(r.hardFaults,s.lastHard);
				double up = (double)std::max(r.dPriv,r.dWs);
				if (l->dtMs && up * 1000.0 / (double)l->dtMs >= hotBps) s.hot = now;
			}
			if (r.priv != NA) s.lastPriv = r.priv;
			if (r.ws != NA) s.lastWs = r.ws;
			if (r.faults != NA) s.lastFaults = r.faults;
			if (r.hardFaults != NA) s.lastHard = r.hardFaults;
			r.seenMs = s.seen;
			r.hotMs = s.hot;
			if (r.priv != NA) s.peak = std::max(s.peak,r.priv);
			if (r.privPeak == NA && r.priv != NA) r.privPeak = s.peak;

			// never known (Linux kernel thread) : no sample, an empty ring reads as unknown, a 0 would not
			if (s.lastPriv != NA && s.lastWs != NA) {
				bool full = (int)s.priv.size() == cap;
				ringPut(s.priv,s.pos,util::toMbf(s.lastPriv),cap);
				ringPut(s.ws,s.pos,util::toMbf(s.lastWs),cap);
				if (full) s.pos = (s.pos + 1) % cap;
			}

			auto ii = g_info.find(k);
			if (ii != g_info.end()) merge(r,ii->second);
			auto wi = g_ws.find(k);
			if (wi != g_ws.end()) {
				r.wsShared = wi->second.shared;
				if (wi->second.shareable != NA) r.wsShareable = wi->second.shareable;
			}
			GpuProc gp = gpu->now(ref(r));
			r.gpuDed = gp.ded;
			r.gpuShared = gp.shared;
			r.gpuDedCommit = gp.dedCommit;
			r.gpuSharedCommit = gp.sharedCommit;
		}

		for (auto it = g_st.begin(); it != g_st.end();) {
			if (it->second.seq == g_seq) ++it;
			else it = g_st.erase(it);
		}
		sweep(g_info);
		sweep(g_ws);

		for (auto& r : rows) {
			if (const ProcRow* p = parentOf(*l,r)) rows[p - rows.data()].children++;
			if (r.pseudo) continue;
			if (r.ws != NA) l->sumWs += r.ws;
			if (r.priv != NA) l->sumPriv += r.priv;
		}
		l->t = now;
		l->seq = g_seq;
		g_list.put(l);
	}

	// paused or unseen : results merge only at a tick, so the jobs run once per snapshot, not on their timer
	bool idle(const ProcList& l,uint64_t& seen) {
		if ((cfg::getb("memPaused",false) || unseen()) && l.seq == seen) return true;
		seen = l.seq;
		return false;
	}

	// path, user, bits once per process (Linux every 3 s, exec and setuid change them), page priority and quota every 3 s
	void infoPass() {
		auto l = latest();
		if (idle(*l,g_infoSeq)) return;
		uint64_t now = util::nowMs();
		bool full = now - g_lastInfo >= 3000;
		if (full) g_lastInfo = now;
		for (auto& r : l->rows) {
			if (sampler::stopping()) return;
			Key k(r.pid,r.start);
			Info i;
			{
				std::lock_guard<std::mutex> lk(g_m);
				auto it = g_info.find(k);
				if (it != g_info.end()) {
					if (!full && it->second.statics) continue;
					i = it->second;
				}
			}
			be::info(r,i);
			std::lock_guard<std::mutex> lk(g_m);
			if (g_st.count(k)) g_info[k] = std::move(i);
		}
	}

	bool wsWanted() {
		return util::nowMs() < g_wsT.load();
	}

	// only while someone looks at the columns, the walk reads every resident page entry
	void wsPass() {
		auto l = latest();
		if (idle(*l,g_wsSeq)) return;
		if (!wsWanted()) {
			g_wsIn = false;
			be::wsFree();
			std::lock_guard<std::mutex> lk(g_m);
			g_ws.clear();
			return;
		}
		for (auto& r : l->rows) {
			if (sampler::stopping() || !wsWanted()) return;
			if (r.pseudo) continue;
			uint64_t sh = NA,sa = NA;
			bool ok = be::wsCounters(r,sh,sa);
			std::lock_guard<std::mutex> lk(g_m);
			Key k(r.pid,r.start);
			if (ok && g_st.count(k)) g_ws[k] = WsC{sh,sa};
			else g_ws.erase(k);
		}
		g_wsIn = true;
	}

	std::string refuse(const ProcRow* row) {
		if (!row) return "process exited";
		if (row->pseudo) return "pseudo process, it has no memory of its own";
		if (row->sys) return "system process, refused";
#ifndef _WIN32
		if (row->pid == 1) return "init is refused";
#endif
		return "";
	}

	bool parseDec(const std::string& s,uint64_t& out) {
		if (s.empty() || s.size() > 20) return false;
		uint64_t v = 0;
		for (char c : s) {
			if (c < '0' || c > '9') return false;
			uint64_t d = (uint64_t)(c - '0');
			if (v > (UINT64_MAX - d) / 10) return false;
			v = v * 10 + d;
		}
		out = v;
		return true;
	}

}

namespace procs {

	void init() {
		cfg::optBool("procHlNew","Processes","Highlight new processes","Tints a row for a moment when the process appears.",true);
		cfg::optBool("procHlGrowth","Processes","Highlight fast growth","Tints a row when private bytes or working set jump past the threshold below.",true);
		cfg::optInt("procHlGrowthMb","Processes","Growth threshold, MB/s","Growth rate that counts as a jump.",100,1,10000);
		cfg::optInt("procHlMs","Processes","Highlight time, ms","How long a highlight stays.",3000,250,60000);
		cfg::optBool("procTotals","Processes","Column totals","A row under the header with the sum of what the filter shows. Working set sums double count shared pages.",true);
		cfg::optBool("procAggregate","Processes","Collapsed parent shows its subtree","Tree view : a collapsed parent adds up its hidden children. Peaks are not added.",false);
		initPage();
		procwin::addFloat([] { until(g_fullT,3000); });
		sampler::add("procs",0,tick);
		sampler::add("procs-info",1000,infoPass);
		sampler::add("procs-ws",3000,wsPass);
	}

	std::shared_ptr<const ProcList> latest() {
		auto p = g_list.get();
		if (p) return p;
		static auto e = std::make_shared<const ProcList>();
		return e;
	}

	std::shared_ptr<const ProcHist> histOf(uint32_t pid,uint64_t start) {
		std::lock_guard<std::mutex> lk(g_m);
		auto it = g_st.find(Key(pid,start));
		if (it == g_st.end()) return nullptr;
		St& s = it->second;
		if (s.hist && s.histSeq == g_seq) return s.hist;
		auto h = std::make_shared<ProcHist>();
		int n = (int)s.priv.size();
		h->privMb.resize(n);
		h->wsMb.resize(n);
		h->unixSec.resize(n);
		for (int i = 0; i < n; i++) {
			int k = (s.pos + i) % n;
			h->privMb[i] = s.priv[k];
			h->wsMb[i] = s.ws[k];
			h->unixSec[i] = timeBack(n - 1 - i);
		}
		s.hist = h;
		s.histSeq = g_seq;
		return h;
	}

	std::shared_ptr<const ProcHist> history(const ProcRef& r) {
		return histOf(r.pid,r.start);
	}

	bool alive(const ProcRef& r) {
		return latest()->find(r) != nullptr;
	}

	ProcRef ref(const ProcRow& r) {
		ProcRef x;
		x.pid = r.pid;
		x.start = r.start;
		x.name = r.name;
		return x;
	}

	void wantWs(uint64_t ms) {
		until(g_wsT,ms);
	}

	bool wsLive() {
		return g_wsIn;
	}

	std::vector<Growth> growth(const ProcList& l,int windowSec) {
		std::vector<Growth> out(l.rows.size());
		std::lock_guard<std::mutex> lk(g_m);
		int nt = (int)g_times.size();
		if (!nt) return out;
		int back = 0;
		uint64_t t0 = timeBack(0);
		while (back + 1 < nt && t0 - timeBack(back + 1) <= (uint64_t)windowSec) back++;
		for (size_t i = 0; i < l.rows.size(); i++) {
			auto it = g_st.find(Key(l.rows[i].pid,l.rows[i].start));
			if (it == g_st.end() || it->second.priv.empty()) continue;
			const St& s = it->second;
			int n = (int)s.priv.size();
			int k = std::min(back,n - 1);
			int newest = (s.pos + n - 1) % n;
			int oldest = (s.pos + n - 1 - k) % n;
			Growth& g = out[i];
			g.n = k + 1;
			g.priv = ((double)s.priv[newest] - (double)s.priv[oldest]) * 1048576.0;
			g.ws = ((double)s.ws[newest] - (double)s.ws[oldest]) * 1048576.0;
			g.secs = t0 - timeBack(k);
		}
		return out;
	}

	std::string snapErr() {
		std::lock_guard<std::mutex> lk(g_m);
		return g_err;
	}

	bool unseen() {
		return util::nowMs() >= g_fullT.load();
	}

	void waitFirst() {
		bool wake = unseen();
		until(g_fullT,60000);
		// the slow pace leaves the list up to 5 s old, a pid spawned just now would be missing
		uint64_t s = latest()->seq;
		if (wake && s && !cfg::getb("memPaused",false)) {
			sampler::kick();
			for (int i = 0; i < 100 && latest()->seq == s && mcp::alive(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		for (int i = 0; i < 100 && latest()->seq == 0 && mcp::alive(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if (latest()->seq) return;
		std::string e = snapErr();
		throw std::runtime_error(!e.empty() ? e : cfg::getb("memPaused",false) ? "no process snapshot yet, sampling is paused in Settings" : "no process snapshot yet, call again in a moment");
	}

	ProcRef target(const Json& a,bool needStart) {
		const Json* p = a.get("pid");
		if (!p || p->t != Json::Num || p->n < 0 || p->n > 4294967295.0 || p->n != std::floor(p->n)) throw std::runtime_error("pid is required, a whole number. list_processes gives it");
		uint32_t pid = (uint32_t)p->n;
		uint64_t start = 0;
		const Json* st = a.get("start_time");
		bool has = st && st->t != Json::Null;
		if (has && (st->t != Json::Str || !parseDec(st->s,start))) throw std::runtime_error("start_time must be a decimal string as list_processes returns it, numbers lose digits");
		if (!has && needStart) throw std::runtime_error("start_time is required here, it stops a reused pid from hitting the wrong process. list_processes gives it");
		waitFirst();
		auto l = latest();
		const ProcRow* r = l->find(pid);
		std::string ps = std::to_string(pid);
		if (!r) {
			std::string e = snapErr();
			throw std::runtime_error("no process with pid " + ps + (e.empty() ? ", it exited or the pid is wrong. list_processes shows what runs" : " in the last good snapshot. " + e));
		}
		if (has && r->start != start) throw std::runtime_error("pid " + ps + " was reused, start_time does not match. Call list_processes again");
		if (r->pseudo) throw std::runtime_error("pid " + ps + " is a pseudo process, it has no memory of its own");
		return ref(*r);
	}

	Json refJson(const ProcRef& r) {
		Json j = Json::obj();
		j.set("pid",r.pid).set("name",r.name).set("start_time",std::to_string(r.start));
		return j;
	}

	std::string reduceWs(const ProcRef& r) {
		std::string lb = "Reduce working set (pid " + std::to_string(r.pid) + ")";
		auto l = latest();
		const ProcRow* row = l->find(r);
		std::string why = refuse(row);
		if (why.empty()) why = be::reduceWs(*row);
		return lb + (why.empty() ? " : OK" : " : failed (" + why + ")");
	}

	std::string setPagePrio(const ProcRef& r,int prio,int* prev) {
		if (prev) *prev = -1;
		std::string lb = "Page priority (pid " + std::to_string(r.pid) + ")";
		auto l = latest();
		const ProcRow* row = l->find(r);
		std::string why = refuse(row);
		if (why.empty() && (prio < 0 || prio > 7)) why = "priority must be 0 to 7";
		if (why.empty()) why = be::setPagePrio(*row,prio,prev);
		if (!why.empty()) return lb + " : failed (" + why + ")";
		std::lock_guard<std::mutex> lk(g_m);
		auto it = g_info.find(Key(r.pid,r.start));
		if (it != g_info.end()) it->second.pagePrio = prio;
		return lb + " : OK";
	}

	const char* pagePrioName(int p) {
		static const char* n[] = {"Lowest","Very low","Low","Medium","Below normal","Normal","6","7"};
		return p >= 0 && p < 8 ? n[p] : "N/A";
	}

}
