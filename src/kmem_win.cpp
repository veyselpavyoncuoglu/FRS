#ifdef _WIN32
#include "kmem.h"
#include "nt.h"
#include <cstddef>
#include <cstdio>

namespace {

	const ULONG CLS_POOLTAG = 22,CLS_HANDLES = 64,CLS_BIGPOOL = 66,CLS_PAGEFILE = 18,CLS_PAGEFILE_EX = 144,CLS_POOLTAG2 = 255;

	// SYSTEM_POOLTAG, phnt ntexapi.h
	struct PoolTag {
		ULONG Tag,PagedAllocs,PagedFrees;
		SIZE_T PagedUsed;
		ULONG NonPagedAllocs,NonPagedFrees;
		SIZE_T NonPagedUsed;
	};

	struct PoolInfo {
		ULONG Count;
		PoolTag TagInfo[1];
	};

	// SYSTEM_POOLTAG2, since 26H1
	struct PoolTag2 {
		ULONG Tag;
		SIZE_T PagedAllocs,PagedFrees,PagedUsed,NonPagedAllocs,NonPagedFrees,NonPagedUsed;
	};

	struct Pool2Info {
		ULONG Count;
		PoolTag2 TagInfo[1];
	};

	// SYSTEM_PAGEFILE_INFORMATION_EX. The legacy class has the same start and no MinimumSize, MaximumSize.
	struct PageFileEx {
		ULONG NextEntryOffset,TotalSize,TotalInUse,PeakUsage;
		nt::USTR PageFileName;
		ULONG MinimumSize,MaximumSize;
	};

	// SYSTEM_BIGPOOL_ENTRY. Bit 0 of the address is the NonPaged flag.
	struct BigEntry {
		ULONG_PTR Addr;
		SIZE_T Size;
		ULONG Tag;
	};

	struct BigInfo {
		ULONG Count;
		BigEntry Info[1];
	};

	// SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX
	struct HandleEntry {
		PVOID Object;
		HANDLE Pid,Value;
		ACCESS_MASK Access;
		USHORT Backtrace,TypeIndex;
		ULONG Attributes,Reserved;
	};

	struct HandleInfo {
		ULONG_PTR Count,Reserved;
		HandleEntry Handles[1];
	};

	// OBJECT_BASIC_INFORMATION
	struct ObjBasic {
		ULONG Attributes;
		ACCESS_MASK Access;
		ULONG HandleCount,PointerCount,PagedPoolCharge,NonPagedPoolCharge,Reserved[3],NameInfoSize,TypeInfoSize,SecurityDescriptorSize;
		LARGE_INTEGER CreationTime;
	};

#ifdef _WIN64
	static_assert(sizeof(PoolTag) == 40 && offsetof(PoolTag,PagedUsed) == 16 && offsetof(PoolTag,NonPagedAllocs) == 24 && offsetof(PoolTag,NonPagedUsed) == 32,"PoolTag");
	static_assert(offsetof(PoolInfo,TagInfo) == 8,"PoolInfo");
	static_assert(sizeof(PoolTag2) == 56 && offsetof(PoolTag2,PagedAllocs) == 8 && offsetof(PoolTag2,NonPagedUsed) == 48,"PoolTag2");
	static_assert(offsetof(Pool2Info,TagInfo) == 8,"Pool2Info");
	static_assert(sizeof(PageFileEx) == 40 && offsetof(PageFileEx,PageFileName) == 16 && offsetof(PageFileEx,MinimumSize) == 32,"PageFileEx");
	static_assert(sizeof(BigEntry) == 24 && offsetof(BigEntry,Tag) == 16,"BigEntry");
	static_assert(offsetof(BigInfo,Info) == 8,"BigInfo");
	static_assert(sizeof(HandleEntry) == 40 && offsetof(HandleEntry,TypeIndex) == 30,"HandleEntry");
	static_assert(offsetof(HandleInfo,Handles) == 16,"HandleInfo");
	static_assert(sizeof(ObjBasic) == 56 && offsetof(ObjBasic,PagedPoolCharge) == 16,"ObjBasic");
#endif

