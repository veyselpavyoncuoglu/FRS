#pragma once
#include "util.h"
#include "json.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// pid alone gets reused. pid + start identifies one process for its whole life.
struct ProcRef {
	uint32_t pid = 0;
	uint64_t start = 0; // creation time (Windows FILETIME) or /proc/pid/stat starttime (Linux)
	std::string name;
	bool operator==(const ProcRef& o) const { return pid == o.pid && start == o.start; }
};

// One process, memory columns. Sizes in bytes, NA when the OS gives nothing (never 0 for unknown).
// Deltas are per tick. Fields filled from the snapshot need no handle, so nothing goes missing.
struct ProcRow {
	uint32_t pid = 0,ppid = 0,session = 0;
	uint64_t start = 0;
	std::string name; // already util::safe
	std::string user,path;

	uint64_t priv = NA,privPeak = NA,ws = NA,wsPeak = NA,wsPriv = NA,wsShareable = NA,wsShared = NA;
	uint64_t virt = NA,virtPeak = NA,minWs = NA,maxWs = NA,sharedCommit = NA;
	uint64_t paged = NA,pagedPeak = NA,nonPaged = NA,nonPagedPeak = NA;
	uint64_t faults = NA,hardFaults = NA;
	uint64_t gpuDed = NA,gpuShared = NA,gpuDedCommit = NA,gpuSharedCommit = NA; // from the GPU monitor's last sample, NA while it is off. Shared is system RAM, never add it to a RAM number
	int64_t dPriv = 0,dWs = 0,dFaults = 0,dHard = 0;
	bool noPrev = true; // no earlier sample, so the deltas are unknown. Group row : until a member has one

	int pagePrio = -1; // 0..7, -1 unknown
	int children = 0;
	int threads = 0;
	uint8_t bits = 0; // 32 or 64, 0 unknown
	uint64_t jobId = NA; // 0 = not in a job. Linux : cgroup inode
	bool pseudo = false; // Idle, Interrupts, kernel threads : no user memory, skip for trim
	bool isNew = false; // appeared on this tick
	bool memCompression = false,registry = false;
	bool sys = false; // System, Secure System, Memory Compression, Registry : real rows, refused for trim and page priority
	bool osProc = false; // part of the OS, the Hide system filter
	bool noHandle = false; // the limited open was refused, handle columns stay N/A
	bool noFull = false; // no full query handle, Shared WS stays N/A
	bool filtered = false; // a full query handle came back with its rights stripped
	uint64_t seenMs = 0; // util::nowMs when first seen, 0 when it was already there at FRS start
	uint64_t hotMs = 0; // util::nowMs of the last jump past the growth highlight threshold, 0 never
	std::string cgroup; // Linux only
	std::string uid; // Linux only
};

struct ProcList {
	uint64_t t = 0; // util::nowMs
	uint64_t seq = 0; // rises by one per tick
	uint64_t dtMs = 0; // real time since the tick before
	std::vector<ProcRow> rows; // sorted by pid
	uint64_t sumWs = 0,sumPriv = 0;
	const ProcRow* find(uint32_t pid) const;
	const ProcRow* find(const ProcRef& r) const; // pid and start must match
};

// ring per process, oldest first
struct ProcHist {
	std::vector<float> privMb,wsMb;
	std::vector<uint64_t> unixSec;
};

namespace procs {

	void init();
	void regTools();
	std::shared_ptr<const ProcList> latest(); // never null after init
	std::shared_ptr<const ProcHist> history(const ProcRef& r); // null when not known
	bool alive(const ProcRef& r);
	ProcRef ref(const ProcRow& r);
	// any thread : minimized or hidden to tray and no MCP process read for 60 s. The list then takes one snapshot every 5 s
	bool unseen();

	// MCP target from args "pid" and "start_time" (decimal string, numbers lose digits). Throws
	// runtime_error with a clear message on : no such process, pid reused, pseudo process.
	// needStart = true makes start_time mandatory, use it for Danger tools.
	ProcRef target(const Json& args,bool needStart = false);
	Json refJson(const ProcRef& r); // {pid,name,start_time}

	// "Reduce working set (pid 12) : OK" or "... : failed (why)". System and pseudo processes are refused.
	// Both call the OS, so go through task::run from the UI thread.
	std::string reduceWs(const ProcRef& r);
	std::string setPagePrio(const ProcRef& r,int prio,int* prev); // prio 0..7, prev gets the old value or -1
	const char* pagePrioName(int prio); // "Normal", "Very low"..., "N/A" when unknown

