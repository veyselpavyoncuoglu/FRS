#ifdef _WIN32
#include "gpumem.h"
#include "nt.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <map>
#include <set>

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

namespace {

	// MinGW has no d3dkmthk.h, so these are by hand from SI plugins/ExtendedTools/d3dkmt/d3dkmthk.h (Microsoft, MIT).
	// x64 offsets were printed from that header and asserted below.
	struct KmtInfo {
		UINT h;
		LUID luid;
		ULONG sources;
		BOOL precise;
	};

	struct KmtEnum {
		ULONG n;
		KmtInfo* list;
	};

	struct KmtQuery {
		UINT h,type;
		void* data;
		UINT size;
	};

	struct KmtClose {
		UINT h;
	};

	struct KmtSegSize {
		ULONGLONG ded,sysDed,shared;
	};

	struct KmtRegInfo {
		WCHAR adapter[MAX_PATH],bios[MAX_PATH],dac[MAX_PATH],chip[MAX_PATH];
	};

	struct KmtRegQuery {
		UINT type,flags;
		WCHAR name[MAX_PATH];
		ULONG valType,physIdx,outSize;
		UINT status;
		ULONGLONG q;
	};

	struct KmtSeg {
		ULONGLONG limit,committed,resident,evicted;
		ULONG allocC,allocR,aperture;
	};

	struct KmtProc {
		ULONG nodes,sources;
		ULONGLONG alloc,reserved;
		ULONG small,large;
		ULONGLONG wcAlloc,wcReserved,cachedAlloc,cachedReserved,sectionAlloc,sectionReserved;
	};

	struct KmtProcAdapter {
		ULONG segs,nodes,sources,virt;
		ULONG dmaCount;
		ULONGLONG dmaBytes;
		ULONG dmaAlloc,dmaPatch;
		ULONGLONG evicted;
	};

	struct KmtGroup {
		ULONGLONG budget,requested,usage;
	};

	struct KmtStats {
		UINT type;
		LUID luid;
		HANDLE proc;
		union {
			ULONGLONG raw[97];
			struct {
				ULONG segs,nodes;
			} ad;
			KmtSeg seg;
			KmtProc pr;
			KmtProcAdapter pa;
			KmtGroup grp;
			ULONGLONG procSegCommitted;
		};
		ULONG query;
	};

	struct KmtProcList {
		LUID luid;
		ULONG* ids;
		SIZE_T n;
	};

#ifdef _WIN64
	static_assert(sizeof(KmtInfo) == 20,"KmtInfo");
	static_assert(sizeof(KmtQuery) == 24,"KmtQuery");
	static_assert(sizeof(KmtRegInfo) == 2080,"KmtRegInfo");
	static_assert(offsetof(KmtRegQuery,valType) == 528 && offsetof(KmtRegQuery,outSize) == 536 && offsetof(KmtRegQuery,status) == 540 && offsetof(KmtRegQuery,q) == 544,"KmtRegQuery");
	static_assert(offsetof(KmtSeg,resident) == 16 && offsetof(KmtSeg,aperture) == 40,"KmtSeg");
	static_assert(offsetof(KmtProc,alloc) == 8 && offsetof(KmtProc,wcAlloc) == 32 && offsetof(KmtProc,sectionReserved) == 72,"KmtProc");
	static_assert(offsetof(KmtProcAdapter,virt) == 12 && offsetof(KmtProcAdapter,evicted) == 40,"KmtProcAdapter");
	static_assert(offsetof(KmtGroup,usage) == 16,"KmtGroup");
	static_assert(sizeof(KmtStats) == 0x328 && offsetof(KmtStats,luid) == 4 && offsetof(KmtStats,proc) == 16 && offsetof(KmtStats,raw) == 24 && offsetof(KmtStats,query) == 800,"KmtStats");
#endif

	const UINT QAI_SEGSIZE = 3,QAI_REGINFO = 8,QAI_TYPE = 15,QAI_REGISTRY = 48,QAI_DESC = 65;
	const UINT QS_ADAPTER = 0,QS_PROCESS = 1,QS_PROC_ADAPTER = 2,QS_SEGMENT = 3,QS_PROC_SEGMENT = 4,QS_PROC_GROUP = 9;
	const ULONG GROUP_LOCAL = 0,GROUP_NON_LOCAL = 1;

