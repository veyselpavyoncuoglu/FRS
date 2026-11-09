#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ntdll calls without an import lib. Everything stays inside this namespace so it never fights winternl.h.
// Status names have no STATUS_ prefix on purpose, winnt.h and ntstatus.h define those as macros.
namespace nt {

	using Status = LONG;

	constexpr Status SUCCESS = 0;
	constexpr Status BUFFER_OVERFLOW = (Status)0x80000005L;
	constexpr Status PARTIAL_COPY = (Status)0x8000000DL;
	constexpr Status NO_MORE_ENTRIES = (Status)0x8000001AL;
	constexpr Status UNSUCCESSFUL = (Status)0xC0000001L;
	constexpr Status NOT_IMPLEMENTED = (Status)0xC0000002L;
	constexpr Status INVALID_INFO_CLASS = (Status)0xC0000003L;
	constexpr Status INFO_LENGTH_MISMATCH = (Status)0xC0000004L;
	constexpr Status ACCESS_VIOLATION = (Status)0xC0000005L;
	constexpr Status INVALID_HANDLE = (Status)0xC0000008L;
	constexpr Status INVALID_PARAMETER = (Status)0xC000000DL;
	constexpr Status NO_MEMORY = (Status)0xC0000017L;
	constexpr Status ACCESS_DENIED = (Status)0xC0000022L;
	constexpr Status BUFFER_TOO_SMALL = (Status)0xC0000023L;
	constexpr Status PRIVILEGE_NOT_HELD = (Status)0xC0000061L;
	constexpr Status NOT_SUPPORTED = (Status)0xC00000BBL;
	constexpr Status PROCESS_IS_TERMINATING = (Status)0xC000010AL;

	inline bool ok(Status s) { return s >= 0; }

	void* proc(const char* name); // ntdll export, null when this Windows lacks it

	// typed lookup. Callers cache it : static auto f = nt::fn<MyFn>("NtXxx");
	template<class F> F fn(const char* name) { return reinterpret_cast<F>(proc(name)); }

	// NtQuerySystemInformation into out, grows it on length mismatch. out.size() is the returned length on success.
	Status querySys(ULONG cls,std::vector<char>& out);

	std::string utf8(const wchar_t* w,int n); // n wide chars to UTF-8, "" on junk
	bool sameStart(HANDLE h,uint64_t start); // GetProcessTimes creation time is start, so the pid was not reused

	struct USTR {
		USHORT Length,MaximumLength;
		PWSTR Buffer;
	};

	struct CID {
		HANDLE UniqueProcess,UniqueThread;
	};

	// SYSTEM_THREAD_INFORMATION, phnt ntexapi.h
	struct STI {
		LARGE_INTEGER KernelTime,UserTime,CreateTime;
		ULONG WaitTime;
		PVOID StartAddress;
		CID ClientId;
		LONG Priority,BasePriority;
		ULONG ContextSwitches;
		ULONG ThreadState,WaitReason;
	};

	// SYSTEM_PROCESS_INFORMATION, phnt ntexapi.h. Walk with NextEntryOffset, Threads[NumberOfThreads] follows.
	// PeakWorkingSetSize is bytes, the phnt comment saying kilobytes is wrong.
	struct SPI {
		ULONG NextEntryOffset,NumberOfThreads;
		ULONGLONG WorkingSetPrivateSize;
		ULONG HardFaultCount,NumberOfThreadsHighWatermark;
		ULONGLONG CycleTime;
		LARGE_INTEGER CreateTime,UserTime,KernelTime;
		USTR ImageName;
		LONG BasePriority;
		HANDLE UniqueProcessId,InheritedFromUniqueProcessId;
		ULONG HandleCount,SessionId;
		ULONG_PTR UniqueProcessKey;
		SIZE_T PeakVirtualSize,VirtualSize;
		ULONG PageFaultCount;
		SIZE_T PeakWorkingSetSize,WorkingSetSize;
		SIZE_T QuotaPeakPagedPoolUsage,QuotaPagedPoolUsage,QuotaPeakNonPagedPoolUsage,QuotaNonPagedPoolUsage;
		SIZE_T PagefileUsage,PeakPagefileUsage,PrivatePageCount;
		LARGE_INTEGER ReadOperationCount,WriteOperationCount,OtherOperationCount;
		LARGE_INTEGER ReadTransferCount,WriteTransferCount,OtherTransferCount;
		STI Threads[1];
	};

#ifdef _WIN64
	static_assert(sizeof(USTR) == 0x10,"USTR");
	static_assert(sizeof(STI) == 0x50,"STI");
	static_assert(offsetof(SPI,WorkingSetPrivateSize) == 0x08,"SPI");
	static_assert(offsetof(SPI,HardFaultCount) == 0x10,"SPI");
	static_assert(offsetof(SPI,CycleTime) == 0x18,"SPI");
	static_assert(offsetof(SPI,CreateTime) == 0x20,"SPI");
	static_assert(offsetof(SPI,ImageName) == 0x38,"SPI");
	static_assert(offsetof(SPI,BasePriority) == 0x48,"SPI");
	static_assert(offsetof(SPI,UniqueProcessId) == 0x50,"SPI");
	static_assert(offsetof(SPI,InheritedFromUniqueProcessId) == 0x58,"SPI");
	static_assert(offsetof(SPI,HandleCount) == 0x60,"SPI");
	static_assert(offsetof(SPI,SessionId) == 0x64,"SPI");
	static_assert(offsetof(SPI,UniqueProcessKey) == 0x68,"SPI");
	static_assert(offsetof(SPI,PeakVirtualSize) == 0x70,"SPI");
	static_assert(offsetof(SPI,PageFaultCount) == 0x80,"SPI");
	static_assert(offsetof(SPI,PeakWorkingSetSize) == 0x88,"SPI");
	static_assert(offsetof(SPI,WorkingSetSize) == 0x90,"SPI");
	static_assert(offsetof(SPI,QuotaPagedPoolUsage) == 0xA0,"SPI");
	static_assert(offsetof(SPI,QuotaNonPagedPoolUsage) == 0xB0,"SPI");
	static_assert(offsetof(SPI,PagefileUsage) == 0xB8,"SPI");
	static_assert(offsetof(SPI,PrivatePageCount) == 0xC8,"SPI");
	static_assert(offsetof(SPI,ReadOperationCount) == 0xD0,"SPI");
	static_assert(offsetof(SPI,Threads) == 0x100,"SPI");
#endif

}
#endif // _WIN32
