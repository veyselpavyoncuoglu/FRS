#ifdef _WIN32
#include "sysmem.h"
#include "smbios.h"
#include "nt.h"
#include <psapi.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>

namespace {

	// phnt ntexapi.h
	struct SBI {
		ULONG Reserved,TimerResolution,PageSize,NumberOfPhysicalPages,LowestPhysicalPageNumber,HighestPhysicalPageNumber,AllocationGranularity;
		ULONG_PTR MinimumUserModeAddress,MaximumUserModeAddress;
		KAFFINITY ActiveProcessorsAffinityMask;
		CHAR NumberOfProcessors;
	};

	struct SPERF {
		LARGE_INTEGER IdleProcessTime,IoReadTransferCount,IoWriteTransferCount,IoOtherTransferCount;
		ULONG IoReadOperationCount,IoWriteOperationCount,IoOtherOperationCount;
		ULONG AvailablePages,CommittedPages,CommitLimit,PeakCommitment,PageFaultCount,CopyOnWriteCount,TransitionCount,CacheTransitionCount,DemandZeroCount;
		ULONG PageReadCount,PageReadIoCount,CacheReadCount,CacheIoCount,DirtyPagesWriteCount,DirtyWriteIoCount,MappedPagesWriteCount,MappedWriteIoCount;
		ULONG PagedPoolPages,NonPagedPoolPages,PagedPoolAllocs,PagedPoolFrees,NonPagedPoolAllocs,NonPagedPoolFrees,FreeSystemPtes;
		ULONG ResidentSystemCodePage,TotalSystemDriverPages,TotalSystemCodePages,NonPagedPoolLookasideHits,PagedPoolLookasideHits,AvailablePagedPoolPages;
		ULONG ResidentSystemCachePage,ResidentPagedPoolPage,ResidentSystemDriverPage;
		ULONG CcFastReadNoWait,CcFastReadWait,CcFastReadResourceMiss,CcFastReadNotPossible;
		ULONG CcFastMdlReadNoWait,CcFastMdlReadWait,CcFastMdlReadResourceMiss,CcFastMdlReadNotPossible;
		ULONG CcMapDataNoWait,CcMapDataWait,CcMapDataNoWaitMiss,CcMapDataWaitMiss,CcPinMappedDataCount;
		ULONG CcPinReadNoWait,CcPinReadWait,CcPinReadNoWaitMiss,CcPinReadWaitMiss;
		ULONG CcCopyReadNoWait,CcCopyReadWait,CcCopyReadNoWaitMiss,CcCopyReadWaitMiss;
		ULONG CcMdlReadNoWait,CcMdlReadWait,CcMdlReadNoWaitMiss,CcMdlReadWaitMiss;
		ULONG CcReadAheadIos,CcLazyWriteIos,CcLazyWritePages,CcDataFlushes,CcDataPages;
		ULONG ContextSwitches,FirstLevelTbFills,SecondLevelTbFills,SystemCalls;
		ULONGLONG CcTotalDirtyPages,CcDirtyPageThreshold; // since Windows 10
		LONGLONG ResidentAvailablePages;
		ULONGLONG SharedCommittedPages;
		ULONGLONG MdlPagesAllocated,PfnDatabaseCommittedPages,SystemPageTableCommittedPages,ContiguousPagesAllocated; // since 24H2
	};

	struct SML {
		SIZE_T ZeroPageCount,FreePageCount,ModifiedPageCount,ModifiedNoWritePageCount,BadPageCount;
		SIZE_T PageCountByPriority[8];
		SIZE_T RepurposedPagesByPriority[8];
		SIZE_T ModifiedPageCountPageFile;
	};

	struct SFC {
		SIZE_T CurrentSize,PeakSize;
		ULONG PageFaultCount;
		SIZE_T MinimumWorkingSet,MaximumWorkingSet,CurrentSizeIncludingTransitionInPages,PeakSizeIncludingTransitionInPages;
		ULONG TransitionRePurposeCount,Flags;
	};

	struct SSI {
		ULONG Version,StoreInformationClass;
		PVOID Data;
		ULONG Length;
	};

	struct SCR {
		ULONG Version,CompressionPid,WorkingSetSize;
		SIZE_T TotalDataCompressed,TotalCompressedSize,TotalUniqueDataCompressed;
	};