	using EnumFn = nt::Status(APIENTRY*)(KmtEnum*);
	using QueryFn = nt::Status(APIENTRY*)(KmtQuery*);
	using CloseFn = nt::Status(APIENTRY*)(const KmtClose*);
	using StatsFn = nt::Status(APIENTRY*)(KmtStats*);
	using ProcListFn = nt::Status(APIENTRY*)(KmtProcList*);

	struct Kmt {
		EnumFn en = nullptr;
		QueryFn query = nullptr;
		CloseFn close = nullptr;
		StatsFn stats = nullptr;
		ProcListFn procList = nullptr;
		Kmt() {
			HMODULE m = LoadLibraryExW(L"gdi32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
			if (!m) return;
			en = (EnumFn)(void*)GetProcAddress(m,"D3DKMTEnumAdapters2");
			query = (QueryFn)(void*)GetProcAddress(m,"D3DKMTQueryAdapterInfo");
			close = (CloseFn)(void*)GetProcAddress(m,"D3DKMTCloseAdapter");
			stats = (StatsFn)(void*)GetProcAddress(m,"D3DKMTQueryStatistics");
			procList = (ProcListFn)(void*)GetProcAddress(m,"D3DKMTEnumProcesses");
		}
		bool ok() const {
			return en && query && close && stats;
		}
	};

	const Kmt& kmt() {
		static Kmt k;
		return k;
	}

	std::string toU8(const wchar_t* w) {
		int sz = WideCharToMultiByte(CP_UTF8,0,w,-1,nullptr,0,nullptr,nullptr);
		if (sz <= 1) return "";
		std::string s(sz,'\0');
		WideCharToMultiByte(CP_UTF8,0,w,-1,&s[0],sz,nullptr,nullptr);
		s.pop_back();
		return s;
	}

	std::string statusTxt(nt::Status st) {
		char t[32];
		snprintf(t,sizeof(t),"status 0x%08lX",(unsigned long)st);
		return t;
	}

	struct Ad {
		LUID luid{};
		std::string name,id;
		std::vector<char> aperture; // per segment
		uint64_t dedLimit = NA,sharedLimit = NA,sysDed = NA,installed = NA,reserved = NA;
		bool soft = false;
	};

	bool qai(UINT h,UINT type,void* data,UINT size) {
		KmtQuery q{h,type,data,size};
		return nt::ok(kmt().query(&q));
	}

	void statsIn(KmtStats& q,UINT type,LUID luid,HANDLE proc = nullptr,ULONG input = 0) {
		memset(&q,0,sizeof(q));
		q.type = type;
		q.luid = luid;
		q.proc = proc;
		q.query = input;
	}

	std::string adapterName(UINT h,LUID luid) {
		KmtRegInfo ri;
		memset(&ri,0,sizeof(ri));
		if (qai(h,QAI_REGINFO,&ri,sizeof(ri)) && ri.adapter[0]) {
			ri.adapter[MAX_PATH - 1] = 0;
			return util::safe(toU8(ri.adapter));
		}
		WCHAR desc[4096];
		memset(desc,0,sizeof(desc));
		if (qai(h,QAI_DESC,desc,sizeof(desc)) && desc[0]) {
			desc[4095] = 0;
			return util::safe(toU8(desc));
		}
		char t[48];
		snprintf(t,sizeof(t),"Adapter %08lX",(unsigned long)luid.LowPart);
		return t;
	}

	uint64_t installedMem(UINT h) {
		KmtRegQuery q;
		memset(&q,0,sizeof(q));
		q.type = 1;
		q.flags = 1;
		wcscpy(q.name,L"HardwareInformation.qwMemorySize");
		q.valType = REG_QWORD;
		if (!qai(h,QAI_REGISTRY,&q,sizeof(q)) || q.status != 0 || !q.q) return NA;
		return q.q;
	}

