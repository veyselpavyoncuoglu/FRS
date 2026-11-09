#ifndef _WIN32
#include "vmem.h"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

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

	using namespace vmem;

	const uint64_t PG = 0x1000;
	const uint64_t TOP = 0x7ffffffff000ULL;

	bool slurp(const std::string& path,std::string& out,int* why = nullptr) {
		out.clear();
		int fd = open(path.c_str(),O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			if (why) *why = errno;
			return false;
		}
		char b[65536];
		for (;;) {
			ssize_t k = read(fd,b,sizeof(b));
			if (k < 0 && errno == EINTR) continue;
			if (k <= 0) {
				if (k < 0 && why) *why = errno;
				close(fd);
				return k == 0;
			}
			out.append(b,(size_t)k);
		}
	}

	// field n of /proc/pid/stat, numbered like proc(5). The name can hold spaces, so count after the last )
	bool statField(uint32_t pid,int n,uint64_t& out) {
		std::string s;
		if (!slurp("/proc/" + std::to_string(pid) + "/stat",s)) return false;
		size_t p = s.rfind(')');
		if (p == std::string::npos) return false;
		const char* c = s.c_str() + p + 1;
		for (int f = 3; ; f++) {
			while (*c == ' ') c++;
			if (!*c) return false;
			if (f == n) {
				out = strtoull(c,nullptr,10);
				return true;
			}
			while (*c && *c != ' ') c++;
		}
	}

	std::string noAccess(int e) {
		if (e == ENOENT || e == ESRCH) return "the process has exited";
		if (e == EACCES || e == EPERM) return "no access, this needs root or ptrace rights over the process (kernel.yama.ptrace_scope)";
		return std::string("error ") + strerror(e);
	}

	bool sameProc(const ProcRef& r,std::string& err) {
		uint64_t st = 0;
		if (!statField(r.pid,22,st)) {
			err = "the process has exited";
			return false;
		}
		if (r.start && st != r.start) {
			err = "the process has exited, its pid was reused";
			return false;
		}
		return true;
	}

	struct Vma {
		uint64_t lo = 0,hi = 0,inode = 0;
		char perms[8] = {};
		unsigned mj = 0,mn = 0;
		std::string path;
		bool r() const { return perms[0] == 'r'; }
		bool w() const { return perms[1] == 'w'; }
		bool x() const { return perms[2] == 'x'; }
		bool shared() const { return perms[3] == 's'; }
	};

	bool parseHead(const char* line,const char* end,Vma& m) {
		unsigned long long lo,hi,off,ino;
		unsigned mj,mn;
		int used = 0;
		char p[8];
		// glibc sscanf takes the length of its input, and line points into the whole smaps text
		char b[128];
		size_t bn = std::min((size_t)(end - line),sizeof(b) - 1);
		memcpy(b,line,bn);
		b[bn] = 0;
		if (sscanf(b,"%llx-%llx %7s %llx %x:%x %llu%n",&lo,&hi,p,&off,&mj,&mn,&ino,&used) < 7) return false;
		m.lo = lo;
		m.hi = hi;
		m.inode = ino;
		m.mj = mj;
		m.mn = mn;
		memcpy(m.perms,p,sizeof(p));
		const char* c = line + used;
		while (c < end && *c == ' ') c++;
		m.path = util::safe(std::string(c,(size_t)(end - c)));
		return true;
	}

	bool loadMaps(uint32_t pid,std::vector<Vma>& out,std::string& err) {
		std::string s;
		int why = 0;
		if (!slurp("/proc/" + std::to_string(pid) + "/maps",s,&why)) {
			err = noAccess(why);
			return false;
		}
		for (const char* c = s.c_str(); *c;) {
			const char* e = strchr(c,'\n');
			if (!e) e = c + strlen(c);
			Vma m;
			if (parseHead(c,e,m)) out.push_back(std::move(m));
			c = *e ? e + 1 : e;
		}
		return true;
	}

	std::string protText(const Vma& m,bool fileBacked) {
		if (!m.r() && !m.w() && !m.x()) return "NA";
		std::string s;
		bool cow = m.w() && !m.shared() && fileBacked;
		if (cow) s = "WC";
		else {
			if (m.r()) s += "R";
			if (m.w()) s += "W";
		}
		if (m.x()) s += "X";
		if (m.shared()) s += "+S";
		return s;
	}

	bool isFile(const Vma& m) {
		return m.inode != 0 && !m.path.empty() && m.path[0] != '[';
	}

	struct SmapVals {
		uint64_t rss = NA,privClean = 0,privDirty = 0,shClean = 0,shDirty = 0,locked = 0,anon = 0;
		std::string flags;
	};

	void readVal(const char* c,const char* e,SmapVals& v) {
		const char* k = strchr(c,':');
		if (!k || k >= e) return;
		size_t kl = (size_t)(k - c);
		if (kl == 7 && !strncmp(c,"VmFlags",7)) {
			const char* p = k + 1;
			while (p < e && *p == ' ') p++;
			v.flags.assign(p,(size_t)(e - p));
			while (!v.flags.empty() && v.flags.back() == ' ') v.flags.pop_back();
			return;
		}
		uint64_t val = strtoull(k + 1,nullptr,10) * 1024;
		if (kl == 3 && !strncmp(c,"Rss",3)) v.rss = val;
		else if (kl == 13 && !strncmp(c,"Private_Clean",13)) v.privClean = val;
		else if (kl == 13 && !strncmp(c,"Private_Dirty",13)) v.privDirty = val;
		else if (kl == 12 && !strncmp(c,"Shared_Clean",12)) v.shClean = val;
		else if (kl == 12 && !strncmp(c,"Shared_Dirty",12)) v.shDirty = val;
		else if (kl == 6 && !strncmp(c,"Locked",6)) v.locked = val;
		else if (kl == 9 && !strncmp(c,"Anonymous",9)) v.anon = val;
	}

	bool hasFlag(const std::string& flags,const char* f) {
		std::string pad = " " + flags + " ";
		return pad.find(std::string(" ") + f + " ") != std::string::npos;
	}

	int findRow(const std::vector<Region>& v,uint64_t a) {
		auto it = std::upper_bound(v.begin(),v.end(),a,[](uint64_t x,const Region& r) { return x < r.base; });
		if (it == v.begin()) return -1;
		--it;
		return a - it->base < it->size ? (int)(it - v.begin()) : -1;
	}

	void addFree(std::vector<Region>& v,uint64_t lo,uint64_t hi) {
		if (lo >= hi) return;
		Region f;
		f.base = f.allocBase = lo;
		f.size = hi - lo;
		f.state = StFree;
		v.push_back(std::move(f));
	}

	void classifyRow(const Vma& m,Region& x,bool groupExec) {
		const std::string& p = m.path;
		if (isFile(m)) {
			x.type = m.shared() ? TyMapped : groupExec ? TyImage : TyMapped;
			x.kind = KFile;
			x.file = x.use = p;
		} else if (p == "[heap]") {
			x.type = TyPrivate;
			x.kind = KHeap;
			x.use = p;
		} else if (p == "[stack]" || p.compare(0,7,"[stack:") == 0) {
			x.type = TyPrivate;
			x.kind = KStack;
			x.use = p;
		} else if (p == "[vdso]") {
			x.type = TyImage;
			x.kind = KUserShared;
			x.use = p;
		} else if (p == "[vvar]" || p == "[vsyscall]" || p.compare(0,6,"[vvar_") == 0) {
			x.type = TyMapped;
			x.kind = KUserShared;
			x.use = p;
		} else if (!p.empty()) {
			x.type = m.shared() ? TyMapped : TyPrivate;
			x.kind = KOther; // [anon:name] from prctl, /dev/zero and friends
			x.use = p;
		} else x.type = m.shared() ? TyMapped : TyPrivate;
	}

	std::string mayProt(const std::string& flags) {
		if (flags.empty()) return "";
		std::string s;
		if (hasFlag(flags,"mr")) s += "R";
		if (hasFlag(flags,"mw")) s += "W";
		if (hasFlag(flags,"me")) s += "X";
		return s.empty() ? "NA" : s;
	}

	// second to last field of task/TID/syscall is the stack pointer, "running" when the thread is on a CPU
	void tagThreads(uint32_t pid,std::vector<Region>& v) {
		std::string dir = "/proc/" + std::to_string(pid) + "/task";
		DIR* d = opendir(dir.c_str());
		if (!d) return;
		while (dirent* e = readdir(d)) {
			if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
			uint32_t tid = (uint32_t)strtoul(e->d_name,nullptr,10);
			std::string s;
			if (!slurp(dir + "/" + e->d_name + "/syscall",s)) continue;
			std::vector<uint64_t> f;
			for (const char* c = s.c_str(); *c;) {
				while (*c == ' ' || *c == '\n') c++;
				if (!*c) break;
				char* nx = nullptr;
				f.push_back(strtoull(c,&nx,0));
				if (nx == c) break;
				c = nx;
			}
			if (f.size() < 3) continue;
			int i = findRow(v,f[f.size() - 2]);
			if (i < 0) continue;
			if (v[i].kind == KNone) {
				v[i].kind = KStack;
				v[i].use = "Stack (thread " + std::to_string(tid) + ")";
				v[i].tid = tid;
			} else if (v[i].kind == KStack && !v[i].tid) v[i].tid = tid;
		}
		closedir(d);
	}

	ssize_t readRemote(uint32_t pid,int& memFd,uint64_t a,uint8_t* out,size_t n) {
		iovec lo = {out,n};
		iovec re = {(void*)(uintptr_t)a,n};
		ssize_t k = process_vm_readv((pid_t)pid,&lo,1,&re,1,0);
		if (k >= 0 || errno != ENOSYS) return k;
		if (memFd == -2) memFd = open(("/proc/" + std::to_string(pid) + "/mem").c_str(),O_RDONLY | O_CLOEXEC);
		if (memFd < 0) return -1;
		return pread(memFd,out,n,(off_t)a);
	}

}

