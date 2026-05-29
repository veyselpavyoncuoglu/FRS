#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <functional>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "app.h"
#include "platform.h"

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_sc = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static std::function<void()> g_renderFn;

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
	switch (msg) {
		case WM_SIZE:
			if (wp != SIZE_MINIMIZED && g_dev) {
				dropRTV();
				g_sc->ResizeBuffers(0,(UINT)LOWORD(lp),(UINT)HIWORD(lp),DXGI_FORMAT_UNKNOWN,0);
				makeRTV();
			}
			return 0;
		case WM_ENTERSIZEMOVE:
			SetTimer(hwnd,1,16,nullptr);
			return 0;
		case WM_EXITSIZEMOVE:
			KillTimer(hwnd,1);
			return 0;
		case WM_TIMER:
			if (g_renderFn) g_renderFn();
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

int WINAPI WinMain(HINSTANCE hi,HINSTANCE,LPSTR,int) {
	plat::raisePrivileges();

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = CS_CLASSDC;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hi;
	wc.lpszClassName = L"FRS";
	wc.hIcon   = LoadIconW(hi,MAKEINTRESOURCEW(1));
	wc.hIconSm = LoadIconW(hi,MAKEINTRESOURCEW(1));
	RegisterClassExW(&wc);

	HWND hwnd = CreateWindowW(wc.lpszClassName,L"FRS - Free RAM System",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,820,660,nullptr,nullptr,hi,nullptr);

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

	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 4.0f;
	style.FrameRounding = 3.0f;
	style.GrabRounding = 3.0f;
	style.TabRounding = 3.0f;

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_dev,g_ctx);

	App app;
	app.init();

	auto doFrame = [&]() {
		app.update();

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		app.drawUI();

		ImGui::Render();
		const float bg[4] = {0.08f,0.08f,0.08f,1.0f};
		g_ctx->OMSetRenderTargets(1,&g_rtv,nullptr);
		g_ctx->ClearRenderTargetView(g_rtv,bg);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_sc->Present(1,0);
	};

	g_renderFn = [&]() { doFrame(); };

	bool running = true;
	while (running) {
		MSG msg;
		while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT) running = false;
		}
		if (!running) break;
		doFrame();
	}

	g_renderFn = nullptr;

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	cleanDX();
	DestroyWindow(hwnd);
	UnregisterClassW(wc.lpszClassName,hi);
	return 0;
}
