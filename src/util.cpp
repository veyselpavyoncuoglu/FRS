#include "util.h"
#include "cfg.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <thread>

namespace {

	const char* UNITS[] = {"B","KB","MB","GB","TB","PB","EB"};

	bool localTm(time_t t,tm& out) {
#ifdef _WIN32
		return localtime_s(&out,&t) == 0;
#else
		return localtime_r(&t,&out) != nullptr;
#endif
	}

	// 3 significant digits, rounding can't print 4 ("1024 MB" becomes "1.00 GB")
	std::string fmtSize(uint64_t bytes) {
		int u = 0;
		double v = (double)bytes;
		while (v >= 1024.0 && u < 6) {
			v /= 1024.0;
			u++;
		}
		if (u > 0 && v >= 1023.5 && u < 6) {
			v /= 1024.0;
			u++;
		}
		char b[32];
		if (u == 0) snprintf(b,sizeof(b),"%llu B",(unsigned long long)bytes);
		else if (v < 9.995) snprintf(b,sizeof(b),"%.2f %s",v,UNITS[u]);
		else if (v < 99.95) snprintf(b,sizeof(b),"%.1f %s",v,UNITS[u]);
		else snprintf(b,sizeof(b),"%.0f %s",v,UNITS[u]);
		return b;
	}

	// how many bytes the UTF-8 char at s[i] takes and its code point, 0 when the bytes are junk
	int decode(const std::string& s,size_t i,uint32_t& cp) {
		unsigned char c = (unsigned char)s[i];
		int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
		if (!n || i + n > s.size()) return 0;
		cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
		for (int k = 1; k < n; k++) {
			unsigned char d = (unsigned char)s[i + k];
			if ((d & 0xC0) != 0x80) return 0;
			cp = (cp << 6) | (d & 0x3F);
		}
		if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF) return 0;
		return n;
	}

	bool dropCp(uint32_t cp) {
		if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return true;
		if (cp == 0x061C || cp == 0x200E || cp == 0x200F || cp == 0xFEFF) return true;
		if (cp >= 0x200B && cp <= 0x200D) return true;
		if (cp >= 0x202A && cp <= 0x202E) return true;
		return cp >= 0x2066 && cp <= 0x2069;
	}

}

namespace util {

	uint64_t nowMs() {
		using namespace std::chrono;
		static const steady_clock::time_point t0 = steady_clock::now();
		return (uint64_t)duration_cast<milliseconds>(steady_clock::now() - t0).count();
	}

	std::string ts() {
		tm t;
		if (!localTm(time(nullptr),t)) return "[--:--:--]";
		char b[16];
		strftime(b,sizeof(b),"[%H:%M:%S]",&t);
		return b;
	}

	std::string size(uint64_t bytes) {
		return bytes == NA ? "N/A" : fmtSize(bytes);
	}

	std::string delta(int64_t bytes) {
		if (bytes == 0) return "";
		uint64_t a = bytes < 0 ? 0 - (uint64_t)bytes : (uint64_t)bytes;
		int u = 0;
		double v = (double)a;
		while (v >= 1024.0 && u < 6) {
			v /= 1024.0;
			u++;
		}
		char b[32];
		const char* sg = bytes < 0 ? "-" : "+";
		if (u > 0 && v < 9.95 && std::fabs(v - std::round(v)) >= 0.05) snprintf(b,sizeof(b),"%s%.1f %s",sg,v,UNITS[u]);
		else snprintf(b,sizeof(b),"%s%.0f %s",sg,v,UNITS[u]);
		return b;
	}

	std::string count(uint64_t n) {
		if (n == NA) return "N/A";
		std::string s = std::to_string(n);
		for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i,",");
		return s;
	}

	std::string hex(uint64_t v) {
		char b[32];
		snprintf(b,sizeof(b),"0x%llx",(unsigned long long)v);
		return b;
	}

	bool parseHex(const std::string& s,uint64_t& out) {
		size_t i = s.rfind("0x",0) == 0 || s.rfind("0X",0) == 0 ? 2 : 0;
		if (i >= s.size() || s.size() - i > 16) return false;
		uint64_t v = 0;
		for (; i < s.size(); i++) {
			char c = s[i];
			int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			if (d < 0) return false;
			v = (v << 4) | (uint64_t)d;
		}
		out = v;
		return true;
	}

	std::string lower(std::string s) {
		for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		return s;
	}

	bool has(const std::string& hay,const std::string& needle) {
		return lower(hay).find(lower(needle)) != std::string::npos;
	}

	std::string safe(const std::string& s) {
		std::string out;
		out.reserve(s.size());
		for (size_t i = 0; i < s.size();) {
			uint32_t cp = 0;
			int n = decode(s,i,cp);
			if (!n) {
				out += '?';
				i++;
				continue;
			}
			if (!dropCp(cp)) out.append(s,i,n);
			i += n;
		}
		return out;
	}

	double mb(uint64_t bytes) {
		return std::round(bytes / 104857.6) / 10.0;
	}

	float toMbf(uint64_t v) {
		return v == NA ? -1.0f : (float)((double)v / 1048576.0);
	}

	std::string pct(uint64_t num,uint64_t den) {
		if (num == NA || den == NA || !den) return "N/A";
		char b[32];
		snprintf(b,sizeof(b),"%.*f%%",std::max(0,std::min(3,cfg::geti("uiPctPrec",1))),(double)num * 100.0 / (double)den);
		return b;
	}

	std::string timeStr(uint64_t unixSec) {
		tm t;
		if (unixSec == NA || !localTm((time_t)unixSec,t)) return "N/A";
		char b[32];
		strftime(b,sizeof(b),"%Y-%m-%d %H:%M:%S",&t);
		return b;
	}

}

