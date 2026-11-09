#ifndef _WIN32
#include "ram.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/uio.h>

// MADV_PAGEOUT may be missing from older <sys/mman.h>; the kernel has had it
// since 5.4. Provide a fallback so we still compile.
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
// glibc before 2.33 has no SYS_ name for these, the kernel decides through ENOSYS
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_process_madvise
#define SYS_process_madvise 440
#endif

// pidfd_open / process_madvise wrappers via syscall(), so we don't depend on a
// recent glibc exposing them. They return -1/ENOSYS on kernels that lack them,
// which we treat as "feature unavailable" and report instead of crashing.
static int sys_pidfd_open(pid_t pid) {
	return (int)syscall(SYS_pidfd_open,pid,0u);
}

static long sys_process_madvise(int pidfd,const struct iovec* iov,size_t n,int advice) {
	return syscall(SYS_process_madvise,pidfd,iov,(unsigned long)n,(unsigned)advice,0u);
}

RamStats getRamStats() {
	RamStats r{0,0,0};
	FILE* f = fopen("/proc/meminfo","r");
	if (!f) return r;
	unsigned long long total = 0,avail = 0,memFree = 0,buffers = 0,cached = 0;
	bool haveAvail = false;
	char key[64]; unsigned long long val; char unit[16];
	char line[256];
	while (fgets(line,sizeof(line),f)) {
		if (sscanf(line,"%63[^:]: %llu %15s",key,&val,unit) >= 2) {
			if (strcmp(key,"MemTotal") == 0) total = val;
			else if (strcmp(key,"MemAvailable") == 0) { avail = val; haveAvail = true; }
			else if (strcmp(key,"MemFree") == 0) memFree = val;
			else if (strcmp(key,"Buffers") == 0) buffers = val;
			else if (strcmp(key,"Cached") == 0) cached = val;
		}
	}
	fclose(f);
	// MemAvailable (kernel >= 3.14) is the accurate figure; fall back to the
	// classic free+buffers+cached estimate on ancient kernels.
	if (!haveAvail) avail = memFree + buffers + cached;
	// /proc/meminfo reports kB; RamStats is in MB.
	r.total = total / 1024;
	r.free = avail / 1024;
	r.used = (total > avail ? total - avail : 0) / 1024;
	return r;
}

std::vector<std::pair<std::string,uint64_t>> getMemInfo() {
	static const char* keys[][2] = {
		{"Cached","cached_mb"},{"Buffers","buffers_mb"},{"Dirty","dirty_mb"},{"Writeback","writeback_mb"},
		{"MemFree","free_list_mb"},{"AnonPages","anon_mb"},{"Shmem","shmem_mb"},{"Slab","slab_mb"},
		{"SReclaimable","slab_reclaimable_mb"},{"Committed_AS","commit_mb"},{"CommitLimit","commit_limit_mb"},
		{"SwapTotal","swap_total_mb"},{"SwapFree","swap_free_mb"},
	};
	std::vector<std::pair<std::string,uint64_t>> v;
	FILE* f = fopen("/proc/meminfo","r");
	if (!f) return v;
	char line[256],key[64];
	unsigned long long kb;
	while (fgets(line,sizeof(line),f)) {
		if (sscanf(line,"%63[^:]: %llu",key,&kb) != 2) continue;
		for (auto& k : keys) if (strcmp(key,k[0]) == 0) v.push_back({k[1],kb / 1024});
	}
	fclose(f);
	return v;
}

std::vector<ProcMem> getProcs() {
	std::vector<ProcMem> v;
	DIR* d = opendir("/proc");
	if (!d) return v;
	while (dirent* e = readdir(d)) {
		char* end;
		long pid = strtol(e->d_name,&end,10);
		if (*end != '\0' || pid <= 0) continue;
		char path[64];
		snprintf(path,sizeof(path),"/proc/%ld/status",pid);
		FILE* f = fopen(path,"r");
		if (!f) continue;
		ProcMem p{(uint32_t)pid,"",0,0};
		char ln[256],nm[128];
		unsigned long long kb;
		while (fgets(ln,sizeof(ln),f)) {
			if (sscanf(ln,"Name: %127[^\n]",nm) == 1) p.name = nm;
			else if (sscanf(ln,"VmRSS: %llu",&kb) == 1) p.ws = kb * 1024;
			else if (sscanf(ln,"RssAnon: %llu",&kb) == 1) p.priv = kb * 1024;
		}
		fclose(f);
		if (p.ws) v.push_back(p);
	}
	closedir(d);
	return v;
}

std::string flushStandby(int sz) {
	// overcommit lets the mmap succeed past real RAM and then the OOM killer picks a victim,
	// so stay under MemAvailable with some headroom
	RamStats rs = getRamStats();
	uint64_t keep = rs.total / 20 + 256;
	int room = rs.free > keep ? (int)(rs.free - keep) : 0;
	bool capped = sz > room;
	if (capped) sz = room;
	if (sz <= 0) return "FlushStandby : skipped (not enough available memory)";
	unsigned int nThreads = std::thread::hardware_concurrency();
	if (nThreads < 1) nThreads = 1;
	size_t total = (size_t)sz * 1024 * 1024;
	size_t chunk = total / nThreads;
	if (chunk == 0) chunk = total;
	std::vector<std::thread> threads;
	struct Joiner { std::vector<std::thread>& v; ~Joiner() { for (auto& t : v) if (t.joinable()) t.join(); } } joiner{threads};
	for (unsigned int i = 0; i < nThreads; i++) {
		size_t bytes = (i == nThreads - 1) ? (total - chunk * i) : chunk;
		threads.emplace_back([bytes]() {
			if (bytes == 0) return;
			void* buf = mmap(nullptr,bytes,PROT_READ | PROT_WRITE,MAP_PRIVATE | MAP_ANONYMOUS,-1,0);
			if (buf == MAP_FAILED) return;
			memset(buf,0,bytes);
			munmap(buf,bytes);
		});
	}
	for (auto& t : threads) t.join();
	std::ostringstream ss;
	ss << "FlushStandby : flushed " << sz << " MB across " << nThreads << " threads";
	if (capped) ss << " (capped to available RAM)";
	return ss.str();
}