	struct MCX {
		HANDLE EventHandle;
		SIZE_T PagesCombined;
		ULONG Flags;
	};

#ifdef _WIN64
	static_assert(sizeof(SBI) == 0x40,"SBI");
	static_assert(offsetof(SPERF,AvailablePages) == 0x2C,"SPERF");
	static_assert(offsetof(SPERF,PageFaultCount) == 0x3C,"SPERF");
	static_assert(offsetof(SPERF,PagedPoolPages) == 0x70,"SPERF");
	static_assert(offsetof(SPERF,ResidentSystemCodePage) == 0x8C,"SPERF");
	static_assert(offsetof(SPERF,ResidentSystemCachePage) == 0xA4,"SPERF");
	static_assert(offsetof(SPERF,CcLazyWritePages) == 0x11C,"SPERF");
	static_assert(offsetof(SPERF,CcTotalDirtyPages) == 0x138,"SPERF");
	static_assert(offsetof(SPERF,MdlPagesAllocated) == 0x158,"SPERF");
	static_assert(sizeof(SPERF) == 0x178,"SPERF");
	static_assert(sizeof(SML) == 176,"SML");
	static_assert(sizeof(SFC) == 64,"SFC");
	static_assert(sizeof(SSI) == 24,"SSI");
	static_assert(sizeof(SCR) == 40,"SCR");
	static_assert(sizeof(MCX) == 24,"MCX");
#endif

	const ULONG kBasic = 0,kPerf = 2,kStore = 109,kMemList = 80,kFileCache = 81,kCombine = 130,kRegFlush = 155;

	nt::Status query(ULONG cls,void* p,ULONG n,ULONG* ret = nullptr) {
		typedef nt::Status(NTAPI* Fn)(ULONG,PVOID,ULONG,PULONG);
		static auto f = nt::fn<Fn>("NtQuerySystemInformation");
		return f ? f(cls,p,n,ret) : nt::NOT_IMPLEMENTED;
	}

	nt::Status setInfo(ULONG cls,void* p,ULONG n) {
		typedef nt::Status(NTAPI* Fn)(ULONG,PVOID,ULONG);
		static auto f = nt::fn<Fn>("NtSetSystemInformation");
		return f ? f(cls,p,n) : nt::NOT_IMPLEMENTED;
	}

	std::string toUtf8(const wchar_t* s,int n = -1) {
		if (n == 0) return "";
		int sz = WideCharToMultiByte(CP_UTF8,0,s,n,nullptr,0,nullptr,nullptr);
		if (sz <= 0) return "";
		std::string r(sz,'\0');
		WideCharToMultiByte(CP_UTF8,0,s,n,&r[0],sz,nullptr,nullptr);
		if (n < 0 && !r.empty()) r.pop_back();
		return r;
	}

	// one line. Status tables often start with a "{Title}" line.
	std::string msgText(HMODULE mod,DWORD code) {
		wchar_t buf[512];
		DWORD fl = FORMAT_MESSAGE_IGNORE_INSERTS | (mod ? FORMAT_MESSAGE_FROM_HMODULE : FORMAT_MESSAGE_FROM_SYSTEM);
		DWORD n = FormatMessageW(fl,mod,code,0,buf,512,nullptr);
		if (!n) return "";
		std::string s = toUtf8(buf,(int)n);
		size_t br = s.find('}');
		if (!s.empty() && s[0] == '{' && br != std::string::npos) s = s.substr(br + 1);
		for (auto& ch : s) if (ch == '\r' || ch == '\n') ch = ' ';
		while (!s.empty() && s.back() == ' ') s.pop_back();
		while (!s.empty() && s[0] == ' ') s.erase(0,1);
		return util::safe(s);
	}

	std::string statusText(nt::Status st) {
		std::string s = msgText(GetModuleHandleW(L"ntdll.dll"),(DWORD)st);
		if (!s.empty()) return s;
		char b[32];
		snprintf(b,sizeof(b),"NTSTATUS 0x%08lX",(unsigned long)st);
		return b;
	}

	std::string lastErrText(DWORD e) {
		std::string s = msgText(nullptr,e);
		return s.empty() ? "error " + std::to_string(e) : s;
	}

	sysmem::CmdRes result(nt::Status st,const char* good,const char* bad) {
		sysmem::CmdRes r;
		r.ok = nt::ok(st);
		r.why = r.ok ? "" : statusText(st);
		r.msg = r.ok ? good : std::string(bad) + " " + r.why;
		return r;
	}

	sysmem::CmdRes listCmd(ULONG cmd,const char* good,const char* bad) {
		return result(setInfo(kMemList,&cmd,sizeof(cmd)),good,bad);
	}

