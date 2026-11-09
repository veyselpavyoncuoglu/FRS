#ifndef _WIN32
#include "procs.h"
#include "platform.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <pwd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

// glibc before 2.33 has no SYS_ name for these, the kernel decides through ENOSYS
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_process_madvise
#define SYS_process_madvise 440
#endif
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

namespace {

	// whole small /proc file into a stack buffer, these never grow past a few KB. -1 on failure.
	int readAll(const char* path,char* b,int n) {
		int fd = open(path,O_RDONLY | O_CLOEXEC);
		if (fd < 0) return -1;
		int got = 0;
		while (got < n - 1) {
			ssize_t k = read(fd,b + got,(size_t)(n - 1 - got));
			if (k < 0 && errno == EINTR) continue;
			if (k <= 0) break;
			got += (int)k;
		}
		close(fd);
		b[got] = 0;
		return got;
	}

	// value of a "Key:  123 kB" line in bytes, NA when the line is not there
	uint64_t kbLine(const char* text,const char* key) {
		size_t kl = strlen(key);
		for (const char* p = text; p && *p; p = strchr(p,'\n'),p = p ? p + 1 : nullptr) {
			if (strncmp(p,key,kl) == 0 && p[kl] == ':') return strtoull(p + kl + 1,nullptr,10) * 1024;
		}
		return NA;
	}

	std::string textLine(const char* text,const char* key) {
		size_t kl = strlen(key);
		for (const char* p = text; p && *p; p = strchr(p,'\n'),p = p ? p + 1 : nullptr) {
			if (strncmp(p,key,kl) != 0 || p[kl] != ':') continue;
			p += kl + 1;
			while (*p == ' ' || *p == '\t') p++;
			const char* e = strchr(p,'\n');
			return e ? std::string(p,(size_t)(e - p)) : std::string(p);
		}
		return "";
	}

	uint64_t addKnown(uint64_t a,uint64_t b) {
		return a == NA || b == NA ? NA : a + b;
	}

	// cgroup2 mount root and mount point, read once. Hybrid systems mount it at /sys/fs/cgroup/unified
	const std::pair<std::string,std::string>& cg2Mount() {
		static const std::pair<std::string,std::string> m = [] {
			std::pair<std::string,std::string> r("/","/sys/fs/cgroup");
			FILE* f = fopen("/proc/self/mountinfo","re");
			if (!f) return r;
			char ln[4096],root[1024],mnt[1024];
			while (fgets(ln,sizeof(ln),f)) {
				const char* d = strstr(ln," - ");
				if (!d || strncmp(d + 3,"cgroup2 ",8)) continue;
				if (sscanf(ln,"%*s %*s %*s %1023s %1023s",root,mnt) == 2) r = {root,mnt};
				break;
			}
			fclose(f);
			return r;
		}();
		return m;
	}

	struct Stat {
		char state = 0;
		uint32_t ppid = 0,session = 0;
		unsigned long flags = 0;
		uint64_t minflt = 0,majflt = 0,start = 0;
		int threads = 0;
	};

	// the name in field 2 can hold spaces and parens, parse from the last ')'
	bool readStat(uint32_t pid,Stat& s) {
		char path[64],b[1024];
		snprintf(path,sizeof(path),"/proc/%u/stat",pid);
		if (readAll(path,b,sizeof(b)) <= 0) return false;
		const char* p = strrchr(b,')');
		if (!p) return false;
		unsigned long minflt = 0,majflt = 0;
		long threads = 0;
		unsigned long long start = 0;
		int n = sscanf(p + 1," %c %u %*d %u %*d %*d %lu %lu %*u %lu %*u %*u %*u %*d %*d %*d %*d %ld %*d %llu",
			&s.state,&s.ppid,&s.session,&s.flags,&minflt,&majflt,&threads,&start);
		if (n != 8) return false;
		s.minflt = minflt;
		s.majflt = majflt;
		s.threads = (int)threads;
		s.start = start;
		return true;
	}

	std::string userOf(uid_t uid) {
		static std::map<uid_t,std::string> cache; // worker thread only
		auto it = cache.find(uid);
		if (it != cache.end()) return it->second;
		std::string n = std::to_string(uid);
		char b[4096];
		passwd pw;
		passwd* res = nullptr;
		if (getpwuid_r(uid,&pw,b,sizeof(b),&res) == 0 && res) n = util::safe(res->pw_name);
		cache[uid] = n;
		return n;
	}

