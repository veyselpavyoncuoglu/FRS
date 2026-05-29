#ifdef _WIN32
#include "platform.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
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

	std::wstring getSettingsPath() {
		wchar_t buf[MAX_PATH];
		SHGetFolderPathW(nullptr,CSIDL_APPDATA,nullptr,0,buf);
		std::wstring dir = std::wstring(buf) + L"\\FRS";
		CreateDirectoryW(dir.c_str(),nullptr);
		return dir + L"\\settings.ini";
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

}

namespace plat {

	void raisePrivileges() {
		enablePriv(SE_DEBUG_NAME);
		enablePriv(L"SeProfileSingleProcessPrivilege");
	}

	std::string readSettings() {
		FILE* f = nullptr;
		_wfopen_s(&f,getSettingsPath().c_str(),L"rb");
		if (!f) return "";
		std::string out;
		char buf[4096];
		size_t n;
		while ((n = fread(buf,1,sizeof(buf),f)) > 0) out.append(buf,n);
		fclose(f);
		return out;
	}

	void writeSettings(const std::string& text) {
		FILE* f = nullptr;
		_wfopen_s(&f,getSettingsPath().c_str(),L"wb");
		if (!f) return;
		fwrite(text.data(),1,text.size(),f);
		fclose(f);
	}

	bool startupSupported() { return true; }

	bool startupEnabled() {
		return runHidden(L"schtasks /query /tn \"FRS\" /fo LIST");
	}

	bool setStartupEnabled(bool on) {
		if (on) {
			wchar_t buf[MAX_PATH];
			GetModuleFileNameW(nullptr,buf,MAX_PATH);
			std::wstring exe = buf;
			std::wstring cmd = L"schtasks /create /tn \"FRS\" /tr \"\\\"" + exe + L"\\\"\" /sc onlogon /rl highest /f";
			return runHidden(cmd);
		} else {
			return runHidden(L"schtasks /delete /tn \"FRS\" /f");
		}
	}

	const char* osName() { return "Windows"; }

}
#endif // _WIN32
