#ifdef _WIN32
#include "vmem.h"
#include "nt.h"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <utility>

namespace {

	using namespace vmem;

	typedef nt::Status(NTAPI* QVM)(HANDLE,PVOID,ULONG,PVOID,SIZE_T,PSIZE_T);
	typedef nt::Status(NTAPI* RWVM)(HANDLE,PVOID,PVOID,SIZE_T,PSIZE_T);
	typedef nt::Status(NTAPI* PVM)(HANDLE,PVOID*,PSIZE_T,ULONG,PULONG);
	typedef nt::Status(NTAPI* FVM)(HANDLE,PVOID*,PSIZE_T,ULONG);
	typedef nt::Status(NTAPI* ULVM)(HANDLE,PVOID*,PSIZE_T,ULONG);
	typedef nt::Status(NTAPI* UVS)(HANDLE,PVOID);
	typedef nt::Status(NTAPI* QIP)(HANDLE,ULONG,PVOID,ULONG,PULONG);
	typedef nt::Status(NTAPI* QSI)(ULONG,PVOID,ULONG,PULONG);
	typedef nt::Status(NTAPI* QS)(HANDLE,ULONG,PVOID,SIZE_T,PSIZE_T);
	typedef nt::Status(NTAPI* QO)(HANDLE,ULONG,PVOID,ULONG,PULONG);
	typedef nt::Status(NTAPI* MVS)(HANDLE,HANDLE,PVOID*,ULONG_PTR,SIZE_T,PLARGE_INTEGER,PSIZE_T,ULONG,ULONG,ULONG);
	typedef nt::Status(NTAPI* RGV)(OSVERSIONINFOW*);

	const ULONG MemoryWorkingSetInformation_ = 1;
	const ULONG MemoryMappedFilenameInformation_ = 2;
	const ULONG MemoryWorkingSetExInformation_ = 4;
	const ULONG MemoryImageInformation_ = 6;
	const ULONG MemoryRegionInformationEx_ = 7;
	const ULONG ProcessBasicInformation_ = 0;
	const ULONG ProcessWow64Information_ = 26;
	const ULONG ProcessBreakOnTermination_ = 29;
	const ULONG ProcessProtectionInformation_ = 61;
	const ULONG SystemExtendedProcessInformation_ = 57;
	const ULONG SystemExtendedHandleInformation_ = 64;
	const ULONG SystemHypervisorSharedPageInformation_ = 197;
	const ULONG SectionBasicInformation_ = 0;
	const ULONG ObjectNameInformation_ = 1;
	const ULONG MAP_PROCESS_ = 1;
	const ULONG ViewUnmap_ = 2;
	const ULONG SEC_COMMIT_ = 0x8000000;
	const ULONG SEC_FILE_ = 0x800000;
	const ULONG SEC_IMAGE_ = 0x1000000;
	const ULONG SEC_RESERVE_ = 0x4000000;
	const nt::Status NOT_LOCKED = (nt::Status)0xC000002AL;
	const nt::Status SECTION_PROTECTION = (nt::Status)0xC000004EL;
	const uint64_t USER_SHARED_DATA_ = 0x7FFE0000;
	const uint64_t GRAN = 0x10000;
	const uint64_t PG = 0x1000;

	// phnt ntmmapi.h
	struct RegInfo {
		PVOID AllocationBase;
		ULONG AllocationProtect,RegionType;
		SIZE_T RegionSize,CommitSize;
	};

	struct ImgInfo {
		PVOID ImageBase;
		SIZE_T SizeOfImage;
		ULONG ImageFlags; // bit 0 partial, 1 not executable, 2..5 signing level
	};

	// MEMORY_WORKING_SET_EX_INFORMATION. Bits : 0 valid, 22 locked, 24..26 priority, 30 shared original, 31 bad
	struct WsEx {
		PVOID va;
		ULONG_PTR bits;
	};

	struct SecBasic {
		PVOID BaseAddress;
		ULONG AllocationAttributes;
		LARGE_INTEGER MaximumSize;
	};

	// phnt ntexapi.h, SYSTEM_EXTENDED_THREAD_INFORMATION
	struct XThread {
		nt::STI t;
		PVOID StackBase,StackLimit,Win32StartAddress,TebBaseAddress;
		ULONG_PTR Reserved2,Reserved3,Reserved4;
	};

	// SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX
	struct HEntry {
		PVOID Object;
		ULONG_PTR pid,handle;
		ACCESS_MASK access;
		USHORT backTrace,type;
		ULONG attrs,res;
	};

#ifdef _WIN64
	static_assert(sizeof(MEMORY_BASIC_INFORMATION) == 0x30,"MBI");
	static_assert(sizeof(RegInfo) == 0x20,"RegInfo");
	static_assert(sizeof(ImgInfo) == 0x18,"ImgInfo");
	static_assert(sizeof(WsEx) == 0x10,"WsEx");
	static_assert(sizeof(SecBasic) == 0x18,"SecBasic");
	static_assert(sizeof(XThread) == 0x88,"XThread");
	static_assert(sizeof(HEntry) == 0x28,"HEntry");
#endif

	// PEB and TEB offsets, phnt ntpebteb.h and ntwow64.h. A table so the two walks read the same.
	struct PebPtr {
		uint32_t off64,off32;
		Kind k;
		const char* use;
	};
	const PebPtr pebPtrs[] = {
		{0x20,0x10,KParams,"USER_PROCESS_PARAMETERS"},
		{0x68,0x38,KApiSet,"ApiSetMap"},
		{0x88,0x4C,KCsr,"CSR shared memory"},
		{0xA0,0x58,KCodePage,"CodePage data"},
		{0xF8,0x94,KGdi,"GDI shared handle table"},
		{0x2D8,0x1E8,KShim,"Shim data"},
		{0x2F8,0x1F8,KActCtx,"Process activation context data"},
		{0x308,0x200,KActCtx,"System activation context data"},
		{0x358,0x230,KWer,"WER registration data"},
		{0x90,0x50,KSilo,"Silo shared data"},
		{0x7A0,0x45C,KTelemetry,"Telemetry coverage map"},
		{0x7B8,0x470,KLeap,"LEAP_SECOND_DATA"},
	};
	const uint32_t PEB_HEAPS = 0xE8,PEB_HEAPPTR = 0xF0;
	const uint32_t PEB32_HEAPS = 0x88,PEB32_HEAPPTR = 0x90;
	const uint32_t TEB_DESKTOP = 0x828; // Win32ClientInfo 0x800 + DesktopBaseAddress 0x28
	const uint32_t MAX_HEAPS = 1000;

	QVM qvm() {
		static auto f = nt::fn<QVM>("NtQueryVirtualMemory");
		return f;
	}

	bool rd(HANDLE h,uint64_t a,void* out,size_t n) {
		static auto f = nt::fn<RWVM>("NtReadVirtualMemory");
		SIZE_T got = 0;
		return f && nt::ok(f(h,(PVOID)a,out,n,&got)) && got == n;
	}

