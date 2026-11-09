#ifndef _WIN32
#include "wswatch.h"
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

// One perf event per thread with a ring buffer each. An inherited event can't be mapped, so threads
// that start later get picked up on the next drain. Closing the fds stops it, nothing stays behind.
namespace {

	const size_t RINGPAGES = 16; // power of two
	const size_t MAXEVS = 256; // perf fds across all targets, the usual soft fd limit is 1024 and FRS needs the rest

	struct Ev {
		int fd = -1;
		perf_event_mmap_page* meta = nullptr;
		size_t len = 0;
	};

	struct T {
		std::map<int,Ev> evs;
	};

	using Key = std::pair<uint32_t,uint64_t>;
	std::mutex g_m;
	std::map<Key,T> g_t;
	size_t g_n = 0; // open events, under g_m

	size_t pageSz() {
		return (size_t)sysconf(_SC_PAGESIZE);
	}

	std::string perfWhy(int e) {
		if (e == EACCES || e == EPERM) return "perf is blocked, it needs root, CAP_PERFMON or a lower /proc/sys/kernel/perf_event_paranoid";
		if (e == ENOSYS || e == ENOENT) return "this kernel has no perf events";
		if (e == ESRCH) return "the process exited";
		return std::string("perf_event_open : ") + strerror(e);
	}

	bool openEv(int tid,Ev& out,int& err) {
		perf_event_attr a;
		memset(&a,0,sizeof(a));
		a.type = PERF_TYPE_SOFTWARE;
		a.size = sizeof(a);
		a.config = PERF_COUNT_SW_PAGE_FAULTS;
		a.sample_period = 1;
		a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR;
		a.exclude_kernel = 1;
		a.exclude_hv = 1;
		int fd = (int)syscall(SYS_perf_event_open,&a,tid,-1,-1,PERF_FLAG_FD_CLOEXEC);
		if (fd < 0) {
			err = errno;
			return false;
		}
		size_t len = (RINGPAGES + 1) * pageSz();
		void* p = mmap(nullptr,len,PROT_READ | PROT_WRITE,MAP_SHARED,fd,0);
		if (p == MAP_FAILED) {
			err = errno;
			close(fd);
			return false;
		}
		out.fd = fd;
		out.meta = (perf_event_mmap_page*)p;
		out.len = len;
		g_n++;
		return true;
	}

	void closeEv(Ev& e) {
		if (e.meta) munmap(e.meta,e.len);
		if (e.fd >= 0) {
			close(e.fd);
			g_n--;
		}
		e = Ev();
	}

	// /proc/pid/stat starttime like procs, the name in field 2 can hold spaces and parens. NA when gone
	uint64_t startOf(uint32_t pid) {
		std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
		std::string s;
		std::getline(f,s);
		size_t i = s.rfind(')');
		if (i == std::string::npos) return NA;
		std::istringstream in(s.substr(i + 1));
		std::string x;
		for (int k = 3; k < 22 && in >> x; k++) {}
		unsigned long long v = 0;
		return in >> v ? (uint64_t)v : NA;
	}

	std::set<int> tasks(uint32_t pid) {
		std::set<int> out;
		std::string dir = "/proc/" + std::to_string(pid) + "/task";
		DIR* d = opendir(dir.c_str());
		if (!d) return out;
		while (dirent* e = readdir(d)) {
			int t = atoi(e->d_name);
			if (t > 0) out.insert(t);
		}
		closedir(d);
		return out;
	}

	// retried on every drain, returns "" or which threads are still not watched and why
	std::string syncThreads(uint32_t pid,T& t) {
		std::set<int> now = tasks(pid);
		size_t miss = 0;
		int last = 0;
		for (int tid : now) {
			if (t.evs.count(tid)) continue;
			if (g_n >= MAXEVS) {
				miss++;
				continue;
			}
			Ev e;
			int err = 0;
			if (openEv(tid,e,err)) t.evs[tid] = e;
			else if (err != ESRCH) {
				miss++;
				last = err;
			}
		}
		if (!miss) return "";
		return std::to_string(miss) + " of " + std::to_string(now.size()) + " threads are not watched, " + (last ? perfWhy(last) : "FRS watches " + std::to_string(MAXEVS) + " threads at most across all processes");
	}

	void copyOut(const char* base,uint64_t size,uint64_t pos,void* to,size_t n) {
		uint64_t off = pos % size;
		size_t first = (size_t)std::min<uint64_t>(n,size - off);
		memcpy(to,base + off,first);
		if (first < n) memcpy((char*)to + first,base,n - first);
	}