namespace vmem {
	namespace be {

		void regions(const ProcRef& r,RegionList& out,const std::atomic<bool>* stop) {
			if (!sameProc(r,out.err)) return;
			std::string base = "/proc/" + std::to_string(r.pid);
			std::string s;
			int why = 0;
			bool smaps = slurp(base + "/smaps",s,&why);
			if (!smaps && !slurp(base + "/maps",s,&why)) {
				out.err = noAccess(why);
				return;
			}
			std::vector<Vma> ms;
			std::vector<SmapVals> vals;
			for (const char* c = s.c_str(); *c;) {
				const char* e = strchr(c,'\n');
				if (!e) e = c + strlen(c);
				Vma m;
				if (parseHead(c,e,m)) {
					ms.push_back(std::move(m));
					vals.emplace_back();
				} else if (!vals.empty()) readVal(c,e,vals.back());
				c = *e ? e + 1 : e;
			}
			if (ms.empty()) {
				// kernel threads have no maps, a zombie neither
				out.note = "This process has no user memory mappings.";
				return;
			}
			uint64_t prevEnd = 0;
			for (size_t i = 0; i < ms.size();) {
				if (stop && *stop) return;
				// a mapped file is one allocation as long as its VMAs follow each other
				size_t j = i + 1;
				bool ex = ms[i].x();
				if (isFile(ms[i])) {
					while (j < ms.size() && ms[j].inode == ms[i].inode && ms[j].path == ms[i].path && ms[j].mj == ms[i].mj && ms[j].mn == ms[i].mn) {
						ex = ex || ms[j].x();
						j++;
					}
				}
				for (size_t k = i; k < j; k++) {
					const Vma& m = ms[k];
					const SmapVals& a = vals[k];
					if (m.lo > prevEnd && prevEnd < TOP) addFree(out.rows,prevEnd,std::min(m.lo,TOP));
					prevEnd = m.hi;
					Region x;
					x.base = m.lo;
					x.allocBase = ms[i].lo;
					x.size = m.hi - m.lo;
					bool none = !m.r() && !m.w() && !m.x() && !m.shared();
					x.state = none ? StReserve : StCommit;
					x.rd = m.r();
					x.wr = m.w();
					x.ex = m.x();
					x.prot = protText(m,isFile(m));
					classifyRow(m,x,ex);
					x.committed = none ? 0 : x.size;
					x.flags = a.flags;
					x.allocProt = mayProt(a.flags);
					if (hasFlag(a.flags,"ss")) {
						x.kind = KShadow;
						x.use = "Shadow stack";
					}
					if (smaps && a.rss != NA) {
						x.ws = a.rss;
						x.wsPriv = a.privClean + a.privDirty;
						x.wsShared = a.shClean + a.shDirty;
						x.wsShareable = isFile(m) || m.shared() ? a.rss : 0;
						x.wsLocked = a.locked;
						x.priv = a.anon;
						x.valid = a.rss > 0;
						if (isFile(m) && x.state == StCommit) {
							x.pagesMod = a.anon / PG;
							x.pagesOrig = a.rss >= a.anon ? (a.rss - a.anon) / PG : 0;
						}
					} else x.priv = NA;
					out.rows.push_back(std::move(x));
				}
				i = j;
			}
			if (prevEnd < TOP) addFree(out.rows,prevEnd,TOP);
			out.wsOk = smaps;
			out.useOk = true;
			tagThreads(r.pid,out.rows);
			// a pthread stack has a PROT_NONE page right below it, the closest thing to a guard flag here
			for (size_t i = 0; i + 1 < out.rows.size(); i++) {
				Region& x = out.rows[i];
				const Region& n = out.rows[i + 1];
				if (x.state == StReserve && x.kind == KNone && x.base + x.size == n.base && n.kind == KStack) {
					x.guard = true;
					x.use = "Stack guard";
					x.kind = KOther;
				}
			}
			uint64_t arg = 0;
			if (statField(r.pid,48,arg) && arg) {
				int i = findRow(out.rows,arg);
				if (i >= 0 && out.rows[i].kind == KNone) {
					out.rows[i].kind = KParams;
					out.rows[i].use = "Command line and environment";
				}
			}
			out.note = "Linux : Type, Reserved and Original protection are read from the mapping flags, not kernel facts like on Windows.";
			if (!smaps) out.note += " smaps is not readable, the working set columns show N/A.";
		}

