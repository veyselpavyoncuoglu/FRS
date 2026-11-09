#pragma once
#include "procs.h"
#include "util.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Kernel memory : pool tags, big pools, page files. Windows reads NtQuerySystemInformation, Linux
// /proc/slabinfo, /proc/vmallocinfo and /proc/swaps. Sizes in bytes.
namespace kmem {

#ifdef _WIN32
	constexpr bool kWin = true;
#else
	constexpr bool kWin = false;
#endif

	constexpr int64_t UNK = INT64_MIN; // a counter this source does not have : N/A, MCP null

	// the 12 counters of a pool row, index into PoolRow::v and PoolRow::d
	enum { VPagedAlloc,VPagedFree,VPagedCur,VPagedBytes,VNpAlloc,VNpFree,VNpCur,VNpBytes,VAlloc,VFree,VCur,VBytes,VN };

	struct PoolRaw {
		uint64_t id = 0; // tag value, Linux : hash of the cache name
		std::string tag,desc;
		bool prot = false; // protected tag, Windows high bit
		int64_t v[VAlloc]; // the first 8, current already signed
	};

	struct PoolRow {
		uint64_t id = 0;
		std::string tag,driver,desc;
		bool prot = false;
		uint64_t seenMs = 0,hotMs = 0; // nowMs when it appeared, 0 when it was in the first sample. Last jump past the highlight threshold
		int64_t v[VN];
		int64_t d[VN]; // change over the snapshot's dtMs, UNK on a row's first sample
	};

	struct TagStat {
		bool loaded = false;
		size_t n = 0;
		std::string path,why; // why : the file is there but was refused
	};

	struct PoolSnap {
		uint64_t t = 0; // nowMs
		uint64_t dtMs = 0; // since the sample the deltas compare with, 0 on the first
		int bits = 32; // width of the alloc and free counters
		std::string err,src;
		TagStat db;
		std::vector<PoolRow> rows;
	};

	// columns of the pool table : tag, driver, description, 12 totals, 12 deltas
	constexpr int PCTag = 0,PCDriver = 1,PCDesc = 2,PCVal = 3,PCDelta = PCVal + VN,PCN = PCDelta + VN;

	struct PoolCol {
		const char* title;
		const char* lxTitle; // null : Linux has no such column
		const char* key; // MCP name
		const char* tip;
		float w;
		bool on; // Bytes preset, also the default set
		bool bytes;
		const char* lxTip = nullptr; // Linux tip when it differs
	};
	extern const PoolCol kPoolCols[PCN];

	int64_t poolVal(const PoolRow& r,int col); // UNK for text
	std::string poolCell(const PoolRow& r,int col); // N/A when unknown, blank for zero
	bool poolLess(const PoolRow& a,const PoolRow& b,int col,bool desc); // unknown goes last both ways
	bool poolMatch(const PoolRow& r,const std::string& low);

	struct BigRaw {
		uint64_t addr,size,id;
		bool np;
	};

	struct BigRow {
		uint64_t addr,size; // addr 0 : blanked by the OS
		uint32_t tag; // index into BigSnap::tags
		bool np;
	};

	struct BigTag {
		uint64_t id = 0;
		std::string tag,driver,desc;
		uint64_t n = 0,bytes = 0,npN = 0,npBytes = 0,largest = 0;
	};

	struct BigSnap {
		uint64_t t = 0;
		std::string err,src;
		std::vector<BigRow> rows;
		std::vector<BigTag> tags; // sorted by tag
		uint64_t bytes = 0,npN = 0,npBytes = 0;
		bool blanked = false; // every address came back 0
		bool tagFile = false; // pooltag.txt is loaded
	};

	struct PageFile {
		std::string path;
		uint64_t size = NA,used = NA,peak = NA,min = NA,max = NA;
	};

	struct PageSnap {
		uint64_t t = 0;
		std::string err;
		std::vector<PageFile> rows;
		bool ownPeak = false; // no kernel peak, FRS tracks it while it runs
	};

	struct CompHist {
		std::vector<float> ws,data; // MB, oldest first, only known samples
	};

	struct ObjRow {
		uint64_t handle = 0;
		std::string type;
		uint64_t paged = NA,nonPaged = NA;
	};

	struct ObjSnap {
		uint64_t t = 0;
		std::string err;
		std::vector<ObjRow> rows;
		uint64_t failed = 0; // could not be duplicated
		std::string stale; // a later refresh failed with this, rows are from t
	};

	void initData(); // page_kernel's init calls it : first values and the sampler job
	void wantPool(uint64_t ms); // keeps the sampler job reading that long
	std::shared_ptr<const PoolSnap> poolLatest(); // null before the first sample
	std::shared_ptr<const PoolSnap> poolFresh(); // MCP : latest, or a new read when it is older than one interval. While paused that read stays private
	std::shared_ptr<const PoolSnap> poolRead(const PoolSnap* prev); // private read, deltas against prev, publishes nothing
	void sampleNow(); // one sample of what the page shows, any thread but the UI one. While paused only the first
	std::shared_ptr<const BigSnap> bigLatest(); // null before the first read
	std::shared_ptr<const BigSnap> bigRead(); // slow, never on the UI thread and never on a tick
	std::shared_ptr<const PageSnap> pfLatest(); // null before the first read
	std::shared_ptr<const PageSnap> pfRead(bool hold = false); // fresh read, any thread but the UI one. hold : the page keeps its numbers while paused
	std::shared_ptr<const CompHist> compHist(); // never null after initData

	// optional pooltag.txt, loaded once. kmem_tags.cpp links with nothing else of FRS so it can be tested alone.
	struct TagInfo {
		std::string driver,desc;
	};

	struct TagDb {
		std::unordered_map<std::string,TagInfo> exact;
		std::vector<std::pair<std::string,TagInfo>> wild; // tags with ?
		const TagInfo* find(const std::string& tag) const; // tag without its padding. Exact first, then the most specific ?
		size_t size() const { return exact.size() + wild.size(); }
	};
	void parseTags(const std::string& txt,TagDb& db);

	// OS part, kmem_win.cpp and kmem_linux.cpp. Strings are "" or why it failed.
	std::string osPool(std::vector<PoolRaw>& out,int& bits,std::string& src);
	std::string osBig(std::vector<BigRaw>& out,std::unordered_map<uint64_t,std::string>& names,std::string& src);
	std::string osPages(PageSnap& s);
	bool osTagFile(std::string& txt,std::string& path,std::string& why); // false when there is no usable file
	std::string osObjects(const ProcRef& r,ObjSnap& s,const std::atomic<bool>& cancel);

}
