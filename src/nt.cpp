#ifdef _WIN32
#include "nt.h"
#include <algorithm>

namespace nt {

	void* proc(const char* name) {
		static HMODULE m = GetModuleHandleW(L"ntdll.dll");
		return m ? (void*)GetProcAddress(m,name) : nullptr;
	}

	std::string utf8(const wchar_t* w,int n) {
		if (!w || n <= 0) return "";
		int sz = WideCharToMultiByte(CP_UTF8,0,w,n,nullptr,0,nullptr,nullptr);
		if (sz <= 0) return "";
		std::string s(sz,'\0');
		WideCharToMultiByte(CP_UTF8,0,w,n,&s[0],sz,nullptr,nullptr);
		return s;
	}

	bool sameStart(HANDLE h,uint64_t start) {
		FILETIME c,e,k,u;
		if (!GetProcessTimes(h,&c,&e,&k,&u)) return false;
		return (((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime) == start;
	}

	Status querySys(ULONG cls,std::vector<char>& out) {
		typedef Status(NTAPI* Fn)(ULONG,PVOID,ULONG,PULONG);
		static auto f = fn<Fn>("NtQuerySystemInformation");
		if (!f) return NOT_IMPLEMENTED;
		out.resize(std::max(out.capacity(),(size_t)0x10000));
		bool exact = false;
		for (int i = 0; i < 16; i++) {
			ULONG need = 0;
			Status st = f(cls,out.data(),(ULONG)out.size(),&need);
			if (st != INFO_LENGTH_MISMATCH && st != BUFFER_TOO_SMALL && st != BUFFER_OVERFLOW) {
				if (ok(st) && need > 0 && need <= out.size()) out.resize(need);
				return st;
			}
			// a class that demands an exact size names one smaller than the buffer, growing can never satisfy it
			if (need && need < out.size()) {
				if (exact) return st;
				exact = true;
				out.resize(need);
				continue;
			}
			// resize touches this memory : without a size to go on, stop early
			size_t sz = std::max(out.size() * 2,(size_t)need + need / 8);
			if (sz > ((size_t)1 << (need ? 30 : 28))) return st;
			out.resize(sz);
		}
		return INFO_LENGTH_MISMATCH;
	}

}
#endif // _WIN32
