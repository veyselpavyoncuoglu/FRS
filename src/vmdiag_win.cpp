#ifdef _WIN32
#include "vmdiag.h"
#include "nt.h"
#include <psapi.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <ctime>
#include <map>
#include <set>

namespace {

	using nt::Status;
	using namespace vmdiag;

	typedef Status(NTAPI* QVM)(HANDLE,PVOID,ULONG,PVOID,SIZE_T,PSIZE_T);
	typedef Status(NTAPI* QIP)(HANDLE,ULONG,PVOID,ULONG,PULONG);
	typedef void*(NTAPI* CreateDbg)(ULONG,BOOLEAN);
	typedef Status(NTAPI* QueryDbg)(HANDLE,ULONG,void*);
	typedef Status(NTAPI* DestroyDbg)(void*);
	typedef Status(NTAPI* CreateRefl)(HANDLE,ULONG,PVOID,PVOID,HANDLE,void*);
	typedef Status(NTAPI* PowerInfo)(ULONG,PVOID,ULONG,PVOID,ULONG);
	typedef Status(NTAPI* CreateThr)(PHANDLE,ACCESS_MASK,PVOID,HANDLE,PVOID,PVOID,ULONG,SIZE_T,SIZE_T,SIZE_T,PVOID);
	typedef Status(NTAPI* QueueApcEx)(HANDLE,HANDLE,PVOID,PVOID,PVOID,PVOID);
	typedef Status(NTAPI* QueueApcW64)(HANDLE,PVOID,PVOID,PVOID,PVOID);
	typedef Status(NTAPI* ResumeThr)(HANDLE,PULONG);
	typedef Status(NTAPI* GetVer)(OSVERSIONINFOW*);

	const ULONG ProcessBasicInformation_ = 0;
	const ULONG ProcessWow64Information_ = 26;
	const ULONG ProcessMitigationPolicy_ = 52;
	const ULONG MemoryMappedFilenameInformation_ = 2;
	const ULONG MemoryWorkingSetExInformation_ = 4;
	const ULONG MemoryImageInformation_ = 6;
	const ULONG QueryHeapSummary_ = 0x4;
	const ULONG QueryHeapEntries_ = 0x10;
	const ULONG QueryLocks_ = 0x20;
	const ULONG QueryNonInvasive_ = 0x80000000;
	const ULONG ReflectInheritHandles_ = 0x2;
	const ULONG ThreadCreateSuspended_ = 0x1;
	const ULONG PowerRequestAction_ = 44;
	const ULONG PlmPowerRequestCreate_ = 72;
	const int PowerRequestExecutionRequired_ = 3;
	const ULONG HeapSegmentSig_ = 0xDDEEDDEE;
	const ULONG HeapNtSig_ = 0xFFEEFFEE;
	const USHORT HeapEntryBusy_ = 0x1;
	const USHORT HeapEntrySegment_ = 0x2;
	const ULONG HeapClassMask_ = 0xf000;
	const ULONG HeapCreateSegmentHeap_ = 0x100;
	const ULONG HeapCreateHardened_ = 0x200;
	const size_t MaxCfgEntries = 800000;
	const size_t MaxModPages = 200000;
	const uint64_t ScanMs = 60000;

	// phnt ntrtl.h
	struct RtlDbg {
		HANDLE SectionHandleClient;
		PVOID ViewBaseClient,ViewBaseTarget;
		ULONG_PTR ViewBaseDelta;
		HANDLE EventPairClient,EventPairTarget,TargetProcessId,TargetThreadHandle;
		ULONG Flags;
		SIZE_T OffsetFree,CommitSize,ViewSize;
		PVOID Modules,BackTraces,Heaps,Locks,SpecificHeap;
		HANDLE TargetProcessHandle;
		PVOID VerifierOptions,ProcessHeap;
		HANDLE CriticalSectionHandle,CriticalSectionOwnerThread;
		PVOID Reserved[4];
	};

	struct RtlHeapEntry {
		SIZE_T Size;
		USHORT Flags,AllocatorBackTraceIndex;
		union {
			struct { SIZE_T Settable; ULONG Tag; } s1;
			struct { SIZE_T CommittedSize; PVOID FirstBlock; } s2;
		} u;
	};

	// RTL_HEAP_INFORMATION_V1, V2 adds ULONG64 HeapTag at the end on Windows 11 past 22000
	struct RtlHeap {
		PVOID BaseAddress;
		ULONG Flags;
		USHORT EntryOverhead,CreatorBackTraceIndex;
		SIZE_T BytesAllocated,BytesCommitted;
		ULONG NumberOfTags,NumberOfEntries,NumberOfPseudoTags,PseudoTagGranularity;
		ULONG Reserved[5];
		PVOID Tags;
		RtlHeapEntry* Entries;
	};
	struct RtlHeapV2 {
		RtlHeap h;
		ULONG64 HeapTag;
	};

	// phnt ntexapi.h
	struct RtlLock {
		PVOID Address;
		USHORT Type,CreatorBackTraceIndex;
		HANDLE OwningThread;
		LONG LockCount;
		ULONG ContentionCount,EntryCount;
		LONG RecursionCount;
		ULONG NumberOfWaitingShared,NumberOfWaitingExclusive;
	};

	// phnt ntmmapi.h
	struct WsEx {
		PVOID VirtualAddress;
		ULONG_PTR Attr;
	};
	struct ImgInfo {
		PVOID ImageBase;
		SIZE_T SizeOfImage;
		ULONG ImageFlags;
	};
	const ULONG_PTR WsValid_ = 1;
	const ULONG_PTR WsSharedOriginal_ = (ULONG_PTR)1 << 30;
	const ULONG ImgPartialMap_ = 1;
	const ULONG ImgNotExecutable_ = 2;

	// phnt ntpoapi.h
	struct ReasonCtx {
		ULONG Version,Flags;
		nt::USTR ResourceFileName;
		USHORT ResourceReasonId;
		ULONG StringCount;
		PVOID ReasonStrings;
	};
	struct PowerAction {
		HANDLE PowerRequestHandle;
		int RequestType;
		BOOLEAN SetAction;
		HANDLE ProcessHandle;
	};

	// phnt ntrtl.h RTLP_PROCESS_REFLECTION_REFLECTION_INFORMATION
	struct ReflInfo {
		HANDLE ReflectionProcessHandle,ReflectionThreadHandle;
		nt::CID ReflectionClientId;
	};

	// phnt ntgdi.h
	struct GdiEntry {
		PVOID Object;
		ULONG Owner; // low 16 bits : pid, cut to 16 bits by the kernel
		USHORT Unique;
		UCHAR Type,Flags;
		PVOID UserPointer;
	};

#ifdef _WIN64
	static_assert(offsetof(RtlDbg,Flags) == 0x40,"RtlDbg");
	static_assert(offsetof(RtlDbg,ViewSize) == 0x58,"RtlDbg");
	static_assert(offsetof(RtlDbg,Heaps) == 0x70,"RtlDbg");
	static_assert(offsetof(RtlDbg,Locks) == 0x78,"RtlDbg");
	static_assert(offsetof(RtlDbg,ProcessHeap) == 0x98,"RtlDbg");
	static_assert(sizeof(RtlDbg) == 0xD0,"RtlDbg");
	static_assert(sizeof(RtlHeapEntry) == 0x20,"RtlHeapEntry");
	static_assert(offsetof(RtlHeap,NumberOfEntries) == 0x24,"RtlHeap");
	static_assert(offsetof(RtlHeap,Entries) == 0x50,"RtlHeap");
	static_assert(sizeof(RtlHeap) == 0x58,"RtlHeap");
	static_assert(sizeof(RtlHeapV2) == 0x60,"RtlHeapV2");
	static_assert(sizeof(RtlLock) == 0x30,"RtlLock");
	static_assert(sizeof(WsEx) == 0x10,"WsEx");
	static_assert(sizeof(ImgInfo) == 0x18,"ImgInfo");
	static_assert(sizeof(ReasonCtx) == 0x28,"ReasonCtx");
	static_assert(sizeof(PowerAction) == 0x18,"PowerAction");
	static_assert(sizeof(ReflInfo) == 0x20,"ReflInfo");
	static_assert(sizeof(GdiEntry) == 0x18,"GdiEntry");
	static_assert(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64,GuardCFFunctionTable) == 0x80,"lc64");
	static_assert(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64,GuardCFFunctionCount) == 0x88,"lc64");
	static_assert(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64,GuardFlags) == 0x90,"lc64");