namespace sampler {

	namespace {

		struct Job {
			std::string name;
			int ms;
			std::function<void()> fn;
			std::thread th;
			bool failLogged = false;
		};

		std::mutex g_m;
		std::condition_variable g_cv;
		std::vector<std::unique_ptr<Job>> g_jobs;
		std::atomic<bool> g_stop{false};
		uint64_t g_kick = 0; // under g_m, each job compares it with the value it last saw
		bool g_started = false;

		void fail(Job& j,const char* why) {
			try {
				if (!j.failLogged) appLog("[!] sampler " + j.name + " : failed (" + util::safe(why) + ")");
			} catch (...) {}
			j.failLogged = true;
		}

		void tick(Job& j) {
			try {
				j.fn();
			} catch (std::exception& e) {
				fail(j,e.what());
			} catch (...) {
				fail(j,"unknown error");
			}
		}

		void loop(Job* j) try {
			uint64_t seen = 0;
			{
				std::lock_guard<std::mutex> lk(g_m);
				seen = g_kick;
			}
			tick(*j);
			while (!g_stop) {
				bool kicked = false;
				{
					std::unique_lock<std::mutex> lk(g_m);
					int ms = j->ms > 0 ? j->ms : intervalMs();
					g_cv.wait_for(lk,std::chrono::milliseconds(ms),[&] { return g_stop.load() || (j->ms <= 0 && g_kick != seen); });
					if (g_stop) break;
					kicked = j->ms <= 0 && g_kick != seen;
					if (kicked) seen = g_kick;
				}
				if (j->ms <= 0 && !kicked && cfg::getb("memPaused",false)) continue;
				tick(*j);
			}
		} catch (...) {}

		void launch(Job& j) {
			try {
				j.th = std::thread(loop,&j);
			} catch (std::exception& e) {
				fail(j,e.what());
			}
		}

	}

	void add(const char* name,int ms,std::function<void()> fn) {
		std::unique_ptr<Job> j(new Job());
		j->name = name;
		j->ms = ms;
		j->fn = std::move(fn);
		std::lock_guard<std::mutex> lk(g_m);
		if (g_started && !g_stop) launch(*j);
		g_jobs.push_back(std::move(j));
	}

	void start() {
		std::lock_guard<std::mutex> lk(g_m);
		if (g_started) return;
		g_started = true;
		for (auto& j : g_jobs) launch(*j);
	}

	void stop() {
		{
			std::lock_guard<std::mutex> lk(g_m);
			g_stop = true;
		}
		g_cv.notify_all();
		std::vector<std::unique_ptr<Job>> jobs;
		{
			std::lock_guard<std::mutex> lk(g_m);
			jobs.swap(g_jobs);
		}
		for (auto& j : jobs) if (j->th.joinable()) j->th.join();
	}

	bool stopping() {
		return g_stop;
	}

	int intervalMs() {
		return std::max(100,std::min(10000,cfg::geti("memRefreshMs",1000)));
	}

	void kick() {
		{
			std::lock_guard<std::mutex> lk(g_m);
			g_kick++;
		}
		g_cv.notify_all();
	}

}

namespace task {

	namespace {

		struct Item {
			std::shared_ptr<T> t;
			std::thread th;
		};

		std::mutex g_m;
		std::vector<Item> g_items;
		bool g_closed = false;

		// g_m held. join only what said it is done, so this never waits on real work.
		void reap() {
			for (size_t i = 0; i < g_items.size();) {
				if (g_items[i].t->done) {
					if (g_items[i].th.joinable()) g_items[i].th.join();
					g_items.erase(g_items.begin() + i);
				} else i++;
			}
		}

		void body(const std::shared_ptr<T>& t,const std::function<std::string(T&)>& fn,bool logRes) {
			try {
				std::string r;
				try {
					r = fn(*t);
				} catch (std::exception& e) {
					r = "Task : failed (" + util::safe(e.what()) + ")";
				} catch (...) {
					r = "Task : failed (unknown error)";
				}
				t->res = r;
				if (logRes && !r.empty()) appLog(r);
			} catch (...) {}
			t->done = true;
		}

	}

	std::shared_ptr<T> run(std::function<std::string(T&)> fn,bool logRes) {
		auto t = std::make_shared<T>();
		std::lock_guard<std::mutex> lk(g_m);
		reap();
		if (g_closed) {
			t->res = "Task : failed (FRS is closing)";
			t->done = true;
			return t;
		}
		try {
			g_items.reserve(g_items.size() + 1); // once the thread runs nothing below may throw
			Item it;
			it.t = t;
			it.th = std::thread([t,fn,logRes] { body(t,fn,logRes); });
			g_items.push_back(std::move(it));
		} catch (std::exception& e) {
			try {
				t->res = "Task : failed (" + util::safe(e.what()) + ")";
				if (logRes) appLog(t->res);
			} catch (...) {}
			t->done = true;
		}
		return t;
	}

	void stopAll() {
		std::vector<Item> items;
		{
			std::lock_guard<std::mutex> lk(g_m);
			g_closed = true;
			items.swap(g_items);
		}
		for (auto& i : items) i.t->cancel = true;
		uint64_t end = util::nowMs() + 3000;
		for (auto& i : items) {
			while (!i.t->done && util::nowMs() < end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
			if (!i.th.joinable()) continue;
			if (i.t->done) i.th.join();
			else i.th.detach();
		}
	}

}
