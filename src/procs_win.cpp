#ifdef _WIN32
#include "procs.h"
#include "nt.h"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>

namespace {

	typedef nt::Status(NTAPI* QIP)(HANDLE,ULONG,PVOID,ULONG,PULONG);
	typedef nt::Status(NTAPI* SIP)(HANDLE,ULONG,PVOID,ULONG);
	typedef nt::Status(NTAPI* QVM)(HANDLE,PVOID,ULONG,PVOID,SIZE_T,PSIZE_T);
	typedef nt::Status(NTAPI* QO)(HANDLE,ULONG,PVOID,ULONG,PULONG);
	typedef nt::Status(NTAPI* QSI)(ULONG,PVOID,ULONG,PULONG);

	const ULONG SystemProcessInformation_ = 5;
	const ULONG SystemProcessIdInformation_ = 88;
	const ULONG ProcessQuotaLimits_ = 1;
	const ULONG ProcessVmCounters_ = 3;
	const ULONG ProcessWow64Information_ = 26;
	const ULONG ProcessPagePriority_ = 39;
	const ULONG MemoryWorkingSetInformation_ = 1;
	const ULONG ObjectBasicInformation_ = 0;

	// phnt ntexapi.h, PROCESS_ENERGY_VALUES
	struct Energy {
		ULONGLONG Cycles[4][2];
		ULONGLONG DiskEnergy,NetworkTailEnergy,MBBTailEnergy,NetworkTxRxBytes,MBBTxRxBytes;
		ULONGLONG Durations[3];
		ULONG CompositionRendered,CompositionDirtyGenerated,CompositionDirtyPropagated,Reserved1;
		ULONGLONG AttributedCycles[4][2];
		ULONGLONG WorkOnBehalfCycles[4][2];
	};

	// phnt ntexapi.h, SYSTEM_PROCESS_INFORMATION_EXTENSION, follows Threads[NumberOfThreads]
	struct SPIX {
		ULONGLONG DiskCounters[5];
		ULONGLONG ContextSwitches;
		ULONG Flags; // bit 0 HasStrongId, 1..4 Classification
		ULONG UserSidOffset,PackageFullNameOffset;
		Energy EnergyValues;
		ULONG AppIdOffset;
		SIZE_T SharedCommitCharge;
		ULONG JobObjectId,SpareUlong;
		ULONGLONG ProcessSequenceNumber;
	};

	// phnt ntpsapi.h
	struct VMC2 {
		SIZE_T PeakVirtualSize,VirtualSize;
		ULONG PageFaultCount;
		SIZE_T PeakWorkingSetSize,WorkingSetSize;
		SIZE_T QuotaPeakPagedPoolUsage,QuotaPagedPoolUsage,QuotaPeakNonPagedPoolUsage,QuotaNonPagedPoolUsage;
		SIZE_T PagefileUsage,PeakPagefileUsage,PrivateUsage;
		SIZE_T PrivateWorkingSetSize;
		ULONGLONG SharedCommitUsage;
	};

	// phnt ntobapi.h
	struct OBI {
		ULONG Attributes;
		ACCESS_MASK GrantedAccess;
		ULONG HandleCount,PointerCount,PagedPoolCharge,NonPagedPoolCharge;
		ULONG Reserved[3];
		ULONG NameInfoSize,TypeInfoSize,SecurityDescriptorSize;
		LARGE_INTEGER CreationTime;
	};

	// phnt ntexapi.h, SYSTEM_PROCESS_ID_INFORMATION
	struct SPID {
		HANDLE ProcessId;
		nt::USTR ImageName;
	};

#ifdef _WIN64
	static_assert(sizeof(SPID) == 0x18,"SPID");
	static_assert(sizeof(Energy) == 0x110,"Energy");
	static_assert(offsetof(SPIX,Flags) == 0x30,"SPIX");
	static_assert(offsetof(SPIX,EnergyValues) == 0x40,"SPIX");
	static_assert(offsetof(SPIX,SharedCommitCharge) == 0x158,"SPIX");
	static_assert(offsetof(SPIX,JobObjectId) == 0x160,"SPIX");
	static_assert(sizeof(SPIX) == 0x170,"SPIX");
	static_assert(sizeof(VMC2) == 0x70,"VMC2");
	static_assert(sizeof(OBI) == 0x38,"OBI");
	static_assert(sizeof(QUOTA_LIMITS_EX) == 0x58,"QUOTA");
#endif