	sysmem::CmdRes combine() {
		MCX c = {};
		nt::Status st = setInfo(kCombine,&c,sizeof(c));
		sysmem::CmdRes r = result(st,"","Unable to combine memory pages.");
		if (r.ok) r.msg = "Memory pages combined : " + util::size((uint64_t)c.PagesCombined * 4096) + " (" + util::count(c.PagesCombined) + " pages)";
		return r;
	}

	// SI prints the size from before the call as if that much was freed, say both ends instead
	sysmem::CmdRes fileCache() {
		SFC b = {};
		nt::Status qs = query(kFileCache,&b,sizeof(b));
		SFC s = {};
		s.MinimumWorkingSet = (SIZE_T)-1;
		s.MaximumWorkingSet = (SIZE_T)-1;
		nt::Status st = setInfo(kFileCache,&s,sizeof(s));
		sysmem::CmdRes r = result(st,"","Unable to empty system file cache.");
		if (!r.ok) return r;
		SFC a = {};
		nt::Status qa = query(kFileCache,&a,sizeof(a));
		r.msg = "System file cache emptied : " + util::size(nt::ok(qs) ? b.CurrentSize : NA) + " before, " + util::size(nt::ok(qa) ? a.CurrentSize : NA) + " after. The pages went to standby";
		return r;
	}

	uint32_t compressionPid() {
		SCR c = {};
		c.Version = 3;
		SSI s = {};
		s.Version = 1;
		s.StoreInformationClass = 22;
		s.Data = &c;
		s.Length = sizeof(c);
		if (nt::ok(query(kStore,&s,sizeof(s))) && c.CompressionPid) return c.CompressionPid;
		std::vector<char> buf;
		if (!nt::ok(nt::querySys(5,buf))) return 0;
		for (size_t off = 0; off + sizeof(nt::SPI) <= buf.size();) {
			auto* p = (const nt::SPI*)(buf.data() + off);
			if (p->ImageName.Buffer && p->ImageName.Length == 36 && !wcsncmp(p->ImageName.Buffer,L"Memory Compression",18)) return (uint32_t)(ULONG_PTR)p->UniqueProcessId;
			if (!p->NextEntryOffset) break;
			off += p->NextEntryOffset;
		}
		return 0;
	}

	// SI goes through its kernel driver here. Same trim from user mode, Windows normally denies the open.
	sysmem::CmdRes compression() {
		sysmem::CmdRes r;
		uint32_t pid = compressionPid();
		if (!pid) {
			r.na = true;
			r.why = "Memory Compression process not found, compression may be off";
			r.msg = "Unable to empty compression store. " + r.why;
			return r;
		}
		HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
		if (!h) {
			DWORD e = GetLastError();
			r.na = e == ERROR_ACCESS_DENIED;
			r.why = lastErrText(e) + " Memory Compression is a protected process, System Informer uses its signed kernel driver for this";
			r.msg = "Unable to empty compression store. " + r.why;
			return r;
		}
		PROCESS_MEMORY_COUNTERS b = {};
		PROCESS_MEMORY_COUNTERS a = {};
		bool hb = GetProcessMemoryInfo(h,&b,sizeof(b)) != FALSE;
		if (SetProcessWorkingSetSize(h,(SIZE_T)-1,(SIZE_T)-1)) {
			bool ha = GetProcessMemoryInfo(h,&a,sizeof(a)) != FALSE;
			r.ok = true;
			r.msg = "Compression store trimmed : " + util::size(hb ? b.WorkingSetSize : NA) + " before, " + util::size(ha ? a.WorkingSetSize : NA) + " after";
		} else {
			DWORD e = GetLastError();
			r.na = e == ERROR_ACCESS_DENIED;
			r.why = lastErrText(e);
			r.msg = "Unable to empty compression store. " + r.why;
		}
		CloseHandle(h);
		return r;
	}