	// OBJECT_TYPE_INFORMATION, its name follows it
	struct ObjType {
		nt::USTR TypeName;
		ULONG Totals[12],InvalidAttributes;
		GENERIC_MAPPING GenericMapping;
		ULONG ValidAccessMask;
		BOOLEAN SecurityRequired,MaintainHandleCount;
		UCHAR TypeIndex;
		CHAR ReservedByte;
		ULONG PoolType,DefaultPagedPoolCharge,DefaultNonPagedPoolCharge;
	};

#ifdef _WIN64
	static_assert(sizeof(ObjType) == 0x68 && offsetof(ObjType,TypeIndex) == 0x5A,"ObjType");
#endif

	typedef nt::Status(NTAPI* QueryFn)(HANDLE,ULONG,PVOID,ULONG,PULONG);

	std::string failed(const char* what,nt::Status st) {
		char b[200];
		const char* next = st == nt::ACCESS_DENIED || st == nt::PRIVILEGE_NOT_HELD ? "Windows refused it, run FRS as administrator." : "Try again, if it keeps failing this Windows may not support it.";
		snprintf(b,sizeof(b),"%s : failed (NTSTATUS 0x%08lX). %s",what,(unsigned long)st,next);
		return b;
	}

	std::string utf8(const wchar_t* w,int n) {
		if (n <= 0) return "";
		int m = WideCharToMultiByte(CP_UTF8,0,w,n,nullptr,0,nullptr,nullptr);
		if (m <= 0) return "";
		std::string s((size_t)m,'\0');
		WideCharToMultiByte(CP_UTF8,0,w,n,&s[0],m,nullptr,nullptr);
		return s;
	}

	// handle table type index to name. Handles that can't be duplicated still get a name this way.
	std::unordered_map<USHORT,std::string> typeNames(QueryFn qry) {
		std::unordered_map<USHORT,std::string> out;
		std::vector<char> b(0x10000);
		ULONG need = 0;
		nt::Status st = qry(nullptr,3,b.data(),(ULONG)b.size(),&need);
		if (st == nt::INFO_LENGTH_MISMATCH && need > b.size()) {
			b.resize(need + 0x1000);
			st = qry(nullptr,3,b.data(),(ULONG)b.size(),&need);
		}
		if (!nt::ok(st)) return out;
		ULONG n = *(const ULONG*)b.data();
		const size_t al = sizeof(ULONG_PTR);
		size_t off = al;
		for (ULONG i = 0; i < n && off + sizeof(ObjType) <= b.size(); i++) {
			auto* t = (const ObjType*)(b.data() + off);
			const char* nm = (const char*)t->TypeName.Buffer;
			if (nm >= b.data() && nm + t->TypeName.Length <= b.data() + b.size()) out[t->TypeIndex] = utf8(t->TypeName.Buffer,t->TypeName.Length / sizeof(WCHAR));
			off += (sizeof(ObjType) + t->TypeName.MaximumLength + al - 1) & ~(al - 1);
		}
		return out;
	}

	// padding, space or zero, is not part of the tag. Other bytes that don't print show as a dot.
	std::string tagText(ULONG v) {
		v &= 0x7FFFFFFFUL;
		unsigned char b[4];
		for (int i = 0; i < 4; i++) b[i] = (unsigned char)(v >> (i * 8));
		int n = 4;
		while (n > 0 && (b[n - 1] == 0 || b[n - 1] == ' ')) n--;
		if (!n) return "(none)";
		std::string s;
		for (int i = 0; i < n; i++) s += b[i] >= 0x20 && b[i] <= 0x7E ? (char)b[i] : '.';
		return s;
	}

	template<class E> void addTag(std::vector<kmem::PoolRaw>& out,const E& e,int64_t pCur,int64_t nCur) {
		out.emplace_back();
		kmem::PoolRaw& r = out.back();
		r.id = e.Tag;
		r.tag = tagText(e.Tag);
		r.prot = (e.Tag & 0x80000000UL) != 0;
		r.v[kmem::VPagedAlloc] = (int64_t)e.PagedAllocs;
		r.v[kmem::VPagedFree] = (int64_t)e.PagedFrees;
		r.v[kmem::VPagedCur] = pCur;
		r.v[kmem::VPagedBytes] = (int64_t)e.PagedUsed;
		r.v[kmem::VNpAlloc] = (int64_t)e.NonPagedAllocs;
		r.v[kmem::VNpFree] = (int64_t)e.NonPagedFrees;
		r.v[kmem::VNpCur] = nCur;
		r.v[kmem::VNpBytes] = (int64_t)e.NonPagedUsed;
	}

}

