#include "ram.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <sstream>

RamStats getRamStats() {
	MEMORYSTATUSEX ms = {};
	ms.dwLength = sizeof(ms);
	GlobalMemoryStatusEx(&ms);
	RamStats r;
	r.total = ms.ullTotalPhys / (1024*1024);
	r.used  = (ms.ullTotalPhys - ms.ullAvailPhys) / (1024*1024);
	r.free  = ms.ullAvailPhys / (1024*1024);
	return r;
}

std::string flushStandby(int szMB) {
	size_t sz = (size_t)szMB * 1024 * 1024;
	void* buf = VirtualAlloc(nullptr,sz,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
	if (!buf) return "FlushStandby : VirtualAlloc failed";
	memset(buf,0,sz);
	VirtualFree(buf,0,MEM_RELEASE);
	std::ostringstream ss;
	ss << "FlushStandby : flushed " << szMB << " MB";
	return ss.str();
}

std::string emptyWorkSets() {
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
	if (snap == INVALID_HANDLE_VALUE) return "EmptyWorkSets : snapshot failed";
	PROCESSENTRY32 pe = {};
	pe.dwSize = sizeof(pe);
	int n = 0;
	if (Process32First(snap,&pe)) {
		do {
			HANDLE ph = OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_SET_QUOTA,FALSE,pe.th32ProcessID);
			if (ph) {
				EmptyWorkingSet(ph);
				CloseHandle(ph);
				n++;
			}
		} while (Process32Next(snap,&pe));
	}
	CloseHandle(snap);
	std::ostringstream ss;
	ss << "EmptyWorkSets : trimmed " << n << " processes";
	return ss.str();
}

std::string purgeStandby() {
	typedef LONG(WINAPI* NtSI_t)(UINT,PVOID,ULONG);
	auto fn = (NtSI_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetSystemInformation");
	if (!fn) return "PurgeStandby : ntdll lookup failed";
	UINT cmd = 4; // MemoryPurgeStandbyList
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
	UINT cmd = 3; // MemoryFlushModifiedList
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
		if (GetTokenInformation(tok,TokenElevation,&te,sizeof(te),&sz))
			elev = te.TokenIsElevated;
		CloseHandle(tok);
	}
	return elev != FALSE;
}