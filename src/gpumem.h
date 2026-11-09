#pragma once
#include "procs.h"
#include <set>

// GPU memory. Shared GPU memory is ordinary system RAM the GPU can reach, it is already inside used RAM,
// never add it on top. Dedicated is VRAM on a discrete card, not RAM.
// Naming trap : ded and shared are what is resident (Windows counters Local Usage and Non Local Usage).
// dedCommit and sharedCommit are the Windows counters named Dedicated Usage and Shared Usage, the numbers
// Task Manager shows as dedicated and shared GPU memory, so the two pairs differ. On the D3DKMT path the committed
// pair is the driver's BytesCommitted per segment instead, another number again.
struct GpuAdapter {
	std::string name,id; // id is the LUID or the card name
	uint64_t ded = NA,shared = NA; // in use. shared sums the processes on Windows like SI, sharedOs is the adapter's own counter
	uint64_t sharedOs = NA;
	uint64_t dedLimit = NA,sharedLimit = NA,sysDed = NA; // what the adapter reports, sysDed is RAM carved out for it
	uint64_t installed = NA,reserved = NA; // the driver's memory size, and that minus what is visible
};

struct GpuProc {
	uint32_t pid = 0;
	uint64_t start = 0;
	std::string name;
	uint64_t ded = NA,shared = NA,dedCommit = NA,sharedCommit = NA,commit = NA;
};

struct GpuSnap {
	uint64_t t = 0; // util::nowMs, 0 before the first sample
	bool on = false; // the option is on
	std::string why; // why there is no data, or why the first choice of source failed
	const char* source = ""; // "performance counters", "D3DKMT", "sysfs"
	const char* commitName = "Total committed"; // the commit field means something else on D3DKMT
	bool hasCommit = true; // Linux has no such number
	std::vector<GpuAdapter> adapters;
	std::vector<GpuProc> procs; // by pid, only what holds GPU memory
	uint64_t ded = NA,dedLimit = NA,shared = NA,sharedLimit = NA,sharedOs = NA;
	std::set<uint32_t> denied; // pids that could not be read, so missing from procs
	const GpuProc* find(const ProcRef& r) const;
	GpuProc now(const ProcRef& r) const; // what r holds. Not in procs is a real 0, all NA when it could not be read or there is no data
};

// rings, oldest first, MB as float, -1 unknown
struct GpuHist {
	std::vector<float> ded,shared;
	std::vector<uint64_t> unixSec;
};

struct GpuRange {
	uint64_t lo = 0,hi = 0; // zero samples don't count, like SI
};

struct GpuProcHist {
	std::vector<float> ded,shared,commit;
	std::vector<uint64_t> unixSec;
	GpuRange rDed,rShared,rDedC,rSharedC,rCommit,rTotal;
};

// one process in depth, system RAM the GPU driver holds for it and the per adapter numbers
struct GpuDetail {
	uint64_t alloc = NA,reserved = NA,wcAlloc = NA,wcReserved = NA,cachedAlloc = NA,cachedReserved = NA,sectionAlloc = NA,sectionReserved = NA;
	uint64_t virt = NA,evicted = NA; // only with the adapter statistics option on
	std::string why;
	bool gone = false; // the process turned out to have exited
};

namespace gpumem {

	void init(); // options, sampler job
	void initPage(); // page_gpu.cpp
	std::shared_ptr<const GpuSnap> latest(); // never null after init
	std::shared_ptr<const GpuHist> history(); // never null after init
	std::shared_ptr<const GpuProcHist> procHist(const ProcRef& r); // null when it never held GPU memory
	void want(const ProcRef& r); // the tab is on screen, keep the detail fresh
	std::shared_ptr<const GpuDetail> detail(const ProcRef& r); // last read, null before the first
	GpuDetail readDetail(const ProcRef& r); // fresh, not on the UI thread
	std::string kmtNote(const GpuSnap& s); // a sentence for the committed pair's tips when it was read through D3DKMT, else ""

	namespace be {

		void read(GpuSnap& s,bool perf); // source, adapters with limits, procs with pid and numbers
		GpuDetail detail(const ProcRef& r,bool adapterStats);

	}

}