	ULONG build() {
		static ULONG b = [] {
			OSVERSIONINFOW v = {};
			v.dwOSVersionInfoSize = sizeof(v);
			auto f = nt::fn<RGV>("RtlGetVersion");
			return f && nt::ok(f(&v)) ? v.dwBuildNumber : 0UL;
		}();
		return b;
	}

	std::string utf8(const wchar_t* w,size_t n) {
		if (!n) return "";
		int k = WideCharToMultiByte(CP_UTF8,0,w,(int)n,nullptr,0,nullptr,nullptr);
		std::string s(k > 0 ? k : 0,'\0');
		if (k > 0) WideCharToMultiByte(CP_UTF8,0,w,(int)n,&s[0],k,nullptr,nullptr);
		return util::safe(s);
	}

	std::string stText(nt::Status st) {
		if (st == nt::ACCESS_DENIED) return "access denied";
		if (st == nt::PROCESS_IS_TERMINATING) return "the process is exiting";
		if (st == nt::PARTIAL_COPY) return "some pages can't be reached";
		if (st == nt::INVALID_PARAMETER) return "the address or size does not fit this region";
		if (st == (nt::Status)0xC0000045L) return "the kernel refused, invalid page protection for this memory";
		if (st == (nt::Status)0xC000001AL) return "that address is not the start of an allocation";
		if (st == (nt::Status)0xC000001BL) return "this view can't be unmapped that way";
		if (st == (nt::Status)0xC0000019L) return "no view is mapped at that address";
		if (st == (nt::Status)0xC000002CL) return "this memory can't be decommitted";
		if (st == (nt::Status)0xC00000A0L) return "nothing is allocated at that address";
		if (st == SECTION_PROTECTION) return "this mapped view was not opened for that protection, it allows at most its allocation protection";
		if (st == (nt::Status)0xC00000F2L) return "copy on write only works on mapped memory, use RW or RWX here";
		char b[32];
		snprintf(b,sizeof(b),"NTSTATUS 0x%08lX",(unsigned long)st);
		return b;
	}

	std::string protOf(HANDLE h) {
		static auto q = nt::fn<QIP>("NtQueryInformationProcess");
		unsigned char lv = 0;
		if (!q || !nt::ok(q(h,ProcessProtectionInformation_,&lv,1,nullptr)) || !(lv & 7)) return "";
		static const char* signers[] = {"None","Authenticode","CodeGen","Antimalware","Lsa","Windows","WinTcb","WinSystem","App"};
		int sg = lv >> 4;
		std::string s = (lv & 7) == 2 ? "PP" : "PPL";
		s += ", signer ";
		s += sg < 9 ? signers[sg] : "Unknown";
		return s;
	}

	std::string protMsg(const std::string& p) {
		return "protected process (" + p + "), memory contents need the signed kernel driver, FRS has none";
	}

	std::string whyNot(uint32_t pid,DWORD e) {
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
		std::string p;
		if (h) {
			p = protOf(h);
			CloseHandle(h);
		}
		if (!p.empty()) return protMsg(p);
		if (e == ERROR_ACCESS_DENIED) return "access denied, run FRS elevated for processes of other users";
		if (e == ERROR_INVALID_PARAMETER) return "the process has exited";
		return "error " + std::to_string(e);
	}

