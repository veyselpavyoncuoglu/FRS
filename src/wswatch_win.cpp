#ifdef _WIN32
#include "wswatch.h"
#include "nt.h"
#include <psapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>

namespace {

	using QipFn = nt::Status(NTAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);
	using SipFn = nt::Status(NTAPI*)(HANDLE,ULONG,PVOID,ULONG);

	const ULONG ProcessWorkingSetWatchEx = 42;
	const nt::Status PORT_ALREADY_SET = (nt::Status)0xC0000048L;

	// PROCESS_WS_WATCH_INFORMATION_EX, phnt ntpsapi.h
	struct WsEx {
		PVOID pc,va;
		HANDLE tid;
		ULONG_PTR flags;
	};
#ifdef _WIN64
	static_assert(sizeof(WsEx) == 32,"WsEx");
#endif

	std::string toU8(const wchar_t* w,int n = -1) {
		int sz = WideCharToMultiByte(CP_UTF8,0,w,n,nullptr,0,nullptr,nullptr);
		if (sz <= 0) return "";
		std::string s(sz,'\0');
		WideCharToMultiByte(CP_UTF8,0,w,n,&s[0],sz,nullptr,nullptr);
		if (n < 0 && !s.empty()) s.pop_back();
		return s;
	}

	std::wstring toW(const std::string& s) {
		int n = MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);
		if (n <= 0) return L"";
		std::wstring w(n,L'\0');
		MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,&w[0],n);
		w.pop_back();
		return w;
	}

	std::string statusTxt(nt::Status st) {
		if (st == nt::ACCESS_DENIED) return "access denied, this needs admin and protected processes refuse even that";
		char t[32];
		snprintf(t,sizeof(t),"status 0x%08lX",(unsigned long)st);
		return t;
	}

	HANDLE openProc(const ProcRef& r,DWORD acc,std::string& why) {
		HANDLE h = OpenProcess(acc | PROCESS_QUERY_LIMITED_INFORMATION,FALSE,r.pid);
		if (!h) {
			DWORD e = GetLastError();
			why = e == ERROR_INVALID_PARAMETER ? "the process exited" : e == ERROR_ACCESS_DENIED ? "can't open the process, access denied, this needs admin and protected processes refuse even that" : "can't open the process, error " + std::to_string(e);
			return nullptr;
		}
		FILETIME c,x,k,u;
		if (r.start && (!GetProcessTimes(h,&c,&x,&k,&u) || (((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime) != r.start)) {
			CloseHandle(h);
			why = "the process exited, its pid was reused";
			return nullptr;
		}
		return h;
	}

	// \Device\HarddiskVolume3\x -> C:\x. Left as it is when no drive matches.
	std::wstring dosPath(const std::wstring& nt) {
		wchar_t drive[3] = L"A:";
		wchar_t dev[MAX_PATH];
		for (wchar_t c = L'A'; c <= L'Z'; c++) {
			drive[0] = c;
			DWORD n = QueryDosDeviceW(drive,dev,MAX_PATH);
			if (!n) continue;
			size_t len = wcslen(dev);
			if (len < nt.size() && nt[len] == L'\\' && !_wcsnicmp(nt.c_str(),dev,len)) return std::wstring(drive) + nt.substr(len);
		}
		return nt;
	}

	struct Drv {
		uint64_t base;
		std::wstring file;
	};

	std::vector<Drv> kernelMods() {
		std::vector<Drv> out;
		std::vector<LPVOID> v(4096);
		DWORD need = 0;
		if (!EnumDeviceDrivers(v.data(),(DWORD)(v.size() * sizeof(LPVOID)),&need)) return out;
		size_t n = std::min(v.size(),(size_t)need / sizeof(LPVOID));
		for (size_t i = 0; i < n; i++) {
			wchar_t w[MAX_PATH];
			if ((uint64_t)v[i] && GetDeviceDriverFileNameW(v[i],w,MAX_PATH)) out.push_back({(uint64_t)v[i],w});
		}
		std::sort(out.begin(),out.end(),[](const Drv& a,const Drv& b) { return a.base < b.base; });
		return out;
	}

	// GetDeviceDriverFileName gives \SystemRoot\x or \??\C:\x
	std::wstring drvPath(const std::wstring& p) {
		if (!_wcsnicmp(p.c_str(),L"\\??\\",4)) return p.substr(4);
		if (_wcsnicmp(p.c_str(),L"\\SystemRoot\\",12)) return p;
		wchar_t w[MAX_PATH];
		UINT n = GetSystemWindowsDirectoryW(w,MAX_PATH);
		return n && n < MAX_PATH ? std::wstring(w,n) + p.substr(11) : p;
	}

	std::string baseName(const std::string& p) {
		size_t i = p.find_last_of("\\/");
		return i == std::string::npos ? p : p.substr(i + 1);
	}

	std::string plus(const std::string& mod,uint64_t off) {
		char t[32];
		snprintf(t,sizeof(t),"+0x%llx",(unsigned long long)off);
		return mod + t;
	}

}

namespace wswatch { namespace be {

	int drain(const ProcRef& r,std::vector<uint64_t>& pcs,uint64_t& lost,std::string& why) {
		static auto qip = nt::fn<QipFn>("NtQueryInformationProcess");
		if (!qip) {
			why = "NtQueryInformationProcess is missing";
			return -1;
		}
		HANDLE h = openProc(r,PROCESS_QUERY_INFORMATION,why);
		if (!h) return -1;
		std::vector<char> buf(0x2000);
		ULONG ret = 0;
		nt::Status st = qip(h,ProcessWorkingSetWatchEx,buf.data(),(ULONG)buf.size(),&ret);
		// the kernel keeps the list until it fits, so nothing is lost here. It can grow between two tries
		for (int i = 0; i < 8 && (st == nt::BUFFER_TOO_SMALL || st == nt::INFO_LENGTH_MISMATCH); i++) {
			buf.resize(std::max((size_t)ret + 0x1000,buf.size() * 2));
			st = qip(h,ProcessWorkingSetWatchEx,buf.data(),(ULONG)buf.size(),&ret);
		}
		CloseHandle(h);
		if (st == nt::UNSUCCESSFUL) return 0;
		if (st == nt::NO_MORE_ENTRIES) return 1;
		if (!nt::ok(st)) {
			why = statusTxt(st);
			return -1;
		}
		size_t n = std::min((size_t)ret,buf.size()) / sizeof(WsEx);
		const WsEx* e = (const WsEx*)buf.data();
		for (size_t i = 0; i < n; i++) {
			if (!e[i].pc) {
				lost += (uint64_t)e[i].va;
				break;
			}
			pcs.push_back((uint64_t)e[i].pc);
		}
		return 1;
	}

	std::string enable(const ProcRef& r) {
		static auto sip = nt::fn<SipFn>("NtSetInformationProcess");
		if (!sip) return "NtSetInformationProcess is missing";
		std::string why;
		HANDLE h = openProc(r,PROCESS_SET_INFORMATION,why);
		if (!h) return why;
		nt::Status st = sip(h,ProcessWorkingSetWatchEx,nullptr,0);
		CloseHandle(h);
		if (st == PORT_ALREADY_SET) return "";
		return nt::ok(st) ? "" : statusTxt(st);
	}

	std::string stop(const ProcRef& r) {
		(void)r;
		return "Windows has no way to turn it off, it stays until the process exits";
	}

	void lookup(const ProcRef& r,std::vector<Row>& rows) {
		std::string why;
		HANDLE h = openProc(r,PROCESS_QUERY_INFORMATION,why);
		std::vector<Drv> drv;
		bool drvDone = false;
		for (auto& x : rows) {
			x.instr = util::hex(x.pc);
			if ((int64_t)x.pc < 0) {
				if (!drvDone) drv = kernelMods();
				drvDone = true;
				const Drv* best = nullptr;
				for (auto& d : drv) if (d.base <= x.pc) best = &d;
				if (best) {
					x.file = util::safe(toU8(drvPath(best->file).c_str()));
					x.instr = util::safe(plus(baseName(x.file),x.pc - best->base));
				}
				continue;
			}
			if (!h) continue;
			MEMORY_BASIC_INFORMATION mi;
			if (!VirtualQueryEx(h,(LPCVOID)x.pc,&mi,sizeof(mi)) || mi.Type != MEM_IMAGE) continue;
			wchar_t w[1024];
			DWORD n = GetMappedFileNameW(h,mi.AllocationBase,w,1024);
			if (!n) continue;
			x.file = util::safe(toU8(dosPath(std::wstring(w,n)).c_str()));
			x.instr = plus(baseName(x.file),x.pc - (uint64_t)mi.AllocationBase);
		}
		if (h) CloseHandle(h);
	}

	bool permanent() {
		return true;
	}

	void openLoc(const std::string& file) {
		wchar_t w[MAX_PATH];
		UINT n = GetSystemWindowsDirectoryW(w,MAX_PATH);
		if (!n || n >= MAX_PATH) return;
		std::wstring exe = std::wstring(w,n) + L"\\explorer.exe";
		std::wstring args = L"/select,\"" + toW(file) + L"\"";
		ShellExecuteW(nullptr,L"open",exe.c_str(),args.c_str(),nullptr,SW_SHOWNORMAL);
	}

} }
#endif
