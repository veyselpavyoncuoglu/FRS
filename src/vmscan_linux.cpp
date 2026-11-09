#ifndef _WIN32
#include "vmscan.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

	using namespace vmscan;

	enum { PR = 1,PW = 2,PX = 4,PS = 8 };

	// /proc/pid/stat field 22, what ProcRef.start holds. The comm can hold spaces and parens, so count from the last )
	bool parseStart(const char* b,uint64_t& out) {
		const char* p = strrchr(b,')');
		if (!p) return false;
		p++;
		for (int i = 0; i < 20; i++) {
			while (*p == ' ') p++;
			if (!*p) return false;
			if (i == 19) break;
			while (*p && *p != ' ') p++;
		}
		out = strtoull(p,nullptr,10);
		return true;
	}

	// 0 or errno
	int readStat(uint32_t pid,char (&b)[1024]) {
		char path[64];
		snprintf(path,sizeof(path),"/proc/%u/stat",pid);
		int fd = ::open(path,O_RDONLY | O_CLOEXEC);
		if (fd < 0) return errno;
		ssize_t n = ::read(fd,b,sizeof(b) - 1);
		int e = errno;
		::close(fd);
		if (n < 0) return e;
		if (!n) return ESRCH;
		b[n] = 0;
		return 0;
	}

	bool startOf(uint32_t pid,uint64_t& out) {
		char b[1024];
		return !readStat(pid,b) && parseStart(b,out);
	}

	// the state follows the last ). A zombie still has its stat file, but no memory left to read
	bool isGone(const char* b) {
		const char* p = strrchr(b,')');
		return p && p[1] == ' ' && (p[2] == 'Z' || p[2] == 'X');
	}

	// T is a stop by signal, t a ptrace stop
	char stateOf(const char* b) {
		const char* p = strrchr(b,')');
		return p && p[1] == ' ' ? p[2] : 0;
	}

	// gdb killed while it holds the threads lets their SIGSTOP through and the target stays in T. Each thread brings its own,
	// so look again a while after a SIGCONT. A gdb that quits on SIGTERM detaches by itself
	void unstop(const ProcRef& r) {
		char b[1024];
		uint64_t s = 0;
		for (int i = 0; i < 50; i++) {
			if (readStat(r.pid,b) || !parseStart(b,s) || (r.start && s != r.start)) return;
			if (stateOf(b) == 'T') {
				kill((pid_t)r.pid,SIGCONT);
				i = std::max(i,40);
			}
			usleep(20000);
		}
	}

	std::string procErr(uint32_t pid,int e) {
		if (e == ENOENT || e == ESRCH) return "the process exited";
		if (e == EACCES || e == EPERM) return "no ptrace access, the process belongs to another user or kernel.yama.ptrace_scope blocks it. Run FRS as root";
		return strerror(e);
	}

	struct MapLine {
		uint64_t a = 0,b = 0;
		uint32_t prot = 0;
		std::string path;
	};

	bool parseMap(const char* l,MapLine& m) {
		char perms[8] = "";
		unsigned long long a,b,off,ino;
		unsigned maj,min;
		int used = 0;
		if (sscanf(l,"%llx-%llx %7s %llx %x:%x %llu %n",&a,&b,perms,&off,&maj,&min,&ino,&used) < 7) return false;
		m.a = a;
		m.b = b;
		m.prot = 0;
		if (perms[0] == 'r') m.prot |= PR;
		if (perms[1] == 'w') m.prot |= PW;
		if (perms[2] == 'x') m.prot |= PX;
		if (perms[3] == 's') m.prot |= PS;
		m.path = used > 0 ? l + used : "";
		while (!m.path.empty() && (m.path.back() == '\n' || m.path.back() == ' ')) m.path.pop_back();
		return true;
	}

	int kindOf(const MapLine& m) {
		bool file = !m.path.empty() && m.path[0] == '/';
		if (!file && !(m.prot & PS)) return KPrivate;
		if (file && (m.prot & PX)) return KImage;
		return KMapped;
	}

	// reading [vvar] faults, device maps can have side effects
	bool skipMap(const MapLine& m) {
		if (!(m.prot & PR)) return true;
		const std::string& p = m.path;
		if (p.compare(0,5,"[vvar") == 0 || p == "[vsyscall]") return true;
		return p.compare(0,5,"/dev/") == 0 && p.compare(0,9,"/dev/shm/") != 0 && p.compare(0,9,"/dev/zero") != 0;
	}

	template<class F> bool eachMap(uint32_t pid,int& err,F fn) {
		char path[64];
		snprintf(path,sizeof(path),"/proc/%u/maps",pid);
		FILE* f = fopen(path,"re");
		if (!f) {
			err = errno;
			return false;
		}
		char line[4096 + 128];
		while (fgets(line,sizeof(line),f)) {
			MapLine m;
			if (!parseMap(line,m)) continue;
			if (!fn(m)) break;
		}
		fclose(f);
		return true;
	}

	// getpwuid is not reentrant and HTTP threads come here at the same time
	bool userOf(uid_t id,uid_t& u,gid_t& g,std::string& home) {
		passwd pw;
		passwd* res = nullptr;
		char b[16384];
		if (getpwuid_r(id,&pw,b,sizeof(b),&res) != 0 || !res) return false;
		u = pw.pw_uid;
		g = pw.pw_gid;
		home = pw.pw_dir ? pw.pw_dir : "";
		return true;
	}

	bool whoStarted(uid_t& u,gid_t& g,std::string& home) {
		const char* s = getenv("PKEXEC_UID");
		if (!s) s = getenv("SUDO_UID");
		if (!s || !*s) return false;
		return userOf((uid_t)strtoul(s,nullptr,10),u,g,home);
	}

	// looked up once, a new gdb takes a restart. Relative PATH entries don't count, this can run as root
	const std::string& gcorePath() {
		static const std::string p = [] {
			const char* e = getenv("PATH");
			std::string s = e && *e ? e : "/usr/bin:/bin";
			for (size_t i = 0; i < s.size();) {
				size_t k = std::min(s.find(':',i),s.size());
				std::string f = s.substr(i,k - i) + "/gcore";
				if (f[0] == '/' && access(f.c_str(),X_OK) == 0) return f;
				i = k + 1;
			}
			return std::string();
		}();
		return p;
	}

}

