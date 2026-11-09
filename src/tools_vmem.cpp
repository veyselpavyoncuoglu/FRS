#include "vmem.h"
#include "mcp.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {

	using namespace vmem;
	using mcp::textArg;
	using mcp::flagArg;

	const double MAXIO = 65536;

	uint64_t addrArg(const Json& a) {
		const Json* v = a.get("address");
		if (!v || v->t != Json::Str) throw std::runtime_error("address is required, a hex string like 0x7ff6a0000000. list_regions gives them, numbers lose digits");
		uint64_t x = 0;
		if (!util::parseHex(v->s,x)) throw std::runtime_error("address must be hex, like 0x7ff6a0000000");
		return x;
	}

	// false when the key is absent
	bool wholeArg(const Json& a,const char* k,double& out) {
		const Json* v = a.get(k);
		if (!v || v->t == Json::Null) return false;
		if (v->t != Json::Num || v->n < 0 || v->n != std::floor(v->n) || v->n > 9007199254740992.0) throw std::runtime_error(std::string(k) + " must be a whole number, not negative");
		out = v->n;
		return true;
	}

	Json textOrNull(const std::string& s) {
		return s.empty() ? Json() : Json(s);
	}

	Json intOrNull(int v) {
		return v < 0 ? Json() : Json(v);
	}

	Json regionJson(const Region& x) {
		Json j = Json::obj();
		j.set("base_address",util::hex(x.base));
		j.set("allocation_base",x.allocBase ? Json(util::hex(x.allocBase)) : Json());
		j.set("size_bytes",x.size);
		j.set("state",x.state == StCommit ? "commit" : x.state == StReserve ? "reserve" : "free");
		j.set("type",x.type == TyPrivate ? Json("private") : x.type == TyMapped ? Json("mapped") : x.type == TyImage ? Json("image") : Json());
		j.set("protection",textOrNull(x.prot));
		j.set("allocation_protection",textOrNull(x.allocProt));
		j.set("use",textOrNull(x.use));
		const char* k = kindToken(x.kind);
		j.set("region_type",k ? Json(k) : Json());
		j.set("mapped_file",textOrNull(x.file));
		j.set("thread_id",x.tid ? Json(x.tid) : Json());
		j.set("heap_id",intOrNull(x.heapId));
		j.set("heap_class",textOrNull(x.heapClass));
		j.set("committed_bytes",x.committed);
		j.set("private_bytes",mcp::bytes(x.priv));
		j.set("ws_total_bytes",mcp::bytes(x.ws));
		j.set("ws_private_bytes",mcp::bytes(x.wsPriv));
		j.set("ws_shareable_bytes",mcp::bytes(x.wsShareable));
		j.set("ws_shared_bytes",mcp::bytes(x.wsShared));
		j.set("ws_locked_bytes",mcp::bytes(x.wsLocked));
		j.set("original_pages",x.pagesOrig == NA ? Json() : Json(x.pagesOrig));
		j.set("modified_pages",x.pagesMod == NA ? Json() : Json(x.pagesMod));
		const char* pn = prioName(x.prio);
		j.set("page_priority",pn ? Json(pn) : Json());
		const char* sn = signingName(x.signing);
		j.set("signing_level",sn ? Json(sn) : Json());
		j.set("region_flags",textOrNull(x.flags));
		return j;
	}

	Json briefJson(const Region& x) {
		Json j = Json::obj();
		j.set("base_address",util::hex(x.base));
		j.set("size_bytes",x.size);
		j.set("state",x.state == StCommit ? "commit" : x.state == StReserve ? "reserve" : "free");
		j.set("protection",textOrNull(x.prot));
		return j;
	}

	Json summary(const RegionList& l) {
		uint64_t cm = 0,pv = 0,ws = 0,wp = 0,xpv = 0;
		int nc = 0,nr = 0,nf = 0,ni = 0,np = 0,nm = 0,nx = 0;
		bool pvKnown = true;
		for (auto& x : l.rows) {
			cm += x.committed;
			if (x.priv == NA) pvKnown = false;
			else pv += x.priv;
			if (x.ws != NA) ws += x.ws;
			if (x.wsPriv != NA) wp += x.wsPriv;
			if (x.state == StCommit) nc++;
			else if (x.state == StReserve) nr++;
			else nf++;
			if (x.type == TyImage) ni++;
			else if (x.type == TyPrivate) np++;
			else if (x.type == TyMapped) nm++;
			if (x.state == StCommit && x.type == TyPrivate && x.ex) {
				nx++;
				xpv += x.size;
			}
		}
		Json j = Json::obj();
		j.set("committed_bytes",cm);
		j.set("private_bytes",pvKnown ? Json(pv) : Json());
		j.set("working_set_bytes",l.wsOk ? Json(ws) : Json());
		j.set("private_working_set_bytes",l.wsOk ? Json(wp) : Json());
		j.set("committed_regions",nc).set("reserved_regions",nr).set("free_regions",nf);
		j.set("image_regions",ni).set("mapped_regions",nm).set("private_regions",np);
		j.set("executable_private_regions",nx).set("executable_private_bytes",xpv);
		return j;
	}

	bool lessBy(const Region& a,const Region& b,const std::string& key,bool desc) {
		uint64_t x = a.base,y = b.base;
		if (key == "size_bytes") { x = a.size; y = b.size; }
		else if (key == "ws_total_bytes") { x = a.ws; y = b.ws; }
		else if (key == "ws_private_bytes") { x = a.wsPriv; y = b.wsPriv; }
		else if (key == "committed_bytes") { x = a.committed; y = b.committed; }
		else if (key == "private_bytes") { x = a.priv; y = b.priv; }
		// unknown goes last both ways
		if (x == NA || y == NA) {
			if (x != y) return y == NA;
			return a.base < b.base;
		}
		if (x != y) return desc ? x > y : x < y;
		return a.base < b.base;
	}

	Json listRegions(const Json& a) {
		ProcRef r = procs::target(a,false);
		bool incFree = flagArg(a,"include_free",false);
		bool incRes = flagArg(a,"include_reserved",true);
		bool allocOnly = flagArg(a,"allocations_only",false);
		bool sumOnly = flagArg(a,"summary_only",false);
		std::string type = textArg(a,"type","");
		if (!type.empty() && type != "private" && type != "mapped" && type != "image") throw std::runtime_error("type must be private, mapped or image");
		std::string needle = textArg(a,"filter","");
		std::string key = textArg(a,"sort_by","base_address");
		static const char* keys[] = {"base_address","size_bytes","ws_total_bytes","ws_private_bytes","committed_bytes","private_bytes"};
		if (std::find_if(std::begin(keys),std::end(keys),[&](const char* k) { return key == k; }) == std::end(keys)) throw std::runtime_error("sort_by must be one of : base_address, size_bytes, ws_total_bytes, ws_private_bytes, committed_bytes, private_bytes");
		bool desc = flagArg(a,"descending",key != "base_address");
		for (const char* k : {"limit","offset"}) {
			double d;
			wholeArg(a,k,d);
		}

		auto l = queryNow(r,1500);
		if (!l->err.empty()) throw std::runtime_error("regions of pid " + std::to_string(r.pid) + " : " + l->err);

		std::vector<const Region*> sel;
		for (auto& x : l->rows) {
			if (x.state == StFree && !incFree) continue;
			if (x.state == StReserve && !incRes) continue;
			if (allocOnly && x.base != x.allocBase) continue;
			if (!type.empty() && type != (x.type == TyPrivate ? "private" : x.type == TyMapped ? "mapped" : x.type == TyImage ? "image" : "")) continue;
			if (!needle.empty()) {
				uint64_t v = 0;
				bool hit = util::parseHex(needle,v) && v >= x.base && v - x.base < x.size;
				const char* k = kindToken(x.kind);
				if (!hit && !util::has(x.use,needle) && !(k && util::has(k,needle)) && !util::has(util::hex(x.base),needle)) continue;
			}
			sel.push_back(&x);
		}
		std::sort(sel.begin(),sel.end(),[&](const Region* p,const Region* q) { return lessBy(*p,*q,key,desc); });

		Json j = Json::obj();
		if (sumOnly) {
			j.set("count",0).set("total_count",sel.size()).set("offset",0).set("limit",0).set("truncated",false).set("regions",Json());
		} else {
			// only the page gets real rows, a browser has tens of thousands of regions
			double dl = a.num("limit",200),dof = a.num("offset",0);
			size_t lim = dl < 1 ? 1 : dl > 2000 ? 2000 : (size_t)dl;
			size_t off = dof <= 0 ? 0 : dof >= (double)sel.size() ? sel.size() : (size_t)dof;
			std::vector<Json> rows(sel.size());
			for (size_t i = off; i < sel.size() && i < off + lim; i++) rows[i] = regionJson(*sel[i]);
			j = mcp::paged(rows,a,200,2000,"regions");
		}
		j.set("process",procs::refJson(r));
		j.set("summary",summary(*l));
		j.set("protected",textOrNull(l->prot));
		j.set("note",textOrNull(l->note));
		j.set("classified",l->useOk);
		j.set("query_ms",l->ms);
		j.set("age_ms",util::nowMs() - l->t);
		return j;
	}

	std::string lbl(const char* what,const ProcRef& r,uint64_t addr) {
		return std::string("[mcp] ") + what + " (pid " + std::to_string(r.pid) + ", " + util::hex(addr) + ")";
	}

	void refuse(const ProcRef& r) {
		if (r.pid == procs::be::selfPid()) throw std::runtime_error("refused, that is FRS itself");
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		if (row && row->sys) throw std::runtime_error("pid " + std::to_string(r.pid) + " is a system process, FRS does not read or change its memory");
		std::string c = be::critical(r);
		if (!c.empty()) throw std::runtime_error("pid " + std::to_string(r.pid) + " : " + c);
	}

	std::string toHex(const uint8_t* d,size_t n) {
		static const char* h = "0123456789abcdef";
		std::string s(n * 2,'0');
		for (size_t i = 0; i < n; i++) {
			s[i * 2] = h[d[i] >> 4];
			s[i * 2 + 1] = h[d[i] & 15];
		}
		return s;
	}

	Json readMemory(const Json& a) {
		ProcRef r = procs::target(a,true);
		refuse(r);
		uint64_t addr = addrArg(a);
		double sz = 0;
		if (!wholeArg(a,"size",sz) || sz < 1) throw std::runtime_error("size is required, 1 to 65536 bytes");
		if (sz > MAXIO) throw std::runtime_error("size above 65536 is refused, read in steps");
		if (addr + (uint64_t)sz < addr) throw std::runtime_error("address + size runs past the top of the address space");
		size_t n = (size_t)sz;
		std::vector<uint8_t> buf(n);
		uint64_t p0 = addr & ~(uint64_t)0xFFF;
		std::vector<uint8_t> map((size_t)((addr + n + 0xFFF - p0) >> 12) + 1);
		std::string err;
		size_t got = be::read(r,addr,n,buf.data(),map.data(),err);
		if (!err.empty()) {
			appLog(lbl("Read memory",r,addr) + " : failed (" + util::safe(err) + ")");
			throw std::runtime_error(err);
		}
		if (!got) {
			appLog(lbl("Read memory",r,addr) + " : failed (nothing readable)");
			throw std::runtime_error("nothing is readable at " + util::hex(addr) + ", it is free, reserved, a guard page or no access. list_regions shows what is there");
		}
		std::string hex = toHex(buf.data(),n);
		size_t bad = 0;
		for (size_t i = 0; i < n; i++) if (!map[(size_t)((addr + i - p0) >> 12)]) {
			hex[i * 2] = hex[i * 2 + 1] = '?';
			bad++;
		}
		appLog(lbl("Read memory",r,addr) + " : OK");
		Json j = Json::obj();
		j.set("process",procs::refJson(r));
		j.set("address",util::hex(addr)).set("size",n).set("read_bytes",n - bad).set("unreadable_bytes",bad).set("complete",bad == 0);
		j.set("hex",hex);
		return j;
	}

	Json writeMemory(const Json& a) {
		ProcRef r = procs::target(a,true);
		refuse(r);
		uint64_t addr = addrArg(a);
		const Json* v = a.get("hex");
		if (!v || v->t != Json::Str) throw std::runtime_error("hex is required, the bytes as a string like 90 90 or 9090");
		std::vector<uint8_t> data;
		int hi = -1;
		for (char c : v->s) {
			if (c == ' ') continue;
			int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			if (d < 0) throw std::runtime_error("hex holds something that is not a hex digit");
			if (hi < 0) hi = d;
			else {
				data.push_back((uint8_t)(hi * 16 + d));
				hi = -1;
			}
		}
		if (hi >= 0) throw std::runtime_error("hex has an odd number of digits");
		if (data.empty()) throw std::runtime_error("hex is empty");
		if ((double)data.size() > MAXIO) throw std::runtime_error("more than 65536 bytes is refused, write in steps");
		if (addr + data.size() < addr) throw std::runtime_error("address + size runs past the top of the address space");
		size_t n = data.size();
		std::vector<uint8_t> old(n);
		uint64_t p0 = addr & ~(uint64_t)0xFFF;
		std::vector<uint8_t> map((size_t)((addr + n + 0xFFF - p0) >> 12) + 1);
		std::string err;
		size_t got = be::read(r,addr,n,old.data(),map.data(),err);
		if (err.empty() && got != n) err = "not all of that range is readable, nothing was written. list_regions shows what is there";
		if (err.empty()) for (size_t p = 0; p < (size_t)((addr + n + 0xFFF - p0) >> 12); p++) if (map[p] != 2) err = "a page in that range is not writable, nothing was written. protect_memory changes that";
		if (!err.empty()) {
			appLog(lbl("Write memory",r,addr) + " : failed (" + util::safe(err) + ")");
			throw std::runtime_error(err);
		}
		size_t wrote = be::write(r,addr,data.data(),n,err);
		Json same;
		if (wrote) {
			std::vector<uint8_t> back(wrote);
			std::string e2;
			if (be::read(r,addr,wrote,back.data(),nullptr,e2) == wrote) same = std::equal(back.begin(),back.end(),data.begin());
		}
		appLog(lbl("Write memory",r,addr) + (wrote == n ? " : OK" : " : failed (" + util::safe(err) + ")"));
		if (!wrote) throw std::runtime_error(err);
		changed(r);
		Json j = Json::obj();
		j.set("process",procs::refJson(r));
		j.set("address",util::hex(addr)).set("size",n).set("written_bytes",wrote).set("complete",wrote == n);
		j.set("old_hex",toHex(old.data(),n)).set("verified",same);
		j.set("error",textOrNull(wrote == n ? std::string() : err));
		return j;
	}

	Json protectMemory(const Json& a) {
		ProcRef r = procs::target(a,true);
		refuse(r);
		uint64_t addr = addrArg(a);
		std::string ps = textArg(a,"protection","");
		if (!be::canChange()) {
			std::string e,o;
			uint64_t s = 0;
			be::protect(r,addr,s,0,o,e);
			throw std::runtime_error(e);
		}
		uint32_t prot = 0;
		if (ps.empty() || !be::parseProt(ps,prot)) throw std::runtime_error(std::string("protection must be ") + be::protHelp());
		Region before;
		std::string err;
		if (!be::regionAt(r,addr,before,err)) throw std::runtime_error(err);
		if (before.state != StCommit) throw std::runtime_error("that memory is not committed, protection only works on committed pages");
		double sz = 0;
		bool hasSize = wholeArg(a,"size",sz);
		if (hasSize && sz < 1) throw std::runtime_error("size must be at least 1 byte, leave it out for the rest of the region");
		uint64_t size = hasSize ? (uint64_t)sz : before.base + before.size - addr;
		std::string old;
		bool ok = be::protect(r,addr,size,prot,old,err);
		appLog(lbl("Change protection",r,addr) + (ok ? " : OK" : " : failed (" + util::safe(err) + ")"));
		if (!ok) throw std::runtime_error(err);
		changed(r);
		Region after;
		std::string e2;
		Json j = Json::obj();
		j.set("process",procs::refJson(r));
		j.set("address",util::hex(addr)).set("size_bytes",size);
		j.set("old_protection",textOrNull(old));
		j.set("protection",be::regionAt(r,addr,after,e2) ? textOrNull(after.prot) : Json());
		return j;
	}

	Json freeMemory(const Json& a) {
		ProcRef r = procs::target(a,true);
		refuse(r);
		uint64_t addr = addrArg(a);
		std::string mode = textArg(a,"mode","");
		if (mode != "free" && mode != "decommit") throw std::runtime_error("mode must be free (release or unmap the whole allocation) or decommit (private memory only)");
		bool dec = mode == "decommit";
		double sz = 0;
		bool hasSize = wholeArg(a,"size",sz);
		if (hasSize && !dec) throw std::runtime_error("size only goes with decommit, free always takes the whole allocation");
		if (hasSize && sz < 1) throw std::runtime_error("size must be at least 1 byte, leave it out for the rest of the region");
		Region before;
		std::string err;
		if (be::canChange() && !be::regionAt(r,addr,before,err)) throw std::runtime_error(err);
		std::string did;
		bool ok = be::freeMem(r,addr,hasSize ? (uint64_t)sz : 0,dec,did,err);
		appLog(lbl(dec ? "Decommit memory" : "Free memory",r,addr) + (ok ? " : OK" : " : failed (" + util::safe(err) + ")"));
		if (!ok) throw std::runtime_error(err);
		changed(r);
		Region after;
		std::string e2;
		Json j = Json::obj();
		j.set("process",procs::refJson(r));
		j.set("address",util::hex(addr)).set("mode",mode).set("done",did);
		j.set("before",briefJson(before));
		j.set("after",be::regionAt(r,addr,after,e2) ? briefJson(after) : Json());
		return j;
	}

}