		size_t read(const ProcRef& r,uint64_t addr,size_t n,uint8_t* out,uint8_t* map,std::string& err) {
			memset(out,0,n);
			uint64_t p0 = addr & ~(PG - 1);
			size_t pages = (size_t)((addr + n + PG - 1 - p0) >> 12);
			if (map) memset(map,0,pages);
			if (!n || addr + n < addr || !sameProc(r,err)) return 0;
			std::vector<Vma> ms;
			if (!loadMaps(r.pid,ms,err)) return 0;
			int memFd = -2;
			size_t got = 0;
			uint64_t end = addr + n;
			bool denied = false;
			for (auto& m : ms) {
				if (m.hi <= addr || m.lo >= end || !m.r()) continue;
				uint64_t cur = std::max(addr,m.lo);
				uint64_t stop = std::min(end,m.hi);
				uint8_t kind = m.w() ? 2 : 1;
				while (cur < stop) {
					ssize_t k = readRemote(r.pid,memFd,cur,out + (cur - addr),(size_t)(stop - cur));
					if (k < 0 && (errno == EPERM || errno == EACCES)) denied = true;
					if (k > 0) {
						if (map) for (uint64_t q = cur & ~(PG - 1); q < cur + (uint64_t)k; q += PG) map[(q - p0) >> 12] = kind;
						got += (size_t)k;
						cur += (uint64_t)k;
					}
					// a hole in the VMA, go on after it
					if (k <= 0) cur = (cur + PG) & ~(PG - 1);
				}
			}
			if (memFd >= 0) close(memFd);
			if (!got && denied) err = noAccess(EPERM);
			return got;
		}