#endif

	QVM qvm() {
		static auto f = nt::fn<QVM>("NtQueryVirtualMemory");
		return f;
	}

	std::string hex32(uint32_t v) {
		char b[16];
		snprintf(b,sizeof(b),"0x%08x",v);
		return b;
	}

	std::string stTxt(Status st) {
		if (st == nt::ACCESS_DENIED) return "access denied : a protected process, or it needs admin";
		if (st == nt::PROCESS_IS_TERMINATING) return "the process is exiting";
		if (st == nt::NOT_IMPLEMENTED) return "this Windows does not have the call";
		return "status " + hex32((uint32_t)st);
	}

	std::string lastErr(const char* what) {
		DWORD e = GetLastError();
		if (e == ERROR_ACCESS_DENIED) return std::string(what) + " : access denied, a protected process or it needs admin";
		return std::string(what) + " : error " + std::to_string(e);
	}

	std::string utf8(const wchar_t* s,size_t n) {
		if (!n) return "";
		int len = WideCharToMultiByte(CP_UTF8,0,s,(int)n,nullptr,0,nullptr,nullptr);
		std::string r((size_t)std::max(len,0),'\0');
		if (len > 0) WideCharToMultiByte(CP_UTF8,0,s,(int)n,&r[0],len,nullptr,nullptr);
		return r;
	}

	struct Hnd {
		HANDLE h = nullptr;
		Hnd() {}
		explicit Hnd(HANDLE v) : h(v) {}
		Hnd(const Hnd&) = delete;
		Hnd& operator=(const Hnd&) = delete;
		~Hnd() { if (h) CloseHandle(h); }
	};

	bool exited(HANDLE h) {
		DWORD c = 0;
		return GetExitCodeProcess(h,&c) && c != STILL_ACTIVE;
	}

	// a walk always gets at least the first free block, so nothing at all is a failure
	std::string noRegions(HANDLE h) {
		std::string e = lastErr("region query");
		return exited(h) ? "the process is gone" : e;
	}

	// open and make sure the pid still is that process. Somebody holding a handle keeps an exited one openable
	HANDLE openRef(const ProcRef& r,DWORD acc,std::string& why) {
		HANDLE h = OpenProcess(acc,FALSE,r.pid);
		if (!h) {
			DWORD e = GetLastError();
			if (e == ERROR_INVALID_PARAMETER) why = "the process is gone";
			else if (e == ERROR_ACCESS_DENIED) why = "access denied : a protected process, or it needs admin";
			else why = "open failed, error " + std::to_string(e);
			return nullptr;
		}
		FILETIME c,x,k,u;
		if (r.start && GetProcessTimes(h,&c,&x,&k,&u)) {
			uint64_t st = ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
			if (st != r.start) {
				CloseHandle(h);
				why = "the process is gone, its pid was reused";
				return nullptr;
			}
		}
		if (exited(h)) {
			CloseHandle(h);
			why = "the process is gone";
			return nullptr;
		}
		return h;
	}

	HANDLE openRead(const ProcRef& r,std::string& why) {
		HANDLE h = openRef(r,PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,why);
		if (h) return h;
		std::string w2;
		h = openRef(r,PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,w2);
		if (h) why.clear();
		return h;
	}

	bool isWow(HANDLE h) {
		BOOL b = FALSE;
		return IsWow64Process(h,&b) && b;
	}

	int buildNo() {
		static int b = [] {
			OSVERSIONINFOW v = {};
			v.dwOSVersionInfoSize = sizeof(v);
			auto f = nt::fn<GetVer>("RtlGetVersion");
			return f && nt::ok(f(&v)) ? (int)v.dwBuildNumber : 0;
		}();
		return b;
	}

	std::wstring mappedName(HANDLE h,void* addr) {
		std::vector<char> b(sizeof(nt::USTR) + 1024);
		for (int i = 0; i < 4; i++) {
			SIZE_T got = 0;
			Status st = qvm()(h,addr,MemoryMappedFilenameInformation_,b.data(),b.size(),&got);
			if (nt::ok(st)) {
				auto* u = (nt::USTR*)b.data();
				if (u->Buffer && u->Length) return std::wstring(u->Buffer,u->Length / sizeof(wchar_t));
				return L"";
			}
			if (st != nt::BUFFER_OVERFLOW && st != nt::INFO_LENGTH_MISMATCH && st != nt::BUFFER_TOO_SMALL) return L"";
			b.resize(std::max(b.size() * 2,(size_t)got));
		}
		return L"";
	}

	// \Device\HarddiskVolume3\x -> C:\x
	struct DevMap {
		std::vector<std::pair<std::wstring,std::wstring>> m;
		DevMap() {
			wchar_t drv[3] = L"A:";
			DWORD mask = GetLogicalDrives();
			for (int i = 0; i < 26; i++) if (mask & (1u << i)) {
				drv[0] = (wchar_t)(L'A' + i);
				wchar_t t[512];
				if (QueryDosDeviceW(drv,t,512)) m.push_back({std::wstring(t) + L"\\",std::wstring(drv) + L"\\"});
			}
			m.push_back({L"\\Device\\Mup\\",L"\\\\"});
		}
		std::string dos(const std::wstring& nt) const {
			for (auto& kv : m) if (nt.compare(0,kv.first.size(),kv.first) == 0) {
				std::wstring s = kv.second + nt.substr(kv.first.size());
				return util::safe(utf8(s.data(),s.size()));
			}
			return util::safe(utf8(nt.data(),nt.size()));
		}
	};

	std::string baseOf(const std::string& p) {
		size_t c = p.find_last_of("\\/");
		return c == std::string::npos ? p : p.substr(c + 1);
	}

	// a PE on disk, read piece by piece. Files are not ours : every offset gets checked.
	struct Pe {
		Hnd f;
		uint64_t size = 0,imgBase = 0;
		bool is64 = false;
		std::vector<IMAGE_SECTION_HEADER> sec;
		IMAGE_DATA_DIRECTORY dir[16] = {};

		bool at(uint64_t off,void* out,size_t n) {
			if (off > size || n > size - off) return false;
			LARGE_INTEGER li;
			li.QuadPart = (LONGLONG)off;
			if (!SetFilePointerEx(f.h,li,nullptr,FILE_BEGIN)) return false;
			char* p = (char*)out;
			while (n) {
				DWORD got = 0;
				DWORD want = (DWORD)std::min(n,(size_t)(1 << 24));
				if (!ReadFile(f.h,p,want,&got,nullptr) || !got) return false;
				p += got;
				n -= got;
			}
			return true;
		}

		// file offset of n bytes at rva r, ~0 when they are not all in one section and in the file. Check before sizing a buffer from the headers
		uint64_t fileOff(uint64_t r,size_t n) {
			for (auto& s : sec) {
				uint64_t va = s.VirtualAddress;
				if (r >= va && r - va < s.SizeOfRawData && n <= s.SizeOfRawData - (r - va)) {
					uint64_t o = (uint64_t)s.PointerToRawData + (r - va);
					return o <= size && n <= size - o ? o : ~0ull;
				}
			}
			return ~0ull;
		}

		bool rva(uint64_t r,void* out,size_t n) {
			uint64_t o = fileOff(r,n);
			return o != ~0ull && at(o,out,n);
		}

		bool open(const std::wstring& ntPath) {
			std::wstring p = L"\\\\?\\GLOBALROOT" + ntPath;
			HANDLE h = CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
			if (h == INVALID_HANDLE_VALUE) return false;
			f.h = h;
			LARGE_INTEGER li;
			if (!GetFileSizeEx(h,&li)) return false;
			size = (uint64_t)li.QuadPart;
			IMAGE_DOS_HEADER d;
			if (!at(0,&d,sizeof(d)) || d.e_magic != IMAGE_DOS_SIGNATURE || d.e_lfanew < 0) return false;
			uint64_t nto = (uint64_t)d.e_lfanew;
			DWORD sig = 0;
			IMAGE_FILE_HEADER fh;
			if (!at(nto,&sig,4) || sig != IMAGE_NT_SIGNATURE || !at(nto + 4,&fh,sizeof(fh))) return false;
			WORD magic = 0;
			uint64_t oh = nto + 4 + sizeof(fh);
			if (!at(oh,&magic,2)) return false;
			if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
				IMAGE_OPTIONAL_HEADER64 o;
				if (fh.SizeOfOptionalHeader < sizeof(o) || !at(oh,&o,sizeof(o))) return false;
				is64 = true;
				imgBase = o.ImageBase;
				memcpy(dir,o.DataDirectory,sizeof(dir));
			} else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
				IMAGE_OPTIONAL_HEADER32 o;
				if (fh.SizeOfOptionalHeader < sizeof(o) || !at(oh,&o,sizeof(o))) return false;
				imgBase = o.ImageBase;
				memcpy(dir,o.DataDirectory,sizeof(dir));
			} else return false;
			if (fh.NumberOfSections > 256) return false;
			sec.resize(fh.NumberOfSections);
			return sec.empty() || at(oh + fh.SizeOfOptionalHeader,sec.data(),sec.size() * sizeof(IMAGE_SECTION_HEADER));
		}

		void exports(SymMod& m) {
			auto& d = dir[IMAGE_DIRECTORY_ENTRY_EXPORT];
			if (!d.VirtualAddress || d.Size < sizeof(IMAGE_EXPORT_DIRECTORY) || d.Size > (1u << 26) || fileOff(d.VirtualAddress,d.Size) == ~0ull) return;
			std::vector<char> b(d.Size);
			if (!rva(d.VirtualAddress,b.data(),b.size())) return;
			auto* e = (IMAGE_EXPORT_DIRECTORY*)b.data();
			uint64_t lo = d.VirtualAddress;
			auto in = [&](uint64_t r,uint64_t n) { return r >= lo && r - lo <= b.size() && n <= b.size() - (r - lo); };
			uint64_t nm = e->NumberOfNames,nf = e->NumberOfFunctions;
			if (!in(e->AddressOfNames,nm * 4) || !in(e->AddressOfNameOrdinals,nm * 2) || !in(e->AddressOfFunctions,nf * 4)) return;
			auto* names = (DWORD*)(b.data() + (e->AddressOfNames - lo));
			auto* ords = (WORD*)(b.data() + (e->AddressOfNameOrdinals - lo));
			auto* funcs = (DWORD*)(b.data() + (e->AddressOfFunctions - lo));
			m.exps.reserve((size_t)nm);
			for (uint64_t i = 0; i < nm; i++) {
				if (ords[i] >= nf || !in(names[i],1)) continue;
				uint32_t fr = funcs[ords[i]];
				if (!fr || in(fr,1)) continue; // forwarded
				const char* s = b.data() + (names[i] - lo);
				size_t mx = b.size() - (names[i] - lo);
				size_t len = strnlen(s,mx);
				if (len == mx) continue;
				m.exps.emplace_back(fr,util::safe(std::string(s,len)));
			}
			std::sort(m.exps.begin(),m.exps.end(),[](const std::pair<uint32_t,std::string>& x,const std::pair<uint32_t,std::string>& y) { return x.first < y.first; });
		}

		void guardTargets(uint64_t base,std::vector<uint64_t>& out) {
			auto& d = dir[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
			if (!d.VirtualAddress || d.Size < 4) return;
			unsigned char lc[0xA0] = {};
			uint32_t lcSz = 0;
			if (!rva(d.VirtualAddress,&lcSz,4)) return;
			size_t want = std::min((size_t)lcSz,sizeof(lc));
			if (!rva(d.VirtualAddress,lc,want)) return;
			uint64_t tbl = 0,cnt = 0;
			uint32_t fl = 0;
			if (is64) {
				if (want < 0x94) return;
				memcpy(&tbl,lc + 0x80,8);
				memcpy(&cnt,lc + 0x88,8);
				memcpy(&fl,lc + 0x90,4);
			} else {
				if (want < 0x5C) return;
				memcpy(&tbl,lc + 0x50,4);
				memcpy(&cnt,lc + 0x54,4);
				memcpy(&fl,lc + 0x58,4);
			}
			if (!tbl || !cnt || cnt > (1u << 24) || tbl < imgBase || tbl - imgBase >= ((uint64_t)1 << 32)) return;
			size_t stride = 4 + ((fl >> 28) & 0xF);
			if (fileOff(tbl - imgBase,(size_t)cnt * stride) == ~0ull) return;
			std::vector<unsigned char> b((size_t)cnt * stride);
			if (!rva(tbl - imgBase,b.data(),b.size())) return;
			for (size_t i = 0; i < (size_t)cnt; i++) {
				uint32_t r;
				memcpy(&r,b.data() + i * stride,4);
				out.push_back((base + r) & ~(uint64_t)15);
			}
		}
	};

	std::string protTxt(uint32_t p) {
		if (!p) return "";
		std::string s;
		if (p & PAGE_NOACCESS) s = "NA";
		else if (p & PAGE_READONLY) s = "R";
		else if (p & PAGE_READWRITE) s = "RW";
		else if (p & PAGE_WRITECOPY) s = "WC";
		else if (p & PAGE_EXECUTE) s = "X";
		else if (p & PAGE_EXECUTE_READ) s = "RX";
		else if (p & PAGE_EXECUTE_READWRITE) s = "RWX";
		else if (p & PAGE_EXECUTE_WRITECOPY) s = "WCX";
		else s = "?";
		if (p & PAGE_GUARD) s += "+G";
		if (p & PAGE_NOCACHE) s += "+NC";
		if (p & PAGE_WRITECOMBINE) s += "+WCM";
		return s;
	}

	// execution required request : without it a frozen packaged app hangs the query for 30 s
	struct KeepAwake {
		HANDLE h = nullptr;
		explicit KeepAwake(HANDLE proc) {
			static auto f = nt::fn<PowerInfo>("NtPowerInformation");
			if (!f) return;
			ReasonCtx c = {};
			c.Flags = 0x80000000; // DIAGNOSTIC_REASON_NOT_SPECIFIED
			HANDLE rq = nullptr;
			if (!nt::ok(f(PlmPowerRequestCreate_,&c,sizeof(c),&rq,sizeof(rq)))) return;
			PowerAction a = {};
			a.PowerRequestHandle = rq;
			a.RequestType = PowerRequestExecutionRequired_;
			a.SetAction = TRUE;
			a.ProcessHandle = proc;
			if (nt::ok(f(PowerRequestAction_,&a,sizeof(a),nullptr,0))) h = rq;
			else CloseHandle(rq);
		}
		~KeepAwake() {
			if (!h) return;
			static auto f = nt::fn<PowerInfo>("NtPowerInformation");
			PowerAction a = {};
			a.PowerRequestHandle = h;
			a.RequestType = PowerRequestExecutionRequired_;
			f(PowerRequestAction_,&a,sizeof(a),nullptr,0);
			CloseHandle(h);
		}
	};

	struct Refl {
		ReflInfo i = {};
		~Refl() {
			if (i.ReflectionThreadHandle) CloseHandle(i.ReflectionThreadHandle);
			if (i.ReflectionProcessHandle) {
				TerminateProcess(i.ReflectionProcessHandle,0);
				CloseHandle(i.ReflectionProcessHandle);
			}
		}
	};

	// RtlQueryProcessDebugInformation. Runs a thread in the target and can block for a long time.
	struct DbgBuf {
		void* p = nullptr;
		Status st = nt::UNSUCCESSFUL;
		explicit DbgBuf(HANDLE pid,ULONG flags) {
			static auto create = nt::fn<CreateDbg>("RtlCreateQueryDebugBuffer");
			static auto query = nt::fn<QueryDbg>("RtlQueryProcessDebugInformation");
			static auto destroy = nt::fn<DestroyDbg>("RtlDestroyQueryDebugBuffer");
			if (!create || !query || !destroy) {
				st = nt::NOT_IMPLEMENTED;
				return;
			}
			for (ULONG i = 0x400000; i && i <= 0x40000000; i *= 2) {
				p = create(i,FALSE);
				if (!p) {
					st = nt::NO_MEMORY;
					return;
				}
				st = query(pid,flags,p);
				if (nt::ok(st)) return;
				destroy(p);
				p = nullptr;
				if (st != nt::NO_MEMORY) return;
			}
		}
		~DbgBuf() {
			static auto destroy = nt::fn<DestroyDbg>("RtlDestroyQueryDebugBuffer");
			if (p) destroy(p);
		}
		RtlDbg* d() const { return (RtlDbg*)p; }
		// the layouts are undocumented, a pointer outside the view means we read them wrong
		bool inView(const void* a,uint64_t n) const {
			uint64_t lo = (uint64_t)d()->ViewBaseClient;
			uint64_t x = (uint64_t)a;
			return x >= lo && x - lo <= d()->ViewSize && n <= d()->ViewSize - (x - lo);
		}
	};

	std::string heapFlags(ULONG f) {
		static const struct { ULONG bit; const char* name; } tbl[] = {
			{HEAP_NO_SERIALIZE,"No serialize"},
			{HEAP_GROWABLE,"Growable"},
			{HEAP_GENERATE_EXCEPTIONS,"Generate exceptions"},
			{HEAP_ZERO_MEMORY,"Zero memory"},
			{HEAP_REALLOC_IN_PLACE_ONLY,"Realloc in-place"},
			{HEAP_TAIL_CHECKING_ENABLED,"Tail checking"},
			{HEAP_FREE_CHECKING_ENABLED,"Free checking"},
			{HEAP_DISABLE_COALESCE_ON_FREE,"No coalesce on free"},
			{HEAP_CREATE_ALIGN_16,"Align 16"},
			{HEAP_CREATE_ENABLE_TRACING,"Traceable"},
			{HEAP_CREATE_ENABLE_EXECUTE,"Executable"},
			{HeapCreateSegmentHeap_,"Segment heap"},
			{HeapCreateHardened_,"Segment hardened"}
		};
		std::string s;
		for (auto& t : tbl) if (f & t.bit) {
			if (!s.empty()) s += ", ";
			s += t.name;
		}
		if (f) s += s.empty() ? hex32(f) : " (" + hex32(f) + ")";
		return s;
	}

	const char* heapClass(ULONG c) {
		switch (c >> 12) {
		case 0: return "Process Heap";
		case 1: return "Private Heap";
		case 2: return "Kernel Heap";
		case 3: return "GDI Heap";
		case 4: return "User Heap";
		case 5: return "Console Heap";
		case 6: return "Desktop Heap";
		case 7: return "CSRSS Shared Heap";
		case 8: return "CSRSS Port Heap";
		}
		return "Unknown Heap";
	}

	// _HEAP.SegmentSignature and FrontEndHeapType, offsets from dt on Windows 10 and later
	std::string heapType(HANDLE h,uint64_t base) {
		ULONG sig = 0;
		UCHAR fe = 0xFF;
		SIZE_T got = 0;
		if (!ReadProcessMemory(h,(void*)(base + 0x10),&sig,sizeof(sig),&got) || got != sizeof(sig)) return "";
		if (!ReadProcessMemory(h,(void*)(base + 0x1a2),&fe,1,&got)) fe = 0xFF;
		std::string s = sig == HeapNtSig_ ? "NT Heap" : sig == HeapSegmentSig_ ? "Segment Heap" : "";
		if (s.empty()) return s;
		if (fe == 1) s += " (Lookaside)";
		else if (fe == 2) s += " (LFH)";
		return s;
	}

	template<class H> bool readHeaps(const DbgBuf& q,HANDLE rd,HeapRes& res) {
		struct List { ULONG n; ULONG pad; };
		auto* l = (List*)q.d()->Heaps;
		if (!q.inView(l,sizeof(List))) return false;
		ULONG n = l->n;
		auto* hs = (H*)((char*)l + sizeof(List));
		if (!q.inView(hs,(uint64_t)n * sizeof(H))) return false;
		for (ULONG i = 0; i < n; i++) {
			const RtlHeap& hp = *(const RtlHeap*)&hs[i];
			if (hp.NumberOfEntries && !q.inView(hp.Entries,(uint64_t)hp.NumberOfEntries * sizeof(RtlHeapEntry))) return false;
			uint64_t used = 0,comm = 0;
			for (ULONG e = 0; e < hp.NumberOfEntries; e++) {
				const RtlHeapEntry& he = hp.Entries[e];
				if (he.Flags & HeapEntryBusy_) used += he.Size;
				if (he.Flags & HeapEntrySegment_) comm += he.u.s2.CommittedSize;
			}
			// the walk can come up a few pages short of what the heap reports, and used can't pass committed
			comm = std::max(comm,(uint64_t)hp.BytesCommitted);
			used = std::min(used,comm);
			Heap r;
			r.base = (uint64_t)hp.BaseAddress;
			r.used = used;
			r.committed = comm;
			r.entries = hp.NumberOfEntries;
			r.flags = hp.Flags;
			r.flagTxt = heapFlags(hp.Flags & ~HeapClassMask_);
			r.type = heapType(rd,r.base);
			r.def = hp.BaseAddress && hp.BaseAddress == q.d()->ProcessHeap;
			// a segment heap keeps no class in its flags, past the default one it is unknown
			if (r.def || r.type.rfind("Segment Heap",0) != 0) r.cls = heapClass(hp.Flags & HeapClassMask_);
			res.rows.push_back(std::move(r));
		}
		return true;
	}

	// image bases in the target. Partial maps and data file maps don't count, as in SI
	struct Img {
		uint64_t base = 0,size = 0;
		std::wstring nt;
	};

	template<class F> bool walkRegions(HANDLE h,const std::atomic<bool>& stop,uint64_t end,F fn) {
		uint64_t a = 0;
		MEMORY_BASIC_INFORMATION mi;
		int n = 0;
		while (VirtualQueryEx(h,(void*)a,&mi,sizeof(mi)) == sizeof(mi)) {
			if ((++n & 1023) == 0 && (stop || util::nowMs() > end)) return false;
			fn(mi);
			uint64_t nx = (uint64_t)mi.BaseAddress + mi.RegionSize;
			if (nx <= a) break;
			a = nx;
		}
		return true;
	}

	void addSyms(SymTab& t,const Img& im,const DevMap& dm,std::vector<uint64_t>* guard) {
		SymMod m;
		m.base = im.base;
		m.size = im.size;
		m.name = baseOf(dm.dos(im.nt));
		Pe pe;
		if (pe.open(im.nt)) {
			pe.exports(m);
			if (guard) pe.guardTargets(im.base,*guard);
		}
		t.push_back(std::move(m));
	}

	void sortSyms(SymTab& t) {
		std::sort(t.begin(),t.end(),[](const SymMod& x,const SymMod& y) { return x.base < y.base; });
	}

	// LdrSystemDllInitBlock : same address in every process. Size tells the layout (phnt ntldr.h, V1 V2 V3).
	struct Bitmaps {
		uint64_t native = 0,nativeSz = 0,wow = 0,wowSz = 0;
		std::string why;
	};
	Bitmaps readBitmaps(HANDLE h) {
		Bitmaps b;
		char* ib = (char*)nt::proc("LdrSystemDllInitBlock");
		if (!ib) {
			b.why = "this Windows has no init block export";
			return b;
		}
		unsigned char blk[0x130] = {};
		SIZE_T got = 0;
		if (!ReadProcessMemory(h,ib,blk,sizeof(blk),&got) && got < 0x80) {
			b.why = lastErr("read init block");
			return b;
		}
		uint32_t sz;
		memcpy(&sz,blk,4);
		size_t off = sz >= 0xF0 ? 0xB8 : sz >= 0xD0 ? 0xB0 : sz >= 0x80 ? 0x60 : 0;
		if (!off) {
			b.why = "this Windows has no CFG bitmap fields";
			return b;
		}
		memcpy(&b.native,blk + off,8);
		memcpy(&b.nativeSz,blk + off + 8,8);
		memcpy(&b.wow,blk + off + 16,8);
		memcpy(&b.wowSz,blk + off + 24,8);
		return b;
	}

	int cfgPolicy(HANDLE h) {
		static auto f = nt::fn<QIP>("NtQueryInformationProcess");
		struct { int Policy; ULONG Flags; } p = {7,0}; // ProcessControlFlowGuardPolicy
		if (!f || !nt::ok(f(h,ProcessMitigationPolicy_,&p,sizeof(p),nullptr))) return -1;
		return (p.Flags & 1) ? 1 : 0;
	}

	// bitmap layout : one byte per 64 bytes of address space, a 64 bit word covers 512 bytes, 2 bits per 16 byte slot.
	// Value 1 valid, 2 export suppressed, 3 valid also unaligned. Checked against the flags in the guard tables of
	// every image in a test process : plain entries are 1, export suppressed ones 2, SI has 1 and 2 the wrong way round.
	struct CfgScan {
		HANDLE h;
		CfgRes& r;
		const std::vector<uint64_t>& image;
		const DevMap& dm;
		const std::atomic<bool>& stop;
		uint64_t end;
		std::vector<char> fileDone;

		int regOf(uint64_t a) {
			auto& v = r.regs;
			auto it = std::upper_bound(v.begin(),v.end(),a,[](uint64_t x,const CfgReg& g) { return x < g.base; });
			if (it == v.begin()) return -1;
			--it;
			if (a - it->base >= it->size) return -1;
			int i = (int)(it - v.begin());
			if (!fileDone[i]) {
				fileDone[i] = 1;
				if (it->type == MEM_IMAGE || it->type == MEM_MAPPED) {
					std::wstring n = mappedName(h,(void*)it->base);
					if (!n.empty()) it->file = dm.dos(n);
				}
			}
			return i;
		}

		// bitmap pages are shared between processes, so they also carry the targets of images not loaded here
		bool mapped(int i) {
			return i >= 0 && r.regs[i].state == MEM_COMMIT;
		}

		bool add(const CfgEnt& e) {
			if (r.rows.size() >= MaxCfgEntries) {
				r.truncated = true;
				r.complete = false;
				r.reason = "row limit";
				return false;
			}
			r.rows.push_back(e);
			r.counts[e.type]++;
			return true;
		}

		void region(const CfgReg& g,uint64_t bmp,bool wow) {
			// most of a bitmap is committed no access : nothing valid lives in that part of the address space
			if (g.base < bmp || (g.prot & PAGE_NOACCESS)) return;
			static const size_t CHUNK = 0x10000;
			std::vector<uint64_t> w(CHUNK / 8);
			uint64_t off = g.base - bmp;
			for (uint64_t done = 0; done < g.size;) {
				if (stop || util::nowMs() > end) {
					r.complete = false;
					r.reason = stop ? "cancelled" : "time limit";
					return;
				}
				size_t sz = (size_t)std::min(g.size - done,(uint64_t)CHUNK);
				SIZE_T got = 0;
				if (!ReadProcessMemory(h,(void*)(g.base + done),w.data(),sz,&got) || got != sz) {
					r.unread += sz;
					done += sz;
					continue;
				}
				uint64_t by = off + done;
				size_t n = sz / 8;
				for (size_t i = 0; i < n;) {
					uint64_t v = w[i];
					if (v == ~(uint64_t)0) {
						size_t j = i;
						while (j < n && w[j] == ~(uint64_t)0) j++;
						uint64_t a = (by + i * 8) * 64;
						uint64_t e = (by + j * 8) * 64;
						if (!r.rows.empty() && r.rows.back().end && r.rows.back().end == a && r.rows.back().wow == wow) r.rows.back().end = e;
						else {
							CfgEnt x;
							x.addr = a;
							x.end = e;
							x.wow = wow;
							x.reg = regOf(a);
							x.type = mapped(x.reg) ? CfgAllValid : CfgUnmapped;
							if (!add(x)) return;
						}
						i = j;
						continue;
					}
					if (v) for (int s = 0; s < 32; s++) {
						int val = (int)((v >> (s * 2)) & 3);
						if (!val) continue;
						CfgEnt x;
						x.addr = (by + i * 8) * 64 + (uint64_t)s * 16;
						x.val = (uint8_t)val;
						x.wow = wow;
						x.reg = regOf(x.addr);
						if (!mapped(x.reg)) x.type = CfgUnmapped;
						else if (val == 2) x.type = CfgSuppressed;
						else if (std::binary_search(image.begin(),image.end(),x.addr)) x.type = CfgImage;
						else x.type = CfgModified;
						if (!add(x)) return;
					}
					i++;
				}
				done += sz;
				r.scanned += sz;
			}
		}

		void bitmap(uint64_t bmp,uint64_t claimed,bool wow) {
			uint64_t alloc = bmp;
			bool found = false;
			for (auto& g : r.regs) if (g.state == MEM_COMMIT && g.alloc == bmp) {
				found = true;
				break;
			}
			if (!found) for (auto& g : r.regs) if (g.state == MEM_COMMIT && g.base <= bmp && bmp - g.base < g.size) {
				alloc = g.alloc;
				break;
			}
			uint64_t comm = 0,rd = 0;
			int cnt = 0;
			for (auto& g : r.regs) if (g.state == MEM_COMMIT && g.alloc == alloc) {
				comm += g.size;
				if (!(g.prot & PAGE_NOACCESS)) rd += g.size;
				cnt++;
			}
			r.summary.push_back(std::string(wow ? "32-bit" : "Native") + " bitmap @ " + util::hex(bmp) + " : claimed " + util::size(claimed) + ", committed " + util::size(comm) +
				" (" + std::to_string(cnt) + (cnt == 1 ? " region" : " regions") + ", " + util::size(rd) + " readable), uncommitted " + util::size(claimed > comm ? claimed - comm : 0) + ".");
			// regs can't move while this loops : regOf only fills file names
			for (size_t i = 0; i < r.regs.size(); i++) {
				if (r.regs[i].state != MEM_COMMIT || r.regs[i].alloc != alloc) continue;
				const CfgReg g = r.regs[i];
				region(g,bmp,wow);
				if (r.truncated || !r.complete) return;
			}
		}
	};

	const char* gdiType(USHORT unique) {
		switch (unique & 0x7F) {
		case 0x21: return "Alt. DC";
		case 0x05: return "Bitmap";
		case 0x10: return "Brush";
		case 0x06: return "Client Object";
		case 0x25: return "DIB Section";
		case 0x01: return "DC";
		case 0x50: return "ExtPen";
		case 0x0A: return "Font";
		case 0x66: return "Metafile DC";
		case 0x46: return "Enhanced Metafile";
		case 0x26: return "Metafile";
		case 0x08: return "Palette";
		case 0x30: return "Pen";
		case 0x04: return "Region";
		}
		return nullptr;
	}

	uint32_t swapRgb(uint32_t c) {
		return ((c & 0xFF) << 24) | ((c & 0xFF00) << 8) | ((c >> 8) & 0xFF00) | (c >> 24);
	}

	// GDI hands foreign handles back too. When it won't the column stays empty
	std::string gdiInfo(uint32_t hv) {
		HGDIOBJ h = (HGDIOBJ)(ULONG_PTR)hv;
		char b[400];
		b[0] = 0;
		switch ((hv >> 16) & 0x7F) {
		case 0x05:
		case 0x25: {
			BITMAP bm;
			if (GetObjectW(h,sizeof(bm),&bm)) snprintf(b,sizeof(b),"Width : %ld, Height : %ld, Depth : %u, Size : %s",bm.bmWidth,bm.bmHeight,bm.bmBitsPixel,util::size((uint64_t)bm.bmWidthBytes * (uint64_t)bm.bmHeight).c_str());
			break;
		}
		case 0x10: {
			LOGBRUSH lb;
			if (GetObjectW(h,sizeof(lb),&lb)) snprintf(b,sizeof(b),"Style : %u, Color : 0x%08x, Hatch : 0x%llx",lb.lbStyle,swapRgb(lb.lbColor),(unsigned long long)lb.lbHatch);
			break;
		}
		case 0x50: {
			// a styled pen needs more than the struct, GetObject then wants the bigger buffer
			char pb[sizeof(EXTLOGPEN) + 64 * sizeof(DWORD)];
			auto* p = (EXTLOGPEN*)pb;
			if (GetObjectW(h,sizeof(pb),p)) snprintf(b,sizeof(b),"Style : 0x%lx, Width : %lu, Color : 0x%08x",p->elpPenStyle,p->elpWidth,swapRgb(p->elpColor));
			break;
		}
		case 0x0A: {
			LOGFONTW lf;
			if (GetObjectW(h,sizeof(lf),&lf)) {
				size_t n = wcsnlen(lf.lfFaceName,LF_FACESIZE);
				snprintf(b,sizeof(b),"Face : %s, Height : %ld",util::safe(utf8(lf.lfFaceName,n)).c_str(),lf.lfHeight);
			}
			break;
		}
		case 0x08: {
			WORD n = 0;
			if (GetObjectW(h,sizeof(n),&n)) snprintf(b,sizeof(b),"Entries : %u, Size : %s",(unsigned)n,util::size((uint64_t)n * sizeof(PALETTEENTRY)).c_str());
			break;
		}
		case 0x30: {
			LOGPEN lp;
			if (GetObjectW(h,sizeof(lp),&lp)) snprintf(b,sizeof(b),"Style : %u, Width : %ld, Color : 0x%08x",lp.lopnStyle,lp.lopnWidth.x,swapRgb(lp.lopnColor));
			break;
		}
		case 0x04: {
			DWORD n = GetRegionData((HRGN)h,0,nullptr);
			if (n) snprintf(b,sizeof(b),"Size : %s",util::size(n).c_str());
			break;
		}
		case 0x01: {
			HDC dc = (HDC)h;
			int ty = GetObjectType(dc);
			if (!ty) break;
			HWND wnd = WindowFromDC(dc);
			POINT org = {};
			GetDCOrgEx(dc,&org);
			RECT cb = {};
			int cr = GetClipBox(dc,&cb);
			HGDIOBJ sel = GetCurrentObject(dc,OBJ_BITMAP);
			BITMAP bm;
			uint64_t bs = 0;
			if (sel && GetObjectW(sel,sizeof(bm),&bm)) bs = (uint64_t)bm.bmWidthBytes * (uint64_t)bm.bmHeight;
			snprintf(b,sizeof(b),"Type : %d, HWND : 0x%llx, Tech : %d, Res : %dx%d, BPP : %dx%d, Org : (%ld,%ld), Clip : %d [%ld,%ld,%ld,%ld], Bitmap : 0x%llx, Size : %s",
				ty,(unsigned long long)(ULONG_PTR)wnd,GetDeviceCaps(dc,TECHNOLOGY),GetDeviceCaps(dc,HORZRES),GetDeviceCaps(dc,VERTRES),GetDeviceCaps(dc,BITSPIXEL),GetDeviceCaps(dc,PLANES),
				org.x,org.y,cr,cb.left,cb.top,cb.right,cb.bottom,(unsigned long long)(ULONG_PTR)sel,util::size(bs).c_str());
			break;
		}
		}
		return b;
	}

	// RtlFlushHeaps and RtlExitUserThread of the 32 bit ntdll in a WOW64 target. PE read from its memory.
	bool wow64Procs(HANDLE h,void*& flushFn,void*& exitFn,std::string& why) {
		uint64_t base = 0;
		std::atomic<bool> never{false};
		walkRegions(h,never,util::nowMs() + ScanMs,[&](const MEMORY_BASIC_INFORMATION& mi) {
			if (base || mi.Type != MEM_IMAGE || mi.AllocationBase != mi.BaseAddress || (uint64_t)mi.BaseAddress >= 0x100000000ull) return;
			std::wstring n = mappedName(h,mi.BaseAddress);
			for (auto& c : n) c = (wchar_t)towlower(c);
			const std::wstring tail = L"\\syswow64\\ntdll.dll";
			if (n.size() > tail.size() && n.compare(n.size() - tail.size(),tail.size(),tail) == 0) base = (uint64_t)mi.BaseAddress;
		});
		if (!base) {
			why = "32-bit ntdll not found in the target";
			return false;
		}
		IMAGE_DOS_HEADER d;
		IMAGE_NT_HEADERS32 th;
		SIZE_T got = 0;
		if (!ReadProcessMemory(h,(void*)base,&d,sizeof(d),&got) || !ReadProcessMemory(h,(void*)(base + (uint64_t)d.e_lfanew),&th,sizeof(th),&got)) {
			why = lastErr("read 32-bit ntdll");
			return false;
		}
		auto& dd = th.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
		if (!dd.VirtualAddress || dd.Size < sizeof(IMAGE_EXPORT_DIRECTORY) || dd.Size > (1u << 24)) {
			why = "32-bit ntdll has no exports";
			return false;
		}
		std::vector<char> b(dd.Size);
		if (!ReadProcessMemory(h,(void*)(base + dd.VirtualAddress),b.data(),b.size(),&got) || got != b.size()) {
			why = lastErr("read 32-bit ntdll exports");
			return false;
		}
		auto* e = (IMAGE_EXPORT_DIRECTORY*)b.data();
		uint64_t lo = dd.VirtualAddress;
		auto in = [&](uint64_t r,uint64_t n) { return r >= lo && r - lo <= b.size() && n <= b.size() - (r - lo); };
		if (!in(e->AddressOfNames,(uint64_t)e->NumberOfNames * 4) || !in(e->AddressOfNameOrdinals,(uint64_t)e->NumberOfNames * 2) || !in(e->AddressOfFunctions,(uint64_t)e->NumberOfFunctions * 4)) {
			why = "32-bit ntdll exports look wrong";
			return false;
		}
		auto* names = (DWORD*)(b.data() + (e->AddressOfNames - lo));
		auto* ords = (WORD*)(b.data() + (e->AddressOfNameOrdinals - lo));
		auto* funcs = (DWORD*)(b.data() + (e->AddressOfFunctions - lo));
		for (DWORD i = 0; i < e->NumberOfNames; i++) {
			if (!in(names[i],1) || ords[i] >= e->NumberOfFunctions) continue;
			const char* s = b.data() + (names[i] - lo);
			size_t mx = b.size() - (names[i] - lo);
			if (strnlen(s,mx) == mx) continue;
			if (!strcmp(s,"RtlFlushHeaps")) flushFn = (void*)(base + funcs[ords[i]]);
			else if (!strcmp(s,"RtlExitUserThread")) exitFn = (void*)(base + funcs[ords[i]]);
		}
		if (!flushFn || !exitFn) {
			why = "RtlFlushHeaps not found in the 32-bit ntdll";
			return false;
		}
		return true;
	}

	// created suspended : no loader data and no process heap in the PEB yet, and a flush in there takes the process down.
	// false when it can't be told, the flush warns about that risk anyway
	bool notStarted(HANDLE h,bool wow) {
		static auto f = nt::fn<QIP>("NtQueryInformationProcess");
		if (!f) return false;
		ULONG_PTR peb = 0;
		if (wow) {
			if (!nt::ok(f(h,ProcessWow64Information_,&peb,sizeof(peb),nullptr))) return false;
		} else {
			struct { PVOID e; PVOID peb; ULONG_PTR a,b,c,d; } pbi = {};
			if (!nt::ok(f(h,ProcessBasicInformation_,&pbi,sizeof(pbi),nullptr))) return false;
			peb = (ULONG_PTR)pbi.peb;
		}
		if (!peb) return false;
		for (size_t off : {wow ? (size_t)0x0C : (size_t)0x18,wow ? (size_t)0x18 : (size_t)0x30}) { // Ldr, ProcessHeap
			uint64_t v = 0;
			if (!ReadProcessMemory(h,(void*)(peb + off),&v,wow ? 4 : 8,nullptr)) return false;
			if (!v) return true;
		}
		return false;
	}

	void memOf(HANDLE h,uint64_t& priv,uint64_t& ws) {
		PROCESS_MEMORY_COUNTERS_EX m = {};
		m.cb = sizeof(m);
		if (!GetProcessMemoryInfo(h,(PROCESS_MEMORY_COUNTERS*)&m,sizeof(m))) return;
		priv = m.PrivateUsage;
		ws = m.WorkingSetSize;
	}

}

