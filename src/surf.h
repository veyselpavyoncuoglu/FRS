#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Inside the surf area only : what surf.cpp and the tray files share. The public part is in domains.h.
namespace surf {

	enum MiniSec { SecGraphs,SecCommit,SecPhys,SecGpu };

	uint32_t physCol(); // current graph colours as 0xAARRGGBB, any thread
	uint32_t commitCol();
	uint32_t warnCol(); // theme warning and critical colours, same format
	uint32_t badCol();
	std::string sizeStr(uint64_t bytes); // util::size under the Max size unit option, any thread
	std::string pctStr(uint64_t part,uint64_t total); // "62.5%", N/A when unknown, any thread
	void openMini(int sec); // UI thread

	// newest sample on the right, 2 px per sample, into w * h pixels 0xAARRGGBB top down. Line in col, fill
	// at half its brightness, like SI's tray graphs. Negative samples are unknown and leave a gap.
	void rasterGraph(uint32_t* px,int w,int h,const std::vector<float>& v,float maxv,uint32_t col,bool clear);

	// tray_win.cpp and tray_linux.cpp
	namespace os {

		void opts(); // declares the tray and taskbar options, from init
		void update(); // UI thread, every frame, also while minimized or hidden
		void shutdown();
		void setOnTop(bool on); // often, acts only on a change
		int screenW(); // virtual screen width in pixels, 0 when unknown

	}

}
