#ifdef _WIN32
#include "vmscan.h"
#include "nt.h"
#include <dbghelp.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

	using namespace vmscan;

	typedef nt::Status(NTAPI* ReadFn)(HANDLE,PVOID,PVOID,SIZE_T,PSIZE_T);
	typedef nt::Status(NTAPI* QueryVmFn)(HANDLE,PVOID,ULONG,PVOID,SIZE_T,PSIZE_T);
	typedef nt::Status(NTAPI* QueryProcFn)(HANDLE,ULONG,PVOID,ULONG,PULONG);

	// phnt ntpsapi.h
	struct BulkHdr {
		ULONG QueryFlags,NumberOfEntries;
		PVOID NextValidAddress;
	};
	static_assert(sizeof(BulkHdr) == 16,"BulkHdr");
	typedef nt::Status(NTAPI* BulkFn)(HANDLE,PVOID,BulkHdr*,SIZE_T,PSIZE_T);

	const nt::Status MORE_ENTRIES = 0x105;
	const ULONG ProcessProtectionInformation = 61;

	std::wstring toW(const std::string& s) {
		int n = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.c_str(),(int)s.size(),nullptr,0);
		if (n <= 0) return L"";
		std::wstring w((size_t)n,L'\0');
		MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.c_str(),(int)s.size(),&w[0],n);
		return w;
	}

	std::string toU8(const wchar_t* w) {
		int n = WideCharToMultiByte(CP_UTF8,0,w,-1,nullptr,0,nullptr,nullptr);
		if (n <= 1) return "";
		std::string s((size_t)n,'\0');
		WideCharToMultiByte(CP_UTF8,0,w,-1,&s[0],n,nullptr,nullptr);
		s.resize((size_t)n - 1);
		return s;
	}

	std::string winErr(DWORD e) {
		wchar_t* b = nullptr;
		DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,e,0,(LPWSTR)&b,0,nullptr);
		std::string s;
		if (n && b) s = toU8(b);
		if (b) LocalFree(b);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) s.pop_back();
		if (s.empty()) {
			char t[32];
			snprintf(t,sizeof(t),"error 0x%lx",(unsigned long)e);
			s = t;
		}
		return util::safe(s);
	}

	bool dead(HANDLE h) {
		DWORD c = 0;
		return GetExitCodeProcess(h,&c) && c != STILL_ACTIVE;
	}

	// a call just failed on a file that is marked for delete, like a dump still being written. Win32 only says access denied
	bool delPending() {
		typedef nt::Status(NTAPI* LastFn)();
		static auto f = nt::fn<LastFn>("RtlGetLastNtStatus");
		return f && f() == (nt::Status)0xC0000056L;
	}

	bool setDelete(HANDLE h,BOOL on) {
		FILE_DISPOSITION_INFO d;
		d.DeleteFile = on;
		return SetFileInformationByHandle(h,FileDispositionInfo,&d,sizeof(d)) != 0;
	}

	// a dump holds passwords, only this user and SYSTEM get in, like 0600 on Linux. Protected, so the folder's ACEs don't come along
	struct OwnerOnly {
		SECURITY_DESCRIPTOR sd;
		DWORD tu[64],acl[128],sys[32];
		bool make() {
			HANDLE t = nullptr;
			if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&t)) return false;
			DWORD n = 0,sn = sizeof(sys);
			BOOL ok = GetTokenInformation(t,TokenUser,tu,sizeof(tu),&n);
			CloseHandle(t);
			PACL a = (PACL)acl;
			return ok && CreateWellKnownSid(WinLocalSystemSid,nullptr,sys,&sn) && InitializeAcl(a,sizeof(acl),ACL_REVISION) &&
				AddAccessAllowedAce(a,ACL_REVISION,FILE_ALL_ACCESS,((TOKEN_USER*)tu)->User.Sid) && AddAccessAllowedAce(a,ACL_REVISION,FILE_ALL_ACCESS,sys) &&
				InitializeSecurityDescriptor(&sd,SECURITY_DESCRIPTOR_REVISION) && SetSecurityDescriptorDacl(&sd,TRUE,a,FALSE) &&
				SetSecurityDescriptorControl(&sd,SE_DACL_PROTECTED,SE_DACL_PROTECTED);
		}
	};

	bool isProtected(HANDLE h) {
		static auto q = nt::fn<QueryProcFn>("NtQueryInformationProcess");
		UCHAR lvl = 0;
		return q && nt::ok(q(h,ProcessProtectionInformation,&lvl,1,nullptr)) && (lvl & 7) != 0;
	}

	// why an open failed, in words that say what to do
	std::string openWhy(uint32_t pid,DWORD e,const char* what) {
		if (e == ERROR_INVALID_PARAMETER) return "the process exited";
		if (e == ERROR_ACCESS_DENIED) {
			HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
			bool pr = h && isProtected(h);
			if (h) CloseHandle(h);
			if (pr) return std::string("protected process, ") + what + " needs the signed driver";
			return "access denied, the process belongs to another user or needs FRS running as admin";
		}
		return winErr(e);
	}

	bool sameStart(HANDLE h,const ProcRef& r) {
		if (!r.start) return true;
		FILETIME c,e,k,u;
		if (!GetProcessTimes(h,&c,&e,&k,&u)) return false;
		return (((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime) == r.start;
	}

	struct CloseH {
		HANDLE h;
		~CloseH() { if (h) CloseHandle(h); }
	};

	int kindOf(DWORD type) {
		return type == MEM_IMAGE ? KImage : type == MEM_MAPPED ? KMapped : KPrivate;
	}

	void readableReg(const MEMORY_BASIC_INFORMATION& m,int& skipped,std::vector<Region>& out) {
		if (m.State != MEM_COMMIT) return;
		if (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
			skipped++;
			return;
		}
		Region g;
		g.base = (uint64_t)m.BaseAddress;
		g.size = (uint64_t)m.RegionSize;
		g.prot = m.Protect;
		g.kind = kindOf(m.Type);
		out.push_back(g);
	}

	// ---- minidump ----

	typedef BOOL(WINAPI* WriteFn)(HANDLE,DWORD,HANDLE,MINIDUMP_TYPE,const PMINIDUMP_EXCEPTION_INFORMATION,const PMINIDUMP_USER_STREAM_INFORMATION,const PMINIDUMP_CALLBACK_INFORMATION);

	WriteFn dumpFn() {
		static HMODULE m = LoadLibraryExW(L"dbghelp.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
		return m ? (WriteFn)GetProcAddress(m,"MiniDumpWriteDump") : nullptr;
	}

	// dbghelp is single threaded
	std::mutex g_dbg;

	// a dump thread's end : running, its caller walked away, the file was kept. First one wins, so a stop can't leave a whole dump behind
	enum { JRun,JLeft,JKept };

	struct Ctx {
		Prog* pg;
		const std::atomic<int>* st;
		bool snap = false,cancelled = false;
		int mods = 0,thr = 0,mem = 0;
	};

	typedef nt::Status(NTAPI* ResumeFn)(HANDLE);

	// the pid dbghelp holds right now. Windows 11 lets go of its threads when the writer dies (thread state change handles, measured),
	// before that it has only SuspendThread and the target would stay frozen after FRS is gone. A crash can't be helped
	std::atomic<DWORD> g_live{0};

	void resumeLive() {
		static auto res = nt::fn<ResumeFn>("NtResumeProcess");
		DWORD pid = g_live.load();
		if (!pid || !res || nt::proc("NtCreateThreadStateChange")) return;
		HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME,FALSE,pid);
		if (!h) return;
		res(h);
		CloseHandle(h);
	}

	// MinGW's header has Status outside the union, in dbghelp it sits at offset 0 with the rest
	void setStatus(PMINIDUMP_CALLBACK_OUTPUT out,HRESULT hr) {
		memcpy((void*)out,&hr,sizeof(hr));
	}

	BOOL CALLBACK dumpCb(PVOID par,const PMINIDUMP_CALLBACK_INPUT in,PMINIDUMP_CALLBACK_OUTPUT out) {
		Ctx& c = *(Ctx*)par;
		switch (in->CallbackType) {
			case CancelCallback:
				if (c.st->load() == JLeft) {
					c.cancelled = true;
					out->Cancel = TRUE;
				}
				out->CheckCancel = TRUE;
				break;
			case IsProcessSnapshotCallback:
				if (c.snap) setStatus(out,S_FALSE);
				break;
			case ReadMemoryFailureCallback:
				setStatus(out,S_OK);
				break;
			case ModuleCallback: {
				if (c.mods < 25) c.mods++;
				std::string nm;
				if (in->Module.FullPath) {
					const wchar_t* p = in->Module.FullPath;
					const wchar_t* s = wcsrchr(p,L'\\');
					nm = util::safe(toU8(s ? s + 1 : p));
				}
				c.pg->say("Processing module " + nm + "...",(5 + c.mods) / 100.0f);
				break;
			}
			case ThreadCallback:
			case ThreadExCallback:
				if (c.thr < 20) c.thr++;
				c.pg->say("Processing thread " + std::to_string(in->Thread.ThreadId) + "...",(30 + c.thr) / 100.0f);
				break;
			case IncludeVmRegionCallback:
				if (c.mem < 40) c.mem++;
				c.pg->say("Processing memory regions...",(50 + c.mem) / 100.0f);
				break;
			default:
				break;
		}
		return TRUE;
	}

	// keeps a Store app from being suspended while the dump runs, taskmgr does the same. Best effort.
	struct PkgTask {
		struct V {
			void* qi;
			ULONG(STDMETHODCALLTYPE* AddRef)(void*);
			ULONG(STDMETHODCALLTYPE* Release)(void*);
			HRESULT(STDMETHODCALLTYPE* BeginTask)(void*,ULONG,ULONG);
			HRESULT(STDMETHODCALLTYPE* BeginTaskByHandle)(void*,HANDLE,ULONG);
			HRESULT(STDMETHODCALLTYPE* EndTask)(void*);
		};
		struct Obj { V* v; };
		struct FV {
			void* qi;
			ULONG(STDMETHODCALLTYPE* AddRef)(void*);
			ULONG(STDMETHODCALLTYPE* Release)(void*);
			HRESULT(STDMETHODCALLTYPE* CreateInstance)(void*,void*,const GUID&,void**);
		};
		struct Fact { FV* v; };
		Obj* o = nullptr;

		static bool packaged(HANDLE h) {
			typedef LONG(WINAPI* Fn)(HANDLE,UINT32*,PWSTR);
			static Fn f = (Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetPackageFullName");
			UINT32 n = 0;
			return f && f(h,&n,nullptr) == ERROR_INSUFFICIENT_BUFFER;
		}

		HRESULT begin(HANDLE h) {
			static const GUID clsid = {0x07fc2b94,0x5285,0x417e,{0x8a,0xc3,0xc2,0xce,0x52,0x40,0xb0,0xfa}};
			static const GUID iid = {0xc7e40572,0xc36a,0x43ea,{0x9a,0x40,0xf3,0xb1,0x68,0xda,0x55,0x58}};
			static const GUID factIid = {0x00000001,0x0000,0x0000,{0xc0,0,0,0,0,0,0,0x46}};
			HMODULE m = LoadLibraryExW(L"twinapi.appcore.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
			typedef HRESULT(STDMETHODCALLTYPE* GetFn)(const GUID&,const GUID&,void**);
			GetFn get = m ? (GetFn)GetProcAddress(m,"DllGetClassObject") : nullptr;
			if (!get) return E_NOINTERFACE;
			Fact* f = nullptr;
			HRESULT hr = get(clsid,factIid,(void**)&f);
			if (FAILED(hr)) return hr;
			hr = f->v->CreateInstance(f,nullptr,iid,(void**)&o);
			f->v->Release(f);
			if (FAILED(hr)) {
				o = nullptr;
				return hr;
			}
			hr = o->v->BeginTaskByHandle(o,h,0x10000);
			if (FAILED(hr)) {
				o->v->Release(o);
				o = nullptr;
			}
			return hr;
		}

		~PkgTask() {
			if (!o) return;
			o->v->EndTask(o);
			o->v->Release(o);
		}
	};

	// ---- process snapshot ----

	typedef nt::Status(NTAPI* PssCapFn)(PHANDLE,HANDLE,ULONG,ULONG);
	typedef nt::Status(NTAPI* PssFreeFn)(HANDLE);

	const ULONG PSSFLAGS = 0x1 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x800 | 0x1000 | 0x4000000 | 0x8000000 | 0x20000000;

	struct PssSnap {
		HANDLE s = nullptr;
		nt::Status take(HANDLE h) {
			static auto cap = nt::fn<PssCapFn>("PssNtCaptureSnapshot");
			if (!cap) return nt::NOT_IMPLEMENTED;
			nt::Status st = cap(&s,h,PSSFLAGS,CONTEXT_ALL);
			if (!nt::ok(st)) s = nullptr;
			return st;
		}
		// the snapshot lives in FRS. This free also closes its clone and trace section (measured). SI frees through the target's handle,
		// which fails and leaks 3 handles, and closes those two first, which with a working free is a double close
		~PssSnap() {
			if (!s) return;
			static auto fr = nt::fn<PssFreeFn>("PssNtFreeSnapshot");
			nt::Status st = fr ? fr(s) : nt::NOT_IMPLEMENTED;
			if (nt::ok(st)) return;
			char t[64];
			snprintf(t,sizeof(t),"[!] Dump : snapshot not freed, status 0x%08lx",(unsigned long)st);
			appLog(t);
		}
	};

}

namespace vmscan {

	bool dumpAvail() {
		return true;
	}

	namespace os {

		struct Proc {
			HANDLE h = nullptr;
			~Proc() { if (h) CloseHandle(h); }
		};

		std::shared_ptr<Proc> open(const ProcRef& r,std::string& err) {
			HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,FALSE,r.pid);
			if (!h) {
				err = openWhy(r.pid,GetLastError(),"reading it");
				return nullptr;
			}
			auto p = std::make_shared<Proc>();
			p->h = h;
			if (!sameStart(h,r)) {
				err = "pid " + std::to_string(r.pid) + " was reused, this is another process now";
				return nullptr;
			}
			return p;
		}

		bool regions(Proc& p,std::vector<Region>& out,int& skipped,std::string& err) {
			out.clear();
			skipped = 0;
			static auto bulk = nt::fn<BulkFn>("NtPssCaptureVaSpaceBulk");
			if (bulk) {
				std::vector<char> b(sizeof(BulkHdr) + sizeof(MEMORY_BASIC_INFORMATION) * 256);
				BulkHdr* hd = (BulkHdr*)b.data();
				hd->QueryFlags = 1;
				hd->NextValidAddress = nullptr;
				bool ok = true;
				for (;;) {
					nt::Status st = bulk(p.h,hd->NextValidAddress,hd,b.size(),nullptr);
					if (!nt::ok(st)) {
						ok = false;
						break;
					}
					auto* m = (MEMORY_BASIC_INFORMATION*)(b.data() + sizeof(BulkHdr));
					for (ULONG i = 0; i < hd->NumberOfEntries; i++) readableReg(m[i],skipped,out);
					if (st != MORE_ENTRIES) break;
				}
				if (ok && !out.empty()) return true;
				out.clear();
				skipped = 0;
			}
			static auto q = nt::fn<QueryVmFn>("NtQueryVirtualMemory");
			if (!q) {
				err = "NtQueryVirtualMemory is missing";
				return false;
			}
			uint64_t a = 0;
			bool any = false;
			for (;;) {
				MEMORY_BASIC_INFORMATION m;
				nt::Status st = q(p.h,(PVOID)a,0,&m,sizeof(m),nullptr);
				if (!nt::ok(st)) {
					if (!any) {
						if (dead(p.h)) {
							err = "the process exited";
							return false;
						}
						char t[64];
						snprintf(t,sizeof(t),"memory walk failed, status 0x%08lx",(unsigned long)st);
						err = t;
						return false;
					}
					break;
				}
				any = true;
				readableReg(m,skipped,out);
				uint64_t nx = (uint64_t)m.BaseAddress + (uint64_t)m.RegionSize;
				if (nx <= a) break;
				a = nx;
			}
			return true;
		}

		size_t read(Proc& p,uint64_t addr,void* buf,size_t n) {
			static auto f = nt::fn<ReadFn>("NtReadVirtualMemory");
			if (!f) return 0;
			SIZE_T got = 0;
			nt::Status st = f(p.h,(PVOID)addr,buf,n,&got);
			if (nt::ok(st)) return n;
			return st == nt::PARTIAL_COPY ? (size_t)got : 0;
		}

		bool exited(Proc& p) {
			return dead(p.h);
		}

		bool regionAt(Proc& p,uint64_t addr,Region& out,std::string& err) {
			static auto q = nt::fn<QueryVmFn>("NtQueryVirtualMemory");
			MEMORY_BASIC_INFORMATION m;
			nt::Status st = q ? q(p.h,(PVOID)addr,0,&m,sizeof(m),nullptr) : nt::NOT_IMPLEMENTED;
			if (!nt::ok(st)) {
				err = dead(p.h) ? "the process exited" : "address " + util::hex(addr) + " is outside the process's address space";
				return false;
			}
			if (m.State != MEM_COMMIT) {
				err = "address " + util::hex(addr) + " is not committed memory";
				return false;
			}
			out.base = (uint64_t)m.BaseAddress;
			out.size = (uint64_t)m.RegionSize;
			out.prot = m.Protect;
			out.kind = kindOf(m.Type);
			return true;
		}

		std::string protStr(uint32_t pr) {
			if (!pr) return "";
			std::string s = pr & PAGE_NOACCESS ? "NA" : pr & PAGE_READONLY ? "R" : pr & PAGE_READWRITE ? "RW" : pr & PAGE_WRITECOPY ? "WC" :
				pr & PAGE_EXECUTE ? "X" : pr & PAGE_EXECUTE_READ ? "RX" : pr & PAGE_EXECUTE_READWRITE ? "RWX" : pr & PAGE_EXECUTE_WRITECOPY ? "WCX" : "?";
			if (pr & PAGE_GUARD) s += "+G";
			if (pr & PAGE_NOCACHE) s += "+NC";
			if (pr & PAGE_WRITECOMBINE) s += "+WCM";
			return s;
		}

		struct Out {
			HANDLE h = INVALID_HANDLE_VALUE;
			bool marked = false;
			~Out() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
		};

		std::shared_ptr<Out> create(const std::string& path,std::string& err) {
			std::wstring w = toW(path);
			if (w.empty()) {
				err = "path is not valid text";
				return nullptr;
			}
			OwnerOnly o;
			if (!o.make()) {
				err = "no owner only ACL for the file, " + winErr(GetLastError()) + ". Nothing was written";
				return nullptr;
			}
			SECURITY_ATTRIBUTES sa = {sizeof(sa),&o.sd,FALSE};
			HANDLE h = CreateFileW(w.c_str(),GENERIC_WRITE | DELETE,FILE_SHARE_READ | FILE_SHARE_DELETE,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				DWORD e = GetLastError();
				if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS || delPending()) err = "the file already exists, nothing was written. Pick another name";
				else err = winErr(e);
				return nullptr;
			}
			auto f = std::make_shared<Out>();
			f->h = h;
			if (GetFileType(h) != FILE_TYPE_DISK) {
				err = "that name is a device, not a file";
				return nullptr;
			}
			// the kernel deletes it when the handle goes, also on exit and crash. Only close takes the mark off, a file of half a process's memory stays nowhere
			f->marked = setDelete(h,TRUE);
			if (!f->marked) appLog("[!] Dump : no delete mark on the file, this disk refused it. A crash would leave it behind");
			return f;
		}

		bool write(Out& f,const void* p,size_t n,std::string& err) {
			const char* c = (const char*)p;
			while (n) {
				DWORD w = 0;
				DWORD k = (DWORD)std::min(n,(size_t)(1 << 26));
				if (!WriteFile(f.h,c,k,&w,nullptr) || !w) {
					err = winErr(GetLastError());
					return false;
				}
				c += w;
				n -= w;
			}
			return true;
		}

		uint64_t close(Out& f,std::string& err) {
			LARGE_INTEGER s;
			uint64_t r = GetFileSizeEx(f.h,&s) ? (uint64_t)s.QuadPart : NA;
			if (f.marked && !setDelete(f.h,FALSE)) err = "the file could not be kept, " + winErr(GetLastError());
			CloseHandle(f.h);
			f.h = INVALID_HANDLE_VALUE;
			return r;
		}

		void discard(Out& f) {
			setDelete(f.h,TRUE);
			CloseHandle(f.h);
			f.h = INVALID_HANDLE_VALUE;
		}

		bool exists(const std::string& p) {
			return GetFileAttributesW(toW(p).c_str()) != INVALID_FILE_ATTRIBUTES || delPending();
		}

		bool pathOk(const std::string& p,std::string& err) {
			err = "path must be a full file path like C:\\Users\\me\\Desktop\\x.dmp";
			if (p.size() < 4) return false;
			char d = p[0];
			if (!((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z')) || p[1] != ':' || (p[2] != '\\' && p[2] != '/')) return false;
			if (p.back() == '\\' || p.back() == '/' || p.back() == '.' || p.back() == ' ') return false;
			for (size_t i = 2; i < p.size(); i++) {
				unsigned char c = (unsigned char)p[i];
				if (c < 32 || c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
					err = "path has a character Windows does not allow in a file name, or a stream name";
					return false;
				}
				bool sep = c == '\\' || c == '/';
				if (sep && p[i + 1] == '.' && p[i + 2] == '.' && (i + 3 >= p.size() || p[i + 3] == '\\' || p[i + 3] == '/')) {
					err = "path can't contain ..";
					return false;
				}
			}
			if (toW(p).empty()) {
				err = "path is not valid text";
				return false;
			}
			err.clear();
			return true;
		}

		std::string defaultDir() {
			wchar_t b[MAX_PATH] = L"";
			if (SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_DESKTOPDIRECTORY,nullptr,0,b)) && b[0]) return toU8(b);
			const wchar_t* e = _wgetenv(L"USERPROFILE");
			return e && *e ? toU8(e) : "C:\\";
		}

		char sep() {
			return '\\';
		}

		uint64_t freeDisk(const std::string& path) {
			std::wstring w = toW(path);
			size_t k = w.find_last_of(L"\\/");
			if (k == std::wstring::npos) return NA;
			w.resize(k + 1);
			ULARGE_INTEGER fr;
			if (!GetDiskFreeSpaceExW(w.c_str(),&fr,nullptr,nullptr)) return NA;
			return fr.QuadPart;
		}

	}

	namespace {

		// all the dump thread touches. dbghelp only asks about cancel between regions and one region can take minutes,
		// so the caller waits in short steps and can walk away, the thread cleans up after itself
		struct DumpCall {
			HANDLE h = nullptr;
			DWORD pid = 0;
			uint32_t flags = 0;
			bool snap = false;
			std::shared_ptr<os::Out> f;
			Prog pg;
			std::atomic<int> st{JRun};
			std::atomic<bool> done{false};
			DumpRes res;
			~DumpCall() { if (h) CloseHandle(h); }
		};

		void dumpThread(DumpCall& j) {
			DumpRes& res = j.res;
			Prog& pg = j.pg;
			HANDLE h = j.h;
			os::Out& f = *j.f;
			std::unique_lock<std::mutex> lk(g_dbg,std::defer_lock);
			pg.say("Waiting for another dump to finish...",0.01f);
			while (!lk.try_lock()) {
				if (j.st.load() == JLeft) {
					os::discard(f);
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
			pg.say("Initializing the minidump...",0.05f);
			PkgTask pkg;
			if (PkgTask::packaged(h)) {
				HRESULT hr = pkg.begin(h);
				if (FAILED(hr)) {
					char t[96];
					snprintf(t,sizeof(t),"Store app task not set (0x%08lx), it may be suspended during the dump",(unsigned long)hr);
					res.note = t;
				}
			}
			PssSnap sn;
			if (j.snap) {
				nt::Status st = sn.take(h);
				if (!nt::ok(st)) {
					char t[96];
					snprintf(t,sizeof(t),"No process snapshot (0x%08lx), dumped the live process",(unsigned long)st);
					res.note += (res.note.empty() ? "" : ". ") + std::string(t);
				}
			}
			Ctx cx;
			cx.pg = &pg;
			cx.st = &j.st;
			cx.snap = sn.s != nullptr;
			MINIDUMP_CALLBACK_INFORMATION cb;
			cb.CallbackRoutine = (MINIDUMP_CALLBACK_ROUTINE)dumpCb;
			cb.CallbackParam = &cx;
			// an old dbghelp refuses flags it doesn't know, retry without the newest ones
			static const uint32_t drop[] = {0x400000 | 0x800000,0x200000,0x40000 | 0x80000 | 0x100000};
			WriteFn wr = dumpFn();
			uint32_t fl = j.flags;
			int d = 0;
			BOOL ok = FALSE;
			DWORD e = 0;
			for (;;) {
				if (!sn.s) g_live = j.pid;
				ok = wr(sn.s ? sn.s : h,j.pid,f.h,(MINIDUMP_TYPE)fl,nullptr,nullptr,&cb);
				if (!ok) e = GetLastError();
				g_live = 0;
				if (ok) break;
				if (cx.cancelled || (e != (DWORD)E_INVALIDARG && e != ERROR_INVALID_PARAMETER)) break;
				while (d < 3 && !(fl & drop[d])) d++;
				if (d >= 3) break;
				fl &= ~drop[d++];
				LARGE_INTEGER z;
				z.QuadPart = 0;
				SetFilePointerEx(f.h,z,nullptr,FILE_BEGIN);
				SetEndOfFile(f.h);
			}
			pg.say("Finalizing the minidump...",0.95f);
			int run = JRun;
			if (!ok || !j.st.compare_exchange_strong(run,JKept)) {
				os::discard(f);
				res.err = dead(h) ? "the process exited" : winErr(e);
				return;
			}
			res.flags = fl;
			if (fl != j.flags) res.note += (res.note.empty() ? "" : ". ") + std::string("This dbghelp refused some flags, they were left out");
			std::string err;
			res.size = os::close(f,err);
			if (!err.empty()) {
				res.err = err;
				return;
			}
			res.ok = true;
			pg.say("Minidump complete.",1.0f);
		}

	}

	DumpRes writeDump(const ProcRef& r,const std::string& path,uint32_t flags,bool snap,Prog& pg,const StopFn& stop) {
		DumpRes res;
		res.path = path;
		res.flags = flags;
		std::string err;
		if (r.pid == GetCurrentProcessId()) {
			res.err = "FRS can't dump itself, dbghelp would freeze every thread of the process that writes the dump";
			return res;
		}
		if (!os::pathOk(path,err)) {
			res.err = err;
			return res;
		}
		if (!dumpFn()) {
			res.err = "dbghelp.dll could not be loaded from System32";
			return res;
		}
		// least access, the handle lives in the dump thread. Handle data needs dup handle, the snapshot's clone needs create process
		const DWORD ladder[] = {
			PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE | PROCESS_VM_READ | (snap ? PROCESS_CREATE_PROCESS : 0u),
			PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE | PROCESS_VM_READ,
			PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
			PROCESS_QUERY_INFORMATION
		};
		HANDLE h = nullptr;
		DWORD why = 0;
		for (DWORD a : ladder) {
			h = OpenProcess(a,FALSE,r.pid);
			if (h) break;
			why = GetLastError();
		}
		if (!h) {
			res.err = openWhy(r.pid,why,"a dump of it");
			return res;
		}
		CloseH hc{h};
		if (!sameStart(h,r)) {
			res.err = "pid " + std::to_string(r.pid) + " was reused, this is another process now";
			return res;
		}
		if (isProtected(h)) {
			res.err = "protected process, a dump of it needs the signed driver";
			return res;
		}
		auto f = os::create(path,err);
		if (!f) {
			res.err = err;
			return res;
		}
		static int regExit = atexit(resumeLive);
		(void)regExit;
		auto j = std::make_shared<DumpCall>();
		j->h = h;
		hc.h = nullptr;
		j->pid = r.pid;
		j->flags = flags;
		j->snap = snap;
		j->f = f;
		j->res = res;
		try {
			std::thread([j] {
				try {
					dumpThread(*j);
				} catch (std::exception& e) {
					j->res.err = e.what();
				}
				j->done = true;
			}).detach();
		} catch (std::exception& e) {
			os::discard(*f);
			res.err = util::safe(e.what());
			return res;
		}
		auto mirror = [&] { pg.say(j->pg.text(),j->pg.frac.load()); };
		while (!j->done.load()) {
			const char* w = stop ? stop() : nullptr;
			int run = JRun;
			if (w && j->st.compare_exchange_strong(run,JLeft)) {
				// dbghelp is often between regions and lets go at once
				for (int i = 0; i < 15 && !j->done.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
				res.cancelled = true;
				res.err = w;
				if (!j->done.load()) res.note = "dbghelp is inside a big region and lets go at its next check. Until then the process can stay frozen and the partial file is locked, then it deletes itself, when FRS exits at the latest";
				return res;
			}
			mirror();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		mirror();
		return j->res;
	}

}
#endif // _WIN32
