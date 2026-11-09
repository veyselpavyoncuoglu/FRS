#ifndef _WIN32
#include "sysmem.h"
#include "smbios.h"
#include "ram.h"
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <strings.h>
#include <unordered_map>

namespace {

	using Map = std::unordered_map<std::string,uint64_t>;

	std::string slurp(const std::string& p) {
		std::ifstream f(p,std::ios::binary);
		if (!f) return "";
		std::stringstream ss;
		ss << f.rdbuf();
		return ss.str();
	}

	// "Key:   123 kB" -> bytes
	Map readMeminfo() {
		Map m;
		std::ifstream f("/proc/meminfo");
		std::string k,unit;
		uint64_t v = 0;
		std::string line;
		while (std::getline(f,line)) {
			std::istringstream is(line);
			unit.clear();
			if (!(is >> k >> v)) continue;
			is >> unit;
			if (!k.empty() && k.back() == ':') k.pop_back();
			m[k] = unit == "kB" ? v * 1024 : v;
		}
		return m;
	}

	Map readVmstat() {
		Map m;
		std::ifstream f("/proc/vmstat");
		std::string k;
		uint64_t v = 0;
		while (f >> k >> v) m[k] = v;
		return m;
	}

	uint64_t get(const Map& m,const char* k) {
		auto it = m.find(k);
		return it == m.end() ? NA : it->second;
	}

	uint64_t add(uint64_t a,uint64_t b) {
		return a == NA || b == NA ? NA : a + b;
	}

	bool writeSys(const char* p,const char* v,std::string& err) {
		int fd = open(p,O_WRONLY);
		if (fd < 0) {
			err = strerror(errno);
			return false;
		}
		ssize_t w = write(fd,v,strlen(v));
		int e = errno;
		close(fd);
		if (w < 0) {
			err = strerror(e);
			return false;
		}
		return true;
	}

	uint64_t readNum(const std::string& p) {
		std::string s = slurp(p);
		if (s.empty()) return NA;
		char* end = nullptr;
		unsigned long long v = strtoull(s.c_str(),&end,10);
		return end == s.c_str() ? NA : (uint64_t)v;
	}

	// set up devices only : one with disksize 0 holds nothing and compact on it fails with EINVAL
	std::vector<std::string> zramDevs() {
		std::vector<std::string> v;
		DIR* d = opendir("/sys/block");
		if (!d) return v;
		while (dirent* e = readdir(d)) {
			if (strncmp(e->d_name,"zram",4)) continue;
			std::string p = std::string("/sys/block/") + e->d_name;
			uint64_t ds = readNum(p + "/disksize");
			if (ds != NA && ds) v.push_back(p);
		}
		closedir(d);
		return v;
	}

	uint64_t modulesBytes() {
		std::ifstream f("/proc/modules");
		if (!f) return NA;
		uint64_t sum = 0;
		std::string line;
		while (std::getline(f,line)) {
			std::istringstream is(line);
			std::string name;
			uint64_t sz = 0;
			if (is >> name >> sz) sum += sz;
		}
		return sum;
	}

	uint64_t installedSysfs() {
		static uint64_t v = [] {
			std::string bs = slurp("/sys/devices/system/memory/block_size_bytes");
			if (bs.empty()) return NA;
			uint64_t block = strtoull(bs.c_str(),nullptr,16);
			DIR* d = opendir("/sys/devices/system/memory");
			if (!d || !block) {
				if (d) closedir(d);
				return NA;
			}
			uint64_t n = 0;
			while (dirent* e = readdir(d)) {
				if (strncmp(e->d_name,"memory",6)) continue;
				if (readNum(std::string("/sys/devices/system/memory/") + e->d_name + "/online") == 1) n++;
			}
			closedir(d);
			return n ? n * block : NA;
		}();
		return v;
	}

