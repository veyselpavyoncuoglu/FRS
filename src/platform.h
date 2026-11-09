#pragma once
#include <cstddef>
#include <string>

// Thin OS-abstraction layer. The shared App core (app.cpp) talks only to these
// functions; the platform-specific implementations live in platform_win.cpp and
// platform_linux.cpp. Both are always compiled, each guards itself with #ifdef _WIN32 / #ifndef _WIN32.
namespace plat {

	// Raise any privileges the cleaning operations benefit from (e.g. SE_DEBUG on
	// Windows so EmptyWorkingSet can reach system processes). No-op where N/A.
	void raisePrivileges();

	// Whole-file settings I/O. The core builds/parses the INI text itself so the
	// platform only deals with locating the file and its (possibly Unicode) path.
	std::string readSettings();              // "" if the file does not exist
	void writeSettings(const std::string& text);

	// any other small file in the settings folder (ui.ini, page priority rules...). Name without a path.
	// Env FRS_CONFIG_DIR replaces the settings folder in a test build (util::testEnv). A file FRS.portable next to the exe
	// or -portable puts it next to the exe
	std::string readFile(const char* name); // "" if the file does not exist
	void writeFile(const char* name,const std::string& text);
	// adds text at the end, first renames the file to name.1 (replacing it) when it would grow past maxBytes. false when it can't write
	bool appendFile(const char* name,const std::string& text,size_t maxBytes);
	std::string filePath(const char* name); // full path of a file in the settings folder
	std::string dirNote(); // why portable mode was asked for and is off, "" normally

	// single instance : false when another FRS on the same settings folder runs, showOther brings that one forward (Linux : says so on stderr).
	// keep false only tests, the next FRS can still take it (the Linux launcher before pkexec). Linux : one that called leaving() is waited for, up to 10 s
	bool firstInstance(bool keep = true);
	// memPage : -sysinfo was asked for, that FRS opens its Memory page (Windows). false : no window found, that FRS is starting or on its way out. Linux : true
	bool showOther(bool memPage = false);
	void leaving(); // Linux : the window is gone, the lock is only held for the shutdown. Windows : nothing, the window going away says it
	void markMain(void* hwnd); // Windows : tags the main window so showOther finds it

	// "Run at startup" toggle. startupSupported() lets the UI hide the control on
	// platforms where it is not implemented.
	bool startupSupported();
	bool startupEnabled();
	bool setStartupEnabled(bool on,std::string* why = nullptr); // returns true on success
	// once for a task an older FRS made : no battery stop, no 3 day limit, normal priority, its triggers and command stay.
	// 1 done, 0 left alone because FRS did not make it (why says so), -1 failed. Linux : 0
	int upgradeStartup(std::string& why);
	bool exeInSafeDir(); // Windows : the exe sits where only admins can write (Program Files, Windows). Linux : the user behind pkexec can't write the exe or a folder above it

	// Windows : a task that starts FRS elevated on demand and a shortcut that runs it, no UAC prompt. Linux : not supported
	bool noPromptSupported();
	bool noPromptOn(); // the shortcut is there
	std::string noPromptLink(); // where the shortcut goes
	bool setNoPrompt(bool on,std::string& why);

	// Display name shown in the title row, e.g. "Windows" / "Linux".
	const char* osName();

	// running elevated on settings the user's unelevated programs can write. Linux : root through pkexec or sudo.
	// Windows : an elevated token that has an unelevated twin. A Danger tool's On is then not taken from the file
	bool rootOnUserFiles();

	// Linux only : the user behind pkexec or sudo while FRS runs as root, false otherwise.
	// gid stays -1 and home empty when that uid has no passwd entry
	bool invoker(unsigned& uid,unsigned& gid,std::string& name,std::string& home);

}