namespace vmdiag {
namespace be {

	bool remote() {
		return true;
	}

	std::shared_ptr<const HeapRes> heaps(const ProcRef& r,bool clone) {
		auto res = std::make_shared<HeapRes>();
		res->unixSec = (uint64_t)time(nullptr);
		DWORD acc = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_LIMITED_INFORMATION | PROCESS_VM_READ;
		if (clone) acc |= PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_DUP_HANDLE;
		Hnd h(openRef(r,acc,res->why));
		if (!h.h) return res;
		if (isWow(h.h)) {
			res->na = true;
			res->why = "32-bit process : its heaps need a 32-bit helper and FRS is one 64-bit exe";
			return res;
		}
		KeepAwake awake(h.h);
		Refl refl;
		HANDLE pid = (HANDLE)(ULONG_PTR)r.pid;
		if (clone) {
			static auto mk = nt::fn<CreateRefl>("RtlCreateProcessReflection");
			if (!mk) {
				res->why = "this Windows has no process reflection";
				return res;
			}
			Status st = mk(h.h,ReflectInheritHandles_,nullptr,nullptr,nullptr,&refl.i);
			if (!nt::ok(st)) {
				res->why = "clone : " + stTxt(st);
				return res;
			}
			pid = refl.i.ReflectionClientId.UniqueProcess;
			res->clone = true;
		}
		DbgBuf q(pid,QueryHeapSummary_ | QueryHeapEntries_ | QueryNonInvasive_);
		if (!nt::ok(q.st)) {
			res->why = exited(h.h) ? "the process exited while the query ran" : stTxt(q.st);
			return res;
		}
		if (!q.d()->Heaps) {
			res->why = "the query returned no heaps, a frozen packaged app does this";
			return res;
		}
		bool good = buildNo() > 22000 ? readHeaps<RtlHeapV2>(q,h.h,*res) : readHeaps<RtlHeap>(q,h.h,*res);
		if (!good) {
			res->rows.clear();
			res->why = "the reply does not match the layout FRS knows, this Windows build changed it";
			return res;
		}
		res->ok = true;
		return res;
	}

