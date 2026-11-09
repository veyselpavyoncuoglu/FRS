#ifndef _WIN32
#include "procdet.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MADV_COLD
#define MADV_COLD 20
#endif
// glibc before 2.33 has no SYS_ name for these, the kernel decides through ENOSYS
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_process_madvise
#define SYS_process_madvise 440
#endif

namespace {

	using namespace procdet;

	// small /proc or cgroup file, false when it can't be read
	bool readFile(const std::string& path,std::string& out,size_t cap = 65536) {
		int fd = open(path.c_str(),O_RDONLY | O_CLOEXEC);
		if (fd < 0) return false;
		out.clear();
		char b[4096];
		while (out.size() < cap) {
			ssize_t k = read(fd,b,sizeof(b));
			if (k < 0 && errno == EINTR) continue;
			if (k < 0) {
				close(fd);
				return false;
			}
			if (k == 0) break;
			out.append(b,(size_t)k);
		}
		close(fd);
		return true;
	}

	// "Key:  123 kB" in bytes, NA when the line is not there
	uint64_t kbLine(const std::string& t,const char* key) {
		std::string k = std::string(key) + ":";
		size_t p = t.rfind("\n" + k);
		if (p != std::string::npos) p++;
		else if (t.compare(0,k.size(),k) == 0) p = 0;
		else return NA;
		return strtoull(t.c_str() + p + k.size(),nullptr,10) * 1024;
	}

	// "key value" line of memory.stat, NA when the line is not there
	uint64_t statVal(const std::string& t,const char* key) {
		std::string k = std::string(key) + " ";
		size_t p = t.compare(0,k.size(),k) == 0 ? 0 : t.find("\n" + k);
		if (p == std::string::npos) return NA;
		if (t[p] == '\n') p++;
		return strtoull(t.c_str() + p + k.size(),nullptr,10);
	}

	std::string errText(int e) {
		if (e == EACCES || e == EPERM) return "permission denied, needs root";
		if (e == ENOENT || e == ESRCH) return "process exited";
		if (e == ENOSYS) return "needs Linux 5.10 or newer";
		return strerror(e);
	}

	const char* V1 = "The memory controller is on cgroup v1 on this system and FRS reads cgroup v2 only, so these stay unknown.";

	// one value file of a cgroup : number, "max" means not set, missing is unknown
	uint64_t limitFile(const std::string& dir,const char* f) {
		std::string s;
		if (dir.empty() || !readFile(dir + "/" + f,s)) return NA;
		if (s.compare(0,3,"max") == 0) return NOLIM;
		if (s.empty() || s[0] < '0' || s[0] > '9') return NA;
		return strtoull(s.c_str(),nullptr,10);
	}

	// the cgroup v2 path. v1 : the memory controller sits on a v1 hierarchy (hybrid or legacy setup), so v2 has no memory.* files
	std::string cgroupOf(uint32_t pid,bool& v1) {
		v1 = false;
		std::string s,cg;
		if (!readFile("/proc/" + std::to_string(pid) + "/cgroup",s)) return "";
		size_t i = 0;
		while (i < s.size()) {
			size_t e = s.find('\n',i);
			if (e == std::string::npos) e = s.size();
			std::string ln = s.substr(i,e - i);
			i = e + 1;
			size_t a = ln.find(':');
			size_t b = a == std::string::npos ? a : ln.find(':',a + 1);
			if (b == std::string::npos) continue;
			if (ln.compare(0,3,"0::") == 0) cg = ln.substr(3);
			else if (("," + ln.substr(a + 1,b - a - 1) + ",").find(",memory,") != std::string::npos) v1 = true;
		}
		return cg;
	}