	// job thread only
	std::vector<Ad> scanAdapters(std::string& why) {
		std::vector<Ad> out;
		const Kmt& k = kmt();
		if (!k.ok()) {
			why = "gdi32 has no D3DKMT calls";
			return out;
		}
		KmtEnum e{0,nullptr};
		nt::Status st = k.en(&e);
		if (!nt::ok(st)) {
			why = "D3DKMTEnumAdapters2 : " + statusTxt(st);
			return out;
		}
		std::vector<KmtInfo> v(e.n + 4);
		e.n = (ULONG)v.size();
		e.list = v.data();
		st = k.en(&e);
		if (!nt::ok(st)) {
			why = "D3DKMTEnumAdapters2 : " + statusTxt(st);
			return out;
		}
		for (ULONG i = 0; i < e.n && i < v.size(); i++) {
			Ad a;
			a.luid = v[i].luid;
			char id[32];
			snprintf(id,sizeof(id),"0x%08lX_0x%08lX",(unsigned long)a.luid.HighPart,(unsigned long)a.luid.LowPart);
			a.id = id;
			UINT at = 0;
			bool render = true;
			if (qai(v[i].h,QAI_TYPE,&at,sizeof(at))) {
				a.soft = at & 4;
				render = (at & 1) || (at & (1u << 11)); // render or compute only, a display only adapter holds no memory
			}
			a.name = adapterName(v[i].h,a.luid);
			KmtSegSize ss{};
			if (qai(v[i].h,QAI_SEGSIZE,&ss,sizeof(ss))) {
				a.dedLimit = ss.ded;
				a.sharedLimit = ss.shared;
				a.sysDed = ss.sysDed;
			}
			a.installed = installedMem(v[i].h);
			uint64_t visible = a.dedLimit == NA ? 0 : a.dedLimit;
			if (a.installed != NA && visible) a.reserved = a.installed > visible ? a.installed - visible : 0;
			KmtStats q;
			statsIn(q,QS_ADAPTER,a.luid);
			if (nt::ok(k.stats(&q))) {
				ULONG n = std::min<ULONG>(q.ad.segs,64);
				a.aperture.assign(n,0);
				uint64_t apLim = 0,dedLim = 0;
				for (ULONG s = 0; s < n; s++) {
					statsIn(q,QS_SEGMENT,a.luid,nullptr,s);
					if (!nt::ok(k.stats(&q))) continue;
					a.aperture[s] = q.seg.aperture ? 1 : 0;
					(q.seg.aperture ? apLim : dedLim) += q.seg.limit;
				}
				if (a.dedLimit == NA) a.dedLimit = dedLim;
				if (a.sharedLimit == NA) a.sharedLimit = apLim;
			}
			KmtClose c{v[i].h};
			k.close(&c);
			if (render) out.push_back(std::move(a));
		}
		bool real = false;
		for (auto& a : out) if (!a.soft) real = true;
		if (real) out.erase(std::remove_if(out.begin(),out.end(),[](const Ad& a) { return a.soft; }),out.end());
		if (out.empty() && why.empty()) why = "no GPU adapter found";
		return out;
	}

	std::mutex g_adM;
	std::vector<Ad> g_ads;
	std::string g_adWhy;
	uint64_t g_adMs = 0;

	std::vector<Ad> adapters() {
		std::lock_guard<std::mutex> lk(g_adM);
		uint64_t now = util::nowMs();
		if (!g_adMs || now - g_adMs > 10000) {
			g_adWhy.clear();
			g_ads = scanAdapters(g_adWhy);
			g_adMs = now;
		}
		return g_ads;
	}

	// RS3 LTSB had bluescreen reports on these queries, SI stays at RS4 and so does this
	bool supported() {
		struct Ver {
			ULONG size,major,minor,build,plat;
			WCHAR csd[128];
		};
		using VerFn = nt::Status(NTAPI*)(Ver*);
		static auto f = nt::fn<VerFn>("RtlGetVersion");
		if (!f) return false;
		Ver v{};
		v.size = sizeof(v);
		return nt::ok(f(&v)) && (v.major > 10 || (v.major == 10 && v.build >= 17134));
	}

	using OpenQ = PDH_STATUS(WINAPI*)(LPCWSTR,DWORD_PTR,PDH_HQUERY*);
	using AddC = PDH_STATUS(WINAPI*)(PDH_HQUERY,LPCWSTR,DWORD_PTR,PDH_HCOUNTER*);
	using Coll = PDH_STATUS(WINAPI*)(PDH_HQUERY);
	using Arr = PDH_STATUS(WINAPI*)(PDH_HCOUNTER,DWORD,LPDWORD,LPDWORD,PPDH_FMT_COUNTERVALUE_ITEM_W);
	using CloseQ = PDH_STATUS(WINAPI*)(PDH_HQUERY);

	// the counter ids are the trap : Windows id 2 Local Usage is what SI calls dedicated resident, id 4 Dedicated Usage what SI calls dedicated committed
	const wchar_t* const kProcCtr[5] = {
		L"\\GPU Process Memory(*)\\Local Usage",
		L"\\GPU Process Memory(*)\\Non Local Usage",
		L"\\GPU Process Memory(*)\\Dedicated Usage",
		L"\\GPU Process Memory(*)\\Shared Usage",
		L"\\GPU Process Memory(*)\\Total Committed"
	};
	const wchar_t* const kAdCtr[2] = {
		L"\\GPU Adapter Memory(*)\\Dedicated Usage",
		L"\\GPU Adapter Memory(*)\\Shared Usage"
	};

