#ifdef _WIN32
#include "platform.h"
#include "util.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <sddl.h>
#include <taskschd.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

	void enablePriv(LPCWSTR name) {
		HANDLE tok;
		if (!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,&tok)) return;
		LUID luid;
		if (LookupPrivilegeValueW(nullptr,name,&luid)) {
			TOKEN_PRIVILEGES tp = {};
			tp.PrivilegeCount = 1;
			tp.Privileges[0].Luid = luid;
			tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
			AdjustTokenPrivileges(tok,FALSE,&tp,sizeof(tp),nullptr,nullptr);
		}
		CloseHandle(tok);
	}

	std::string utf8(const std::wstring& w) {
		int n = WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),nullptr,0,nullptr,nullptr);
		std::string s(n > 0 ? n : 0,'\0');
		if (n > 0) WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),&s[0],n,nullptr,nullptr);
		return s;
	}

	std::wstring exePath() {
		wchar_t b[MAX_PATH];
		DWORD n = GetModuleFileNameW(nullptr,b,MAX_PATH);
		return std::wstring(b,n < MAX_PATH ? n : 0);
	}

	bool hasArg(const wchar_t* sw) {
		int n = 0;
		LPWSTR* a = CommandLineToArgvW(GetCommandLineW(),&n);
		if (!a) return false;
		bool r = false;
		for (int i = 1; i < n; i++) if ((a[i][0] == L'-' || a[i][0] == L'/') && !_wcsicmp(a[i] + 1,sw)) r = true;
		LocalFree(a);
		return r;
	}

	std::string g_note;
	bool g_portSw = false; // portable only through -portable, the tasks have to pass it on

	// FRS_CONFIG_DIR in a test build, next to the exe in portable mode when that folder takes a file, else %APPDATA%\FRS
	std::wstring baseDir() {
		const char* e = util::testEnv("FRS_CONFIG_DIR");
		if (e && *e) return _wgetenv(L"FRS_CONFIG_DIR");
		wchar_t buf[MAX_PATH] = L"";
		SHGetFolderPathW(nullptr,CSIDL_APPDATA,nullptr,0,buf);
		std::wstring app = std::wstring(buf) + L"\\FRS";
		std::wstring ex = exePath();
		size_t sl = ex.find_last_of(L"\\/");
		ex = sl == std::wstring::npos ? L"" : ex.substr(0,sl);
		bool mark = !ex.empty() && GetFileAttributesW((ex + L"\\FRS.portable").c_str()) != INVALID_FILE_ATTRIBUTES;
		if (ex.empty() || (!mark && !hasArg(L"portable"))) return app;
		HANDLE h = CreateFileW((ex + L"\\FRS.wtest").c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			g_note = "Portable mode : failed (can't write next to FRS.exe), settings stay in " + utf8(app);
			return app;
		}
		CloseHandle(h);
		g_portSw = !mark;
		return ex;
	}

	std::wstring cfgDir() {
		static const std::wstring dir = baseDir();
		for (size_t i = 1; i <= dir.size(); i++) {
			if (i == dir.size() || dir[i] == L'\\' || dir[i] == L'/') CreateDirectoryW(dir.substr(0,i).c_str(),nullptr);
		}
		return dir;
	}

	std::wstring cfgPath(const char* name) {
		wchar_t w[MAX_PATH];
		w[0] = 0;
		MultiByteToWideChar(CP_UTF8,0,name,-1,w,MAX_PATH);
		return cfgDir() + L"\\" + w;
	}

	// a test build works on its own task only, never the user's FRS one
	std::wstring taskName() {
#ifdef FRS_TEST
		const char* e = util::testEnv("FRS_TEST_TASK");
		wchar_t w[128] = L"FRS-test-notask";
		if (e && *e) MultiByteToWideChar(CP_UTF8,0,e,-1,w,128);
		return w;
#else
		return L"FRS";
#endif
	}

	bool runHidden(std::wstring cmd) {
		SECURITY_ATTRIBUTES sa = {};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;
		HANDLE nul = CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
		STARTUPINFOW si = {};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
		si.wShowWindow = SW_HIDE;
		si.hStdOutput = nul;
		si.hStdError = nul;
		PROCESS_INFORMATION pi = {};
		bool ok = CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi) != 0;
		if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
		if (!ok) return false;
		WaitForSingleObject(pi.hProcess,5000);
		DWORD code = 1;
		GetExitCodeProcess(pi.hProcess,&code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		return code == 0;
	}

	std::string errText(HRESULT hr) {
		wchar_t* m = nullptr;
		FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,(DWORD)hr,0,(LPWSTR)&m,0,nullptr);
		std::wstring w = m ? m : L"";
		if (m) LocalFree(m);
		while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ' || w.back() == L'.')) w.pop_back();
		char b[16];
		snprintf(b,sizeof(b),"0x%08lX",(unsigned long)hr);
		return w.empty() ? b : util::safe(utf8(w)) + " " + b;
	}

	std::wstring userSid() {
		HANDLE tok;
		if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&tok)) return L"";
		std::vector<BYTE> b(512);
		DWORD n = 0;
		std::wstring r;
		LPWSTR s = nullptr;
		if (GetTokenInformation(tok,TokenUser,b.data(),(DWORD)b.size(),&n) && ConvertSidToStringSidW(((TOKEN_USER*)b.data())->User.Sid,&s)) {
			r = s;
			LocalFree(s);
		}
		CloseHandle(tok);
		return r;
	}

	std::wstring xmlEsc(const std::wstring& s) {
		std::wstring o;
		for (wchar_t c : s) o += c == L'&' ? L"&amp;" : c == L'<' ? L"&lt;" : c == L'>' ? L"&gt;" : std::wstring(1,c);
		return o;
	}

	// logon : the Run at startup task, else the on demand one behind the no-prompt shortcut. Highest run level is what /rl highest gave,
	// normal priority, no battery stop and no 3 day limit
	std::wstring taskXml(bool logon) {
		std::wstring sid = userSid();
		const wchar_t* lvl = L"HighestAvailable";
#ifdef FRS_TEST
		if (util::testEnv("FRS_TEST_TASKLOW")) lvl = L"LeastPrivilege";
#endif
		std::wstring x = L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n";
		x += std::wstring(L"<RegistrationInfo><Description>") + (logon ? L"Starts FRS when you log on." : L"Starts FRS as admin from the FRS (no prompt) shortcut.") + L"</Description></RegistrationInfo>\r\n";
		if (logon) x += L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + sid + L"</UserId></LogonTrigger></Triggers>\r\n";
		x += L"<Principals><Principal id=\"Author\"><UserId>" + sid + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>" + lvl + L"</RunLevel></Principal></Principals>\r\n";
		x += std::wstring(L"<Settings><MultipleInstancesPolicy>") + (logon ? L"IgnoreNew" : L"Parallel") + L"</MultipleInstancesPolicy>"
			L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
			L"<AllowStartOnDemand>true</AllowStartOnDemand><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority></Settings>\r\n";
		cfgDir();
		x += L"<Actions Context=\"Author\"><Exec><Command>\"" + xmlEsc(exePath()) + L"\"</Command>" + (g_portSw ? L"<Arguments>-portable</Arguments>" : L"") + L"</Exec></Actions>\r\n</Task>\r\n";
#ifdef FRS_TEST
		if (util::testEnv("FRS_TEST_TASK")) {
			std::string raw("\xFF\xFE",2);
			raw.append((const char*)x.data(),x.size() * sizeof(wchar_t));
			plat::writeFile(logon ? "task_startup.xml" : "task_noprompt.xml",raw);
		}
#endif
		return x;
	}

	// the root task folder, the caller releases both
	HRESULT taskRoot(ITaskService*& svc,ITaskFolder*& fld) {
		VARIANT e;
		VariantInit(&e);
		BSTR root = SysAllocString(L"\\");
		HRESULT hr = root ? CoCreateInstance(__uuidof(TaskScheduler),nullptr,CLSCTX_INPROC_SERVER,__uuidof(ITaskService),(void**)&svc) : E_OUTOFMEMORY;
		if (SUCCEEDED(hr)) hr = svc->Connect(e,e,e,e);
		if (SUCCEEDED(hr)) hr = svc->GetFolder(root,&fld);
		SysFreeString(root);
		return hr;
	}

	// straight through the Task Scheduler API : an XML file in a folder the unelevated user can write could be swapped before schtasks reads it
	bool regTask(const std::wstring& name,const std::wstring& xml,std::string* why) {
		HRESULT co = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
		ITaskService* svc = nullptr;
		ITaskFolder* fld = nullptr;
		IRegisteredTask* t = nullptr;
		VARIANT e;
		VariantInit(&e);
		BSTR bn = SysAllocString(name.c_str());
		BSTR bx = SysAllocString(xml.c_str());
		HRESULT hr = bn && bx ? taskRoot(svc,fld) : E_OUTOFMEMORY;
		if (SUCCEEDED(hr)) hr = fld->RegisterTask(bn,bx,TASK_CREATE_OR_UPDATE,e,e,TASK_LOGON_INTERACTIVE_TOKEN,e,&t);
		if (t) t->Release();
		if (fld) fld->Release();
		if (svc) svc->Release();
		SysFreeString(bn);
		SysFreeString(bx);
		if (SUCCEEDED(co)) CoUninitialize();
		if (FAILED(hr) && why) *why = errText(hr);
		return SUCCEEDED(hr);
	}

	bool taskExists(const std::wstring& name) {
		HRESULT co = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
		ITaskService* svc = nullptr;
		ITaskFolder* fld = nullptr;
		IRegisteredTask* t = nullptr;
		BSTR bn = SysAllocString(name.c_str());
		if (bn && SUCCEEDED(taskRoot(svc,fld))) fld->GetTask(bn,&t);
		bool r = t != nullptr;
		if (t) t->Release();
		if (fld) fld->Release();
		if (svc) svc->Release();
		SysFreeString(bn);
		if (SUCCEEDED(co)) CoUninitialize();
		return r;
	}

	// a test build keeps it in its own settings folder, never on the Desktop
	std::wstring linkPath() {
#ifdef FRS_TEST
		return cfgDir() + L"\\FRS (no prompt).lnk";
#else
		PWSTR p = nullptr;
		std::wstring d;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&p))) d = p;
		CoTaskMemFree(p);
		return d.empty() ? d : d + L"\\FRS (no prompt).lnk";
