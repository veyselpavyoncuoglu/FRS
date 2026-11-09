#ifndef _WIN32
#include "kmem.h"
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {

	uint64_t fnv(const char* s) {
		uint64_t h = 14695981039346656037ULL;
		for (; *s; s++) {
			h ^= (unsigned char)*s;
			h *= 1099511628211ULL;
		}
		return h;
	}

	std::string openWhy(const char* path,const char* what) {
		if (errno == EACCES || errno == EPERM) return std::string(what) + " needs root, " + path + " is only readable by root. Start FRS without sudo, it asks for root itself through pkexec.";
		if (errno == ENOENT) return std::string(path) + " does not exist on this kernel, so there is no " + what + " to show.";
		return std::string(what) + " : failed (" + path + " : " + strerror(errno) + ")";
	}

	// /proc/swaps writes a space in a path as \040
	std::string unescape(const char* s) {
		std::string o;
		for (; *s; s++) {
			if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
				o += (char)(((s[1] - '0') << 6) | ((s[2] - '0') << 3) | (s[3] - '0'));
				s += 3;
			} else o += *s;
		}
		return o;
	}

}

namespace kmem {

	std::string osPool(std::vector<PoolRaw>& out,int& bits,std::string& src) {
		FILE* f = fopen("/proc/slabinfo","r");
		if (!f) return openWhy("/proc/slabinfo","Slab cache list");
		char ln[512];
		errno = 0;
		while (fgets(ln,sizeof(ln),f)) {
			if (ln[0] == '#' || !strncmp(ln,"slabinfo",8)) continue;
			char name[128];
			unsigned long long act,num,sz,per,pgs;
			if (sscanf(ln,"%127s %llu %llu %llu %llu %llu",name,&act,&num,&sz,&per,&pgs) != 6) continue;
			out.emplace_back();
			PoolRaw& r = out.back();
			r.id = fnv(name);
			r.tag = util::safe(name);
			r.desc = util::size(sz) + " each, " + std::to_string(per) + " per slab";
			r.v[VPagedAlloc] = r.v[VPagedFree] = r.v[VPagedCur] = r.v[VPagedBytes] = UNK;
			r.v[VNpAlloc] = (int64_t)num;
			r.v[VNpFree] = UNK;
			r.v[VNpCur] = (int64_t)act;
			r.v[VNpBytes] = (int64_t)(act * sz);
		}
		bool bad = ferror(f) != 0;
		int e = errno;
		fclose(f);
		if (bad) {
			errno = e;
			return openWhy("/proc/slabinfo","Slab cache list");
		}
		if (out.empty()) return "/proc/slabinfo lists no caches on this kernel.";
		bits = 64;
		src = "/proc/slabinfo";
		return "";
	}

	std::string osBig(std::vector<BigRaw>& out,std::unordered_map<uint64_t,std::string>& names,std::string& src) {
		FILE* f = fopen("/proc/vmallocinfo","r");
		if (!f) return openWhy("/proc/vmallocinfo","The vmalloc list");
		char ln[1024];
		errno = 0;
		while (fgets(ln,sizeof(ln),f)) {
			unsigned long long a,z,sz;
			char who[256];
			int got = sscanf(ln,"%llx-%llx %llu %255s",&a,&z,&sz,who);
			if (got < 3) continue;
			if (got < 4 || !strncmp(who,"pages=",6) || !strcmp(who,"vmalloc") || !strcmp(who,"ioremap") || !strcmp(who,"vmap")) strcpy(who,"(no caller)");
			if (char* plus = strchr(who,'+')) *plus = 0;
			uint64_t id = fnv(who);
			out.push_back({(uint64_t)a,(uint64_t)sz,id,true});
			if (!names.count(id)) names.emplace(id,util::safe(who));
		}
		bool bad = ferror(f) != 0;
		int e = errno;
		fclose(f);
		if (bad) {
			errno = e;
			return openWhy("/proc/vmallocinfo","The vmalloc list");
		}
		src = "/proc/vmallocinfo";
		return "";
	}

	std::string osPages(PageSnap& s) {
		s.ownPeak = true;
		FILE* f = fopen("/proc/swaps","r");
		if (!f) return openWhy("/proc/swaps","The swap list");
		char ln[1024];
		bool head = true;
		while (fgets(ln,sizeof(ln),f)) {
			if (head) {
				head = false;
				continue;
			}
			char name[512],type[32];
			unsigned long long sz,used;
			if (sscanf(ln,"%511s %31s %llu %llu",name,type,&sz,&used) != 4) continue;
			PageFile r;
			r.path = unescape(name);
			r.size = (uint64_t)sz * 1024;
			r.used = (uint64_t)used * 1024;
			s.rows.push_back(std::move(r));
		}
		fclose(f);
		return "";
	}

	bool osTagFile(std::string& txt,std::string& path,std::string& why) {
		return false;
	}

	std::string osObjects(const ProcRef& r,ObjSnap& s,const std::atomic<bool>& cancel) {
		return "Not available on Linux, the kernel keeps no pool charge per object.";
	}

}
#endif // _WIN32
