#include "procdet.h"
#include "mcp.h"
#include "ram.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace {

	using namespace procdet;
	using mcp::textArg;
	using mcp::flagArg;

	Json by(uint64_t v) {
		return mcp::bytes(v);
	}

	Json txt(const std::string& s) {
		return s.empty() ? Json() : Json(s);
	}

	// a limit : {set:true,bytes:n}, {set:false,bytes:null} when readable and not set, {set:null,bytes:null} unknown
	Json lim(uint64_t v) {
		Json j = Json::obj();
		j.set("set",v == NA ? Json() : Json(v != NOLIM));
		j.set("bytes",v == NA || v == NOLIM ? Json() : Json(v));
		return j;
	}

	Json triState(int v) {
		return v < 0 ? Json() : Json(v != 0);
	}

	bool intArg(const Json& a,const char* k,int lo,int hi,int& out) {
		const Json* v = a.get(k);
		if (!v || v->t == Json::Null) return false;
		if (v->t != Json::Num || v->n != std::floor(v->n) || v->n < lo || v->n > hi) throw std::runtime_error(std::string(k) + " must be a whole number from " + std::to_string(lo) + " to " + std::to_string(hi));
		out = (int)v->n;
		return true;
	}

	Json prioJson(int p) {
		return p < 0 ? Json() : Json(p);
	}

	Json prioName(int p) {
		return p < 0 ? Json() : Json(procs::pagePrioName(p));
	}

	Json jobJson(const Job& j) {
		Json o = Json::obj();
		o.set("name",txt(j.name)).set("found_through",txt(j.via));
		o.set("process_memory_limit",lim(j.procMem)).set("job_memory_limit",lim(j.jobMem));
		o.set("working_set_minimum",lim(j.wsMin)).set("working_set_maximum",lim(j.wsMax));
		o.set("peak_process_memory_bytes",by(j.peakProc)).set("peak_job_memory_bytes",by(j.peakJob));
		o.set("total_page_faults",by(j.faults)).set("active_processes",by(j.active));
		o.set("total_processes",by(j.total)).set("terminated_processes",by(j.term));
		o.set("error",txt(j.why));
		return o;
	}

	Json getProcess(const Json& a) {
		bool thr = flagArg(a,"include_threads",false);
		ProcRef r = procs::target(a);
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		if (!row) throw std::runtime_error("pid " + std::to_string(r.pid) + " just exited");
		Det d = readAll(*row,thr);
		Json j = Json::obj();
		j.set("pid",row->pid).set("name",row->name).set("start_time",std::to_string(row->start));
		j.set("ppid",row->ppid).set("session",row->session);
		j.set("user",txt(row->user)).set("path",txt(row->path)).set("command_line",txt(d.cmd));
		j.set("command_line_error",txt(d.cmdWhy));
		j.set("bits",row->bits ? Json(row->bits) : Json()).set("threads",row->threads);
		j.set("system",sysLike(*row));
		j.set("access_denied",d.denied).set("limited_access",d.noFull).set("access_error",txt(d.why));
		j.set("sample_age_ms",util::nowMs() - l->t).set("interval_ms",sampler::intervalMs()).set("deltas_cover_ms",l->dtMs);

		Json st = Json::obj();
		for (int c = 0; c < procs::COLS; c++) {
			if (!procs::cols[c].key || procs::cols[c].kind == procs::KText || c == procs::CPid || c == procs::CHist || c == procs::CBits || c == procs::CPrio || c == procs::CKids) continue;
			int64_t v = 0;
			if (c == procs::CWsShared && d.ws.shared != NA) st.set(procs::cols[c].key,d.ws.shared);
			else if (c == procs::CWsShareable && d.ws.shareable != NA) st.set(procs::cols[c].key,d.ws.shareable);
			else if (procs::colNum(*row,c,v)) st.set(procs::cols[c].key,v);
			else st.set(procs::cols[c].key,Json());
		}
		j.set("statistics",st);

		Json ws = Json::obj();
		ws.set("total_bytes",by(d.ws.total)).set("private_bytes",by(d.ws.priv != NA ? d.ws.priv : row->wsPriv));
		ws.set("shareable_bytes",by(d.ws.shareable)).set("shared_bytes",by(d.ws.shared));
		ws.set("locked_bytes",by(d.ws.locked)).set("modified_bytes",by(d.ws.modified)).set("original_bytes",by(d.ws.original));
		ws.set("anonymous_bytes",by(d.ws.anon)).set("highest_page_priority",prioJson(d.ws.prio));
		ws.set("private_from",d.ws.priv != NA ? Json("page walk") : row->wsPriv != NA ? Json("snapshot") : Json());
		ws.set("error",txt(d.ws.why));
		j.set("working_set",ws);

		Json cm = Json::obj();
		cm.set("private_commit_bytes",by(d.commit.priv)).set("peak_private_commit_bytes",by(d.commit.peak));
		cm.set("shared_commit_bytes",by(d.commit.shared));
		cm.set("private_commit_limit",lim(d.commit.privLimit)).set("total_commit_limit",lim(d.commit.totalLimit));
#ifdef _WIN32
		cm.set("scope",d.jobs.in == 1 ? Json("whole job") : Json()).set("source",Json());
#else
		cm.set("scope",d.jobs.in == 1 ? Json("whole cgroup") : Json());
		cm.set("source","cgroup v2 files, not commit : private_commit_bytes memory.current, peak memory.peak, private_commit_limit memory.max, total_commit_limit memory.swap.max (a swap only cap), shared_commit_bytes shmem of memory.stat");
#endif
		cm.set("error",txt(d.commit.why));
		j.set("commit",cm);

		Json q = Json::obj();
		q.set("min_working_set_bytes",by(d.quota.minWs != NA ? d.quota.minWs : row->minWs));
		q.set("max_working_set_bytes",by(d.quota.maxWs != NA ? d.quota.maxWs : row->maxWs));
		q.set("hard_minimum",triState(d.quota.hardMin)).set("hard_maximum",triState(d.quota.hardMax));
		q.set("paged_pool_limit",lim(d.quota.paged)).set("nonpaged_pool_limit",lim(d.quota.nonPaged));
		q.set("page_file_limit",lim(d.quota.pagefile)).set("error",txt(d.quota.why));
		j.set("quota",q);

		Json jb = Json::obj();
		jb.set("in_job",triState(d.jobs.in)).set("job_id",by(row->jobId));
		Json list = Json::arr();
		for (auto& x : d.jobs.jobs) list.push(jobJson(x));
		jb.set("jobs",list).set("note",txt(d.jobs.why));
#ifdef _WIN32
		jb.set("source",Json());
#else
		jb.set("source","cgroup v2 files : job_memory_limit memory.max, working_set_minimum memory.min, working_set_maximum memory.high (a throttle), peak_job_memory_bytes memory.peak, total_page_faults pgfault of memory.stat, process_memory_limit RLIMIT_AS (address space)");
#endif
		j.set("job",jb);

		int pp = d.denied ? row->pagePrio : be::pagePrio(*row);
		j.set("page_priority",prioJson(pp)).set("page_priority_name",prioName(pp));
		std::string cmd = d.cmd;
		Rule ru;
		if (ruleFor(*row,cmd,ru)) {
			Json x = Json::obj();
			x.set("kind",ruleKind(ru.kind)).set("key",ru.key).set("priority",ru.prio).set("priority_name",procs::pagePrioName(ru.prio));
			j.set("page_priority_rule",x);
		} else j.set("page_priority_rule",Json());
		j.set("ifeo_page_priority",prioJson(d.ifeo));
		j.set("page_combining_restricted",triState(d.combine)).set("ksm_merged_bytes",by(d.ksm));

		if (thr) {
			std::vector<Json> t;
			t.reserve(d.thr.v.size());
			for (auto& x : d.thr.v) {
				Json o = Json::obj();
				o.set("tid",x.tid).set("page_priority",prioJson(x.prio)).set("page_priority_name",prioName(x.prio));
				o.set("memory_wait",txt(x.wait)).set("stack_committed_bytes",by(x.stackUsed)).set("stack_reserved_bytes",by(x.stackLimit));
				t.push_back(o);
			}
			Json pg = mcp::paged(t,a,4096,4096,"thread_list");
			j.set("thread_list",*pg.get("thread_list")).set("threads_count",*pg.get("count")).set("threads_truncated",*pg.get("truncated"));
			j.set("threads_total",*pg.get("total_count")).set("threads_offset",*pg.get("offset")).set("threads_limit",*pg.get("limit"));
			j.set("threads_waiting_on_memory",d.thr.memWaits).set("thread_note",txt(d.thr.why));
		}
		return j;
	}

	std::string afterColon(const std::string& s) {
		size_t a = s.find("failed (");
		if (a == std::string::npos) return s;
		std::string w = s.substr(a + 8);
		if (!w.empty() && w.back() == ')') w.pop_back();
		return w;
	}

	Json reduceWorkingSet(const Json& a) {
		std::vector<Json> targets;
		const Json* one = a.get("pid");
		const Json* many = a.get("pids");
		const Json* starts = a.get("start_times");
		const Json* st1 = a.get("start_time");
		if (one && one->t != Json::Null && many && many->t != Json::Null) throw std::runtime_error("give pid or pids, not both");
		if (one && one->t != Json::Null && starts && starts->t != Json::Null) throw std::runtime_error("start_times goes with pids, give start_time with pid");
		if (many && many->t != Json::Null && st1 && st1->t != Json::Null) throw std::runtime_error("start_time goes with pid, give start_times with pids");
		if (one && one->t != Json::Null) {
			Json t = Json::obj();
			t.set("pid",*one);
			if (st1) t.set("start_time",*st1);
			targets.push_back(t);
		} else if (many && many->t != Json::Null) {
			if (many->t != Json::Arr || many->a.empty()) throw std::runtime_error("pids must be a list of process ids");
			if (many->a.size() > 256) throw std::runtime_error("at most 256 pids in one call");
			if (starts && starts->t != Json::Null && (starts->t != Json::Arr || starts->a.size() != many->a.size())) throw std::runtime_error("start_times must be a list as long as pids");
			std::set<double> seen;
			for (size_t i = 0; i < many->a.size(); i++) {
				if (many->a[i].t == Json::Num && !seen.insert(many->a[i].n).second) continue;
				Json t = Json::obj();
				t.set("pid",many->a[i]);
				if (starts && starts->t == Json::Arr) t.set("start_time",starts->a[i]);
				targets.push_back(t);
			}
		} else throw std::runtime_error("pid or pids is required. list_processes gives them");
		Json res = Json::arr();
		int good = 0,bad = 0;
		double freed = 0.0;
		bool unk = false;
		for (auto& t : targets) {
			Json o = Json::obj();
			const Json* pv = t.get("pid");
			o.set("pid",pv && pv->t == Json::Num ? *pv : Json());
			try {
				ProcRef r = procs::target(t);
				o.set("name",r.name).set("start_time",std::to_string(r.start));
				Trim tr;
				std::string s = trim(r,&tr);
				bool ok = s.find(" : OK") != std::string::npos;
				appLog("[mcp] " + s);
				o.set("ok",ok).set("before_bytes",by(tr.before)).set("after_bytes",by(tr.after));
				if (tr.before != NA && tr.after != NA) {
					double f = (double)tr.before - (double)tr.after;
					o.set("freed_bytes",f);
					if (ok) freed += f;
				} else {
					o.set("freed_bytes",Json());
					if (ok) unk = true;
				}
				o.set("error",ok ? Json() : Json(afterColon(s)));
				if (ok) good++;
				else bad++;
			} catch (std::runtime_error& e) {
				o.set("name",Json()).set("start_time",Json()).set("ok",false).set("before_bytes",Json()).set("after_bytes",Json()).set("freed_bytes",Json());
				o.set("error",std::string(e.what()));
				appLog("[mcp] Reduce working set (" + (pv && pv->t == Json::Num ? "pid " + pv->dump() : std::string("no valid pid")) + ") : failed (" + e.what() + ")");
				bad++;
			}
			res.push(o);
		}
		Json j = Json::obj();
		j.set("ok_count",good).set("failed_count",bad).set("freed_bytes",unk ? Json() : Json(freed)).set("results",res);
#ifdef _WIN32
		j.set("note","Trimmed pages moved to the standby list and fault back when used, that memory was not lost or destroyed. Private bytes does not change.");
#else
		j.set("note","File pages were dropped and anonymous pages went to swap or zram, without swap they stay in RAM. They fault back when used, nothing was lost or destroyed. Private bytes does not change.");
#endif
		return j;
	}

	Json setPagePriority(const Json& a) {
		int prio = -1;
		if (!intArg(a,"priority",1,5,prio)) throw std::runtime_error("priority is required, 1 very low, 2 low, 3 medium, 4 below normal, 5 normal");
		ProcRef r = procs::target(a);
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		if (!row) throw std::runtime_error("pid " + std::to_string(r.pid) + " just exited");
		const Json* t = a.get("tid");
		if (t && t->t != Json::Null) {
			if (t->t != Json::Num || t->n != std::floor(t->n) || t->n <= 0 || t->n > 4294967295.0) throw std::runtime_error("tid must be a whole number, get_process with include_threads gives it");
			int prev = -1;
			uint32_t id = (uint32_t)t->n;
			std::string s = setThrPrio(r,id,0,prio,&prev);
			appLog("[mcp] " + s);
			if (s.find(" : OK") == std::string::npos) throw std::runtime_error(afterColon(s));
			ThrView v;
			be::threads(*row,v);
			int now = -1;
			for (auto& x : v.v) if (x.tid == id) now = x.prio;
			Json j = procs::refJson(r);
			j.set("tid",id).set("previous_priority",prioJson(prev)).set("previous_priority_name",prioName(prev));
			j.set("priority",prioJson(now)).set("priority_name",prioName(now)).set("rule_updated",false);
			return j;
		}
		int prev = -1;
		bool fol = false;
		std::string s = setPrio(r,prio,&prev,&fol);
		appLog("[mcp] " + s);
		if (s.find(" : OK") == std::string::npos) throw std::runtime_error(afterColon(s));
		int now = be::pagePrio(*row);
		Json j = procs::refJson(r);
		j.set("tid",Json()).set("previous_priority",prioJson(prev)).set("previous_priority_name",prioName(prev));
		j.set("priority",prioJson(now)).set("priority_name",prioName(now)).set("rule_updated",fol);
		return j;
	}

	Json ruleJson(const Rule& r) {
		Json o = Json::obj();
		o.set("kind",ruleKind(r.kind)).set("key",r.key).set("priority",prioJson(r.prio)).set("priority_name",prioName(r.prio));
		return o;
	}

	int kindArg(const Json& a) {
		std::string k = textArg(a,"kind","name");
		if (k == "name") return RuleName;
		if (k == "command_line") return RuleCmd;
		if (k == "ifeo") return RuleIfeo;
		throw std::runtime_error("kind must be name, command_line or ifeo");
	}

	Json pagePriorityRules(const Json& a) {
		std::string act = textArg(a,"action","list");
		if (act == "list") {
			std::vector<Rule> v = rulesAll();
			std::vector<Json> rows;
			rows.reserve(v.size());
			for (auto& r : v) rows.push_back(ruleJson(r));
			Json j = mcp::paged(rows,a,100,1000,"rules");
			j.set("ifeo_supported",ifeoSupported()).set("ifeo_writable",ifeoWritable());
			return j;
		}
		if (act != "add" && act != "remove") throw std::runtime_error("action must be list, add or remove");
		int kind = kindArg(a);
		std::string key = textArg(a,"key","");
		const Json* pid = a.get("pid");
		int prio = -1;
		if (act == "add") intArg(a,"priority",1,5,prio);
		if (pid && pid->t != Json::Null) {
			ProcRef r = procs::target(a);
			auto l = procs::latest();
			const ProcRow* row = l->find(r);
			if (!row) throw std::runtime_error("pid " + std::to_string(r.pid) + " just exited");
			if (key.empty()) {
				if (act == "add" && sysLike(*row)) throw std::runtime_error("FRS never applies a rule to a system process, pick another pid");
				key = kind == RuleCmd ? be::cmdLine(*row) : row->name;
				if (key.empty()) throw std::runtime_error("FRS can't read this process's command line, give key yourself");
			}
			if (prio < 0 && act == "add") prio = be::pagePrio(*row);
		}
		if (key.empty()) throw std::runtime_error("key is required : the program name, the exact command line, or pid to take it from a running process");
		std::string lb = "Page priority rule " + act + " (" + ruleKind(kind) + ")";
		Json j = Json::obj();
		if (act == "add") {
			if (prio < 0) throw std::runtime_error("priority is required, 1 very low, 2 low, 3 medium, 4 below normal, 5 normal");
			Rule prev;
			std::string why = addRule(kind,key,prio,&prev);
			appLog("[mcp] " + lb + (why.empty() ? " : OK" : " : failed (" + why + ")"));
			if (!why.empty()) throw std::runtime_error(why);
			Rule cur;
			cur.kind = kind;
			cur.key = ruleKey(kind,key);
			cur.prio = prio;
			j.set("rule",ruleJson(cur)).set("replaced",prev.kind < 0 ? Json() : ruleJson(prev));
			return j;
		}
		Rule old;
		std::string why = delRule(kind,key,&old);
		appLog("[mcp] " + lb + (why.empty() ? " : OK" : " : failed (" + why + ")"));
		if (!why.empty()) throw std::runtime_error(why);
		j.set("removed",ruleJson(old));
		return j;
	}

}