	std::string sidName(PSID sid) {
		wchar_t nm[256],dm[256];
		DWORD a = 256,b = 256;
		SID_NAME_USE use;
		if (!LookupAccountSidW(nullptr,sid,nm,&a,dm,&b,&use)) return "";
		std::string d = nt::utf8(dm,(int)wcslen(dm));
		std::string n = nt::utf8(nm,(int)wcslen(nm));
		return util::safe(d.empty() ? n : d + "\\" + n);
	}

	// worker thread only, an account lookup can take long and the answers never change
	std::string userOf(const std::string& raw) {
		static std::map<std::string,std::string> cache;
		auto it = cache.find(raw);
		if (it != cache.end()) return it->second;
		std::string n = sidName((PSID)raw.data());
		cache[raw] = n;
		return n;
	}

	// by SID, account names are localized : NT AUTHORITY (S-1-5-1 to 20, SYSTEM is 18), Window Manager (90), Font Driver Host (96)
	bool svcSid(PSID sid) {
		SID_IDENTIFIER_AUTHORITY nta = SECURITY_NT_AUTHORITY;
		if (memcmp(GetSidIdentifierAuthority(sid),&nta,sizeof(nta)) != 0 || !*GetSidSubAuthorityCount(sid)) return false;
		DWORD r = *GetSidSubAuthority(sid,0);
		return r <= 20 || r == 90 || r == 96;
	}

	std::string tokenUser(HANDLE h,bool& svc) {
		HANDLE t = nullptr;
		if (!OpenProcessToken(h,TOKEN_QUERY,&t)) return "";
		char b[256];
		DWORD n = 0;
		std::string out;
		if (GetTokenInformation(t,TokenUser,b,sizeof(b),&n)) {
			PSID sid = ((TOKEN_USER*)b)->User.Sid;
			svc = svcSid(sid);
			out = userOf(std::string((const char*)sid,GetLengthSid(sid)));
		}
		CloseHandle(t);
		return out;
	}

	// \Device\HarddiskVolume3\Windows\System32\x.exe, needs no handle, so it works on what FRS can't open
	std::string ntImage(uint32_t pid) {
		static auto qsi = nt::fn<QSI>("NtQuerySystemInformation");
		static wchar_t b[32767]; // info job only, the longest a UNICODE_STRING holds, so no length mismatch
		SPID x = {(HANDLE)(ULONG_PTR)pid,{0,(USHORT)sizeof(b),b}};
		if (!qsi || !nt::ok(qsi(SystemProcessIdInformation_,&x,sizeof(x),nullptr))) return "";
		return nt::utf8(b,x.ImageName.Length / 2);
	}

	bool isOs(const procs::Info& i,const ProcRow& r) {
		if (r.sys || r.pseudo || (i.noHandle && r.session == 0)) return true;
		static const std::string win = [] {
			wchar_t b[MAX_PATH];
			UINT n = GetWindowsDirectoryW(b,MAX_PATH);
			return util::lower(nt::utf8(b,(int)n)) + "\\";
		}();
		if (!i.path.empty()) return util::lower(i.path).rfind(win,0) == 0;
		// unelevated, winlogon, csrss, dwm and fontdrvhost of the user's session can't be opened
		static const std::string ntWin = [] {
			wchar_t b[MAX_PATH],d[MAX_PATH];
			UINT n = GetWindowsDirectoryW(b,MAX_PATH);
			if (n < 3 || n >= MAX_PATH || b[1] != L':') return std::string();
			wchar_t drv[3] = {b[0],L':',0};
			if (!QueryDosDeviceW(drv,d,MAX_PATH)) return std::string();
			return util::lower(nt::utf8(d,(int)wcslen(d)) + nt::utf8(b + 2,(int)n - 2)) + "\\";
		}();
		return !ntWin.empty() && util::lower(ntImage(r.pid)).rfind(ntWin,0) == 0;
	}