	std::shared_ptr<const LockRes> locks(const ProcRef& r) {
		auto res = std::make_shared<LockRes>();
		res->unixSec = (uint64_t)time(nullptr);
		Hnd h(openRef(r,PROCESS_QUERY_INFORMATION | PROCESS_SET_LIMITED_INFORMATION,res->why));
		if (!h.h) return res;
		if (isWow(h.h)) {
			res->na = true;
			res->why = "32-bit process : its locks need a 32-bit helper and FRS is one 64-bit exe";
			return res;
		}
		KeepAwake awake(h.h);
		DbgBuf q((HANDLE)(ULONG_PTR)r.pid,QueryLocks_);
		if (!nt::ok(q.st)) {
			res->why = exited(h.h) ? "the process exited while the query ran" : stTxt(q.st);
			return res;
		}
		if (!q.d()->Locks) {
			res->why = "the query returned no locks, a frozen packaged app does this";
			return res;
		}
		struct List { ULONG n; ULONG pad; };
		auto* l = (List*)q.d()->Locks;
		auto* e = (RtlLock*)((char*)l + sizeof(List));
		if (!q.inView(l,sizeof(List)) || !q.inView(e,(uint64_t)l->n * sizeof(RtlLock))) {
			res->why = "the reply does not match the layout FRS knows, this Windows build changed it";
			return res;
		}
		DevMap dm;
		res->rows.reserve(l->n);
		for (ULONG i = 0; i < l->n; i++) {
			Lock o;
			o.addr = (uint64_t)e[i].Address;
			o.owner = (uint64_t)e[i].OwningThread;
			o.type = e[i].Type == 0 ? "Critical section" : e[i].Type == 1 ? "Resource" : "Unknown";
			if (e[i].Address) {
				std::wstring n = mappedName(h.h,e[i].Address);
				if (!n.empty()) o.file = dm.dos(n);
			}
			o.lockCount = e[i].LockCount;
			o.contention = e[i].ContentionCount;
			o.entry = e[i].EntryCount;
			o.recursion = e[i].RecursionCount;
			o.waitShared = e[i].NumberOfWaitingShared;
			o.waitExcl = e[i].NumberOfWaitingExclusive;
			res->rows.push_back(std::move(o));
		}
		res->ok = true;
		return res;
	}

