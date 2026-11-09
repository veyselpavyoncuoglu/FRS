#include "procdet.h"
#include "cfg.h"
#include "domains.h"
#include "platform.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <utility>

namespace {

	using namespace procdet;
	using Key = std::pair<uint32_t,uint64_t>;

	const size_t MAXRULES = 256;
	const size_t MAXKEY = 4096;
	const char* RULEFILE = "pagerules.ini";

	std::mutex g_rm;
	std::vector<Rule> g_rules; // writers hold g_rm across the file write
	Snap<std::vector<Rule>> g_pub; // what readers get, the UI asks every frame and must not wait on the disk
	Snap<std::vector<Rule>> g_ifeo;

	struct W {
		ProcRef r;
		uint64_t until = 0,uWs = 0,uJobs = 0,uThr = 0,uIfeo = 0;
		uint64_t tWs = 0,tJobs = 0,tThr = 0,tIfeo = 0,cost = 0;
		std::shared_ptr<const Det> d;
	};

	std::mutex g_wm;
	std::map<Key,W> g_w;
	uint64_t g_ifeoWant = 0,g_ifeoT = 0;

	bool sameKey(const Rule& r,int kind,const std::string& key) {
		if (r.kind != kind) return false;
		return kind == RuleCmd ? r.key == key : util::lower(r.key) == util::lower(key);
	}

	void loadRules() {
		std::string s = plat::readFile(RULEFILE);
		size_t i = 0;
		while (i < s.size() && g_rules.size() < MAXRULES) {
			size_t e = s.find('\n',i);
			std::string ln = s.substr(i,e == std::string::npos ? e : e - i);
			i = e == std::string::npos ? s.size() : e + 1;
			if (ln.size() < 5 || (ln[0] != 'n' && ln[0] != 'c') || ln[1] != ' ' || ln[2] < '1' || ln[2] > '5' || ln[3] != ' ') continue;
			Rule r;
			r.kind = ln[0] == 'n' ? RuleName : RuleCmd;
			r.prio = ln[2] - '0';
			r.key = ruleKey(r.kind,ln.substr(4));
			if (!r.key.empty()) g_rules.push_back(r);
		}
		g_pub.put(std::make_shared<const std::vector<Rule>>(g_rules));
	}

	// a change that can't be saved goes back to bak, so no rule runs that a restart would lose
	std::string saveRules(const std::vector<Rule>& bak) {
		std::string t;
		for (auto& r : g_rules) t += std::string(r.kind == RuleName ? "n " : "c ") + (char)('0' + r.prio) + " " + r.key + "\n";
		plat::writeFile(RULEFILE,t);
		if (plat::readFile(RULEFILE) != t) {
			g_rules = bak;
			return "could not write " + std::string(RULEFILE);
		}
		g_pub.put(std::make_shared<const std::vector<Rule>>(g_rules));
		return "";
	}

	const Rule* match(const std::vector<Rule>& rs,const std::string& name,const std::string& cmd) {
		if (!cmd.empty()) for (auto& r : rs) if (r.kind == RuleCmd && r.key == cmd) return &r;
		std::string low = util::lower(name);
		for (auto& r : rs) if (r.kind == RuleName && util::lower(r.key) == low) return &r;
		return nullptr;
	}

	bool isOk(const std::string& s) {
		return s.find(" : OK") != std::string::npos;
	}

	bool gone(const std::string& why) {
		return why == "process exited" || why == "process is exiting";
	}

	struct Seen {
		bool skip = false,held = false,told = false;
		int cmdTries = 0;
		std::string cmd;
		uint64_t pass = 0;
	};