	// cgroup2 is at /sys/fs/cgroup, or /sys/fs/cgroup/unified on a hybrid setup. "" when FRS can't see it mounted
	std::string cgDir(const std::string& cg) {
		static const std::pair<std::string,std::string> m = [] {
			std::pair<std::string,std::string> x;
			std::string s;
			if (!readFile("/proc/self/mountinfo",s,1 << 22)) return x;
			size_t i = 0;
			while (i < s.size()) {
				size_t e = s.find('\n',i);
				if (e == std::string::npos) e = s.size();
				std::string ln = s.substr(i,e - i);
				i = e + 1;
				size_t d = ln.find(" - ");
				if (d == std::string::npos || ln.compare(d + 3,8,"cgroup2 ") != 0) continue;
				// id parent dev root mountpoint ...
				char root[1024],mp[1024];
				if (sscanf(ln.c_str(),"%*s %*s %*s %1023s %1023s",root,mp) != 2) continue;
				x.first = root;
				x.second = mp;
				break;
			}
			return x;
		}();
		if (m.second.empty()) return "";
		std::string p = cg;
		// a mount of a sub tree, like in a container without its own cgroup namespace
		const std::string& rt = m.first;
		if (rt != "/" && p.compare(0,rt.size(),rt) == 0 && (p.size() == rt.size() || p[rt.size()] == '/')) p = p.substr(rt.size());
		return m.second + p;
	}

	bool sameStart(const ProcRow& r) {
		std::string s;
		if (!readFile("/proc/" + std::to_string(r.pid) + "/stat",s)) return false;
		size_t p = s.rfind(')');
		if (p == std::string::npos) return false;
		unsigned long long st = 0;
		if (sscanf(s.c_str() + p + 1," %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",&st) != 1) return false;
		return st == r.start;
	}

}

namespace procdet { namespace be {

	void fast(const ProcRow& r,Det& d) {
		d.commit = Commit();
		d.quota = Quota();
		d.denied = false;
		d.why.clear();
		if (!sameStart(r)) {
			d.denied = true;
			d.why = "process exited";
			d.commit.why = d.why;
			d.quota.why = d.why;
			return;
		}
		d.quota.why = "Linux has no per process working set quota, see the cgroup limits";
		bool v1 = false;
		std::string cg = cgroupOf(r.pid,v1);
		if (cg.empty()) {
			d.commit.why = v1 ? V1 : "No cgroup v2 entry for this process.";
			d.jobs.in = -1;
		} else {
			std::string dir = cgDir(cg);
			d.commit.priv = limitFile(dir,"memory.current");
			d.commit.peak = limitFile(dir,"memory.peak");
			d.commit.privLimit = limitFile(dir,"memory.max");
			d.commit.totalLimit = limitFile(dir,"memory.swap.max");
			std::string st;
			if (!dir.empty() && readFile(dir + "/memory.stat",st)) d.commit.shared = statVal(st,"shmem");
			if (v1) d.commit.why = V1;
			else if (dir.empty()) d.commit.why = "FRS can't find where cgroup v2 is mounted.";
			d.jobs.in = 1;
		}
		std::string ksm;
		if (readFile("/proc/" + std::to_string(r.pid) + "/ksm_stat",ksm)) {
			size_t p = ksm.find("ksm_merging_pages");
			if (p != std::string::npos) d.ksm = strtoull(ksm.c_str() + p + 17,nullptr,10) * (uint64_t)sysconf(_SC_PAGESIZE);
		}
		if (!d.cmdRead) {
			d.cmd = cmdLine(r);
			d.cmdRead = !d.cmd.empty();
			if (!d.cmdRead && !r.pseudo) d.cmdWhy = "empty or not readable";
		}
	}

	void ws(const ProcRow& r,Ws& w) {
		std::string s;
		if (!readFile("/proc/" + std::to_string(r.pid) + "/smaps_rollup",s)) {
			w.why = errText(errno);
			return;
		}
		uint64_t pc = kbLine(s,"Private_Clean");
		uint64_t pd = kbLine(s,"Private_Dirty");
		uint64_t sc = kbLine(s,"Shared_Clean");
		uint64_t sd = kbLine(s,"Shared_Dirty");
		w.total = kbLine(s,"Rss");
		if (pc != NA && pd != NA) w.priv = pc + pd;
		if (sc != NA && sd != NA) w.shared = sc + sd;
		w.locked = kbLine(s,"Locked");
		w.anon = kbLine(s,"Anonymous");
		w.shareable = r.wsShareable;
	}