	std::string errText(int e) {
		if (e == EPERM || e == EACCES) return "permission denied, needs root or CAP_SYS_NICE";
		if (e == ENOSYS) return "needs Linux 5.10 or newer";
		if (e == ESRCH) return "process exited";
		return strerror(e);
	}

}

namespace procs { namespace be {

	bool snapshot(std::vector<ProcRow>& out,std::string& err) {
		DIR* d = opendir("/proc");
		if (!d) {
			err = strerror(errno);
			return false;
		}
		const size_t KT = 0x00200000; // PF_KTHREAD
		while (dirent* e = readdir(d)) {
			char* end;
			unsigned long pid = strtoul(e->d_name,&end,10);
			if (*end || !pid) continue;
			Stat st;
			if (!readStat((uint32_t)pid,st)) continue;
			char path[64],b[8192];
			snprintf(path,sizeof(path),"/proc/%lu/status",pid);
			if (readAll(path,b,sizeof(b)) <= 0) continue;
			ProcRow r;
			r.pid = (uint32_t)pid;
			r.ppid = st.ppid;
			r.session = st.session;
			r.start = st.start;
			r.faults = st.minflt + st.majflt;
			r.hardFaults = st.majflt;
			r.threads = st.threads;
			r.name = util::safe(textLine(b,"Name"));
			if (r.name.empty()) r.name = "pid " + std::to_string(pid);
			r.pseudo = (st.flags & KT) != 0;
			r.ws = kbLine(b,"VmRSS");
			r.wsPeak = kbLine(b,"VmHWM");
			r.wsPriv = kbLine(b,"RssAnon");
			uint64_t file = kbLine(b,"RssFile");
			uint64_t shm = kbLine(b,"RssShmem");
			r.wsShareable = addKnown(file,shm);
			r.sharedCommit = shm;
			// no per process commit charge here, anonymous resident plus swapped out is the usual stand-in
			r.priv = addKnown(r.wsPriv,kbLine(b,"VmSwap"));
			r.virt = kbLine(b,"VmSize");
			r.virtPeak = kbLine(b,"VmPeak");
			std::string uid = textLine(b,"Uid");
			r.uid = uid.substr(0,uid.find_first_of(" \t"));
			out.push_back(std::move(r));
		}
		closedir(d);
		if (out.empty()) err = "/proc shows no processes";
		return !out.empty();
	}