namespace kmem {

	std::string osPool(std::vector<PoolRaw>& out,int& bits,std::string& src) {
		static std::atomic<bool> no2{false};
		std::vector<char> b;
		if (!no2) {
			nt::Status st = nt::querySys(CLS_POOLTAG2,b);
			if (nt::ok(st) && b.size() >= offsetof(Pool2Info,TagInfo)) {
				auto* p = (const Pool2Info*)b.data();
				if (p->Count && offsetof(Pool2Info,TagInfo) + p->Count * sizeof(PoolTag2) <= b.size()) {
					out.reserve(p->Count);
					for (ULONG i = 0; i < p->Count; i++) {
						const PoolTag2& e = p->TagInfo[i];
						addTag(out,e,(int64_t)(e.PagedAllocs - e.PagedFrees),(int64_t)(e.NonPagedAllocs - e.NonPagedFrees));
					}
					bits = 64;
					src = "SystemPoolTagInformation2";
					return "";
				}
			}
			no2 = true;
			b.clear();
		}
		nt::Status st = nt::querySys(CLS_POOLTAG,b);
		if (!nt::ok(st)) return failed("Pool tag query",st);
		if (b.size() < offsetof(PoolInfo,TagInfo)) return "Pool tag query : failed (short answer). Try again.";
		auto* p = (const PoolInfo*)b.data();
		if (offsetof(PoolInfo,TagInfo) + p->Count * sizeof(PoolTag) > b.size()) return "Pool tag query : failed (count past the buffer). Try again.";
		if (!p->Count) return "Pool tag query : Windows returned no tags. Try again.";
		out.reserve(p->Count);
		for (ULONG i = 0; i < p->Count; i++) {
			const PoolTag& e = p->TagInfo[i];
			// the counters are 32 bit and wrap, the difference is the signed number it really is
			addTag(out,e,(int64_t)(LONG)(e.PagedAllocs - e.PagedFrees),(int64_t)(LONG)(e.NonPagedAllocs - e.NonPagedFrees));
		}
		bits = 32;
		src = "SystemPoolTagInformation";
		return "";
	}

	std::string osBig(std::vector<BigRaw>& out,std::unordered_map<uint64_t,std::string>& names,std::string& src) {
		std::vector<char> b;
		nt::Status st = nt::querySys(CLS_BIGPOOL,b);
		if (!nt::ok(st)) return failed("Big pool query",st);
		if (b.size() < offsetof(BigInfo,Info)) return "Big pool query : failed (short answer). Try again.";
		auto* p = (const BigInfo*)b.data();
		if (offsetof(BigInfo,Info) + p->Count * sizeof(BigEntry) > b.size()) return "Big pool query : failed (count past the buffer). Try again.";
		out.reserve(p->Count);
		for (ULONG i = 0; i < p->Count; i++) {
			const BigEntry& e = p->Info[i];
			out.push_back({(uint64_t)(e.Addr & ~(ULONG_PTR)1),(uint64_t)e.Size,(uint64_t)e.Tag,(e.Addr & 1) != 0});
			auto it = names.find(e.Tag);
			if (it == names.end()) names.emplace(e.Tag,tagText(e.Tag));
		}
		src = "SystemBigPoolInformation";
		return "";
	}

	std::string osPages(PageSnap& s) {
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		uint64_t pg = si.dwPageSize ? si.dwPageSize : 4096;
		std::vector<char> b;
		nt::Status st = nt::querySys(CLS_PAGEFILE_EX,b);
		bool ex = nt::ok(st);
		if (!ex) {
			b.clear();
			st = nt::querySys(CLS_PAGEFILE,b);
		}
		if (!nt::ok(st)) return failed("Page file query",st);
		size_t esz = ex ? sizeof(PageFileEx) : offsetof(PageFileEx,MinimumSize);
		size_t off = 0;
		// the buffer starts zeroed and no page file writes nothing, so TotalSize 0 means none
		while (off + esz <= b.size()) {
			auto* e = (const PageFileEx*)(b.data() + off);
			if (!e->TotalSize) break;
			PageFile f;
			const char* nm = (const char*)e->PageFileName.Buffer;
			if (nm >= b.data() && e->PageFileName.Length && nm + e->PageFileName.Length <= b.data() + b.size())
				f.path = utf8(e->PageFileName.Buffer,e->PageFileName.Length / sizeof(WCHAR));
			if (f.path.compare(0,4,"\\??\\") == 0) f.path.erase(0,4);
			f.size = (uint64_t)e->TotalSize * pg;
			f.used = (uint64_t)e->TotalInUse * pg;
			f.peak = (uint64_t)e->PeakUsage * pg;
			if (ex) {
				f.min = (uint64_t)e->MinimumSize * pg;
				f.max = (uint64_t)e->MaximumSize * pg;
			}
			s.rows.push_back(std::move(f));
			if (!e->NextEntryOffset) break;
			off += e->NextEntryOffset;
		}
		return "";
	}