	void drainRing(Ev& e,std::vector<uint64_t>& pcs,uint64_t& lost) {
		perf_event_mmap_page* m = e.meta;
		uint64_t head = __atomic_load_n(&m->data_head,__ATOMIC_ACQUIRE);
		uint64_t tail = m->data_tail;
		uint64_t size = m->data_size ? m->data_size : RINGPAGES * pageSz();
		const char* base = (const char*)m + (m->data_offset ? m->data_offset : pageSz());
		while (tail < head) {
			perf_event_header h;
			copyOut(base,size,tail,&h,sizeof(h));
			if (h.size < sizeof(h)) {
				tail = head;
				break;
			}
			if (h.type == PERF_RECORD_SAMPLE && h.size >= sizeof(h) + sizeof(uint64_t)) {
				uint64_t ip = 0;
				copyOut(base,size,tail + sizeof(h),&ip,sizeof(ip));
				if (ip) pcs.push_back(ip);
			} else if (h.type == PERF_RECORD_LOST && h.size >= sizeof(h) + 2 * sizeof(uint64_t)) {
				uint64_t v[2] = {0,0};
				copyOut(base,size,tail + sizeof(h),v,sizeof(v));
				lost += v[1];
			}
			tail += h.size;
		}
		__atomic_store_n(&m->data_tail,tail,__ATOMIC_RELEASE);
	}

	struct Map {
		uint64_t lo,hi;
		std::string path;
	};

	std::vector<Map> readMaps(uint32_t pid) {
		std::vector<Map> out;
		std::ifstream f("/proc/" + std::to_string(pid) + "/maps");
		std::string ln;
		while (std::getline(f,ln)) {
			unsigned long long lo = 0,hi = 0;
			int off = 0;
			if (sscanf(ln.c_str(),"%llx-%llx %*s %*s %*s %*s %n",&lo,&hi,&off) < 2) continue;
			Map m;
			m.lo = lo;
			m.hi = hi;
			if (off > 0 && (size_t)off < ln.size()) m.path = ln.substr((size_t)off);
			out.push_back(std::move(m));
		}
		return out;
	}

}

namespace wswatch { namespace be {

	int drain(const ProcRef& r,std::vector<uint64_t>& pcs,uint64_t& lost,std::string& why) {
		std::lock_guard<std::mutex> lk(g_m);
		auto it = g_t.find(Key(r.pid,r.start));
		if (it == g_t.end()) return 0;
		T& t = it->second;
		// the events stay on the old threads, so what they hold is this target's. A reused pid gets nothing new attached
		for (auto& e : t.evs) drainRing(e.second,pcs,lost);
		std::set<int> now;
		if (startOf(r.pid) == r.start) {
			why = syncThreads(r.pid,t);
			now = tasks(r.pid);
		}
		if (startOf(r.pid) != r.start) {
			for (auto& e : t.evs) closeEv(e.second);
			t.evs.clear();
			why = "the process exited";
			return -1;
		}
		for (auto e = t.evs.begin(); e != t.evs.end();) {
			if (!now.count(e->first)) {
				drainRing(e->second,pcs,lost);
				closeEv(e->second);
				e = t.evs.erase(e);
			} else ++e;
		}
		return 1;
	}

	std::string enable(const ProcRef& r) {
		std::lock_guard<std::mutex> lk(g_m);
		Key k(r.pid,r.start);
		if (g_t.count(k)) return "";
		std::set<int> now = tasks(r.pid);
		if (now.empty() || startOf(r.pid) != r.start) return "the process exited";
		T t;
		int err = 0;
		for (int tid : now) {
			if (g_n >= MAXEVS) break;
			Ev e;
			if (openEv(tid,e,err)) t.evs[tid] = e;
		}
		if (t.evs.empty()) return err ? perfWhy(err) : "FRS already watches " + std::to_string(MAXEVS) + " threads across processes, stop another watch first";
		if (startOf(r.pid) != r.start) {
			for (auto& e : t.evs) closeEv(e.second);
			return "the process exited";
		}
		g_t[k] = std::move(t);
		return "";
	}

	std::string stop(const ProcRef& r) {
		std::lock_guard<std::mutex> lk(g_m);
		auto it = g_t.find(Key(r.pid,r.start));
		if (it == g_t.end()) return "";
		for (auto& e : it->second.evs) closeEv(e.second);
		g_t.erase(it);
		return "";
	}