#endif
	}

	std::wstring npTask() {
		return taskName() + L" NoPrompt";
	}

	bool makeLink(const std::wstring& lnk,std::string& why) {
		HRESULT co = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
		IShellLinkW* sl = nullptr;
		IPersistFile* pf = nullptr;
		wchar_t sys[MAX_PATH] = L"";
		GetSystemDirectoryW(sys,MAX_PATH);
		HRESULT hr = CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,(void**)&sl);
		if (SUCCEEDED(hr)) {
			sl->SetPath((std::wstring(sys) + L"\\schtasks.exe").c_str());
			sl->SetArguments((L"/run /tn \"" + npTask() + L"\"").c_str());
			sl->SetShowCmd(SW_SHOWMINNOACTIVE);
			sl->SetIconLocation(exePath().c_str(),0);
			sl->SetDescription(L"Starts FRS as admin without the UAC prompt");
			hr = sl->QueryInterface(IID_IPersistFile,(void**)&pf);
		}
		if (SUCCEEDED(hr)) hr = pf->Save(lnk.c_str(),TRUE);
		if (pf) pf->Release();
		if (sl) sl->Release();
		if (SUCCEEDED(co)) CoUninitialize();
		if (FAILED(hr)) why = errText(hr);
		return SUCCEEDED(hr);
	}

	// the token the user's normal programs run with : the linked one behind an elevated FRS, else FRS's own
	HANDLE userTok() {
		HANDLE tok = nullptr,r = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY | TOKEN_DUPLICATE,&tok)) return nullptr;
		TOKEN_ELEVATION_TYPE t = TokenElevationTypeDefault;
		TOKEN_LINKED_TOKEN lt = {};
		DWORD n = 0;
		if (GetTokenInformation(tok,TokenElevationType,&t,sizeof(t),&n) && t == TokenElevationTypeFull) {
			if (GetTokenInformation(tok,TokenLinkedToken,&lt,sizeof(lt),&n)) r = lt.LinkedToken;
		} else if (!DuplicateToken(tok,SecurityIdentification,&r)) r = nullptr;
		CloseHandle(tok);
		return r;
	}

	// what tok may do to the file or folder, everything when that can't be found out
	DWORD rights(HANDLE tok,const std::wstring& path) {
		SECURITY_INFORMATION si = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
		DWORD n = 0;
		GetFileSecurityW(path.c_str(),si,nullptr,0,&n);
		std::vector<BYTE> sd(n ? n : 1);
		GENERIC_MAPPING gm = {FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};
		BYTE ps[256];
		DWORD psn = sizeof(ps),got = 0;
		BOOL ok = FALSE;
		if (!n || !GetFileSecurityW(path.c_str(),si,sd.data(),n,&n) || !AccessCheck(sd.data(),tok,MAXIMUM_ALLOWED,&gm,(PRIVILEGE_SET*)ps,&psn,&got,&ok)) return ~0u;
		return ok ? got : 0;
	}

	// "" above the root
	std::wstring parentDir(const std::wstring& p) {
		size_t sl = p.find_last_of(L'\\');
		if (sl == std::wstring::npos || sl + 1 == p.size()) return L"";
		return p.substr(0,sl == 2 && p[1] == L':' ? 3 : sl);
	}

	// nothing the user runs unelevated can change the exe, add to its folder or swap a folder on the way up. C:\ lets users add folders, that alone is fine
	bool safeExe(HANDLE tok,std::wstring p) {
		if (!tok || p.empty()) return false;
		for (int lv = 0; !p.empty(); lv++,p = parentDir(p)) {
			DWORD bad = lv == 0 ? FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER
				: lv == 1 ? FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER
				: FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;
			if (rights(tok,p) & bad) return false;
		}
		return true;
	}

	// one FRS per settings folder : the hash names the mutex and tags the window
	uint64_t dirHash() {
		wchar_t b[MAX_PATH] = L"";
		DWORD n = GetFullPathNameW(cfgDir().c_str(),MAX_PATH,b,nullptr);
		std::wstring d = n && n < MAX_PATH ? b : cfgDir();
		for (auto& c : d) if (c == L'/') c = L'\\';
		if (!d.empty()) CharLowerBuffW(&d[0],(DWORD)d.size());
		while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
		uint64_t h = 1469598103934665603ULL;
		for (wchar_t c : d) {
			h ^= (uint64_t)c;
			h *= 1099511628211ULL;
		}
		return h | 1;
	}

}

