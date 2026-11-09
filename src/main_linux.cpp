#ifndef _WIN32
// Linux backend: GLFW + OpenGL3, mirroring the Win32+DX11 backend in main.cpp.
// All UI and logic live in the shared App core (app.cpp); this file only owns the
// window, the GL context, and the frame loop.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <unistd.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include "app.h"
#include "cfg.h"
#include "platform.h"
#include "ui.h"
#include "util.h"

static App* g_app = nullptr;

static void renderFrame(GLFWwindow* win);

static void glfwErrorCb(int err,const char* desc) {
	fprintf(stderr,"[GLFW] error %d: %s\n",err,desc);
}

// no window : the pkexec parent waiting on 125 runs FRS without root instead
static int displayHint() {
	if (geteuid() == 0 && getenv("SUDO_UID")) {
		fprintf(stderr,"[!] sudo hides your desktop session from FRS. Run ./FRS without sudo,\n"
			"    it asks for root itself through pkexec (or use: sudo -E ./FRS)\n");
	}
	return getenv("FRS_ELEVATED") && getenv("PKEXEC_UID") ? 125 : 1;
}

// Keep rendering while the window is being resized/moved (analogous to the Win32
// WM_TIMER trick): GLFW fires this callback during the modal resize loop.
static void windowRefreshCb(GLFWwindow* win) {
	if (g_app) renderFrame(win);
}

// winW/winH keep the normal window's size : App::update saves the size of the frame before, so that one and this one have
// to be normal. Maximized is kept for the next start, a minimized window leaves it as it was
static void winState(GLFWwindow* win) {
	static bool was = false;
	bool ic = glfwGetWindowAttrib(win,GLFW_ICONIFIED) != 0;
	bool mx = glfwGetWindowAttrib(win,GLFW_MAXIMIZED) != 0;
	App::sizeOk = was && !ic && !mx;
	was = !ic && !mx;
	if (!ic) cfg::seti("winMax",mx ? 1 : 0);
}