std::string emptyWorkSets() {
	// Collect all process IDs from /proc.
	std::vector<pid_t> pids;
	if (DIR* d = opendir("/proc")) {
		struct dirent* e;
		while ((e = readdir(d)) != nullptr) {
			char* end;
			long pid = strtol(e->d_name,&end,10);
			if (*end == '\0' && pid > 0) pids.push_back((pid_t)pid);
		}
		closedir(d);
	}
	if (pids.empty()) return "EmptyWorkSets : failed (cannot read /proc)";

	unsigned int nThreads = std::thread::hardware_concurrency();
	if (nThreads < 1) nThreads = 1;
	if (nThreads > (unsigned int)pids.size()) nThreads = (unsigned int)pids.size();

	std::atomic<int> trimmed{0};
	std::atomic<bool> unsupported{false};
	std::vector<std::thread> threads;
	struct Joiner { std::vector<std::thread>& v; ~Joiner() { for (auto& t : v) if (t.joinable()) t.join(); } } joiner{threads};
	for (unsigned int i = 0; i < nThreads; i++) {
		threads.emplace_back([&pids,&trimmed,&unsupported,i,nThreads]() {
			for (size_t j = i; j < pids.size(); j += nThreads) {
				pid_t pid = pids[j];

				// Parse the target's address space into a list of regions to page out.
				// The kernel silently truncates a call to ~2GB total, so big regions are cut
				// into 1GB pieces and batches stay under 1GB; then a short count can only
				// mean the region it stopped on failed.
				const unsigned long CAP = 1UL << 30;
				char mapsPath[64];
				snprintf(mapsPath,sizeof(mapsPath),"/proc/%d/maps",(int)pid);
				FILE* mf = fopen(mapsPath,"r");
				if (!mf) continue; // process gone or not permitted
				std::vector<struct iovec> regions;
				char ln[512];
				while (fgets(ln,sizeof(ln),mf)) {
					unsigned long start,end;
					char perms[8] = "",path[256] = "";
					if (sscanf(ln,"%lx-%lx %7s %*s %*s %*s %255[^\n]",&start,&end,perms,path) < 3 || end <= start) continue;
					// unreadable reservations hold nothing, and device/vdso maps (GPU buffers etc)
					// make process_madvise bail with EINVAL
					if (perms[0] != 'r' || strncmp(path,"/dev/",5) == 0 || strncmp(path,"[v",2) == 0) continue;
					for (unsigned long a = start; a < end; a += CAP) {
						struct iovec v;
						v.iov_base = (void*)a;
						v.iov_len = (size_t)(end - a < CAP ? end - a : CAP);
						regions.push_back(v);
					}
				}
				fclose(mf);
				if (regions.empty()) continue;

				int fd = sys_pidfd_open(pid);
				if (fd < 0) {
					if (errno == ENOSYS) unsupported.store(true);
					continue;
				}
				bool any = false;
				size_t k = 0;
				while (k < regions.size()) {
					size_t n = 0,tot = 0;
					while (k + n < regions.size() && n < 1024 && tot + regions[k + n].iov_len <= CAP) tot += regions[k + n++].iov_len;
					long r = sys_process_madvise(fd,&regions[k],n,MADV_PAGEOUT);
					if (r < 0) {
						if (errno == ENOSYS) unsupported.store(true);
						if (errno == ENOSYS || errno == EPERM || errno == ESRCH) break;
						k++; // first region of the batch can't be paged out, skip it
						continue;
					}
					if (r > 0) any = true;
					size_t i = 0,left = (size_t)r;
					while (i < n && left >= regions[k + i].iov_len) left -= regions[k + i++].iov_len;
					k += i < n ? i + 1 : n;
				}
				if (any) trimmed.fetch_add(1);
				close(fd);
			}
		});
	}
	for (auto& t : threads) t.join();

	std::ostringstream ss;
	if (trimmed.load() == 0 && unsupported.load()) {
		ss << "EmptyWorkSets : failed (process_madvise unsupported on this kernel)";
	} else {
		ss << "EmptyWorkSets : trimmed " << trimmed.load() << " processes across " << nThreads << " threads";
	}
	return ss.str();
}

// Shared helper for the two /proc/sys/vm knobs that need root.
static std::string writeProc(const char* path,const char* value,const char* label) {
	int fd = open(path,O_WRONLY);
	if (fd < 0) {
		std::ostringstream ss;
		ss << label << " : failed (" << strerror(errno) << " - need root?)";
		return ss.str();
	}
	ssize_t w = write(fd,value,strlen(value));
	close(fd);
	if (w < 0) {
		std::ostringstream ss;
		ss << label << " : failed (" << strerror(errno) << ")";
		return ss.str();
	}
	std::ostringstream ss;
	ss << label << " : OK";
	return ss.str();
}

std::string purgeStandby() {
	// Drop the clean page cache (the closest analog to the Windows standby list).
	sync(); // flush dirty pages first so more of the cache is actually droppable
	return writeProc("/proc/sys/vm/drop_caches","1\n","PurgeStandby");
}

std::string flushModified() {
	// Flush all modified (dirty) pages to backing storage.
	sync();
	return "FlushModified : OK";
}

bool isElevated() {
	return geteuid() == 0;
}
#endif // !_WIN32
