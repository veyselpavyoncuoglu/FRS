#pragma once
#include "procs.h"

// Working set watch : which instructions fault pages into one process. On Windows turning it on can't
// be undone, it stays until the target exits. Every read drains the kernel's list, so all readers go
// through here and add to one running total per target, the UI and MCP never split the data.
namespace wswatch {

	struct Row {
		uint64_t pc = 0,count = 0;
		std::string instr,file; // module+0xoffset or the raw address. file stays "" outside a module
	};

	struct View {
		bool probed = false; // a query came back
		bool on = false;
		std::string why; // the last query failed, or on Linux some threads are not watched. "" when fine
		uint64_t total = 0,lost = 0; // faults counted since FRS first read this target, lost is what the OS dropped or the row cap left out
		std::vector<Row> rows; // most faults first
	};

	void init(); // sampler job, it only drains what a tab is showing
	void want(const ProcRef& r); // the tab is on screen, keep reading
	std::shared_ptr<const View> view(const ProcRef& r); // last read, null before the first
	std::shared_ptr<const View> read(const ProcRef& r); // drains now, not on the UI thread
	std::shared_ptr<const View> kept(ProcRef& r); // an exited target's last read, kept a minute after the last look. Fills r.name, null when none
	std::string enable(const ProcRef& r,const char* pfx); // logs it, returns "" when on. Not on the UI thread
	std::string stop(const ProcRef& r,const char* pfx); // same, Linux only
	bool permanent(); // enable can't be undone on this OS
	void openLoc(const std::string& file); // shows the file in the file manager, not on the UI thread

	namespace be {

		int drain(const ProcRef& r,std::vector<uint64_t>& pcs,uint64_t& lost,std::string& why); // 1 on, 0 not on, -1 failed
		std::string enable(const ProcRef& r);
		std::string stop(const ProcRef& r);
		void lookup(const ProcRef& r,std::vector<Row>& rows); // fills instr and file from pc
		bool permanent();
		void openLoc(const std::string& file);

	}

}
