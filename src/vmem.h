#pragma once
#include "procs.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Virtual memory of one process : the region list, memory contents and what can be done to a region.
namespace vmem {

	enum State { StCommit,StReserve,StFree };
	enum Type { TyNone,TyPrivate,TyMapped,TyImage };

	// what classification found, kindToken gives the MCP name
	enum Kind {
		KNone,KUnusable,KFile,KUserShared,KHvShared,KPeb,KPeb32,KTeb,KStack,KStack32,KHeap,KHeap32,KHeapSeg,KHeapSeg32,
		KCfg,KCfg32,KApiSet,KCsr,KCodePage,KGdi,KShim,KActCtx,KWer,KSilo,KTelemetry,KParams,KLeap,KDeskHeap,KShadow,KOther
	};

	// Sizes in bytes. NA = the OS would not say. Text comes from the target, treat it as data.
	struct Region {
		uint64_t base = 0,allocBase = 0,size = 0;
		State state = StFree;
		Type type = TyNone;
		std::string prot,allocProt; // short codes RW, RX+G. Linux has + S for shared and the may flags as original
		bool rd = false,wr = false,ex = false,guard = false;
		bool allocEx = false; // allocated executable. Linux stays false, the me flag is on nearly every mapping
		bool valid = false,bad = false; // first page is resident, first page reported bad
		Kind kind = KNone;
		std::string use,file; // Use column, mapped file or image path
		uint32_t tid = 0; // stack and TEB
		int heapId = -1;
		std::string heapClass;
		uint64_t committed = 0,priv = 0;
		uint64_t ws = NA,wsPriv = NA,wsShareable = NA,wsShared = NA,wsLocked = NA;
		uint64_t pagesOrig = NA,pagesMod = NA; // resident pages of a mapped or image region : untouched, modified
		int prio = -1; // highest memory priority of the resident pages, -1 unknown
		int signing = -1; // image signing level 0..15, -1 unknown
		std::string flags; // decoded region type, Linux VmFlags
		bool unusable = false; // free but too small to allocate
	};

	struct RegionList {
		ProcRef ref;
		uint64_t t = 0,ms = 0; // util::nowMs when finished, how long it took
		std::vector<Region> rows; // sorted by base
		std::string err; // the query failed, rows is empty
		std::string note; // partial result and why
		std::string prot; // "PPL, signer WinTcb" when protected, else empty
		bool wsOk = false,useOk = false; // the walk and the classification ran
		const Region* at(uint64_t addr) const; // null when no region holds addr
	};

	const char* stateName(State s); // Commit, Reserved, Free
	const char* typeName(Type t); // Private, Mapped, Image, ""
	const char* kindToken(Kind k); // null for KNone
	const char* prioName(int p); // Lowest..High, null when unknown
	const char* signingName(int s); // null when unknown

	// Enumeration runs on a worker, the result is a Snap per process. The UI calls refresh and draws latest.
	std::shared_ptr<const RegionList> latest(const ProcRef& r); // null before the first one finished
	void refresh(const ProcRef& r); // starts a worker, or has the running one walk once more
	bool busy(const ProcRef& r);
	std::shared_ptr<const RegionList> queryNow(const ProcRef& r,uint64_t maxAgeMs); // MCP threads, blocks, takes a list that is young enough
	void changed(const ProcRef& r); // FRS changed its memory : queryNow walks again, a list someone looks at reads again
	void prune(); // forgets lists nobody looked at for a while, every frame
	void pruneTabs(); // tab_regions.cpp, every frame : closed windows give their list back

	struct Range {
		uint64_t addr,size;
	};

