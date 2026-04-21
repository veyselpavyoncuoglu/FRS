#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <d3d11.h>
#include <cstdio>
#include <ctime>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "ram.h"

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

static void enablePriv(LPCWSTR name) {
	HANDLE tok;
	if (!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,&tok)) return;
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

static std::string ts() {
	time_t t = time(nullptr);
	tm tm_;
	localtime_s(&tm_,&t);
	char buf[16];
	strftime(buf,sizeof(buf),"[%H:%M:%S]",&tm_);
	return buf;
}

static std::wstring getSettingsPath() {
	wchar_t buf[MAX_PATH];
	SHGetFolderPathW(nullptr,CSIDL_APPDATA,nullptr,0,buf);
	std::wstring dir = std::wstring(buf) + L"\\FRS";
	CreateDirectoryW(dir.c_str(),nullptr);
	return dir + L"\\settings.ini";
}

static void saveSettings(const std::wstring& path,int flushSz,bool autoRefresh,float refreshSec,bool autoClean,int cleanThr,bool acFS,bool acEW,bool acPS,bool acFM,int maxLog) {
	FILE* f = nullptr;
	_wfopen_s(&f,path.c_str(),L"w");
	if (!f) return;
	fprintf(f,"flushSz=%d\n",flushSz);
	fprintf(f,"autoRefresh=%d\n",(int)autoRefresh);
	fprintf(f,"refreshSec=%.2f\n",refreshSec);
	fprintf(f,"autoClean=%d\n",(int)autoClean);
	fprintf(f,"cleanThr=%d\n",cleanThr);
	fprintf(f,"acFS=%d\n",(int)acFS);
	fprintf(f,"acEW=%d\n",(int)acEW);
	fprintf(f,"acPS=%d\n",(int)acPS);
	fprintf(f,"acFM=%d\n",(int)acFM);
	fprintf(f,"maxLog=%d\n",maxLog);
	fclose(f);
}

static void loadSettings(const std::wstring& path,int& flushSz,bool& autoRefresh,float& refreshSec,bool& autoClean,int& cleanThr,bool& acFS,bool& acEW,bool& acPS,bool& acFM,int& maxLog) {
	FILE* f = nullptr;
	_wfopen_s(&f,path.c_str(),L"r");
	if (!f) return;
	char line[128];
	while (fgets(line,sizeof(line),f)) {
		char key[64]; int ival; float fval;
		if (sscanf_s(line,"%63[^=]=%f",key,(unsigned int)sizeof(key),&fval) == 2) {
			ival = (int)fval;
			if (strcmp(key,"flushSz") == 0) flushSz = ival;
			else if (strcmp(key,"autoRefresh") == 0) autoRefresh = ival != 0;
			else if (strcmp(key,"refreshSec") == 0) refreshSec = fval;
			else if (strcmp(key,"autoClean") == 0) autoClean = ival != 0;
			else if (strcmp(key,"cleanThr") == 0) cleanThr = ival;
			else if (strcmp(key,"acFS") == 0) acFS = ival != 0;
			else if (strcmp(key,"acEW") == 0) acEW = ival != 0;
			else if (strcmp(key,"acPS") == 0) acPS = ival != 0;
			else if (strcmp(key,"acFM") == 0) acFM = ival != 0;
			else if (strcmp(key,"maxLog") == 0) maxLog = ival;
		}
	}
	fclose(f);
}

static bool runHidden(std::wstring cmd) {
	SECURITY_ATTRIBUTES sa = {};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	HANDLE nul = CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	si.wShowWindow = SW_HIDE;
	si.hStdOutput = nul;
	si.hStdError = nul;
	PROCESS_INFORMATION pi = {};
	bool ok = CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi) != 0;
	if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
	if (!ok) return false;
	WaitForSingleObject(pi.hProcess,5000);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess,&code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return code == 0;
}

static bool isStartupEnabled() {
	return runHidden(L"schtasks /query /tn \"FRS\" /fo LIST");
}