	std::shared_ptr<const ModRes> modPages(const ProcRef& r,const std::atomic<bool>& stop) {
		auto res = std::make_shared<ModRes>();
		res->unixSec = (uint64_t)time(nullptr);
		Hnd h(openRead(r,res->why));
		if (!h.h) return res;
		uint64_t end = util::nowMs() + ScanMs;
		std::vector<Img> imgs;
		bool any = false;
		bool whole = walkRegions(h.h,stop,end,[&](const MEMORY_BASIC_INFORMATION& mi) {
			any = true;
			if (mi.Type != MEM_IMAGE || mi.AllocationBase != mi.BaseAddress) return;
			ImgInfo ii = {};
			if (!nt::ok(qvm()(h.h,mi.BaseAddress,MemoryImageInformation_,&ii,sizeof(ii),nullptr))) {
				res->fails++;
				return;
			}
			if (!ii.ImageBase || (ii.ImageFlags & (ImgPartialMap_ | ImgNotExecutable_))) {
				res->skipped++;
				return;
			}
			for (auto& x : imgs) if (x.base == (uint64_t)ii.ImageBase) return;
			Img im;
			im.base = (uint64_t)ii.ImageBase;
			im.size = ii.SizeOfImage;
			im.nt = mappedName(h.h,ii.ImageBase);
			imgs.push_back(std::move(im));
		});
		if (!any) {
			res->why = noRegions(h.h);
			return res;
		}
		if (!whole) {
			res->complete = false;
			res->reason = stop ? "cancelled" : "time limit";
		}
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		uint64_t ps = si.dwPageSize;
		DevMap dm;
		SymTab syms;
		std::vector<WsEx> wb;
		Status lastBad = nt::SUCCESS;
		for (auto& im : imgs) {
			if (stop || util::nowMs() > end) {
				res->complete = false;
				res->reason = stop ? "cancelled" : "time limit";
				break;
			}
			if (res->rows.size() >= MaxModPages) {
				res->complete = false;
				res->reason = "row limit";
				break;
			}
			uint64_t n = (im.size + ps - 1) / ps;
			if (!n) continue;
			wb.resize((size_t)n);
			for (uint64_t i = 0; i < n; i++) {
				wb[(size_t)i].VirtualAddress = (void*)(im.base + i * ps);
				wb[(size_t)i].Attr = 0;
			}
			Status st = qvm()(h.h,nullptr,MemoryWorkingSetExInformation_,wb.data(),(SIZE_T)n * sizeof(WsEx),nullptr);
			if (!nt::ok(st)) {
				res->fails++;
				lastBad = st;
				continue;
			}
			res->images++;
			std::string file;
			size_t before = res->rows.size(),i = 0;
			for (; i < (size_t)n; i++) {
				if (wb[i].Attr & WsSharedOriginal_) continue;
				if (res->rows.size() >= MaxModPages) {
					res->complete = false;
					res->reason = "row limit";
					break;
				}
				if (file.empty()) file = baseOf(dm.dos(im.nt));
				ModPage p;
				p.base = im.base;
				p.addr = (uint64_t)wb[i].VirtualAddress;
				p.file = file;
				p.resident = (wb[i].Attr & WsValid_) != 0;
				if (p.resident) res->residentN++;
				res->rows.push_back(std::move(p));
			}
			res->scanned += i * ps;
			if (res->rows.size() > before) {
				addSyms(syms,im,dm,nullptr);
				sortSyms(syms);
				for (size_t i = before; i < res->rows.size(); i++) res->rows[i].sym = symAt(syms,res->rows[i].addr);
			}
		}
		if (exited(h.h)) {
			res->why = "the process is gone";
			return res;
		}
		if (!res->images && res->fails && !imgs.empty()) {
			res->why = "no image could be queried, " + stTxt(lastBad);
			return res;
		}
		res->ok = true;
		return res;
	}