namespace vmscan {

	bool dumpAvail() {
		return !gcorePath().empty();
	}

	namespace os {

		struct Proc {
			int fd = -1;
			uint32_t pid = 0;
			~Proc() { if (fd >= 0) ::close(fd); }
		};

		std::shared_ptr<Proc> open(const ProcRef& r,std::string& err) {
			char path[64];
			snprintf(path,sizeof(path),"/proc/%u/mem",r.pid);
			int fd = ::open(path,O_RDONLY | O_CLOEXEC);
			if (fd < 0) {
				err = procErr(r.pid,errno);
				return nullptr;
			}
			auto p = std::make_shared<Proc>();
			p->fd = fd;
			p->pid = r.pid;
			// the fd pins this process, so check the start after opening it
			uint64_t st = 0;
			if (r.start && (!startOf(r.pid,st) || st != r.start)) {
				err = "pid " + std::to_string(r.pid) + " was reused, this is another process now";
				return nullptr;
			}
			return p;
		}

		bool regions(Proc& p,std::vector<Region>& out,int& skipped,std::string& err) {
			out.clear();
			skipped = 0;
			int e = 0;
			bool ok = eachMap(p.pid,e,[&](const MapLine& m) {
				if (skipMap(m)) {
					skipped++;
					return true;
				}
				Region g;
				g.base = m.a;
				g.size = m.b - m.a;
				g.prot = m.prot;
				g.kind = kindOf(m);
				out.push_back(g);
				return true;
			});
			if (!ok) err = procErr(p.pid,e);
			return ok;
		}

		size_t read(Proc& p,uint64_t addr,void* buf,size_t n) {
			if (addr > (uint64_t)INT64_MAX) return 0;
			for (;;) {
				ssize_t k = pread(p.fd,buf,n,(off_t)addr);
				if (k < 0 && errno == EINTR) continue;
				return k > 0 ? (size_t)k : 0;
			}
		}

		bool exited(Proc& p) {
			char b[1024];
			int e = readStat(p.pid,b);
			return e ? e == ENOENT || e == ESRCH : isGone(b);
		}

