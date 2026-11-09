#include "sysmem.h"
#include "smbios.h"
#include "mcp.h"
#include "cfg.h"
#include "ram.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace {

	Json by(uint64_t v) {
		return mcp::bytes(v);
	}

	Json txt(const std::string& s) {
		return s.empty() ? Json() : Json(s);
	}

	uint64_t sum2(uint64_t a,uint64_t b) {
		return a == NA ? b : b == NA ? a : a + b;
	}

	Json signedDelta(uint64_t a,uint64_t b) {
		return a == NA || b == NA ? Json() : Json((double)b - (double)a);
	}

	Json detailsJson(const SysMem& m) {
		Json j = Json::obj();
		uint64_t now = util::nowMs();
		j.set("age_ms",m.t && now >= m.t ? Json(now - m.t) : Json());
		j.set("paused",cfg::getb("memPaused",false)).set("elevated",isElevated()).set("page_size",m.pageSize);

		Json ph = Json::obj();
		ph.set("total_bytes",by(m.physTotal)).set("installed_bytes",by(m.physInstalled)).set("available_bytes",by(m.physAvail));
		ph.set("used_bytes",by(m.physUsed)).set("reserved_bytes",by(m.reserved));
		ph.set("cache_ws_bytes",by(m.cacheWs)).set("kernel_ws_bytes",by(m.kernelWs)).set("driver_ws_bytes",by(m.driverWs));
		j.set("physical",ph);

		Json cm = Json::obj();
		cm.set("current_bytes",by(m.commit)).set("peak_bytes",by(m.commitPeak)).set("limit_bytes",by(m.commitLimit));
		cm.set("peak_scope",m.commitPeak == NA ? Json() : Json(m.peakSinceStart ? "since FRS started" : "since boot"));
		j.set("commit",cm);

		Json pp = Json::obj();
		pp.set("working_set_bytes",by(m.pagedWs)).set("virtual_size_bytes",by(m.pagedVs)).set("limit_bytes",Json());
		pp.set("allocs_per_sec",by(m.dPagedAllocs)).set("frees_per_sec",by(m.dPagedFrees));
		pp.set("size_change_bytes_per_sec",m.hasPagedDelta ? Json(m.dPagedBytes) : Json());
		j.set("paged_pool",pp);

		Json np = Json::obj();
		np.set("usage_bytes",by(m.nonPaged)).set("limit_bytes",by(m.nonPagedLimit));
		np.set("limit_scope",m.nonPagedLimit == NA ? Json() : Json("documented formula, RAM or 16 TB, not read from the kernel"));
		np.set("allocs_per_sec",by(m.dNonPagedAllocs)).set("frees_per_sec",by(m.dNonPagedFrees));
		np.set("size_change_bytes_per_sec",m.hasNonPagedDelta ? Json(m.dNonPagedBytes) : Json());
		j.set("nonpaged_pool",np);

		Json pg = Json::obj();
		pg.set("faults_per_sec",by(m.dFaults)).set("page_reads_per_sec",by(m.dPageReads));
		pg.set("pagefile_writes_per_sec",by(m.dPfWrites)).set("mapped_writes_per_sec",by(m.dMappedWrites));
		j.set("paging",pg);

		Json io = Json::obj();
		io.set("read_bytes_per_sec",by(m.dMappedReadBytes)).set("write_bytes_per_sec",by(m.dMappedWriteBytes));
		j.set("mapped_io",io);

		Json pl = Json::obj();
		pl.set("zeroed_bytes",by(m.zeroed)).set("free_bytes",by(m.free)).set("modified_bytes",by(m.modified));
		pl.set("modified_no_write_bytes",by(m.modifiedNoWrite)).set("modified_pagefile_bytes",by(m.modifiedPf)).set("bad_bytes",by(m.bad));
		pl.set("standby_bytes",by(m.standby)).set("repurposed_total_bytes",by(m.repurposedTotal));
		Json sp = Json::arr();
		Json rp = Json::arr();
		for (int i = 0; i < 8; i++) {
			sp.push(by(m.standbyPrio[i]));
			rp.push(by(m.repurposed[i]));
		}
		pl.set("standby_by_priority_bytes",sp).set("repurposed_by_priority_bytes",rp);
		pl.set("unavailable_reason",txt(m.listsWhy));
		j.set("page_lists",pl);

		Json fc = Json::obj();
		fc.set("current_bytes",by(m.fileCache)).set("peak_bytes",by(m.fileCachePeak)).set("min_bytes",by(m.fileCacheMin)).set("max_bytes",by(m.fileCacheMax));
		fc.set("current_including_transition_bytes",by(m.fileCacheTrans));
		fc.set("unavailable_reason",txt(m.cacheWhy));
		j.set("file_cache",fc);

		Json cp = Json::obj();
		cp.set("store_ws_bytes",by(m.compWs)).set("total_data_bytes",by(m.compData)).set("total_compressed_bytes",by(m.compSize)).set("unique_data_bytes",by(m.compUnique));
		bool hasRatio = m.compData != NA && m.compSize != NA && m.compData && m.compSize;
		double ratio = hasRatio ? (double)m.compData / (double)m.compSize : 0.0;
		cp.set("ratio",hasRatio ? Json(std::round(ratio * 100.0) / 100.0) : Json());
		cp.set("saved_bytes",hasRatio && m.compWs != NA ? Json(std::max(0.0,(double)m.compWs * ratio - (double)m.compWs)) : Json());
		cp.set("pid",m.compPid ? Json(m.compPid) : Json());
		cp.set("unavailable_reason",txt(m.compWhy));
		j.set("compression",cp);

		Json ct = Json::obj();
		ct.set("processes",m.processes ? Json(m.processes) : Json()).set("threads",m.threads ? Json(m.threads) : Json()).set("handles",m.handles ? Json(m.handles) : Json());
		j.set("counts",ct);

		Json ex = Json::obj();
		for (auto& e : m.extra) ex.set(e.first,by(e.second));
		j.set("extra_bytes",ex);
		Json ec = Json::obj();
		for (auto& e : m.counts) ec.set(e.first,by(e.second));
		j.set("extra_counts",ec);
		j.set("query_error",txt(m.perfWhy));
		return j;
	}

	Json snapJson(const std::shared_ptr<const SysMem>& s) {
		if (!s) return Json();
		Json j = Json::obj();
		j.set("available_bytes",by(s->physAvail)).set("used_bytes",by(s->physUsed)).set("commit_bytes",by(s->commit));
		j.set("standby_bytes",by(s->standby)).set("modified_bytes",by(s->modified)).set("free_bytes",by(s->free)).set("zeroed_bytes",by(s->zeroed));
		j.set("file_cache_bytes",by(s->fileCache)).set("compression_ws_bytes",by(s->compWs));
		return j;
	}

	Json runJson(const std::shared_ptr<sysmem::Run>& r) {
		static const char* const names[] = {"waiting","running","ok","failed","cancelled","unavailable"};
		Json j = Json::obj();
		bool done = r->done;
		j.set("command",r->id).set("running",!done);
		Json steps = Json::arr();
		int bad = 0;
		{
			std::lock_guard<std::mutex> lk(r->m);
			for (auto& s : r->steps) {
				Json o = Json::obj();
				o.set("command",s.id).set("label",s.label).set("status",names[s.st]).set("ok",s.st == 2);
				o.set("message",txt(s.res.msg));
				Json d = Json::arr();
				for (auto& it : s.res.items) {
					Json x = Json::obj();
					x.set("name",it.first).set("status",it.second);
					d.push(x);
				}
				o.set("details",d);
				steps.push(o);
				if (s.st == 3 || s.st == 4) bad++;
			}
		}
		j.set("all_ok",done ? Json(bad == 0) : Json());
		j.set("steps",steps);
		auto b = done ? r->before : nullptr;
		auto a = done ? r->after : nullptr;
		j.set("before",snapJson(b)).set("after",snapJson(a));
		bool both = b && a;
		j.set("freed_bytes",both ? signedDelta(sum2(b->free,b->zeroed),sum2(a->free,a->zeroed)) : Json());
		j.set("available_delta_bytes",both ? signedDelta(b->physAvail,a->physAvail) : Json());
		j.set("standby_delta_bytes",both ? signedDelta(b->standby,a->standby) : Json());
		j.set("modified_delta_bytes",both ? signedDelta(b->modified,a->modified) : Json());
		return j;
	}

	Json recJson(const smbios::Rec& r,bool serial) {
		Json j = Json::obj();
		j.set("title",r.title);
		for (auto& f : r.f) {
			if (f.key == "serial_number" && !serial) j.set(f.key,Json());
			else if (f.isNum) j.set(f.key,by(f.num));
			else j.set(f.key,txt(f.val));
		}
		return j;
	}

	Json recs(const std::vector<smbios::Rec>& v,bool serial) {
		Json a = Json::arr();
		for (auto& r : v) a.push(recJson(r,serial));
		return a;
	}

	struct Stat { double mn = 0,mx = 0,sum = 0; int n = 0; };

	void statAdd(Stat& s,float v) {
		if (v < 0.0f) return;
		double b = (double)v * 1048576.0;
		s.mn = s.n ? std::min(s.mn,b) : b;
		s.mx = s.n ? std::max(s.mx,b) : b;
		s.sum += b;
		s.n++;
	}

	Json statJson(const Stat& s,uint64_t cur,uint64_t lim) {
		Json j = Json::obj();
		j.set("current_bytes",by(cur)).set("limit_bytes",by(lim));
		j.set("min_bytes",s.n ? Json(std::round(s.mn)) : Json()).set("max_bytes",s.n ? Json(std::round(s.mx)) : Json());
		j.set("avg_bytes",s.n ? Json(std::round(s.sum / s.n)) : Json());
		return j;
	}

	Json mbJson(float v) {
		return v < 0.0f ? Json() : Json(std::round((double)v * 1048576.0));
	}

}

