#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "app.h"
#include "cfg.h"
#include "platform.h"
#include "ui.h"
#include "util.h"
#include "domains.h"
#include "mcp.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_sc = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static std::function<void()> g_renderFn;
static UINT g_showMsg = 0; // a second launch asks this one to come forward

static POINT g_pos = {0,0};
static bool g_posOk = false;
static HMONITOR g_mmMon = nullptr; // the monitor CreateWindowW puts it on, WM_GETMINMAXINFO comes before the window has a rect

// position of the normal window from every move (keyboard snap and the shell too), maximized and minimized keep the last one
static void notePos(HWND hwnd) {
	RECT r;
	if (IsIconic(hwnd) || IsZoomed(hwnd) || !GetWindowRect(hwnd,&r)) return;
	g_pos = {r.left,r.top};
	g_posOk = true;
}

static void savePos(HWND hwnd) {
	notePos(hwnd);
	if (g_posOk) cfg::sets("winPos",std::to_string(g_pos.x) + "," + std::to_string(g_pos.y));
	// minimized from maximized counts as maximized, the flag stays set after a restore and means something only while minimized
	WINDOWPLACEMENT wp = {};
	wp.length = sizeof(wp);
	if (GetWindowPlacement(hwnd,&wp)) cfg::seti("winMax",wp.showCmd == SW_SHOWMAXIMIZED || (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED)) ? 1 : 0);
}

static void makeRTV() {
	ID3D11Texture2D* buf = nullptr;
	g_sc->GetBuffer(0,IID_PPV_ARGS(&buf));
	g_dev->CreateRenderTargetView(buf,nullptr,&g_rtv);
	buf->Release();
}

static void dropRTV() {
	if (g_rtv) {
		g_rtv->Release();
		g_rtv = nullptr;
	}
}

static bool initDX(HWND hwnd) {
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	D3D_FEATURE_LEVEL fl;
	D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0};
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,fls,2,D3D11_SDK_VERSION,&sd,&g_sc,&g_dev,&fl,&g_ctx);
	if (hr == DXGI_ERROR_UNSUPPORTED) {
		hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,fls,2,D3D11_SDK_VERSION,&sd,&g_sc,&g_dev,&fl,&g_ctx);
	}
	if (FAILED(hr)) return false;
	makeRTV();
	return true;
}

static void cleanDX() {
	dropRTV();
	if (g_sc) { g_sc->Release(); g_sc = nullptr; }
	if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
	if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static LRESULT WINAPI WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
	if (ImGui_ImplWin32_WndProcHandler(hwnd,msg,wp,lp)) return true;
	if (g_showMsg && msg == g_showMsg) {
		bool na = util::testEnv("FRS_TEST_NOACTIVATE") != nullptr;
		if (IsIconic(hwnd)) ShowWindow(hwnd,na ? SW_SHOWNOACTIVATE : SW_RESTORE);
		else ShowWindow(hwnd,na ? SW_SHOWNA : SW_SHOW);
		if (!na) SetForegroundWindow(hwnd);
		// a second launch with -sysinfo
		if (wp == 1) mcp::post([] { nav::go("memory"); });
		return 0;
	}
	long long res = 0;
	if (surf::winMsg(hwnd,msg,(unsigned long long)wp,(long long)lp,res)) return (LRESULT)res;
	switch (msg) {
		case WM_SIZE:
			if (wp != SIZE_MINIMIZED && g_dev) {
				dropRTV();
				g_sc->ResizeBuffers(0,(UINT)LOWORD(lp),(UINT)HIWORD(lp),DXGI_FORMAT_UNKNOWN,0);
				makeRTV();
			}
			return 0;
		case WM_MOVE:
			notePos(hwnd);
			break;
		case WM_ENTERSIZEMOVE:
			SetTimer(hwnd,1,16,nullptr);
			return 0;
		case WM_EXITSIZEMOVE:
			KillTimer(hwnd,1);
			savePos(hwnd);
			return 0;
		case WM_TIMER:
			if (g_renderFn) g_renderFn();
			return 0;
		case WM_GETMINMAXINFO: {
			HMONITOR mon = g_mmMon ? g_mmMon : MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST);
			float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);
			auto mm = (MINMAXINFO*)lp;
			MONITORINFO mi = {};
			mi.cbSize = sizeof(mi);
			mm->ptMinTrackSize.x = (LONG)(900.0f * dpi);
			mm->ptMinTrackSize.y = (LONG)(600.0f * dpi);
			if (GetMonitorInfoW(mon,&mi)) {
				mm->ptMinTrackSize.x = std::min(mm->ptMinTrackSize.x,mi.rcWork.right - mi.rcWork.left);
				mm->ptMinTrackSize.y = std::min(mm->ptMinTrackSize.y,mi.rcWork.bottom - mi.rcWork.top);
			}
			return 0;
		}
		case WM_DPICHANGED: {
			auto r = (const RECT*)lp;
			SetWindowPos(hwnd,nullptr,r->left,r->top,r->right - r->left,r->bottom - r->top,SWP_NOZORDER | SWP_NOACTIVATE);
			if (ImGui::GetCurrentContext()) ui::applyTheme((float)HIWORD(wp) / 96.0f);
			return 0;
		}
		case WM_SYSCOMMAND:
			if ((wp & 0xfff0) == SC_KEYMENU) return 0;
			break;
		case WM_DESTROY:
			savePos(hwnd);
			RemovePropW(hwnd,L"FRS");
			PostQuitMessage(0);
			return 0;
	}
	return DefWindowProcW(hwnd,msg,wp,lp);
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