	std::string winErr(DWORD e) {
		if (e == ERROR_ACCESS_DENIED) return "access denied, protected or needs admin";
		if (e == ERROR_INVALID_PARAMETER) return "process exited";
		return "error " + std::to_string(e);
	}

	std::vector<char> g_wsBuf; // procs-ws job only

	std::string ntErr(nt::Status st) {
		if (st == nt::ACCESS_DENIED) return "access denied, protected or needs admin";
		if (st == nt::PRIVILEGE_NOT_HELD) return "needs a privilege FRS does not hold, run it as admin";
		if (st == nt::PROCESS_IS_TERMINATING) return "process is exiting";
		if (st == nt::INVALID_PARAMETER) return "Windows rejected the value";
		char b[32];
		snprintf(b,sizeof(b),"NTSTATUS 0x%08lX",(unsigned long)st);
		return b;
	}

}

namespace procs { namespace be {

	bool snapshot(std::vector<ProcRow>& out,std::string& err) {
		static std::vector<char> buf;
		nt::Status st = nt::querySys(SystemProcessInformation_,buf);
		if (!nt::ok(st)) {
			err = ntErr(st);
			return false;
		}
		size_t base = 0;
		const size_t hdr = offsetof(nt::SPI,Threads);
		while (base + hdr <= buf.size()) {
			const nt::SPI* p = (const nt::SPI*)(buf.data() + base);
			size_t room = p->NextEntryOffset ? (size_t)p->NextEntryOffset : buf.size() - base;
			ProcRow r;
			r.pid = (uint32_t)(ULONG_PTR)p->UniqueProcessId;
			r.ppid = (uint32_t)(ULONG_PTR)p->InheritedFromUniqueProcessId;
			r.session = p->SessionId;
			r.start = (uint64_t)p->CreateTime.QuadPart;
			r.priv = p->PagefileUsage;
			r.privPeak = p->PeakPagefileUsage;
			r.ws = p->WorkingSetSize;
			r.wsPeak = p->PeakWorkingSetSize;
			r.wsPriv = p->WorkingSetPrivateSize;
			r.virt = p->VirtualSize;
			r.virtPeak = p->PeakVirtualSize;
			r.paged = p->QuotaPagedPoolUsage;
			r.pagedPeak = p->QuotaPeakPagedPoolUsage;
			r.nonPaged = p->QuotaNonPagedPoolUsage;
			r.nonPagedPeak = p->QuotaPeakNonPagedPoolUsage;
			r.faults = p->PageFaultCount;
			r.hardFaults = p->HardFaultCount;
			r.threads = (int)p->NumberOfThreads;
			if (p->ImageName.Buffer && p->ImageName.Length) r.name = util::safe(nt::utf8(p->ImageName.Buffer,p->ImageName.Length / 2));
			if (r.pid == 0) {
				r.name = "System Idle Process";
				r.pseudo = true;
			}
			if (r.pid == 4) r.sys = true;
			if (r.name.empty()) r.name = "pid " + std::to_string(r.pid);

			size_t ext = hdr + sizeof(nt::STI) * (size_t)p->NumberOfThreads;
			// the image name follows the extension, older builds have a shorter one and its bytes are not fields
			uintptr_t s = (uintptr_t)p,nm = (uintptr_t)p->ImageName.Buffer;
			if (nm >= s + ext && nm < s + room) room = nm - s;
			if (ext + offsetof(SPIX,UserSidOffset) <= room) {
				SPIX x = {};
				memcpy(&x,(const char*)p + ext,std::min(sizeof(x),room - ext));
				int cls = (int)((x.Flags >> 1) & 0xF);
				if (cls) r.sys = true;
				r.memCompression = cls == 3;
				r.registry = cls == 4;
				if (ext + offsetof(SPIX,JobObjectId) <= room) r.sharedCommit = x.SharedCommitCharge;
				if (ext + offsetof(SPIX,SpareUlong) <= room) r.jobId = x.JobObjectId;
			}
			out.push_back(std::move(r));
			if (!p->NextEntryOffset) break;
			base += p->NextEntryOffset;
		}
		if (out.empty()) err = "empty process list";
		return !out.empty();
	}

