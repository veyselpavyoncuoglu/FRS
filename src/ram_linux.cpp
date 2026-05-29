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

// pidfd_open / process_madvise wrappers via syscall(), so we don't depend on a
// recent glibc exposing them. They return -1/ENOSYS on kernels that lack them,
// which we treat as "feature unavailable" and report instead of crashing.
static int sys_pidfd_open(pid_t pid) {
#ifdef SYS_pidfd_open
	return (int)syscall(SYS_pidfd_open,pid,0u);
#else
	(void)pid; errno = ENOSYS; return -1;
#endif
}

static long sys_process_madvise(int pidfd,const struct iovec* iov,size_t n,int advice) {
#ifdef SYS_process_madvise
	return syscall(SYS_process_madvise,pidfd,iov,(unsigned long)n,(unsigned)advice,0u);
#else
	(void)pidfd;(void)iov;(void)n;(void)advice; errno = ENOSYS; return -1;
#endif
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

std::string flushStandby(int sz) {
	unsigned int nThreads = std::thread::hardware_concurrency();
	if (nThreads < 1) nThreads = 1;
	size_t total = (size_t)sz * 1024 * 1024;
	size_t chunk = total / nThreads;
	if (chunk == 0) chunk = total;
	std::vector<std::thread> threads;
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
	for (unsigned int i = 0; i < nThreads; i++) {
		threads.emplace_back([&pids,&trimmed,&unsupported,i,nThreads]() {
			for (size_t j = i; j < pids.size(); j += nThreads) {
				pid_t pid = pids[j];

				// Parse the target's address space into a list of regions to page out.
				char mapsPath[64];
				snprintf(mapsPath,sizeof(mapsPath),"/proc/%d/maps",(int)pid);
				FILE* mf = fopen(mapsPath,"r");
				if (!mf) continue; // process gone or not permitted
				std::vector<struct iovec> regions;
				char ln[512];
				while (fgets(ln,sizeof(ln),mf)) {
					unsigned long start,end;
					if (sscanf(ln,"%lx-%lx",&start,&end) == 2 && end > start) {
						struct iovec v;
						v.iov_base = (void*)start;
						v.iov_len = (size_t)(end - start);
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
				// process_madvise caps each call at IOV_MAX iovecs; submit in batches.
				bool any = false;
				for (size_t k = 0; k < regions.size(); k += 1024) {
					size_t n = regions.size() - k;
					if (n > 1024) n = 1024;
					long r = sys_process_madvise(fd,&regions[k],n,MADV_PAGEOUT);
					if (r < 0 && errno == ENOSYS) { unsupported.store(true); break; }
					if (r >= 0) any = true;
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
