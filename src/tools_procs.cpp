#include "procs.h"
#include "cfg.h"
#include "mcp.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace {

	using namespace procs;
	using mcp::textArg;
	using mcp::flagArg;
	using mcp::numArg;

	Json flags(const ProcRow& r,Json j) {
		j.set("threads",r.threads).set("pseudo",r.pseudo).set("system",r.sys);
		j.set("memory_compression",r.memCompression).set("registry",r.registry).set("is_new",r.isNew);
		j.set("handle_denied",r.noHandle).set("ws_denied",r.noFull).set("handle_filtered",r.filtered);
		return j;
	}

	Json rowJson(const ProcRow& r) {
		Json j = Json::obj();
		j.set("pid",r.pid).set("name",r.name).set("start_time",std::to_string(r.start));
		j.set("ppid",r.ppid).set("session",r.session);
		j.set("user",r.user.empty() ? Json() : Json(r.user));
		j.set("path",r.path.empty() ? Json() : Json(r.path));
		for (int c = 0; c < COLS; c++) {
			if (!cols[c].key || cols[c].kind == KText || c == CPid) continue;
			int64_t v = 0;
			if (colNum(r,c,v)) j.set(cols[c].key,v);
			else j.set(cols[c].key,Json());
			if (c == CPrio) j.set("page_priority_name",r.pagePrio < 0 ? Json() : Json(pagePrioName(r.pagePrio)));
		}
		return flags(r,j);
	}

	Json groupJson(const ProcRow& g,const std::vector<uint32_t>& pids) {
		Json j = Json::obj();
		j.set("name",g.name).set("count",g.children);
		Json p = Json::arr();
		for (uint32_t x : pids) p.push(x);
		j.set("pids",p);
		for (int c = 0; c < COLS; c++) {
			if (!cols[c].key || !cols[c].sum) continue;
			int64_t v = 0;
			if (colNum(g,c,v)) j.set(cols[c].key,v);
			else j.set(cols[c].key,Json());
		}
		return j;
	}

	Json page(const std::vector<Json>& rows,const Json& a,int def,int mx,const char* key) {
		numArg(a,"limit",def);
		numArg(a,"offset",0);
		return mcp::paged(rows,a,def,mx,key);
	}

	Json errJson() {
		std::string e = snapErr();
		return e.empty() ? Json() : Json(e);
	}

	std::string keyList(bool grp) {
		std::string s;
		for (int c = 0; c < COLS; c++) if (cols[c].key && (!grp || c == CName || cols[c].sum)) {
			if (!s.empty()) s += ", ";
			s += cols[c].key;
		}
		return s;
	}

	int colByKey(const std::string& k) {
		for (int c = 0; c < COLS; c++) if (cols[c].key && k == cols[c].key) return c;
		throw std::runtime_error("sort_by must be one of : " + keyList(false));
	}

	Json listProcesses(const Json& a) {
		waitFirst();
		int col = colByKey(textArg(a,"sort_by","working_set_bytes"));
		bool textCol = cols[col].kind == KText;
		bool desc = flagArg(a,"descending",!textCol && col != CPid);
		std::string low = util::lower(textArg(a,"filter",""));
		bool grp = flagArg(a,"group",false);
		if (grp && col != CName && !cols[col].sum) throw std::runtime_error("with group, sort_by must be one of : " + keyList(true));
		bool want = flagArg(a,"ws_counters",false);
		if (want) wantWs(30000);
		auto l = latest();
		std::vector<const ProcRow*> sel;
		for (auto& r : l->rows) if (rowMatch(r,low)) sel.push_back(&r);
		std::vector<Json> rows;
		if (grp) {
			std::map<std::string,std::pair<ProcRow,std::vector<uint32_t>>> m;
			for (auto* r : sel) {
				if (r->pseudo) continue;
				auto& e = m[util::lower(r->name)];
				if (e.first.name.empty()) e.first.name = r->name;
				addSum(e.first,*r);
				e.first.children++;
				e.second.push_back(r->pid);
			}
			std::vector<const std::pair<ProcRow,std::vector<uint32_t>>*> gs;
			for (auto& kv : m) gs.push_back(&kv.second);
			std::sort(gs.begin(),gs.end(),[&](auto* x,auto* y) { return rowLess(x->first,y->first,col,desc); });
			rows.reserve(gs.size());
			for (auto* g : gs) rows.push_back(groupJson(g->first,g->second));
		} else {
			std::sort(sel.begin(),sel.end(),[&](const ProcRow* x,const ProcRow* y) { return rowLess(*x,*y,col,desc); });
			rows.reserve(sel.size());
			for (auto* r : sel) rows.push_back(rowJson(*r));
		}
		Json j = page(rows,a,50,1000,grp ? "groups" : "processes");
		j.set("seq",l->seq).set("sample_age_ms",util::nowMs() - l->t).set("interval_ms",sampler::intervalMs());
		bool paused = cfg::getb("memPaused",false);
		j.set("paused",paused).set("snapshot_error",errJson());
		j.set("sum_working_set_bytes",l->sumWs).set("sum_private_bytes",l->sumPriv);
		j.set("ws_counters",wsLive() ? "on" : want && !paused ? "pending" : "off");
		return j;
	}

	Json rankProcesses(const Json& a) {
		waitFirst();
		std::string by = textArg(a,"rank_by","private_growth");
		bool pg = by == "private_growth";
		bool wg = by == "working_set_growth";
		bool pb = by == "private_bytes";
		bool wb = by == "working_set_bytes";
		if (!pg && !wg && !pb && !wb) throw std::runtime_error("rank_by must be one of : private_growth, working_set_growth, private_bytes, working_set_bytes");
		int win = (int)std::max(1.0,std::min(86400.0,numArg(a,"window_seconds",60.0)));
		std::string low = util::lower(textArg(a,"filter",""));
		auto l = latest();
		std::vector<Growth> gr = growth(*l,win);
		struct R {
			const ProcRow* r;
			const Growth* g;
			double key;
			bool known;
		};
		std::vector<R> v;
		for (size_t i = 0; i < l->rows.size(); i++) {
			const ProcRow& r = l->rows[i];
			if (r.pseudo || !rowMatch(r,low)) continue;
			R x{&r,&gr[i],0.0,false};
			if (pg || wg) {
				x.known = gr[i].n >= 2;
				x.key = pg ? gr[i].priv : gr[i].ws;
			} else {
				uint64_t u = pb ? r.priv : r.ws;
				x.known = u != NA;
				x.key = (double)u;
			}
			v.push_back(x);
		}
		std::sort(v.begin(),v.end(),[](const R& x,const R& y) {
			if (x.known != y.known) return x.known;
			if (x.known && x.key != y.key) return x.key > y.key;
			return x.r->pid < y.r->pid;
		});
		std::vector<Json> rows;
		rows.reserve(v.size());
		for (auto& x : v) {
			Json j = Json::obj();
			j.set("pid",x.r->pid).set("name",x.r->name).set("start_time",std::to_string(x.r->start));
			j.set("private_bytes",mcp::bytes(x.r->priv)).set("working_set_bytes",mcp::bytes(x.r->ws));
			bool gk = x.g->n >= 2;
			j.set("private_growth_bytes",gk ? Json(std::round(x.g->priv)) : Json());
			j.set("working_set_growth_bytes",gk ? Json(std::round(x.g->ws)) : Json());
			j.set("sample_count",x.g->n).set("covered_seconds",x.g->secs);
			rows.push_back(j);
		}
		Json j = page(rows,a,10,1000,"processes");
		j.set("rank_by",by).set("window_seconds",win).set("interval_ms",sampler::intervalMs());
		j.set("snapshot_error",errJson());
		return j;
	}

	Json stats(const std::vector<float>& v,size_t from) {
		Json j = Json::obj();
		if (from >= v.size()) {
			j.set("last",Json()).set("min",Json()).set("max",Json()).set("avg",Json());
			return j;
		}
		double mn = v[from],mx = v[from],sum = 0.0;
		for (size_t i = from; i < v.size(); i++) {
			mn = std::min(mn,(double)v[i]);
			mx = std::max(mx,(double)v[i]);
			sum += v[i];
		}
		const double M = 1048576.0;
		j.set("last",std::round(v.back() * M)).set("min",std::round(mn * M)).set("max",std::round(mx * M));
		j.set("avg",std::round(sum / (double)(v.size() - from) * M));
		return j;
	}

	Json processHistory(const Json& a) {
		ProcRef r = target(a);
		auto h = history(r);
		if (!h) throw std::runtime_error("no history for pid " + std::to_string(r.pid) + ", it just exited or FRS has not sampled it yet");
		size_t n = h->unixSec.size();
		size_t from = 0;
		uint64_t win = (uint64_t)std::max(1.0,std::min(86400.0,numArg(a,"window_seconds",86400.0)));
		if (a.get("window_seconds") && n) {
			uint64_t t0 = h->unixSec.back();
			while (from + 1 < n && t0 - h->unixSec[from] > win) from++;
		}
		std::vector<Json> rows;
		if (flagArg(a,"include_samples",true)) {
			rows.reserve(n - from);
			for (size_t i = from; i < n; i++) {
				Json s = Json::obj();
				s.set("unix_sec",h->unixSec[i]);
				s.set("private_bytes",std::round(h->privMb[i] * 1048576.0));
				s.set("working_set_bytes",std::round(h->wsMb[i] * 1048576.0));
				rows.push_back(s);
			}
		}
		Json j = page(rows,a,300,300,"samples");
		j.set("pid",r.pid).set("name",r.name).set("start_time",std::to_string(r.start));
		j.set("sample_count",n - from);
		j.set("covered_seconds",n > from ? h->unixSec.back() - h->unixSec[from] : 0);
		j.set("interval_ms",sampler::intervalMs());
		j.set("private_bytes",stats(h->privMb,from)).set("working_set_bytes",stats(h->wsMb,from));
		return j;
	}

}

