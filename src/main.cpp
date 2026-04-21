#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <ctime>
#include <vector>
#include <string>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "ram.h"

static ID3D11Device*           g_dev = nullptr;
static ID3D11DeviceContext*    g_ctx = nullptr;
static IDXGISwapChain*         g_sc  = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;

static void makeRTV() {
	ID3D11Texture2D* buf = nullptr;
	g_sc->GetBuffer(0,IID_PPV_ARGS(&buf));
	g_dev->CreateRenderTargetView(buf,nullptr,&g_rtv);
	buf->Release();
}

static void dropRTV() {
	if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
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
	D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0 };
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,fls,2,D3D11_SDK_VERSION,&sd,&g_sc,&g_dev,&fl,&g_ctx);
	if (hr == DXGI_ERROR_UNSUPPORTED)
		hr = D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,fls,2,D3D11_SDK_VERSION,&sd,&g_sc,&g_dev,&fl,&g_ctx);
	if (FAILED(hr)) return false;
	makeRTV();
	return true;
}

static void cleanDX() {
	dropRTV();
	if (g_sc)  { g_sc->Release();  g_sc  = nullptr; }
	if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
	if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

static void tryEnableDebugPriv() {
	HANDLE tok;
	if (!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&tok)) return;
	LUID luid;
	if (LookupPrivilegeValue(nullptr,SE_DEBUG_NAME,&luid)) {
		TOKEN_PRIVILEGES tp = {};
		tp.PrivilegeCount = 1;
		tp.Privileges[0].Luid = luid;
		tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
		AdjustTokenPrivileges(tok,FALSE,&tp,sizeof(tp),nullptr,nullptr);
	}
	CloseHandle(tok);
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static LRESULT WINAPI WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
	if (ImGui_ImplWin32_WndProcHandler(hwnd,msg,wp,lp)) return true;
	switch (msg) {
		case WM_SIZE:
			if (wp != SIZE_MINIMIZED && g_dev) {
				dropRTV();
				g_sc->ResizeBuffers(0,(UINT)LOWORD(lp),(UINT)HIWORD(lp),DXGI_FORMAT_UNKNOWN,0);
				makeRTV();
			}
			return 0;
		case WM_SYSCOMMAND:
			if ((wp & 0xfff0) == SC_KEYMENU) return 0;
			break;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
	}
	return DefWindowProcW(hwnd,msg,wp,lp);
}

static std::string ts() {
	time_t t = time(nullptr);
	tm tm_;
	localtime_s(&tm_,&t);
	char buf[16];
	strftime(buf,sizeof(buf),"[%H:%M:%S]",&tm_);
	return buf;
}

int WINAPI WinMain(HINSTANCE hi,HINSTANCE,LPSTR,int) {
	tryEnableDebugPriv();

	WNDCLASSEXW wc = {sizeof(wc),CS_CLASSDC,WndProc,0L,0L,hi,nullptr,nullptr,nullptr,nullptr,L"RAMCleaner",nullptr};
	RegisterClassExW(&wc);
	HWND hwnd = CreateWindowW(wc.lpszClassName,L"RAM Cleaner",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,720,560,nullptr,nullptr,hi,nullptr);

	if (!initDX(hwnd)) {
		cleanDX();
		UnregisterClassW(wc.lpszClassName,hi);
		return 1;
	}

	ShowWindow(hwnd,SW_SHOWDEFAULT);
	UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_dev,g_ctx);

	RamStats stats = getRamStats();
	bool elev = isElevated();
	int flushSz = 512;
	std::vector<std::string> log;
	bool scrollLog = false;

	bool running = true;
	while (running) {
		MSG msg;
		while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT) running = false;
		}
		if (!running) break;

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		ImGui::SetNextWindowPos(ImVec2(0,0));
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::Begin("##w",nullptr,
			ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
			ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|
			ImGuiWindowFlags_NoSavedSettings);

		ImGui::TextColored(ImVec4(0.4f,0.85f,1.0f,1.0f),"RAM Cleaner");
		ImGui::SameLine();
		ImGui::TextDisabled("(Windows)");

		if (!elev)
			ImGui::TextColored(ImVec4(1.0f,0.3f,0.3f,1.0f),"[!] Not elevated -- EmptyWorkingSets may be limited");

		ImGui::Separator();

		float used  = (float)stats.used;
		float total = (float)stats.total;
		float frac  = (total > 0.0f) ? used/total : 0.0f;
		char overlay[64];
		snprintf(overlay,sizeof(overlay),"%.0f / %.0f MB  (%.0f MB free)",used,total,(float)stats.free);
		ImGui::Text("RAM :");
		ImGui::ProgressBar(frac,ImVec2(-1.0f,22.0f),overlay);

		if (ImGui::Button("Refresh")) {
			stats = getRamStats();
			elev  = isElevated();
		}

		ImGui::Separator();

		ImGui::AlignTextToFramePadding();
		ImGui::Text("Flush block size (MB) :");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120.0f);
		ImGui::InputInt("##fsz",&flushSz,64,256);
		if (flushSz < 64)   flushSz = 64;
		if (flushSz > 8192) flushSz = 8192;

		ImGui::Separator();

		if (ImGui::Button("Flush Standby")) {
			log.push_back(ts() + "  " + flushStandby(flushSz));
			stats = getRamStats();
			scrollLog = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Empty Working Sets")) {
			log.push_back(ts() + "  " + emptyWorkSets());
			stats = getRamStats();
			scrollLog = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Both")) {
			log.push_back(ts() + "  " + flushStandby(flushSz));
			log.push_back(ts() + "  " + emptyWorkSets());
			stats = getRamStats();
			scrollLog = true;
		}

		ImGui::Separator();

		ImGui::Text("Log :");
		ImGui::BeginChild("##log",ImVec2(0,-1.0f),true);
		for (auto& e : log)
			ImGui::TextUnformatted(e.c_str());
		if (scrollLog) {
			ImGui::SetScrollHereY(1.0f);
			scrollLog = false;
		}
		ImGui::EndChild();

		ImGui::End();

		ImGui::Render();
		const float bg[4] = {0.08f,0.08f,0.08f,1.0f};
		g_ctx->OMSetRenderTargets(1,&g_rtv,nullptr);
		g_ctx->ClearRenderTargetView(g_rtv,bg);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_sc->Present(1,0);
	}

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	cleanDX();
	DestroyWindow(hwnd);
	UnregisterClassW(wc.lpszClassName,hi);
	return 0;
}