	bool osTagFile(std::string& txt,std::string& path,std::string& why) {
		std::vector<wchar_t> w(32768);
		DWORD n = GetModuleFileNameW(nullptr,w.data(),(DWORD)w.size());
		if (!n || n >= w.size()) return false;
		std::wstring p(w.data(),n);
		size_t cut = p.find_last_of(L"\\/");
		if (cut == std::wstring::npos) return false;
		p.resize(cut + 1);
		p += L"pooltag.txt";
		FILE* f = _wfopen(p.c_str(),L"rb");
		if (!f) return false;
		path = utf8(p.data(),(int)p.size());
		const size_t mx = 16 * 1024 * 1024;
		txt.resize(mx + 1);
		size_t got = fread(&txt[0],1,txt.size(),f);
		fclose(f);
		if (got > mx) {
			txt.clear();
			why = "Over 16 MB, this is no tag list";
			return false;
		}
		txt.resize(got);
		return true;
	}

	std::string osObjects(const ProcRef& r,ObjSnap& s,const std::atomic<bool>& cancel) {
		typedef nt::Status(NTAPI* DupFn)(HANDLE,HANDLE,HANDLE,PHANDLE,ACCESS_MASK,ULONG,ULONG);
		static auto dup = nt::fn<DupFn>("NtDuplicateObject");
		static auto qry = nt::fn<QueryFn>("NtQueryObject");
		if (!dup || !qry) return "This Windows lacks NtDuplicateObject or NtQueryObject";

		HANDLE p = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION,FALSE,r.pid);
		if (!p) {
			return GetLastError() == ERROR_ACCESS_DENIED ? "Windows refused handle access to this process. It is protected, or FRS is not elevated and it belongs to another user." : "Could not open this process, it may have exited.";
		}
		FILETIME ct,et,kt,ut;
		if (r.start && GetProcessTimes(p,&ct,&et,&kt,&ut) && (((uint64_t)ct.dwHighDateTime << 32) | ct.dwLowDateTime) != r.start) {
			CloseHandle(p);
			return "This process has exited, its pid is used by another one now.";
		}

		std::vector<char> b;
		nt::Status st = nt::querySys(CLS_HANDLES,b);
		if (!nt::ok(st)) {
			CloseHandle(p);
			return failed("Handle list query",st);
		}
		auto* hi = (const HandleInfo*)b.data();
		if (b.size() < offsetof(HandleInfo,Handles) || offsetof(HandleInfo,Handles) + hi->Count * sizeof(HandleEntry) > b.size()) {
			CloseHandle(p);
			return "Handle list query : failed (count past the buffer). Try again.";
		}

		auto types = typeNames(qry);
		for (ULONG_PTR i = 0; i < hi->Count && !cancel; i++) {
			const HandleEntry& e = hi->Handles[i];
			if ((uint32_t)(ULONG_PTR)e.Pid != r.pid) continue;
			ObjRow row;
			row.handle = (uint64_t)(ULONG_PTR)e.Value;
			HANDLE d = nullptr;
			if (nt::ok(dup(p,e.Value,GetCurrentProcess(),&d,0,0,0)) && d) {
				ObjBasic oi = {};
				if (nt::ok(qry(d,0,&oi,sizeof(oi),nullptr))) {
					row.paged = oi.PagedPoolCharge;
					row.nonPaged = oi.NonPagedPoolCharge;
				}
				CloseHandle(d);
			}
			if (row.paged == NA) s.failed++;
			auto t = types.find(e.TypeIndex);
			row.type = t != types.end() ? t->second : "Type " + std::to_string(e.TypeIndex);
			s.rows.push_back(std::move(row));
		}
		CloseHandle(p);
		return "";
	}

}
#endif // _WIN32