	// empty means flushed, otherwise the reason. err is 0 on success.
	std::string flushOne(const std::wstring& vol,DWORD& err) {
		std::wstring path = vol;
		if (!path.empty() && path.back() == L'\\') path.pop_back();
		HANDLE h = CreateFileW(path.c_str(),FILE_WRITE_DATA | SYNCHRONIZE,FILE_SHARE_READ | FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
		err = 0;
		if (h == INVALID_HANDLE_VALUE) err = GetLastError();
		else {
			if (!FlushFileBuffers(h)) err = GetLastError();
			CloseHandle(h);
		}
		return err ? lastErrText(err) : "";
	}

	// Per volume results. SI's status is whatever the last volume gave, so a card reader with no card fails the lot.
	sysmem::CmdRes flushVolumes() {
		sysmem::CmdRes r;
		std::vector<std::wstring> vols;
		wchar_t name[MAX_PATH];
		HANDLE fv = FindFirstVolumeW(name,MAX_PATH);
		if (fv == INVALID_HANDLE_VALUE) {
			r.why = lastErrText(GetLastError());
			r.msg = "Unable to flush volume file cache. " + r.why;
			return r;
		}
		do vols.push_back(name); while (FindNextVolumeW(fv,name,MAX_PATH));
		FindVolumeClose(fv);
		int okN = 0;
		int bad = 0;
		std::string first;
		for (auto& v : vols) {
			wchar_t paths[512] = {};
			DWORD got = 0;
			std::string shown = GetVolumePathNamesForVolumeNameW(v.c_str(),paths,512,&got) && paths[0] ? toUtf8(paths) : toUtf8(v.c_str());
			auto err = std::make_shared<DWORD>(0);
			std::string txt;
			std::wstring vol = v;
			if (!sysmem::bounded([vol,err] { return flushOne(vol,*err); },20000,txt)) {
				r.items.push_back({shown,"Timed out after 20 s"});
				bad++;
				if (first.empty()) first = shown + " timed out";
				continue;
			}
			if (!*err) {
				r.items.push_back({shown,"Flushed"});
				okN++;
			} else if (*err == ERROR_NOT_READY || *err == ERROR_WRITE_PROTECT || *err == ERROR_NO_MEDIA_IN_DRIVE) {
				r.items.push_back({shown,"Skipped, " + txt});
			} else {
				r.items.push_back({shown,txt});
				bad++;
				if (first.empty()) first = shown + " : " + txt;
			}
		}
		r.ok = !bad;
		r.why = first;
		r.msg = r.ok ? "Volume file cache flushed to disk, " + std::to_string(okN) + " volumes" : "Unable to flush " + std::to_string(bad) + " volumes, " + std::to_string(okN) + " flushed. " + first;
		return r;
	}

	uint64_t installedBytes() {
		typedef BOOL(WINAPI* Fn)(PULONGLONG);
		static uint64_t v = [] {
			auto f = (Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetPhysicallyInstalledSystemMemory");
			ULONGLONG kb = 0;
			return f && f(&kb) && kb ? (uint64_t)kb * 1024 : NA;
		}();
		return v;
	}

	// this kernel stopped keeping some counters, they sit at 0 since boot. That is unknown, not 0 /s.
	uint64_t kept(ULONG v) {
		return v ? v : NA;
	}

	// got 0 : the query failed. Every key stays, unknown goes out as null.
	void perfExtras(SysMem& m,const SPERF& p,ULONG got) {
		uint64_t pg = m.pageSize;
		auto ex = [&m,pg,got](const char* k,uint64_t pages,ULONG need = 1) { m.extra.push_back({k,got >= need ? pages * pg : NA}); };
		ex("Total system code",p.TotalSystemCodePages);
		ex("Total system drivers",p.TotalSystemDriverPages);
		ex("Available paged pool",p.AvailablePagedPoolPages);
		const ULONG win10 = offsetof(SPERF,MdlPagesAllocated);
		ex("Cache dirty pages",p.CcTotalDirtyPages,win10);
		ex("Cache dirty threshold",p.CcDirtyPageThreshold,win10);
		ex("Resident available",p.ResidentAvailablePages > 0 ? (uint64_t)p.ResidentAvailablePages : 0,win10);
		ex("Shared committed",p.SharedCommittedPages,win10);
		const ULONG h24 = sizeof(SPERF);
		ex("MDL pages",p.MdlPagesAllocated,h24);
		ex("PFN database",p.PfnDatabaseCommittedPages,h24);
		ex("System page tables",p.SystemPageTableCommittedPages,h24);
		ex("Contiguous pages",p.ContiguousPagesAllocated,h24);
		auto cn = [&m,got](const char* k,uint64_t v) { m.counts.push_back({k,got ? v : NA}); };
		cn("Copy on write faults",p.CopyOnWriteCount);
		cn("Transition faults",p.TransitionCount);
		cn("Cache transition faults",kept(p.CacheTransitionCount));
		cn("Demand zero faults",p.DemandZeroCount);
		cn("Page read IOs",p.PageReadIoCount);
		cn("Cache reads",kept(p.CacheReadCount));
		cn("Cache IOs",kept(p.CacheIoCount));
		cn("Dirty write IOs",p.DirtyWriteIoCount);
		cn("Mapped write IOs",p.MappedWriteIoCount);
		// this kernel can run the counter below 0, typeperf shows the same wrapped number just under 2^32
		cn("Free system PTEs",(int32_t)p.FreeSystemPtes < 0 ? NA : (uint64_t)p.FreeSystemPtes);
	}

	void readPerf(SysMem& m) {
		SBI bi = {};
		nt::Status st = query(kBasic,&bi,sizeof(bi));
		uint64_t pg = nt::ok(st) && bi.PageSize ? bi.PageSize : 4096;
		m.pageSize = pg;
		if (nt::ok(st)) m.physTotal = (uint64_t)bi.NumberOfPhysicalPages * pg;
		m.physInstalled = installedBytes();

		SPERF p = {};
		ULONG got = 0;
		st = nt::INFO_LENGTH_MISMATCH;
		const ULONG sizes[] = {(ULONG)sizeof(SPERF),0x158,0x138};
		for (ULONG n : sizes) {
			got = 0;
			st = query(kPerf,&p,n,&got);
			if (st != nt::INFO_LENGTH_MISMATCH) break;
		}
		if (!nt::ok(st)) {
			m.perfWhy = statusText(st);
			perfExtras(m,p,0);
			return;
		}
		if (!got) got = 0x138;
		auto b = [pg](uint64_t pages) { return pages * pg; };
		m.physAvail = b(p.AvailablePages);
		m.commit = b(p.CommittedPages);
		m.commitLimit = b(p.CommitLimit);
		m.commitPeak = b(p.PeakCommitment);
		m.cacheWs = b(p.ResidentSystemCachePage);
		m.kernelWs = b(p.ResidentSystemCodePage);
		m.driverWs = b(p.ResidentSystemDriverPage);
		m.pagedWs = b(p.ResidentPagedPoolPage);
		m.pagedVs = b(p.PagedPoolPages);
		m.nonPaged = b(p.NonPagedPoolPages);
		// documented limit : RAM or 16 TB. SI's comment says TB and its code uses GB.
		const uint64_t lim = 16ULL << 40;
		if (m.physTotal != NA) m.nonPagedLimit = m.physTotal < lim ? m.physTotal : lim;
		m.cPagedAllocs = kept(p.PagedPoolAllocs);
		m.cPagedFrees = kept(p.PagedPoolFrees);
		m.cNonPagedAllocs = kept(p.NonPagedPoolAllocs);
		m.cNonPagedFrees = kept(p.NonPagedPoolFrees);
		m.cFaults = p.PageFaultCount;
		m.cPageReads = p.PageReadCount;
		m.cPfWrites = p.DirtyPagesWriteCount;
		m.cMappedWrites = p.MappedPagesWriteCount;
		m.cMappedReadBytes = b(p.PageReadCount);
		m.cMappedWriteBytes = b((uint64_t)p.MappedPagesWriteCount + p.DirtyPagesWriteCount + p.CcLazyWritePages);
		perfExtras(m,p,got);

		PERFORMANCE_INFORMATION pi = {};
		pi.cb = sizeof(pi);
		if (GetPerformanceInfo(&pi,sizeof(pi))) {
			m.processes = pi.ProcessCount;
			m.threads = pi.ThreadCount;
			m.handles = pi.HandleCount;
		}
	}

	void readLists(SysMem& m) {
		SML l = {};
		nt::Status st = query(kMemList,&l,sizeof(l));
		if (!nt::ok(st)) {
			m.listsWhy = statusText(st);
			return;
		}
		uint64_t pg = m.pageSize;
		m.zeroed = l.ZeroPageCount * pg;
		m.free = l.FreePageCount * pg;
		m.modified = l.ModifiedPageCount * pg;
		m.modifiedNoWrite = l.ModifiedNoWritePageCount * pg;
		m.modifiedPf = l.ModifiedPageCountPageFile * pg;
		m.bad = l.BadPageCount * pg;
		m.standby = 0;
		m.repurposedTotal = 0;
		for (int i = 0; i < 8; i++) {
			m.standbyPrio[i] = l.PageCountByPriority[i] * pg;
			m.repurposed[i] = l.RepurposedPagesByPriority[i] * pg;
			m.standby += m.standbyPrio[i];
			m.repurposedTotal += m.repurposed[i];
		}
	}

	void readCache(SysMem& m) {
		SFC c = {};
		nt::Status st = query(kFileCache,&c,sizeof(c));
		if (!nt::ok(st)) {
			m.cacheWhy = statusText(st);
			m.counts.push_back({"File cache page faults",NA});
			m.counts.push_back({"File cache transition repurposes",NA});
			return;
		}
		m.fileCache = c.CurrentSize;
		m.fileCachePeak = c.PeakSize;
		m.fileCacheMin = (uint64_t)c.MinimumWorkingSet * m.pageSize; // pages, unlike the two above
		m.fileCacheMax = (uint64_t)c.MaximumWorkingSet * m.pageSize;
		m.fileCacheTrans = (uint64_t)c.CurrentSizeIncludingTransitionInPages * m.pageSize;
		m.counts.push_back({"File cache page faults",c.PageFaultCount});
		m.counts.push_back({"File cache transition repurposes",c.TransitionRePurposeCount});
	}

	void readComp(SysMem& m) {
		SCR c = {};
		c.Version = 3;
		SSI s = {};
		s.Version = 1;
		s.StoreInformationClass = 22;
		s.Data = &c;
		s.Length = sizeof(c);
		nt::Status st = query(kStore,&s,sizeof(s));
		if (!nt::ok(st)) {
			m.compWhy = statusText(st);
			return;
		}
		m.compPid = c.CompressionPid;
		if (!c.CompressionPid) {
			m.compWhy = "Memory compression is off";
			return;
		}
		m.compWs = c.WorkingSetSize;
		m.compData = c.TotalDataCompressed;
		m.compSize = c.TotalCompressedSize;
		m.compUnique = c.TotalUniqueDataCompressed;
	}

}

namespace sysmem {

	void osRead(SysMem& m) {
		readPerf(m);
		readLists(m);
		readCache(m);
		readComp(m);
	}

	CmdRes osCmd(int idx) {
		switch (idx) {
		case 0: return combine();
		case 1: return compression();
		case 2: return fileCache();
		case 3: return result(setInfo(kRegFlush,nullptr,0),"Registry cache emptied.","Unable to empty registry cache.");
		case 4: return listCmd(2,"Working sets emptied.","Unable to empty working sets.");
		case 5: return listCmd(3,"Modified page lists emptied.","Unable to empty modified page lists.");
		case 6: return flushVolumes();
		case 7: return listCmd(4,"Standby lists emptied.","Unable to empty standby lists.");
		case 8: return listCmd(5,"Priority 0 standby list emptied.","Unable to empty priority 0 standby list.");
		}
		CmdRes r;
		r.why = "unknown command";
		r.msg = r.why;
		return r;
	}

	bool osCompact(CmdRes&) {
		return false;
	}

	bool osArgSysinfo() {
		int n = 0;
		LPWSTR* a = CommandLineToArgvW(GetCommandLineW(),&n);
		if (!a) return false;
		bool r = false;
		for (int i = 1; i < n; i++) {
			if (_wcsicmp(a[i],L"-sysinfo")) continue;
			r = i + 1 >= n || !_wcsicmp(a[i + 1],L"Memory");
		}
		LocalFree(a);
		return r;
	}

}

namespace smbios {

	bool osRead(std::vector<uint8_t>& out,int& major,int& minor,std::string& err) {
		const DWORD sig = 'R' << 24 | 'S' << 16 | 'M' << 8 | 'B';
		UINT need = GetSystemFirmwareTable(sig,0,nullptr,0);
		if (!need) {
			err = "GetSystemFirmwareTable : " + lastErrText(GetLastError());
			return false;
		}
		std::vector<uint8_t> b(need);
		UINT got = GetSystemFirmwareTable(sig,0,b.data(),need);
		if (!got || got > need) {
			err = "GetSystemFirmwareTable : " + lastErrText(GetLastError());
			return false;
		}
		// RawSMBIOSData : used20, major, minor, dmi revision, DWORD length, table
		if (got < 8) {
			err = "SMBIOS table too short";
			return false;
		}
		major = b[1];
		minor = b[2];
		uint32_t len = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
		if (len > got - 8) len = got - 8;
		out.assign(b.begin() + 8,b.begin() + 8 + len);
		return true;
	}

}
#endif // _WIN32