	// exec changes path and bits, setuid the user, a move the cgroup : all read again on every call
	void info(const ProcRow& r,Info& i) {
		i.statics = true;
		i.user = r.uid.empty() ? "" : userOf((uid_t)strtoul(r.uid.c_str(),nullptr,10));
		i.osProc = r.pseudo;
		if (r.pseudo) return;
		char p[64],b[4096];
		snprintf(p,sizeof(p),"/proc/%u/exe",r.pid);
		ssize_t n = readlink(p,b,sizeof(b) - 1);
		int e = errno;
		std::string path;
		if (n > 0) {
			b[n] = 0;
			path = util::safe(b);
		}
		i.noHandle = n <= 0 && (e == EACCES || e == EPERM);
		i.noFull = i.noHandle; // smaps_rollup needs the same access, Shared WS stays N/A
		if (path != i.path || !i.bits) {
			i.bits = 0;
			int fd = open(p,O_RDONLY | O_CLOEXEC);
			if (fd >= 0) {
				unsigned char h[5];
				if (read(fd,h,5) == 5 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') i.bits = h[4] == 2 ? 64 : h[4] == 1 ? 32 : 0;
				close(fd);
			}
		}
		i.path = path;
		i.cgroup.clear();
		i.jobId = NA;
		snprintf(p,sizeof(p),"/proc/%u/cgroup",r.pid);
		if (readAll(p,b,sizeof(b)) > 0) {
			const char* v2 = strncmp(b,"0::",3) ? strstr(b,"\n0::") : b;
			if (v2 && *v2 == '\n') v2++;
			const char* s = v2 ? v2 + 3 : strrchr(b,':');
			if (s && !v2) s++;
			if (s) {
				const char* e = strchr(s,'\n');
				std::string cg = e ? std::string(s,(size_t)(e - s)) : std::string(s);
				i.cgroup = util::safe(cg);
				auto& m = cg2Mount();
				if (m.first != "/" && cg.compare(0,m.first.size(),m.first) == 0 && (cg.size() == m.first.size() || cg[m.first.size()] == '/')) cg.erase(0,m.first.size());
				struct stat sb;
				if (v2 && stat((m.second + cg).c_str(),&sb) == 0) i.jobId = sb.st_ino;
			}
		}
	}

	bool wsCounters(const ProcRow& r,uint64_t& shared,uint64_t& shareable) {
		char p[64],b[4096];
		snprintf(p,sizeof(p),"/proc/%u/smaps_rollup",r.pid);
		if (readAll(p,b,sizeof(b)) <= 0) return false;
		uint64_t c = kbLine(b,"Shared_Clean");
		uint64_t d = kbLine(b,"Shared_Dirty");
		if (c == NA || d == NA) return false;
		shared = c + d;
		shareable = NA;
		return true;
	}

	void wsFree() {}

	std::string reduceWs(const ProcRow& r) {
		int fd = (int)syscall(SYS_pidfd_open,(pid_t)r.pid,0u);
		if (fd < 0) return errText(errno);
		// the fd pins this process, so check the start after opening it
		Stat st;
		if (!readStat(r.pid,st) || st.start != r.start) {
			close(fd);
			return "process exited";
		}
		char p[64];
		snprintf(p,sizeof(p),"/proc/%u/maps",r.pid);
		FILE* f = fopen(p,"r");
		if (!f) {
			int e = errno;
			close(fd);
			return errText(e);
		}
		char ln[512];
		int ok = 0,bad = 0,lastErr = 0;
		while (fgets(ln,sizeof(ln),f)) {
			unsigned long long a,b;
			if (sscanf(ln,"%llx-%llx",&a,&b) != 2) continue;
			if (strstr(ln,"[vvar]") || strstr(ln,"[vdso]") || strstr(ln,"[vsyscall]")) continue;
			// one call per range : a locked or special range fails alone, the rest still goes out.
			// One call also stops at MAX_RW_COUNT, about 2 GB, and returns what it did
			long k = 1;
			while (a < b && k > 0) {
				iovec v;
				v.iov_base = (void*)(uintptr_t)a;
				v.iov_len = (size_t)(b - a);
				k = syscall(SYS_process_madvise,fd,&v,1ul,(unsigned)MADV_PAGEOUT,0u);
				if (k > 0) a += (unsigned long long)k;
			}
			if (k >= 0) ok++;
			else {
				bad++;
				lastErr = errno;
			}
		}
		fclose(f);
		close(fd);
		if (!ok && bad) return errText(lastErr);
		return "";
	}

	std::string setPagePrio(const ProcRow&,int,int*) {
		return "not available on Linux";
	}

	uint32_t selfPid() {
		return (uint32_t)getpid();
	}

	bool invoker(std::string& user,std::string& home) {
		unsigned u,g;
		return plat::invoker(u,g,user,home);
	}

	int writeFile(const std::string& path,const std::string& data,bool overwrite,std::string& err) {
		int fd = open(path.c_str(),O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | (overwrite ? O_TRUNC : O_EXCL),0644);
		if (fd < 0) {
			if (errno == EEXIST) return 1;
			err = strerror(errno);
			return -1;
		}
		// a new file made as root stays the user's, an overwritten one keeps its owner
		unsigned u,g;
		std::string nm,hm;
		if (!overwrite && plat::invoker(u,g,nm,hm) && fchown(fd,(uid_t)u,(gid_t)g) != 0) appLog("[!] Export : could not give the file to its user");
		size_t off = 0;
		while (off < data.size()) {
			ssize_t k = write(fd,data.data() + off,data.size() - off);
			if (k < 0 && errno == EINTR) continue;
			if (k <= 0) {
				err = strerror(errno);
				close(fd);
				return -1;
			}
			off += (size_t)k;
		}
		close(fd);
		return 0;
	}

} }
#endif // _WIN32