	// polled like SI : a new process runs at its old priority until the next tick. One failed set and that
	// process is left alone until the rule list changes, so protected targets don't fill the log.
	void ruleTick() {
		static std::map<Key,Seen> seen;
		static uint64_t pass = 0;
		static std::shared_ptr<const std::vector<Rule>> last;
		auto cur = g_pub.get();
		if (cur != last) {
			seen.clear();
			last = cur;
		}
		if (!cur || cur->empty()) return;
		const std::vector<Rule>& rs = *cur;
		bool needCmd = false;
		for (auto& r : rs) if (r.kind == RuleCmd) needCmd = true;
		auto l = procs::latest();
		pass++;
		for (auto& row : l->rows) {
			if (row.pseudo || row.sys) continue;
			Seen& s = seen[Key(row.pid,row.start)];
			s.pass = pass;
			if (s.skip) continue;
			if (needCmd && s.cmd.empty() && s.cmdTries < 5) {
				s.cmdTries++;
				s.cmd = be::cmdLine(row);
			}
			const Rule* m = match(rs,row.name,s.cmd);
			if (!m) continue;
			if (row.pagePrio == m->prio) s.held = true;
			// fresh read, a row can be a tick behind the set. Once per process, one that keeps moving its own value must not fill the log
			int was = s.held && !s.told && row.pagePrio != m->prio ? be::pagePrio(row) : -1;
			std::string why;
			if (!be::applyRule(row,m->prio,why)) continue;
			std::string lb = "Page priority rule (pid " + std::to_string(row.pid) + ", " + row.name + ")";
			if (!why.empty()) {
				s.skip = true;
				appLog(lb + " : failed (" + why + ")");
			} else if (!s.held) {
				s.held = true;
				appLog(lb + " : OK");
			} else if (was >= 0 && was != m->prio) {
				s.told = true;
				appLog(lb + " : put back to " + procs::pagePrioName(m->prio) + ", it was " + procs::pagePrioName(was) + ". Change the saved rule to keep another value");
			}
		}
		for (auto it = seen.begin(); it != seen.end();) {
			if (it->second.pass != pass) it = seen.erase(it);
			else ++it;
		}
	}

	void workTick() {
		static bool low = false;
		if (!low) {
			be::lowPrio();
			low = true;
		}
		uint64_t now = util::nowMs();
		std::vector<W> todo;
		bool ifeoDue = false;
		{
			std::lock_guard<std::mutex> lk(g_wm);
			for (auto it = g_w.begin(); it != g_w.end();) {
				if (now > it->second.until + 30000) {
					it = g_w.erase(it);
					continue;
				}
				if (now <= it->second.until) todo.push_back(it->second);
				++it;
			}
			ifeoDue = now <= g_ifeoWant && now >= g_ifeoT + 5000;
			if (ifeoDue) g_ifeoT = now;
		}
		if (ifeoDue) g_ifeo.put(std::make_shared<const std::vector<Rule>>(be::ifeoList()));
		auto l = procs::latest();
		for (W& w : todo) {
			if (sampler::stopping()) return;
			const ProcRow* r = l->find(w.r);
			if (!r) continue;
			auto d = w.d ? std::make_shared<Det>(*w.d) : std::make_shared<Det>();
			bool ended = false;
			if (r->pseudo) {
				d->why = d->commit.why = d->quota.why = "Pseudo process, it has no memory of its own";
				d->denied = true;
				d->jobs.in = 0;
			} else {
				be::fast(*r,*d);
				ended = gone(d->why);
				if (now <= w.uIfeo && now >= w.tIfeo + 5000) {
					d->ifeo = be::ifeoGet(r->name);
					w.tIfeo = now;
				}
				if (now <= w.uWs && now >= w.tWs + std::max((uint64_t)3000,w.cost * 20)) {
					uint64_t t0 = util::nowMs();
					Ws x;
					be::ws(*r,x);
					ended = ended || gone(x.why);
					d->ws = x;
					d->wsRead = true;
					w.cost = util::nowMs() - t0;
					w.tWs = now;
				}
				if (now <= w.uJobs && now >= w.tJobs + 5000) {
					JobView j;
					be::jobs(*r,j);
					ended = ended || gone(j.why);
					d->jobs.jobs = std::move(j.jobs);
					d->jobs.why = std::move(j.why);
					d->jobs.read = true;
					if (j.in >= 0) d->jobs.in = j.in;
					w.tJobs = now;
				}
				if (now <= w.uThr && now >= w.tThr + 2000) {
					ThrView t;
					be::threads(*r,t);
					ended = ended || gone(t.why);
					d->thr = std::move(t);
					w.tThr = now;
				}
			}
			std::lock_guard<std::mutex> lk(g_wm);
			auto it = g_w.find(Key(w.r.pid,w.r.start));
			if (it == g_w.end()) continue;
			W& o = it->second;
			// the process ended before the snapshot noticed : keep the last good read on screen, not this failed one
			if (!ended || !w.d) o.d = d;
			if (w.tWs > o.tWs) o.tWs = w.tWs;
			if (w.tJobs > o.tJobs) o.tJobs = w.tJobs;
			if (w.tThr > o.tThr) o.tThr = w.tThr;
			if (w.tIfeo > o.tIfeo) o.tIfeo = w.tIfeo;
			o.cost = w.cost;
		}
	}

	std::string checkKey(int kind,const std::string& key) {
		if (kind != RuleName && kind != RuleCmd && kind != RuleIfeo) return "kind must be name, command_line or ifeo";
		if (key.empty()) return "the match text is empty";
		if (key.size() > MAXKEY) return "the match text is over 4096 bytes";
		if (kind == RuleCmd) return "";
		if (key.size() > 255) return "a process name is at most 255 bytes";
		if (key.find_first_of("\\/:*?\"<>|") != std::string::npos) return "give the process name only, like notepad.exe, not a path";
		if (key == "." || key == "..") return "that is not a process name";
		return "";
	}

}

