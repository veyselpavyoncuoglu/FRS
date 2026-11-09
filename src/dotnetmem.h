#pragma once
#include "procs.h"

// GC numbers of a .NET Framework process, from the CLR's public memory block. A read only section map,
// nothing gets injected and no debugging stack is involved. CLR v2 and v4 only, CoreCLR has no such block.
// genSize[0] is the Gen 0 budget that triggers the next GC, not bytes in use, so it stays out of every total.
struct ClrMem {
	enum Kind { Unknown,None,V2,V4,Core,NotHere,Gone };
	Kind kind = Unknown;
	bool wow64 = false,block = false; // block : the numbers below are real
	std::string why; // why there are no numbers
	uint64_t genCount[3] = {NA,NA,NA};
	uint64_t promoted[2] = {NA,NA};
	uint64_t promotedFinal = NA,committed = NA,reserved = NA,loh = NA,survivors = NA,handles = NA;
	uint64_t genSize[3] = {NA,NA,NA};
	uint64_t alloc = NA,allocLoh = NA,induced = NA,pinned = NA,sink = NA,loader = NA;
	uint64_t allHeaps = NA; // Gen 1 + Gen 2 + LOH
	// change since the sample before, NA on the first. Promoted ones are last-GC values, so that is all they can say.
	uint64_t rGen0 = NA,rGen1 = NA,rFinal = NA,rAlloc = NA;
	uint64_t t = 0;
};

namespace dotnetmem {

	void init(); // sampler job, it only reads what a tab is showing
	void want(const ProcRef& r);
	std::shared_ptr<const ClrMem> view(const ProcRef& r); // last read, null before the first
	std::shared_ptr<const ClrMem> read(const ProcRef& r); // fresh, not on the UI thread
	std::shared_ptr<const ClrMem> kept(ProcRef& r); // an exited target's last read, kept a minute after the last look. Fills r.name, null when none
	const char* kindName(ClrMem::Kind k); // "v4"..., null when not .NET or unknown

	namespace be {

		void read(const ProcRef& r,ClrMem& m); // all but allHeaps, the rates and t. Gone when the target turned out dead

	}

}
