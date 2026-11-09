#include "ram.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <sstream>
#include <thread>
#include <vector>

RamStats getRamStats() {
	MEMORYSTATUSEX ms = {};
	ms.dwLength = sizeof(ms);
	GlobalMemoryStatusEx(&ms);
	RamStats r;
	r.total = ms.ullTotalPhys / (1024 * 1024);
	r.used = (ms.ullTotalPhys - ms.ullAvailPhys) / (1024 * 1024);
	r.free = ms.ullAvailPhys / (1024 * 1024);
	return r;
}

std::vector<std::pair<std::string,uint64_t>> getMemInfo() {
	std::vector<std::pair<std::string,uint64_t>> v;
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	uint64_t pg = si.dwPageSize;
	auto mb = [&](uint64_t pages) { return pages * pg / (1024 * 1024); };

	// SystemMemoryListInformation (80)
	struct {
		ULONG_PTR zero,free,modified,modifiedNoWrite,bad;
		ULONG_PTR standby[8];
		ULONG_PTR repurposed[8];
		ULONG_PTR modifiedPageFile;
	} ml = {};
	typedef LONG(WINAPI* NtQSI_t)(UINT,PVOID,ULONG,PULONG);
	auto fn = (NtQSI_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation");
	if (fn && fn(80,&ml,sizeof(ml),nullptr) == 0) {
		uint64_t sb = 0;
		for (auto n : ml.standby) sb += n;
		v.push_back({"standby_mb",mb(sb)});
		v.push_back({"modified_mb",mb(ml.modified + ml.modifiedNoWrite)});
		v.push_back({"free_list_mb",mb(ml.free + ml.zero)});
	}

	PERFORMANCE_INFORMATION pi = {};
	pi.cb = sizeof(pi);
	if (GetPerformanceInfo(&pi,sizeof(pi))) {
		v.push_back({"commit_mb",mb(pi.CommitTotal)});
		v.push_back({"commit_limit_mb",mb(pi.CommitLimit)});
		v.push_back({"system_cache_mb",mb(pi.SystemCache)});
		v.push_back({"kernel_paged_mb",mb(pi.KernelPaged)});
		v.push_back({"kernel_nonpaged_mb",mb(pi.KernelNonpaged)});
		v.push_back({"processes",pi.ProcessCount});
		v.push_back({"threads",pi.ThreadCount});
		v.push_back({"handles",pi.HandleCount});
	}
	return v;
}

std::vector<ProcMem> getProcs() {
	std::vector<ProcMem> v;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
	if (snap == INVALID_HANDLE_VALUE) return v;
	PROCESSENTRY32W pe = {};
	pe.dwSize = sizeof(pe);
	if (Process32FirstW(snap,&pe)) {
		do {
			HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pe.th32ProcessID);
			if (!ph) continue;
			PROCESS_MEMORY_COUNTERS_EX pmc = {};
			if (GetProcessMemoryInfo(ph,(PROCESS_MEMORY_COUNTERS*)&pmc,sizeof(pmc))) {
				char nm[MAX_PATH * 3];
				WideCharToMultiByte(CP_UTF8,0,pe.szExeFile,-1,nm,sizeof(nm),nullptr,nullptr);
				v.push_back({pe.th32ProcessID,nm,pmc.WorkingSetSize,pmc.PrivateUsage});
			}
			CloseHandle(ph);
		} while (Process32NextW(snap,&pe));
	}
	CloseHandle(snap);
	return v;
}

std::string flushStandby(int sz) {
	unsigned int nThreads = std::thread::hardware_concurrency();
	if (nThreads < 1) nThreads = 1;
	size_t total = (size_t)sz * 1024 * 1024;
	size_t chunk = total / nThreads;
	if (chunk == 0) chunk = total;
	std::vector<std::thread> threads;
	struct Joiner { std::vector<std::thread>& v; ~Joiner() { for (auto& t : v) if (t.joinable()) t.join(); } } joiner{threads};
	for (unsigned int i = 0; i < nThreads; i++) {
		size_t bytes = (i == nThreads - 1) ? (total - chunk * i) : chunk;
		threads.emplace_back([bytes]() {
			void* buf = VirtualAlloc(nullptr,bytes,MEM_COMMIT | MEM_RESERVE,PAGE_READWRITE);
			if (!buf) return;
			memset(buf,0,bytes);
			VirtualFree(buf,0,MEM_RELEASE);
		});
	}
	for (auto& t : threads) t.join();
	std::ostringstream ss;
	ss << "FlushStandby : flushed " << sz << " MB across " << nThreads << " threads";
	return ss.str();
}

std::string emptyWorkSets() {
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
	if (snap == INVALID_HANDLE_VALUE) return "EmptyWorkSets : snapshot failed";
	std::vector<DWORD> pids;
	PROCESSENTRY32 pe = {};
	pe.dwSize = sizeof(pe);
	if (Process32First(snap,&pe)) {
		do { pids.push_back(pe.th32ProcessID); } while (Process32Next(snap,&pe));
	}
	CloseHandle(snap);
	unsigned int nThreads = std::thread::hardware_concurrency();
	if (nThreads < 1) nThreads = 1;
	if (nThreads > (unsigned int)pids.size()) nThreads = (unsigned int)pids.size();
	std::vector<int> counts(nThreads,0);
	std::vector<std::thread> threads;
	struct Joiner { std::vector<std::thread>& v; ~Joiner() { for (auto& t : v) if (t.joinable()) t.join(); } } joiner{threads};
	for (unsigned int i = 0; i < nThreads; i++) {
		threads.emplace_back([&pids,&counts,i,nThreads]() {
			for (size_t j = i; j < pids.size(); j += nThreads) {
				HANDLE ph = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA,FALSE,pids[j]);
				if (ph) {
					EmptyWorkingSet(ph);
					CloseHandle(ph);
					counts[i]++;
				}
			}
		});
	}
	for (auto& t : threads) t.join();
	int total = 0;
	for (int c : counts) total += c;
	std::ostringstream ss;
	ss << "EmptyWorkSets : trimmed " << total << " processes across " << nThreads << " threads";
	return ss.str();
}

std::string purgeStandby() {
	typedef LONG(WINAPI* NtSI_t)(UINT,PVOID,ULONG);
	auto fn = (NtSI_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetSystemInformation");
	if (!fn) return "PurgeStandby : ntdll lookup failed";
	UINT cmd = 4;
	LONG r = fn(80,&cmd,sizeof(cmd));
	if (r != 0) {
		std::ostringstream ss;
		ss << "PurgeStandby : NTSTATUS 0x" << std::hex << (unsigned)r;
		return ss.str();
	}
	return "PurgeStandby : OK";
}

std::string flushModified() {
	typedef LONG(WINAPI* NtSI_t)(UINT,PVOID,ULONG);
	auto fn = (NtSI_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetSystemInformation");
	if (!fn) return "FlushModified : ntdll lookup failed";
	UINT cmd = 3;
	LONG r = fn(80,&cmd,sizeof(cmd));
	if (r != 0) {
		std::ostringstream ss;
		ss << "FlushModified : NTSTATUS 0x" << std::hex << (unsigned)r;
		return ss.str();
	}
	return "FlushModified : OK";
}

bool isElevated() {
	BOOL elev = FALSE;
	HANDLE tok;
	if (OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&tok)) {
		TOKEN_ELEVATION te;
		DWORD sz;
		if (GetTokenInformation(tok,TokenElevation,&te,sizeof(te),&sz)) {
			elev = te.TokenIsElevated;
		}
		CloseHandle(tok);
	}
	return elev != FALSE;
}
#endif // _WIN32