namespace procs {

	void regTools() {
		mcp::add("Processes","list_processes",
#ifdef _WIN32
			"Every process with all memory columns, read without a handle so System, Memory Compression and protected ones show up. "
#else
			"Every process with all memory columns from /proc. "
#endif
			"Working set includes shared pages, so sums double count. Private bytes is commit, a trim does not lower it. Unknown is null, deltas are per tick.",
			R"j({"type":"object","properties":{
				"filter":{"type":"string","description":"case-insensitive text in name, pid, user or path"},
				"sort_by":{"type":"string","description":"column key, default working_set_bytes. With group : name or a summed key"},
				"descending":{"type":"boolean","description":"default true, false for name, pid, user, path"},
				"group":{"type":"boolean","description":"one row per process name with summed columns and pids"},
				"ws_counters":{"type":"boolean","description":"also shared and shareable working set from a background page walk : null until it is done, call again after a few seconds. ws_denied, handle_filtered, pseudo and system rows may stay null. The result's ws_counters says on, pending or off"},
				"limit":{"type":"integer","description":"rows to return, default 50, max 1000"},
				"offset":{"type":"integer","description":"rows to skip"}}})j",
			mcp::Read,listProcesses);

		mcp::add("Processes","rank_processes",
			"Top processes by current size or by growth over a window, from FRS's own rings (5 minutes at most at 1 s sampling). "
			"Growth is newest minus oldest. A process younger than the window has a low sample_count, and after a clean every row goes negative, neither says anything about a leak.",
			R"j({"type":"object","properties":{
				"rank_by":{"type":"string","enum":["private_growth","working_set_growth","private_bytes","working_set_bytes"],"description":"default private_growth"},
				"window_seconds":{"type":"integer","description":"growth window, default 60"},
				"filter":{"type":"string","description":"case-insensitive text in name, pid, user or path"},
				"limit":{"type":"integer","description":"rows to return, default 10, max 1000"},
				"offset":{"type":"integer","description":"rows to skip"}}})j",
			mcp::Read,rankProcesses);

		mcp::add("Processes","process_history",
			"Private bytes and working set rings of one process, oldest first, with last, min, max, avg in bytes. "
			"Rings start when FRS first sees the process and go away when it exits.",
			R"j({"type":"object","required":["pid"],"properties":{
				"pid":{"type":"integer","description":"process id"},
				"start_time":{"type":"string","description":"from list_processes, refuses a reused pid"},
				"window_seconds":{"type":"integer","description":"only the newest seconds, default all"},
				"include_samples":{"type":"boolean","description":"false returns the stats only, default true"},
				"limit":{"type":"integer","description":"samples to return, default and max 300"},
				"offset":{"type":"integer","description":"samples to skip"}}})j",
			mcp::Read,processHistory);
	}

}