namespace procdet {

	void watch(const ProcRef& r,int what) {
		uint64_t until = util::nowMs() + 1500;
		std::lock_guard<std::mutex> lk(g_wm);
		W& w = g_w[Key(r.pid,r.start)];
		w.r = r;
		w.until = until;
		if (what & WWs) w.uWs = until;
		if (what & WJobs) w.uJobs = until;
		if (what & WThr) w.uThr = until;
		if (what & WIfeo) {
			w.uIfeo = until;
			g_ifeoWant = until;
		}
	}

	void poke(const ProcRef& r) {
		std::lock_guard<std::mutex> lk(g_wm);
		g_ifeoT = 0;
		auto it = g_w.find(Key(r.pid,r.start));
		if (it == g_w.end()) return;
		W& w = it->second;
		w.tWs = w.tJobs = w.tThr = w.tIfeo = 0;
	}

	std::shared_ptr<const Det> det(const ProcRef& r) {
		std::lock_guard<std::mutex> lk(g_wm);
		auto it = g_w.find(Key(r.pid,r.start));
		return it == g_w.end() ? nullptr : it->second.d;
	}

	Det readAll(const ProcRow& r,bool threads) {
		Det d;
		if (r.pseudo) {
			d.denied = true;
			d.why = "pseudo process, it has no memory of its own";
			return d;
		}
		be::fast(r,d);
		be::ws(r,d.ws);
		d.wsRead = true;
		JobView j;
		be::jobs(r,j);
		d.jobs.jobs = std::move(j.jobs);
		d.jobs.why = std::move(j.why);
		d.jobs.read = true;
		if (j.in >= 0) d.jobs.in = j.in;
		if (threads) be::threads(r,d.thr);
		d.ifeo = be::ifeoGet(r.name);
		return d;
	}

	const char* ruleKind(int k) {
		return k == RuleCmd ? "command_line" : k == RuleIfeo ? "ifeo" : "name";
	}

	std::string ruleKey(int kind,const std::string& raw) {
		std::string k = util::safe(raw);
		size_t a = k.find_first_not_of(' ');
		if (a == std::string::npos) return "";
		// a command line matches exactly and can really end in a space
		return kind == RuleCmd ? k : k.substr(a,k.find_last_not_of(' ') - a + 1);
	}

	std::vector<Rule> rules() {
		auto p = g_pub.get();
		return p ? *p : std::vector<Rule>();
	}

	std::vector<Rule> ifeoCached() {
		auto p = g_ifeo.get();
		return p ? *p : std::vector<Rule>();
	}

	std::vector<Rule> rulesAll() {
		std::vector<Rule> v = rules();
		std::vector<Rule> i = be::ifeoList();
		v.insert(v.end(),i.begin(),i.end());
		return v;
	}

	std::string addRule(int kind,const std::string& rawKey,int prio,Rule* prev) {
		std::string key = ruleKey(kind,rawKey);
		if (prev) prev->kind = -1;
		std::string why = checkKey(kind,key);
		if (!why.empty()) return why;
		if (prio < 1 || prio > 5) return "priority must be 1 to 5 : 1 very low, 2 low, 3 medium, 4 below normal, 5 normal";
		if (kind == RuleIfeo) {
			if (!be::ifeoOk()) return "not available on Linux";
			int old = be::ifeoGet(key);
			why = be::ifeoSet(key,prio);
			if (why.empty() && prev && old >= 0) {
				prev->kind = RuleIfeo;
				prev->key = key;
				prev->prio = old;
			}
			std::lock_guard<std::mutex> lk(g_wm);
			g_ifeoT = 0;
			return why;
		}
		std::lock_guard<std::mutex> lk(g_rm);
		std::vector<Rule> bak = g_rules;
		for (auto& r : g_rules) if (sameKey(r,kind,key)) {
			if (prev) *prev = r;
			r.prio = prio;
			return saveRules(bak);
		}
		if (g_rules.size() >= MAXRULES) return "too many rules, the limit is 256";
		Rule r;
		r.kind = kind;
		r.key = key;
		r.prio = prio;
		g_rules.push_back(r);
		return saveRules(bak);
	}

