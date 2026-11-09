#ifndef _WIN32
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "surf.h"
#include "domains.h"
#include "cfg.h"
#include "mcp.h"

// No tray on Linux, close always exits. The rest of surf's OS seam stays so shared code looks the same on both.

namespace surf {

	void setHwnd(void*) {}
	bool winMsg(void*,unsigned,unsigned long long,long long,long long&) { return false; }
	void notify(const std::string&,const std::string&,bool) {}

	namespace os {

		void opts() {
			cfg::optBool("uiOnTop","Window","Always on top","Keeps the FRS window above other windows on X11, where the window manager allows it. Wayland ignores it, GLFW can't float a Wayland window.",false);
			cfg::optBool("startMin","Window","Start minimized","From the next start.",false);
		}

		// the Ask prompt only draws in a frame, minimized has none and the call would wait out its 60 s unseen
		void update() {
			GLFWwindow* w = glfwGetCurrentContext();
			if (w && glfwGetWindowAttrib(w,GLFW_ICONIFIED) && mcp::askPending()) glfwRestoreWindow(w);
		}

		void shutdown() {}

		void setOnTop(bool on) {
			static bool was = false;
			if (on == was) return;
			was = on;
			GLFWwindow* w = glfwGetCurrentContext();
#if GLFW_VERSION_MAJOR > 3 || GLFW_VERSION_MINOR >= 3
			if (w) glfwSetWindowAttrib(w,GLFW_FLOATING,on ? GLFW_TRUE : GLFW_FALSE);
#else
			(void)w;
#endif
		}

		int screenW() {
			GLFWmonitor* mon = glfwGetPrimaryMonitor();
			if (!mon) return 0;
			const GLFWvidmode* vm = glfwGetVideoMode(mon);
			return vm ? vm->width : 0;
		}

	}

}
#endif // !_WIN32