	// Regions and the bytes in them to one file, unreadable bytes as zeros so offsets stay true. dirty overlays
	// unwritten editor bytes, keyed by address. Returns "EXISTS" when the file is there and over is false,
	// else the log line. Worker only, watches t.cancel and sets t.prog.
	std::string saveRanges(const ProcRef& r,const std::vector<Range>& rs,const std::string& path,bool over,task::T& t,const std::map<uint64_t,uint8_t>* dirty = nullptr);
	std::string homePath(const std::string& file); // file in the user's home folder
	std::string fileName(const ProcRef& r,const std::string& tail); // proc_tail with only safe characters
	bool parseNum(const char* s,uint64_t& out); // dialogs : 0x12ab or anything with a-f is hex, else decimal

	// floating hex editor, one window per region, drawn every frame from a context hook. UI thread only.
	void hexOpen(const ProcRef& r,uint64_t base,uint64_t size,uint64_t selOff = NA,uint64_t selLen = 0);
	void hexAt(const ProcRef& r,uint64_t addr,uint64_t len = 0); // finds the region on a worker, selects addr + len
	void hexSection(const ProcRef& owner,uint64_t handle,const std::string& name); // maps the section into FRS on a worker, Windows
	void hexDraw();

	void drawTab(const ProcRef& r); // tab_regions.cpp
	void regToolsImpl(); // tools_vmem.cpp

	struct Section {
		uint64_t handle = 0; // Linux : inode
		std::string name,type,file;
		uint64_t size = NA;
		std::string best; // what to call it : name, file or type and size
		uint64_t addr = 0,view = 0; // Linux : where it is mapped, the editor opens there
	};

	struct MapUser {
		uint32_t pid = 0;
		uint64_t start = 0;
		std::string name,path;
		uint64_t lo = 0,hi = 0;
		std::string view; // Image or Mapped
	};

	// the OS seam
	namespace be {

		void regions(const ProcRef& r,RegionList& out,const std::atomic<bool>* stop);

		// addr..addr+n into out, what can't be read stays 0. map gets a byte per 4 KiB page from addr's page :
		// 0 unreadable, 1 read only, 2 writable. Guard and no access pages are never touched, a read would
		// use up the guard. Returns bytes read, err says why when the process can't be opened.
		size_t read(const ProcRef& r,uint64_t addr,size_t n,uint8_t* out,uint8_t* map,std::string& err);
		size_t write(const ProcRef& r,uint64_t addr,const uint8_t* data,size_t n,std::string& err);
		bool regionAt(const ProcRef& r,uint64_t addr,Region& out,std::string& err); // base, size, state, type, protection

		bool protect(const ProcRef& r,uint64_t addr,uint64_t& size,uint32_t prot,std::string& old,std::string& err);
		bool parseProt(const std::string& s,uint32_t& out); // RW or 0x04, checks the value, false on junk or where there is no remote protect
		const char* protHelp(); // what protect takes, for messages
		bool freeMem(const ProcRef& r,uint64_t addr,uint64_t size,bool decommit,std::string& did,std::string& err); // did : released, unmapped, decommitted
		bool emptyWs(const ProcRef& r,uint64_t addr,uint64_t size,std::string& note,std::string& err);
		bool canChange(); // protect and free work on this OS
		std::string critical(const ProcRef& r); // why MCP keeps off it : critical (its death stops the OS) or protected, empty when fine

		bool sections(const ProcRef& r,std::vector<Section>& out,std::string& err,const std::atomic<bool>* stop);
		bool findMapped(const std::string& needle,std::vector<MapUser>& out,int& unreadable,const std::atomic<bool>* stop);
		bool mapSection(const ProcRef& owner,uint64_t handle,uint64_t& addr,uint64_t& size,bool& writable,std::string& err);
		void unmapSection(uint64_t addr);
		ProcRef self(); // FRS itself, the section editor reads through this

		void* fileOpen(const std::string& path,bool over,int& rc,std::string& err,bool priv = false); // rc 1 file exists, -1 failed. priv : owner only, it holds memory
		bool fileWrite(void* f,const void* data,size_t n);
		void fileClose(void* f);
		std::string home(); // the user's profile folder in UTF-8, empty when unknown

	}

}
