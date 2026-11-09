#pragma once
#include "util.h"
#include <string>
#include <utility>
#include <vector>

// System wide memory numbers, sampled in the background. All sizes in bytes, NA when unknown
// on this OS or without rights. Delta fields are per second.
struct SysMem {
	uint64_t t = 0; // util::nowMs at sample time
	uint64_t pageSize = 4096;

	uint64_t physTotal = NA,physAvail = NA,physUsed = NA,reserved = NA;
	uint64_t commit = NA,commitPeak = NA,commitLimit = NA;
	uint64_t cacheWs = NA,kernelWs = NA,driverWs = NA;
	uint64_t pagedWs = NA,pagedVs = NA,nonPaged = NA,nonPagedLimit = NA;
	uint64_t dPagedAllocs = NA,dPagedFrees = NA,dNonPagedAllocs = NA,dNonPagedFrees = NA;
	uint64_t dFaults = NA,dPageReads = NA,dPfWrites = NA,dMappedWrites = NA;
	uint64_t dMappedReadBytes = NA,dMappedWriteBytes = NA;

	// page lists
	uint64_t zeroed = NA,free = NA,modified = NA,modifiedNoWrite = NA,modifiedPf = NA,bad = NA;
	uint64_t standby = NA; // sum of standbyPrio
	uint64_t standbyPrio[8] = {NA,NA,NA,NA,NA,NA,NA,NA};
	uint64_t repurposed[8] = {NA,NA,NA,NA,NA,NA,NA,NA};

	// file cache and compression
	uint64_t fileCache = NA,fileCachePeak = NA,fileCacheMin = NA,fileCacheMax = NA,fileCacheTrans = NA;
	uint64_t compWs = NA,compData = NA,compSize = NA;
	uint32_t compPid = 0;

	uint32_t processes = 0,threads = 0,handles = 0;

	// labeled extras only one OS has (swap, slab, shmem, resident available...), bytes
	std::vector<std::pair<std::string,uint64_t>> extra;

	uint64_t physInstalled = NA;
	uint64_t repurposedTotal = NA;
	uint64_t compUnique = NA;
	// counters since boot, the deltas come from these
	uint64_t cPagedAllocs = NA,cPagedFrees = NA,cNonPagedAllocs = NA,cNonPagedFrees = NA;
	uint64_t cFaults = NA,cPageReads = NA,cPfWrites = NA,cMappedWrites = NA;
	uint64_t cMappedReadBytes = NA,cMappedWriteBytes = NA;
	// pool size change per second, valid when its has flag is set (both samples known)
	int64_t dPagedBytes = 0,dNonPagedBytes = 0;
	bool hasPagedDelta = false,hasNonPagedDelta = false;
	bool peakSinceStart = false; // Linux has no kernel peak, commitPeak only covers this FRS run
	std::string perfWhy,listsWhy,cacheWhy,compWhy; // why that group is NA
	std::vector<std::pair<std::string,uint64_t>> counts; // like extra, plain counts
};

// rings, oldest first, all the same length. MB as float keeps them small. -1 is an unknown sample.
struct SysHist {
	std::vector<float> commit,phys,cache,kernel,standby,modified,freeMem;
	std::vector<uint64_t> unixSec;
	// biggest process at each sample, for graph tooltips. By working set, topPriv by private bytes.
	std::vector<std::string> topProc;
	std::vector<uint64_t> topProcBytes;
	std::vector<std::string> topPriv;
	std::vector<uint64_t> topPrivBytes;
};

namespace sysmem {

	void init(); // registers page, sampler job, options, palette commands
	void regTools();
	std::shared_ptr<const SysMem> latest(); // never null after init
	std::shared_ptr<const SysHist> history(); // never null after init
	std::shared_ptr<const SysMem> sampleNow(); // fresh read without deltas, any thread but the UI one

	// Empty menu, System Informer's commands in its order
	struct Cmd { const char* id; const char* label; const char* working; const char* tip; };
	extern const Cmd kCmds[];
	extern const int kCmdN;

	struct CmdRes {
		bool ok = false;
		bool na = false; // can't run here : not on this OS, protected, nothing to act on. Not a failure, ok stays false
		std::string msg,why; // msg is the full line, why only the reason for "Label : failed (why)"
		std::vector<std::pair<std::string,std::string>> items; // per volume and such
	};

	struct Step {
		std::string id,label;
		int st = 0; // 0 waiting, 1 running, 2 ok, 3 failed, 4 cancelled, 5 not available here
		CmdRes res;
	};

	struct Run {
		std::mutex m; // guards steps
		std::string id;
		std::vector<Step> steps;
		std::atomic<int> nDone{0};
		std::atomic<bool> cancel{false},began{false},done{false};
		std::shared_ptr<const SysMem> before,after; // only after done
	};

	// starts on a task, returns "" or why it refused (a clean runs, a command runs, needs admin)
	std::string start(const std::string& id,bool fromMcp,std::shared_ptr<Run>* out = nullptr);
	std::shared_ptr<Run> lastRun(); // null before the first command
	bool busy();
	// one kCmds command by id, synchronously on the calling thread (never the UI one), for the clean set. Fakes like start() in a test exe.
	// Returns "<label> : OK", " : failed (why)" or " : not available (why)" without a prefix, logs nothing, lastRun stays as it is
	std::string runStep(const char* id);
	void drawPage();
	void regPalette();

	// fn on its own thread, false when it outlives ms. A stuck volume can't be interrupted, the thread stays behind.
	bool bounded(std::function<std::string()> fn,int ms,std::string& out);

	// OS part, sysmem_win.cpp and sysmem_linux.cpp
	void osRead(SysMem& m); // everything but the deltas, any thread
	CmdRes osCmd(int idx); // idx in kCmds
	bool osCompact(CmdRes& r); // extra last step of Empty all, false when this OS has none
	bool osArgSysinfo(); // started with -sysinfo or -sysinfo Memory

}