	// job thread only. Loaded on first use from System32, so an exe folder can't plant one.
	struct Perf {
		Arr arr = nullptr;
		Coll coll = nullptr;
		PDH_HQUERY q = nullptr;
		PDH_HCOUNTER pc[5] = {};
		PDH_HCOUNTER ac[2] = {};
		std::string why;
		uint64_t retryMs = 0;

		bool open(uint64_t now) {
			if (q) return true;
			if (retryMs && now < retryMs) return false;
			retryMs = now + 60000;
			HMODULE m = LoadLibraryExW(L"pdh.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
			if (!m) {
				why = "pdh.dll did not load";
				return false;
			}
			auto oq = (OpenQ)(void*)GetProcAddress(m,"PdhOpenQueryW");
			auto ac2 = (AddC)(void*)GetProcAddress(m,"PdhAddEnglishCounterW");
			auto cq = (CloseQ)(void*)GetProcAddress(m,"PdhCloseQuery");
			coll = (Coll)(void*)GetProcAddress(m,"PdhCollectQueryData");
			arr = (Arr)(void*)GetProcAddress(m,"PdhGetFormattedCounterArrayW");
			if (!oq || !ac2 || !cq || !coll || !arr) {
				why = "pdh.dll lacks a needed call";
				return false;
			}
			PDH_HQUERY h = nullptr;
			if (oq(nullptr,0,&h) != ERROR_SUCCESS) {
				why = "PdhOpenQuery failed";
				return false;
			}
			bool good = true;
			for (int i = 0; i < 5; i++) good = good && ac2(h,kProcCtr[i],0,&pc[i]) == ERROR_SUCCESS;
			for (int i = 0; i < 2; i++) good = good && ac2(h,kAdCtr[i],0,&ac[i]) == ERROR_SUCCESS;
			if (!good) {
				cq(h);
				why = "this Windows has no GPU memory counters";
				return false;
			}
			q = h;
			why.clear();
			return true;
		}

		template<class F> bool each(PDH_HCOUNTER c,F f) {
			DWORD sz = 0,n = 0;
			PDH_STATUS st = arr(c,PDH_FMT_LARGE | PDH_FMT_NOSCALE,&sz,&n,nullptr);
			if (st == ERROR_SUCCESS || st == (PDH_STATUS)PDH_NO_DATA || st == (PDH_STATUS)PDH_CSTATUS_NO_INSTANCE) return true; // no instance right now
			if (st != (PDH_STATUS)PDH_MORE_DATA || !sz) return false;
			std::vector<char> b(sz);
			if (arr(c,PDH_FMT_LARGE | PDH_FMT_NOSCALE,&sz,&n,(PPDH_FMT_COUNTERVALUE_ITEM_W)b.data()) != ERROR_SUCCESS) return false;
			auto* it = (PDH_FMT_COUNTERVALUE_ITEM_W*)b.data();
			for (DWORD i = 0; i < n; i++) {
				if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
				if (it[i].FmtValue.largeValue >= 0) f(it[i].szName,(uint64_t)it[i].FmtValue.largeValue);
			}
			return true;
		}
	};

	Perf g_perf;

	Ad* byLuid(std::vector<Ad>& ads,unsigned long hi,unsigned long lo) {
		for (auto& a : ads) if ((unsigned long)a.luid.HighPart == hi && (unsigned long)a.luid.LowPart == lo) return &a;
		return nullptr;
	}

	void addTo(uint64_t& v,uint64_t x) {
		v = (v == NA ? 0 : v) + x;
	}

	GpuAdapter outAd(const Ad& a) {
		GpuAdapter o;
		o.name = a.name;
		o.id = a.id;
		o.dedLimit = a.dedLimit;
		o.sharedLimit = a.sharedLimit;
		o.sysDed = a.sysDed;
		o.installed = a.installed;
		o.reserved = a.reserved;
		return o;
	}