		bool regionAt(Proc& p,uint64_t addr,Region& out,std::string& err) {
			bool found = false;
			int e = 0;
			bool ok = eachMap(p.pid,e,[&](const MapLine& m) {
				if (addr < m.a || addr >= m.b) return true;
				if (skipMap(m)) return false;
				out.base = m.a;
				out.size = m.b - m.a;
				out.prot = m.prot;
				out.kind = kindOf(m);
				found = true;
				return false;
			});
			if (!ok) {
				err = procErr(p.pid,e);
				return false;
			}
			if (!found) err = exited(p) ? "the process exited" : "address " + util::hex(addr) + " is not in a readable mapping";
			return found;
		}

		std::string protStr(uint32_t pr) {
			std::string s = "---p";
			if (pr & PR) s[0] = 'r';
			if (pr & PW) s[1] = 'w';
			if (pr & PX) s[2] = 'x';
			if (pr & PS) s[3] = 's';
			return s;
		}

		struct Out {
			int fd = -1;
			std::string path;
			~Out() { if (fd >= 0) ::close(fd); }
		};

		std::shared_ptr<Out> create(const std::string& path,std::string& err) {
			// a dump holds secrets, owner only. O_EXCL also refuses a symlink at that name
			int fd = ::open(path.c_str(),O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,0600);
			if (fd < 0) {
				if (errno == EEXIST) err = "the file already exists, nothing was written. Pick another name";
				else err = strerror(errno);
				return nullptr;
			}
			auto f = std::make_shared<Out>();
			f->fd = fd;
			f->path = path;
			uid_t u;
			gid_t g;
			std::string home;
			if (whoStarted(u,g,home) && fchown(fd,u,g) != 0) appLog("[!] Dump : could not give the file to its user");
			return f;
		}

		bool write(Out& f,const void* p,size_t n,std::string& err) {
			const char* c = (const char*)p;
			while (n) {
				ssize_t k = ::write(f.fd,c,n);
				if (k < 0 && errno == EINTR) continue;
				if (k <= 0) {
					err = k < 0 ? strerror(errno) : "disk full";
					return false;
				}
				c += k;
				n -= (size_t)k;
			}
			return true;
		}

		uint64_t close(Out& f,std::string&) {
			struct stat st;
			uint64_t r = fstat(f.fd,&st) == 0 ? (uint64_t)st.st_size : NA;
			::close(f.fd);
			f.fd = -1;
			return r;
		}

		void discard(Out& f) {
			::close(f.fd);
			f.fd = -1;
			unlink(f.path.c_str());
		}

		bool exists(const std::string& p) {
			struct stat st;
			return lstat(p.c_str(),&st) == 0;
		}

		bool pathOk(const std::string& p,std::string& err) {
			err = "path must be a full file path like /home/me/x.bin";
			if (p.size() < 2 || p[0] != '/' || p.back() == '/') return false;
			for (size_t i = 0; i < p.size(); i++) {
				unsigned char c = (unsigned char)p[i];
				if (c < 32) {
					err = "path has a control character";
					return false;
				}
				if (c == '/' && p.compare(i + 1,2,"..") == 0 && (i + 3 >= p.size() || p[i + 3] == '/')) {
					err = "path can't contain ..";
					return false;
				}
			}
			err.clear();
			return true;
		}

		std::string defaultDir() {
			uid_t u;
			gid_t g;
			std::string home;
			if (!whoStarted(u,g,home) || home.empty()) {
				const char* h = getenv("HOME");
				if (h && *h) home = h;
				else userOf(getuid(),u,g,home);
			}
			if (home.empty()) return "/tmp";
			std::string d = home + "/Desktop";
			struct stat st;
			return stat(d.c_str(),&st) == 0 && S_ISDIR(st.st_mode) ? d : home;
		}

		char sep() {
			return '/';
		}

		uint64_t freeDisk(const std::string& path) {
			size_t k = path.find_last_of('/');
			if (k == std::string::npos) return NA;
			std::string d = k ? path.substr(0,k) : "/";
			struct statvfs v;
			if (statvfs(d.c_str(),&v) != 0) return NA;
			return (uint64_t)v.f_bavail * v.f_frsize;
		}

	}

