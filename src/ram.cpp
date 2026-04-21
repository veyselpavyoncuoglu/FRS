#include "ram.h"
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

std::string flushStandby(int sz) {
	unsigned int nThreads = std::thread::hardware_concurrency();
	if (nThreads < 1) nThreads = 1;
	size_t total = (size_t)sz * 1024 * 1024;
	size_t chunk = total / nThreads;
	if (chunk == 0) chunk = total;
	std::vector<std::thread> threads;
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