namespace plat {

	void raisePrivileges() {
		enablePriv(SE_DEBUG_NAME);
		enablePriv(L"SeProfileSingleProcessPrivilege");
		enablePriv(L"SeIncreaseQuotaPrivilege");
		enablePriv(L"SeIncreaseWorkingSetPrivilege");
		enablePriv(L"SeIncreaseBasePriorityPrivilege");
	}

	std::string readFile(const char* name) {
		FILE* f = nullptr;
		_wfopen_s(&f,cfgPath(name).c_str(),L"rb");
		if (!f) return "";
		std::string out;
		char buf[4096];
		size_t n;
		while ((n = fread(buf,1,sizeof(buf),f)) > 0) out.append(buf,n);
		fclose(f);
		return out;
	}

	void writeFile(const char* name,const std::string& text) {
		FILE* f = nullptr;
		_wfopen_s(&f,cfgPath(name).c_str(),L"wb");
		if (!f) return;
		fwrite(text.data(),1,text.size(),f);
		fclose(f);
	}

	// shared both ways : a log viewer, a second FRS (-clean) or a scan may have it open. A rotate that can't move it skips this time
	bool appendFile(const char* name,const std::string& text,size_t maxBytes) {
		std::wstring p = cfgPath(name);
		WIN32_FILE_ATTRIBUTE_DATA a;
		if (GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&a)) {
			uint64_t sz = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
			if (sz && sz + text.size() > maxBytes) MoveFileExW(p.c_str(),(p + L".1").c_str(),MOVEFILE_REPLACE_EXISTING);
		}
		HANDLE h = CreateFileW(p.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;
		DWORD n = 0;
		bool ok = WriteFile(h,text.data(),(DWORD)text.size(),&n,nullptr) && n == text.size();
		return CloseHandle(h) && ok;
	}