	// mm_stat : orig_data_size compr_data_size mem_used_total ...
	void readZram(SysMem& m) {
		uint64_t orig = 0;
		uint64_t compr = 0;
		uint64_t used = 0;
		bool any = false;
		for (auto& d : zramDevs()) {
			std::istringstream is(slurp(d + "/mm_stat"));
			uint64_t a = 0;
			uint64_t b = 0;
			uint64_t c = 0;
			if (!(is >> a >> b >> c)) continue;
			orig += a;
			compr += b;
			used += c;
			any = true;
		}
		if (any && orig) {
			m.compData = orig;
			m.compSize = compr;
			m.compWs = used;
			return;
		}
		// meminfo has zswap since 5.19 and needs no root, debugfs before that
		auto mi = readMeminfo();
		uint64_t zs = get(mi,"Zswap");
		uint64_t zd = get(mi,"Zswapped");
		if (zs == NA || zd == NA || !zd) {
			zs = readNum("/sys/kernel/debug/zswap/pool_total_size");
			uint64_t pages = readNum("/sys/kernel/debug/zswap/stored_pages");
			zd = pages == NA ? NA : pages * m.pageSize;
		}
		if (zs != NA && zd != NA && zd) {
			m.compData = zd;
			m.compSize = zs;
			m.compWs = zs;
			return;
		}
		// set up but nothing swapped yet : a known 0
		if (any) {
			m.compData = orig;
			m.compSize = compr;
			m.compWs = used;
			return;
		}
		m.compWhy = "No zram device or zswap pool in use";
	}

	sysmem::CmdRes dropCaches(const char* label) {
		sysmem::CmdRes r;
		std::string tmp;
		if (!sysmem::bounded([] { sync(); return std::string(); },60000,tmp)) {
			r.why = "sync did not finish in 60 s";
			r.msg = std::string("Unable to empty ") + label + ". " + r.why;
			return r;
		}
		std::string err;
		if (!writeSys("/proc/sys/vm/drop_caches","1\n",err)) {
			r.why = err;
			r.msg = std::string("Unable to empty ") + label + ". " + err;
			return r;
		}
		r.ok = true;
		r.msg = std::string(label) + " dropped.";
		return r;
	}

	sysmem::CmdRes ksm() {
		sysmem::CmdRes r;
		std::string err;
		if (!writeSys("/sys/kernel/mm/ksm/run","1\n",err)) {
			r.na = err == strerror(ENOENT);
			r.why = r.na ? "This kernel has no KSM" : err;
			r.msg = "Unable to combine memory pages. " + r.why;
			return r;
		}
		r.ok = true;
		uint64_t sh = readNum("/sys/kernel/mm/ksm/pages_sharing");
		uint64_t pg = (uint64_t)sysconf(_SC_PAGESIZE);
		r.msg = "KSM started, it stays on. Saved so far : " + util::size(sh == NA ? NA : sh * pg) + ". Only MADV_MERGEABLE memory merges, it takes a while";
		return r;
	}

	sysmem::CmdRes compactZram() {
		sysmem::CmdRes r;
		auto devs = zramDevs();
		if (devs.empty()) {
			r.na = true;
			r.why = "No zram device in use, zswap has no empty command";
			r.msg = "Unable to empty compression store. " + r.why;
			return r;
		}
		int bad = 0;
		for (auto& d : devs) {
			std::string err;
			std::string name = d.substr(d.rfind('/') + 1);
			if (writeSys((d + "/compact").c_str(),"1\n",err)) r.items.push_back({name,"Compacted"});
			else {
				r.items.push_back({name,err});
				if (!bad++) r.why = name + " : " + err;
			}
		}
		r.ok = !bad;
		r.msg = r.ok ? "zram compacted." : "Unable to empty compression store. " + r.why;
		return r;
	}