		size_t write(const ProcRef& r,uint64_t addr,const uint8_t* data,size_t n,std::string& err) {
			if (!sameProc(r,err)) return 0;
			iovec lo = {(void*)data,n};
			iovec re = {(void*)(uintptr_t)addr,n};
			// not /proc/pid/mem, that one writes through page protection
			ssize_t k = process_vm_writev((pid_t)r.pid,&lo,1,&re,1,0);
			if (k < 0) {
				err = errno == EFAULT ? "a page is not writable, and Linux has no remote protect" : noAccess(errno);
				return 0;
			}
			if ((size_t)k < n) err = "a page is not writable, and Linux has no remote protect";
			return (size_t)k;
		}

		bool regionAt(const ProcRef& r,uint64_t addr,Region& out,std::string& err) {
			if (!sameProc(r,err)) return false;
			std::vector<Vma> ms;
			if (!loadMaps(r.pid,ms,err)) return false;
			uint64_t prev = 0;
			for (auto& m : ms) {
				if (addr >= m.lo && addr < m.hi) {
					out.base = out.allocBase = m.lo;
					out.size = m.hi - m.lo;
					bool none = !m.r() && !m.w() && !m.x() && !m.shared();
					out.state = none ? StReserve : StCommit;
					out.rd = m.r();
					out.wr = m.w();
					out.ex = m.x();
					out.prot = protText(m,isFile(m));
					classifyRow(m,out,m.x());
					out.committed = none ? 0 : out.size;
					return true;
				}
				if (addr < m.lo) {
					out.base = out.allocBase = prev;
					out.size = m.lo - prev;
					out.state = StFree;
					return true;
				}
				prev = m.hi;
			}
			if (addr >= TOP) {
				err = "that address is outside user space";
				return false;
			}
			out.base = out.allocBase = prev;
			out.size = TOP - prev;
			out.state = StFree;
			return true;
		}

