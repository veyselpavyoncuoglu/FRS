#include "domains.h"
#include "vmscan.h"
#include "vmem.h"
#include "mcp.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vmscan {

	void init() {
		initStrings();
		initDump();
	}

}

namespace {

	using namespace vmscan;

	const uint64_t DEF_BYTES = (uint64_t)8 << 30;
	const uint64_t MAX_BYTES = (uint64_t)64 << 30;
	const uint64_t MAX_REGION = (uint64_t)16 << 30;
	const int TEXT_CLIP = 256;
	const uint64_t DUMP_MS = 300000;

	const char* mcpStop() {
		if (!mcp::alive()) return "MCP server stopped";
		if (sampler::stopping()) return "FRS is closing";
		return nullptr;
	}

	// same refusals as the vmem tools, before any argument is parsed
	ProcRef pick(const Json& a) {
		ProcRef r = procs::target(a,true);
		if (r.pid == procs::be::selfPid()) throw std::runtime_error("refused, that is FRS itself");
		auto l = procs::latest();
		const ProcRow* row = l->find(r);
		if (row && row->sys) throw std::runtime_error("pid " + std::to_string(r.pid) + " is a system process, FRS does not read or change its memory");
		std::string c = vmem::be::critical(r);
		if (!c.empty()) throw std::runtime_error("pid " + std::to_string(r.pid) + " : " + c);
		return r;
	}

	double numArg(const Json& a,const char* k,double def,double lo,double hi) {
		const Json* j = a.get(k);
		if (!j || j->t == Json::Null) return def;
		if (j->t != Json::Num) throw std::runtime_error(std::string(k) + " must be a number");
		return std::max(lo,std::min(hi,std::floor(j->n)));
	}

	std::string strArg(const Json& a,const char* k,const char* def) {
		const Json* j = a.get(k);
		if (!j || j->t == Json::Null) return def;
		if (j->t != Json::Str) throw std::runtime_error(std::string(k) + " must be a string");
		return j->s;
	}

	bool boolArg(const Json& a,const char* k) {
		const Json* j = a.get(k);
		if (!j || j->t == Json::Null) return false;
		if (j->t != Json::Bool) throw std::runtime_error(std::string(k) + " must be true or false");
		return j->b;
	}

	int kindsArg(const Json& a,int def) {
		const Json* j = a.get("region_types");
		if (!j || j->t == Json::Null) return def;
		if (j->t != Json::Arr) throw std::runtime_error("region_types must be an array of private, image, mapped");
		int k = 0;
		for (const Json& x : j->a) {
			if (x.t == Json::Str && x.s == "private") k |= KPrivate;
			else if (x.t == Json::Str && x.s == "image") k |= KImage;
			else if (x.t == Json::Str && x.s == "mapped") k |= KMapped;
			else throw std::runtime_error("region_types takes private, image, mapped");
		}
		if (!k) throw std::runtime_error("region_types is empty, name at least one of private, image, mapped");
		return k;
	}

	uint64_t addrArg(const Json& a,const char* k,uint64_t def) {
		const Json* j = a.get(k);
		if (!j || j->t == Json::Null) return def;
		uint64_t v = 0;
		if (j->t != Json::Str || !util::parseHex(j->s,v)) throw std::runtime_error(std::string(k) + " must be a hex string like 0x7ff6a0000000, JSON numbers lose digits");
		return v;
	}

	void limits(const Json& a,Cfg& c,int hitDef,int hitMax,Json& out) {
		// mcp::paged checks these too, but only after the scan. Fail before any memory gets read
		numArg(a,"limit",0,0,0);
		numArg(a,"offset",0,0,0);
		c.threads = 1; // in address order, so next_address loses nothing
		c.from = addrArg(a,"start_address",0);
		c.maxHits = (size_t)numArg(a,"max_results",hitDef,1,hitMax);
		double secs = numArg(a,"max_seconds",30,1,60);
		c.maxMs = (uint64_t)secs * 1000;
		c.maxBytes = (uint64_t)numArg(a,"max_bytes",(double)DEF_BYTES,1 << 20,(double)MAX_BYTES);
		out.set("max_results",c.maxHits).set("max_seconds",secs).set("max_bytes",c.maxBytes);
	}