static bool setStartupEnabled(bool on) {
	if (on) {
		wchar_t buf[MAX_PATH];
		GetModuleFileNameW(nullptr,buf,MAX_PATH);
		std::wstring exe = buf;
		std::wstring cmd = L"schtasks /create /tn \"FRS\" /tr \"\\\"" + exe + L"\\\"\" /sc onlogon /rl highest /f";
		return runHidden(cmd);
	} else {
		return runHidden(L"schtasks /delete /tn \"FRS\" /f");
	}
}

int WINAPI WinMain(HINSTANCE hi,HINSTANCE,LPSTR,int) {
	enablePriv(SE_DEBUG_NAME);
	enablePriv(L"SeProfileSingleProcessPrivilege");

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

	RamStats stats = getRamStats();
	bool elev = isElevated();
	int flushSz = 512;
	std::vector<std::string> log;
	bool scrollLog = false;

	bool autoRefresh = true;
	float refreshSec = 1.0f;
	bool autoClean = false;
	int cleanThr = 20;
	bool acFS = false,acEW = true,acPS = false,acFM = false;
	int maxLog = 200;

	ULONGLONG lastRefresh = GetTickCount64();

	std::wstring cfgPath = getSettingsPath();
	loadSettings(cfgPath,flushSz,autoRefresh,refreshSec,autoClean,cleanThr,acFS,acEW,acPS,acFM,maxLog);
	bool startupEnabled = isStartupEnabled();

	std::mutex logMtx;
	std::atomic<bool> cleaning{false};
	std::atomic<float> cleanProg{0.0f};
	std::vector<std::string> pendingLog;

	auto trimLog = [&]() {
		while ((int)log.size() > maxLog) log.erase(log.begin());
	};
	auto addLog = [&](std::string s) {
		log.push_back(s);
		trimLog();
		scrollLog = true;
	};
	auto flushPending = [&]() {
		std::lock_guard<std::mutex> lk(logMtx);
		for (auto& e : pendingLog) addLog(e);
		pendingLog.clear();
	};

	auto runClean = [&](bool fs,bool ew,bool ps,bool fm,const char* pfx = "") {
		if (cleaning.load()) return;
		int total = (int)fs + (int)ew + (int)ps + (int)fm;
		if (total == 0) return;
		cleaning.store(true);
		cleanProg.store(0.0f);
		int sz = flushSz;
		std::string pre = pfx;
		std::thread([&,fs,ew,ps,fm,total,sz,pre]() {
			int done = 0;
			auto step = [&](std::string r) {
				{
					std::lock_guard<std::mutex> lk(logMtx);
					pendingLog.push_back(ts() + " " + pre + r);
				}
				++done;
				cleanProg.store((float)done / (float)total);
			};
			if (fs) step(flushStandby(sz));
			if (ew) step(emptyWorkSets());
			if (ps) step(purgeStandby());
			if (fm) step(flushModified());
			cleaning.store(false);
		}).detach();
	};

	auto doFrame = [&]() {
		flushPending();
		ULONGLONG now = GetTickCount64();
		if (autoRefresh && (now - lastRefresh) >= (ULONGLONG)(refreshSec * 1000.0f)) {
			if (!cleaning.load()) {
				stats = getRamStats();
				lastRefresh = now;
				if (autoClean) {
					float pct = (stats.total > 0) ? (float)stats.used / (float)stats.total * 100.0f : 0.0f;
					if (pct >= (float)cleanThr) {
						runClean(acFS,acEW,acPS,acFM,"[auto] ");
					}
				}
			} else {
				lastRefresh = now;
			}
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		ImGui::SetNextWindowPos(ImVec2(0,0));
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::Begin("##w",nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoSavedSettings);

		ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"FRS");
		ImGui::SameLine();
		ImGui::TextDisabled("Free RAM System  (Windows)");
		ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("[NOT ELEVATED]").x);
		if (elev) {
			ImGui::TextColored(ImVec4(0.3f,1.0f,0.45f,1.0f),"[ADMIN]");
		} else {
			ImGui::TextColored(ImVec4(1.0f,0.3f,0.3f,1.0f),"[NOT ELEVATED]");
		}

		ImGui::Separator();

		float used = (float)stats.used;
		float total = (float)stats.total;
		float frac = (total > 0.0f) ? used / total : 0.0f;
		auto lerp4 = [](ImVec4 a,ImVec4 b,float t) {
			return ImVec4(a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t, 1.0f);
		};
		ImVec4 colGreen = ImVec4(0.2f,0.78f,0.3f,1.0f);
		ImVec4 colYellow = ImVec4(1.0f,0.7f,0.1f,1.0f);
		ImVec4 colRed = ImVec4(1.0f,0.25f,0.25f,1.0f);
		ImVec4 barCol;
		if (frac < 0.6f) barCol = lerp4(colGreen,colYellow,frac / 0.6f);
		else barCol = lerp4(colYellow,colRed,(frac - 0.6f) / 0.4f);

		ImGui::Text("RAM :");
		ImGui::SameLine();
		char ovl[80];
		snprintf(ovl,sizeof(ovl),"%.0f / %.0f MB   (%.0f MB free | %.1f%%)",used,total,(float)stats.free,frac * 100.0f);
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram,barCol);
		ImGui::ProgressBar(frac,ImVec2(-1.0f,20.0f),"");
		ImGui::PopStyleColor();
		{
			ImVec2 rmin = ImGui::GetItemRectMin();
			ImVec2 rmax = ImGui::GetItemRectMax();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImVec2 tsz = ImGui::CalcTextSize(ovl);
			ImVec2 tp = ImVec2(rmin.x + (rmax.x - rmin.x - tsz.x) * 0.5f, rmin.y + (rmax.y - rmin.y - tsz.y) * 0.5f);
			ImU32 sh = IM_COL32(0,0,0,210);
			dl->AddText(ImVec2(tp.x+1,tp.y+1),sh,ovl);
			dl->AddText(ImVec2(tp.x-1,tp.y+1),sh,ovl);
			dl->AddText(ImVec2(tp.x+1,tp.y-1),sh,ovl);
			dl->AddText(ImVec2(tp.x-1,tp.y-1),sh,ovl);
			dl->AddText(tp,IM_COL32(255,255,255,255),ovl);
		}

		if (ImGui::Button("Refresh")) {
			stats = getRamStats();
			elev = isElevated();
			lastRefresh = GetTickCount64();
		}
		ImGui::SameLine();
		if (autoRefresh) {
			ImGui::TextColored(ImVec4(0.3f,1.0f,0.45f,1.0f),"AUTO");
		} else {
			ImGui::TextDisabled("AUTO  [off]");
		}

		ImGui::Separator();

		if (ImGui::BeginTabBar("##tabs")) {

			if (ImGui::BeginTabItem("Dashboard")) {
				ImGui::Spacing();

				ImGui::AlignTextToFramePadding();
				ImGui::Text("Flush block (MB) :");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(110.0f);
				if (ImGui::InputInt("##fsz",&flushSz,64,512)) saveSettings(cfgPath,flushSz,autoRefresh,refreshSec,autoClean,cleanThr,acFS,acEW,acPS,acFM,maxLog);
				ImGui::SameLine();
				if (ImGui::Button("Max")) {
					stats = getRamStats();
					flushSz = (int)stats.free;
				}
				if (flushSz < 64) flushSz = 64;
				if (flushSz > 65536) flushSz = 65536;

				ImGui::Spacing();

				bool busy = cleaning.load();
				if (busy) ImGui::BeginDisabled();

				if (ImGui::Button("Flush Standby")) runClean(true,false,false,false);
				ImGui::SameLine();
				if (ImGui::Button("Empty Working Sets")) runClean(false,true,false,false);
				ImGui::SameLine();
				if (ImGui::Button("Purge Standby")) runClean(false,false,true,false);
				ImGui::SameLine();
				if (ImGui::Button("Flush Modified")) runClean(false,false,false,true);

				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.15f,0.55f,0.2f,1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.2f,0.75f,0.28f,1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0.1f,0.45f,0.15f,1.0f));
				if (ImGui::Button("Full Clean  (all methods)",ImVec2(-1.0f,28.0f))) {
					runClean(true,true,true,true);
				}
				ImGui::PopStyleColor(3);

				if (busy) ImGui::EndDisabled();

				ImGui::Spacing();
				if (busy) {
					float p = cleanProg.load();
					char pl[32];
					snprintf(pl,sizeof(pl),"Cleaning... %.0f%%",p * 100.0f);
					ImGui::PushStyleColor(ImGuiCol_PlotHistogram,ImVec4(0.35f,0.85f,1.0f,1.0f));
					ImGui::ProgressBar(p,ImVec2(-1.0f,18.0f),pl);
					ImGui::PopStyleColor();
				} else {
					ImGui::PushStyleColor(ImGuiCol_PlotHistogram,ImVec4(0,0,0,0));
					ImGui::ProgressBar(0.0f,ImVec2(-1.0f,18.0f),"Idle");
					ImGui::PopStyleColor();
				}

				ImGui::Separator();
				ImGui::Text("Log :");
				ImGui::SameLine();
				if (ImGui::SmallButton("Clear")) log.clear();

				ImGui::BeginChild("##log",ImVec2(0,-1.0f),true);
				for (auto& e : log) {
					const char* s = e.c_str();
					ImVec4 col;
					if (strstr(s,"failed") || strstr(s,"NTSTATUS") || strstr(s,"[!]"))
						col = ImVec4(1.0f,0.35f,0.35f,1.0f);
					else if (strstr(s,"[auto]"))
						col = ImVec4(1.0f,0.75f,0.2f,1.0f);
					else if (strstr(s,"OK") || strstr(s,"flushed") || strstr(s,"trimmed") || strstr(s,"purge"))
						col = ImVec4(0.3f,1.0f,0.5f,1.0f);
					else
						col = ImVec4(0.75f,0.75f,0.75f,1.0f);
					ImGui::TextColored(col,"%s",s);
				}
				if (scrollLog) {
					ImGui::SetScrollHereY(1.0f);
					scrollLog = false;
				}
				ImGui::EndChild();

				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Settings")) {
				ImGui::Spacing();
				bool changed = false;

				ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"Auto-Refresh");
				changed |= ImGui::Checkbox("Enabled##ar",&autoRefresh);
				ImGui::SameLine();
				ImGui::SetNextItemWidth(70.0f);
				changed |= ImGui::InputFloat("sec##ri",&refreshSec,0.1f,1.0f,"%.1f");
				if (refreshSec < 0.1f) refreshSec = 0.1f;
				if (refreshSec > 60.0f) refreshSec = 60.0f;
				ImGui::TextDisabled("  Polls RAM stats every N seconds.");

				ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

				ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"Auto-Clean");
				changed |= ImGui::Checkbox("Enabled##ac",&autoClean);
				ImGui::SameLine();
				ImGui::SetNextItemWidth(70.0f);
				changed |= ImGui::InputInt("%% threshold##ct",&cleanThr,1,5);
				if (cleanThr < 1) cleanThr = 1;
				if (cleanThr > 100) cleanThr = 100;
				ImGui::TextDisabled("  Triggers clean when RAM usage >= threshold.");

				ImGui::Spacing();
				ImGui::Text("Methods :");
				changed |= ImGui::Checkbox("Flush Standby##acm",&acFS);
				ImGui::SameLine(220.0f);
				changed |= ImGui::Checkbox("Empty Working Sets##acm",&acEW);
				changed |= ImGui::Checkbox("Purge Standby##acm",&acPS);
				ImGui::SameLine(220.0f);
				changed |= ImGui::Checkbox("Flush Modified##acm",&acFM);

				ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

				ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"Log");
				ImGui::SetNextItemWidth(100.0f);
				changed |= ImGui::InputInt("max entries##ml",&maxLog,10,50);
				if (maxLog < 10) maxLog = 10;
				if (maxLog > 9999) maxLog = 9999;
				ImGui::SameLine();
				if (ImGui::Button("Clear Log")) log.clear();
				ImGui::TextDisabled("  Oldest entries removed when limit is exceeded.");

				if (changed) saveSettings(cfgPath,flushSz,autoRefresh,refreshSec,autoClean,cleanThr,acFS,acEW,acPS,acFM,maxLog);

				ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

				ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"Startup");
				bool startupCopy = startupEnabled;
				if (ImGui::Checkbox("Run at Windows startup (no UAC prompt)##st",&startupCopy)) {
					if (setStartupEnabled(startupCopy)) {
						startupEnabled = startupCopy;
					}
				}
				ImGui::TextDisabled("  Uses Task Scheduler with 'Run with highest privileges'.");

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