	bool readPerf(GpuSnap& s,std::vector<Ad>& ads) {
		if (!g_perf.open(util::nowMs())) return false;
		if (g_perf.coll(g_perf.q) != ERROR_SUCCESS) {
			g_perf.why = "PdhCollectQueryData failed";
			return false;
		}
		s.source = "performance counters";
		s.commitName = "Total committed";
		std::map<std::string,GpuAdapter> out;
		for (auto& a : ads) {
			GpuAdapter o = outAd(a);
			o.ded = 0;
			o.shared = 0;
			o.sharedOs = 0;
			out[a.id] = o;
		}
		int hits = 0;
		bool good = true;
		for (int i = 0; i < 2; i++) good = g_perf.each(g_perf.ac[i],[&](const wchar_t* nm,uint64_t v) {
			unsigned long hi = 0,lo = 0;
			if (swscanf(nm,L"luid_0x%lx_0x%lx",&hi,&lo) != 2) return;
			Ad* a = byLuid(ads,hi,lo);
			if (!a) return;
			hits++;
			GpuAdapter& o = out[a->id];
			(i == 0 ? o.ded : o.sharedOs) += v;
		}) && good;
		if (!good || !hits) {
			g_perf.why = "the adapter counters returned nothing";
			return false;
		}
		std::map<uint32_t,GpuProc> m;
		for (int i = 0; i < 5; i++) good = g_perf.each(g_perf.pc[i],[&](const wchar_t* nm,uint64_t v) {
			unsigned long pid = 0,hi = 0,lo = 0;
			if (swscanf(nm,L"pid_%lu_luid_0x%lx_0x%lx",&pid,&hi,&lo) != 3) return;
			Ad* a = byLuid(ads,hi,lo);
			if (!a) return;
			GpuProc& p = m[(uint32_t)pid];
			p.pid = (uint32_t)pid;
			uint64_t* f[5] = {&p.ded,&p.shared,&p.dedCommit,&p.sharedCommit,&p.commit};
			addTo(*f[i],v);
			if (i == 1) out[a->id].shared += v;
		}) && good;
		if (!good) {
			g_perf.why = "the process counters failed";
			return false;
		}
		for (auto& a : ads) s.adapters.push_back(out[a.id]);
		for (auto& kv : m) {
			GpuProc& p = kv.second;
			if (p.ded == NA) p.ded = 0;
			if (p.shared == NA) p.shared = 0;
			if (p.dedCommit == NA) p.dedCommit = 0;
			if (p.sharedCommit == NA) p.sharedCommit = 0;
			if (p.commit == NA) p.commit = 0;
			if (p.ded || p.shared || p.dedCommit || p.sharedCommit || p.commit) s.procs.push_back(p);
		}
		return true;
	}

	HANDLE openQuery(uint32_t pid) {
		HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION,FALSE,pid);
		return h ? h : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
	}

	void readKmt(GpuSnap& s,std::vector<Ad>& ads) {
		const Kmt& k = kmt();
		s.source = "D3DKMT";
		s.commitName = "System memory allocated";
		KmtStats q;
		for (auto& a : ads) {
			GpuAdapter o = outAd(a);
			o.ded = 0;
			o.shared = 0;
			bool any = false;
			for (size_t i = 0; i < a.aperture.size(); i++) {
				statsIn(q,QS_SEGMENT,a.luid,nullptr,(ULONG)i);
				if (!nt::ok(k.stats(&q))) continue;
				any = true;
				(q.seg.aperture ? o.shared : o.ded) += q.seg.resident;
			}
			if (!any) o.ded = o.shared = NA;
			s.adapters.push_back(o);
		}
		// Win11 can list who uses an adapter. Without it every process gets asked, slower but the numbers are the same.
		std::set<uint32_t> users;
		bool filtered = k.procList != nullptr;
		if (filtered) for (auto& a : ads) {
			KmtProcList pl{a.luid,nullptr,0};
			nt::Status st = k.procList(&pl);
			if (!nt::ok(st) && st != nt::BUFFER_TOO_SMALL) {
				filtered = false;
				break;
			}
			std::vector<ULONG> ids(pl.n + 16);
			pl.ids = ids.data();
			pl.n = ids.size();
			if (!nt::ok(k.procList(&pl))) {
				filtered = false;
				break;
			}
			for (SIZE_T i = 0; i < pl.n && i < ids.size(); i++) users.insert((uint32_t)ids[i]);
		}
		auto l = procs::latest();
		for (auto& r : l->rows) {
			if (r.pseudo || (filtered && !users.count(r.pid))) continue;
			HANDLE h = openQuery(r.pid);
			if (!h) {
				s.denied.insert(r.pid);
				continue;
			}
			GpuProc p;
			p.pid = r.pid;
			p.ded = p.shared = p.dedCommit = p.sharedCommit = p.commit = 0;
			bool any = false,refused = false;
			for (auto& a : ads) {
				for (ULONG g = GROUP_LOCAL; g <= GROUP_NON_LOCAL; g++) {
					statsIn(q,QS_PROC_GROUP,a.luid,h,g);
					nt::Status st = k.stats(&q);
					if (!nt::ok(st)) {
						refused = refused || st == nt::ACCESS_DENIED;
						continue;
					}
					any = true;
					(g == GROUP_LOCAL ? p.ded : p.shared) += q.grp.usage;
				}
				for (size_t i = 0; i < a.aperture.size(); i++) {
					statsIn(q,QS_PROC_SEGMENT,a.luid,h,(ULONG)i);
					if (nt::ok(k.stats(&q))) (a.aperture[i] ? p.sharedCommit : p.dedCommit) += q.procSegCommitted;
				}
				statsIn(q,QS_PROCESS,a.luid,h);
				if (nt::ok(k.stats(&q))) p.commit += q.pr.alloc;
			}
			CloseHandle(h);
			if (!any && (filtered || refused)) s.denied.insert(r.pid);
			if (p.ded || p.shared || p.dedCommit || p.sharedCommit || p.commit) s.procs.push_back(p);
		}
	}

}

namespace gpumem { namespace be {