static std::string utf8(const wchar_t* w) {
	int n = WideCharToMultiByte(CP_UTF8,0,w,-1,nullptr,0,nullptr,nullptr);
	std::string s(n > 1 ? n - 1 : 0,'\0');
	if (n > 1) WideCharToMultiByte(CP_UTF8,0,w,-1,&s[0],n,nullptr,nullptr);
	return s;
}

static void showBox(const std::string& s,bool warn) {
	// a test exe never puts a box in front of the user, the text goes to its settings folder
	if (util::testEnv("FRS_TEST_NOACTIVATE")) {
		plat::writeFile("clibox.txt",std::string(warn ? "warning\n" : "info\n") + s);
		return;
	}
	int n = MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);
	std::wstring w(n > 0 ? n : 1,L'\0');
	MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,&w[0],n);
	MessageBoxW(nullptr,w.c_str(),L"FRS",MB_OK | (warn ? MB_ICONWARNING : MB_ICONINFORMATION));
}

int WINAPI WinMain(HINSTANCE hi,HINSTANCE,LPSTR,int) {
	plat::raisePrivileges();
	ImGui_ImplWin32_EnableDpiAwareness();

	std::vector<std::string> args;
	int argc = 0;
	LPWSTR* av = CommandLineToArgvW(GetCommandLineW(),&argc);
	for (int i = 1; av && i < argc; i++) args.push_back(utf8(av[i]));
	if (av) LocalFree(av);
	std::string out;
	int code = App::cli(args,out);
	if (code >= 0) {
		if (!out.empty()) showBox(out,code != 0);
		return code;
	}
	bool mini = false;
	for (auto& a : args) if (util::lower(a) == "-minimized" || util::lower(a) == "/minimized") mini = true;

	std::string ini = plat::readSettings();
	if (!iniInt(ini,"multiInst")) {
		// the other one has no window while it starts, or while it closes and finishes a clean : wait up to 10 s for one or the other
		bool first = plat::firstInstance(),mem = sysmem::osArgSysinfo();
		for (int i = 0; !first && i < 50; i++) {
			if (plat::showOther(mem)) return 0;
			Sleep(200);
			first = plat::firstInstance();
			// it saved its last size and spot on the way out
			if (first) ini = plat::readSettings();
		}
		if (!first) return 0;
	}
	g_showMsg = RegisterWindowMessageW(L"FRS_SHOW");

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_CLASSDC;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hi;
	wc.lpszClassName = L"FRS";
	wc.hIcon   = LoadIconW(hi,MAKEINTRESOURCEW(1));
	wc.hIconSm = LoadIconW(hi,MAKEINTRESOURCEW(1));
	RegisterClassExW(&wc);

	// winW/winH hold the client size, zero means first run
	float pdpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY));
	int cw = iniInt(ini,"winW");
	int ch = iniInt(ini,"winH");
	if (cw <= 0 || ch <= 0) {
		cw = (int)(1240.0f * pdpi);
		ch = (int)(800.0f * pdpi);
	}
	RECT wr = {0,0,cw,ch};
	AdjustWindowRect(&wr,WS_OVERLAPPEDWINDOW,FALSE);
	int ww = wr.right - wr.left;
	int wh = wr.bottom - wr.top;
	// back where it was closed while that spot is on a monitor, else where Windows puts it
	int px = 0,py = 0;
	HMONITOR mon = iniPos(ini,px,py) ? MonitorFromPoint(POINT{px + ww / 2,py + wh / 2},MONITOR_DEFAULTTONULL) : nullptr;
	MONITORINFO mi = {};
	mi.cbSize = sizeof(mi);
	bool place = mon && GetMonitorInfoW(mon,&mi);
	// the frame at that monitor's DPI, AdjustWindowRect takes the primary's and the size crept on a monitor with another scale. Windows 10 1607+
	typedef BOOL (WINAPI* AdjFn)(LPRECT,DWORD,BOOL,DWORD,UINT);
	auto adj = (AdjFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"),"AdjustWindowRectExForDpi");
	RECT mr = {0,0,cw,ch};
	if (place && adj && adj(&mr,WS_OVERLAPPEDWINDOW,FALSE,0,(UINT)(ImGui_ImplWin32_GetDpiScaleForMonitor(mon) * 96.0f + 0.5f))) {
		ww = mr.right - mr.left;
		wh = mr.bottom - mr.top;
	}
	RECT area = {};
	if (place) area = mi.rcWork;
	if (place || SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0)) {
		ww = std::min(ww,(int)(area.right - area.left));
		wh = std::min(wh,(int)(area.bottom - area.top));
	}
	if (place) {
		px = std::max((int)area.left,std::min(px,(int)area.right - ww));
		py = std::max((int)area.top,std::min(py,(int)area.bottom - wh));
	}

	// closed maximized : created maximized and hidden, the size and spot above stay its normal ones
	bool max = iniInt(ini,"winMax") != 0;
	// and with no saved spot no cascade either, the nudge below can't move a maximized window and a restore would go under the taskbar
	if (max && !place) {
		px = area.left;
		py = area.top;
	}
	g_mmMon = place ? mon : nullptr;
	HWND hwnd = CreateWindowW(wc.lpszClassName,L"FRS - Free RAM System",WS_OVERLAPPEDWINDOW | (max ? WS_MAXIMIZE : 0),place || max ? px : CW_USEDEFAULT,place || max ? py : CW_USEDEFAULT,ww,wh,nullptr,nullptr,hi,nullptr);
	g_mmMon = nullptr;
	surf::setHwnd(hwnd);
	plat::markMain(hwnd);

	// CW_USEDEFAULT cascades, a window as big as the work area would end up under the taskbar
	RECT cur;
	if (!place && !max && area.right > area.left && GetWindowRect(hwnd,&cur) && (cur.right > area.right || cur.bottom > area.bottom)) {
		SetWindowPos(hwnd,nullptr,std::max(area.left,area.right - (cur.right - cur.left)),std::max(area.top,area.bottom - (cur.bottom - cur.top)),0,0,SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}

	if (!initDX(hwnd)) {
		cleanDX();
		UnregisterClassW(wc.lpszClassName,hi);
		return 1;
	}

	BOOL dark = TRUE;
	DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE, old builds just refuse
	// Start hidden waits for its tray icon, minimized until then so the window does not flash up
	bool icon = false;
	for (auto k : {"trayPhysHist","trayCommitHist","trayPhysText","trayCommitText","trayGpuHist","trayGpuText"}) icon |= iniInt(ini,k) != 0;
	mini |= iniInt(ini,"startMin") != 0 || (icon && iniInt(ini,"trayStartHidden") != 0);
	// minimized wins, restoring it then goes back to maximized
	bool na = util::testEnv("FRS_TEST_NOACTIVATE") != nullptr;
	ShowWindow(hwnd,mini ? SW_SHOWMINNOACTIVE : max ? (na ? SW_SHOWNA : SW_SHOWMAXIMIZED) : na ? SW_SHOWNOACTIVATE : SW_SHOWDEFAULT);
	UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;

	float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
	ui::applyTheme(dpi);
	ui::loadFonts(dpi);

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_dev,g_ctx);

	{
		App app;
		app.init();

		auto doFrame = [&]() {
			app.update();

			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			// the next update() saves this io.DisplaySize as winW/winH, only the normal window's
			App::sizeOk = !IsZoomed(hwnd) && !IsIconic(hwnd);
			ImGui::NewFrame();

			app.drawUI();

			ImGui::Render();
			const float bg[4] = {ui::c.bg.x,ui::c.bg.y,ui::c.bg.z,1.0f};
			g_ctx->OMSetRenderTargets(1,&g_rtv,nullptr);
			g_ctx->ClearRenderTargetView(g_rtv,bg);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			g_sc->Present(1,0);
		};

		g_renderFn = [&]() { doFrame(); };

		bool running = true;
		int idle = 0;
		while (running) {
			// nothing came in for a few frames : wait for input up to 100 ms instead of drawing every refresh. Data changes once a second at most.
			// Also paces an occluded window (locked screen), where Present returns at once
			if (idle >= 3) MsgWaitForMultipleObjectsEx(0,nullptr,100,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
			MSG msg;
			while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
				if (msg.message == WM_QUIT) running = false;
				idle = 0;
			}
			if (!running) break;
			// nothing to draw and Present returns at once, so pace it. Auto-clean and MCP keep going.
			RECT rc = {};
			GetClientRect(hwnd,&rc);
			if (IsIconic(hwnd) || !IsWindowVisible(hwnd) || rc.right <= rc.left || rc.bottom <= rc.top) {
				Sleep(20);
				app.update();
				continue;
			}
			doFrame();
			idle++;
		}

		g_renderFn = nullptr;
	}

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	cleanDX();
	DestroyWindow(hwnd);
	UnregisterClassW(wc.lpszClassName,hi);
	return 0;
}
#endif // _WIN32
