#ifndef _WIN32
#include "platform.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>

namespace {

	// XDG base dir: $XDG_CONFIG_HOME or ~/.config
	std::string configHome() {
		const char* xdg = getenv("XDG_CONFIG_HOME");
		if (xdg && *xdg) return xdg;
		const char* home = getenv("HOME");
		if (home && *home) return std::string(home) + "/.config";
		return "."; // last resort: current directory
	}

	void mkdirp(const std::string& path) {
		// Create each component; ignore "already exists".
		for (size_t i = 1; i <= path.size(); i++) {
			if (i == path.size() || path[i] == '/') {
				std::string sub = path.substr(0,i);
				if (!sub.empty()) mkdir(sub.c_str(),0755);
			}
		}
	}

	std::string settingsDir() { return configHome() + "/FRS"; }
	std::string settingsPath() { return settingsDir() + "/settings.ini"; }

	std::string autostartPath() {
		return configHome() + "/autostart/FRS.desktop";
	}

	// Absolute path to the running executable, for the autostart entry.
	std::string exePath() {
		char buf[PATH_MAX];
		ssize_t n = readlink("/proc/self/exe",buf,sizeof(buf) - 1);
		if (n > 0) { buf[n] = '\0'; return buf; }
		return "FRS";
	}

}

namespace plat {

	void raisePrivileges() {
		// On Linux the relevant privileges come from running as root (the cleaning
		// operations check geteuid); there is no per-privilege token to raise.
	}

	std::string readSettings() {
		FILE* f = fopen(settingsPath().c_str(),"rb");
		if (!f) return "";
		std::string out;
		char buf[4096];
		size_t n;
		while ((n = fread(buf,1,sizeof(buf),f)) > 0) out.append(buf,n);
		fclose(f);
		return out;
	}

	void writeSettings(const std::string& text) {
		mkdirp(settingsDir());
		FILE* f = fopen(settingsPath().c_str(),"wb");
		if (!f) return;
		fwrite(text.data(),1,text.size(),f);
		fclose(f);
	}

	bool startupSupported() { return true; }

	bool startupEnabled() {
		struct stat st;
		return stat(autostartPath().c_str(),&st) == 0;
	}

	bool setStartupEnabled(bool on) {
		std::string path = autostartPath();
		if (on) {
			mkdirp(configHome() + "/autostart");
			FILE* f = fopen(path.c_str(),"w");
			if (!f) return false;
			fprintf(f,
				"[Desktop Entry]\n"
				"Type=Application\n"
				"Name=FRS\n"
				"Comment=Free RAM System\n"
				"Exec=%s\n"
				"Terminal=false\n"
				"X-GNOME-Autostart-enabled=true\n",
				exePath().c_str());
			fclose(f);
			return true;
		} else {
			return remove(path.c_str()) == 0 || !startupEnabled();
		}
	}

	const char* osName() { return "Linux"; }

}
#endif // !_WIN32