	// /proc/mounts writes space, tab, newline and backslash as \040 \011 \012 \134
	std::string unescMount(const std::string& s) {
		std::string r;
		for (size_t i = 0; i < s.size(); i++) {
			bool esc = s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '3' && s[i + 2] >= '0' && s[i + 2] <= '7' && s[i + 3] >= '0' && s[i + 3] <= '7';
			if (!esc) {
				r += s[i];
				continue;
			}
			r += (char)((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
			i += 3;
		}
		return r;
	}

	// one syncfs per real mount, so a dead one shows up by name. ZFS datasets have no /dev source.
	sysmem::CmdRes syncMounts() {
		sysmem::CmdRes r;
		std::ifstream f("/proc/mounts");
		std::string line;
		int okN = 0;
		int bad = 0;
		std::vector<std::string> seen;
		while (std::getline(f,line)) {
			std::istringstream is(line);
			std::string dev,dir,type;
			if (!(is >> dev >> dir >> type) || (dev.compare(0,5,"/dev/") && type != "zfs")) continue;
			dir = unescMount(dir);
			if (std::find(seen.begin(),seen.end(),dir) != seen.end()) continue;
			seen.push_back(dir);
			std::string txt;
			bool fin = sysmem::bounded([dir] {
				int fd = open(dir.c_str(),O_RDONLY | O_DIRECTORY);
				if (fd < 0) return std::string(strerror(errno));
				int rc = syncfs(fd);
				int e = errno;
				close(fd);
				return rc ? std::string(strerror(e)) : std::string();
			},20000,txt);
			if (!fin) txt = "Timed out after 20 s";
			r.items.push_back({dir,txt.empty() ? "Flushed" : txt});
			if (txt.empty()) okN++;
			else if (!bad++) r.why = dir + " : " + txt;
		}
		// overlay in a container : nothing to name, sync still covers it
		if (!okN && !bad) {
			std::string tmp;
			r.ok = sysmem::bounded([] { sync(); return std::string(); },60000,tmp);
			r.why = r.ok ? "" : "sync did not finish in 60 s";
			r.msg = r.ok ? "Volume file cache flushed to disk with sync, no block device mounts." : "Unable to flush volume file cache. " + r.why;
			return r;
		}
		r.ok = !bad;
		r.msg = r.ok ? "Volume file cache flushed to disk, " + std::to_string(okN) + " mounts" : "Unable to flush volume file cache. " + r.why;
		return r;
	}

	sysmem::CmdRes notHere(const char* what) {
		sysmem::CmdRes r;
		r.na = true;
		r.why = "not available on Linux";
		r.msg = std::string(what) + " is not available on Linux";
		return r;
	}

}

namespace sysmem {

	void osRead(SysMem& m) {
		m.pageSize = (uint64_t)sysconf(_SC_PAGESIZE);
		m.peakSinceStart = true;
		Map mi = readMeminfo();
		Map vm = readVmstat();
		// keys stay when a read fails or an older kernel lacks one, unknown goes out as null
		for (const char* k : {"SwapTotal","SwapFree","Shmem","Slab","SReclaimable","PageTables","Mapped","AnonPages","VmallocUsed","Active","Inactive","Unevictable","Mlocked","Hugetlb"}) m.extra.push_back({k,get(mi,k)});
		uint64_t st = readNum("/sys/kernel/mm/ksm/pages_sharing");
		m.extra.push_back({"KSM saved",st == NA ? NA : st * m.pageSize});
		for (const char* k : {"nr_dirty","nr_writeback","nr_page_table_pages","pgmajfault","pswpin","pgsteal_file","pgscan_file"}) m.counts.push_back({k,get(vm,k)});
		if (mi.empty()) {
			m.perfWhy = "Cannot read /proc/meminfo";
			m.listsWhy = m.perfWhy;
			return;
		}
		m.physTotal = get(mi,"MemTotal");
		m.physAvail = get(mi,"MemAvailable");
		m.commit = get(mi,"Committed_AS");
		m.commitLimit = get(mi,"CommitLimit");
		static std::atomic<uint64_t> peak{0};
		if (m.commit != NA) {
			uint64_t p = peak.load();
			while (m.commit > p && !peak.compare_exchange_weak(p,m.commit)) {}
			m.commitPeak = peak.load();
		}
		auto dmi = smbios::latest();
		m.physInstalled = dmi->installed != NA ? dmi->installed : installedSysfs();

		uint64_t cached = get(mi,"Cached");
		uint64_t buffers = get(mi,"Buffers");
		m.cacheWs = add(cached,buffers);
		m.fileCache = cached;
		m.kernelWs = get(mi,"KernelStack");
		m.driverWs = modulesBytes();
		m.nonPaged = get(mi,"SUnreclaim");

		m.free = get(mi,"MemFree");
		uint64_t dirty = get(mi,"Dirty");
		uint64_t wb = get(mi,"Writeback");
		m.modified = add(dirty,wb);
		uint64_t file = add(get(mi,"Active(file)"),get(mi,"Inactive(file)"));
		// dirty pages sit on the file lists too, without this the parts add up to more than RAM
		if (file != NA) m.standby = m.modified != NA && file > m.modified ? file - m.modified : m.modified != NA ? 0 : file;
		m.bad = get(mi,"HardwareCorrupted");
		uint64_t steal = get(vm,"pgsteal_file");
		if (steal != NA) m.repurposedTotal = steal * m.pageSize;

		m.cFaults = get(vm,"pgfault");
		m.cPageReads = get(vm,"pgmajfault");
		m.cPfWrites = get(vm,"pswpout");
		m.cMappedWrites = get(vm,"nr_written");
		uint64_t in = get(vm,"pgpgin");
		uint64_t out = get(vm,"pgpgout");
		if (in != NA) m.cMappedReadBytes = in * 1024;
		if (out != NA) m.cMappedWriteBytes = out * 1024;

		readZram(m);

		if (DIR* d = opendir("/proc")) {
			uint32_t n = 0;
			while (dirent* e = readdir(d)) if (e->d_name[0] >= '0' && e->d_name[0] <= '9') n++;
			closedir(d);
			m.processes = n;
		}
		std::istringstream la(slurp("/proc/loadavg"));
		std::string a,b,c,th;
		if (la >> a >> b >> c >> th) {
			size_t sl = th.find('/');
			if (sl != std::string::npos) m.threads = (uint32_t)strtoul(th.c_str() + sl + 1,nullptr,10);
		}
		uint64_t fh = readNum("/proc/sys/fs/file-nr");
		if (fh != NA) m.handles = (uint32_t)fh;
	}

	CmdRes osCmd(int idx) {
		CmdRes r;
		switch (idx) {
		case 0: return ksm();
		case 1: return compactZram();
		case 2: return dropCaches("system file cache");
		case 3: return notHere("Empty registry cache");
		case 4: {
			std::string s = emptyWorkSets();
			r.ok = s.find("failed") == std::string::npos;
			r.na = s.find("unsupported") != std::string::npos;
			r.msg = s;
			if (!r.ok) r.why = s;
			// without root process_madvise reaches FRS itself at most, that is no trim
			size_t p = s.find("trimmed ");
			long n = p == std::string::npos ? -1 : strtol(s.c_str() + p + 8,nullptr,10);
			if (r.ok && (n == 0 || (n == 1 && !isElevated()))) {
				r.ok = false;
				r.why = isElevated() ? "no process could be paged out" : "needs root, paged out " + std::to_string(n) + " processes";
				r.msg = "Unable to empty working sets, " + r.why;
			}
			return r;
		}
		case 5: {
			std::string tmp;
			r.ok = bounded([] { sync(); return std::string(); },60000,tmp);
			r.why = r.ok ? "" : "sync did not finish in 60 s";
			r.msg = r.ok ? "Dirty pages written out." : "Unable to write dirty pages. " + r.why;
			return r;
		}
		case 6: return syncMounts();
		case 7: return dropCaches("page cache");
		case 8: return notHere("Empty priority 0 standby list");
		}
		r.why = "unknown command";
		r.msg = r.why;
		return r;
	}

	bool osCompact(CmdRes& r) {
		std::string err;
		r.ok = writeSys("/proc/sys/vm/compact_memory","1\n",err);
		r.na = !r.ok && err == strerror(ENOENT);
		r.why = err;
		r.msg = r.ok ? "Free memory compacted." : "Unable to compact memory. " + err;
		return true;
	}

	bool osArgSysinfo() {
		std::string s = slurp("/proc/self/cmdline");
		std::vector<std::string> a;
		size_t pos = 0;
		while (pos < s.size()) {
			size_t e = s.find('\0',pos);
			if (e == std::string::npos) e = s.size();
			a.push_back(s.substr(pos,e - pos));
			pos = e + 1;
		}
		bool r = false;
		for (size_t i = 1; i < a.size(); i++) {
			if (strcasecmp(a[i].c_str(),"-sysinfo")) continue;
			r = i + 1 >= a.size() || !strcasecmp(a[i + 1].c_str(),"Memory");
		}
		return r;
	}

}

namespace smbios {

	bool osRead(std::vector<uint8_t>& out,int& major,int& minor,std::string& err) {
		errno = 0;
		std::ifstream f("/sys/firmware/dmi/tables/DMI",std::ios::binary);
		int e = errno;
		if (!f) {
			err = e == EACCES ? "/sys/firmware/dmi/tables/DMI needs root" : e == ENOENT ? "This kernel has no DMI table" : std::string("/sys/firmware/dmi/tables/DMI : ") + strerror(e);
			return false;
		}
		std::string s((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
		if (s.empty()) {
			err = "/sys/firmware/dmi/tables/DMI is empty";
			return false;
		}
		out.assign(s.begin(),s.end());
		std::string ep = slurp("/sys/firmware/dmi/tables/smbios_entry_point");
		if (ep.size() > 8 && !ep.compare(0,5,"_SM3_")) {
			major = (uint8_t)ep[7];
			minor = (uint8_t)ep[8];
		} else if (ep.size() > 7 && !ep.compare(0,4,"_SM_")) {
			major = (uint8_t)ep[6];
			minor = (uint8_t)ep[7];
		}
		return true;
	}

}
#endif // !_WIN32