	std::string filePath(const char* name) { return utf8(cfgPath(name)); }

	std::string dirNote() {
		cfgDir();
		return g_note;
	}

	std::string readSettings() { return readFile("settings.ini"); }
	void writeSettings(const std::string& text) { writeFile("settings.ini",text); }

	bool firstInstance(bool keep) {
		char b[40];
		snprintf(b,sizeof(b),"Local\\FRS-%016llx",(unsigned long long)dirHash());
		HANDLE m = CreateMutexA(nullptr,FALSE,b);
		DWORD e = GetLastError();
		bool first = !(e == ERROR_ALREADY_EXISTS || (!m && e == ERROR_ACCESS_DENIED));
		// our handle would keep the other one's mutex alive after it exits
		if (m && (!keep || !first)) CloseHandle(m);
		return first;
	}

	// a test exe never raises another window, it only looks. wParam 1 : that one opens its Memory page
	bool showOther(bool memPage) {
		uint64_t h[3] = {dirHash(),memPage ? 1u : 0u,util::testEnv("FRS_TEST_NOACTIVATE") ? 0u : 1u};
		EnumWindows([](HWND w,LPARAM lp) -> BOOL {
			uint64_t* q = (uint64_t*)lp;
			wchar_t c[8] = L"";
			if (!GetClassNameW(w,c,8) || wcscmp(c,L"FRS") || GetPropW(w,L"FRS") != (HANDLE)(uintptr_t)q[0]) return TRUE;
			if (q[2]) {
				DWORD pid = 0;
				GetWindowThreadProcessId(w,&pid);
				AllowSetForegroundWindow(pid);
				PostMessageW(w,RegisterWindowMessageW(L"FRS_SHOW"),(WPARAM)q[1],0);
			}
			q[2] = 2;
			return FALSE;
		},(LPARAM)h);
		return h[2] == 2;
	}