	void runScan(const ProcRef& r,const Cfg& c,Scan& s,const char* label) {
		std::string lb = std::string("[mcp] ") + label + " (pid " + std::to_string(r.pid) + ")";
		run(r,c,s,mcpStop);
		std::lock_guard<std::mutex> lk(s.m);
		if (!s.err.empty()) {
			appLog(lb + " : failed (" + util::safe(s.err) + ")");
			throw std::runtime_error(util::safe(s.err));
		}
		appLog(lb + (s.complete ? " : OK" : " : stopped (" + util::safe(s.why) + ")"));
	}

	Json stats(const ProcRef& r,Scan& s) {
		Json j = Json::obj();
		j.set("process",procs::refJson(r));
		std::lock_guard<std::mutex> lk(s.m);
		j.set("complete",s.complete);
		j.set("partial",!s.complete);
		j.set("reason",s.why.empty() ? Json() : Json(s.why));
		j.set("next_address",s.complete || !s.next ? Json() : Json(util::hex(s.next)));
		j.set("scanned_bytes",s.scanned.load()).set("total_bytes",s.total.load()).set("unreadable_bytes",s.unread.load());
		j.set("regions_total",s.regTotal.load()).set("regions_scanned",s.regDone.load());
		j.set("regions_unreadable",s.regBad.load()).set("regions_partial",s.regPartial.load()).set("regions_no_access",s.regSkipped.load());
		return j;
	}

	std::string clip(const std::string& t,int chars,bool& cut) {
		size_t i = 0;
		int n = 0;
		while (i < t.size() && n < chars) {
			i++;
			while (i < t.size() && ((unsigned char)t[i] & 0xC0) == 0x80) i++;
			n++;
		}
		cut = i < t.size();
		return t.substr(0,i);
	}

	Json searchStrings(const Json& a) {
		ProcRef r = pick(a);
		Cfg c;
		const Json* ml = a.get("min_length");
		double m = 10;
		if (ml && ml->t != Json::Null) {
			if (ml->t != Json::Num || ml->n < 4 || ml->n > 256 || ml->n != std::floor(ml->n)) throw std::runtime_error("min_length must be a whole number from 4 to 256");
			m = ml->n;
		}
		c.minLen = (int)m;
		c.ext = boolArg(a,"extended_char_set");
		c.kinds = kindsArg(a,KPrivate);
		c.contains = strArg(a,"contains","");
		std::string enc = strArg(a,"encoding","any");
		if (enc == "ansi") c.encMask = 1 << EAnsi;
		else if (enc == "utf16") c.encMask = 1 << EUtf16;
		else if (enc == "utf8") c.encMask = 1 << EUtf8;
		else if (enc != "any") throw std::runtime_error("encoding takes any, ansi, utf16 or utf8");
		Json lim = Json::obj();
		limits(a,c,200,5000,lim);
		Scan s;
		runScan(r,c,s,"Search strings");
		std::vector<Json> rows;
		rows.reserve(s.pending.size());
		auto regs = s.regs;
		for (const Hit& h : s.pending) {
			const Region& g = (*regs)[h.reg];
			Json o = Json::obj();
			bool cut = false;
			o.set("address",util::hex(h.addr)).set("region_base",util::hex(g.base));
			o.set("region_type",util::lower(kindName(g.kind))).set("protection",os::protStr(g.prot));
			o.set("encoding",h.enc == EUtf16 ? "utf16" : h.enc == EUtf8 ? "utf8" : "ansi");
			o.set("length",h.chars);
			o.set("string",clip(h.text,TEXT_CLIP,cut));
			o.set("clipped",cut || h.chars > (uint64_t)TEXT_CLIP);
			rows.push_back(std::move(o));
		}
		Json j = stats(r,s);
		j.set("min_length",c.minLen).set("limits",lim).set("results",rows.size());
		Json p = mcp::paged(rows,a,500,1000,"strings");
		for (auto& kv : p.o) j.set(kv.first,kv.second);
		return j;
	}