namespace procdet {

	void regTools() {
		mcp::add("Process details","get_process",
			"One process in full : statistics, working set split (page walk), commit, quota, job limits, page priority, saved rule, path, command line. "
			"Working set includes shared pages. Private bytes is commit, a trim does not lower it. In a job the commit block is the whole job (scope), not this process. A job limit counts commit, not working set. "
			"FRS sees a job only when it can open it, else jobs is empty and note says why. Nested jobs come innermost first. Linux : the job is the cgroup, commit and job keep the Windows names and source says which cgroup file each one is. A limit is {set,bytes}, set null means unknown.",
			R"j({"type":"object","required":["pid"],"properties":{
				"pid":{"type":"integer","description":"process id"},
				"start_time":{"type":"string","description":"from list_processes, refuses a reused pid"},
				"include_threads":{"type":"boolean","description":"also per thread page priority, memory wait and stack size"},
				"limit":{"type":"integer","description":"with include_threads : threads to return, default and max 4096"},
				"offset":{"type":"integer","description":"with include_threads : threads to skip"}}})j",
			mcp::Read,getProcess);

		mcp::add("Process details","reduce_working_set",
			"Trim the working set of one or several processes, before and after per pid. "
#ifdef _WIN32
			"Pages move to standby and fault back when used, "
#else
			"File pages are dropped and anonymous pages go to swap or zram, without swap they stay in RAM. They fault back when used, "
#endif
			"so this frees RAM only for as long as the process leaves them alone, and hard faults can rise. "
			"System and pseudo processes are refused. Each failure is reported in its own result.",
			R"j({"type":"object","properties":{
				"pid":{"type":"integer","description":"one process id"},
				"start_time":{"type":"string","description":"with pid, from list_processes, refuses a reused pid"},
				"pids":{"type":"array","items":{"type":"integer"},"description":"several process ids instead of pid, at most 256"},
				"start_times":{"type":"array","items":{"type":"string"},"description":"with pids, same length and order"}}})j",
			mcp::Write,reduceWorkingSet);

		mcp::add("Process details","set_page_priority",
			"Set the memory priority of a process's new pages, or of one thread. Low priority pages leave standby first. It does not free anything by itself. "
			"Returns the previous value so it can be undone. A saved rule for that process follows the new value, or FRS would undo it. Windows only.",
			R"j({"type":"object","required":["pid","priority"],"properties":{
				"pid":{"type":"integer","description":"process id"},
				"start_time":{"type":"string","description":"from list_processes, refuses a reused pid"},
				"priority":{"type":"integer","minimum":1,"maximum":5,"description":"1 very low, 2 low, 3 medium, 4 below normal, 5 normal"},
				"tid":{"type":"integer","description":"set this thread instead of the process, get_process include_threads lists them"}}})j",
			mcp::Write,setPagePriority);

		mcp::add("Process details","page_priority_rules",
			"List, add or remove saved page priority rules. name and command_line rules live in FRS and are reapplied while FRS runs. "
			"ifeo writes HKLM Image File Execution Options, needs admin, and Windows applies it without FRS. Remove touches only that value. On Linux there is no page priority, a rule below normal marks pages cold once a minute.",
			R"j({"type":"object","properties":{
				"action":{"type":"string","enum":["list","add","remove"],"description":"default list"},
				"kind":{"type":"string","enum":["name","command_line","ifeo"],"description":"default name"},
				"key":{"type":"string","description":"program name like notepad.exe, or the exact command line"},
				"pid":{"type":"integer","description":"take key from this running process, and priority when none is given"},
				"start_time":{"type":"string","description":"with pid, refuses a reused pid"},
				"priority":{"type":"integer","minimum":1,"maximum":5,"description":"1 very low, 2 low, 3 medium, 4 below normal, 5 normal"},
				"limit":{"type":"integer","description":"list : rows to return, default 100, max 1000"},
				"offset":{"type":"integer","description":"list : rows to skip"}}})j",
			mcp::Write,pagePriorityRules);
	}

}