	void info(const ProcRow& r,Info& i) {
		if (r.pid == 0) {
			i.user = "NT AUTHORITY\\SYSTEM";
			i.osProc = true;
			i.statics = true;
			return;
		}
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,r.pid);
		if (!h) {
			i.noHandle = true;
			i.noFull = true;
			i.osProc = isOs(i,r);
			i.statics = true;
			return;
		}
		if (!nt::sameStart(h,r.start)) {
			CloseHandle(h);
			return;
		}
		i.noHandle = false;
		static auto qip = nt::fn<QIP>("NtQueryInformationProcess");
		if (!i.statics) {
			wchar_t w[1024];
			DWORD n = 1024;
			if (QueryFullProcessImageNameW(h,0,w,&n)) i.path = util::safe(nt::utf8(w,(int)n));
			ULONG_PTR wow = 0;
			if (qip && nt::ok(qip(h,ProcessWow64Information_,&wow,sizeof(wow),nullptr))) i.bits = wow ? 32 : sizeof(void*) * 8;
			bool svc = false;
			i.user = tokenUser(h,svc);
			static auto qo = nt::fn<QO>("NtQueryObject");
			HANDLE q = OpenProcess(PROCESS_QUERY_INFORMATION,FALSE,r.pid);
			i.noFull = !q;
			if (q) {
				OBI o;
				if (qo && nt::ok(qo(q,ObjectBasicInformation_,&o,sizeof(o),nullptr))) i.filtered = !(o.GrantedAccess & PROCESS_QUERY_INFORMATION);
				CloseHandle(q);
			}
			i.osProc = svc || isOs(i,r);
			i.statics = true;
		}
		if (qip) {
			ULONG pp = 0;
			i.pagePrio = nt::ok(qip(h,ProcessPagePriority_,&pp,sizeof(pp),nullptr)) ? (int)pp : -1;
			QUOTA_LIMITS q;
			bool got = nt::ok(qip(h,ProcessQuotaLimits_,&q,sizeof(q),nullptr));
			i.minWs = got ? q.MinimumWorkingSetSize : NA;
			i.maxWs = got ? q.MaximumWorkingSetSize : NA;
			if (r.sharedCommit == NA) {
				VMC2 c;
				if (nt::ok(qip(h,ProcessVmCounters_,&c,sizeof(c),nullptr))) i.sharedCommit = c.SharedCommitUsage;
			}
		}
		CloseHandle(h);
	}