	std::shared_ptr<const CfgRes> cfg(const ProcRef& r,const std::atomic<bool>& stop) {
		auto res = std::make_shared<CfgRes>();
		res->unixSec = (uint64_t)time(nullptr);
		Hnd h(openRead(r,res->why));
		if (!h.h) return res;
		res->enabled = cfgPolicy(h.h);
		Bitmaps bm = readBitmaps(h.h);
		if (!bm.why.empty()) {
			res->why = bm.why;
			return res;
		}
		uint64_t end = util::nowMs() + ScanMs;
		DevMap dm;
		std::vector<Img> imgs;
		bool whole = walkRegions(h.h,stop,end,[&](const MEMORY_BASIC_INFORMATION& mi) {
			CfgReg g;
			g.alloc = (uint64_t)mi.AllocationBase;
			g.base = (uint64_t)mi.BaseAddress;
			g.size = mi.RegionSize;
			g.type = mi.Type;
			g.state = mi.State;
			g.prot = mi.Protect;
			g.typeTxt = mi.Type == MEM_IMAGE ? "Image" : mi.Type == MEM_MAPPED ? "Mapped" : mi.Type == MEM_PRIVATE ? "Private" : "";
			g.stateTxt = mi.State == MEM_COMMIT ? "Commit" : mi.State == MEM_RESERVE ? "Reserve" : mi.State == MEM_FREE ? "Free" : "";
			g.protTxt = protTxt(mi.Protect);
			res->regs.push_back(std::move(g));
			if (mi.Type != MEM_IMAGE || mi.AllocationBase != mi.BaseAddress) return;
			ImgInfo ii = {};
			if (!nt::ok(qvm()(h.h,mi.BaseAddress,MemoryImageInformation_,&ii,sizeof(ii),nullptr)) || !ii.ImageBase) return;
			Img im;
			im.base = (uint64_t)ii.ImageBase;
			im.size = ii.SizeOfImage;
			im.nt = mappedName(h.h,ii.ImageBase);
			imgs.push_back(std::move(im));
		});
		if (res->regs.empty()) {
			res->why = noRegions(h.h);
			return res;
		}
		if (!whole) {
			res->complete = false;
			res->reason = stop ? "cancelled" : "time limit";
		}
		std::vector<uint64_t> image;
		for (auto& im : imgs) {
			if (stop || util::nowMs() > end) {
				res->complete = false;
				res->reason = stop ? "cancelled" : "time limit";
				break;
			}
			if (!im.nt.empty()) addSyms(res->syms,im,dm,&image);
		}
		// Windows can map pages past SizeOfImage for its thunks, they still belong to that file
		for (auto& m : res->syms) for (auto& g : res->regs) if (g.alloc == m.base && g.base + g.size - m.base > m.size) m.size = g.base + g.size - m.base;
		sortSyms(res->syms);
		std::sort(image.begin(),image.end());
		image.erase(std::unique(image.begin(),image.end()),image.end());
		CfgScan sc{h.h,*res,image,dm,stop,end,std::vector<char>(res->regs.size(),0)};
		if (bm.native && bm.nativeSz) sc.bitmap(bm.native,bm.nativeSz,false);
		else res->summary.push_back("No CFG bitmap is present for this process.");
		if (res->complete && !res->truncated && isWow(h.h) && bm.wow && bm.wowSz) sc.bitmap(bm.wow,bm.wowSz,true);
		if (res->truncated) res->summary.push_back("Listing truncated to " + util::count(MaxCfgEntries) + " entries.");
		if (res->unread) res->summary.push_back(util::size(res->unread) + " of the bitmap could not be read.");
		if (res->enabled == 0 && bm.native) res->summary.push_back("The process policy has CFG off, the bitmap is not enforced for it.");
		res->ok = true;
		return res;
	}