	void leaving() {}

	void markMain(void* hwnd) {
		SetPropW((HWND)hwnd,L"FRS",(HANDLE)(uintptr_t)dirHash());
	}

	bool startupSupported() { return true; }

	bool startupEnabled() {
		return runHidden(L"schtasks /query /tn \"" + taskName() + L"\" /fo LIST");
	}

	bool setStartupEnabled(bool on,std::string* why) {
		if (on) return regTask(taskName(),taskXml(true),why);
		bool ok = runHidden(L"schtasks /delete /tn \"" + taskName() + L"\" /f");
		if (!ok && why) *why = "schtasks /delete did not succeed";
		return ok;
	}

	// older FRS made the task with schtasks /sc onlogon : logon triggers, one command with no arguments (or -portable), the logged on user.
	// Only its settings change, the command it runs stays, a copy started from elsewhere never takes it over
	int upgradeStartup(std::string& why) {
		HRESULT co = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
		ITaskService* svc = nullptr;
		ITaskFolder* fld = nullptr;
		IRegisteredTask* t = nullptr;
		IRegisteredTask* nt = nullptr;
		ITaskDefinition* def = nullptr;
		ITriggerCollection* tc = nullptr;
		IActionCollection* ac = nullptr;
		IAction* a = nullptr;
		IExecAction* ex = nullptr;
		IPrincipal* pr = nullptr;
		ITaskSettings* ts = nullptr;
		BSTR bn = SysAllocString(taskName().c_str());
		BSTR lim = SysAllocString(L"PT0S");
		BSTR args = nullptr;
		VARIANT e;
		VariantInit(&e);
		LONG nTr = 0,nAc = 0;
		TASK_LOGON_TYPE lt = TASK_LOGON_NONE;
		HRESULT hr = bn && lim ? taskRoot(svc,fld) : E_OUTOFMEMORY;
		if (SUCCEEDED(hr)) hr = fld->GetTask(bn,&t);
		if (SUCCEEDED(hr)) hr = t->get_Definition(&def);
		if (SUCCEEDED(hr)) hr = def->get_Triggers(&tc);
		if (SUCCEEDED(hr)) hr = tc->get_Count(&nTr);
		if (SUCCEEDED(hr)) hr = def->get_Actions(&ac);
		if (SUCCEEDED(hr)) hr = ac->get_Count(&nAc);
		if (SUCCEEDED(hr)) hr = def->get_Principal(&pr);
		bool own = SUCCEEDED(hr) && nTr > 0 && nAc == 1;
		for (LONG i = 1; own && i <= nTr; i++) {
			ITrigger* tr = nullptr;
			TASK_TRIGGER_TYPE2 ty = TASK_TRIGGER_EVENT;
			own = SUCCEEDED(tc->get_Item(i,&tr)) && SUCCEEDED(tr->get_Type(&ty)) && ty == TASK_TRIGGER_LOGON;
			if (tr) tr->Release();
		}
		own = own && SUCCEEDED(ac->get_Item(1,&a)) && SUCCEEDED(a->QueryInterface(__uuidof(IExecAction),(void**)&ex)) && SUCCEEDED(ex->get_Arguments(&args)) && (!args || !*args || !_wcsicmp(args,L"-portable"));
		own = own && SUCCEEDED(pr->get_LogonType(&lt)) && lt == TASK_LOGON_INTERACTIVE_TOKEN;
		int r = -1;
		if (FAILED(hr)) why = errText(hr);
		else if (!own) {
			why = "not the logon task FRS makes, its triggers, command and settings stay as they are";
			r = 0;
		} else {
			hr = def->get_Settings(&ts);
			if (SUCCEEDED(hr)) hr = ts->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
			if (SUCCEEDED(hr)) hr = ts->put_StopIfGoingOnBatteries(VARIANT_FALSE);
			if (SUCCEEDED(hr)) hr = ts->put_ExecutionTimeLimit(lim);
			if (SUCCEEDED(hr)) hr = ts->put_Priority(4);
			if (SUCCEEDED(hr)) hr = fld->RegisterTaskDefinition(bn,def,TASK_UPDATE,e,e,lt,e,&nt);
			if (SUCCEEDED(hr)) r = 1;
			else why = errText(hr);
		}
		for (IUnknown* u : {(IUnknown*)ts,(IUnknown*)pr,(IUnknown*)ex,(IUnknown*)a,(IUnknown*)ac,(IUnknown*)tc,(IUnknown*)def,(IUnknown*)nt,(IUnknown*)t,(IUnknown*)fld,(IUnknown*)svc}) if (u) u->Release();
		SysFreeString(args);
		SysFreeString(lim);
		SysFreeString(bn);
		if (SUCCEEDED(co)) CoUninitialize();
		return r;
	}