	// Processes page selection, UI thread. select also shows the first row when the page draws
	std::vector<ProcRef> selected();
	void select(const std::vector<ProcRef>& r);

}

// Floating process window. Other areas add tabs in their init(). A tab keeps its own per process
// state, keyed by ProcRef, and must not block : slow reads go through task::run or sampler.
namespace procwin {

	using TabFn = void(*)(const ProcRef&);
	void addTab(const char* title,int order,TabFn fn); // lower order goes left
	void addFloat(void(*fn)()); // runs every frame, also with no process window open. Windows that outlive their tab, close checks
	void open(const ProcRef& r,const char* tab = nullptr); // opens or focuses, on that tab when given
	void openPid(uint32_t pid);
	void select(const ProcRef& r,const char* title); // an open window goes to that tab
	bool isOpen(const ProcRef& r);
	void draw(); // shell only

}

// Inside the procs area only : the OS seam and the column table the page and the tools share.
namespace procs {

	// slower facts from the second pass, kept per ProcRef until the process exits
	struct Info {
		std::string user,path,cgroup;
		uint8_t bits = 0;
		int pagePrio = -1;
		uint64_t minWs = NA,maxWs = NA,sharedCommit = NA,jobId = NA;
		bool statics = false; // path, user, bits are in. They never change on Windows, Linux reads them again every full pass
		bool noHandle = false,noFull = false,filtered = false,osProc = false;
	};

	namespace be {

		bool snapshot(std::vector<ProcRow>& out,std::string& err); // rows in any order, identity and what needs no handle
		void info(const ProcRow& r,Info& i); // statics once on Windows, the rest every pass
		bool wsCounters(const ProcRow& r,uint64_t& shared,uint64_t& shareable); // page walk. shareable stays NA when the snapshot has it
		void wsFree(); // the walk stopped, drop its buffer
		std::string reduceWs(const ProcRow& r); // "" when it worked, else why
		std::string setPagePrio(const ProcRow& r,int prio,int* prev); // same
		uint32_t selfPid();
		bool invoker(std::string& user,std::string& home); // Linux under pkexec or sudo : who started FRS. false when FRS runs as its own user
		int writeFile(const std::string& path,const std::string& data,bool overwrite,std::string& err); // 0 written, 1 exists, -1 failed

	}

	enum ColKind { KText,KSize,KDelta,KCount,KCountDelta,KOther };

	enum Col {
		CName,CPid,CUser,CWs,CWsPriv,CPriv,CDPriv,CDWs,CDFaults,CDHard,CHist,CPrivPeak,CWsPeak,CWsShared,CWsShareable,
		CVirt,CVirtPeak,CMinWs,CMaxWs,CFaults,CHard,CPrio,CPaged,CPagedPeak,CNonPaged,CNonPagedPeak,CSharedCommit,
		CJob,CKids,CBits,CGpuDed,CGpuShared,CGpuDedCommit,CGpuSharedCommit,CPath,COLS
	};

	struct ColDef {
		const char* title;
		const char* key; // MCP name, null when it has none
		const char* tip;
		ColKind kind;
		float w;
		bool mem; // Memory preset, also the default set
		bool sum; // adds up on group rows and collapsed parents
		bool total; // has a column total
	};
	extern const ColDef cols[COLS];

	bool colNum(const ProcRow& r,int col,int64_t& out); // false when unknown or text
	std::string colText(const ProcRow& r,int col); // cell text. N/A when unknown, blank for a zero delta
	bool rowLess(const ProcRow& a,const ProcRow& b,int col,bool desc); // col < 0 : by pid. Unknown goes last both ways
	bool rowMatch(const ProcRow& r,const std::string& needle); // case-insensitive on name, pid, user, path
	void addSum(ProcRow& to,const ProcRow& r); // the sum columns only
	const ProcRow* parentOf(const ProcList& l,const ProcRow& r); // null when the parent is gone or the pid was reused

	std::shared_ptr<const ProcHist> histOf(uint32_t pid,uint64_t start); // history without building a ProcRef
	void wantWs(uint64_t ms); // shared and shareable WS are looked at, keeps the walk going that long
	bool wsLive(); // walk results are in

	struct Growth {
		int n = 0; // samples in the window
		double priv = 0,ws = 0; // bytes
		uint64_t secs = 0;
	};
	std::vector<Growth> growth(const ProcList& l,int windowSec); // same order as l.rows
	void waitFirst(); // MCP threads : the first snapshot is in, a fresh one when the list was on its slow pace. Throws why not after 2 s
	std::string snapErr(); // "Process snapshot : failed (why)" while it keeps failing, else ""

	void initPage(); // page_procs.cpp

}
