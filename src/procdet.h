#pragma once
#include "procs.h"
#include <memory>
#include <string>
#include <vector>

// One process in depth : tabs Memory and Limits, saved page priority rules, get_process and friends.
namespace procdet {

	// a limit FRS could read and that is not set, NA stays unknown
	constexpr uint64_t NOLIM = NA - 1;

	// FRS changes nothing on these. PID 1 is not marked sys, that would also say it has no user heaps
	inline bool sysLike(const ProcRow& r) {
#ifdef _WIN32
		return r.sys;
#else
		return r.sys || r.pid == 1;
#endif
	}

	struct Ws {
		uint64_t total = NA,priv = NA,shareable = NA,shared = NA; // page walk
		uint64_t locked = NA,modified = NA,original = NA,anon = NA; // modified and original : mapped and image pages only. anon : Linux
		int prio = -1; // highest page priority on a resident page
		std::string why;
	};

	struct Commit {
		uint64_t shared = NA,priv = NA,peak = NA,privLimit = NA,totalLimit = NA;
		std::string why;
	};

	struct Quota {
		uint64_t minWs = NA,maxWs = NA,paged = NA,nonPaged = NA,pagefile = NA;
		int hardMin = -1,hardMax = -1;
		std::string why;
	};

	struct Job {
		std::string name,via; // via : how FRS got to it
		uint64_t procMem = NA,jobMem = NA,wsMin = NA,wsMax = NA;
		uint64_t peakProc = NA,peakJob = NA,faults = NA,total = NA,active = NA,term = NA;
		std::string why;
	};

	struct JobView {
		int in = -1; // -1 unknown, 0 no, 1 yes
		std::vector<Job> jobs; // innermost first, why says when a tie leaves the order unknown
		bool read = false; // a lookup ran
		std::string why;
	};

	struct Thr {
		uint32_t tid = 0;
		uint64_t start = 0;
		int prio = -1;
		std::string wait; // only when it waits on the memory manager
		uint64_t stackUsed = NA,stackLimit = NA;
	};

	struct ThrView {
		std::vector<Thr> v;
		int memWaits = 0;
		bool read = false;
		std::string why;
	};

	struct Det {
		bool denied = false; // no handle at all
		bool noFull = false; // limited handle only
		std::string why;
		std::string cmd;
		bool cmdRead = false;
		std::string cmdWhy;
		int combine = -1; // Windows, page combining restricted : 1 yes, 0 no, -1 unknown
		uint64_t ksm = NA; // Linux, bytes merged by KSM
		int ifeo = -1; // saved IFEO page priority, -1 none
		Ws ws;
		bool wsRead = false;
		Commit commit;
		Quota quota;
		JobView jobs;
		ThrView thr;
	};

	enum { WWs = 1,WJobs = 2,WThr = 4,WIfeo = 8 };
	void watch(const ProcRef& r,int what); // UI thread, every frame the data is on screen
	void poke(const ProcRef& r); // read again now, after an action
	std::shared_ptr<const Det> det(const ProcRef& r); // latest finished read, null until there is one
	Det readAll(const ProcRow& r,bool threads); // blocks, worker and MCP threads only

	enum { RuleName,RuleCmd,RuleIfeo };
	struct Rule {
		int kind = RuleName;
		std::string key;
		int prio = 5;
	};
	const char* ruleKind(int kind);
	std::string ruleKey(int kind,const std::string& raw); // the text addRule stores, "" when blank
	std::vector<Rule> rules(); // name and command line
	std::vector<Rule> ifeoCached(); // from the last worker read
	std::vector<Rule> rulesAll(); // blocks, reads the registry too
	// "" when it worked, else why. prev gets a rule the add replaced, kind stays -1 when none
	std::string addRule(int kind,const std::string& key,int prio,Rule* prev = nullptr);
	std::string delRule(int kind,const std::string& key,Rule* old = nullptr);
	bool ruleFor(const ProcRow& r,const std::string& cmd,Rule& out); // command line wins over name, like SI
	// a page priority picked by hand goes into the matching rule, or the rule job undoes it on its next tick.
	// worker threads, it may read the command line
	bool ruleFollow(const ProcRef& r,int prio);

	struct Trim {
		uint64_t before = NA,after = NA;
	};
	std::string trim(const ProcRef& r,Trim* t = nullptr); // procs::reduceWs with a size before and after
	std::string setPrio(const ProcRef& r,int prio,int* prev,bool* follow = nullptr); // procs::setPagePrio plus ruleFollow
	std::string setThrPrio(const ProcRef& r,uint32_t tid,uint64_t tstart,int prio,int* prev); // "Label : OK" shape
	std::string setQuota(const ProcRef& r,uint64_t mn,uint64_t mx); // same
	bool ifeoSupported();
	bool ifeoWritable();

	void initTabs(); // tab_stats.cpp
	void saveMenu(const ProcRef& r); // UI thread, inside another page's row popup : the same save items for that row's process

	namespace be {

		void fast(const ProcRow& r,Det& d); // small structs : commit, quota, in job, combining, command line once
		void ws(const ProcRow& r,Ws& w); // page walk
		void jobs(const ProcRow& r,JobView& j); // handle scan or cgroup files
		void threads(const ProcRow& r,ThrView& t);
		std::string setThrPrio(const ProcRow& r,uint32_t tid,uint64_t tstart,int prio,int* prev); // "" or why
		std::string setQuota(const ProcRow& r,uint64_t mn,uint64_t mx);
		std::string cmdLine(const ProcRow& r); // "" when unknown
		bool now(const ProcRow& r,uint64_t& ws,uint64_t& priv); // fresh read
		int pagePrio(const ProcRow& r); // fresh read, -1 unknown
		bool applyRule(const ProcRow& r,int prio,std::string& why); // what a rule does on this OS. true when it acted, why set when that failed
		void lowPrio(); // calling thread does background work

		bool ifeoOk(); // this OS has it
		bool ifeoCanWrite();
		int ifeoGet(const std::string& exe);
		std::string ifeoSet(const std::string& exe,int prio);
		std::string ifeoDel(const std::string& exe,bool& had);
		std::vector<Rule> ifeoList();

	}

}