	void jobs(const ProcRow& r,JobView& v) {
		v.read = true;
		bool v1 = false;
		std::string cg = cgroupOf(r.pid,v1);
		if (cg.empty()) {
			v.why = v1 ? V1 : "No cgroup v2 entry for this process. A cgroup v1 system keeps its limits elsewhere, FRS reads v2 only.";
			return;
		}
		v.in = 1;
		std::string dir = cgDir(cg);
		Job j;
		j.name = util::safe(cg);
		j.via = "/proc/pid/cgroup";
		j.jobMem = limitFile(dir,"memory.max");
		j.wsMin = limitFile(dir,"memory.min");
		j.wsMax = limitFile(dir,"memory.high");
		j.peakJob = limitFile(dir,"memory.peak");
		if (j.peakJob == NOLIM) j.peakJob = NA;
		std::string st;
		if (!dir.empty() && readFile(dir + "/memory.stat",st)) j.faults = statVal(st,"pgfault");
		std::string lim;
		if (readFile("/proc/" + std::to_string(r.pid) + "/limits",lim)) {
			size_t p = lim.find("Max address space");
			if (p != std::string::npos) {
				const char* q = lim.c_str() + p + 17;
				while (*q == ' ') q++;
				j.procMem = strncmp(q,"unlimited",9) == 0 ? NOLIM : strtoull(q,nullptr,10);
			}
		}
		if (j.jobMem == NA && j.wsMin == NA && j.wsMax == NA) j.why = v1 ? V1 : dir.empty() ? "FRS can't find where cgroup v2 is mounted." : "This cgroup has no readable memory.* files, it is the root cgroup or the memory controller is off for it.";
		v.jobs.push_back(std::move(j));
	}

	void threads(const ProcRow& r,ThrView& t) {
		t.read = true;
		std::string base = "/proc/" + std::to_string(r.pid);
		DIR* d = opendir((base + "/task").c_str());
		if (!d) {
			t.why = errText(errno);
			return;
		}
		uint64_t vmStk = NA,stkMax = NA;
		std::string s;
		if (readFile(base + "/status",s)) vmStk = kbLine(s,"VmStk");
		if (readFile(base + "/limits",s)) {
			size_t p = s.find("Max stack size");
			if (p != std::string::npos) {
				const char* q = s.c_str() + p + 14;
				while (*q == ' ') q++;
				if (strncmp(q,"unlimited",9) != 0) stkMax = strtoull(q,nullptr,10);
			}
		}
		while (dirent* e = readdir(d)) {
			char* end;
			unsigned long tid = strtoul(e->d_name,&end,10);
			if (*end || !tid) continue;
			std::string dir = base + "/task/" + e->d_name;
			Thr x;
			x.tid = (uint32_t)tid;
			std::string st;
			if (readFile(dir + "/stat",st)) {
				size_t p = st.rfind(')');
				if (p != std::string::npos && p + 2 < st.size() && st[p + 2] == 'D') {
					std::string wc;
					readFile(dir + "/wchan",wc);
					x.wait = "D";
					if (!wc.empty() && wc != "0") x.wait += " " + util::safe(wc);
					static const char* mem[] = {"page","folio","swap","reclaim","compact","oom","mem","shrink"};
					for (auto m : mem) if (x.wait.find(m) != std::string::npos) {
						t.memWaits++;
						break;
					}
				}
			}
			if (tid == r.pid) {
				x.stackUsed = vmStk;
				x.stackLimit = stkMax;
			}
			t.v.push_back(std::move(x));
		}
		closedir(d);
		std::sort(t.v.begin(),t.v.end(),[](const Thr& a,const Thr& b) { return a.tid < b.tid; });
	}

	std::string setThrPrio(const ProcRow&,uint32_t,uint64_t,int,int*) {
		return "not available on Linux";
	}

	// nearest thing to a per process cap : the cgroup's soft floor and ceiling, for everything in that cgroup
	std::string setQuota(const ProcRow& r,uint64_t mn,uint64_t mx) {
		bool v1 = false;
		std::string cg = cgroupOf(r.pid,v1);
		// after the read, so the path really came from this process and not a reused pid
		if (!sameStart(r)) return "process exited";
		if (v1) return "the memory controller is on cgroup v1 on this system and FRS writes cgroup v2 only";
		if (cg.empty()) return "no cgroup v2 entry for this process";
		std::string dir = cgDir(cg);
		if (dir.empty()) return "FRS can't find where cgroup v2 is mounted";
		auto put = [&](const char* f,uint64_t v) -> std::string {
			int fd = open((dir + "/" + f).c_str(),O_WRONLY | O_CLOEXEC);
			if (fd < 0) {
				int e = errno;
				return std::string(f) + " : " + (e == ENOENT ? std::string("not there, the memory controller is off for this cgroup") : errText(e));
			}
			std::string t = std::to_string(v);
			ssize_t k = write(fd,t.data(),t.size());
			int e = errno;
			close(fd);
			return k == (ssize_t)t.size() ? "" : std::string(f) + " : " + errText(e);
		};
		std::string why = put("memory.min",mn);
		if (why.empty()) why = put("memory.high",mx);
		return why;
	}

