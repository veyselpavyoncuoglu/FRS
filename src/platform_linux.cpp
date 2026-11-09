#ifndef _WIN32
#include "platform.h"
#include "util.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <pwd.h>
#include <grp.h>

namespace {

	// uid of the user who started us through pkexec/sudo, -1 otherwise
	int realUid() {
		if (geteuid() != 0) return -1;
		const char* s = getenv("PKEXEC_UID");
		if (!s) s = getenv("SUDO_UID");
		return s ? atoi(s) : -1;
	}

	// root works on the user's files as that user (this thread only) : a symlink they planted can't aim root's writes
	// at /etc/passwd, and what gets created is theirs without a chown
	struct AsUser {
		int uid = realUid();
		int ou = 0,og = 0;
		AsUser() {
			passwd pw;
			passwd* res = nullptr;
			char b[4096];
			if (uid < 0 || getpwuid_r((uid_t)uid,&pw,b,sizeof(b),&res) != 0 || !res) {
				uid = -1;
				return;
			}
			og = setfsgid(pw.pw_gid);
			ou = setfsuid((uid_t)uid);
		}
		~AsUser() {
			if (uid < 0) return;
			setfsuid((uid_t)ou);
			setfsgid((gid_t)og);
		}
	};

	// XDG base dir: $XDG_CONFIG_HOME or ~/.config (of the real user when elevated, sudo points HOME at /root)
	std::string configHome() {
		const char* xdg = getenv("XDG_CONFIG_HOME");
		if (xdg && *xdg) return xdg;
		int uid = realUid();
		passwd* pw = uid >= 0 ? getpwuid(uid) : nullptr;
		if (pw) return std::string(pw->pw_dir) + "/.config";
		const char* home = getenv("HOME");
		if (home && *home) return std::string(home) + "/.config";
		return "."; // last resort: current directory
	}

	void mkdirp(const std::string& path) {
		// Create each component; ignore "already exists". 0700 : settings.ini holds the MCP token
		for (size_t i = 1; i <= path.size(); i++) {
			if (i == path.size() || path[i] == '/') {
				std::string sub = path.substr(0,i);
				if (!sub.empty()) mkdir(sub.c_str(),0700);
			}
		}
	}

	// Absolute path to the running executable, for the autostart entry.
	std::string exePath() {
		char buf[PATH_MAX];
		ssize_t n = readlink("/proc/self/exe",buf,sizeof(buf) - 1);
		if (n > 0) { buf[n] = '\0'; return buf; }
		return "FRS";
	}

	bool portableArg() {
		std::string cl;
		if (FILE* f = fopen("/proc/self/cmdline","rb")) {
			char b[4096];
			size_t n;
			while ((n = fread(b,1,sizeof(b),f)) > 0) cl.append(b,n);
			fclose(f);
		}
		// any case, - or / like the other switches
		size_t p = cl.find('\0');
		while (p != std::string::npos && p + 1 < cl.size()) {
			size_t e = cl.find('\0',p + 1);
			std::string a = util::lower(cl.substr(p + 1,e == std::string::npos ? std::string::npos : e - p - 1));
			if (a.size() > 1 && a[0] == '/') a[0] = '-';
			if (a == "-portable") return true;
			p = e;
		}
		return false;
	}

	std::string g_note;
	bool g_port = false; // the folder is the exe's, its mode stays as the user set it
	bool g_portSw = false; // and only -portable put it there, autostart has to pass it on

	// FRS_CONFIG_DIR in a test build, next to the exe in portable mode when the user can write there, else the XDG folder
	std::string pickDir() {
		const char* e = util::testEnv("FRS_CONFIG_DIR");
		if (e && *e) return e;
		std::string def = configHome() + "/FRS";
		std::string ex = exePath();
		size_t sl = ex.find_last_of('/');
		ex = sl == std::string::npos || sl == 0 ? "" : ex.substr(0,sl);
		struct stat st;
		bool mark = !ex.empty() && stat((ex + "/FRS.portable").c_str(),&st) == 0;
		if (ex.empty() || (!mark && !portableArg())) return def;
		AsUser au;
		std::string t = ex + "/.frs_wtest";
		int fd = open(t.c_str(),O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,0600);
		if (fd < 0) {
			g_note = "Portable mode : failed (can't write next to FRS), settings stay in " + util::safe(def);
			return def;
		}
		close(fd);
		unlink(t.c_str());
		g_port = true;
		g_portSw = !mark;
		return ex;
	}