	bool exeInSafeDir() {
		HANDLE tok = userTok();
		bool r = safeExe(tok,exePath());
		if (tok) CloseHandle(tok);
		return r;
	}

	bool noPromptSupported() { return true; }

	// the shortcut deleted by hand leaves the task, Remove still has to take it
	bool noPromptOn() {
		std::wstring l = linkPath();
		return (!l.empty() && GetFileAttributesW(l.c_str()) != INVALID_FILE_ATTRIBUTES) || taskExists(npTask());
	}

	std::string noPromptLink() { return utf8(linkPath()); }

	bool setNoPrompt(bool on,std::string& why) {
		std::wstring lnk = linkPath();
		if (lnk.empty()) {
			why = "no Desktop folder";
			return false;
		}
		if (!on) {
			bool ok = DeleteFileW(lnk.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
			if (!ok) why = errText(HRESULT_FROM_WIN32(GetLastError()));
			if (!runHidden(L"schtasks /delete /tn \"" + npTask() + L"\" /f") && taskExists(npTask())) {
				if (ok) why = "schtasks /delete did not succeed";
				ok = false;
			}
			return ok;
		}
		if (!regTask(npTask(),taskXml(false),&why)) return false;
		if (makeLink(lnk,why)) return true;
		runHidden(L"schtasks /delete /tn \"" + npTask() + L"\" /f");
		return false;
	}

	const char* osName() { return "Windows"; }

	// an elevated token with an unelevated twin : the user's normal programs can write %APPDATA%\FRS
	bool rootOnUserFiles() {
		static bool v = [] {
			HANDLE tok;
			if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&tok)) return true;
			TOKEN_ELEVATION_TYPE t = TokenElevationTypeDefault;
			DWORD n = 0;
			bool ok = GetTokenInformation(tok,TokenElevationType,&t,sizeof(t),&n) != 0;
			CloseHandle(tok);
			return !ok || t == TokenElevationTypeFull;
		}();
		return v;
	}

}
#endif // _WIN32