	bool wsCounters(const ProcRow& r,uint64_t& shared,uint64_t& shareable) {
		static auto qvm = nt::fn<QVM>("NtQueryVirtualMemory");
		if (!qvm) return false;
		HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION,FALSE,r.pid);
		if (!h) return false;
		if (!nt::sameStart(h,r.start)) {
			CloseHandle(h);
			return false;
		}
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		uint64_t pg = si.dwPageSize;
		std::vector<char>& buf = g_wsBuf;
		const size_t head = sizeof(ULONG_PTR);
		const size_t mx = (size_t)1 << 30;
		buf.resize(std::max(buf.size(),std::min(mx,head + (size_t)((r.ws == NA ? 0 : r.ws) / pg + 4096) * sizeof(ULONG_PTR))));
		nt::Status st = nt::UNSUCCESSFUL;
		for (int i = 0; i < 8; i++) {
			st = qvm(h,nullptr,MemoryWorkingSetInformation_,buf.data(),buf.size(),nullptr);
			if (st != nt::INFO_LENGTH_MISMATCH && st != nt::BUFFER_OVERFLOW) break;
			size_t n = head + (size_t)(*(ULONG_PTR*)buf.data() + 4096) * sizeof(ULONG_PTR);
			if (n > mx) break;
			buf.resize(std::max(n,buf.size() + buf.size() / 2));
		}
		CloseHandle(h);
		bool ok = nt::ok(st);
		if (ok) {
			ULONG_PTR n = *(ULONG_PTR*)buf.data();
			const ULONG_PTR* b = (const ULONG_PTR*)(buf.data() + head);
			n = std::min(n,(ULONG_PTR)((buf.size() - head) / sizeof(ULONG_PTR)));
			uint64_t sh = 0,sa = 0;
			// Protection : 5, ShareCount : 3, Shared : 1
			for (ULONG_PTR i = 0; i < n; i++) {
				if (((b[i] >> 5) & 7) > 1) sh++;
				if ((b[i] >> 8) & 1) sa++;
			}
			shared = sh * pg;
			shareable = sa * pg;
		}
		if (buf.size() > ((size_t)64 << 20)) wsFree();
		return ok;
	}

	void wsFree() {
		std::vector<char>().swap(g_wsBuf);
	}

	std::string reduceWs(const ProcRow& r) {
		static auto sip = nt::fn<SIP>("NtSetInformationProcess");
		if (!sip) return "NtSetInformationProcess missing";
		HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION,FALSE,r.pid);
		if (!h) return winErr(GetLastError());
		std::string why;
		if (!nt::sameStart(h,r.start)) why = "process exited";
		else {
			QUOTA_LIMITS_EX q = {};
			q.MinimumWorkingSetSize = (SIZE_T)-1;
			q.MaximumWorkingSetSize = (SIZE_T)-1;
			nt::Status st = sip(h,ProcessQuotaLimits_,&q,sizeof(q));
			if (!nt::ok(st)) why = ntErr(st);
		}
		CloseHandle(h);
		return why;
	}

	std::string setPagePrio(const ProcRow& r,int prio,int* prev) {
		static auto qip = nt::fn<QIP>("NtQueryInformationProcess");
		static auto sip = nt::fn<SIP>("NtSetInformationProcess");
		if (!qip || !sip) return "NtSetInformationProcess missing";
		HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,FALSE,r.pid);
		if (!h) return winErr(GetLastError());
		std::string why;
		if (!nt::sameStart(h,r.start)) why = "process exited";
		else {
			ULONG old = 0;
			if (prev && nt::ok(qip(h,ProcessPagePriority_,&old,sizeof(old),nullptr))) *prev = (int)old;
			ULONG v = (ULONG)prio;
			nt::Status st = sip(h,ProcessPagePriority_,&v,sizeof(v));
			if (!nt::ok(st)) why = ntErr(st);
		}
		CloseHandle(h);
		return why;
	}

	uint32_t selfPid() {
		return GetCurrentProcessId();
	}

	bool invoker(std::string&,std::string&) {
		return false;
	}

	int writeFile(const std::string& path,const std::string& data,bool overwrite,std::string& err) {
		int n = MultiByteToWideChar(CP_UTF8,0,path.c_str(),-1,nullptr,0);
		std::wstring w(n > 0 ? n : 1,L'\0');
		if (n > 0) MultiByteToWideChar(CP_UTF8,0,path.c_str(),-1,&w[0],n);
		HANDLE h = CreateFileW(w.c_str(),GENERIC_WRITE,0,nullptr,overwrite ? CREATE_ALWAYS : CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			DWORD e = GetLastError();
			if (e == ERROR_FILE_EXISTS) return 1;
			err = e == ERROR_PATH_NOT_FOUND ? "folder does not exist" : e == ERROR_ACCESS_DENIED ? "access denied" : "error " + std::to_string(e);
			return -1;
		}
		DWORD wr = 0;
		bool ok = WriteFile(h,data.data(),(DWORD)data.size(),&wr,nullptr) && wr == data.size();
		DWORD e = GetLastError();
		CloseHandle(h);
		if (!ok) {
			err = "error " + std::to_string(e);
			return -1;
		}
		return 0;
	}

} }
#endif // _WIN32
