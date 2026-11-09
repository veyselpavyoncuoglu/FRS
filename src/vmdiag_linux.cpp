#ifndef _WIN32
#include "vmdiag.h"
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>

namespace {

	using namespace vmdiag;

	const size_t MaxModPages = 200000;
	const uint64_t ScanMs = 60000;

	struct Vma {
		uint64_t start = 0,end = 0;
		char perms[8] = "";
		std::string path;
		uint64_t rss = NA,anon = NA;
	};

	bool sameProc(const ProcRef& r,std::string& why) {
		char path[64],b[1024];
		snprintf(path,sizeof(path),"/proc/%u/stat",r.pid);
		FILE* f = fopen(path,"r");
		if (!f) {
			why = "the process is gone";
			return false;
		}
		size_t n = fread(b,1,sizeof(b) - 1,f);
		fclose(f);
		b[n] = 0;
		const char* p = strrchr(b,')');
		unsigned long long st = 0;
		if (!p || sscanf(p + 1," %*c %*u %*d %*u %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",&st) != 1) {
			why = "the process is gone";
			return false;
		}
		if (r.start && st != r.start) {
			why = "the process is gone, its pid was reused";
			return false;
		}
		return true;
	}

	bool readSmaps(uint32_t pid,std::vector<Vma>& out,std::string& why) {
		char path[64];
		snprintf(path,sizeof(path),"/proc/%u/smaps",pid);
		FILE* f = fopen(path,"r");
		if (!f) {
			why = errno == ENOENT ? "the process is gone" : errno == EACCES || errno == EPERM ? "smaps denied : another user's process, FRS needs root for it" : std::string("smaps : ") + strerror(errno);
			return false;
		}
		char line[4096];
		bool any = false;
		while (fgets(line,sizeof(line),f)) {
			uint64_t a = 0,b = 0,kb = 0;
			char perm[8] = "";
			int off = 0;
			if (sscanf(line,"%" SCNx64 "-%" SCNx64 " %7s %*s %*s %*s %n",&a,&b,perm,&off) == 3) {
				Vma v;
				v.start = a;
				v.end = b;
				strncpy(v.perms,perm,sizeof(v.perms) - 1);
				if (off > 0) {
					v.path = line + off;
					while (!v.path.empty() && (v.path.back() == '\n' || v.path.back() == '\r')) v.path.pop_back();
				}
				out.push_back(std::move(v));
				any = true;
				continue;
			}
			if (out.empty()) continue;
			if (sscanf(line,"Rss: %" SCNu64 " kB",&kb) == 1) out.back().rss = kb * 1024;
			else if (sscanf(line,"Anonymous: %" SCNu64 " kB",&kb) == 1) out.back().anon = kb * 1024;
		}
		bool bad = ferror(f) != 0;
		fclose(f);
		if (!any && bad) {
			why = "smaps denied : another user's process, FRS needs root for it";
			return false;
		}
		return true;
	}

	std::string baseOf(const std::string& p) {
		size_t c = p.rfind('/');
		return c == std::string::npos ? p : p.substr(c + 1);
	}

}

namespace vmdiag {
namespace be {

	bool remote() {
		return false;
	}

	std::shared_ptr<const HeapRes> heaps(const ProcRef& r,bool) {
		auto res = std::make_shared<HeapRes>();
		res->unixSec = (uint64_t)time(nullptr);
		res->approx = true;
		std::vector<Vma> vs;
		if (!sameProc(r,res->why) || !readSmaps(r.pid,vs,res->why)) return res;
		for (auto& v : vs) if (v.path == "[heap]") {
			Heap h;
			h.base = v.start;
			h.committed = v.rss;
			h.cls = "[heap] mapping";
			h.def = true;
			res->rows.push_back(std::move(h));
		}
		res->note = "malloc arenas can't be walked from outside a process, so used, entries and flags stay N/A. Committed is the resident size of the [heap] mapping from smaps, memory malloc got through mmap is not in it.";
		res->ok = true;
		return res;
	}

	std::shared_ptr<const LockRes> locks(const ProcRef&) {
		auto res = std::make_shared<LockRes>();
		res->unixSec = (uint64_t)time(nullptr);
		res->na = true;
		res->why = "Not available on Linux : futexes can't be listed from outside a process and /proc/locks only has file locks.";
		return res;
	}

