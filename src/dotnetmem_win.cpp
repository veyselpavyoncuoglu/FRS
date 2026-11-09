#ifdef _WIN32
#include "dotnetmem.h"
#include "nt.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

	// CLR block layouts from SI plugins/DotNetTools/clr (MIT). x64 and WOW64 offsets printed from those headers and asserted.
	const USHORT VER_IPC_BLOCK = 4;

	struct PerfGc {
		size_t genCollections[3];
		size_t promoted[2];
		size_t promotedFinal,pid;
		size_t genSize[3];
		size_t committed,reserved,loh,survivors,handles,alloc,allocLoh,induced;
		ULONG timeInGc,timeInGcBase;
		size_t pinned,sink;
	};

#pragma pack(push,4)
	struct PerfGc32 {
		ULONG genCollections[3];
		ULONG promoted[2];
		ULONG promotedFinal,pid;
		ULONG genSize[3];
		ULONG committed,reserved,loh,survivors,handles,alloc,allocLoh,induced;
		ULONG timeInGc,timeInGcBase;
		ULONG pinned,sink;
	};
#pragma pack(pop)

#ifdef _WIN64
	static_assert(sizeof(PerfGc) == 168 && offsetof(PerfGc,genSize) == 56 && offsetof(PerfGc,pinned) == 152,"PerfGc");
	static_assert(sizeof(PerfGc32) == 88 && offsetof(PerfGc32,pid) == 24 && offsetof(PerfGc32,sink) == 84,"PerfGc32");
