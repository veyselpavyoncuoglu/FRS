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

static void enablePriv(LPCWSTR name) {
	HANDLE tok;
	if (!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&tok)) return;
	LUID luid;
	if (LookupPrivilegeValueW(nullptr,name,&luid)) {
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
	enablePriv(SE_DEBUG_NAME);
	enablePriv(L"SeProfileSingleProcessPrivilege");

	WNDCLASSEXW wc = {sizeof(wc),CS_CLASSDC,WndProc,0L,0L,hi,nullptr,nullptr,nullptr,nullptr,L"FRS",nullptr};
	RegisterClassExW(&wc);
	HWND hwnd = CreateWindowW(wc.lpszClassName,L"FRS - Free RAM System",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,820,660,nullptr,nullptr,hi,nullptr);

	if (!initDX(hwnd)) { cleanDX(); UnregisterClassW(wc.lpszClassName,hi); return 1; }

	ShowWindow(hwnd,SW_SHOWDEFAULT);
	UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	ImGui::StyleColorsDark();

	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 4.0f;
	style.FrameRounding  = 3.0f;
	style.GrabRounding   = 3.0f;
	style.TabRounding    = 3.0f;

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_dev,g_ctx);

	// -- state
	RamStats stats = getRamStats();
	bool elev = isElevated();
	int flushSz = 512;
	std::vector<std::string> log;
	bool scrollLog = false;

	// -- settings
	bool autoRefresh = true;
	int  refreshSec  = 1;
	bool autoClean   = false;
	int  cleanThr    = 85;
	bool acFS = true, acEW = true, acPS = true, acFM = false;
	int  maxLog      = 200;

	ULONGLONG lastRefresh = GetTickCount64();

	auto trimLog = [&]() {
		while ((int)log.size() > maxLog) log.erase(log.begin());
	};
	auto addLog = [&](std::string s) {
		log.push_back(s); trimLog(); scrollLog = true;
	};
	auto runClean = [&](bool fs,bool ew,bool ps,bool fm,const char* pfx="") {
		if (fs) addLog(ts() + " " + pfx + flushStandby(flushSz));
		if (ew) addLog(ts() + " " + pfx + emptyWorkSets());
		if (ps) addLog(ts() + " " + pfx + purgeStandby());
		if (fm) addLog(ts() + " " + pfx + flushModified());
		stats = getRamStats();
		lastRefresh = GetTickCount64();
	};

	bool running = true;
	while (running) {
		MSG msg;
		while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT) running = false;
		}
		if (!running) break;

		// auto-refresh + auto-clean
		ULONGLONG now = GetTickCount64();
		if (autoRefresh && (now - lastRefresh) >= (ULONGLONG)refreshSec * 1000) {
			stats = getRamStats();
			lastRefresh = now;
			if (autoClean) {
				float pct = (stats.total > 0) ? (float)stats.used / (float)stats.total * 100.0f : 0.0f;
				if (pct >= (float)cleanThr)
					runClean(acFS,acEW,acPS,acFM,"[auto] ");
			}
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		ImGui::SetNextWindowPos(ImVec2(0,0));
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::Begin("##w",nullptr,
			ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
			ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|
			ImGuiWindowFlags_NoSavedSettings);

		// -- header
		ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"FRS");
		ImGui::SameLine();
		ImGui::TextDisabled("Free RAM System  (Windows)");
		ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("[NOT ELEVATED]").x);
		if (elev)
			ImGui::TextColored(ImVec4(0.3f,1.0f,0.45f,1.0f),"[ADMIN]");
		else
			ImGui::TextColored(ImVec4(1.0f,0.3f,0.3f,1.0f),"[NOT ELEVATED]");

		ImGui::Separator();

		// -- RAM bar (color shifts green -> yellow -> red)
		float used  = (float)stats.used;
		float total = (float)stats.total;
		float frac  = (total > 0.0f) ? used/total : 0.0f;
		ImVec4 barCol = (frac < 0.6f) ? ImVec4(0.2f,0.78f,0.3f,1.0f) :
		                (frac < 0.8f) ? ImVec4(1.0f,0.7f,0.1f,1.0f)  :
		                                ImVec4(1.0f,0.25f,0.25f,1.0f);
		ImGui::Text("RAM :");
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram,barCol);
		char ovl[80];
		snprintf(ovl,sizeof(ovl),"%.0f / %.0f MB   (%.0f MB free | %.1f%%)",used,total,(float)stats.free,frac*100.0f);
		ImGui::ProgressBar(frac,ImVec2(-1.0f,20.0f),ovl);
		ImGui::PopStyleColor();

		// -- refresh controls
		if (ImGui::Button("Refresh")) {
			stats = getRamStats(); elev = isElevated(); lastRefresh = GetTickCount64();
		}
		ImGui::SameLine();
		if (autoRefresh)
			ImGui::TextColored(ImVec4(0.3f,1.0f,0.45f,1.0f),"AUTO");
		else
			ImGui::TextDisabled("AUTO  [off]");

		ImGui::Separator();

		if (ImGui::BeginTabBar("##tabs")) {

			// ===== DASHBOARD =====
			if (ImGui::BeginTabItem("Dashboard")) {
				ImGui::Spacing();

				ImGui::AlignTextToFramePadding();
				ImGui::Text("Flush block (MB) :");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(110.0f);
				ImGui::InputInt("##fsz",&flushSz,64,512);
				ImGui::SameLine();
				if (ImGui::Button("Max")) { stats = getRamStats(); flushSz = (int)stats.free; }
				if (flushSz < 64)    flushSz = 64;
				if (flushSz > 65536) flushSz = 65536;

				ImGui::Spacing();

				if (ImGui::Button("Flush Standby"))      runClean(true,false,false,false);
				ImGui::SameLine();
				if (ImGui::Button("Empty Working Sets"))  runClean(false,true,false,false);
				ImGui::SameLine();
				if (ImGui::Button("Purge Standby"))       runClean(false,false,true,false);
				ImGui::SameLine();
				if (ImGui::Button("Flush Modified"))      runClean(false,false,false,true);

				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Button,       ImVec4(0.15f,0.55f,0.2f,1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.2f,0.75f,0.28f,1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f,0.45f,0.15f,1.0f));
				if (ImGui::Button("Full Clean  (all methods)",ImVec2(-1.0f,28.0f)))
					runClean(true,true,true,true);
				ImGui::PopStyleColor(3);

				ImGui::Separator();
				ImGui::Text("Log :");
				ImGui::SameLine();
				if (ImGui::SmallButton("Clear")) log.clear();

				ImGui::BeginChild("##log",ImVec2(0,-1.0f),true);
				for (auto& e : log) ImGui::TextUnformatted(e.c_str());
				if (scrollLog) { ImGui::SetScrollHereY(1.0f); scrollLog = false; }
				ImGui::EndChild();

				ImGui::EndTabItem();
			}

			// ===== SETTINGS =====
			if (ImGui::BeginTabItem("Settings")) {
				ImGui::Spacing();

				ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"Auto-Refresh");
				ImGui::Checkbox("Enabled##ar",&autoRefresh);
				ImGui::SameLine();
				ImGui::SetNextItemWidth(70.0f);
				ImGui::InputInt("sec##ri",&refreshSec,1,5);
				if (refreshSec < 1)  refreshSec = 1;
				if (refreshSec > 60) refreshSec = 60;
				ImGui::TextDisabled("  Polls RAM stats every N seconds.");

				ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

				ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"Auto-Clean");
				ImGui::Checkbox("Enabled##ac",&autoClean);
				ImGui::SameLine();
				ImGui::SetNextItemWidth(70.0f);
				ImGui::InputInt("%% threshold##ct",&cleanThr,1,5);
				if (cleanThr < 1)   cleanThr = 1;
				if (cleanThr > 100) cleanThr = 100;
				ImGui::TextDisabled("  Triggers clean when RAM usage >= threshold.");

				ImGui::Spacing();
				ImGui::Text("Methods :");
				ImGui::Checkbox("Flush Standby##acm",&acFS);   ImGui::SameLine(220.0f);
				ImGui::Checkbox("Empty Working Sets##acm",&acEW);
				ImGui::Checkbox("Purge Standby##acm",&acPS);   ImGui::SameLine(220.0f);
				ImGui::Checkbox("Flush Modified##acm",&acFM);

				ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

				ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"Log");
				ImGui::SetNextItemWidth(100.0f);
				ImGui::InputInt("max entries##ml",&maxLog,10,50);
				if (maxLog < 10)   maxLog = 10;
				if (maxLog > 9999) maxLog = 9999;
				ImGui::SameLine();
				if (ImGui::Button("Clear Log")) log.clear();
				ImGui::TextDisabled("  Oldest entries removed when limit is exceeded.");

				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}

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