	// private file mappings with anonymous pages in them are this OS's copy-on-write hits. pagemap names the pages :
	// bit 63 present, 62 swapped, 61 file page or shared anon. Flags stay readable without root, only the PFN is blanked.
	std::shared_ptr<const ModRes> modPages(const ProcRef& r,const std::atomic<bool>& stop) {
		auto res = std::make_shared<ModRes>();
		res->unixSec = (uint64_t)time(nullptr);
		std::vector<Vma> vs;
		if (!sameProc(r,res->why) || !readSmaps(r.pid,vs,res->why)) return res;
		char path[64];
		snprintf(path,sizeof(path),"/proc/%u/pagemap",r.pid);
		int fd = open(path,O_RDONLY | O_CLOEXEC);
		long psz = sysconf(_SC_PAGESIZE);
		uint64_t ps = psz > 0 ? (uint64_t)psz : 4096;
		uint64_t end = util::nowMs() + ScanMs;
		std::map<std::string,uint64_t> baseByFile;
		for (auto& v : vs) if (!v.path.empty() && v.path[0] == '/' && !baseByFile.count(v.path)) baseByFile[v.path] = v.start;
		res->images = baseByFile.size();
		std::vector<uint64_t> buf(16384);
		bool cut = false;
		for (auto& v : vs) {
			if (cut) break;
			if (v.path.empty() || v.path[0] != '/' || v.perms[3] != 'p' || v.anon == 0) continue;
			if (stop || util::nowMs() > end) {
				res->complete = false;
				res->reason = stop ? "cancelled" : "time limit";
				break;
			}
			uint64_t base = baseByFile[v.path];
			std::string file = util::safe(baseOf(v.path));
			char off[32];
			if (fd < 0) {
				ModPage p;
				p.base = base;
				p.addr = v.start;
				p.file = file;
				p.sym = "whole mapping : " + util::count(v.anon == NA ? 0 : v.anon / ps) + " anonymous pages";
				res->rows.push_back(std::move(p));
				res->scanned += v.end - v.start;
				continue;
			}
			for (uint64_t a = v.start; a < v.end && !cut;) {
				size_t want = (size_t)std::min((v.end - a) / ps,(uint64_t)buf.size());
				ssize_t got = pread(fd,buf.data(),want * 8,(off_t)(a / ps * 8));
				if (got < 8) {
					res->fails++;
					break;
				}
				size_t n = (size_t)got / 8,i = 0;
				for (; i < n; i++) {
					uint64_t e = buf[i];
					if (!(e >> 62) || ((e >> 61) & 1)) continue; // not there, or the file's own page
					if (res->rows.size() >= MaxModPages) {
						res->complete = false;
						res->reason = "row limit";
						cut = true;
						break;
					}
					ModPage p;
					p.base = base;
					p.addr = a + i * ps;
					p.file = file;
					snprintf(off,sizeof(off),"+0x%" PRIx64,p.addr - base);
					p.sym = file + off;
					p.resident = (e >> 63) != 0;
					if (p.resident) res->residentN++;
					res->rows.push_back(std::move(p));
				}
				res->scanned += (uint64_t)i * ps;
				a += (uint64_t)n * ps;
			}
		}
		if (fd >= 0) close(fd);
		else {
			res->complete = false;
			res->reason = "pagemap denied, one row per mapping";
		}
		res->note = "Linux : anonymous pages inside private file mappings, from smaps and pagemap. Symbols are file+offset, ELF symbols are not read.";
		res->ok = true;
		return res;
	}

	std::shared_ptr<const CfgRes> cfg(const ProcRef&,const std::atomic<bool>&) {
		auto res = std::make_shared<CfgRes>();
		res->unixSec = (uint64_t)time(nullptr);
		res->na = true;
		res->why = "Not available on Linux : Control Flow Guard is a Windows feature.";
		return res;
	}

	CfgCheck cfgCheck(const ProcRef&,uint64_t) {
		CfgCheck c;
		c.why = "Not available on Linux : Control Flow Guard is a Windows feature.";
		return c;
	}

	std::shared_ptr<const GdiRes> gdi(const ProcRef&) {
		auto res = std::make_shared<GdiRes>();
		res->unixSec = (uint64_t)time(nullptr);
		res->na = true;
		res->why = "Not available on Linux : GDI is a Windows feature, X11 resources are not comparable.";
		return res;
	}

	FlushRes flush(const ProcRef&) {
		FlushRes o;
		o.na = true;
		o.why = "Not available on Linux : malloc_trim would have to run inside the target, that takes code injection.";
		return o;
	}

}
}
#endif // !_WIN32