	CfgCheck cfgCheck(const ProcRef& r,uint64_t addr) {
		CfgCheck c;
		Hnd h(openRead(r,c.why));
		if (!h.h) return c;
		c.enabled = cfgPolicy(h.h);
		Bitmaps bm = readBitmaps(h.h);
		if (!bm.why.empty()) {
			c.why = bm.why;
			return c;
		}
		c.ok = true;
		c.wow = isWow(h.h) && addr < 0x100000000ull;
		uint64_t base = c.wow ? bm.wow : bm.native;
		uint64_t sz = c.wow ? bm.wowSz : bm.nativeSz;
		uint64_t off = (addr / 64) & ~(uint64_t)7;
		if (!base || !sz) {
			c.noBitmap = true;
			return c;
		}
		if (off >= sz || sz - off < 8) return c;
		uint64_t w = 0;
		SIZE_T got = 0;
		if (!ReadProcessMemory(h.h,(void*)(base + off),&w,8,&got) || got != 8) return c;
		c.val = (int)((w >> (((addr - off * 64) / 16) * 2)) & 3);
		return c;
	}

	std::shared_ptr<const GdiRes> gdi(const ProcRef& r) {
		auto res = std::make_shared<GdiRes>();
		res->unixSec = (uint64_t)time(nullptr);
		std::string why;
		Hnd h(openRef(r,PROCESS_QUERY_LIMITED_INFORMATION,why));
		if (!h.h && why.find("gone") != std::string::npos) {
			res->why = why;
			return res;
		}
		// the snapshot knows the session of a process that refuses to be opened, the table needs no handle
		DWORD ses = 0,me = 0;
		auto list = procs::latest();
		const ProcRow* row = list->find(r);
		if (row) ses = row->session;
		if ((!row && !ProcessIdToSessionId(r.pid,&ses)) || !ProcessIdToSessionId(GetCurrentProcessId(),&me)) {
			res->why = lastErr("session query");
			return res;
		}
		if (ses != me) {
			res->na = true;
			res->why = "The GDI table is per session : this process is in session " + std::to_string(ses) + ", FRS in " + std::to_string(me) + ".";
			return res;
		}
		if (h.h) {
			DWORD c = GetGuiResources(h.h,GR_GDIOBJECTS);
			DWORD p = GetGuiResources(h.h,GR_GDIOBJECTS_PEAK);
			if (c || GetLastError() == 0) res->osCount = c;
			if (p || GetLastError() == 0) res->osPeak = p;
		}
#ifdef _WIN64
		char* peb = *(char**)((char*)NtCurrentTeb() + 0x60);
		char* tab = *(char**)(peb + 0xF8);
#else
		char* peb = *(char**)((char*)NtCurrentTeb() + 0x30);
		char* tab = *(char**)(peb + 0x94);
#endif
		if (!tab) {
			res->na = true;
			res->why = "No GDI table is mapped in FRS.";
			return res;
		}
		MEMORY_BASIC_INFORMATION mi;
		if (!VirtualQuery(tab,&mi,sizeof(mi))) {
			res->why = lastErr("table query");
			return res;
		}
		size_t cnt = std::min((size_t)0xFFFF,(size_t)mi.RegionSize / sizeof(GdiEntry));
		// through a read call : the layout is undocumented and a bad page must not take FRS down
		std::vector<GdiEntry> t(cnt);
		SIZE_T got = 0;
		if (!ReadProcessMemory(GetCurrentProcess(),tab,t.data(),cnt * sizeof(GdiEntry),&got) && got < sizeof(GdiEntry)) {
			res->why = lastErr("table read");
			return res;
		}
		cnt = std::min(cnt,(size_t)got / sizeof(GdiEntry));
		USHORT pid = (USHORT)r.pid;
		if (!pid) {
			res->na = true;
			res->why = "The table keeps only 16 bits of the owner pid and for this pid they are 0, the same as objects nobody owns.";
			return res;
		}
		for (size_t i = 0; i < cnt; i++) {
			if ((USHORT)t[i].Owner != pid) continue;
			const char* ty = gdiType(t[i].Unique);
			if (!ty) {
				if (t[i].Unique) res->other++;
				continue;
			}
			GdiObj o;
			o.handle = ((uint32_t)t[i].Unique << 16) | (uint32_t)i;
			o.object = (uint64_t)t[i].Object;
			o.type = ty;
			o.info = gdiInfo(o.handle);
			res->rows.push_back(std::move(o));
		}
		// the table is per session, so only a pid in this one can land here. Fewer rows than the OS count is normal, most entries carry no owner
		uint32_t twin = 0;
		for (auto& p : list->rows) if (p.pid != r.pid && (USHORT)p.pid == pid && p.session == ses) twin = p.pid;
		size_t shown = res->rows.size() + res->other;
		if (twin && shown) res->note = "Pid " + std::to_string(twin) + " has the same low 16 bits as this pid and the table keeps only those, so these rows can be its objects. Only the OS count is this process's own.";
		else if (res->osCount != NA && shown > res->osCount) res->note = "The table lists more objects than the OS counts for this process. It keeps only 16 bits of the owner, so these rows can be another process's objects.";
		res->ok = true;
		return res;
	}

