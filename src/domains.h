#pragma once
#include "sysmem.h"
#include "procs.h"

// Each feature area owns its own files and shows up here only through init and regTools.
// init : pages, process tabs, sampler jobs, options, palette commands. UI thread, before settings load.
// regTools : MCP tools only. Must not grow after the server started.
namespace procdet { void init(); void regTools(); }
namespace vmem { void init(); void regTools(); }
namespace vmscan { void init(); void regTools(); }
namespace vmdiag { void init(); void regTools(); }
namespace kmem { void init(); void regTools(); }
namespace extra { void init(); void regTools(); }

namespace surf {
	void init();
	void regTools();
	void update(); // every frame, also while minimized
	void drawStatusBar(); // bottom of the shell, one text row in a thin panel
	void drawHeaderGraphs(); // small graphs in the shell header : runs inside a child one frame row tall, take the width from GetContentRegionAvail
	void shutdown();
	// Windows message hook, called first by main.cpp. Return true when handled and set res. hwnd is void* so
	// no OS headers leak into shared code. Linux never calls it.
	bool winMsg(void* hwnd,unsigned msg,unsigned long long wp,long long lp,long long& res);
	void setHwnd(void* hwnd);
	void notify(const std::string& title,const std::string& text,bool warn); // tray balloon after a clean, any thread. Nothing on Linux
}

namespace domains {
	void initAll();
	void regToolsAll();
}