		bool protect(const ProcRef&,uint64_t,uint64_t&,uint32_t,std::string&,std::string& err) {
			err = "not available on Linux, there is no remote mprotect without injecting code with ptrace";
			return false;
		}

		bool parseProt(const std::string&,uint32_t&) {
			return false;
		}

		const char* protHelp() {
			return "not available on Linux";
		}

		bool freeMem(const ProcRef&,uint64_t,uint64_t,bool,std::string&,std::string& err) {
			err = "not available on Linux, there is no remote munmap or madvise(MADV_DONTNEED) without injecting code with ptrace";
			return false;
		}

		bool emptyWs(const ProcRef& r,uint64_t addr,uint64_t size,std::string&,std::string& err) {
			if (!sameProc(r,err)) return false;
			long pfd = syscall(SYS_pidfd_open,r.pid,0);
			if (pfd < 0) {
				err = errno == ENOSYS ? "needs Linux 5.10 or newer" : noAccess(errno);
				return false;
			}
			// one call stops at MAX_RW_COUNT, about 2 GiB, and returns what it did
			unsigned long long p = addr,end = addr + size;
			long k = 1;
			int e = 0;
			while (p < end && k > 0) {
				iovec v = {(void*)(uintptr_t)p,(size_t)(end - p)};
				k = syscall(SYS_process_madvise,pfd,&v,1,MADV_PAGEOUT,0);
				if (k > 0) p += (unsigned long long)k;
				else if (k < 0) e = errno;
			}
			close((int)pfd);
			if (k >= 0) return true;
			if (e == ENOSYS) err = "needs Linux 5.10 or newer";
			else if (e == EPERM || e == EACCES) err = "this needs root, CAP_SYS_NICE and ptrace read over the process";
			else if (e == EINVAL) err = "the kernel refused this mapping, a locked or special one can't be paged out";
			else err = std::string("error ") + strerror(e);
			return false;
		}

		bool canChange() {
			return false;
		}

		std::string critical(const ProcRef& r) {
			return r.pid == 1 ? "init, the kernel panics when it dies, FRS does not read or change its memory over MCP" : "";
		}

		bool sections(const ProcRef& r,std::vector<Section>& out,std::string& err,const std::atomic<bool>* stop) {
			if (!sameProc(r,err)) return false;
			std::vector<Vma> ms;
			if (!loadMaps(r.pid,ms,err)) return false;
			std::string base = "/proc/" + std::to_string(r.pid) + "/map_files/";
			for (size_t i = 0; i < ms.size();) {
				if (stop && *stop) return false;
				const Vma& m = ms[i];
				if (!isFile(m)) {
					i++;
					continue;
				}
				Section s;
				s.handle = m.inode;
				s.name = s.file = s.best = m.path;
				s.addr = m.lo;
				bool ex = m.x(),sh = m.shared();
				size_t j = i + 1;
				while (j < ms.size() && ms[j].inode == m.inode && ms[j].path == m.path) {
					ex = ex || ms[j].x();
					sh = sh || ms[j].shared();
					j++;
				}
				s.view = ms[j - 1].hi - m.lo;
				bool shm = m.path.compare(0,9,"/dev/shm/") == 0 || m.path.compare(0,7,"/memfd:") == 0 || m.path.compare(0,5,"/SYSV") == 0;
				s.type = shm ? "Shared memory" : ex ? "Image" : "File";
				char rng[64];
				snprintf(rng,sizeof(rng),"%llx-%llx",(unsigned long long)m.lo,(unsigned long long)m.hi);
				struct stat st;
				if (stat((base + rng).c_str(),&st) == 0 || stat(m.path.c_str(),&st) == 0) s.size = (uint64_t)st.st_size;
				bool seen = false;
				for (auto& o : out) if (o.handle == s.handle && o.name == s.name) seen = true;
				if (!seen) out.push_back(std::move(s));
				i = j;
			}
			return true;
		}