	void lookup(const ProcRef& r,std::vector<Row>& rows) {
		std::vector<Map> maps = readMaps(r.pid);
		if (startOf(r.pid) != r.start) maps.clear(); // a reused pid's modules would name the wrong code
		for (auto& x : rows) {
			x.instr = util::hex(x.pc);
			for (auto& m : maps) {
				if (x.pc < m.lo || x.pc >= m.hi || m.path.empty()) continue;
				if (m.path[0] != '/') {
					x.instr = util::safe(m.path) + " " + x.instr;
					break;
				}
				uint64_t base = m.lo;
				for (auto& o : maps) if (o.path == m.path && o.lo < base) base = o.lo;
				x.file = util::safe(m.path);
				size_t i = x.file.rfind('/');
				char t[32];
				snprintf(t,sizeof(t),"+0x%llx",(unsigned long long)(x.pc - base));
				x.instr = x.file.substr(i + 1) + t;
				break;
			}
		}
	}

	bool permanent() {
		return false;
	}

	void openLoc(const std::string& file) {
		size_t i = file.rfind('/');
		if (i == std::string::npos) return;
		std::string dir = i ? file.substr(0,i) : "/";
		// FRS runs as root after pkexec or sudo, the file manager must open as the user who started it
		uid_t u = 0;
		gid_t g = 0;
		std::vector<std::string> env;
		const char* s = geteuid() == 0 ? getenv("PKEXEC_UID") : nullptr;
		if (geteuid() == 0 && !s) s = getenv("SUDO_UID");
		bool drop = s && *s;
		if (drop) {
			u = (uid_t)strtoul(s,nullptr,10);
			char b[4096];
			passwd pw;
			passwd* res = nullptr;
			if (getpwuid_r(u,&pw,b,sizeof(b),&res) != 0 || !res) {
				appLog("[!] Open file location : failed (no user " + std::to_string(u) + ")");
				return;
			}
			g = res->pw_gid;
			// root's identity and FRS's own switches must not reach what the user starts from the file manager
			for (char** e = environ; *e; e++) {
				std::string k(*e,strcspn(*e,"="));
				if (k == "HOME" || k == "USER" || k == "LOGNAME" || k == "SHELL" || k == "PATH" || k == "MAIL" || k == "FRS_ELEVATED" || k == "PKEXEC_UID" || !k.compare(0,5,"SUDO_")) continue;
				env.push_back(*e);
			}
			env.push_back(std::string("HOME=") + res->pw_dir);
			env.push_back(std::string("USER=") + res->pw_name);
			env.push_back(std::string("LOGNAME=") + res->pw_name);
			env.push_back(std::string("SHELL=") + (res->pw_shell && *res->pw_shell ? res->pw_shell : "/bin/sh"));
			env.push_back("PATH=/usr/local/bin:/usr/bin:/bin");
		}
		std::vector<char*> envp;
		for (auto& x : env) envp.push_back(&x[0]);
		envp.push_back(nullptr);
		char* av[] = {(char*)"xdg-open",&dir[0],nullptr};
		int fd[2];
		if (pipe2(fd,O_CLOEXEC) != 0) {
			appLog(std::string("[!] Open file location : failed (") + strerror(errno) + ")");
			return;
		}
		pid_t p = fork();
		if (p == 0) {
			// grandchild so nothing stays a zombie. It sends errno back when it can't start xdg-open
			if (fork() == 0) {
				// the MCP sockets are close on exec, this keeps any other inherited fd out of the file manager too
				for (int i = 3; i < 4096; i++) if (i != fd[1]) ::close(i);
				if (!drop) execvp(av[0],av);
				else if (setgroups(0,nullptr) == 0 && setgid(g) == 0 && setuid(u) == 0) execvpe(av[0],av,envp.data());
				int e = errno;
				if (write(fd[1],&e,sizeof(e)) < 0) {}
				_exit(127);
			}
			_exit(0);
		}
		int e = p < 0 ? errno : 0;
		close(fd[1]);
		if (p > 0) {
			waitpid(p,nullptr,0);
			if (::read(fd[0],&e,sizeof(e)) != (ssize_t)sizeof(e)) e = 0;
		}
		close(fd[0]);
		if (e) appLog(std::string("[!] Open file location : failed (") + strerror(e) + ")");
	}

} }
#endif
