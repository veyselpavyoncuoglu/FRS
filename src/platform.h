#pragma once
#include <string>

// Thin OS-abstraction layer. The shared App core (app.cpp) talks only to these
// functions; the platform-specific implementations live in platform_win.cpp and
// platform_linux.cpp. Each TU is compiled only on its matching platform.
namespace plat {

	// Raise any privileges the cleaning operations benefit from (e.g. SE_DEBUG on
	// Windows so EmptyWorkingSet can reach system processes). No-op where N/A.
	void raisePrivileges();

	// Whole-file settings I/O. The core builds/parses the INI text itself so the
	// platform only deals with locating the file and its (possibly Unicode) path.
	std::string readSettings();              // "" if the file does not exist
	void writeSettings(const std::string& text);

	// "Run at startup" toggle. startupSupported() lets the UI hide the control on
	// platforms where it is not implemented.
	bool startupSupported();
	bool startupEnabled();
	bool setStartupEnabled(bool on);         // returns true on success

	// Display name shown in the title row, e.g. "Windows" / "Linux".
	const char* osName();

}
