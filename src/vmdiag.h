#pragma once
#include "procs.h"
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Heaps, locks, modified image pages, CFG bitmap and GDI table of one process.
// Every be:: call is slow, so task::run or an MCP thread only. Heaps, locks and flush also run a thread
// inside the target : only when someone asked, one per process (claim), never from a sampler.
namespace vmdiag {

	struct Res {
		bool ok = false;
		bool na = false; // can't be done here : this OS, this kind of target. why says it, not a failure
		std::string why; // when !ok
		bool complete = true;
		std::string reason; // when !complete
		uint64_t unixSec = 0;
	};

	// nearest export or file+0xoff, no PDBs. Sorted by base, exps by rva.
	struct SymMod {
		uint64_t base = 0,size = 0;
		std::string name;
		std::vector<std::pair<uint32_t,std::string>> exps;
	};
	using SymTab = std::vector<SymMod>;
	std::string symAt(const SymTab& t,uint64_t addr); // "" outside every module

	struct Heap {
		uint64_t base = 0,used = NA,committed = NA,entries = NA,flags = NA;
		std::string flagTxt,cls,type;
		bool def = false;
	};
	struct HeapRes : Res {
		std::vector<Heap> rows;
		bool clone = false; // read from a reflection of the target
		bool approx = false; // Linux : the [heap] VMA only
		std::string note;
	};

	struct Lock {
		uint64_t addr = 0,owner = 0;
		std::string type,file;
		int64_t lockCount = -1; // -1 free, nobody holds it
		uint64_t contention = 0,entry = 0;
		int64_t recursion = 0;
		uint64_t waitShared = 0,waitExcl = 0;
	};
	struct LockRes : Res {
		std::vector<Lock> rows;
	};

	struct ModPage {
		uint64_t base = 0,addr = 0;
		std::string file,sym;
		bool resident = false;
	};
	struct ModRes : Res {
		std::vector<ModPage> rows;
		uint64_t images = 0,skipped = 0,fails = 0,residentN = 0;
		uint64_t scanned = 0; // address space whose page states were checked
		std::string note;
	};

	enum CfgType { CfgAllValid,CfgImage,CfgModified,CfgSuppressed,CfgUnmapped };
	struct CfgEnt {
		uint64_t addr = 0,end = 0; // end only on a range where every slot is valid
		uint8_t val = 0; // 2 bit slot : 1 valid, 2 export suppressed, 3 valid also unaligned. 0 on a range
		uint8_t type = CfgModified;
		bool wow = false;
		int reg = -1; // index in regs, -1 none
	};
	struct CfgReg {
		uint64_t alloc = 0,base = 0,size = 0;
		uint32_t type = 0,state = 0,prot = 0;
		std::string typeTxt,stateTxt,protTxt,file;
	};
	struct CfgRes : Res {
		std::vector<std::string> summary;
		std::vector<CfgEnt> rows;
		std::vector<CfgReg> regs;
		SymTab syms;
		uint64_t counts[5] = {0,0,0,0,0};
		uint64_t unread = 0; // readable bitmap bytes where the read failed anyway
		uint64_t scanned = 0; // bitmap bytes read and decoded
		int enabled = -1; // process policy : 1 on, 0 off, -1 unknown
		bool truncated = false;
	};
	struct CfgCheck {
		bool ok = false;
		std::string why;
		int enabled = -1;
		int val = -1; // -1 unknown : the bitmap page behind it is not committed or unreadable, raw slot otherwise
		bool noBitmap = false; // this process has no bitmap at all
		bool wow = false;
		std::string sym;
	};
	const char* cfgTypeName(int t);
	const char* cfgMeaning(const CfgCheck& c,uint64_t addr); // what a call to addr would do

	struct GdiObj {
		uint32_t handle = 0;
		uint64_t object = 0;
		std::string type,info;
	};
	struct GdiRes : Res {
		std::vector<GdiObj> rows;
		uint64_t osCount = NA,osPeak = NA,other = 0;
		std::string note;
	};

	struct FlushRes {
		bool ok = false,na = false;
		bool held = false; // the thread is still in the target : flush keeps the claim until it ends, the caller must not unclaim
		std::string why;
		uint64_t privBefore = NA,privAfter = NA,wsBefore = NA,wsAfter = NA;
	};

	bool claim(uint32_t pid); // false while a call that runs in that target is still out
	void unclaim(uint32_t pid);
	void cacheTrim(); // drops MCP results past their paging window

	namespace be {

		bool remote(); // heaps and locks run a thread in the target on this OS
		std::shared_ptr<const HeapRes> heaps(const ProcRef& r,bool clone);
		std::shared_ptr<const LockRes> locks(const ProcRef& r);
		std::shared_ptr<const ModRes> modPages(const ProcRef& r,const std::atomic<bool>& stop);
		std::shared_ptr<const CfgRes> cfg(const ProcRef& r,const std::atomic<bool>& stop);
		CfgCheck cfgCheck(const ProcRef& r,uint64_t addr);
		std::shared_ptr<const GdiRes> gdi(const ProcRef& r);
		FlushRes flush(const ProcRef& r);

	}

}