	std::string delRule(int kind,const std::string& rawKey,Rule* old) {
		std::string key = ruleKey(kind,rawKey);
		if (old) old->kind = -1;
		std::string why = checkKey(kind,key);
		if (!why.empty()) return why;
		if (kind == RuleIfeo) {
			if (!be::ifeoOk()) return "not available on Linux";
			int was = be::ifeoGet(key);
			bool had = false;
			why = be::ifeoDel(key,had);
			if (why.empty() && !had) return "no such rule";
			if (why.empty() && old) {
				old->kind = RuleIfeo;
				old->key = key;
				old->prio = was;
			}
			std::lock_guard<std::mutex> lk(g_wm);
			g_ifeoT = 0;
			return why;
		}
		std::lock_guard<std::mutex> lk(g_rm);
		std::vector<Rule> bak = g_rules;
		for (size_t i = 0; i < g_rules.size(); i++) if (sameKey(g_rules[i],kind,key)) {
			if (old) *old = g_rules[i];
			g_rules.erase(g_rules.begin() + i);
			return saveRules(bak);
		}
		return "no such rule";
	}

	bool ruleFor(const ProcRow& r,const std::string& cmd,Rule& out) {
		std::lock_guard<std::mutex> lk(g_rm);
		const Rule* m = match(g_rules,r.name,cmd);
		if (m) out = *m;
		return m != nullptr;
	}

	bool ruleFollow(const ProcRef& r,int prio) {
		if (prio < 1 || prio > 5) return false;
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		if (!row) return false;
		bool needCmd = false;
		{
			std::lock_guard<std::mutex> lk(g_rm);
			if (g_rules.empty()) return false;
			for (auto& x : g_rules) if (x.kind == RuleCmd) needCmd = true;
		}
		std::string cmd = needCmd ? be::cmdLine(*row) : "";
		std::lock_guard<std::mutex> lk(g_rm);
		Rule* m = const_cast<Rule*>(match(g_rules,row->name,cmd));
		if (!m) return false;
		if (m->prio != prio) {
			std::vector<Rule> bak = g_rules;
			m->prio = prio;
			std::string why = saveRules(bak);
			if (!why.empty()) {
				appLog("Page priority rule : failed (" + why + ")");
				return false;
			}
		}
		return true;
	}

	std::string trim(const ProcRef& r,Trim* t) {
#ifndef _WIN32
		if (r.pid == 1) return "Reduce working set (pid 1) : failed (init is refused)";
#endif
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		uint64_t a = NA,b = NA,pv = NA;
		if (row && !be::now(*row,b,pv)) b = NA;
		std::string s = procs::reduceWs(r);
		if (row && !be::now(*row,a,pv)) a = NA;
		if (t) {
			t->before = b;
			t->after = a;
		}
		poke(r);
		return s;
	}

	std::string setPrio(const ProcRef& r,int prio,int* prev,bool* follow) {
		std::string s = procs::setPagePrio(r,prio,prev);
		bool f = isOk(s) && ruleFollow(r,prio);
		if (follow) *follow = f;
		poke(r);
		return s;
	}

	std::string setThrPrio(const ProcRef& r,uint32_t tid,uint64_t tstart,int prio,int* prev) {
		std::string lb = "Thread page priority (pid " + std::to_string(r.pid) + ", tid " + std::to_string(tid) + ")";
		if (prev) *prev = -1;
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		std::string why = !row ? "process exited" : row->pseudo ? "pseudo process, it has no memory of its own" : row->sys ? "system process, refused" : prio < 1 || prio > 5 ? "priority must be 1 to 5" : "";
		if (why.empty()) why = be::setThrPrio(*row,tid,tstart,prio,prev);
		poke(r);
		return lb + (why.empty() ? " : OK" : " : failed (" + why + ")");
	}

	std::string setQuota(const ProcRef& r,uint64_t mn,uint64_t mx) {
		std::string lb = "Working set quota (pid " + std::to_string(r.pid) + ")";
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		std::string why = !row ? "process exited" : row->pseudo ? "pseudo process, it has no memory of its own" : sysLike(*row) ? "system process, refused" : mn >= mx ? "minimum must be below maximum" : "";
		if (why.empty()) why = be::setQuota(*row,mn,mx);
		poke(r);
		return lb + (why.empty() ? " : OK" : " : failed (" + why + ")");
	}

	bool ifeoSupported() {
		return be::ifeoOk();
	}

	bool ifeoWritable() {
		return be::ifeoCanWrite();
	}

	void init() {
		loadRules();
		cfg::optBool("procGraphFit","Processes","Process graphs fit the visible samples","Off : a process graph scales from its peak, so a small process stays small on it. On : it scales to the largest sample on screen.",false);
		initTabs();
		sampler::add("procdet",1000,workTick);
		sampler::add("procdet-rules",0,ruleTick);
	}

}