namespace vmem {

	void regToolsImpl() {
		mcp::add("Virtual memory","list_regions",
			"Virtual memory regions of a process with classification (stack, heap, TEB, PEB, image, mapped file...) and working set per region, no contents. Virtual size is address space, committed and working set are what costs RAM. summary covers all regions whatever the filters. private_bytes here is private regions only, list_processes private bytes also charges copy on write pages. A protected or limited process gives base, size, type, protection and mapped file only. Up to 1.5 s old (age_ms), changes made through FRS always show.",
			R"j({"type":"object","properties":{
				"pid":{"type":"number","description":"Process id"},
				"start_time":{"type":"string","description":"From list_processes, refuses a reused pid"},
				"include_free":{"type":"boolean","description":"Also free ranges"},
				"include_reserved":{"type":"boolean","description":"Default true"},
				"allocations_only":{"type":"boolean","description":"Only regions at an allocation base"},
				"type":{"type":"string","enum":["private","mapped","image"],"description":"Only this type"},
				"filter":{"type":"string","description":"Text in use or region type, or an address inside the region"},
				"summary_only":{"type":"boolean","description":"No rows"},
				"sort_by":{"type":"string","enum":["base_address","size_bytes","ws_total_bytes","ws_private_bytes","committed_bytes","private_bytes"],"description":"Default base_address"},
				"descending":{"type":"boolean","description":"Sizes default to largest first"},
				"limit":{"type":"number","description":"Rows per page, default 200, max 2000"},
				"offset":{"type":"number","description":"Rows to skip"}
			},"required":["pid"]})j",
			mcp::Read,listRegions);
		mcp::add("Virtual memory","read_memory",
			"Reads bytes of a process as hex, ?? where a page can't be read. May hold passwords and keys. Faults paged out memory back in, so the target's working set can grow. Guard pages are never touched. Refused above 64 KiB. A protected process can't be read.",
			R"j({"type":"object","properties":{
				"pid":{"type":"number","description":"Process id"},
				"start_time":{"type":"string","description":"From list_processes, refuses a reused pid"},
				"address":{"type":"string","description":"Hex string like 0x7ff6a0000000"},
				"size":{"type":"number","description":"Bytes, 1 to 65536"}
			},"required":["pid","start_time","address","size"]})j",
			mcp::Danger,readMemory);
		mcp::add("Virtual memory","write_memory",
			"Overwrites bytes in a process, can crash it. All or nothing up front : every page must be writable, otherwise nothing is written. Returns the old bytes so it can be undone. Max 64 KiB. Some programs ban accounts for this.",
			R"j({"type":"object","properties":{
				"pid":{"type":"number","description":"Process id"},
				"start_time":{"type":"string","description":"From list_processes, refuses a reused pid"},
				"address":{"type":"string","description":"Hex string like 0x7ff6a0000000"},
				"hex":{"type":"string","description":"Bytes to write, like 9090 or 90 90"}
			},"required":["pid","start_time","address","hex"]})j",
			mcp::Danger,writeMemory);
		mcp::add("Virtual memory","protect_memory",
			"Changes page protection, can crash the process. Returns the old value so it can be undone. Windows only, Linux has no remote mprotect.",
			R"j({"type":"object","properties":{
				"pid":{"type":"number","description":"Process id"},
				"start_time":{"type":"string","description":"From list_processes, refuses a reused pid"},
				"address":{"type":"string","description":"Hex string, start of the range"},
				"protection":{"type":"string","description":"NA, R, RW, WC, X, RX, RWX, WCX with optional +G, +NC, +WCM, or a value like 0x04"},
				"size":{"type":"number","description":"Bytes, rounded out to whole pages. Default : to the end of the region"}
			},"required":["pid","start_time","address","protection"]})j",
			mcp::Danger,protectMemory);
		mcp::add("Virtual memory","free_memory",
			"Frees memory in a process, it crashes if it still uses it. free releases a private allocation or unmaps a view, it needs the allocation base. decommit gives the pages back and keeps the range reserved, private memory only. Not a trim : the contents are gone. Windows only.",
			R"j({"type":"object","properties":{
				"pid":{"type":"number","description":"Process id"},
				"start_time":{"type":"string","description":"From list_processes, refuses a reused pid"},
				"address":{"type":"string","description":"Hex string. Allocation base for free"},
				"mode":{"type":"string","enum":["free","decommit"],"description":"free or decommit"},
				"size":{"type":"number","description":"decommit only, bytes, rounded out to whole pages. Default : to the end of the region"}
			},"required":["pid","start_time","address","mode"]})j",
			mcp::Danger,freeMemory);
	}

}