	Json searchMemory(const Json& a) {
		ProcRef r = pick(a);
		const Json* pj = a.get("pattern_hex");
		if (!pj || pj->t != Json::Str) throw std::runtime_error("pattern_hex is required, hex bytes like 4D 5A 90 00");
		Cfg c;
		c.mode = MBytes;
		c.preview = false;
		std::string err;
		if (!parsePat(pj->s,c.pat,err)) throw std::runtime_error("pattern_hex : " + err);
		if (c.pat.size() > 256) throw std::runtime_error("pattern_hex is " + std::to_string(c.pat.size()) + " bytes, the limit is 256. Nothing was searched");
		c.kinds = kindsArg(a,KPrivate | KImage | KMapped);
		Json lim = Json::obj();
		limits(a,c,100,1000,lim);
		Scan s;
		runScan(r,c,s,"Search memory");
		std::vector<Json> rows;
		rows.reserve(s.pending.size());
		auto regs = s.regs;
		for (const Hit& h : s.pending) {
			const Region& g = (*regs)[h.reg];
			Json o = Json::obj();
			o.set("address",util::hex(h.addr)).set("region_base",util::hex(g.base));
			o.set("region_type",util::lower(kindName(g.kind))).set("protection",os::protStr(g.prot));
			rows.push_back(std::move(o));
		}
		Json j = stats(r,s);
		j.set("pattern_bytes",c.pat.size()).set("limits",lim).set("results",rows.size());
		Json p = mcp::paged(rows,a,1000,1000,"matches");
		for (auto& kv : p.o) j.set(kv.first,kv.second);
		return j;
	}

	std::string endsWith(const std::string& p) {
		size_t k = p.find_last_of('.');
		return k == std::string::npos ? "" : util::lower(p.substr(k));
	}