	void read(GpuSnap& s,bool perf) {
		if (!supported()) {
			s.why = "GPU memory needs Windows 10 version 1803 or later";
			return;
		}
		std::vector<Ad> ads = adapters();
		if (ads.empty()) {
			std::lock_guard<std::mutex> lk(g_adM);
			s.why = g_adWhy;
			return;
		}
		if (perf && readPerf(s,ads)) return;
		if (perf) s.why = "performance counters failed (" + g_perf.why + "), using D3DKMT";
		readKmt(s,ads);
	}

	GpuDetail detail(const ProcRef& r,bool adapterStats) {
		GpuDetail d;
		if (!supported()) {
			d.why = "needs Windows 10 version 1803 or later";
			return d;
		}
		std::vector<Ad> ads = adapters();
		if (ads.empty()) {
			d.why = "no GPU adapter found";
			return d;
		}
		HANDLE h = openQuery(r.pid);
		if (!h) {
			DWORD e = GetLastError();
			d.gone = e == ERROR_INVALID_PARAMETER;
			d.why = d.gone ? "the process exited" : e == ERROR_ACCESS_DENIED ? "can't open the process, access denied, this needs admin" : "can't open the process";
			return d;
		}
		FILETIME c,x,kt,u;
		if (r.start && GetProcessTimes(h,&c,&x,&kt,&u) && (((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime) != r.start) {
			CloseHandle(h);
			d.gone = true;
			d.why = "the process exited, its pid was reused";
			return d;
		}
		const Kmt& k = kmt();
		KmtStats q;
		nt::Status st = nt::SUCCESS;
		bool any = false;
		for (auto& a : ads) {
			statsIn(q,QS_PROCESS,a.luid,h);
			st = k.stats(&q);
			if (nt::ok(st)) {
				any = true;
				addTo(d.alloc,q.pr.alloc);
				addTo(d.reserved,q.pr.reserved);
				addTo(d.wcAlloc,q.pr.wcAlloc);
				addTo(d.wcReserved,q.pr.wcReserved);
				addTo(d.cachedAlloc,q.pr.cachedAlloc);
				addTo(d.cachedReserved,q.pr.cachedReserved);
				addTo(d.sectionAlloc,q.pr.sectionAlloc);
				addTo(d.sectionReserved,q.pr.sectionReserved);
			}
			if (!adapterStats) continue;
			statsIn(q,QS_PROC_ADAPTER,a.luid,h);
			if (nt::ok(k.stats(&q))) {
				addTo(d.virt,q.pa.virt);
				addTo(d.evicted,q.pa.evicted);
			}
		}
		DWORD code = 0;
		d.gone = !any && GetExitCodeProcess(h,&code) && code != STILL_ACTIVE;
		CloseHandle(h);
		// the kernel answers invalid parameter for a process that never opened a GPU device
		if (d.gone) d.why = "the process exited";
		else if (!any) d.why = st == nt::INVALID_PARAMETER ? "no GPU state, this process has not opened a GPU device" : "D3DKMTQueryStatistics : " + (st == nt::ACCESS_DENIED ? std::string("access denied, this needs admin") : statusTxt(st));
		return d;
	}

} }
#endif