		bool findMapped(const std::string& needle,std::vector<MapUser>& out,int& unreadable,const std::atomic<bool>* stop) {
			auto l = procs::latest();
			unreadable = 0;
			for (auto& pr : l->rows) {
				if (stop && *stop) return false;
				if (pr.pseudo) continue;
				std::vector<Vma> ms;
				std::string e;
				if (!loadMaps(pr.pid,ms,e)) {
					unreadable++;
					continue;
				}
				for (size_t i = 0; i < ms.size();) {
					size_t j = i + 1;
					if (!isFile(ms[i]) || !util::has(ms[i].path,needle)) {
						i = j;
						continue;
					}
					bool ex = ms[i].x();
					while (j < ms.size() && ms[j].inode == ms[i].inode && ms[j].path == ms[i].path) {
						ex = ex || ms[j].x();
						j++;
					}
					MapUser u;
					u.pid = pr.pid;
					u.start = pr.start;
					u.name = pr.name;
					u.path = ms[i].path;
					u.lo = ms[i].lo;
					u.hi = ms[j - 1].hi;
					u.view = ex ? "Image" : "Mapped";
					out.push_back(std::move(u));
					i = j;
				}
			}
			return true;
		}

		bool mapSection(const ProcRef&,uint64_t,uint64_t&,uint64_t&,bool&,std::string& err) {
			err = "not used on Linux, the editor opens on the mapping in the process";
			return false;
		}

		void unmapSection(uint64_t) {}

		ProcRef self() {
			ProcRef r;
			r.pid = (uint32_t)getpid();
			r.name = "Section";
			return r;
		}

		void* fileOpen(const std::string& path,bool over,int& rc,std::string& err,bool priv) {
			rc = 0;
			int fd = open(path.c_str(),O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,priv ? 0600 : 0644);
			if (fd >= 0) {
				// a new file made as root under pkexec or sudo goes to the user whose home it is in, an overwritten one keeps its owner
				const char* s = getenv("PKEXEC_UID");
				if (!s) s = getenv("SUDO_UID");
				if (geteuid() == 0 && s && *s) {
					uid_t u = (uid_t)strtoul(s,nullptr,10);
					gid_t g = (gid_t)-1;
					char b[4096];
					passwd pw;
					passwd* res = nullptr;
					if (getpwuid_r(u,&pw,b,sizeof(b),&res) == 0 && res) g = res->pw_gid;
					if (fchown(fd,u,g) != 0) appLog("[!] Save : could not give the file to its user");
				}
				return (void*)(intptr_t)(fd + 1);
			}
			if (errno == EEXIST && over) {
				fd = open(path.c_str(),O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
				if (fd >= 0) {
					if (priv && fchmod(fd,0600) != 0) appLog("[!] Save : could not make the file owner only");
					return (void*)(intptr_t)(fd + 1);
				}
			}
			if (errno == EEXIST) {
				rc = 1;
				return nullptr;
			}
			rc = -1;
			err = errno == ENOENT ? "the folder does not exist" : errno == EACCES ? "access denied" : std::string("error ") + strerror(errno);
			return nullptr;
		}

		bool fileWrite(void* f,const void* data,size_t n) {
			int fd = (int)(intptr_t)f - 1;
			const char* c = (const char*)data;
			while (n) {
				ssize_t k = ::write(fd,c,n);
				if (k < 0 && errno == EINTR) continue;
				if (k <= 0) return false;
				c += k;
				n -= (size_t)k;
			}
			return true;
		}

		void fileClose(void* f) {
			close((int)(intptr_t)f - 1);
		}

		std::string home() {
			const char* h = getenv("HOME");
			return h ? h : "";
		}

	}
}
#endif