	// gcore -o <prefix> <pid> writes <prefix>.<pid> and overwrites what is there. So <path>.<pid> is taken first with O_EXCL,
	// gcore writes into a new private folder next to it and that file then replaces the empty one.
	DumpRes writeDump(const ProcRef& r,const std::string& path,uint32_t,bool,Prog& pg,const StopFn& stop) {
		DumpRes res;
		std::string err,pid = std::to_string(r.pid);
		std::string out = path + "." + pid;
		res.path = out;
		if (r.pid == (uint32_t)getpid()) {
			res.err = "FRS can't dump itself, gcore would stop the process that waits for it";
			return res;
		}
		if (!os::pathOk(path,err)) {
			res.err = err;
			return res;
		}
		if (!dumpAvail()) {
			res.err = "gcore was not found, it comes with gdb. Install gdb, or save a region instead";
			return res;
		}
		if (!os::open(r,err)) {
			res.err = err;
			return res;
		}
		uint64_t est = readableBytes(r);
		auto f = os::create(out,err);
		if (!f) {
			res.err = err;
			return res;
		}
		std::string dir = out + ".XXXXXX";
		if (!mkdtemp(&dir[0])) {
			res.err = strerror(errno);
			os::discard(*f);
			return res;
		}
		std::string prefix = dir + "/core";
		std::string core = prefix + "." + pid;
		const char* av[] = {gcorePath().c_str(),"-o",prefix.c_str(),pid.c_str(),nullptr};
		char sb[1024];
		bool ran = !readStat(r.pid,sb) && stateOf(sb) != 'T' && stateOf(sb) != 't';
		pid_t kid = fork();
		if (kid == 0) {
			setpgid(0,0);
			umask(077);
			int n = ::open("/dev/null",O_RDWR);
			if (n >= 0) {
				dup2(n,0);
				dup2(n,1);
				dup2(n,2);
			}
			for (int i = 3; i < 4096; i++) ::close(i);
			execv(av[0],(char* const*)av);
			_exit(127);
		}
		auto drop = [&] {
			unlink(core.c_str());
			rmdir(dir.c_str());
		};
		if (kid < 0) {
			res.err = strerror(errno);
			drop();
			os::discard(*f);
			return res;
		}
		setpgid(kid,kid);
		pg.say("Writing a core file with gcore, the process is stopped meanwhile...",0.0f);
		const char* why = nullptr;
		uint64_t hardAt = 0;
		bool killed = false;
		int st = 0;
		for (;;) {
			pid_t w = waitpid(kid,&st,WNOHANG);
			if (w == kid) break;
			if (w < 0 && errno != EINTR) {
				st = -1;
				break;
			}
			if (!why && stop && (why = stop())) {
				kill(-kid,SIGTERM);
				hardAt = util::nowMs() + 3000;
			} else if (hardAt && util::nowMs() >= hardAt) {
				kill(-kid,SIGKILL);
				hardAt = 0;
				killed = true;
			}
			struct stat s;
			if (est != NA && est && stat(core.c_str(),&s) == 0) pg.frac = (float)std::min(0.99,(double)s.st_size / (double)est);
			usleep(50000);
		}
		if (why) {
			if (killed && ran) unstop(r);
			drop();
			os::discard(*f);
			res.err = why;
			res.cancelled = true;
			return res;
		}
		struct stat s;
		if (st == -1 || !WIFEXITED(st) || WEXITSTATUS(st) != 0 || stat(core.c_str(),&s) != 0 || !s.st_size) {
			res.err = st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 127 ? "gcore could not be started" : "gcore failed. It needs ptrace access : run FRS as root, or check kernel.yama.ptrace_scope. A full disk does this too";
			drop();
			os::discard(*f);
			return res;
		}
		err.clear();
		os::close(*f,err);
		// still inside the private folder, a link planted at out can't redirect the chown
		uid_t u;
		gid_t g;
		std::string home;
		if (whoStarted(u,g,home) && chown(core.c_str(),u,g) != 0) appLog("[!] Dump : could not give the file to its user");
		if (rename(core.c_str(),out.c_str()) != 0) {
			res.err = strerror(errno);
			unlink(out.c_str());
			drop();
			return res;
		}
		rmdir(dir.c_str());
		res.size = (uint64_t)s.st_size;
		res.ok = true;
		res.note = "Core file from gcore, it adds .<pid> to the name. Presets and flags are minidump options, they don't apply";
		pg.say("Core file complete.",1.0f);
		return res;
	}

}
#endif // _WIN32