	bool same(HANDLE h,uint64_t start) {
		FILETIME c,e,k,u;
		if (!GetProcessTimes(h,&c,&e,&k,&u)) return false;
		return (((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime) == start;
	}

	struct Proc {
		HANDLE h = nullptr;
		Proc(const ProcRef& r,DWORD acc,std::string& err) {
			h = OpenProcess(acc | PROCESS_QUERY_LIMITED_INFORMATION,FALSE,r.pid);
			if (!h) {
				err = whyNot(r.pid,GetLastError());
				return;
			}
			if (r.start && !same(h,r.start)) {
				CloseHandle(h);
				h = nullptr;
				err = "the process has exited, its pid was reused";
			}
		}
		~Proc() {
			if (h) CloseHandle(h);
		}
		Proc(const Proc&) = delete;
		Proc& operator=(const Proc&) = delete;
	};

	std::string protStr(ULONG p) {
		if (!p) return "";
		const char* b = "?";
		switch (p & 0xFF) {
			case PAGE_NOACCESS: b = "NA"; break;
			case PAGE_READONLY: b = "R"; break;
			case PAGE_READWRITE: b = "RW"; break;
			case PAGE_WRITECOPY: b = "WC"; break;
			case PAGE_EXECUTE: b = "X"; break;
			case PAGE_EXECUTE_READ: b = "RX"; break;
			case PAGE_EXECUTE_READWRITE: b = "RWX"; break;
			case PAGE_EXECUTE_WRITECOPY: b = "WCX"; break;
		}
		std::string s = b;
		if (p & PAGE_GUARD) s += "+G";
		if (p & PAGE_NOCACHE) s += "+NC";
		if (p & PAGE_WRITECOMBINE) s += "+WCM";
		return s;
	}

	bool protWr(ULONG p) {
		return (p & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
	}

	bool protEx(ULONG p) {
		return (p & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
	}

	// execute only pages read fine through NtReadVirtualMemory
	bool protRd(ULONG p) {
		return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
	}

	void fromMbi(const MEMORY_BASIC_INFORMATION& m,Region& x) {
		x.base = (uint64_t)m.BaseAddress;
		x.size = m.RegionSize;
		x.allocBase = (uint64_t)m.AllocationBase;
		x.state = m.State == MEM_COMMIT ? StCommit : m.State == MEM_RESERVE ? StReserve : StFree;
		x.type = m.State == MEM_FREE ? TyNone : m.Type == MEM_PRIVATE ? TyPrivate : m.Type == MEM_MAPPED ? TyMapped : m.Type == MEM_IMAGE ? TyImage : TyNone;
		x.prot = protStr(m.Protect);
		x.allocProt = protStr(m.AllocationProtect);
		x.rd = m.State == MEM_COMMIT && protRd(m.Protect) && !(m.Protect & PAGE_GUARD);
		x.wr = m.State == MEM_COMMIT && protWr(m.Protect) && !(m.Protect & PAGE_GUARD);
		x.ex = m.State == MEM_COMMIT && protEx(m.Protect);
		x.allocEx = protEx(m.AllocationProtect);
		x.guard = m.State == MEM_COMMIT && (m.Protect & PAGE_GUARD);
		if (m.State == MEM_COMMIT) {
			x.committed = m.RegionSize;
			if (m.Type == MEM_PRIVATE) x.priv = m.RegionSize;
		}
	}

	std::vector<std::pair<std::wstring,std::wstring>> devMap() {
		std::vector<std::pair<std::wstring,std::wstring>> out;
		wchar_t d[3] = L"A:";
		wchar_t t[1024];
		for (wchar_t c = L'A'; c <= L'Z'; c++) {
			d[0] = c;
			if (QueryDosDeviceW(d,t,1024)) out.emplace_back(t,d);
		}
		return out;
	}

	std::string dosPath(const std::wstring& p,const std::vector<std::pair<std::wstring,std::wstring>>& devs) {
		for (auto& kv : devs) {
			size_t n = kv.first.size();
			if (p.size() > n && p[n] == L'\\' && p.compare(0,n,kv.first) == 0) {
				std::wstring s = kv.second + p.substr(n);
				return utf8(s.data(),s.size());
			}
		}
		const std::wstring mup = L"\\Device\\Mup\\";
		if (p.compare(0,mup.size(),mup) == 0) {
			std::wstring s = L"\\\\" + p.substr(mup.size());
			return utf8(s.data(),s.size());
		}
		return utf8(p.data(),p.size());
	}

	// the name only, no \Device resolving here so a walk over every process stays cheap
	bool mappedName(HANDLE h,uint64_t addr,std::wstring& out) {
		std::vector<char> b(0x200);
		for (int i = 0; i < 4; i++) {
			SIZE_T need = 0;
			nt::Status st = qvm()(h,(PVOID)addr,MemoryMappedFilenameInformation_,b.data(),b.size(),&need);
			if (st == nt::BUFFER_OVERFLOW || st == nt::INFO_LENGTH_MISMATCH || st == nt::BUFFER_TOO_SMALL) {
				b.resize(std::max(need,b.size() * 2));
				continue;
			}
			if (!nt::ok(st)) return false;
			auto* u = (nt::USTR*)b.data();
			if (!u->Buffer || !u->Length) return false;
			out.assign(u->Buffer,u->Length / 2);
			return true;
		}
		return false;
	}

	int lookup(const std::vector<Region>& v,uint64_t a) {
		auto it = std::upper_bound(v.begin(),v.end(),a,[](uint64_t x,const Region& r) { return x < r.base; });
		if (it == v.begin()) return -1;
		--it;
		return a - it->base < it->size ? (int)(it - v.begin()) : -1;
	}

	int rootOf(const std::vector<Region>& v,int i) {
		int b = lookup(v,v[i].allocBase);
		return b >= 0 && v[b].base == v[i].allocBase ? b : -1;
	}

	// the new row starts at a and has no tag, the old one ends there. Don't copy a stale index past this.
	int splitAt(std::vector<Region>& v,int i,uint64_t a) {
		uint64_t st = v[i].base,en = st + v[i].size;
		if (a <= st || a >= en) return -1;
		Region n = v[i];
		bool cm = v[i].committed == v[i].size;
		bool pv = v[i].priv == v[i].size;
		n.base = a;
		n.size = en - a;
		n.kind = KNone;
		n.use.clear();
		n.tid = 0;
		n.heapId = -1;
		n.heapClass.clear();
		n.valid = n.bad = false;
		n.committed = cm ? n.size : 0;
		n.priv = pv ? n.size : 0;
		v[i].size = a - st;
		if (cm) v[i].committed = v[i].size;
		if (pv) v[i].priv = v[i].size;
		v.insert(v.begin() + i + 1,std::move(n));
		return i + 1;
	}

	// PhpSetMemoryRegionType : first tag wins unless the address is inside a tagged region, then that gets split
	int tag(std::vector<Region>& v,uint64_t a,bool toBase,Kind k,const std::string& use) {
		int i = lookup(v,a);
		if (i < 0) return -1;
		if (toBase) {
			int b = rootOf(v,i);
			if (b >= 0) i = b;
		}
		if (!toBase && v[i].base != a) {
			int n = splitAt(v,i,a);
			if (n >= 0) i = n;
			else if (v[i].kind != KNone) return -1;
		}
		if (v[i].kind != KNone) return -1;
		v[i].kind = k;
		v[i].use = use;
		return i;
	}

	const char* heapClassName(ULONG c) {
		static const char* t[] = {"Process Heap","Private Heap","Kernel Heap","GDI Heap","User Heap","Console Heap","Desktop Heap","CSRSS Shared Heap","CSRSS Port Heap"};
		ULONG i = (c & 0xF000) >> 12;
		return i < 9 ? t[i] : "Unknown Heap";
	}

	// heap header : native heap at 0x98 or 0x60 for 32 bit, segment heap at 0x10 or 0x08. Layouts from SI heapstruct.h
	bool heapClass(HANDLE h,uint64_t a,std::string& out) {
		unsigned char b[0x100];
		if (!rd(h,a,b,sizeof(b))) return false;
		auto u32 = [&](size_t o) {
			uint32_t v;
			memcpy(&v,b + o,4);
			return v;
		};
		const uint32_t HS = 0xEEFFEEFF,SG = 0xDDEEDDEE;
		ULONG f = 0;
		if (u32(0x98) == HS) f = u32(0x70);
		else if (u32(0x60) == HS) f = u32(0x40);
		else if (u32(0x10) == SG) f = u32(0x14);
		else if (u32(0x08) == SG) f = u32(0x0C);
		else return false;
		out = heapClassName(f);
		return true;
	}

	void tagHeaps(HANDLE h,std::vector<Region>& v,uint64_t peb,bool is32) {
		ULONG n = 0;
		if (!rd(h,peb + (is32 ? PEB32_HEAPS : PEB_HEAPS),&n,4) || !n || n >= MAX_HEAPS) return;
		std::vector<uint64_t> hs(n);
		if (is32) {
			ULONG p = 0;
			std::vector<ULONG> h32(n);
			if (!rd(h,peb + PEB32_HEAPPTR,&p,4) || !rd(h,p,h32.data(),n * 4)) return;
			for (ULONG i = 0; i < n; i++) hs[i] = h32[i];
		} else {
			uint64_t p = 0;
			if (!rd(h,peb + PEB_HEAPPTR,&p,8) || !rd(h,p,hs.data(),n * 8)) return;
		}
		for (ULONG i = 0; i < n; i++) {
			std::string cl;
			bool has = !is32 && heapClass(h,hs[i],cl);
			std::string use = (has ? cl : std::string("Heap")) + (is32 ? " 32-bit" : "") + " (ID " + std::to_string(i + 1) + ")";
			int k = tag(v,hs[i],true,is32 ? KHeap32 : KHeap,use);
			if (k < 0) continue;
			v[k].heapId = (int)i + 1;
			if (has) v[k].heapClass = cl;
		}
	}

	void tagPeb(HANDLE h,std::vector<Region>& v,uint64_t peb,bool is32,bool split) {
		tag(v,peb,!split,is32 ? KPeb32 : KPeb,is32 ? "PEB 32-bit" : "PEB");
		tagHeaps(h,v,peb,is32);
		for (auto& e : pebPtrs) {
			uint64_t p = 0;
			if (is32) {
				ULONG q = 0;
				if (!rd(h,peb + e.off32,&q,4)) continue;
				p = q;
			} else if (!rd(h,peb + e.off64,&p,8)) continue;
			if (p) tag(v,p,true,e.k,e.use);
		}
	}

	void tagThreads(HANDLE h,std::vector<Region>& v,uint32_t pid,bool wow,bool split) {
		std::vector<char> b;
		if (!nt::ok(nt::querySys(SystemExtendedProcessInformation_,b)) || b.size() < sizeof(nt::SPI)) return;
		const char* c = b.data();
		for (;;) {
			auto* p = (const nt::SPI*)c;
			if ((uint32_t)(uintptr_t)p->UniqueProcessId == pid) {
				auto* ts = (const XThread*)p->Threads;
				for (ULONG i = 0; i < p->NumberOfThreads; i++) {
					uint64_t teb = (uint64_t)ts[i].TebBaseAddress;
					if (!teb) continue;
					uint32_t id = (uint32_t)(uintptr_t)ts[i].t.ClientId.UniqueThread;
					std::string sid = " (thread " + std::to_string(id) + ")";
					int k = tag(v,teb,!split,KTeb,"TEB" + sid);
					if (k >= 0) v[k].tid = id;
					uint64_t tib[3];
					if (rd(h,teb,tib,sizeof(tib))) {
						if (tib[2] < tib[1]) {
							k = tag(v,tib[2],true,KStack,"Stack" + sid);
							if (k >= 0) v[k].tid = id;
						}
						if (wow && tib[0]) {
							ULONG t32[3];
							if (rd(h,(uint32_t)tib[0],t32,sizeof(t32)) && t32[2] < t32[1]) {
								k = tag(v,t32[2],true,KStack32,"Stack 32-bit" + sid);
								if (k >= 0) v[k].tid = id;
							}
						}
					}
					// meant ClientDelta before RS2
					uint64_t dk = 0;
					if (split && rd(h,teb + TEB_DESKTOP,&dk,8) && dk) tag(v,dk,true,KDeskHeap,"Desktop heap");
				}
				return;
			}
			if (!p->NextEntryOffset) return;
			c += p->NextEntryOffset;
		}
	}

	// the Heap field of a HEAP_SEGMENT header, 0 when b is not one. b holds 0x70 bytes
	uint64_t segHeap(const unsigned char* b,Kind& want) {
		uint32_t s64,s32;
		memcpy(&s64,b + 0x10,4);
		memcpy(&s32,b + 0x08,4);
		const uint32_t SEG = 0xFFEEFFEE;
		uint64_t hp = 0;
		want = KHeap;
		if (s64 == SEG) memcpy(&hp,b + 0x28,8);
		else if (s32 == SEG) {
			uint32_t q;
			memcpy(&q,b + 0x18,4);
			hp = q;
			want = KHeap32;
		}
		return hp;
	}

	// newer Windows keeps HeapCreate heaps out of the PEB list. A heap's first segment is the heap itself, so its Heap field points home
	void tagOwnHeaps(HANDLE h,std::vector<Region>& v,const std::atomic<bool>* stop) {
		for (auto& x : v) {
			if (stop && *stop) return;
			if (x.kind != KNone || x.type == TyImage || x.base != x.allocBase || x.state != StCommit || !x.valid || x.bad || !x.rd) continue;
			unsigned char b[0x70];
			Kind want = KHeap;
			if (!rd(h,x.base,b,sizeof(b)) || segHeap(b,want) != x.base) continue;
			std::string cl;
			bool has = want == KHeap && heapClass(h,x.base,cl);
			x.kind = want;
			x.use = want == KHeap32 ? "Heap 32-bit" : has ? cl : "Heap";
			if (has) x.heapClass = cl;
		}
	}

	bool tagFile(HANDLE h,Region& x,const std::vector<std::pair<std::wstring,std::wstring>>& devs) {
		if (x.type == TyImage) {
			ImgInfo ii = {};
			if (nt::ok(qvm()(h,(PVOID)x.base,MemoryImageInformation_,&ii,sizeof(ii),nullptr))) x.signing = (int)((ii.ImageFlags >> 2) & 15);
		}
		std::wstring nm;
		if (!mappedName(h,x.base,nm)) return false;
		x.kind = KFile;
		x.file = x.use = dosPath(nm,devs);
		return true;
	}

	// reads only pages that are resident already, so the scan does not grow the working set it reports
	void tagScan(HANDLE h,std::vector<Region>& v,const std::atomic<bool>* stop) {
		auto devs = devMap();
		for (size_t i = 0; i < v.size(); i++) {
			if (stop && *stop) return;
			Region& x = v[i];
			if (x.kind != KNone) continue;
			bool root = x.base == x.allocBase;
			if ((x.type == TyMapped || x.type == TyImage) && root && tagFile(h,x,devs)) continue;
			if (x.state == StCommit && x.valid && !x.bad && x.rd) {
				unsigned char b[0x70];
				if (rd(h,x.base,b,sizeof(b))) {
					Kind want = KHeap;
					uint64_t hp = segHeap(b,want);
					if (hp) {
						int k = lookup(v,hp);
						if (k >= 0 && v[k].base == hp && v[k].kind == want) {
							bool n32 = want == KHeap32;
							std::string cl = v[k].heapClass.empty() ? "Heap" : v[k].heapClass;
							x.kind = n32 ? KHeapSeg32 : KHeapSeg;
							x.heapId = v[k].heapId;
							x.heapClass = v[k].heapClass;
							x.use = cl + " Segment" + (n32 ? " 32-bit" : "") + (v[k].heapId > 0 ? " (ID " + std::to_string(v[k].heapId) + ")" : "");
							continue;
						}
					}
				}
			}
			if (x.type == TyMapped && root && x.allocProt == "R" && x.rd) {
				uint32_t magic = 0;
				if (rd(h,x.base,&magic,4) && magic == 0x78746341) { // Actx
					x.kind = KActCtx;
					x.use = "Activation context data";
				}
			}
		}
	}

	// the 64 bit ntdll sits at one address in every process, so its export is where the block is in the target too
	void tagCfg(HANDLE h,std::vector<Region>& v,bool wow) {
		HMODULE nd = GetModuleHandleW(L"ntdll.dll");
		uint64_t a = nd ? (uint64_t)GetProcAddress(nd,"LdrSystemDllInitBlock") : 0;
		if (!a) return;
		int r = lookup(v,a);
		if (r < 0 || v[r].allocBase != (uint64_t)nd) return;
		// PS_SYSTEM_DLL_INIT_BLOCK : V3 from 20H1, V2 from RS2. Offsets of CfgBitMap and Wow64CfgBitMap
		uint32_t oCfg = build() >= 19041 ? 0xB8 : build() >= 15063 ? 0xB0 : 0x60;
		uint32_t oWow = oCfg + 0x10;
		uint32_t sz = 0;
		uint64_t bm = 0;
		if (!rd(h,a,&sz,4)) return;
		auto mark = [&](uint32_t o,Kind k,const char* use) {
			if (sz < o + 8 || !rd(h,a + o,&bm,8) || !bm) return;
			int i = lookup(v,bm);
			if (i < 0) return;
			uint64_t root = v[i].base;
			for (size_t j = i; j < v.size() && v[j].allocBase == root; j++) {
				v[j].kind = k;
				v[j].use = use;
			}
		};
		mark(oCfg,KCfg,"CFG Bitmap");
		if (wow) mark(oWow,KCfg32,"CFG Bitmap 32-bit");
	}

	void classify(HANDLE h,std::vector<Region>& v,uint32_t pid,const std::atomic<bool>* stop) {
		bool split = build() >= 15063; // RS2 put PEB and TEB in sub regions of one VAD
		tag(v,USER_SHARED_DATA_,true,KUserShared,"USER_SHARED_DATA");
		static uint64_t hv = [] {
			auto f = nt::fn<QSI>("NtQuerySystemInformation");
			PVOID p = nullptr;
			return f && nt::ok(f(SystemHypervisorSharedPageInformation_,&p,sizeof(p),nullptr)) ? (uint64_t)p : 0ULL;
		}();
		if (hv) tag(v,hv,true,KHvShared,"HYPERVISOR_SHARED_DATA");
		static auto q = nt::fn<QIP>("NtQueryInformationProcess");
		if (!q) return;
		struct { PVOID e; PVOID peb; ULONG_PTR a,b,c,d; } pbi = {};
		if (nt::ok(q(h,ProcessBasicInformation_,&pbi,sizeof(pbi),nullptr)) && pbi.peb) tagPeb(h,v,(uint64_t)pbi.peb,false,split);
		PVOID p32 = nullptr;
		bool wow = nt::ok(q(h,ProcessWow64Information_,&p32,sizeof(p32),nullptr)) && p32;
		if (wow) tagPeb(h,v,(uint64_t)p32,true,split);
		tagThreads(h,v,pid,wow,split);
		tagOwnHeaps(h,v,stop);
		tagScan(h,v,stop);
		tagCfg(h,v,wow);
	}

	void wsApplyEx(WsEx* e,int* idx,int n,std::vector<Region>& v) {
		for (int i = 0; i < n; i++) {
			ULONG_PTR b = e[i].bits;
			if (!(b & 1)) continue;
			Region& x = v[idx[i]];
			if ((b >> 22) & 1) x.wsLocked += PG;
			int pr = (int)((b >> 24) & 7);
			if (pr > x.prio) x.prio = pr;
			if (x.type == TyMapped || x.type == TyImage) {
				if ((b >> 30) & 1) x.pagesOrig++;
				else x.pagesMod++;
			}
		}
	}

	// MemoryWorkingSetInformation then the Ex attributes of the resident pages, batched
	bool wsWalk(HANDLE h,std::vector<Region>& v,const std::atomic<bool>* stop) {
		std::vector<ULONG_PTR> b(0x2000);
		nt::Status st = nt::INFO_LENGTH_MISMATCH;
		for (int i = 0; i < 12 && st == nt::INFO_LENGTH_MISMATCH; i++) {
			st = qvm()(h,nullptr,MemoryWorkingSetInformation_,b.data(),b.size() * sizeof(ULONG_PTR),nullptr);
			if (st == nt::INFO_LENGTH_MISMATCH) {
				size_t need = b[0] + 64;
				need += need / 8;
				if (need > ((size_t)1 << 28)) return false;
				b.resize(std::max(need,b.size() * 2));
			}
		}
		if (!nt::ok(st)) return false;
		for (auto& x : v) {
			x.ws = x.wsPriv = x.wsShareable = x.wsShared = 0;
			x.wsLocked = 0;
			if (x.state == StCommit && (x.type == TyMapped || x.type == TyImage)) x.pagesOrig = x.pagesMod = 0;
		}
		const int BATCH = 256;
		WsEx ex[BATCH];
		int idx[BATCH];
		int nx = 0;
		bool exOk = true;
		int cur = -1;
		size_t n = b[0];
		for (size_t i = 0; i < n; i++) {
			if (stop && *stop) return false;
			ULONG_PTR e = b[i + 1];
			uint64_t va = (uint64_t)(e >> 12) * PG;
			if (cur < 0 || va < v[cur].base || va - v[cur].base >= v[cur].size) cur = lookup(v,va);
			if (cur < 0) continue;
			Region& x = v[cur];
			int shares = (int)((e >> 5) & 7);
			x.ws += PG;
			if (shares > 1) x.wsShared += PG;
			if (shares == 0) x.wsPriv += PG;
			if ((e >> 8) & 1) x.wsShareable += PG;
			ex[nx].va = (PVOID)va;
			ex[nx].bits = 0;
			idx[nx++] = cur;
			if (nx == BATCH) {
				if (nt::ok(qvm()(h,nullptr,MemoryWorkingSetExInformation_,ex,nx * sizeof(WsEx),nullptr))) wsApplyEx(ex,idx,nx,v);
				else exOk = false;
				nx = 0;
			}
		}
		if (nx) {
			if (nt::ok(qvm()(h,nullptr,MemoryWorkingSetExInformation_,ex,nx * sizeof(WsEx),nullptr))) wsApplyEx(ex,idx,nx,v);
			else exOk = false;
		}
		if (!exOk) for (auto& x : v) {
			x.wsLocked = x.pagesOrig = x.pagesMod = NA;
			x.prio = -1;
		}
		return true;
	}

	std::string regionFlags(ULONG t) {
		if (!t) return "";
		static const char* names[] = {
			"Private","MappedDataFile","MappedImage","MappedPageFile","MappedPhysical","DirectMapped","Software enclave",
			"PageSize64K","Placeholder","Mapped AWE","MappedWriteWatch","PageSizeLarge","PageSizeHuge"
		};
		std::string s;
		for (int i = 0; i < 13; i++) if (t & (1u << i)) {
			if (!s.empty()) s += ", ";
			s += names[i];
		}
		char b[24];
		snprintf(b,sizeof(b),"%s(0x%lx)",s.empty() ? "" : " ",(unsigned long)t);
		return s + b;
	}

	void walk(HANDLE h,std::vector<Region>& v,bool detail,const std::atomic<bool>* stop) {
		uint64_t a = 0;
		for (;;) {
			if (stop && *stop) return;
			MEMORY_BASIC_INFORMATION m;
			if (!nt::ok(qvm()(h,(PVOID)a,0,&m,sizeof(m),nullptr)) || !m.RegionSize) return; // past the top of user space is how this ends
			Region x;
			fromMbi(m,x);
			a = x.base + x.size;
			if (m.State == MEM_FREE) {
				x.allocBase = x.base;
				if (x.base & (GRAN - 1)) {
					// not on an allocation boundary, nobody can allocate here
					uint64_t nx = (x.base + GRAN) & ~(GRAN - 1);
					x.unusable = true;
					x.kind = KUnusable;
					if (nx < x.base + x.size) {
						Region o = x;
						o.base = o.allocBase = nx;
						o.size = x.base + x.size - nx;
						o.unusable = false;
						o.kind = KNone;
						x.size = nx - x.base;
						v.push_back(x);
						v.push_back(o);
						continue;
					}
				}
				v.push_back(x);
				continue;
			}
			if (detail) {
				WsEx e = {};
				e.va = (PVOID)x.base;
				if (nt::ok(qvm()(h,nullptr,MemoryWorkingSetExInformation_,&e,sizeof(e),nullptr))) {
					x.valid = e.bits & 1;
					x.bad = (e.bits >> 31) & 1;
				}
				RegInfo ri = {};
				if (nt::ok(qvm()(h,(PVOID)x.base,MemoryRegionInformationEx_,&ri,sizeof(ri),nullptr))) x.flags = regionFlags(ri.RegionType);
			}
			v.push_back(std::move(x));
		}
	}

	std::wstring wide(const std::string& s) {
		int n = MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);
		std::wstring w(n > 0 ? n : 1,L'\0');
		if (n > 0) MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,&w[0],n);
		w.resize(wcslen(w.c_str()));
		return w;
	}

	std::string sectionType(ULONG a) {
		if (a & SEC_COMMIT_) return "Commit";
		if (a & SEC_FILE_) return "File";
		if (a & SEC_IMAGE_) return "Image";
		if (a & SEC_RESERVE_) return "Reserve";
		return "Unknown";
	}

}

namespace vmem {
	namespace be {

		void regions(const ProcRef& r,RegionList& out,const std::atomic<bool>* stop) {
			if (!qvm()) {
				out.err = "NtQueryVirtualMemory is missing";
				return;
			}
			bool full = true;
			HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,FALSE,r.pid);
			DWORD e1 = h ? 0 : GetLastError();
			if (!h) {
				full = false;
				h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,r.pid);
			}
			if (!h) {
				out.err = whyNot(r.pid,e1 ? e1 : GetLastError());
				return;
			}
			if (r.start && !same(h,r.start)) {
				CloseHandle(h);
				out.err = "the process has exited, its pid was reused";
				return;
			}
			out.prot = protOf(h);
			walk(h,out.rows,full,stop);
			if (out.rows.empty() && !(stop && *stop)) {
				out.err = out.prot.empty() ? "no region could be read, the process may be exiting or its rights are stripped" : protMsg(out.prot);
				CloseHandle(h);
				return;
			}
			if (full) {
				classify(h,out.rows,r.pid,stop);
				out.useOk = true;
				out.wsOk = wsWalk(h,out.rows,stop);
			} else {
				// the file name query only needs the limited handle
				auto devs = devMap();
				for (auto& x : out.rows) {
					if (stop && *stop) break;
					if ((x.type == TyMapped || x.type == TyImage) && x.base == x.allocBase) tagFile(h,x,devs);
				}
			}
			CloseHandle(h);
			if (!full) {
				if (!out.prot.empty()) out.note = "Protected process (" + out.prot + "). Base, size, type, protection and mapped file names only. Stacks, heaps, working set and contents need the signed kernel driver, FRS has none.";
				else out.note = "Limited access to this process, so Use shows mapped files only and the working set columns stay empty. Run FRS elevated for processes of other users.";
			} else if (!out.wsOk) out.note = "The working set walk failed, those columns show N/A.";
		}

		size_t read(const ProcRef& r,uint64_t addr,size_t n,uint8_t* out,uint8_t* map,std::string& err) {
			memset(out,0,n);
			uint64_t p0 = addr & ~(PG - 1);
			size_t pages = (size_t)((addr + n + PG - 1 - p0) >> 12);
			if (map) memset(map,0,pages);
			if (!n || addr + n < addr) return 0;
			Proc p(r,PROCESS_VM_READ,err);
			if (!p.h) return 0;
			static auto f = nt::fn<RWVM>("NtReadVirtualMemory");
			if (!f) {
				err = "NtReadVirtualMemory is missing";
				return 0;
			}
			size_t got = 0;
			uint64_t cur = addr,end = addr + n;
			while (cur < end) {
				MEMORY_BASIC_INFORMATION m;
				if (!nt::ok(qvm()(p.h,(PVOID)cur,0,&m,sizeof(m),nullptr)) || !m.RegionSize) break;
				uint64_t rEnd = (uint64_t)m.BaseAddress + m.RegionSize;
				uint64_t chunkEnd = std::min(rEnd,end);
				if (m.State == MEM_COMMIT && protRd(m.Protect) && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
					uint8_t kind = protWr(m.Protect) ? 2 : 1;
					// a page can still fail, go on after it so one hole does not hide the rest
					while (cur < chunkEnd) {
						SIZE_T k = 0;
						size_t want = (size_t)(chunkEnd - cur);
						nt::Status st = f(p.h,(PVOID)cur,out + (cur - addr),want,&k);
						if (nt::ok(st)) k = want;
						if (map && k) for (uint64_t q = cur & ~(PG - 1); q < cur + k; q += PG) map[(q - p0) >> 12] = kind;
						got += k;
						cur += k;
						if (cur < chunkEnd && !nt::ok(st)) cur = (cur + PG) & ~(PG - 1);
					}
				}
				cur = chunkEnd;
			}
			return got;
		}

		size_t write(const ProcRef& r,uint64_t addr,const uint8_t* data,size_t n,std::string& err) {
			Proc p(r,PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,err);
			if (!p.h) return 0;
			static auto f = nt::fn<RWVM>("NtWriteVirtualMemory");
			if (!f) {
				err = "NtWriteVirtualMemory is missing";
				return 0;
			}
			SIZE_T k = 0;
			nt::Status st = f(p.h,(PVOID)addr,(PVOID)data,n,&k);
			if (nt::ok(st)) return n;
			err = st == nt::PARTIAL_COPY || st == nt::ACCESS_VIOLATION ? "a page is not writable, change its protection first" : stText(st);
			return k;
		}

		bool regionAt(const ProcRef& r,uint64_t addr,Region& out,std::string& err) {
			Proc p(r,0,err);
			if (!p.h) return false;
			MEMORY_BASIC_INFORMATION m;
			nt::Status st = qvm()(p.h,(PVOID)addr,0,&m,sizeof(m),nullptr);
			if (!nt::ok(st)) {
				err = st == nt::INVALID_PARAMETER ? "that address is outside user space" : stText(st);
				return false;
			}
			fromMbi(m,out);
			if (m.State == MEM_FREE) out.allocBase = out.base;
			return true;
		}

		bool protect(const ProcRef& r,uint64_t addr,uint64_t& size,uint32_t prot,std::string& old,std::string& err) {
			Proc p(r,PROCESS_VM_OPERATION,err);
			if (!p.h) return false;
			static auto f = nt::fn<PVM>("NtProtectVirtualMemory");
			if (!f) {
				err = "NtProtectVirtualMemory is missing";
				return false;
			}
			PVOID b = (PVOID)addr;
			SIZE_T s = size;
			ULONG o = 0;
			nt::Status st = f(p.h,&b,&s,prot,&o);
			if (!nt::ok(st)) {
				err = stText(st);
				return false;
			}
			size = s;
			old = protStr(o);
			return true;
		}

		bool parseProt(const std::string& s,uint32_t& out) {
			static const struct { const char* n; uint32_t v; } names[] = {
				{"NA",PAGE_NOACCESS},{"R",PAGE_READONLY},{"RW",PAGE_READWRITE},{"WC",PAGE_WRITECOPY},
				{"X",PAGE_EXECUTE},{"RX",PAGE_EXECUTE_READ},{"RWX",PAGE_EXECUTE_READWRITE},{"WCX",PAGE_EXECUTE_WRITECOPY}
			};
			uint64_t v = 0;
			std::string base = s;
			uint32_t mods = 0;
			size_t plus;
			while ((plus = base.rfind('+')) != std::string::npos) {
				std::string m = base.substr(plus + 1);
				if (m == "G") mods |= PAGE_GUARD;
				else if (m == "NC") mods |= PAGE_NOCACHE;
				else if (m == "WCM") mods |= PAGE_WRITECOMBINE;
				else return false;
				base.resize(plus);
			}
			bool named = false;
			for (auto& e : names) if (base == e.n) {
				v = e.v;
				named = true;
			}
			if (!named) {
				if (base.size() < 3 || base[0] != '0' || (base[1] != 'x' && base[1] != 'X') || !util::parseHex(base,v) || v > 0x7FF) return false;
			}
			v |= mods;
			uint32_t b = v & 0xFF;
			if (!b || (b & (b - 1)) || (v & ~(uint64_t)0x7FF)) return false;
			if ((v & PAGE_GUARD) && b == PAGE_NOACCESS) return false;
			if ((v & PAGE_NOCACHE) && (v & PAGE_WRITECOMBINE)) return false;
			out = (uint32_t)v;
			return true;
		}

		const char* protHelp() {
			return "NA, R, RW, WC, X, RX, RWX, WCX with optional +G, +NC, +WCM, or a value like 0x04";
		}

		bool freeMem(const ProcRef& r,uint64_t addr,uint64_t size,bool decommit,std::string& did,std::string& err) {
			Proc p(r,PROCESS_VM_OPERATION,err);
			if (!p.h) return false;
			MEMORY_BASIC_INFORMATION m;
			nt::Status st = qvm()(p.h,(PVOID)addr,0,&m,sizeof(m),nullptr);
			if (!nt::ok(st)) {
				err = st == nt::INVALID_PARAMETER ? "that address is outside user space" : stText(st);
				return false;
			}
			if (m.State == MEM_FREE) {
				err = "that region is already free";
				return false;
			}
			if (m.Type != MEM_PRIVATE) {
				if (decommit) {
					err = "mapped and image memory can't be decommitted, free unmaps the view";
					return false;
				}
				if ((uint64_t)m.AllocationBase != addr) {
					err = "unmapping needs the base of the view, " + util::hex((uint64_t)m.AllocationBase);
					return false;
				}
				static auto u = nt::fn<UVS>("NtUnmapViewOfSection");
				if (!u) {
					err = "NtUnmapViewOfSection is missing";
					return false;
				}
				st = u(p.h,(PVOID)addr);
				if (!nt::ok(st)) {
					err = stText(st);
					return false;
				}
				did = "unmapped";
				return true;
			}
			if (!decommit && (uint64_t)m.AllocationBase != addr) {
				err = "free releases a whole allocation, it needs the allocation base " + util::hex((uint64_t)m.AllocationBase) + ". Decommit works on a part";
				return false;
			}
			static auto f = nt::fn<FVM>("NtFreeVirtualMemory");
			if (!f) {
				err = "NtFreeVirtualMemory is missing";
				return false;
			}
			// RegionSize counts from addr's page, the kernel rounds addr + size up and would take the next page too
			PVOID b = decommit && !size ? m.BaseAddress : (PVOID)addr;
			SIZE_T s = decommit ? (size ? size : m.RegionSize) : 0;
			st = f(p.h,&b,&s,decommit ? MEM_DECOMMIT : MEM_RELEASE);
			if (!nt::ok(st)) {
				err = stText(st);
				return false;
			}
			did = decommit ? "decommitted" : "released";
			return true;
		}

		bool emptyWs(const ProcRef& r,uint64_t addr,uint64_t size,std::string& note,std::string& err) {
			Proc p(r,PROCESS_VM_OPERATION,err);
			if (!p.h) return false;
			static auto f = nt::fn<ULVM>("NtUnlockVirtualMemory");
			if (!f) {
				err = "NtUnlockVirtualMemory is missing";
				return false;
			}
			PVOID b = (PVOID)addr;
			SIZE_T s = size;
			nt::Status st = f(p.h,&b,&s,MAP_PROCESS_);
			// unlocking pages that are not locked is the trim, so "not locked" is what success looks like
			if (st == NOT_LOCKED) return true;
			if (nt::ok(st)) {
				note = "the pages were locked in memory, they are unlocked now";
				return true;
			}
			err = stText(st);
			return false;
		}

		std::string critical(const ProcRef& r) {
			std::string err;
			Proc p(r,0,err);
			// can't open it : the call that wants it can't either and says why
			if (!p.h) return "";
			std::string pr = protOf(p.h);
			if (!pr.empty()) return protMsg(pr);
			static auto q = nt::fn<QIP>("NtQueryInformationProcess");
			ULONG bt = 0;
			if (!q || !nt::ok(q(p.h,ProcessBreakOnTermination_,&bt,sizeof(bt),nullptr))) return "can't tell whether it is a critical system process, refused";
			if (bt) return "critical system process, Windows stops with a bugcheck when it dies, FRS does not read or change its memory over MCP";
			return "";
		}

		bool canChange() {
			return true;
		}

		bool sections(const ProcRef& r,std::vector<Section>& out,std::string& err,const std::atomic<bool>* stop) {
			Proc p(r,PROCESS_DUP_HANDLE,err);
			if (!p.h) return false;
			static auto qs = nt::fn<QS>("NtQuerySection");
			static auto qo = nt::fn<QO>("NtQueryObject");
			static auto mv = nt::fn<MVS>("NtMapViewOfSection");
			static auto uv = nt::fn<UVS>("NtUnmapViewOfSection");
			if (!qs || !qo || !mv || !uv) {
				err = "NT section calls are missing";
				return false;
			}
			// the table has type numbers and only a live section of our own says which one means Section
			HANDLE mine = CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READONLY,0,4096,nullptr);
			if (!mine) {
				err = "could not create a test section, error " + std::to_string(GetLastError());
				return false;
			}
			std::vector<char> b;
			nt::Status st = nt::querySys(SystemExtendedHandleInformation_,b);
			if (!nt::ok(st) || b.size() < 16) {
				CloseHandle(mine);
				err = "could not read the handle table, " + stText(st);
				return false;
			}
			size_t n = *(ULONG_PTR*)b.data();
			auto* es = (const HEntry*)(b.data() + 16);
			DWORD self = GetCurrentProcessId();
			int typeNo = -1;
			for (size_t i = 0; i < n; i++) if (es[i].pid == self && (HANDLE)es[i].handle == mine) {
				typeNo = es[i].type;
				break;
			}
			CloseHandle(mine);
			if (typeNo < 0) {
				err = "could not find the Section type number";
				return false;
			}
			auto devs = devMap();
			for (size_t i = 0; i < n; i++) {
				if (stop && *stop) return false;
				if (es[i].pid != r.pid || es[i].type != typeNo) continue;
				Section s;
				s.handle = es[i].handle;
				HANDLE d = nullptr;
				bool mapRead = DuplicateHandle(p.h,(HANDLE)es[i].handle,GetCurrentProcess(),&d,SECTION_QUERY | SECTION_MAP_READ,FALSE,0);
				if (!mapRead && !DuplicateHandle(p.h,(HANDLE)es[i].handle,GetCurrentProcess(),&d,SECTION_QUERY,FALSE,0)) {
					s.type = "Unknown";
					s.best = "N/A, no access to this handle";
					out.push_back(std::move(s));
					continue;
				}
				SecBasic sb = {};
				if (nt::ok(qs(d,SectionBasicInformation_,&sb,sizeof(sb),nullptr))) {
					s.type = sectionType(sb.AllocationAttributes);
					s.size = (uint64_t)sb.MaximumSize.QuadPart;
				} else s.type = "Unknown";
				char nb[1024];
				ULONG ret = 0;
				if (nt::ok(qo(d,ObjectNameInformation_,nb,sizeof(nb),&ret))) {
					auto* u = (nt::USTR*)nb;
					if (u->Buffer && u->Length) s.name = utf8(u->Buffer,u->Length / 2);
				}
				if (mapRead) {
					// a page mapped for a moment, only to ask whose file this is. Nothing is read
					PVOID base = nullptr;
					SIZE_T vs = 4096;
					if (nt::ok(mv(d,GetCurrentProcess(),&base,0,0,nullptr,&vs,ViewUnmap_,0,PAGE_READONLY))) {
						std::wstring nm;
						if (mappedName(GetCurrentProcess(),(uint64_t)base,nm)) s.file = dosPath(nm,devs);
						uv(GetCurrentProcess(),base);
					}
				}
				CloseHandle(d);
				if (!s.name.empty()) s.best = s.name;
				else if (!s.file.empty()) s.best = s.file;
				else s.best = s.type + (s.size != NA ? " (" + util::size(s.size) + ")" : "");
				out.push_back(std::move(s));
			}
			return true;
		}