	std::string settingsDir() {
		static const std::string d = pickDir();
		return d;
	}

	// older builds run with sudo -E left the folder and files owned by root, the user can't write them any more.
	// Given back only inside a folder that user owns, no symlink, no hard link, so nothing else changes hands
	void reclaim(const std::string& dir,const char* name) {
		int uid = realUid();
		passwd pw;
		passwd* res = nullptr;
		char b[4096];
		if (uid < 0 || getpwuid_r((uid_t)uid,&pw,b,sizeof(b),&res) != 0 || !res) return;
		size_t sl = dir.find_last_of('/');
		std::string up = sl == std::string::npos ? "." : sl == 0 ? "/" : dir.substr(0,sl);
		int p = open(up.c_str(),O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (p < 0) return;
		struct stat st;
		int d = -1;
		if (fstat(p,&st) == 0 && st.st_uid == (uid_t)uid) d = openat(p,dir.c_str() + sl + 1,O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		close(p);
		if (d < 0) return;
		bool bad = false;
		int f = openat(d,name,O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		if (f >= 0) {
			if (fstat(f,&st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 && st.st_nlink == 1) bad |= fchown(f,(uid_t)uid,pw.pw_gid) != 0;
			close(f);
		}
		if (fstat(d,&st) == 0 && st.st_uid == 0) bad |= fchown(d,(uid_t)uid,pw.pw_gid) != 0;
		close(d);
		if (bad) appLog("[!] Settings : could not give " + util::safe(dir) + " back to its user");
	}

	// plain sudo and pkexec point HOME at root's, older builds kept their files in root's ~/.config/FRS
	std::string rootFile(const char* name) {
		if (realUid() < 0 || settingsDir() != configHome() + "/FRS") return "";
		passwd pw;
		passwd* res = nullptr;
		char b[4096];
		if (getpwuid_r(geteuid(),&pw,b,sizeof(b),&res) != 0 || !res) return "";
		FILE* f = fopen((std::string(pw.pw_dir) + "/.config/FRS/" + name).c_str(),"rb");
		if (!f) return "";
		std::string out;
		char buf[4096];
		size_t n;
		while ((n = fread(buf,1,sizeof(buf),f)) > 0) out.append(buf,n);
		fclose(f);
		return out;
	}

	std::string autostartPath() {
		return configHome() + "/autostart/FRS.desktop";
	}

	// a run without root found no settings.ini (pkexec dismissed after an update) : the file it writes says so, and the first run
	// with root still brings the older build's root copy over. Unknown keys stay in the file, so later runs keep the mark
	const char* kPend = "rootCopyPending=1";
	bool g_pend = false;
	int g_lock = -1; // frs.lock while this FRS holds it

	const char* kMerged = "Settings : root copy from an older FRS brought over, your own settings win (yours as they were in settings.ini.bak)";

	void merged() {
		g_note += std::string(g_note.empty() ? "" : "\n") + kMerged;
	}

	// the older build's root copy is the base, every key of the user's file wins over it. Lines without a key and the mark go
	std::string mergeIni(const std::string& old,const std::string& cur) {
		auto lines = [](const std::string& t) {
			std::vector<std::pair<std::string,std::string>> v;
			size_t p = 0;
			while (p < t.size()) {
				size_t e = std::min(t.find('\n',p),t.size());
				std::string l = t.substr(p,e - p);
				p = e + 1;
				size_t eq = l.find('=');
				if (eq != std::string::npos && eq > 0 && l.compare(0,eq,"rootCopyPending")) v.push_back({l.substr(0,eq),l});
			}
			return v;
		};
		auto o = lines(old),c = lines(cur);
		std::set<std::string> ks;
		for (auto& l : c) ks.insert(l.first);
		std::string out;
		for (auto& l : o) if (!ks.count(l.first)) out += l.second + "\n";
		for (auto& l : c) out += l.second + "\n";
		return out;
	}

	// owned by uid (the owner can chmod it), group write for any of its groups, or world write. ACLs are not looked at
	bool canWrite(const struct stat& st,uid_t uid,const std::vector<gid_t>& gs) {
		return st.st_uid == uid || ((st.st_mode & S_IWGRP) && std::find(gs.begin(),gs.end(),st.st_gid) != gs.end()) || (st.st_mode & S_IWOTH);
	}

}

namespace plat {

	void raisePrivileges() {
		if (geteuid() == 0) {
			// sudo strips XDG_RUNTIME_DIR, Wayland/EGL need it to find the session
			int uid = realUid();
			std::string rd = "/run/user/" + std::to_string(uid);
			if (uid >= 0 && !getenv("XDG_RUNTIME_DIR") && access(rd.c_str(),F_OK) == 0) setenv("XDG_RUNTIME_DIR",rd.c_str(),1);
			// and points HOME at /root, so X11 can't find the user's cookie
			passwd* pw = uid >= 0 ? getpwuid(uid) : nullptr;
			std::string xa = pw ? std::string(pw->pw_dir) + "/.Xauthority" : "";
			if (pw && !getenv("XAUTHORITY") && access(xa.c_str(),R_OK) == 0) setenv("XAUTHORITY",xa.c_str(),1);
			return;
		}
		if (getenv("FRS_ELEVATED")) return;

		// Like the UAC manifest on Windows : relaunch as root through pkexec (graphical password
		// prompt). pkexec wipes the env so pass on what the window needs. 126/127 means the
		// prompt was dismissed/denied (or no pkexec), then we just keep running unprivileged.
		std::vector<std::string> args = {"pkexec","/usr/bin/env","FRS_ELEVATED=1"};
		for (const char* k : {"DISPLAY","XAUTHORITY","WAYLAND_DISPLAY","XDG_RUNTIME_DIR","XDG_CONFIG_HOME","XDG_SESSION_TYPE","LANG"}) {
			const char* v = getenv(k);
			if (v && *v) args.push_back(std::string(k) + "=" + v);
		}
		// a test build's switches, the root copy has to fake its cleans too. The plain build has none
		for (const char* k : {"FRS_CONFIG_DIR","FRS_TEST_FAKECMD","FRS_TEST_IFEO_KEY","FRS_TEST_NOACTIVATE","FRS_TEST_PAGE","FRS_TEST_PROC","FRS_TEST_TAB","FRS_TEST_TASK"}) {
			const char* v = util::testEnv(k);
			if (v) args.push_back(std::string(k) + "=" + v);
		}
		const char* home = getenv("HOME");
		std::string xa = std::string(home ? home : "") + "/.Xauthority";
		if (!getenv("XAUTHORITY") && home && access(xa.c_str(),R_OK) == 0) args.push_back("XAUTHORITY=" + xa);
		args.push_back(exePath());
		// our own arguments go along, pkexec starts a fresh process
		std::string cl;
		if (FILE* cf = fopen("/proc/self/cmdline","rb")) {
			char b[4096];
			size_t n;
			while ((n = fread(b,1,sizeof(b),cf)) > 0) cl.append(b,n);
			fclose(cf);
		}
		size_t p = cl.find('\0');
		while (p != std::string::npos && p + 1 < cl.size()) {
			size_t e = cl.find('\0',p + 1);
			args.push_back(e == std::string::npos ? cl.substr(p + 1) : cl.substr(p + 1,e - p - 1));
			p = e;
		}

		std::string bak = settingsDir() + "/settings.ini.bak";
		struct stat b0,b1;
		bool had = stat(bak.c_str(),&b0) == 0;
		pid_t pid = fork();
		if (pid == 0) {
			std::vector<char*> av;
			for (auto& a : args) av.push_back(&a[0]);
			av.push_back(nullptr);
			execvp("pkexec",av.data());
			_exit(127);
		}
		if (pid < 0) return;
		int st = 0;
		waitpid(pid,&st,0);
		int code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
		// 125 : the root copy got no window (X server or compositor refuses root), this one carries on without root.
		// A new settings.ini.bak : it brought an older root file over first, this run's App::init says so
		if (code == 125) {
			fprintf(stderr,"[!] FRS as root could not open a window, it runs without root\n");
			if (stat(bak.c_str(),&b1) == 0 && (!had || b1.st_mtime != b0.st_mtime || b1.st_size != b0.st_size)) merged();
		} else if (code != 126 && code != 127) _exit(code);
	}

	std::string readFile(const char* name) {
		bool set = !strcmp(name,"settings.ini");
		std::string out;
		bool pend = false;
		{
			AsUser au;
			// older builds left the folder 0755 and the files 0644, other users could read the token
			std::string d = settingsDir();
			if (!g_port) chmod(d.c_str(),0700);
			FILE* f = fopen((settingsDir() + "/" + name).c_str(),"rb");
			if (f) {
				fchmod(fileno(f),0600);
				char buf[4096];
				size_t n;
				while ((n = fread(buf,1,sizeof(buf),f)) > 0) out.append(buf,n);
				fclose(f);
				size_t m = set ? ("\n" + out).find(std::string("\n") + kPend) : std::string::npos;
				// one with no mark : the root copy already came over (the root child of this launch did it), nothing is pending
				if (set && m == std::string::npos) g_pend = false;
				if (m == std::string::npos || realUid() < 0) return out;
				size_t e = out.find('\n',m);
				out.erase(m,e == std::string::npos ? std::string::npos : e - m + 1);
				pend = true;
			} else if (errno != ENOENT) return "";
			else if (set && geteuid() != 0 && settingsDir() == configHome() + "/FRS") {
				g_pend = true;
				return "";
			}
		}
		// the user has none yet, or one a run without root made : what an older build left in root's folder comes over once.
		// Into the user's one with their keys winning, theirs goes to settings.ini.bak first. No root copy : only the mark goes
		std::string old = rootFile(name);
		if (old.empty()) {
			if (pend) writeFile(name,out);
			return out;
		}
		if (pend) {
			writeFile("settings.ini.bak",out);
			old = mergeIni(old,out);
			// no window yet : App::init logs it through dirNote, -clean with its frs.log lines, a root copy that got no window
			// through the run it hands back to
			merged();
			fprintf(stderr,"%s\n",kMerged);
		}
		writeFile(name,old);
		return old;
	}

	void writeFile(const char* name,const std::string& text) {
		reclaim(settingsDir(),name);
		AsUser au;
		mkdirp(settingsDir());
		if (!g_port) chmod(settingsDir().c_str(),0700);
		std::string path = settingsDir() + "/" + name;
		FILE* f = fopen(path.c_str(),"wb");
		if (!f) {
			std::string why = strerror(errno);
			static std::atomic<bool> told{false};
			if (!told.exchange(true)) appLog("[!] Settings : can't write " + util::safe(path) + " (" + why + "), changes are not saved");
			return;
		}
		fchmod(fileno(f),0600);
		fwrite(text.data(),1,text.size(),f);
		if (g_pend && !strcmp(name,"settings.ini") && ("\n" + text).find(std::string("\n") + kPend) == std::string::npos) fprintf(f,"%s%s\n",!text.empty() && text.back() != '\n' ? "\n" : "",kPend);
		fclose(f);
	}

	bool appendFile(const char* name,const std::string& text,size_t maxBytes) {
		reclaim(settingsDir(),name);
		AsUser au;
		mkdirp(settingsDir());
		std::string p = settingsDir() + "/" + name;
		struct stat st;
		if (lstat(p.c_str(),&st) == 0 && st.st_size > 0 && (size_t)st.st_size + text.size() > maxBytes) rename(p.c_str(),(p + ".1").c_str());
		int fd = open(p.c_str(),O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC,0600);
		if (fd < 0) return false;
		size_t off = 0;
		while (off < text.size()) {
			ssize_t n = write(fd,text.data() + off,text.size() - off);
			if (n <= 0) break;
			off += (size_t)n;
		}
		close(fd);
		return off == text.size();
	}

	std::string filePath(const char* name) { return settingsDir() + "/" + name; }

	std::string dirNote() {
		settingsDir();
		return g_note;
	}

	std::string readSettings() { return readFile("settings.ini"); }
	void writeSettings(const std::string& text) { writeFile("settings.ini",text); }

	// the lock lasts as long as the process, the fd stays open. keep false lets it go at once.
	// One that wrote x into the file has no window any more and finishes a clean before it exits : waited for, up to 10 s
	bool firstInstance(bool keep) {
		AsUser au;
		mkdirp(settingsDir());
		int fd = open((settingsDir() + "/frs.lock").c_str(),O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC,0600);
		if (fd < 0) return true;
		bool got = false;
		for (int i = 0;; i++) {
			if (flock(fd,LOCK_EX | LOCK_NB) == 0) {
				got = true;
				break;
			}
			if (errno != EWOULDBLOCK) break;
			char c = 0;
			if (i >= 50 || pread(fd,&c,1,0) != 1 || c != 'x') {
				close(fd);
				return false;
			}
			usleep(200000);
		}
		if (!got || !keep) close(fd);
		else {
			if (ftruncate(fd,0) != 0) {}
			g_lock = fd;
		}
		return true;
	}

	bool showOther(bool) {
		fprintf(stderr,"FRS already runs\n");
		return true;
	}

	void leaving() {
		if (g_lock >= 0 && pwrite(g_lock,"x",1,0) != 1) {}
	}

	void markMain(void*) {}

	bool startupSupported() { return true; }

	bool startupEnabled() {
		struct stat st;
		return stat(autostartPath().c_str(),&st) == 0;
	}

	bool setStartupEnabled(bool on,std::string* why) {
		settingsDir();
		AsUser au;
		std::string path = autostartPath();
		if (on) {
			mkdirp(configHome() + "/autostart");
			FILE* f = fopen(path.c_str(),"w");
			if (!f) {
				if (why) *why = strerror(errno);
				return false;
			}
			fprintf(f,
				"[Desktop Entry]\n"
				"Type=Application\n"
				"Name=FRS\n"
				"Comment=Free RAM System\n"
				"Exec=\"%s\"%s\n"
				"Terminal=false\n"
				"X-GNOME-Autostart-enabled=true\n",
				exePath().c_str(),g_portSw ? " -portable" : "");
			fclose(f);
			return true;
		} else {
			bool ok = remove(path.c_str()) == 0 || !startupEnabled();
			if (!ok && why) *why = strerror(errno);
			return ok;
		}
	}

	int upgradeStartup(std::string& why) {
		why = "no task on Linux";
		return 0;
	}

	// FRS runs as root through pkexec : the exe, its folder or one above it the user can write lets anything running as them
	// swap the binary before the next prompt. Root with no invoker : nobody to ask about
	bool exeInSafeDir() {
		int uid = geteuid() == 0 ? realUid() : (int)geteuid();
		passwd pw;
		passwd* res = nullptr;
		char b[4096];
		if (uid < 0 || getpwuid_r((uid_t)uid,&pw,b,sizeof(b),&res) != 0 || !res) return true;
		// the primary group and the supplementary ones, a group writable /usr/local or /opt counts
		std::vector<gid_t> gs(64);
		int n = (int)gs.size();
		if (getgrouplist(pw.pw_name,pw.pw_gid,gs.data(),&n) < 0) {
			gs.resize(n > 0 ? n : 1);
			if (getgrouplist(pw.pw_name,pw.pw_gid,gs.data(),&n) < 0) n = 0;
		}
		gs.resize(std::max(n,0));
		gs.push_back(pw.pw_gid);
		std::string p = exePath();
		while (!p.empty() && p != "/") {
			struct stat st;
			if (lstat(p.c_str(),&st) != 0) return true;
			if (canWrite(st,(uid_t)uid,gs)) return false;
			size_t sl = p.find_last_of('/');
			p = sl == std::string::npos ? "" : sl == 0 ? "/" : p.substr(0,sl);
		}
		return true;
	}

	bool noPromptSupported() { return false; }
	bool noPromptOn() { return false; }
	std::string noPromptLink() { return ""; }

	bool setNoPrompt(bool,std::string& why) {
		why = "not on Linux";
		return false;
	}

	const char* osName() { return "Linux"; }

	bool rootOnUserFiles() { return realUid() >= 0; }

	bool invoker(unsigned& uid,unsigned& gid,std::string& name,std::string& home) {
		if (geteuid() != 0) return false;
		const char* s = getenv("PKEXEC_UID");
		if (!s) s = getenv("SUDO_UID");
		if (!s) return false;
		uid = (unsigned)strtoul(s,nullptr,10);
		gid = (unsigned)-1;
		name = std::to_string(uid);
		char b[4096];
		passwd pw;
		passwd* res = nullptr;
		if (getpwuid_r((uid_t)uid,&pw,b,sizeof(b),&res) == 0 && res) {
			gid = res->pw_gid;
			name = util::safe(res->pw_name);
			home = res->pw_dir;
		}
		return true;
	}

}
#endif // !_WIN32
