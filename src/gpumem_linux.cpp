#ifndef _WIN32
#include "gpumem.h"
#include <dirent.h>
#include <unistd.h>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

// sysfs has adapter totals on amdgpu. Everything per process comes from the drm keys in /proc/<pid>/fdinfo.
// Other drivers (i915, nouveau, nvidia) have no mem_info files, so their totals stay N/A.
namespace {

	const int MAXFDS = 4096;

	bool readU64(const std::string& p,uint64_t& out) {
		std::ifstream f(p);
		unsigned long long v = 0;
		if (!(f >> v)) return false;
		out = v;
		return true;
	}

	std::string readLine(const std::string& p) {
		std::ifstream f(p);
		std::string s;
		std::getline(f,s);
		return s;
	}

	std::string hexId(std::string s) {
		return s.rfind("0x",0) == 0 ? s.substr(2) : s;
	}

	std::string baseOf(const std::string& link) {
		char t[PATH_MAX];
		ssize_t n = readlink(link.c_str(),t,sizeof(t) - 1);
		if (n <= 0) return "";
		t[n] = 0;
		const char* s = strrchr(t,'/');
		return s ? s + 1 : t;
	}

	void addTo(uint64_t& v,uint64_t x) {
		v = (v == NA ? 0 : v) + x;
	}

	void scanCards(GpuSnap& s) {
		DIR* d = opendir("/sys/class/drm");
		if (!d) return;
		std::map<int,GpuAdapter> cards;
		while (dirent* e = readdir(d)) {
			int n = 0;
			int used = 0;
			if (sscanf(e->d_name,"card%d%n",&n,&used) != 1 || e->d_name[used]) continue;
			std::string dev = std::string("/sys/class/drm/") + e->d_name + "/device/";
			GpuAdapter a;
			a.id = e->d_name;
			std::string drv = baseOf(dev + "driver");
			std::string ven = hexId(readLine(dev + "vendor"));
			std::string did = hexId(readLine(dev + "device"));
			a.name = util::safe((drv.empty() ? std::string("GPU") : drv) + (ven.empty() ? "" : " " + ven + ":" + did) + " (" + e->d_name + ")");
			readU64(dev + "mem_info_vram_used",a.ded);
			readU64(dev + "mem_info_vram_total",a.dedLimit);
			readU64(dev + "mem_info_gtt_used",a.shared);
			readU64(dev + "mem_info_gtt_total",a.sharedLimit);
			a.installed = a.dedLimit;
			cards[n] = a;
		}
		closedir(d);
		for (auto& kv : cards) s.adapters.push_back(kv.second);
	}

	uint64_t parseSize(const char* v) {
		char* end = nullptr;
		unsigned long long n = strtoull(v,&end,10);
		while (end && *end == ' ') end++;
		if (end && !strncmp(end,"KiB",3)) n *= 1024ull;
		else if (end && !strncmp(end,"MiB",3)) n *= 1048576ull;
		else if (end && !strncmp(end,"GiB",3)) n *= 1073741824ull;
		return n;
	}

	bool regionIs(const char* r,const char* a,const char* b,const char* c = nullptr) {
		return !strncmp(r,a,strlen(a)) || !strncmp(r,b,strlen(b)) || (c && !strncmp(r,c,strlen(c)));
	}

	struct Fd {
		std::string client;
		bool drm = false;
		uint64_t ded = 0,shared = 0,dedTotal = 0,sharedTotal = 0,evicted = NA; // evicted only amdgpu has
	};

	// one file of drm-* keys. VRAM and local regions are dedicated, GTT and system are RAM.
	bool readFdinfo(const std::string& path,Fd& out) {
		std::ifstream f(path);
		if (!f) return false;
		std::string ln;
		std::map<std::string,uint64_t> res; // by region. amdgpu also prints drm-memory-* as an alias of drm-resident-*, the two must not add up
		while (std::getline(f,ln)) {
			size_t c = ln.find(':');
			if (c == std::string::npos) continue;
			std::string k = ln.substr(0,c);
			const char* v = ln.c_str() + c + 1;
			while (*v == ' ' || *v == '\t') v++;
			if (k == "drm-client-id") {
				out.client = v;
				out.drm = true;
			} else if (k == "amd-evicted-vram") addTo(out.evicted,parseSize(v));
			else if (!k.compare(0,13,"drm-resident-")) res[k.substr(13)] = parseSize(v);
			else if (!k.compare(0,11,"drm-memory-")) res.emplace(k.substr(11),parseSize(v));
			else if (!k.compare(0,10,"drm-total-")) {
				const char* r = k.c_str() + 10;
				if (regionIs(r,"vram","local","stolen")) out.dedTotal += parseSize(v);
				else if (regionIs(r,"gtt","system")) out.sharedTotal += parseSize(v);
			}
		}
		for (auto& kv : res) {
			const char* r = kv.first.c_str();
			if (regionIs(r,"vram","local","stolen")) out.ded += kv.second;
			else if (regionIs(r,"gtt","system")) out.shared += kv.second;
		}
		return true;
	}

	// false when the process can't be looked at
	bool scanProc(uint32_t pid,GpuProc& p,uint64_t* evicted) {
		std::string base = "/proc/" + std::to_string(pid);
		DIR* d = opendir((base + "/fd").c_str());
		if (!d) return false;
		std::set<std::string> seen;
		bool any = false;
		int n = 0;
		while (dirent* e = readdir(d)) {
			if (e->d_name[0] == '.' || ++n > MAXFDS) continue;
			char t[64];
			ssize_t len = readlink((base + "/fd/" + e->d_name).c_str(),t,sizeof(t) - 1);
			if (len <= 0) continue;
			t[len] = 0;
			if (strncmp(t,"/dev/dri/",9)) continue;
			Fd fd;
			if (!readFdinfo(base + "/fdinfo/" + e->d_name,fd) || !fd.drm || !seen.insert(fd.client).second) continue;
			any = true;
			addTo(p.ded,fd.ded);
			addTo(p.shared,fd.shared);
			addTo(p.dedCommit,fd.dedTotal);
			addTo(p.sharedCommit,fd.sharedTotal);
			if (evicted && fd.evicted != NA) addTo(*evicted,fd.evicted);
		}
		closedir(d);
		if (any) p.pid = pid;
		return true;
	}

}

namespace gpumem { namespace be {

	void read(GpuSnap& s,bool perf) {
		(void)perf;
		s.source = "sysfs";
		s.hasCommit = false;
		scanCards(s);
		auto l = procs::latest();
		for (auto& r : l->rows) {
			if (r.pseudo) continue;
			GpuProc p;
			if (!scanProc(r.pid,p,nullptr)) {
				s.denied.insert(r.pid);
				continue;
			}
			if (!p.pid) continue;
			// fdinfo has no total committed, resident plus total of both pools is not a real number
			if (p.ded || p.shared || p.dedCommit || p.sharedCommit) s.procs.push_back(p);
		}
		if (s.adapters.empty()) s.why = "no GPU found under /sys/class/drm";
	}

	GpuDetail detail(const ProcRef& r,bool adapterStats) {
		GpuDetail d;
		GpuProc p;
		uint64_t ev = NA;
		if (!scanProc(r.pid,p,&ev)) {
			d.gone = access(("/proc/" + std::to_string(r.pid)).c_str(),F_OK) != 0;
			d.why = d.gone ? "the process exited" : "can't read this process's file descriptors, this needs root or the same user";
			return d;
		}
		if (adapterStats && p.pid) d.evicted = ev;
		return d;
	}

} }
#endif