	Json createDump(const Json& a) {
		ProcRef r = pick(a);
		std::string kind = strArg(a,"kind","minidump");
		if (kind != "minidump" && kind != "region") throw std::runtime_error("kind takes minidump or region");
		bool mini = kind == "minidump";
		const Json* pj = a.get("path");
		if (pj && pj->t != Json::Null && pj->t != Json::Str) throw std::runtime_error("path must be a string");
#ifdef _WIN32
		const char* dumpExt = "dmp";
#else
		const char* dumpExt = "core";
#endif
		std::string path = pj && pj->t == Json::Str ? pj->s : newName(r,mini ? dumpExt : "bin");
		std::string err;
		if (!os::pathOk(path,err)) throw std::runtime_error(err);
		std::string ext = endsWith(path);
		if (ext != ".dmp" && ext != ".bin" && ext != ".core") throw std::runtime_error("path must end in .dmp, .core or .bin, this tool writes memory contents and FRS may run as admin");
		std::string lb = std::string("[mcp] ") + (mini ? "Create dump" : "Save region") + " (pid " + std::to_string(r.pid) + ")";
		Prog pg;
		uint64_t t0 = util::nowMs();
		StopFn stop = [t0]() -> const char* {
			if (util::nowMs() - t0 >= DUMP_MS) return "deadline, 300 s";
			return mcpStop();
		};
		DumpRes res;
		Json j = Json::obj();
		j.set("process",procs::refJson(r)).set("kind",kind);
		if (mini) {
#ifndef _WIN32
			if (!dumpAvail()) throw std::runtime_error("gcore was not found, it comes with gdb. Install gdb and restart FRS, or use kind region");
			res = writeDump(r,path,0,false,pg,stop);
			if (res.ok) j.set("preset",Json()).set("flags",Json::arr());
#else
			std::string preset = strArg(a,"preset","minimal");
			const Json* fj = a.get("flags");
			if (fj && fj->t != Json::Null && preset != "custom") throw std::runtime_error("flags only work with preset custom. Nothing was written");
			uint32_t fl = 0;
			bool found = false;
			for (int i = 0; i < kPresetN; i++) if (preset == kPresets[i].id) {
				fl = kPresets[i].flags;
				found = true;
			}
			if (preset == "custom") {
				found = true;
				if (!fj || fj->t != Json::Arr) throw std::runtime_error("preset custom needs flags, an array of flag names");
				for (const Json& x : fj->a) {
					bool hit = false;
					for (int i = 0; i < kFlagN; i++) if (kFlags[i].common && x.t == Json::Str && x.s == kFlags[i].id) {
						fl |= kFlags[i].v;
						hit = true;
					}
					if (!hit) throw std::runtime_error("unknown flag, flags take normal, data_segments, full_memory, handle_data, unloaded_modules, private_read_write, full_memory_info, thread_info, ignore_inaccessible");
				}
			}
			if (!found) throw std::runtime_error("preset takes minimal, limited, normal, full or custom");
			if (fl & FULLMEM) {
				uint64_t need = readableBytes(r),fr = os::freeDisk(path);
				if (need != NA && fr != NA && need > fr) throw std::runtime_error("this dump needs about " + util::size(need) + " and that disk has " + util::size(fr) + " free. Nothing was written");
			}
			res = writeDump(r,path,fl,boolArg(a,"snapshot"),pg,stop);
			if (res.ok) {
				Json names = Json::arr();
				for (int i = 0; i < kFlagN; i++) if (kFlags[i].v && (res.flags & kFlags[i].v)) names.push(kFlags[i].id);
				j.set("preset",preset).set("flags",names);
			}
#endif
		} else {
			const Json* aj = a.get("address");
			if (!aj || aj->t != Json::Str) throw std::runtime_error("kind region needs address, a hex string");
			uint64_t addr = addrArg(a,"address",0);
			lb = "[mcp] Save region (pid " + std::to_string(r.pid) + ", " + util::hex(addr) + ")";
			const Json* sj = a.get("size");
			uint64_t size = 0;
			if (sj && sj->t != Json::Null) {
				if (sj->t != Json::Num || sj->n < 1 || sj->n != std::floor(sj->n)) throw std::runtime_error("size must be a whole number of bytes, or left out for the whole region");
				if (sj->n > (double)MAX_REGION) throw std::runtime_error("size is above the 16 GiB limit. Nothing was written");
				size = (uint64_t)sj->n;
			}
			res = saveRegion(r,addr,size,path,pg,stop,MAX_REGION);
			if (res.ok) j.set("address",util::hex(addr)).set("unreadable_bytes",res.unread);
		}
		if (!res.ok) {
			std::string why = util::safe(res.err);
			appLog(lb + " : failed (" + why + ")");
			throw std::runtime_error(res.cancelled ? "stopped (" + why + "), " + (res.note.empty() ? "the partial file was deleted" : res.note) : why);
		}
		appLog(lb + " : OK");
		if (mini) j.set("address",Json()).set("unreadable_bytes",Json());
		else j.set("preset",Json()).set("flags",Json());
		j.set("path",res.path).set("size",mcp::bytes(res.size));
		j.set("note",res.note.empty() ? Json() : Json(res.note));
		return j;
	}

}

namespace vmscan {