namespace sysmem {

	void regTools() {
		mcp::add("Memory","memory_details",
			"Everything on the Memory page : physical, commit, pools, paging, page lists by priority, file cache, compression store. "
			"Sizes in bytes, null means unknown or needs admin. Available includes standby : standby is cache that can be reused, not lost memory. "
#ifdef _WIN32
			"paging rates count pages, not I/Os : page_reads_per_sec is perfmon Pages Input/sec, not Page Reads/sec.",
#else
			"paging : page_reads_per_sec is major faults (pgmajfault), pagefile_writes_per_sec is pages swapped out (pswpout), mapped_writes_per_sec is all dirty writeback (nr_written).",
#endif
			R"j({"type":"object","properties":{}})j",mcp::Read,
			[](const Json&) {
				auto m = latest();
				if (!m->t) m = sampleNow();
				Json j = detailsJson(*m);
				auto r = lastRun();
				j.set("last_command",r ? runJson(r) : Json());
				return j;
			});

		mcp::add("Memory","memory_history",
			"Commit and physical history, one sample per tick, newest first. min, max and avg cover the whole ring. "
			"Each sample names the biggest process, by working set (includes shared pages) and by private bytes. Null until the process list has rows.",
			R"j({"type":"object","properties":{
				"limit":{"type":"integer","description":"samples to return, default 120, max 3600"},
				"offset":{"type":"integer","description":"samples to skip from the newest, default 0"}}})j",mcp::Read,
			[](const Json& a) {
				auto h = history();
				auto m = latest();
				Stat sc;
				Stat sp;
				for (float v : h->commit) statAdd(sc,v);
				for (float v : h->phys) statAdd(sp,v);
				std::vector<Json> rows;
				size_t n = h->commit.size();
				double lim = a.num("limit",120);
				double off = a.num("offset",0);
				size_t want = (lim < 1.0 ? 1 : lim > 3600.0 ? 3600 : (size_t)lim) + (off <= 0.0 ? 0 : off > 3600.0 ? 3600 : (size_t)off);
				rows.reserve(std::min(n,want));
				for (size_t k = 0; k < n; k++) {
					// rows past what this page can show only need to exist for the counts
					if (k >= want) {
						rows.emplace_back();
						continue;
					}
					size_t i = n - 1 - k;
					Json r = Json::obj();
					r.set("unix_sec",h->unixSec[i]).set("time",util::timeStr(h->unixSec[i]));
					r.set("commit_bytes",mbJson(h->commit[i])).set("physical_bytes",mbJson(h->phys[i]));
					r.set("standby_bytes",mbJson(h->standby[i])).set("modified_bytes",mbJson(h->modified[i])).set("free_bytes",mbJson(h->freeMem[i]));
					r.set("cache_bytes",mbJson(h->cache[i])).set("kernel_bytes",mbJson(h->kernel[i]));
					r.set("top_process",txt(h->topProc[i])).set("top_process_ws_bytes",by(h->topProcBytes[i]));
					r.set("top_private_process",txt(h->topPriv[i])).set("top_private_bytes",by(h->topPrivBytes[i]));
					rows.push_back(std::move(r));
				}
				Json j = mcp::paged(rows,a,120,3600,"samples");
				j.set("interval_ms",sampler::intervalMs());
				j.set("commit",statJson(sc,m->commit,m->commitLimit)).set("physical",statJson(sp,m->physUsed,m->physTotal));
				return j;
			});

		mcp::add("Memory","memory_command",
			"Empties one system page list or every working set, finer than clean. Waits by default, returns before, after, freed and a status per step. Refuses while a clean runs. "
			"Trimmed pages go to standby or modified and fault back on use. Standby is reusable cache that available already counts, so judge by freed_bytes (free and zeroed lists). "
#ifdef _WIN32
			"compression normally comes back unavailable, FRS has no kernel driver. "
#else
			"registry_cache and standby_low don't exist on Linux, compression compacts zram. combine_pages, also part of all, turns KSM on and it stays on until reboot. "
#endif
			"A step with status unavailable can't run here and does not count against all_ok.",
			R"j({"type":"object","properties":{
				"command":{"type":"string","enum":["combine_pages","compression","file_cache","registry_cache","empty_working_sets","modified","modified_file_cache","standby","standby_low","all"],"description":"what to empty"},
				"wait":{"type":"boolean","description":"wait for the end, default true. false returns at once, memory_details last_command shows the progress"}},"required":["command"]})j",mcp::Write,
			[](const Json& a) {
				std::string id = a.str("command","");
				if (id.empty()) throw std::runtime_error("command is required, see the tool schema for the names");
				bool known = id == "all";
				for (int i = 0; i < kCmdN; i++) if (id == kCmds[i].id) known = true;
				if (!known) throw std::runtime_error("unknown command '" + util::safe(id) + "', see the tool schema for the names");
				std::shared_ptr<Run> r;
				std::string why = start(id,true,&r);
				if (!why.empty()) throw std::runtime_error(why);
				if (a.flag("wait",true)) {
					uint64_t end = util::nowMs() + 600000;
					while (!r->done && util::nowMs() < end) {
						if (!mcp::alive()) throw std::runtime_error("MCP server is stopping, the command keeps running");
						std::this_thread::sleep_for(std::chrono::milliseconds(50));
					}
				}
				return runJson(r);
			});

		mcp::add("Memory","ram_modules",
			"RAM sticks and arrays from the SMBIOS table (types 16, 17, 18, 19, 20). Firmware text, it can be blank or wrong, and some boards report slots they don't have. "
			"Serial numbers stay null unless include_serial is true.",
			R"j({"type":"object","properties":{
				"include_serial":{"type":"boolean","description":"also return stick serial numbers, default false"}}})j",mcp::Read,
			[](const Json& a) {
				smbios::load();
				auto t = smbios::latest();
				bool serial = a.flag("include_serial",false);
				Json j = Json::obj();
				j.set("ok",t->ok).set("reason",txt(t->err));
				j.set("smbios_version",t->ok && t->major ? Json(std::to_string(t->major) + "." + std::to_string(t->minor)) : Json());
				j.set("slots_used",t->ok ? Json(t->slotsUsed) : Json()).set("slots_total",t->ok ? Json(t->slotsTotal) : Json());
				j.set("installed_bytes",by(t->installed)).set("speed_mts",by(t->speed));
				j.set("form_factor",txt(t->formFactor)).set("memory_type",txt(t->memType)).set("technology",txt(t->tech));
				j.set("arrays",recs(t->arrays,serial)).set("modules",recs(t->devices,serial));
				j.set("memory_errors",recs(t->errors,serial)).set("array_maps",recs(t->arrayMaps,serial)).set("device_maps",recs(t->deviceMaps,serial));
				return j;
			});
	}

}
