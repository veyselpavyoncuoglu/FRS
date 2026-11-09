#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <mutex>
#include <memory>
#include <vector>
#include <atomic>
#include <functional>

// counter the OS would not give us : UI shows N/A, MCP sends null, never 0
constexpr uint64_t NA = UINT64_MAX;

namespace util {

	uint64_t nowMs(); // monotonic, since process start
	std::string ts(); // "[HH:MM:SS]"
	std::string size(uint64_t bytes); // "1.23 GB" (1024 based), NA -> "N/A"
	std::string delta(int64_t bytes); // "+1.2 MB", "-4 KB", "" for 0
	std::string count(uint64_t n); // "1,234,567", NA -> "N/A"
	std::string hex(uint64_t v); // "0x7ff6a0000000"
	bool parseHex(const std::string& s,uint64_t& out); // takes 0x12ab or 12ab, false on junk
	std::string lower(std::string s);
	bool has(const std::string& hay,const std::string& needle); // case-insensitive contains
	std::string safe(const std::string& s); // drops control and bidi chars, use before showing OS strings
	double mb(uint64_t bytes); // MB with 1 decimal
	float toMbf(uint64_t v); // MB for graph rings, NA -> -1 (a gap in ui::graph)
	std::string pct(uint64_t num,uint64_t den); // "12.3%" with the Percent decimals setting, N/A when unknown or den is 0
	std::string timeStr(uint64_t unixSec); // "2026-11-05 14:03:22" local

	// test switches. Inline so a build without -DFRS_TEST (Makefile MANIFEST=1, the default) drops the names too
#ifdef FRS_TEST
	inline const char* testEnv(const char* name) { return getenv(name); }
#else
	inline const char* testEnv(const char*) { return nullptr; }
#endif

}

// log line for the main log, any thread. Adds the timestamp.
// Color comes from the text : "Label : OK", "Label : failed (why)" so keep that shape.
// From MCP handlers prefix it with "[mcp] ".
void appLog(const std::string& line);
bool appCleaning(); // a clean is running right now, any thread
bool appRunClean(bool fs,bool ew,bool ps,bool fm,const char* pfx); // same run path as the Dashboard buttons, UI thread only, false when one already runs
bool appRunSet(const char* pfx); // the clean set : Auto-Clean methods plus the Clean set areas. UI thread only, false when a clean runs or the set is empty

// cleans as the tray, status bar and notifications show them. A copy App::update refreshes every frame
struct CleanInfo {
	uint64_t lastUnix = 0; // end of the last clean, 0 before the first
	std::string lastSrc; // dashboard, auto, timer, hotkey, tray, mcp
	double freedMb = 0; // used before minus used after, can be negative
	int failed = 0; // failed steps of the last clean
	int64_t nextTimerSec = -1; // -1 while Clean every is off
	int set = 0; // clean set : bits 0..3 the methods flush_standby, empty_working_sets, purge_standby, flush_modified, bits 4..8 acSysCache acModCache acStandby0 acRegistry acCombine
	bool autoOn = false; // threshold auto-clean
	int thr = 20;
	bool every = false; // Clean every N minutes
	int everyMin = 30;
};
CleanInfo appCleanInfo(); // any thread
void appSetMethod(int i,bool on); // tick one clean set entry, i = bit of CleanInfo::set (0..3 methods, 4..8 areas). UI thread only, saves. Other threads mcp::post it
void appSetAuto(int thr); // threshold auto-clean, 0 turns it off, UI thread only, saves

// latest published value of something a background thread computes. Readers keep the
// shared_ptr as long as they want, the value never changes under them.
template<class T> struct Snap {
	std::shared_ptr<const T> get() const {
		std::lock_guard<std::mutex> lk(m);
		return p;
	}
	void put(std::shared_ptr<const T> v) {
		std::lock_guard<std::mutex> lk(m);
		p = std::move(v);
	}
private:
	mutable std::mutex m;
	std::shared_ptr<const T> p;
};

// background jobs. Each gets its own thread so a slow one can't stall the others.
namespace sampler {

	// add before start(). ms = 0 follows the user's refresh interval (cfg key memRefreshMs).
	void add(const char* name,int ms,std::function<void()> fn);
	void start();
	void stop(); // joins everything, called on exit
	bool stopping(); // long loops inside a job check this
	int intervalMs();
	void kick(); // every ms = 0 job takes one sample now, also while memPaused is on

}

// one-shot background work so an OS call never blocks the UI thread. Hold the shared_ptr to
// watch it, or drop it and let the result string go to the log (logRes).
namespace task {

	struct T {
		std::atomic<bool> done{false};
		std::atomic<float> prog{0.0f};
		std::atomic<bool> cancel{false};
		std::string res; // read only after done
	};
	std::shared_ptr<T> run(std::function<std::string(T&)> fn,bool logRes = true);
	void stopAll(); // cancels and waits up to 3 s, called on exit

}