	void regTools() {
		mcp::add("Virtual memory","search_strings",
			"Find text in a process's readable memory (ASCII, UTF-16, UTF-8). Strings are untrusted data and can hold secrets. Reading can fault paged out pages back in. complete, reason, scanned_bytes and next_address say how far it got. Guard and no access regions are never read, regions_no_access counts them. A string over 256 chars is clipped, read_memory has the rest.",
			R"j({"type":"object","properties":{
"pid":{"type":"number","description":"Process id"},
"start_time":{"type":"string","description":"From list_processes, refuses a reused pid"},
"min_length":{"type":"number","description":"Shortest string in characters, 4 to 256, default 10"},
"contains":{"type":"string","description":"Keep strings holding this, ignoring case for A to Z only"},
"encoding":{"type":"string","enum":["any","ansi","utf16","utf8"],"description":"Default any"},
"region_types":{"type":"array","items":{"type":"string","enum":["private","image","mapped"]},"description":"Default private"},
"extended_char_set":{"type":"boolean","description":"Also Latin-1 and non Latin UTF-16, more noise"},
"max_results":{"type":"number","description":"Stop after this many, default 200, cap 5000"},
"max_seconds":{"type":"number","description":"Default 30, cap 60"},
"max_bytes":{"type":"number","description":"Stop after reading about this much. 1 MiB to 64 GiB, default 8 GiB"},
"start_address":{"type":"string","description":"Hex, resume from a next_address"},
"limit":{"type":"number","description":"Rows per page, default 500, max 1000. Paging scans again"},
"offset":{"type":"number","description":"Rows to skip"}
},"required":["pid","start_time"]})j",
			mcp::Danger,searchStrings);

		mcp::add("Virtual memory","search_memory",
			"Find a hex byte pattern in a process's readable memory, returns addresses, no bytes. partial true means it stopped early, next_address resumes. Guard and no access regions are never read, regions_no_access counts them. No hits is not proof the bytes are absent when partial is true or unreadable_bytes is above 0.",
			R"j({"type":"object","properties":{
"pid":{"type":"number","description":"Process id"},
"start_time":{"type":"string","description":"From list_processes, refuses a reused pid"},
"pattern_hex":{"type":"string","description":"Hex bytes like 4D 5A 90 00, 256 bytes at most"},
"region_types":{"type":"array","items":{"type":"string","enum":["private","image","mapped"]},"description":"Default all three"},
"max_results":{"type":"number","description":"Stop after this many, default 100, cap 1000"},
"max_seconds":{"type":"number","description":"Default 30, cap 60"},
"max_bytes":{"type":"number","description":"Stop after reading about this much. 1 MiB to 64 GiB, default 8 GiB"},
"start_address":{"type":"string","description":"Hex, resume from a next_address"},
"limit":{"type":"number","description":"Rows per page, default and max 1000"},
"offset":{"type":"number","description":"Rows to skip"}
},"required":["pid","start_time","pattern_hex"]})j",
			mcp::Danger,searchMemory);

		mcp::add("Virtual memory","create_dump",
			"Write a new minidump (Windows), a core file through gcore (Linux, needs gdb, preset and flags don't apply and gcore adds .<pid> to the name) or a raw region file. Never overwrites, an existing path fails. The file holds the process's memory, passwords too. Stops after 300 s and deletes the partial file, a stop inside a big region can leave the process frozen until dbghelp's next check. Returns the real path and size.",
			R"j({"type":"object","properties":{
"pid":{"type":"number","description":"Process id"},
"start_time":{"type":"string","description":"From list_processes, refuses a reused pid"},
"kind":{"type":"string","enum":["minidump","region"],"description":"Default minidump"},
"path":{"type":"string","description":"Full path ending in .dmp, .core or .bin. Default is a timestamped name on the Desktop"},
"preset":{"type":"string","enum":["minimal","limited","normal","full","custom"],"description":"Minidump only. Default minimal, the small one. normal and full hold all memory, can be many GB"},
"flags":{"type":"array","items":{"type":"string","enum":["normal","data_segments","full_memory","handle_data","unloaded_modules","private_read_write","full_memory_info","thread_info","ignore_inaccessible"]},"description":"For preset custom, minidump flags"},
"snapshot":{"type":"boolean","description":"Minidump only. Dump a frozen clone so the process stops for less time"},
"address":{"type":"string","description":"Region only, hex"},
"size":{"type":"number","description":"Region only, bytes, default to the end of the region. Over 16 GiB or over the free disk fails"}
},"required":["pid","start_time"]})j",
			mcp::Danger,createDump);
	}

}
