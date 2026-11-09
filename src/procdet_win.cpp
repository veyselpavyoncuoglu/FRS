#ifdef _WIN32
#include "procdet.h"
#include "nt.h"
#include "ram.h"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <mutex>

namespace {

	using namespace procdet;

	typedef nt::Status(NTAPI* QIP)(HANDLE,ULONG,PVOID,ULONG,PULONG);
	typedef nt::Status(NTAPI* SIP)(HANDLE,ULONG,PVOID,ULONG);
	typedef nt::Status(NTAPI* QIT)(HANDLE,ULONG,PVOID,ULONG,PULONG);
	typedef nt::Status(NTAPI* SIT)(HANDLE,ULONG,PVOID,ULONG);
	typedef nt::Status(NTAPI* QVM)(HANDLE,PVOID,ULONG,PVOID,SIZE_T,PSIZE_T);
	typedef nt::Status(NTAPI* QO)(HANDLE,ULONG,PVOID,ULONG,PULONG);
	typedef nt::Status(NTAPI* IPJ)(HANDLE,HANDLE);
	typedef nt::Status(NTAPI* DUP)(HANDLE,HANDLE,HANDLE,PHANDLE,ACCESS_MASK,ULONG,ULONG);
	typedef nt::Status(NTAPI* CMP)(HANDLE,HANDLE);

	const ULONG SystemProcessInformation_ = 5;
	const ULONG SystemExtendedHandleInformation_ = 64;
	const ULONG ProcessQuotaLimits_ = 1;
	const ULONG ProcessVmCounters_ = 3;
	const ULONG ProcessWow64Information_ = 26;
	const ULONG ProcessPagePriority_ = 39;
	const ULONG ProcessMitigationPolicy_ = 52;
	const ULONG ProcessCommandLineInformation_ = 60;
	const ULONG ProcessJobMemoryInformation_ = 69;
	const ULONG ThreadBasicInformation_ = 0;
	const ULONG ThreadPagePriority_ = 24;
	const ULONG MemoryWorkingSetInformation_ = 1;
	const ULONG MemoryWorkingSetExInformation_ = 4;
	const ULONG ObjectNameInformation_ = 1;
	const ULONG SideChannelPolicy_ = 14;

	const nt::Status PROCESS_NOT_IN_JOB_ = 0x00000123L;
	const nt::Status PROCESS_IN_JOB_ = 0x00000124L;
	const nt::Status MORE_ENTRIES_ = 0x00000105L;
	const nt::Status NO_JOB_ = (nt::Status)0xC00001AEL; // ProcessJobMemoryInformation outside a job, Win32 error 759
	const nt::Status NO_RES_ = (nt::Status)0xC000009AL;
	const ACCESS_MASK JOB_QUERY_ = 0x0004;
	const ACCESS_MASK DIR_ACCESS_ = 0x0003; // query, traverse

	// phnt ntpsapi.h
	struct JMI {
		ULONGLONG SharedCommitUsage,PrivateCommitUsage,PeakPrivateCommitUsage,PrivateCommitLimit,TotalCommitLimit;
	};

	struct VMC {
		SIZE_T PeakVirtualSize,VirtualSize;
		ULONG PageFaultCount;
		SIZE_T PeakWorkingSetSize,WorkingSetSize;
		SIZE_T QuotaPeakPagedPoolUsage,QuotaPagedPoolUsage,QuotaPeakNonPagedPoolUsage,QuotaNonPagedPoolUsage;
		SIZE_T PagefileUsage,PeakPagefileUsage;
	};

	struct TBI {
		LONG ExitStatus;
		PVOID TebBaseAddress;
		nt::CID ClientId;
		ULONG_PTR AffinityMask;
		LONG Priority,BasePriority;
	};

	struct MIT {
		ULONG Policy,Flags;
	};

	// phnt ntmmapi.h, MEMORY_WORKING_SET_EX_INFORMATION
	struct WSX {
		PVOID VirtualAddress;
		ULONG_PTR Attr;
	};

	// phnt ntexapi.h, SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX
	struct HEX {
		PVOID Object;
		HANDLE UniqueProcessId,HandleValue;
		ACCESS_MASK GrantedAccess;
		USHORT CreatorBackTraceIndex,ObjectTypeIndex;
		ULONG HandleAttributes,Reserved;
	};

	// phnt ntobapi.h and ntdef
	struct ODI {
		nt::USTR Name,TypeName;
	};

	struct OA {
		ULONG Length;
		HANDLE RootDirectory;
		nt::USTR* ObjectName;
		ULONG Attributes;
		PVOID SecurityDescriptor,SecurityQualityOfService;
	};

	typedef nt::Status(NTAPI* OJ)(PHANDLE,ACCESS_MASK,OA*);

#ifdef _WIN64
	static_assert(sizeof(JMI) == 0x28,"JMI");
	static_assert(sizeof(VMC) == 0x58,"VMC");
	static_assert(sizeof(TBI) == 0x30,"TBI");
	static_assert(sizeof(WSX) == 0x10,"WSX");
	static_assert(sizeof(HEX) == 0x28,"HEX");
	static_assert(sizeof(ODI) == 0x20,"ODI");
	static_assert(sizeof(OA) == 0x30,"OA");
#endif

	std::string toU8(const wchar_t* w,int n) {
		if (!w || n <= 0) return "";
		int sz = WideCharToMultiByte(CP_UTF8,0,w,n,nullptr,0,nullptr,nullptr);
		if (sz <= 0) return "";
		std::string s(sz,'\0');
		WideCharToMultiByte(CP_UTF8,0,w,n,&s[0],sz,nullptr,nullptr);
		return s;
	}