	std::string cmdLine(const ProcRow& r) {
		std::string s;
		if (!readFile("/proc/" + std::to_string(r.pid) + "/cmdline",s,1 << 20)) return "";
		while (!s.empty() && s.back() == '\0') s.pop_back();
		std::replace(s.begin(),s.end(),'\0',' ');
		return util::safe(s);
	}

	bool now(const ProcRow& r,uint64_t& ws,uint64_t& priv) {
		if (!sameStart(r)) return false;
		std::string s;
		if (!readFile("/proc/" + std::to_string(r.pid) + "/status",s)) return false;
		ws = kbLine(s,"VmRSS");
		uint64_t an = kbLine(s,"RssAnon");
		uint64_t sw = kbLine(s,"VmSwap");
		priv = an == NA || sw == NA ? NA : an + sw;
		return ws != NA;
	}

	int pagePrio(const ProcRow&) {
		return -1;
	}

	// no page priority on Linux. A rule below Normal marks the process's pages cold once a minute, so reclaim takes them first.
	bool applyRule(const ProcRow& r,int prio,std::string& why) {
		if (prio >= 5 || r.pid == 1) return false;
		static std::map<std::pair<uint32_t,uint64_t>,uint64_t> last; // rule job only
		uint64_t now = util::nowMs();
		auto key = std::make_pair(r.pid,r.start);
		auto it = last.find(key);
		if (it != last.end() && now < it->second + 60000) return false;
		if (last.size() > 4096) last.clear();
		last[key] = now;
		int fd = (int)syscall(SYS_pidfd_open,(pid_t)r.pid,0u);
		if (fd < 0) {
			why = errText(errno);
			return true;
		}
		if (!sameStart(r)) {
			close(fd);
			why = "process exited";
			return true;
		}
		FILE* f = fopen(("/proc/" + std::to_string(r.pid) + "/maps").c_str(),"r");
		if (!f) {
			why = errText(errno);
			close(fd);
			return true;
		}
		char ln[512];
		int ok = 0,bad = 0,lastErr = 0;
		while (fgets(ln,sizeof(ln),f)) {
			unsigned long long a,b;
			if (sscanf(ln,"%llx-%llx",&a,&b) != 2) continue;
			if (strstr(ln,"[vvar]") || strstr(ln,"[vdso]") || strstr(ln,"[vsyscall]")) continue;
			// one call stops at MAX_RW_COUNT, about 2 GB, and returns what it did
			unsigned long long p = a;
			long k = 1;
			while (p < b && k > 0) {
				iovec v;
				v.iov_base = (void*)(uintptr_t)p;
				v.iov_len = (size_t)(b - p);
				k = syscall(SYS_process_madvise,fd,&v,1ul,(unsigned)MADV_COLD,0u);
				if (k > 0) p += (unsigned long long)k;
			}
			if (k >= 0 || p > a) ok++;
			else {
				bad++;
				lastErr = errno;
			}
		}
		fclose(f);
		close(fd);
		if (!ok && bad) why = errText(lastErr);
		return true;
	}

	void lowPrio() {
#ifdef SYS_ioprio_set
		const int IDLE = 3,WHO_PROCESS = 1,SHIFT = 13;
		syscall(SYS_ioprio_set,WHO_PROCESS,0,IDLE << SHIFT);
#endif
	}

	bool ifeoOk() {
		return false;
	}

	bool ifeoCanWrite() {
		return false;
	}

	int ifeoGet(const std::string&) {
		return -1;
	}

	std::string ifeoSet(const std::string&,int) {
		return "not available on Linux";
	}

	std::string ifeoDel(const std::string&,bool& had) {
		had = false;
		return "not available on Linux";
	}

	std::vector<Rule> ifeoList() {
		return std::vector<Rule>();
	}

} }
#endif // _WIN32