		bool findMapped(const std::string& needle,std::vector<MapUser>& out,int& unreadable,const std::atomic<bool>* stop) {
			auto l = procs::latest();
			auto devs = devMap();
			unreadable = 0;
			for (auto& pr : l->rows) {
				if (stop && *stop) return false;
				if (pr.pseudo || pr.pid == 0) continue;
				HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pr.pid);
				if (!h) {
					unreadable++;
					continue;
				}
				uint64_t a = 0;
				MapUser cur;
				for (;;) {
					MEMORY_BASIC_INFORMATION m;
					if (!nt::ok(qvm()(h,(PVOID)a,0,&m,sizeof(m),nullptr)) || !m.RegionSize) break;
					uint64_t end = (uint64_t)m.BaseAddress + m.RegionSize;
					a = end;
					if (m.State == MEM_FREE || (m.Type != MEM_MAPPED && m.Type != MEM_IMAGE)) continue;
					if (cur.lo && (uint64_t)m.AllocationBase == cur.lo) {
						cur.hi = end;
						continue;
					}
					if (cur.lo) out.push_back(cur);
					cur = MapUser();
					if ((uint64_t)m.AllocationBase != (uint64_t)m.BaseAddress) continue;
					std::wstring nm;
					if (!mappedName(h,(uint64_t)m.BaseAddress,nm)) continue;
					std::string path = dosPath(nm,devs);
					if (!util::has(path,needle)) continue;
					cur.pid = pr.pid;
					cur.start = pr.start;
					cur.name = pr.name;
					cur.path = path;
					cur.lo = (uint64_t)m.BaseAddress;
					cur.hi = end;
					cur.view = m.Type == MEM_IMAGE ? "Image" : "Mapped";
				}
				if (cur.lo) out.push_back(cur);
				CloseHandle(h);
			}
			return true;
		}

		bool mapSection(const ProcRef& owner,uint64_t handle,uint64_t& addr,uint64_t& size,bool& writable,std::string& err) {
			Proc p(owner,PROCESS_DUP_HANDLE,err);
			if (!p.h) return false;
			static auto qs = nt::fn<QS>("NtQuerySection");
			static auto mv = nt::fn<MVS>("NtMapViewOfSection");
			if (!qs || !mv) {
				err = "NT section calls are missing";
				return false;
			}
			HANDLE d = nullptr;
			writable = DuplicateHandle(p.h,(HANDLE)handle,GetCurrentProcess(),&d,SECTION_QUERY | SECTION_MAP_READ | SECTION_MAP_WRITE,FALSE,0);
			if (!writable && !DuplicateHandle(p.h,(HANDLE)handle,GetCurrentProcess(),&d,SECTION_QUERY | SECTION_MAP_READ,FALSE,0)) {
				err = "could not copy the handle, error " + std::to_string(GetLastError());
				return false;
			}
			PVOID base = nullptr;
			SIZE_T vs = 0;
			nt::Status st = mv(d,GetCurrentProcess(),&base,0,0,nullptr,&vs,ViewUnmap_,0,writable ? PAGE_READWRITE : PAGE_READONLY);
			if (st == SECTION_PROTECTION && writable) {
				writable = false;
				base = nullptr;
				vs = 0;
				st = mv(d,GetCurrentProcess(),&base,0,0,nullptr,&vs,ViewUnmap_,0,PAGE_READONLY);
			}
			CloseHandle(d);
			if (!nt::ok(st)) {
				err = stText(st);
				return false;
			}
			addr = (uint64_t)base;
			size = vs;
			return true;
		}

		void unmapSection(uint64_t addr) {
			static auto uv = nt::fn<UVS>("NtUnmapViewOfSection");
			if (uv) uv(GetCurrentProcess(),(PVOID)addr);
		}

		ProcRef self() {
			ProcRef r;
			r.pid = GetCurrentProcessId();
			r.name = "Section";
			return r;
		}

		void* fileOpen(const std::string& path,bool over,int& rc,std::string& err,bool) {
			rc = 0;
			HANDLE h = CreateFileW(wide(path).c_str(),GENERIC_WRITE,0,nullptr,over ? CREATE_ALWAYS : CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
			if (h != INVALID_HANDLE_VALUE) return h;
			DWORD e = GetLastError();
			if (e == ERROR_FILE_EXISTS) {
				rc = 1;
				return nullptr;
			}
			rc = -1;
			err = e == ERROR_PATH_NOT_FOUND ? "the folder does not exist" : e == ERROR_ACCESS_DENIED ? "access denied" : "error " + std::to_string(e);
			return nullptr;
		}

		bool fileWrite(void* f,const void* data,size_t n) {
			DWORD wr = 0;
			return WriteFile((HANDLE)f,data,(DWORD)n,&wr,nullptr) && wr == n;
		}

		void fileClose(void* f) {
			CloseHandle((HANDLE)f);
		}

		std::string home() {
			for (const wchar_t* k : {L"USERPROFILE",L"HOME"}) {
				const wchar_t* h = _wgetenv(k);
				if (h && *h) return utf8(h,wcslen(h));
			}
			return "";
		}

	}
}
#endif // _WIN32