	std::wstring toW(const std::string& s) {
		int n = MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),nullptr,0);
		std::wstring w(n > 0 ? n : 0,L'\0');
		if (n > 0) MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),&w[0],n);
		return w;
	}

	std::string winErr(DWORD e) {
		if (e == ERROR_ACCESS_DENIED) return "access denied, protected or needs admin";
		if (e == ERROR_INVALID_PARAMETER) return "process exited";
		return "error " + std::to_string(e);
	}

	std::string ntErr(nt::Status st) {
		if (st == nt::ACCESS_DENIED) return "access denied, protected or needs admin";
		if (st == nt::PRIVILEGE_NOT_HELD) return "needs a privilege FRS does not hold, run it as admin";
		if (st == nt::PROCESS_IS_TERMINATING) return "process is exiting";
		if (st == nt::INVALID_PARAMETER) return "Windows rejected the value";
		char b[32];
		snprintf(b,sizeof(b),"NTSTATUS 0x%08lX",(unsigned long)st);
		return b;
	}

	bool sameStart(HANDLE h,uint64_t start) {
		FILETIME c,e,k,u;
		if (!GetProcessTimes(h,&c,&e,&k,&u)) return false;
		return (((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime) == start;
	}

	HANDLE openP(const ProcRow& r,DWORD acc,std::string& why) {
		HANDLE h = OpenProcess(acc,FALSE,r.pid);
		if (!h) {
			why = winErr(GetLastError());
			return nullptr;
		}
		if (!sameStart(h,r.start)) {
			CloseHandle(h);
			why = "process exited";
			return nullptr;
		}
		return h;
	}

	QIP qip() {
		static auto f = nt::fn<QIP>("NtQueryInformationProcess");
		return f;
	}

	std::string cmdOf(HANDLE h,std::string* why) {
		auto q = qip();
		if (!q) return "";
		std::vector<char> b(4096);
		ULONG need = 0;
		nt::Status st = q(h,ProcessCommandLineInformation_,b.data(),(ULONG)b.size(),&need);
		if ((st == nt::INFO_LENGTH_MISMATCH || st == nt::BUFFER_OVERFLOW || st == nt::BUFFER_TOO_SMALL) && need > b.size() && need <= (1u << 20)) {
			b.resize(need);
			st = q(h,ProcessCommandLineInformation_,b.data(),(ULONG)b.size(),&need);
		}
		if (!nt::ok(st)) {
			if (why) *why = ntErr(st);
			return "";
		}
		const nt::USTR* u = (const nt::USTR*)b.data();
		if (!u->Buffer || !u->Length) return "";
		return util::safe(toU8(u->Buffer,u->Length / 2));
	}

	uint64_t pageSize() {
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		return si.dwPageSize;
	}

	struct Reg {
		uint64_t a,b;
		DWORD type;
	};

	// the WS list comes in any order, so one sorted region table instead of a query per page
	std::vector<Reg> regions(HANDLE h) {
		std::vector<Reg> v;
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		uint64_t p = (uint64_t)si.lpMinimumApplicationAddress;
		uint64_t end = (uint64_t)si.lpMaximumApplicationAddress;
		MEMORY_BASIC_INFORMATION m;
		while (p < end && VirtualQueryEx(h,(LPCVOID)p,&m,sizeof(m)) == sizeof(m)) {
			uint64_t next = (uint64_t)m.BaseAddress + m.RegionSize;
			if (next <= p) break;
			if (m.State != MEM_FREE) v.push_back({(uint64_t)m.BaseAddress,next,m.Type});
			p = next;
		}
		return v;
	}

	DWORD typeAt(const std::vector<Reg>& v,uint64_t a,size_t& cur) {
		if (cur < v.size() && a >= v[cur].a && a < v[cur].b) return v[cur].type;
		auto it = std::upper_bound(v.begin(),v.end(),a,[](uint64_t x,const Reg& r) { return x < r.a; });
		if (it == v.begin()) return 0;
		--it;
		if (a >= it->b) return 0;
		cur = (size_t)(it - v.begin());
		return it->type;
	}

	bool inJob(HANDLE proc,HANDLE job) {
		static auto f = nt::fn<IPJ>("NtIsProcessInJob");
		// not in job is a success code too, so the exact value decides
		return f && f(proc,job) == PROCESS_IN_JOB_;
	}

	std::string objName(HANDLE h) {
		static auto f = nt::fn<QO>("NtQueryObject");
		if (!f) return "";
		char b[1024];
		ULONG n = 0;
		if (!nt::ok(f(h,ObjectNameInformation_,b,sizeof(b),&n))) return "";
		const nt::USTR* u = (const nt::USTR*)b;
		if (!u->Buffer || !u->Length) return "";
		return util::safe(toU8(u->Buffer,u->Length / 2));
	}

	struct Found {
		HANDLE h;
		std::string via;
		uint32_t pid; // holder, 0 for a named job
		HANDLE hv; // handle value in the holder
		std::wstring path; // named job
	};

	// one job often has many handles, keep it once. Takes over f.h.
	void addFound(std::vector<Found>& out,const Found& f) {
		static auto cmp = nt::fn<CMP>("NtCompareObjects");
		for (auto& y : out) if (cmp && cmp(f.h,y.h) == nt::SUCCESS) {
			CloseHandle(f.h);
			return;
		}
		out.push_back(f);
	}

	// no driver : any process's handle to the job works, so walk the handle table. Misses a job nobody holds a
	// handle to, or that only protected or higher rights processes hold. The duplicate is closed again right
	// away by the caller, keeping it would keep a kill on close job alive.
	void scanHandles(HANDLE proc,std::vector<Found>& out,std::string& why) {
		uint32_t self = GetCurrentProcessId();
		static auto dup = nt::fn<DUP>("NtDuplicateObject");
		if (!dup) {
			why = "NtDuplicateObject missing";
			return;
		}
		HANDLE own = CreateJobObjectW(nullptr,nullptr);
		if (!own) {
			why = winErr(GetLastError());
			return;
		}
		std::vector<char> buf;
		nt::Status st = nt::querySys(SystemExtendedHandleInformation_,buf);
		if (!nt::ok(st)) {
			CloseHandle(own);
			why = "handle list : " + ntErr(st);
			return;
		}
		const size_t hdr = sizeof(ULONG_PTR) * 2;
		if (buf.size() < hdr) {
			CloseHandle(own);
			why = "handle list came back empty";
			return;
		}
		size_t n = (size_t)*(const ULONG_PTR*)buf.data();
		n = std::min(n,(buf.size() - hdr) / sizeof(HEX));
		const HEX* e = (const HEX*)(buf.data() + hdr);
		int type = -1;
		for (size_t i = 0; i < n && type < 0; i++) if ((uint32_t)(ULONG_PTR)e[i].UniqueProcessId == self && e[i].HandleValue == own) type = e[i].ObjectTypeIndex;
		CloseHandle(own);
		if (type < 0) {
			why = "could not tell the job object type";
			return;
		}
		std::map<uint32_t,HANDLE> owners;
		size_t tried = 0,opened = 0;
		for (size_t i = 0; i < n && out.size() < 16; i++) {
			if (e[i].ObjectTypeIndex != type) continue;
			uint32_t pid = (uint32_t)(ULONG_PTR)e[i].UniqueProcessId;
			tried++;
			auto it = owners.find(pid);
			if (it == owners.end()) it = owners.emplace(pid,OpenProcess(PROCESS_DUP_HANDLE,FALSE,pid)).first;
			if (!it->second) continue;
			HANDLE d = nullptr;
			if (!nt::ok(dup(it->second,e[i].HandleValue,GetCurrentProcess(),&d,JOB_QUERY_,0,0)) || !d) continue;
			opened++;
			if (inJob(proc,d)) addFound(out,{d,"handle held by pid " + std::to_string(pid),pid,e[i].HandleValue,L""});
			else CloseHandle(d);
		}
		for (auto& o : owners) if (o.second) CloseHandle(o.second);
		if (out.empty()) why = "FRS could open " + std::to_string(opened) + " of " + std::to_string(tried) + " job handles on the system and none is this job. Whoever holds its handle is protected or has higher rights, or nobody holds one.";
	}

	// named jobs need no owner, SI's only driverless path. Finds what a higher rights owner hides when the DACL lets us in
	void scanNamed(HANDLE proc,uint32_t session,std::vector<Found>& out) {
		typedef nt::Status(NTAPI* OD)(PHANDLE,ACCESS_MASK,OA*);
		typedef nt::Status(NTAPI* QD)(HANDLE,PVOID,ULONG,BOOLEAN,BOOLEAN,PULONG,PULONG);
		static auto od = nt::fn<OD>("NtOpenDirectoryObject");
		static auto qd = nt::fn<QD>("NtQueryDirectoryObject");
		static auto oj = nt::fn<OJ>("NtOpenJobObject");
		if (!od || !qd || !oj) return;
		std::wstring paths[2] = {L"\\BaseNamedObjects",L"\\Sessions\\" + std::to_wstring(session) + L"\\BaseNamedObjects"};
		std::vector<char> buf(0x10000);
		for (auto& p : paths) {
			nt::USTR us{(USHORT)(p.size() * 2),(USHORT)(p.size() * 2),&p[0]};
			OA a{sizeof(OA),nullptr,&us,0x40,nullptr,nullptr};
			HANDLE dir = nullptr;
			if (!nt::ok(od(&dir,DIR_ACCESS_,&a))) continue;
			ULONG ctx = 0,ret = 0;
			bool first = true;
			for (int g = 0; g < 2000 && out.size() < 16; g++) {
				nt::Status st = qd(dir,buf.data(),(ULONG)buf.size(),FALSE,first,&ctx,&ret);
				first = false;
				if (!nt::ok(st)) break;
				const ODI* i = (const ODI*)buf.data();
				for (; i->Name.Length && out.size() < 16; i++) {
					if (i->TypeName.Length != 6 || wcsncmp(i->TypeName.Buffer,L"Job",3) != 0) continue;
					nt::USTR nm = i->Name;
					OA ja{sizeof(OA),dir,&nm,0x40,nullptr,nullptr};
					HANDLE j = nullptr;
					if (!nt::ok(oj(&j,JOB_QUERY_,&ja)) || !j) continue;
					if (inJob(proc,j)) addFound(out,{j,"named job",0,nullptr,p + L"\\" + std::wstring(nm.Buffer,nm.Length / 2)});
					else CloseHandle(j);
				}
				if (st != MORE_ENTRIES_) break;
			}
			CloseHandle(dir);
		}
	}

	// a job a past scan found, opened again without walking the handle table. nullptr when that fails or it no longer holds proc
	HANDLE reopen(HANDLE proc,const Found& f) {
		HANDLE d = nullptr;
		if (f.pid) {
			static auto dup = nt::fn<DUP>("NtDuplicateObject");
			HANDLE o = OpenProcess(PROCESS_DUP_HANDLE,FALSE,f.pid);
			if (!o) return nullptr;
			if (!dup || !nt::ok(dup(o,f.hv,GetCurrentProcess(),&d,JOB_QUERY_,0,0))) d = nullptr;
			CloseHandle(o);
		} else {
			static auto oj = nt::fn<OJ>("NtOpenJobObject");
			std::wstring p = f.path;
			nt::USTR us{(USHORT)(p.size() * 2),(USHORT)(p.size() * 2),&p[0]};
			OA a{sizeof(OA),nullptr,&us,0x40,nullptr,nullptr};
			if (!oj || !nt::ok(oj(&d,JOB_QUERY_,&a))) d = nullptr;
		}
		if (d && !inJob(proc,d)) {
			CloseHandle(d);
			d = nullptr;
		}
		return d;
	}

	// where the jobs of a process were found, so the Limits tab re-reads them every 5 s without a new handle table walk
	struct JobIds {
		std::vector<Found> ids;
		std::string why;
		uint64_t jobId = 0,t = 0;
	};
	std::mutex g_jm;
	std::map<std::pair<uint32_t,uint64_t>,JobIds> g_jc;

	Job readJob(const Found& f) {
		Job j;
		j.via = f.via;
		j.name = objName(f.h);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION x = {};
		if (QueryInformationJobObject(f.h,JobObjectExtendedLimitInformation,&x,sizeof(x),nullptr)) {
			DWORD fl = x.BasicLimitInformation.LimitFlags;
			j.procMem = fl & JOB_OBJECT_LIMIT_PROCESS_MEMORY ? x.ProcessMemoryLimit : NOLIM;
			j.jobMem = fl & JOB_OBJECT_LIMIT_JOB_MEMORY ? x.JobMemoryLimit : NOLIM;
			j.wsMin = fl & JOB_OBJECT_LIMIT_WORKINGSET ? x.BasicLimitInformation.MinimumWorkingSetSize : NOLIM;
			j.wsMax = fl & JOB_OBJECT_LIMIT_WORKINGSET ? x.BasicLimitInformation.MaximumWorkingSetSize : NOLIM;
			j.peakProc = x.PeakProcessMemoryUsed;
			j.peakJob = x.PeakJobMemoryUsed;
		} else {
			j.why = "limits : " + winErr(GetLastError());
		}
		JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION a = {};
		if (QueryInformationJobObject(f.h,JobObjectBasicAndIoAccountingInformation,&a,sizeof(a),nullptr)) {
			j.faults = a.BasicInfo.TotalPageFaultCount;
			j.total = a.BasicInfo.TotalProcesses;
			j.active = a.BasicInfo.ActiveProcesses;
			j.term = a.BasicInfo.TotalTerminatedProcesses;
		} else {
			if (!j.why.empty()) j.why += ", ";
			j.why += "accounting : " + winErr(GetLastError());
		}
		return j;
	}

	const wchar_t* IFEO_ = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";

	// FRS_TEST_IFEO_KEY : the same logic against a scratch key under HKCU, for unelevated test runs. Ignored when elevated.
	HKEY ifeoRoot(std::wstring& base) {
		static const char* e = isElevated() ? nullptr : util::testEnv("FRS_TEST_IFEO_KEY");
		if (e && *e) {
			base = toW(e);
			return HKEY_CURRENT_USER;
		}
		base = IFEO_;
		return HKEY_LOCAL_MACHINE;
	}

	std::string regErr(LONG e) {
		if (e == ERROR_ACCESS_DENIED) return "access denied, IFEO needs admin";
		return "registry error " + std::to_string(e);
	}

	bool keyEmpty(HKEY k) {
		DWORD subs = 1,vals = 1;
		return RegQueryInfoKeyW(k,nullptr,nullptr,nullptr,&subs,nullptr,nullptr,&vals,nullptr,nullptr,nullptr,nullptr) == ERROR_SUCCESS && !subs && !vals;
	}

	const char* memWait(ULONG reason) {
		switch (reason) {
			case 1: return "FreePage";
			case 2: return "PageIn";
			case 3: return "PoolAllocation";
			case 8: return "WrFreePage";
			case 9: return "WrPageIn";
			case 10: return "WrPoolAllocation";
			case 18: return "WrVirtualMemory";
			case 19: return "WrPageOut";
			case 39: return "WrPhysicalFault";
			case 41: return "WrMdlCache";
		}
		return nullptr;
	}

	bool stackOf(HANDLE th,HANDLE proc,bool wow,uint64_t& used,uint64_t& limit) {
		static auto qit = nt::fn<QIT>("NtQueryInformationThread");
		if (!qit) return false;
		TBI b;
		if (!nt::ok(qit(th,ThreadBasicInformation_,&b,sizeof(b),nullptr)) || !b.TebBaseAddress) return false;
		uint64_t base = 0,lim = 0;
		if (wow) {
			// WOW64_GET_TEB32 : the 32 bit TEB sits one 8 KB past the native one. NtTib : ExceptionList, StackBase, StackLimit
			DWORD t[3] = {};
			SIZE_T got = 0;
			if (!ReadProcessMemory(proc,(const char*)b.TebBaseAddress + 0x2000,t,sizeof(t),&got) || got != sizeof(t)) return false;
			base = t[1];
			lim = t[2];
		} else {
			ULONG_PTR t[3] = {};
			SIZE_T got = 0;
			if (!ReadProcessMemory(proc,b.TebBaseAddress,t,sizeof(t),&got) || got != sizeof(t)) return false;
			base = t[1];
			lim = t[2];
		}
		if (!base || !lim || base < lim) return false;
		MEMORY_BASIC_INFORMATION m;
		if (VirtualQueryEx(proc,(LPCVOID)lim,&m,sizeof(m)) != sizeof(m)) return false;
		used = base - lim;
		limit = base - (uint64_t)m.AllocationBase;
		return true;
	}

}

namespace procdet { namespace be {

	void fast(const ProcRow& r,Det& d) {
		std::string why;
		d.commit = Commit();
		d.quota = Quota();
		d.denied = false;
		d.why.clear();
		HANDLE h = openP(r,PROCESS_QUERY_LIMITED_INFORMATION,why);
		if (!h) {
			d.denied = true;
			d.noFull = true;
			d.why = why;
			d.commit.why = why;
			d.quota.why = why;
			d.jobs.in = r.jobId == NA ? -1 : r.jobId != 0;
			return;
		}
		HANDLE q = OpenProcess(PROCESS_QUERY_INFORMATION,FALSE,r.pid);
		d.noFull = !q;
		HANDLE u = q ? q : h;
		auto f = qip();
		if (f) {
			JMI j;
			nt::Status st = f(u,ProcessJobMemoryInformation_,&j,sizeof(j),nullptr);
			if (nt::ok(st)) {
				d.commit.shared = j.SharedCommitUsage;
				d.commit.priv = j.PrivateCommitUsage;
				d.commit.peak = j.PeakPrivateCommitUsage;
				// measured : for a process in a job all of this is the whole job, and PrivateCommitLimit is its job memory limit, 0 when none is set
				d.commit.privLimit = j.PrivateCommitLimit ? j.PrivateCommitLimit : NOLIM;
				d.commit.totalLimit = j.TotalCommitLimit ? j.TotalCommitLimit : NOLIM;
			} else {
				d.commit.why = st == NO_JOB_ ? "Not in a job, Windows has no commit view for this process" : ntErr(st);
			}
			QUOTA_LIMITS_EX x = {};
			st = f(u,ProcessQuotaLimits_,&x,sizeof(x),nullptr);
			if (nt::ok(st)) {
				d.quota.minWs = x.MinimumWorkingSetSize;
				d.quota.maxWs = x.MaximumWorkingSetSize;
				d.quota.paged = x.PagedPoolLimit == (SIZE_T)-1 ? NOLIM : x.PagedPoolLimit;
				d.quota.nonPaged = x.NonPagedPoolLimit == (SIZE_T)-1 ? NOLIM : x.NonPagedPoolLimit;
				d.quota.pagefile = x.PagefileLimit == (SIZE_T)-1 ? NOLIM : x.PagefileLimit;
				d.quota.hardMin = (x.Flags & QUOTA_LIMITS_HARDWS_MIN_ENABLE) ? 1 : 0;
				d.quota.hardMax = (x.Flags & QUOTA_LIMITS_HARDWS_MAX_ENABLE) ? 1 : 0;
			} else {
				d.quota.why = ntErr(st);
			}
			MIT m{SideChannelPolicy_,0};
			st = f(u,ProcessMitigationPolicy_,&m,sizeof(m),nullptr);
			d.combine = nt::ok(st) ? (int)((m.Flags >> 2) & 1) : -1;
		}
		static auto ipj = nt::fn<IPJ>("NtIsProcessInJob");
		if (ipj) {
			nt::Status st = ipj(h,nullptr);
			d.jobs.in = st == PROCESS_IN_JOB_ ? 1 : st == PROCESS_NOT_IN_JOB_ ? 0 : -1;
		}
		if (!d.cmdRead) {
			d.cmd = cmdOf(h,&d.cmdWhy);
			d.cmdRead = !d.cmd.empty() || !d.cmdWhy.empty();
		}
		if (q) CloseHandle(q);
		CloseHandle(h);
	}

	void ws(const ProcRow& r,Ws& w) {
		static auto qvm = nt::fn<QVM>("NtQueryVirtualMemory");
		if (!qvm) {
			w.why = "NtQueryVirtualMemory missing";
			return;
		}
		std::string why;
		HANDLE h = openP(r,PROCESS_QUERY_INFORMATION,why);
		if (!h) {
			w.why = why;
			return;
		}
		uint64_t pg = pageSize();
		const size_t head = sizeof(ULONG_PTR);
		const size_t mx = (size_t)1 << 30;
		std::vector<char> buf(std::min(mx,head + (size_t)((r.ws == NA ? 0 : r.ws) / pg + 4096) * sizeof(ULONG_PTR)));
		nt::Status st = nt::UNSUCCESSFUL;
		for (int i = 0; i < 8; i++) {
			st = qvm(h,nullptr,MemoryWorkingSetInformation_,buf.data(),buf.size(),nullptr);
			if (st != nt::INFO_LENGTH_MISMATCH && st != nt::BUFFER_OVERFLOW) break;
			size_t n = head + (size_t)(*(ULONG_PTR*)buf.data() + 4096) * sizeof(ULONG_PTR);
			if (n > mx) break;
			buf.resize(std::max(n,buf.size() + buf.size() / 2));
		}
		if (!nt::ok(st)) {
			CloseHandle(h);
			w.why = ntErr(st);
			return;
		}
		ULONG_PTR n = *(ULONG_PTR*)buf.data();
		const ULONG_PTR* b = (const ULONG_PTR*)(buf.data() + head);
		n = std::min(n,(ULONG_PTR)((buf.size() - head) / sizeof(ULONG_PTR)));
		uint64_t pv = 0,sa = 0,sh = 0,lock = 0,mod = 0,orig = 0;
		int top = -1;
		std::vector<Reg> regs = regions(h);
		size_t cur = 0;
		const size_t BATCH = 256;
		std::vector<WSX> ex(BATCH);
		size_t in = 0;
		bool exOk = true;
		auto flush = [&]() {
			if (!in || !exOk) {
				in = 0;
				return;
			}
			if (!nt::ok(qvm(h,nullptr,MemoryWorkingSetExInformation_,ex.data(),in * sizeof(WSX),nullptr))) {
				exOk = false;
				in = 0;
				return;
			}
			for (size_t i = 0; i < in; i++) {
				ULONG_PTR a = ex[i].Attr;
				if (!(a & 1)) continue;
				if ((a >> 22) & 1) lock++;
				int p = (int)((a >> 24) & 7);
				if (p > top) top = p;
				DWORD t = typeAt(regs,(uint64_t)ex[i].VirtualAddress,cur);
				if (t == MEM_MAPPED || t == MEM_IMAGE) {
					if ((a >> 30) & 1) orig++;
					else mod++;
				}
			}
			in = 0;
		};
		// Protection : 5, ShareCount : 3, Shared : 1, Node : 3, VirtualPage : 52
		for (ULONG_PTR i = 0; i < n; i++) {
			ULONG_PTR e = b[i];
			int sc = (int)((e >> 5) & 7);
			if (sc > 1) sh++;
			if (sc == 0) pv++;
			if ((e >> 8) & 1) sa++;
			ex[in].VirtualAddress = (PVOID)((e >> 12) * (ULONG_PTR)pg);
			ex[in].Attr = 0;
			if (++in == BATCH) flush();
		}
		flush();
		CloseHandle(h);
		w.total = (uint64_t)n * pg;
		w.priv = pv * pg;
		w.shareable = sa * pg;
		w.shared = sh * pg;
		if (exOk) {
			w.locked = lock * pg;
			w.prio = top;
			if (!regs.empty()) {
				w.modified = mod * pg;
				w.original = orig * pg;
			}
		}
	}

	void jobs(const ProcRow& r,JobView& v) {
		std::string why;
		HANDLE h = openP(r,PROCESS_QUERY_LIMITED_INFORMATION,why);
		if (!h) {
			v.why = why;
			v.in = r.jobId == NA ? -1 : r.jobId != 0;
			return;
		}
		static auto ipj = nt::fn<IPJ>("NtIsProcessInJob");
		nt::Status st = ipj ? ipj(h,nullptr) : nt::NOT_IMPLEMENTED;
		if (st == PROCESS_NOT_IN_JOB_) {
			v.in = 0;
			CloseHandle(h);
			return;
		}
		if (st != PROCESS_IN_JOB_) {
			v.why = ntErr(st);
			CloseHandle(h);
			return;
		}
		v.in = 1;
		std::vector<Found> f;
		auto key = std::make_pair(r.pid,r.start);
		uint64_t now = util::nowMs();
		JobIds c;
		bool hit = false;
		{
			std::lock_guard<std::mutex> lk(g_jm);
			auto it = g_jc.find(key);
			// a new innermost job id means the process moved, the old list can't say
			if (it != g_jc.end() && now < it->second.t + 60000 && it->second.jobId == r.jobId) {
				c = it->second;
				hit = true;
			}
		}
		for (size_t i = 0; hit && i < c.ids.size(); i++) {
			Found x = c.ids[i];
			x.h = reopen(h,x);
			if (x.h) f.push_back(x);
			else hit = false;
		}
		if (hit) v.why = c.why;
		else {
			for (auto& x : f) CloseHandle(x.h);
			f.clear();
			scanHandles(h,f,v.why);
			scanNamed(h,r.session,f);
			c.ids = f;
			for (auto& x : c.ids) x.h = nullptr;
			c.why = v.why;
			c.jobId = r.jobId;
			c.t = now;
			std::lock_guard<std::mutex> lk(g_jm);
			for (auto it = g_jc.begin(); it != g_jc.end();) {
				if (now > it->second.t + 120000) it = g_jc.erase(it);
				else ++it;
			}
			g_jc[key] = c;
		}
		CloseHandle(h);
		for (auto& x : f) {
			v.jobs.push_back(readJob(x));
			CloseHandle(x.h);
		}
		if (!v.jobs.empty()) v.why.clear();
		// nested jobs : a child only ever holds a subset of its parent's processes, so fewer active, then fewer in total, comes first
		std::stable_sort(v.jobs.begin(),v.jobs.end(),[](const Job& a,const Job& b) { return a.active != b.active ? a.active < b.active : a.total < b.total; });
		for (size_t i = 1; i < v.jobs.size(); i++) if (v.jobs[i].active == v.jobs[i - 1].active && v.jobs[i].total == v.jobs[i - 1].total) v.why = "Some of these jobs have the same process counts, so FRS can't tell which one holds the other and their order is unknown.";
	}

	void threads(const ProcRow& r,ThrView& t) {
		static auto qit = nt::fn<QIT>("NtQueryInformationThread");
		t.read = true;
		std::vector<char> buf;
		nt::Status st = nt::querySys(SystemProcessInformation_,buf);
		if (!nt::ok(st)) {
			t.why = ntErr(st);
			return;
		}
		const size_t hdr = offsetof(nt::SPI,Threads);
		size_t base = 0;
		const nt::SPI* p = nullptr;
		while (base + hdr <= buf.size()) {
			const nt::SPI* x = (const nt::SPI*)(buf.data() + base);
			if ((uint32_t)(ULONG_PTR)x->UniqueProcessId == r.pid && (uint64_t)x->CreateTime.QuadPart == r.start) {
				p = x;
				break;
			}
			if (!x->NextEntryOffset) break;
			base += x->NextEntryOffset;
		}
		if (!p) {
			t.why = "process exited";
			return;
		}
		size_t room = (p->NextEntryOffset ? (size_t)p->NextEntryOffset : buf.size() - base);
		size_t cnt = std::min((size_t)p->NumberOfThreads,(room - std::min(room,hdr)) / sizeof(nt::STI));
		std::string pw;
		HANDLE proc = openP(r,PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,pw);
		if (!proc && pw == "process exited") {
			t.why = pw;
			return;
		}
		bool wow = false;
		if (proc) {
			auto f = qip();
			ULONG_PTR w = 0;
			if (f && nt::ok(f(proc,ProcessWow64Information_,&w,sizeof(w),nullptr))) wow = w != 0;
		}
		t.v.reserve(cnt);
		int denied = 0;
		for (size_t i = 0; i < cnt; i++) {
			const nt::STI& s = p->Threads[i];
			Thr x;
			x.tid = (uint32_t)(ULONG_PTR)s.ClientId.UniqueThread;
			x.start = (uint64_t)s.CreateTime.QuadPart;
			HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,x.tid);
			if (!th) denied++;
			else {
				FILETIME c,e,k,u;
				// the tid went to another thread since the snapshot, its numbers must not show here
				bool same = GetThreadTimes(th,&c,&e,&k,&u) && (((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime) == x.start;
				if (same) {
					ULONG pp = 0;
					if (qit && nt::ok(qit(th,ThreadPagePriority_,&pp,sizeof(pp),nullptr))) x.prio = (int)pp;
					if (proc) stackOf(th,proc,wow,x.stackUsed,x.stackLimit);
				}
				CloseHandle(th);
				if (!same) continue;
			}
			const char* mw = s.ThreadState == 5 ? memWait(s.WaitReason) : nullptr;
			if (mw) {
				x.wait = mw;
				t.memWaits++;
			}
			t.v.push_back(std::move(x));
		}
		if (denied) t.why = "FRS can't open " + std::to_string(denied) + " of " + std::to_string(cnt) + " threads, access denied, protected or needs admin";
		else if (!proc) t.why = "FRS can't read this process's memory, so stack sizes stay unknown";
		if (proc) CloseHandle(proc);
	}

	std::string setThrPrio(const ProcRow& r,uint32_t tid,uint64_t tstart,int prio,int* prev) {
		static auto qit = nt::fn<QIT>("NtQueryInformationThread");
		static auto sit = nt::fn<SIT>("NtSetInformationThread");
		if (!qit || !sit) return "NtSetInformationThread missing";
		HANDLE th = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,FALSE,tid);
		if (!th) {
			DWORD e = GetLastError();
			return e == ERROR_INVALID_PARAMETER ? "no such thread, it exited or the tid is wrong" : winErr(e);
		}
		std::string why;
		FILETIME c,e,k,u;
		if (GetProcessIdOfThread(th) != r.pid) why = "that thread is not in this process";
		else if (tstart && (!GetThreadTimes(th,&c,&e,&k,&u) || (((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime) != tstart)) why = "thread exited";
		else {
			ULONG old = 0;
			if (prev && nt::ok(qit(th,ThreadPagePriority_,&old,sizeof(old),nullptr))) *prev = (int)old;
			ULONG v = (ULONG)prio;
			nt::Status st = sit(th,ThreadPagePriority_,&v,sizeof(v));
			if (!nt::ok(st)) why = ntErr(st);
		}
		CloseHandle(th);
		return why;
	}

	std::string setQuota(const ProcRow& r,uint64_t mn,uint64_t mx) {
		static auto sip = nt::fn<SIP>("NtSetInformationProcess");
		if (!sip) return "NtSetInformationProcess missing";
		// measured : this leaves the working set alone and still answers success
		if (!mn) return "Windows ignores a minimum of 0, give one above 0";
		std::string why;
		HANDLE h = openP(r,PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION,why);
		if (!h) return why;
		QUOTA_LIMITS_EX q = {};
		q.MinimumWorkingSetSize = (SIZE_T)mn;
		q.MaximumWorkingSetSize = (SIZE_T)mx;
		nt::Status st = sip(h,ProcessQuotaLimits_,&q,sizeof(q));
		QUOTA_LIMITS_EX b = {};
		auto f = qip();
		bool got = nt::ok(st) && f && nt::ok(f(h,ProcessQuotaLimits_,&b,sizeof(b),nullptr));
		CloseHandle(h);
		if (st == NO_RES_) return "Windows can't keep that much in this process, give smaller sizes";
		if (!nt::ok(st)) return ntErr(st);
		uint64_t pg = pageSize();
		if (got && (b.MinimumWorkingSetSize / pg != mn / pg || b.MaximumWorkingSetSize / pg != mx / pg)) return "Windows applied minimum " + util::size(b.MinimumWorkingSetSize) + ", maximum " + util::size(b.MaximumWorkingSetSize) + " instead";
		return "";
	}

	std::string cmdLine(const ProcRow& r) {
		std::string why;
		HANDLE h = openP(r,PROCESS_QUERY_LIMITED_INFORMATION,why);
		if (!h) return "";
		std::string s = cmdOf(h,nullptr);
		CloseHandle(h);
		return s;
	}

	bool now(const ProcRow& r,uint64_t& ws,uint64_t& priv) {
		std::string why;
		HANDLE h = openP(r,PROCESS_QUERY_LIMITED_INFORMATION,why);
		if (!h) return false;
		auto f = qip();
		VMC c;
		bool ok = f && nt::ok(f(h,ProcessVmCounters_,&c,sizeof(c),nullptr));
		CloseHandle(h);
		if (!ok) return false;
		ws = c.WorkingSetSize;
		priv = c.PagefileUsage;
		return true;
	}

	int pagePrio(const ProcRow& r) {
		std::string why;
		HANDLE h = openP(r,PROCESS_QUERY_LIMITED_INFORMATION,why);
		if (!h) return -1;
		auto f = qip();
		ULONG p = 0;
		bool ok = f && nt::ok(f(h,ProcessPagePriority_,&p,sizeof(p),nullptr));
		CloseHandle(h);
		return ok ? (int)p : -1;
	}

	bool applyRule(const ProcRow& r,int prio,std::string& why) {
		if (r.pagePrio == prio) return false;
		std::string s = procs::setPagePrio(procs::ref(r),prio,nullptr);
		size_t a = s.find("failed (");
		if (a != std::string::npos) {
			why = s.substr(a + 8);
			if (!why.empty() && why.back() == ')') why.pop_back();
		}
		return true;
	}

	void lowPrio() {
		static auto sit = nt::fn<SIT>("NtSetInformationThread");
		ULONG v = 2; // MEMORY_PRIORITY_LOW
		if (sit) sit(GetCurrentThread(),ThreadPagePriority_,&v,sizeof(v));
		SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
	}

	bool ifeoOk() {
		return true;
	}

	bool ifeoCanWrite() {
		static const bool w = [] {
			std::wstring b;
			return ifeoRoot(b) == HKEY_CURRENT_USER || isElevated();
		}();
		return w;
	}

	int ifeoGet(const std::string& exe) {
		if (exe.empty() || exe.find('\\') != std::string::npos) return -1;
		std::wstring base;
		HKEY root = ifeoRoot(base);
		std::wstring p = base + L"\\" + toW(exe) + L"\\PerfOptions";
		DWORD v = 0,sz = sizeof(v),ty = 0;
		HKEY k;
		if (RegOpenKeyExW(root,p.c_str(),0,KEY_QUERY_VALUE,&k) != ERROR_SUCCESS) return -1;
		LONG e = RegQueryValueExW(k,L"PagePriority",nullptr,&ty,(LPBYTE)&v,&sz);
		RegCloseKey(k);
		return e == ERROR_SUCCESS && ty == REG_DWORD && v <= 7 ? (int)v : -1;
	}

	std::string ifeoSet(const std::string& exe,int prio) {
		std::wstring base;
		HKEY root = ifeoRoot(base);
		std::wstring p = base + L"\\" + toW(exe) + L"\\PerfOptions";
		HKEY k;
		LONG e = RegCreateKeyExW(root,p.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&k,nullptr);
		if (e != ERROR_SUCCESS) return regErr(e);
		DWORD v = (DWORD)prio;
		e = RegSetValueExW(k,L"PagePriority",0,REG_DWORD,(const BYTE*)&v,sizeof(v));
		RegCloseKey(k);
		return e == ERROR_SUCCESS ? "" : regErr(e);
	}

	// SI deletes PerfOptions and then the exe key without looking, which loses a Debugger or GlobalFlag next to it.
	// Here only our value goes, and a key only when nothing at all is left in it. Nothing gets created on the way.
	std::string ifeoDel(const std::string& exe,bool& had) {
		had = false;
		std::wstring base;
		HKEY root = ifeoRoot(base);
		std::wstring nm = toW(exe);
		std::wstring ex = base + L"\\" + nm;
		std::wstring pf = ex + L"\\PerfOptions";
		HKEY k;
		LONG e = RegOpenKeyExW(root,pf.c_str(),0,KEY_SET_VALUE | KEY_QUERY_VALUE,&k);
		if (e == ERROR_FILE_NOT_FOUND) return "";
		if (e != ERROR_SUCCESS) return regErr(e);
		e = RegDeleteValueW(k,L"PagePriority");
		if (e == ERROR_FILE_NOT_FOUND) {
			RegCloseKey(k);
			return "";
		}
		if (e != ERROR_SUCCESS) {
			RegCloseKey(k);
			return regErr(e);
		}
		had = true;
		bool empty = keyEmpty(k);
		RegCloseKey(k);
		if (!empty) return "";
		HKEY x;
		if (RegOpenKeyExW(root,ex.c_str(),0,KEY_QUERY_VALUE | KEY_SET_VALUE,&x) != ERROR_SUCCESS) return "";
		RegDeleteKeyW(x,L"PerfOptions");
		empty = keyEmpty(x);
		RegCloseKey(x);
		if (!empty) return "";
		HKEY b;
		if (RegOpenKeyExW(root,base.c_str(),0,KEY_SET_VALUE,&b) != ERROR_SUCCESS) return "";
		RegDeleteKeyW(b,nm.c_str());
		RegCloseKey(b);
		return "";
	}

	std::vector<Rule> ifeoList() {
		std::vector<Rule> v;
		std::wstring base;
		HKEY root = ifeoRoot(base);
		HKEY k;
		if (RegOpenKeyExW(root,base.c_str(),0,KEY_ENUMERATE_SUB_KEYS,&k) != ERROR_SUCCESS) return v;
		wchar_t nm[256];
		for (DWORD i = 0; i < 20000; i++) {
			DWORD n = 256;
			if (RegEnumKeyExW(k,i,nm,&n,nullptr,nullptr,nullptr,nullptr) != ERROR_SUCCESS) break;
			std::string exe = toU8(nm,(int)n);
			int p = ifeoGet(exe);
			if (p < 0) continue;
			Rule r;
			r.kind = RuleIfeo;
			r.key = util::safe(exe);
			r.prio = p;
			v.push_back(r);
		}
		RegCloseKey(k);
		return v;
	}

} }
#endif // _WIN32
