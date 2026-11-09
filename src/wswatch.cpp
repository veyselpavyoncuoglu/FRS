#include "wswatch.h"
#include <algorithm>
#include <map>
#include <unordered_map>

namespace {

	using namespace wswatch;
	using Key = std::pair<uint32_t,uint64_t>;

	const size_t MAXROWS = 100000;

	struct W {
		ProcRef r;
		std::mutex m; // one reader at a time, what a drain returned must reach the totals
		std::unordered_map<uint64_t,Row> rows;
		bool probed = false,on = false;
		std::string why;
		uint64_t total = 0,lost = 0;
		std::atomic<uint64_t> wantMs{0},useMs{0}; // useMs : the tab or MCP looked at it, keeps it a minute after an exit
		Snap<View> pub;
	};

	std::mutex g_m;
	std::map<Key,std::shared_ptr<W>> g_w;

	std::shared_ptr<W> get(const ProcRef& r) {
		std::lock_guard<std::mutex> lk(g_m);
		auto& w = g_w[Key(r.pid,r.start)];
		if (!w) {
			w = std::make_shared<W>();
			w->r = r;
		}
		w->useMs = util::nowMs();
		return w;
	}

	std::shared_ptr<const View> readW(W& w) {
		std::lock_guard<std::mutex> lk(w.m);
		std::vector<uint64_t> pcs;
		uint64_t lost = 0;
		std::string why;
		int st = be::drain(w.r,pcs,lost,why);
		w.why = why;
		if (st >= 0) {
			w.probed = true;
			w.on = st > 0;
		}
		w.total += pcs.size();
		w.lost += lost;
		std::vector<Row> fresh;
		for (uint64_t pc : pcs) {
			auto it = w.rows.find(pc);
			if (it != w.rows.end()) it->second.count++;
			else if (w.rows.size() < MAXROWS) {
				Row x;
				x.pc = pc;
				x.count = 1;
				w.rows[pc] = x;
				fresh.push_back(x);
			} else {
				w.total--;
				w.lost++;
			}
		}
		if (!fresh.empty()) {
			be::lookup(w.r,fresh);
			for (auto& x : fresh) {
				auto& d = w.rows[x.pc];
				d.instr = x.instr;
				d.file = x.file;
			}
		}
		auto v = std::make_shared<View>();
		v->probed = w.probed;
		v->on = w.on;
		v->why = w.why;
		v->total = w.total;
		v->lost = w.lost;
		v->rows.reserve(w.rows.size());
		for (auto& kv : w.rows) v->rows.push_back(kv.second);
		std::sort(v->rows.begin(),v->rows.end(),[](const Row& a,const Row& b) { return a.count != b.count ? a.count > b.count : a.pc < b.pc; });
		std::shared_ptr<const View> cv = v;
		w.pub.put(cv);
		return cv;
	}

	void tick() {
		std::vector<std::shared_ptr<W>> l;
		{
			std::lock_guard<std::mutex> lk(g_m);
			for (auto& kv : g_w) l.push_back(kv.second);
		}
		uint64_t now = util::nowMs();
		for (auto& w : l) {
			uint64_t age = now - std::min(now,w->wantMs.load());
			uint64_t idle = now - std::min(now,w->useMs.load());
			bool up = procs::alive(w->r);
			if (up && age < 3000) readW(*w);
			else if (!up && idle > 60000) {
				be::stop(w->r);
				std::lock_guard<std::mutex> lk(g_m);
				g_w.erase(Key(w->r.pid,w->r.start));
			}
		}
	}

	std::string logLine(const char* pfx,const char* label,const ProcRef& r,const std::string& why) {
		appLog(std::string(pfx) + label + " (pid " + std::to_string(r.pid) + ")" + (why.empty() ? " : OK" : " : failed (" + why + ")"));
		return why;
	}

}

namespace wswatch {

	void init() {
		sampler::add("wswatch",1000,tick);
	}

	void want(const ProcRef& r) {
		get(r)->wantMs = util::nowMs();
	}

	std::shared_ptr<const View> view(const ProcRef& r) {
		return get(r)->pub.get();
	}

	std::shared_ptr<const View> read(const ProcRef& r) {
		return readW(*get(r));
	}

	std::shared_ptr<const View> kept(ProcRef& r) {
		std::shared_ptr<W> w;
		{
			std::lock_guard<std::mutex> lk(g_m);
			auto it = g_w.find(Key(r.pid,r.start));
			if (it == g_w.end()) return nullptr;
			w = it->second;
		}
		w->useMs = util::nowMs();
		r.name = w->r.name;
		return w->pub.get();
	}

	std::string enable(const ProcRef& r,const char* pfx) {
		std::string why = be::enable(r);
		if (why.empty()) readW(*get(r));
		return logLine(pfx,"WS watch on",r,why);
	}

	std::string stop(const ProcRef& r,const char* pfx) {
		std::string why = be::stop(r);
		if (why.empty()) readW(*get(r));
		return logLine(pfx,"WS watch off",r,why);
	}

	bool permanent() {
		return be::permanent();
	}

	void openLoc(const std::string& file) {
		be::openLoc(file);
	}

}
