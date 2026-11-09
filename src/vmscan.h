#pragma once
#include "procs.h"
#include <atomic>
#include <bitset>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Memory scans and dumps of one process. Engine in vmscan.cpp, OS part in vmscan_win.cpp and vmscan_linux.cpp.
// Everything here blocks : run it on task::run or an MCP handler thread, never on the UI thread.
namespace vmscan {

	enum Kind { KPrivate = 1,KImage = 2,KMapped = 4 };
	enum Enc : uint8_t { EAnsi,EUtf16,EUtf8,EBytes,ERegex };
	enum Mode { MStrings,MBytes,MRegex };

	struct Region {
		uint64_t base = 0,size = 0;
		uint32_t prot = 0; // OS flags, os::protStr prints them
		int kind = KPrivate;
	};

	struct Hit {
		uint64_t addr = 0,bytes = 0; // where it sits and how long it is in memory
		uint64_t chars = 0;
		uint32_t idx = 0; // discovery order, from 1
		uint32_t reg = 0; // into Scan::regs
		Enc enc = EAnsi;
		std::string text; // util::safe, cut at Cfg::textCap chars
	};

	// Thompson NFA over bytes. No backtracking and no recursion while matching, so a hostile pattern or a
	// 4 MiB subject can't hang it or overflow the stack, std::regex does both.
	class Rx {
	public:
		bool compile(const std::string& pat,bool icase,std::string& err);
		bool ok() const { return !prog.empty(); }
		bool match(const char* s,size_t n) const; // anywhere in s
		// leftmost longest non empty match starting at from or later, maxLen bytes at most
		bool find(const uint8_t* s,size_t n,size_t from,size_t maxLen,size_t& b,size_t& e) const;
		struct Ins { uint8_t op; int x,y; };
	private:
		bool run(const uint8_t* s,size_t n,size_t from,size_t maxLen,bool any,size_t& b,size_t& e) const;
		std::vector<Ins> prog;
		std::vector<std::bitset<256>> sets;
		std::bitset<256> first;
		bool useFirst = false;
	};

	struct Cfg {
		Mode mode = MStrings;
		int minLen = 10;
		bool ext = false;
		int kinds = KPrivate;
		int threads = 1; // 0 auto
		uint64_t from = 0; // skip everything below this address
		uint64_t maxBytes = 0,maxMs = 0,maxText = 0; // 0 no limit
		size_t maxHits = 0; // 0 no limit
		size_t textCap = 8192;
		int encMask = 31; // 1 << Enc
		std::string contains;
		std::vector<uint8_t> pat;
		std::shared_ptr<const Rx> rx;
		bool preview = true; // bytes mode : put the bytes at the hit in text
	};

	// Live state of one scan. Counters are safe from any thread, the rest under m until done.
	struct Scan {
		std::atomic<uint64_t> total{0},scanned{0},unread{0},hitN{0},textN{0};
		std::atomic<int> regTotal{0},regDone{0},regBad{0},regPartial{0},regSkipped{0};
		std::atomic<size_t> nextReg{0};
		std::atomic<bool> stop{false},done{false};
		std::mutex m;
		std::vector<Hit> pending;
		std::shared_ptr<const std::vector<Region>> regs;
		std::string err,why;
		uint64_t next = 0; // single worker cut short : scan again from here
		uint32_t idxN = 0;
		bool complete = false;
	};

	// null to go on, else why to stop. Called per chunk, from several threads.
	using StopFn = std::function<const char*()>;

	void run(const ProcRef& r,const Cfg& c,Scan& s,const StopFn& stop);
	bool parsePat(const std::string& txt,std::vector<uint8_t>& out,std::string& err); // "4D 5A" or "4d5a", junk and odd digits fail
	const char* encName(Enc e);
	const char* kindName(int kind);

	// dump presets and flags, values are MINIDUMP_TYPE
	struct Preset { const char* id; const char* label; const char* tip; uint32_t flags; };
	extern const Preset kPresets[];
	extern const int kPresetN;
	struct DFlag { const char* id; const char* label; uint32_t v; bool common; };
	extern const DFlag kFlags[];
	extern const int kFlagN;
	const uint32_t FULLMEM = 2;

	struct Prog {
		std::atomic<float> frac{0.0f};
		std::mutex m;
		std::string msg;
		void say(const std::string& t,float f = -1.0f);
		std::string text();
	};

	struct DumpRes {
		bool ok = false,cancelled = false;
		std::string path,err,note;
		uint64_t size = NA,unread = 0;
		uint32_t flags = 0; // what really went in, retries drop what an old dbghelp refuses
	};

	bool dumpAvail(); // Linux : gcore is on PATH
	// path has to be new, an existing file fails and stays as it is. A failed or cancelled dump leaves no file.
	// Linux : a core file through gcore at path.<pid>, res.path has the real name. flags and snap don't apply.
	DumpRes writeDump(const ProcRef& r,const std::string& path,uint32_t flags,bool snap,Prog& pg,const StopFn& stop);
	// raw bytes of [addr,addr+size), size 0 runs to the end of the region. Unreadable pages become zeros and are counted.
	// Fails with nothing written above cap (0 no cap) or when the disk has too little free.
	DumpRes saveRegion(const ProcRef& r,uint64_t addr,uint64_t size,const std::string& path,Prog& pg,const StopFn& stop,uint64_t cap = 0);
	uint64_t readableBytes(const ProcRef& r); // what a full memory dump is about the size of, NA when it can't tell
	std::string newName(const ProcRef& r,const char* ext); // <name>_<date>_<time>.<ext> in the default folder, _2 and up when that is taken

	void initStrings(); // tab_strings.cpp
	void initDump(); // tab_dump.cpp
	void dumpWins(); // tab_dump.cpp, each frame : a closed window cancels its dumps

	namespace os {

		struct Proc;
		std::shared_ptr<Proc> open(const ProcRef& r,std::string& err); // checks start, so a reused pid fails
		bool regions(Proc& p,std::vector<Region>& out,int& skipped,std::string& err); // committed and readable, walk order
		size_t read(Proc& p,uint64_t addr,void* buf,size_t n); // bytes read, 0 when none
		bool exited(Proc& p);
		bool regionAt(Proc& p,uint64_t addr,Region& out,std::string& err); // committed region holding addr
		std::string protStr(uint32_t prot);

		struct Out;
		std::shared_ptr<Out> create(const std::string& path,std::string& err); // new file only, never overwrites. Windows : marked for delete until close, so an exit or crash takes it along
		bool write(Out& f,const void* p,size_t n,std::string& err);
		uint64_t close(Out& f,std::string& err); // keeps the file. Size on disk, NA when it can't tell. err set : it could not be kept and is gone
		void discard(Out& f); // closes and deletes

		bool pathOk(const std::string& path,std::string& err); // full path to a file, no tricks
		bool exists(const std::string& path);
		std::string defaultDir();
		char sep();
		uint64_t freeDisk(const std::string& path); // on the volume that holds path, NA when unknown

	}

}