static void renderFrame(GLFWwindow* win) {
	winState(win);
	g_app->update();

	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	g_app->drawUI();

	ImGui::Render();
	int w,h;
	glfwGetFramebufferSize(win,&w,&h);
	glViewport(0,0,w,h);
	glClearColor(ui::c.bg.x,ui::c.bg.y,ui::c.bg.z,1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	glfwSwapBuffers(win);
}

static int iniInt(const std::string& text,const char* key) {
	std::string k = std::string("\n") + key + "=";
	size_t p = ("\n" + text).find(k);
	return p == std::string::npos ? 0 : atoi(text.c_str() + p + k.size() - 1);
}

static bool iniPos(const std::string& text,int& x,int& y) {
	std::string k = "\nwinPos=";
	size_t p = ("\n" + text).find(k);
	return p != std::string::npos && sscanf(text.c_str() + p + k.size() - 1,"%d,%d",&x,&y) == 2;
}

// Wayland clients can't place their window. A 3.3 library is X11 or Wayland, the X11 one on XWayland places fine
static bool canPlace() {
#if GLFW_VERSION_MAJOR > 3 || GLFW_VERSION_MINOR >= 4
	return glfwGetPlatform() != GLFW_PLATFORM_WAYLAND;
#else
	return !strstr(glfwGetVersionString(),"Wayland");
#endif
}

// any case, - or / like App::cli
static bool hasSw(const std::vector<std::string>& args,const char* sw) {
	for (auto a : args) {
		a = util::lower(a);
		if (a.size() > 1 && a[0] == '/') a[0] = '-';
		if (a == sw) return true;
	}
	return false;
}

// the monitor whose work area holds that point, and that work area
static GLFWmonitor* onScreen(int x,int y,int& ax,int& ay,int& aw,int& ah) {
#if GLFW_VERSION_MAJOR > 3 || GLFW_VERSION_MINOR >= 3
	int n = 0;
	GLFWmonitor** ms = glfwGetMonitors(&n);
	for (int i = 0; i < n; i++) {
		aw = ah = 0;
		glfwGetMonitorWorkarea(ms[i],&ax,&ay,&aw,&ah);
		if (x >= ax && y >= ay && x < ax + aw && y < ay + ah) return ms[i];
	}
#endif
	return nullptr;
}

#if GLFW_VERSION_MAJOR > 3 || GLFW_VERSION_MINOR >= 3
// Wayland gives the work area in pixels while window sizes are in scaled units, and ImGui's scale there is 1.0. X11 has both in pixels
static void workArea(GLFWmonitor* mon,float dpi,int& aw,int& ah) {
	int ax,ay;
	float sx = 1.0f,sy = 1.0f;
	aw = ah = 0;
	glfwGetMonitorWorkarea(mon,&ax,&ay,&aw,&ah);
	glfwGetMonitorContentScale(mon,&sx,&sy);
	if (sx > dpi && sy > 0.0f) {
		aw = (int)((float)aw / sx);
		ah = (int)((float)ah / sy);
	}
}
#endif

// -clean has no window for dirNote (a root copy from an older FRS brought over, portable mode failed), frs.log gets it
// with the [cli] lines. stderr already had the root copy one
static void cliNote() {
	std::string n = plat::dirNote(),f;
	if (n.empty() || !cfg::getb("logFile",false)) return;
	std::string d = util::timeStr((uint64_t)time(nullptr));
	d = d.size() > 11 ? d.substr(0,11) : "";
	size_t p = 0;
	while (p < n.size()) {
		size_t e = std::min(n.find('\n',p),n.size());
		f += d + util::ts() + " [cli] " + n.substr(p,e - p) + "\n";
		p = e + 1;
	}
	plat::appendFile("frs.log",f,1 << 20);
}

static void posCb(GLFWwindow* w,int x,int y) {
	if (glfwGetWindowAttrib(w,GLFW_ICONIFIED) || glfwGetWindowAttrib(w,GLFW_MAXIMIZED)) return;
	cfg::sets("winPos",std::to_string(x) + "," + std::to_string(y));
}

int main(int argc,char** argv) {
	std::vector<std::string> args;
	for (int i = 1; i < argc; i++) args.push_back(argv[i]);
	// -help needs no root, -clean goes through pkexec first like the window does
	std::string out;
	int code = App::cli(args,out,true);
	if (code < 0) {
		// a running FRS : no pkexec password for nothing. Only a look at the lock, the root copy takes it
		bool gui = !hasSw(args,"-clean") && !hasSw(args,"-clean:full");
		if (gui && geteuid() != 0 && !getenv("FRS_ELEVATED") && !iniInt(plat::readSettings(),"multiInst") && !plat::firstInstance(false)) {
			plat::showOther();
			return 0;
		}
		plat::raisePrivileges();
		code = App::cli(args,out);
		if (code >= 0) cliNote();
	}
	if (code >= 0) {
		fputs(out.c_str(),stdout);
		return code;
	}
	bool mini = hasSw(args,"-minimized");

	if (!iniInt(plat::readSettings(),"multiInst") && !plat::firstInstance()) {
		plat::showOther();
		return 0;
	}
	// read once the lock is ours : one that was closing saved its last size and spot on the way out
	std::string ini = plat::readSettings();

	glfwSetErrorCallback(glfwErrorCb);
	if (!glfwInit()) {
		fprintf(stderr,"[!] glfwInit failed\n");
		return displayHint();
	}

	// GL 3.0 + GLSL 130 — matches imgui_impl_opengl3's default and works on the
	// widest range of Linux GPU drivers.
	const char* glsl_version = "#version 130";
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,0);

	GLFWmonitor* mon = glfwGetPrimaryMonitor();
	float dpi = mon ? ImGui_ImplGlfw_GetContentScaleForMonitor(mon) : 1.0f;
	int ww = iniInt(ini,"winW");
	int wh = iniInt(ini,"winH");
	if (ww <= 0 || wh <= 0) {
		ww = (int)(1240.0f * dpi);
		wh = (int)(800.0f * dpi);
	}
	// back on the monitor the saved spot is on, inside that monitor's work area like Windows. None : the primary's
	bool place = canPlace();
	int px = 0,py = 0,ax = 0,ay = 0,aw = 0,ah = 0;
	GLFWmonitor* at = place && iniPos(ini,px,py) ? onScreen(px + ww / 2,py + wh / 2,ax,ay,aw,ah) : nullptr;
#if GLFW_VERSION_MAJOR > 3 || GLFW_VERSION_MINOR >= 3
	if (at || mon) {
		int sw,sh;
		workArea(at ? at : mon,dpi,sw,sh);
		if (sw > 0 && sh > 0) {
			ww = std::min(ww,sw);
			wh = std::min(wh,sh);
		}
	}
#endif

	// hidden until it is placed, X11 would show it at the window manager's spot first
	glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
	// closed maximized : opens maximized, ww/wh and the spot are the normal window's for the restore
	bool max = iniInt(ini,"winMax") != 0;
	if (max) glfwWindowHint(GLFW_MAXIMIZED,GLFW_TRUE);
	GLFWwindow* win = glfwCreateWindow(ww,wh,"FRS - Free RAM System",nullptr,nullptr);
	if (!win) {
		fprintf(stderr,"[!] glfwCreateWindow failed\n");
		code = displayHint();
		glfwTerminate();
		return code;
	}
	int minW = (int)(900.0f * dpi);
	int minH = (int)(600.0f * dpi);
#if GLFW_VERSION_MAJOR > 3 || GLFW_VERSION_MINOR >= 3
	if (mon) {
		int aw,ah,fl = 0,ft = 0,fr = 0,fb = 0;
		workArea(mon,dpi,aw,ah);
		glfwGetWindowFrameSize(win,&fl,&ft,&fr,&fb);
		if (aw > 0 && ah > 0) {
			minW = std::min(minW,aw - fl - fr);
			minH = std::min(minH,ah - ft - fb);
		}
	}
#endif
	glfwSetWindowSizeLimits(win,minW,minH,GLFW_DONT_CARE,GLFW_DONT_CARE);
	if (place) {
		// inside that monitor's work area like Windows : a size saved while maximized would hang past its edges
		if (at) {
			int fl = 0,ft = 0,fr = 0,fb = 0;
			glfwGetWindowFrameSize(win,&fl,&ft,&fr,&fb);
			px = std::max(ax + fl,std::min(px,ax + aw - ww - fr));
			py = std::max(ay + ft,std::min(py,ay + ah - wh - fb));
			glfwSetWindowPos(win,px,py);
		}
		glfwSetWindowPosCallback(win,posCb);
	}
	glfwMakeContextCurrent(win);
	// no vsync : on Wayland the swap blocks forever while the window is hidden/minimized,
	// which froze auto-clean and the MCP server. Frames are paced by hand in the loop below.
	glfwSwapInterval(0);
	glfwSetWindowRefreshCallback(win,windowRefreshCb);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;

	ui::applyTheme(dpi);
	ui::loadFonts(dpi);

	ImGui_ImplGlfw_InitForOpenGL(win,true);
	ImGui_ImplOpenGL3_Init(glsl_version);

	{
		App app;
		app.init();
		g_app = &app;
		// not every GLFW backend takes the hint, this one is a no-op where it did
		if (max) glfwMaximizeWindow(win);
		// shown when the first frame is about to be drawn. X11 can't map a window iconic through GLFW, minimized follows the show at once
		glfwShowWindow(win);
		if (mini || iniInt(ini,"startMin")) glfwIconifyWindow(win);

		auto next = std::chrono::steady_clock::now();
		while (!glfwWindowShouldClose(win)) {
			glfwPollEvents();
			if (glfwGetWindowAttrib(win,GLFW_ICONIFIED) != 0) {
				glfwWaitEventsTimeout(0.1); // don't spin while minimized
				winState(win);
				app.update(); // but keep auto-clean and MCP going
				continue;
			}
			renderFrame(win);
			next += std::chrono::microseconds(16667);
			auto now = std::chrono::steady_clock::now();
			if (next > now) std::this_thread::sleep_until(next);
			else next = now;
		}

		g_app = nullptr;
		glfwHideWindow(win);
		plat::leaving();
	}

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(win);
	glfwTerminate();
	return 0;
}
#endif // !_WIN32