#endif

	// from the start of the view, for a 64 bit FRS : Perf_GC and the loader heap size, native then WOW64
	const size_t V4_GC = 48,V4_LOADER = 304,V4_GC32 = 44,V4_LOADER32 = 216;
	const size_t V2_GC = 40,V2_LOADER = 296,V2_GC32 = 32,V2_LOADER32 = 204;
	const size_t VER_AT_V4 = 16,VER_AT_V2 = 0;

	struct View {
		HANDLE sec = nullptr;
		const char* p = nullptr;
		size_t size = 0;
		~View() {
			reset();
		}
		void reset() {
			if (p) UnmapViewOfFile(p);
			if (sec) CloseHandle(sec);
			p = nullptr;
			sec = nullptr;
			size = 0;
		}
		bool map() {
			if (!sec) return false;
			p = (const char*)MapViewOfFile(sec,FILE_MAP_READ,0,0,0);
			if (!p) return false;
			MEMORY_BASIC_INFORMATION mi;
			size = VirtualQuery(p,&mi,sizeof(mi)) ? mi.RegionSize : 0;
			return size >= 2048;
		}
	};

	PSID appContainerSid(uint32_t pid,std::vector<char>& buf) {
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
		if (!h) return nullptr;
		HANDLE tok = nullptr;
		PSID out = nullptr;
		if (OpenProcessToken(h,TOKEN_QUERY,&tok)) {
			buf.resize(256);
			DWORD ret = 0;
			if (GetTokenInformation(tok,(TOKEN_INFORMATION_CLASS)31,buf.data(),(DWORD)buf.size(),&ret)) out = *(PSID*)buf.data(); // TokenAppContainerSid
			CloseHandle(tok);
		}
		CloseHandle(h);
		return out;
	}

	struct ObjAttr {
		ULONG Length;
		HANDLE RootDirectory;
		nt::USTR* ObjectName;
		ULONG Attributes;
		PVOID SecurityDescriptor,SecurityQualityOfService;
	};

	const ULONG OBJ_NOCASE = 0x40;

	nt::USTR ustr(const wchar_t* s) {
		USHORT n = (USHORT)(wcslen(s) * sizeof(wchar_t));
		return nt::USTR{n,n,const_cast<PWSTR>(s)};
	}

	using OpenSecFn = nt::Status(NTAPI*)(PHANDLE,ACCESS_MASK,ObjAttr*);
	using CreateBdFn = PVOID(NTAPI*)(nt::USTR*,ULONG);
	using AddSidFn = nt::Status(NTAPI*)(PVOID*,PSID);
	using DelBdFn = VOID(NTAPI*)(PVOID);
	using OpenNsFn = nt::Status(NTAPI*)(PHANDLE,ACCESS_MASK,ObjAttr*,PVOID);

	nt::Status openSection(const wchar_t* name,HANDLE root,ACCESS_MASK acc,HANDLE& out) {
		static auto f = nt::fn<OpenSecFn>("NtOpenSection");
		if (!f) return nt::NOT_IMPLEMENTED;
		nt::USTR u = ustr(name);
		ObjAttr oa{sizeof(ObjAttr),root,&u,OBJ_NOCASE,nullptr,nullptr};
		return f(&out,acc,&oa);
	}

	// CLR v4 lives in a private namespace bound to the Everyone SID, plus the AppContainer for store apps.
	// The namespace handle is the root of the open, the Win32 alias would mix up two targets read at once.
	bool openV4(uint32_t pid,View& v) {
		static auto mkBd = nt::fn<CreateBdFn>("RtlCreateBoundaryDescriptor");
		static auto addSid = nt::fn<AddSidFn>("RtlAddSIDToBoundaryDescriptor");
		static auto delBd = nt::fn<DelBdFn>("RtlDeleteBoundaryDescriptor");
		static auto openNs = nt::fn<OpenNsFn>("NtOpenPrivateNamespace");
		if (!mkBd || !addSid || !delBd || !openNs) return false;
		SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
		PSID everyone = nullptr;
		if (!AllocateAndInitializeSid(&world,1,SECURITY_WORLD_RID,0,0,0,0,0,0,0,&everyone)) return false;
		std::wstring name = L"Cor_CLR_IPCBlock_" + std::to_wstring(pid);
		nt::USTR un = ustr(name.c_str());
		nt::USTR up = ustr(L"Cor_CLR_READER");
		std::vector<char> acBuf;
		PSID ac = nullptr;
		bool ok = false;
		for (int pass = 0; pass < 2 && !ok; pass++) {
			if (pass == 1 && !(ac = appContainerSid(pid,acBuf))) break;
			PVOID b = mkBd(&un,0);
			if (!b) break;
			if (nt::ok(addSid(&b,everyone)) && (!ac || nt::ok(addSid(&b,ac)))) {
				ObjAttr oa{sizeof(ObjAttr),(HANDLE)b,&up,OBJ_NOCASE,nullptr,nullptr};
				HANDLE ns = nullptr;
				if (nt::ok(openNs(&ns,MAXIMUM_ALLOWED,&oa,b))) {
					HANDLE h = nullptr;
					if (nt::ok(openSection(L"Cor_SxSPublic_IPCBlock",ns,SECTION_MAP_READ,h))) {
						v.reset();
						v.sec = h;
						ok = v.map();
					}
					CloseHandle(ns);
				}
			}
			delBd(b);
		}
		FreeSid(everyone);
		return ok;
	}

	// A failed open is a plain false here, upstream's NT_SUCCESS around a BOOLEAN let it through and read a null map
	bool openV2(uint32_t pid,View& v) {
		std::wstring name = L"\\BaseNamedObjects\\Cor_Public_IPCBlock_" + std::to_wstring(pid);
		HANDLE h = nullptr;
		if (!nt::ok(openSection(name.c_str(),nullptr,SECTION_MAP_READ,h))) return false;
		v.sec = h;
		return v.map();
	}

	// section check needs no process handle. Access denied still means it exists.
	bool sectionThere(const std::wstring& name) {
		HANDLE h = nullptr;
		nt::Status st = openSection(name.c_str(),nullptr,SECTION_QUERY,h);
		if (nt::ok(st)) CloseHandle(h);
		return nt::ok(st) || st == nt::ACCESS_DENIED;
	}

	// listing pipes never connects to one. "...-1234-key-socket" on .NET 5+, plain "...-1234" on Core 3
	bool corePipe(uint32_t pid) {
		std::wstring pre = L"dotnet-diagnostic-" + std::to_wstring(pid);
		std::wstring pat = L"\\\\.\\pipe\\" + pre + L"*";
		WIN32_FIND_DATAW fd;
		HANDLE f = FindFirstFileW(pat.c_str(),&fd);
		if (f == INVALID_HANDLE_VALUE) return false;
		bool hit = false;
		do {
			wchar_t c = fd.cFileName[pre.size()];
			hit = c == 0 || c == L'-';
		} while (!hit && FindNextFileW(f,&fd));
		FindClose(f);
		return hit;
	}

	// 1 read, 0 not this layout, -1 no GC yet : the CLR writes its pid with the GC numbers at the end of each GC
	template<class G,class L> int fill(ClrMem& m,const char* base,size_t size,size_t gcOff,size_t loaderOff,uint32_t pid) {
		if (gcOff + sizeof(G) > size || loaderOff + sizeof(L) > size) return 0;
		G g;
		memcpy(&g,base + gcOff,sizeof(g));
		if ((uint32_t)g.pid != pid) return !g.pid && !g.genCollections[0] && !g.genCollections[1] && !g.genCollections[2] ? -1 : 0;
		for (int i = 0; i < 3; i++) {
			m.genCount[i] = g.genCollections[i];
			m.genSize[i] = g.genSize[i];
		}
		m.promoted[0] = g.promoted[0];
		m.promoted[1] = g.promoted[1];
		m.promotedFinal = g.promotedFinal;
		m.committed = g.committed;
		m.reserved = g.reserved;
		m.loh = g.loh;
		m.survivors = g.survivors;
		m.handles = g.handles;
		m.alloc = g.alloc;
		m.allocLoh = g.allocLoh;
		m.induced = g.induced;
		m.pinned = g.pinned;
		m.sink = g.sink;
		L lh;
		memcpy(&lh,base + loaderOff,sizeof(lh));
		m.loader = (uint64_t)lh;
		return 1;
	}

	int fillAs(ClrMem& m,const View& v,uint32_t pid,bool isV4,bool wow64) {
		if (wow64) return fill<PerfGc32,ULONG>(m,v.p,v.size,isV4 ? V4_GC32 : V2_GC32,isV4 ? V4_LOADER32 : V2_LOADER32,pid);
		return fill<PerfGc,size_t>(m,v.p,v.size,isV4 ? V4_GC : V2_GC,isV4 ? V4_LOADER : V2_LOADER,pid);
	}

	// bitness decides the layout, an unknown one tries both. The pid stored in the block backs it up, so garbage never shows as numbers.
	bool readBlock(ClrMem& m,const View& v,uint32_t pid,bool isV4,bool wow64,bool knownBits) {
		USHORT ver = 0;
		memcpy(&ver,v.p + (isV4 ? VER_AT_V4 : VER_AT_V2),sizeof(ver));
		if (ver > VER_IPC_BLOCK) {
			m.why = "This CLR block version is newer than FRS knows.";
			return false;
		}
		int ok = fillAs(m,v,pid,isV4,wow64);
		if (!ok && !knownBits) {
			wow64 = !wow64;
			ok = fillAs(m,v,pid,isV4,wow64);
		}
		if (ok <= 0) {
			m.why = ok < 0 ? "No garbage collection has run in this process yet, the numbers appear after the first one." : "FRS can't read this CLR's counters block, it does not hold this process's id.";
			return false;
		}
		m.why.clear();
		m.wow64 = wow64;
		return true;
	}

	bool isWow64(const ProcRef& r,bool& out) {
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		if (row && row->bits) {
			out = row->bits == 32;
			return true;
		}
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,r.pid);
		if (!h) return false;
		BOOL w = FALSE;
		bool ok = IsWow64Process(h,&w);
		CloseHandle(h);
		out = w != FALSE;
		return ok;
	}

	// asked after the read : still running here means the read saw the target, not a dead pid or a reused one
	bool exited(const ProcRef& r) {
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,r.pid);
		if (!h) return GetLastError() == ERROR_INVALID_PARAMETER;
		DWORD code = 0;
		FILETIME c,x,k,u;
		bool gone = GetExitCodeProcess(h,&code) && code != STILL_ACTIVE;
		if (!gone && r.start && GetProcessTimes(h,&c,&x,&k,&u)) gone = (((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime) != r.start;
		CloseHandle(h);
		return gone;
	}

	void readClr(const ProcRef& r,ClrMem& m) {
		bool w64 = false;
		bool knownBits = isWow64(r,w64);
		std::wstring pid = std::to_wstring(r.pid);
		{
			View v;
			if (openV4(r.pid,v)) {
				m.kind = ClrMem::V4;
				m.block = readBlock(m,v,r.pid,true,w64,knownBits);
				return;
			}
		}
		{
			View v;
			if (openV2(r.pid,v)) {
				m.kind = ClrMem::V2;
				m.block = readBlock(m,v,r.pid,false,w64,knownBits);
				return;
			}
		}
		if (sectionThere(L"\\BaseNamedObjects\\Cor_Private_IPCBlock_v4_" + pid)) {
			m.kind = ClrMem::V4;
			m.why = "This is a CLR v4 process but its public block could not be mapped, it may still be starting or be too restricted.";
			return;
		}
		if (sectionThere(L"\\BaseNamedObjects\\Cor_Private_IPCBlock_" + pid)) {
			m.kind = ClrMem::V2;
			m.why = "This is a CLR v2 process but its public block could not be mapped.";
			return;
		}
		if (corePipe(r.pid)) {
			m.kind = ClrMem::Core;
			m.why = ".NET 5+ or .NET Core process. CoreCLR publishes no memory block, its GC numbers are only on the diagnostics pipe, which FRS does not use.";
			return;
		}
		m.kind = ClrMem::None;
		m.why = "No CLR v2 or v4 block found, this is not a .NET Framework process.";
	}

}

namespace dotnetmem { namespace be {

	void read(const ProcRef& r,ClrMem& m) {
		readClr(r,m);
		if (!exited(r)) return;
		m = ClrMem();
		m.kind = ClrMem::Gone;
		m.why = "The process exited before this was read.";
	}

} }
#endif