	FlushRes flush(const ProcRef& r) {
		FlushRes o;
		Hnd h(openRef(r,PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_LIMITED_INFORMATION | PROCESS_VM_READ,o.why));
		if (!h.h) return o;
		memOf(h.h,o.privBefore,o.wsBefore);
		static auto mkThr = nt::fn<CreateThr>("NtCreateThreadEx");
		static auto apcEx = nt::fn<QueueApcEx>("NtQueueApcThreadEx");
		static auto apcW64 = nt::fn<QueueApcW64>("RtlQueueApcWow64Thread");
		static auto resume = nt::fn<ResumeThr>("NtResumeThread");
		void* flushFn = nt::proc("RtlFlushHeaps");
		void* exitFn = nt::proc("RtlExitUserThread");
		bool wow = isWow(h.h);
		if (!mkThr || !apcEx || !resume || !flushFn || !exitFn || (wow && !apcW64)) {
			o.why = "this Windows lacks a call flush needs";
			return o;
		}
		if (wow) {
			flushFn = exitFn = nullptr;
			if (!wow64Procs(h.h,flushFn,exitFn,o.why)) return o;
		}
		if (notStarted(h.h,wow)) {
			o.why = "this process has not started yet, its loader has not run (created suspended). A flush now would crash it, nothing was done";
			return o;
		}
		KeepAwake awake(h.h);
		HANDLE th = nullptr;
		Status st = mkThr(&th,SYNCHRONIZE | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_TERMINATE | THREAD_QUERY_LIMITED_INFORMATION,nullptr,h.h,exitFn,nullptr,ThreadCreateSuspended_,0,0,0,nullptr);
		if (!nt::ok(st)) {
			o.why = "thread : " + stTxt(st);
			return o;
		}
		Hnd t(th);
		st = wow ? apcW64(th,flushFn,nullptr,nullptr,nullptr) : apcEx(th,(HANDLE)1,flushFn,nullptr,nullptr,nullptr); // 1 = special user APC
		if (!nt::ok(st)) {
			// the suspended thread would stay forever
			TerminateThread(th,0);
			o.why = "apc : " + stTxt(st);
			return o;
		}
		st = resume(th,nullptr);
		if (!nt::ok(st)) {
			TerminateThread(th,0);
			o.why = "resume : " + stTxt(st);
			return o;
		}
		DWORD w = WaitForSingleObject(th,4000);
		if (w == WAIT_TIMEOUT) {
			o.why = "no answer in 4 s : a heap lock is held or the target is frozen. The thread stays in there until it can finish, so another flush or query of this process is refused until then, retrying does not help";
			memOf(h.h,o.privAfter,o.wsAfter);
			uint32_t pid = r.pid;
			auto ran = std::make_shared<std::atomic<bool>>(false);
			// a retry would stack one more thread in there, so the claim lasts as long as this one
			auto wt = task::run([th,pid,ran](task::T& tk) {
				*ran = true;
				DWORD e;
				while ((e = WaitForSingleObject(th,200)) == WAIT_TIMEOUT && !tk.cancel) {}
				CloseHandle(th);
				unclaim(pid);
				if (e == WAIT_OBJECT_0) appLog("Flush heaps (pid " + std::to_string(pid) + ") : the blocked thread ended, the process takes queries again");
				return std::string();
			},false);
			// done without having run : no thread could be started, the caller lets the process go
			if (!wt->done || *ran) {
				t.h = nullptr;
				o.held = true;
			}
			return o;
		}
		DWORD ec = 1;
		if (w != WAIT_OBJECT_0 || !GetExitCodeThread(th,&ec)) {
			o.why = lastErr("wait");
			return o;
		}
		bool dead = exited(h.h);
		if (ec || dead) {
			o.why = dead ? "the target died during the flush, the thread ended with " + hex32(ec) : "the thread ended with " + hex32(ec);
			return o;
		}
		memOf(h.h,o.privAfter,o.wsAfter);
		o.ok = true;
		return o;
	}

}
}
#endif // _WIN32
