#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "surf.h"
#include "domains.h"
#include "cfg.h"
#include "mcp.h"
#include "ui.h"
// the extra area owns this one, a build without it just has no GPU icons
#if __has_include("gpumem.h")
#include "gpumem.h"
#define TRAY_GPU
#endif

// Tray icons and the taskbar overlay run on their own thread, Shell_NotifyIcon and ITaskbarList3 can block
// for as long as Explorer hangs (SI notifico.c does the same). Menu picks reach the UI thread as bits in g_act.

namespace {

	enum Kind { KPhys,KCommit,KGpu };
	struct IconDef { const char* key; const char* name; int kind; bool text; };
	const IconDef kIcons[] = {
		{"trayPhysHist","Physical memory history",KPhys,false},
		{"trayCommitHist","Commit charge history",KCommit,false},
		{"trayPhysText","Physical usage (text)",KPhys,true},
		{"trayCommitText","Commit usage (text)",KCommit,true},
#ifdef TRAY_GPU
		{"trayGpuHist","GPU memory history",KGpu,false},
		{"trayGpuText","GPU memory usage (text)",KGpu,true},
#endif
	};
	const int IcoN = sizeof(kIcons) / sizeof(kIcons[0]);

	// ActN stays last, trayProc forwards menu ids below CmdBase + ActN only
	enum Act { ActToggle,ActShowHide,ActMemory,ActMiniCommit,ActMiniPhys,ActFlush,ActEmpty,ActPurge,ActModified,ActFull,ActExit,ActGpu,ActSet,ActSettings,ActShow,ActN };
	const UINT CmdBase = 100; // menu id = CmdBase + Act
	const UINT CmdArea = 200; // + CleanInfo::set bit, toggles it
	const UINT CmdAbove = 300; // + threshold %, 0 turns threshold auto-clean off
	const UINT CmdEvery = 500; // + minutes, 0 turns Clean every off
	const wchar_t* kSetNames[] = {L"Flush Standby",L"Empty Working Sets",L"Purge Standby",L"Flush Modified",L"System file cache",L"Modified file cache",L"Priority 0 standby list",L"Registry cache",L"Combine memory pages"};

	const UINT WM_TRAY = WM_APP + 1;
	const UINT WM_STOP = WM_APP + 2;
	const UINT WM_NOTE = WM_APP + 3;
	const UINT_PTR TimerTick = 1;
	const UINT_PTR TimerClick = 2;

	const int HotId = 1;
	const char* kMods[] = {"Ctrl+Shift","Ctrl+Alt","Alt+Shift","Ctrl+Alt+Shift","Ctrl","Alt"};
	const UINT kModBits[] = {MOD_CONTROL | MOD_SHIFT,MOD_CONTROL | MOD_ALT,MOD_ALT | MOD_SHIFT,MOD_CONTROL | MOD_ALT | MOD_SHIFT,MOD_CONTROL,MOD_ALT};
	const int KeyN = 48; // F1..F12, A..Z, 0..9
	const wchar_t* kFonts[] = {L"Tahoma",L"Segoe UI",L"Consolas",L"Lucida Console",L"Arial"};

	HWND g_main = nullptr;
	std::thread g_th;
	std::atomic<int> g_state{0}; // 0 no thread, 1 running, 2 finished and waiting to be joined
	std::atomic<HWND> g_tw{nullptr};
	std::atomic<bool> g_stop{false};
	std::atomic<int> g_up{0}; // icons in the tray right now
	std::atomic<uint32_t> g_act{0};
	std::atomic<bool> g_hideNext{false};
	bool g_exit = false; // UI thread, the next WM_CLOSE really closes
	bool g_started = false;
	uint64_t g_nextStart = 0;
	uint64_t g_nextCheck = 0;
	int g_hkTried = 0; // combo last asked for, (mods + 1) << 8 | key, 0 off
	bool g_hkUp = false;

	std::mutex g_noteM; // the balloon surf::notify hands to the tray thread
	std::string g_noteTitle,g_noteText;
	bool g_noteWarn = false;

	// below : tray thread only
	bool g_added[IcoN] = {};
	bool g_failLog = false;
	UINT g_tickMs = 0;
	int g_clickIcon = 0;
	bool g_clickUp = false;
	bool g_inMenu = false;

	HDC g_dc = nullptr;
	HBITMAP g_bmp = nullptr;
	uint32_t* g_bits = nullptr;
	int g_sz = 0;
	HFONT g_font[2] = {}; // two digits, three
	int g_fontKey[2] = {-1,-1};
	uint64_t g_noteAt = 0;

	ITaskbarList3* g_tb = nullptr;
	bool g_ovlOn = false;
	bool g_progOn = false;
	bool g_tbFailLog = false;

	bool noActivate() { return util::testEnv("FRS_TEST_NOACTIVATE") != nullptr; }

	void act(int a) { g_act.fetch_or(1u << a); }

	bool anyIcon() {
		for (auto& d : kIcons) if (cfg::getb(d.key,false)) return true;
		return false;
	}

	bool wanted() {
		return anyIcon() || cfg::geti("taskOverlay",0) != 0 || cfg::getb("taskProgress",false);
	}

	void freeDib() {
		if (g_dc) DeleteDC(g_dc);
		if (g_bmp) DeleteObject(g_bmp);
		g_dc = nullptr;
		g_bmp = nullptr;
		g_bits = nullptr;
		g_sz = 0;
	}

	bool ensureDib(int sz) {
		if (g_bmp && g_sz == sz) return true;
		freeDib();
		BITMAPINFO bi = {};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = sz;
		bi.bmiHeader.biHeight = -sz;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		void* bits = nullptr;
		g_dc = CreateCompatibleDC(nullptr);
		if (!g_dc) return false;
		g_bmp = CreateDIBSection(g_dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
		if (!g_bmp) {
			freeDib();
			return false;
		}
		g_bits = (uint32_t*)bits;
		g_sz = sz;
		return true;
	}

	HICON toIcon() {
		GdiFlush();
		HBITMAP mask = CreateBitmap(g_sz,g_sz,1,1,nullptr);
		if (!mask) return nullptr;
		ICONINFO ii = {};
		ii.fIcon = TRUE;
		ii.hbmColor = g_bmp;
		ii.hbmMask = mask;
		HICON ic = CreateIconIndirect(&ii);
		DeleteObject(mask);
		return ic;
	}

	// an icon whose alpha is all 0 falls back to its mask and draws as a black square
	void keepAlpha() {
		uint32_t any = 0;
		for (int i = 0; i < g_sz * g_sz; i++) any |= g_bits[i];
		if (!(any & 0xFF000000u)) g_bits[0] |= 0x01000000u;
	}

	// what one icon draws. GPU borrows the commit colour like SI does, a dedicated limit of 0 (iGPU) shows as no data.
	struct Src {
		const std::vector<float>* hist = nullptr;
		uint64_t part = NA,total = NA;
		uint32_t col = 0;
		std::shared_ptr<const void> keep;
	};

	Src srcOf(int kind,const SysMem& m,const SysHist& h) {
		Src s;
		if (kind == KCommit) {
			s.hist = &h.commit;
			s.part = m.commit;
			s.total = m.commitLimit;
			s.col = surf::commitCol();
#ifdef TRAY_GPU
		} else if (kind == KGpu) {
			auto gs = gpumem::latest();
			auto gh = gpumem::history();
			s.hist = &gh->ded;
			s.part = gs->ded;
			s.total = gs->dedLimit;
			s.col = surf::commitCol();
			s.keep = gh;
#endif
		} else {
			s.hist = &h.phys;
			s.part = m.physUsed;
			s.total = m.physTotal;
			s.col = surf::physCol();
		}
		return s;
	}

	// trayHl : past hlWarn or hlBad, the user's colour or the theme's as 0xRRGGBB
	bool lvlCol(const Src& s,uint32_t& c) {
		if (!cfg::getb("trayHl",false) || s.part == NA || s.total == NA || !s.total) return false;
		double p = (double)s.part * 100.0 / (double)s.total;
		bool bad = p >= cfg::geti("hlBad",90);
		if (!bad && p < cfg::geti("hlWarn",80)) return false;
		if (!cfg::getColor(bad ? "trayColBad" : "trayColWarn",c)) c = (bad ? surf::badCol() : surf::warnCol()) & 0xFFFFFFu;
		return true;
	}

	// hl : the line takes the threshold colour, tray icons only, the taskbar overlay keeps the series colour
	HICON graphIcon(const Src& s,bool clear,bool hl) {
		int sz = GetSystemMetrics(SM_CXSMICON);
		if (sz <= 0 || !ensureDib(sz)) return nullptr;
		float mx = s.total == NA ? 0.0f : (float)((double)s.total / 1048576.0);
		uint32_t col = s.col;
		if (hl) lvlCol(s,col);
		surf::rasterGraph(g_bits,sz,sz,*s.hist,mx,col,clear);
		if (clear) keepAlpha();
		return toIcon();
	}

	// three digits get the tallest font that fits, at the two digit size 100 is cut on both sides. Inside the frame with a free px on each side
	HFONT fontFor(int sz,const wchar_t* t,bool frame) {
		int fi = std::min(std::max(cfg::geti("trayFont",0),0),4);
		bool aa = cfg::getb("trayAA",true);
		int n = lstrlenW(t),k = n > 2,key = sz * 32 + frame * 16 + fi * 2 + aa;
		if (g_font[k] && g_fontKey[k] == key) return g_font[k];
		if (g_font[k]) DeleteObject(g_font[k]);
		g_fontKey[k] = key;
		for (int h = MulDiv(11,sz,16); ; h--) {
			g_font[k] = CreateFontW(-h,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,aa ? ANTIALIASED_QUALITY : NONANTIALIASED_QUALITY,DEFAULT_PITCH,kFonts[fi]);
			if (!k || !g_font[k] || h <= sz / 3) break;
			SIZE e = {};
			HGDIOBJ o = SelectObject(g_dc,g_font[k]);
			bool fit = GetTextExtentPoint32W(g_dc,t,n,&e) && e.cx <= (frame ? sz - 4 : sz);
			SelectObject(g_dc,o);
			if (fit) break;
			DeleteObject(g_font[k]);
		}
		return g_font[k];
	}

	// GDI leaves alpha 0 on what it draws, so it gets set afterwards. With a background every pixel inside is
	// opaque. Without one the digits go white on black first, that gray is their coverage and becomes the alpha
	// of the text colour (icons take straight alpha), so black digits and smooth edges work too
	HICON textIcon(const Src& s,bool clear) {
		int sz = GetSystemMetrics(SM_CXSMICON);
		if (sz <= 0 || !ensureDib(sz)) return nullptr;
		wchar_t t[8] = L"--";
		if (s.part != NA && s.total != NA && s.total) {
			unsigned pct = (unsigned)((double)s.part * 100.0 / (double)s.total + 0.5);
			wsprintfW(t,L"%u",pct > 999 ? 999 : pct);
		}
		// below 24 px a frame leaves 100 no room to be read, that draw goes without it
		bool frame = cfg::getb("trayBorder",false) && (sz >= 24 || lstrlenW(t) < 3);
		HFONT f = fontFor(sz,t,frame);
		uint32_t fg = s.col & 0xFFFFFFu,bg = 0,lc = 0;
		cfg::getColor("trayColText",fg);
		bool own = cfg::getColor("trayColBg",bg);
		bool noBg = cfg::getb("trayNoBg",false) || (clear && !own);
		if (lvlCol(s,lc)) {
			if (cfg::getb("trayLvlBg",false)) {
				bg = lc;
				noBg = false;
			} else fg = lc;
		}
		int rr = cfg::getb("trayRound",false) ? sz / 4 : 0;
		auto in = [&](int x,int y) {
			if (x < 0 || y < 0 || x >= sz || y >= sz) return false;
			int cx = x < rr ? rr : x >= sz - rr ? sz - 1 - rr : x;
			int cy = y < rr ? rr : y >= sz - rr ? sz - 1 - rr : y;
			return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rr * rr;
		};
		for (int y = 0; y < sz; y++) for (int x = 0; x < sz; x++) g_bits[y * sz + x] = !noBg && in(x,y) ? bg : 0;
		HGDIOBJ oldB = SelectObject(g_dc,g_bmp);
		HFONT oldF = f ? (HFONT)SelectObject(g_dc,f) : nullptr;
		SetBkMode(g_dc,TRANSPARENT);
		uint32_t tc = noBg ? 0xFFFFFFu : fg;
		SetTextColor(g_dc,RGB((tc >> 16) & 255,(tc >> 8) & 255,tc & 255));
		RECT r = {0,0,sz,sz};
		DrawTextW(g_dc,t,-1,&r,DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		if (oldF) SelectObject(g_dc,oldF);
		SelectObject(g_dc,oldB);
		GdiFlush();
		for (int y = 0; y < sz; y++) for (int x = 0; x < sz; x++) {
			uint32_t& p = g_bits[y * sz + x];
			bool inside = in(x,y);
			if (frame && inside && (!in(x - 1,y) || !in(x + 1,y) || !in(x,y - 1) || !in(x,y + 1))) p = 0xFF000000u | fg;
			else if (!noBg) p = inside ? p | 0xFF000000u : 0;
			else {
				uint32_t a = (p >> 8) & 255;
				p = a ? a << 24 | fg : 0;
			}
		}
		if (noBg) keepAlpha();
		return toIcon();
	}

	// too long : whole lines go, a number cut short would read as a wrong one
	void copyTip(wchar_t* to,int cap,const std::string& s) {
		int n = MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);
		std::wstring w(n > 1 ? n : 1,L'\0');
		if (n > 1) MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,&w[0],n);
		w.resize(n > 1 ? n - 1 : 0);
		if ((int)w.size() >= cap) {
			size_t p = w.rfind(L'\n',cap - 1);
			w.resize(p == std::wstring::npos ? cap - 1 : p);
		}
		lstrcpynW(to,w.c_str(),cap);
	}

	std::string memLine(const char* name,uint64_t part,uint64_t total) {
		return std::string(name) + " : " + surf::sizeStr(part) + " (" + surf::pctStr(part,total) + ")";
	}

	// szTip holds 128 chars, the icon's own series goes first
	std::string tipText(const Src& s,int kind,const SysMem& m) {
		std::string a = memLine("Physical memory",m.physUsed,m.physTotal);
		std::string b = memLine("Commit",m.commit,m.commitLimit);
		if (kind == KGpu) return memLine("GPU memory",s.part,s.total) + "\n" + a + "\n" + b;
		std::string c = memLine("Available",m.physAvail,m.physTotal);
		return (kind == KCommit ? b + "\n" + a : a + "\n" + b) + "\n" + c;
	}

	void updIcon(HWND w,int i,const SysMem& m,const SysHist& h) {
		const IconDef& d = kIcons[i];
		NOTIFYICONDATAW nd = {};
		nd.cbSize = sizeof(nd);
		nd.hWnd = w;
		nd.uID = (UINT)i + 1;
		if (!cfg::getb(d.key,false)) {
			if (g_added[i]) {
				Shell_NotifyIconW(NIM_DELETE,&nd);
				g_added[i] = false;
				g_up--;
			}
			return;
		}
		bool clear = cfg::getb("trayTransparent",false);
		Src s = srcOf(d.kind,m,h);
		HICON ic = d.text ? textIcon(s,clear) : graphIcon(s,clear,true);
		if (!ic) return;
		nd.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
		nd.uCallbackMessage = WM_TRAY;
		nd.hIcon = ic;
		copyTip(nd.szTip,ARRAYSIZE(nd.szTip),tipText(s,d.kind,m));
		// modify fails when Explorer restarted and forgot the icon, that also is the first add. Cheaper than TaskbarCreated.
		bool ok = Shell_NotifyIconW(NIM_MODIFY,&nd) != FALSE;
		if (!ok) {
			ok = Shell_NotifyIconW(NIM_ADD,&nd) != FALSE;
			if (ok) {
				nd.uVersion = NOTIFYICON_VERSION_4;
				Shell_NotifyIconW(NIM_SETVERSION,&nd);
				appLog(std::string("Tray icon ") + d.name + " : OK");
			} else if (!g_failLog) {
				g_failLog = true;
				appLog(std::string("Tray icon ") + d.name + " : failed (the shell refused it, is Explorer running)");
			}
		}
		if (ok && !g_added[i]) g_up++;
		if (!ok && g_added[i]) g_up--;
		g_added[i] = ok;
		DestroyIcon(ic);
	}

	void dropTaskbar() {
		if (!g_tb) return;
		g_tb->Release();
		g_tb = nullptr;
	}

	void clearTaskbar() {
		if (!g_tb) return;
		if (g_ovlOn) g_tb->SetOverlayIcon(g_main,nullptr,nullptr);
		if (g_progOn) g_tb->SetProgressState(g_main,TBPF_NOPROGRESS);
		g_ovlOn = false;
		g_progOn = false;
	}

	bool getTaskbar() {
		if (g_tb) return true;
		HRESULT hr = CoCreateInstance(CLSID_TaskbarList,nullptr,CLSCTX_INPROC_SERVER,IID_ITaskbarList3,(void**)&g_tb);
		if (SUCCEEDED(hr)) hr = g_tb->HrInit();
		if (SUCCEEDED(hr)) return true;
		dropTaskbar();
		if (!g_tbFailLog) {
			g_tbFailLog = true;
			char b[64];
			wsprintfA(b,"Taskbar overlay : failed (0x%08lX)",(unsigned long)hr);
			appLog(b);
		}
		return false;
	}

	void updTaskbar(const SysMem& m,const SysHist& h) {
		int mode = cfg::geti("taskOverlay",0);
		bool prog = cfg::getb("taskProgress",false);
		if (!mode && !prog) {
			clearTaskbar();
			dropTaskbar();
			return;
		}
		if (!getTaskbar()) return;
		HRESULT hr = S_OK;
		if (mode == 1 || mode == 2) {
			bool commit = mode == 2;
			HICON ic = graphIcon(srcOf(commit ? KCommit : KPhys,m,h),false,false);
			if (ic) {
				hr = g_tb->SetOverlayIcon(g_main,ic,commit ? L"Commit charge history" : L"Physical memory history");
				g_ovlOn = true;
				DestroyIcon(ic);
			}
		} else if (g_ovlOn) {
			g_tb->SetOverlayIcon(g_main,nullptr,nullptr);
			g_ovlOn = false;
		}
		if (prog) {
			bool commit = cfg::getb("sumCommit",true);
			uint64_t part = commit ? m.commit : m.physUsed;
			uint64_t total = commit ? m.commitLimit : m.physTotal;
			g_progOn = true;
			if (appCleaning() || sysmem::busy()) {
				g_tb->SetProgressState(g_main,TBPF_INDETERMINATE);
			} else if (part == NA || total == NA || !total) {
				g_tb->SetProgressState(g_main,TBPF_NOPROGRESS);
			} else {
				double p = (double)part * 100.0 / (double)total;
				g_tb->SetProgressState(g_main,p >= cfg::geti("hlBad",90) ? TBPF_ERROR : p >= cfg::geti("hlWarn",80) ? TBPF_PAUSED : TBPF_NORMAL);
				g_tb->SetProgressValue(g_main,part,total);
			}
		} else if (g_progOn) {
			g_tb->SetProgressState(g_main,TBPF_NOPROGRESS);
			g_progOn = false;
		}
		// a failed call means Explorer went away, a new object next tick picks up the new taskbar. Hidden has no button.
		if (FAILED(hr) && IsWindowVisible(g_main)) {
			dropTaskbar();
			g_ovlOn = false;
			g_progOn = false;
		}
	}

	void tick(HWND w) {
		if (g_stop.load() || !wanted()) {
			PostQuitMessage(0);
			return;
		}
		UINT ms = (UINT)std::max(500,sampler::intervalMs());
		if (ms != g_tickMs) {
			g_tickMs = ms;
			SetTimer(w,TimerTick,ms,nullptr);
		}
		auto m = sysmem::latest();
		if (!m->t) return;
		auto h = sysmem::history();
		for (int i = 0; i < IcoN; i++) updIcon(w,i,*m,*h);
		updTaskbar(*m,*h);
	}

	// the clean summary on the first icon that is up, one every 30 s, the rest only in the log
	void showNote(HWND w) {
		NOTIFYICONDATAW nd = {};
		{
			std::lock_guard<std::mutex> lk(g_noteM);
			copyTip(nd.szInfoTitle,ARRAYSIZE(nd.szInfoTitle),g_noteTitle);
			copyTip(nd.szInfo,ARRAYSIZE(nd.szInfo),g_noteText);
			nd.dwInfoFlags = g_noteWarn ? NIIF_WARNING : NIIF_INFO;
		}
		uint64_t now = util::nowMs();
		if (g_noteAt && now - g_noteAt < 30000) return;
		int i = 0;
		while (i < IcoN && !g_added[i]) i++;
		if (i == IcoN) return;
		nd.cbSize = sizeof(nd);
		nd.hWnd = w;
		nd.uID = (UINT)i + 1;
		nd.uFlags = NIF_INFO;
		if (!cfg::getb("trayNotifySound",true)) nd.dwInfoFlags |= NIIF_NOSOUND;
		if (Shell_NotifyIconW(NIM_MODIFY,&nd)) g_noteAt = now;
	}

	// presets plus the current value when it is none of them, then off (0), as one radio group
	void radios(HMENU m,UINT base,std::vector<int> v,int cur,const wchar_t* fmt,const wchar_t* off) {
		if (cur && std::find(v.begin(),v.end(),cur) == v.end()) {
			v.push_back(cur);
			std::sort(v.begin(),v.end());
		}
		v.push_back(0);
		int first = GetMenuItemCount(m);
		int pick = first;
		for (int i = 0; i < (int)v.size(); i++) {
			wchar_t t[48];
			if (v[i]) wsprintfW(t,fmt,v[i]);
			else lstrcpynW(t,off,48);
			AppendMenuW(m,MF_STRING,base + v[i],t);
			if (v[i] == cur) pick = first + i;
		}
		CheckMenuRadioItem(m,first,first + (int)v.size() - 1,pick,MF_BYPOSITION);
	}

	// state from appCleanInfo, never App itself, this runs on the tray thread
	HMENU makeMenu() {
		CleanInfo ci = appCleanInfo();
		bool busy = appCleaning();
		UINT fl = busy ? MF_GRAYED : MF_ENABLED;
		HMENU cl = CreatePopupMenu();
		AppendMenuW(cl,fl,CmdBase + ActFlush,L"Flush Standby");
		AppendMenuW(cl,fl,CmdBase + ActEmpty,L"Empty Working Sets");
		AppendMenuW(cl,fl,CmdBase + ActPurge,L"Purge Standby");
		AppendMenuW(cl,fl,CmdBase + ActModified,L"Flush Modified");
		AppendMenuW(cl,MF_SEPARATOR,0,nullptr);
		AppendMenuW(cl,fl,CmdBase + ActFull,L"Full Clean  (all methods)");
		AppendMenuW(cl,busy || !ci.set ? MF_GRAYED : MF_ENABLED,CmdBase + ActSet,L"Clean set");
		HMENU ar = CreatePopupMenu();
		for (int i = 0; i < 9; i++) {
			if (i == 4) AppendMenuW(ar,MF_SEPARATOR,0,nullptr);
			AppendMenuW(ar,ci.set & (1 << i) ? MF_CHECKED : MF_UNCHECKED,CmdArea + i,kSetNames[i]);
		}
		HMENU au = CreatePopupMenu();
		radios(au,CmdAbove,{50,60,70,80,90},ci.autoOn ? ci.thr : 0,L"Above %d%%",L"No threshold clean");
		AppendMenuW(au,MF_SEPARATOR,0,nullptr);
		radios(au,CmdEvery,{10,15,30,60,120},ci.every ? ci.everyMin : 0,L"Every %d min",L"No timed clean");
		HMENU m = CreatePopupMenu();
		AppendMenuW(m,MF_ENABLED,CmdBase + ActShowHide,IsWindowVisible(g_main) && !IsIconic(g_main) ? L"Hide FRS" : L"Show FRS");
		AppendMenuW(m,MF_ENABLED,CmdBase + ActMemory,L"Memory");
		AppendMenuW(m,MF_POPUP,(UINT_PTR)cl,L"Clean");
		AppendMenuW(m,MF_POPUP,(UINT_PTR)ar,L"Clean set areas");
		AppendMenuW(m,MF_POPUP,(UINT_PTR)au,L"Auto-clean");
		AppendMenuW(m,MF_SEPARATOR,0,nullptr);
		AppendMenuW(m,MF_ENABLED,CmdBase + ActSettings,L"Settings");
		AppendMenuW(m,MF_ENABLED,CmdBase + ActExit,L"Exit");
		SetMenuDefaultItem(m,CmdBase + ActShowHide,FALSE);
		return m;
	}

	// the set and the timer change from here, cfg is thread safe, App fields go through the UI thread
	void onCmd(UINT id) {
		if (id >= CmdBase && id < CmdBase + ActN) {
			act(id - CmdBase);
		} else if (id >= CmdArea && id < CmdArea + 9) {
			int i = id - CmdArea;
			bool on = !(appCleanInfo().set & (1 << i));
			mcp::post([i,on] { appSetMethod(i,on); });
		} else if (id >= CmdAbove && id <= CmdAbove + 100) {
			int t = id - CmdAbove;
			mcp::post([t] { appSetAuto(t); });
		} else if (id >= CmdEvery && id <= CmdEvery + 1440) {
			if (id > CmdEvery) cfg::seti("acEveryMin",id - CmdEvery);
			cfg::setb("acEvery",id > CmdEvery);
		}
	}

	int miniAct(int icon) {
		int k = kIcons[icon].kind;
		return k == KCommit ? ActMiniCommit : k == KGpu ? ActGpu : ActMiniPhys;
	}

	// the owner must be foreground or the menu won't go away when clicking elsewhere (SI mainwnd.c:5297).
	// wp of a version 4 icon is the anchor : the click, or the icon when the keyboard opened the menu
	void showMenu(HWND w,WPARAM wp) {
		if (g_inMenu) return;
		g_inMenu = true;
		HMENU m = makeMenu();
		if (!noActivate()) SetForegroundWindow(w);
		TrackPopupMenuEx(m,TPM_RIGHTBUTTON,(short)LOWORD(wp),(short)HIWORD(wp),w,nullptr);
		PostMessageW(w,WM_NULL,0,0);
		DestroyMenu(m);
		g_inMenu = false;
	}

	// UIPI lets any program post WM_TRAY to an elevated FRS, a real click has the cursor on the icon.
	// An icon hidden in the overflow gets the chevron's rect, its click comes from the shell's open flyout (Windows 11, Windows 10)
	bool onIcon(HWND w,int icon) {
		NOTIFYICONIDENTIFIER ni = {};
		ni.cbSize = sizeof(ni);
		ni.hWnd = w;
		ni.uID = (UINT)icon + 1;
		RECT r;
		POINT p;
		if (!GetCursorPos(&p)) return false;
		if (SUCCEEDED(Shell_NotifyIconGetRect(&ni,&r))) {
			InflateRect(&r,2,2);
			if (PtInRect(&r,p)) return true;
		}
		HWND f = GetAncestor(WindowFromPoint(p),GA_ROOT);
		HWND t = FindWindowW(L"Shell_TrayWnd",nullptr);
		wchar_t cls[64] = {};
		if (!f || !t || !GetClassNameW(f,cls,64)) return false;
		if (lstrcmpW(cls,L"TopLevelWindowForOverflowXamlIsland") && lstrcmpW(cls,L"NotifyIconOverflowWindow")) return false;
		DWORD fp = 0,sp = 0;
		GetWindowThreadProcessId(f,&fp);
		GetWindowThreadProcessId(t,&sp);
		return fp && fp == sp;
	}

	// the single click action waits out the double click time, like SI notifico.c:572
	void onTray(HWND w,WPARAM wp,LPARAM lp) {
		int icon = (int)HIWORD(lp) - 1;
		if (icon < 0 || icon >= IcoN) return;
		bool single = cfg::getb("traySingleClick",false);
		switch (LOWORD(lp)) {
			case WM_LBUTTONDOWN:
				if (single) act(ActToggle);
				else g_clickUp = true;
				break;
			case WM_LBUTTONUP:
				if (!single && g_clickUp) {
					g_clickIcon = icon;
					SetTimer(w,TimerClick,GetDoubleClickTime() + 140,nullptr);
				}
				g_clickUp = false;
				break;
			case WM_LBUTTONDBLCLK:
				if (!single) {
					KillTimer(w,TimerClick);
					g_clickUp = false;
					act(ActToggle);
				}
				break;
			case WM_MBUTTONUP: {
				int a = cfg::geti("trayMidAct",0);
				if ((a == 2 || a == 3) && !onIcon(w,icon)) break;
				if (a == 1) act(ActShowHide);
				else if (a == 2) act(ActSet);
				else if (a == 3) act(ActFull);
				else if (a == 4) act(miniAct(icon));
				break;
			}
			case WM_CONTEXTMENU:
				KillTimer(w,TimerClick);
				showMenu(w,wp);
				break;
			// Enter on the icon brings a covered FRS forward, never hides it
			case NIN_KEYSELECT:
				act(ActShow);
				break;
		}
	}

	LRESULT CALLBACK trayProc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
		switch (msg) {
			case WM_TIMER:
				if (wp == TimerTick) {
					tick(w);
				} else if (wp == TimerClick) {
					KillTimer(w,TimerClick);
					act(miniAct(g_clickIcon));
				}
				return 0;
			case WM_TRAY:
				onTray(w,wp,lp);
				return 0;
			case WM_COMMAND:
				onCmd(LOWORD(wp));
				return 0;
			case WM_NOTE:
				showNote(w);
				return 0;
			case WM_STOP:
				EndMenu();
				PostQuitMessage(0);
				return 0;
		}
		return DefWindowProcW(w,msg,wp,lp);
	}

	void shellThread() {
		// ImGui makes only the UI thread DPI aware. Windows 10 and up, older ones skip it and get the scaled icon.
		using DpiFn = HANDLE (WINAPI*)(HANDLE);
		if (auto fn = (DpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetThreadDpiAwarenessContext")) fn((HANDLE)-4);
		HRESULT co = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
		HINSTANCE hi = GetModuleHandleW(nullptr);
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = trayProc;
		wc.hInstance = hi;
		wc.lpszClassName = L"FRS_Tray";
		RegisterClassExW(&wc);
		// a real hidden window, a message only one can't be foreground for the menu
		HWND w = CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"FRS tray",WS_POPUP,0,0,0,0,nullptr,nullptr,hi,nullptr);
		if (w) {
			g_tw = w;
			ChangeWindowMessageFilterEx(w,WM_TRAY,MSGFLT_ALLOW,nullptr); // FRS runs elevated, Explorer does not
			tick(w);
			MSG msg;
			while (GetMessageW(&msg,nullptr,0,0) > 0) {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			g_tw = nullptr;
			for (int i = 0; i < IcoN; i++) {
				if (!g_added[i]) continue;
				NOTIFYICONDATAW nd = {};
				nd.cbSize = sizeof(nd);
				nd.hWnd = w;
				nd.uID = (UINT)i + 1;
				Shell_NotifyIconW(NIM_DELETE,&nd);
				g_added[i] = false;
			}
			g_up = 0;
			DestroyWindow(w);
		} else {
			appLog("Tray : failed (no window for the icons)");
		}
		clearTaskbar();
		dropTaskbar();
		UnregisterClassW(wc.lpszClassName,hi);
		freeDib();
		for (int i = 0; i < 2; i++) {
			if (g_font[i]) DeleteObject(g_font[i]);
			g_font[i] = nullptr;
			g_fontKey[i] = -1;
		}
		g_tickMs = 0;
		g_failLog = false;
		g_tbFailLog = false;
		if (SUCCEEDED(co)) CoUninitialize();
		g_state = 2;
	}

	void hideMain() {
		ShowWindow(g_main,SW_HIDE);
	}

	void showMain() {
		bool na = noActivate();
		if (IsIconic(g_main)) ShowWindow(g_main,na ? SW_SHOWNOACTIVATE : SW_RESTORE);
		else ShowWindow(g_main,na ? SW_SHOWNA : SW_SHOW);
		if (!na) SetForegroundWindow(g_main);
	}

	void runClean(bool fs,bool ew,bool ps,bool fm) {
		if (!appRunClean(fs,ew,ps,fm,"[tray] ")) ui::toast("A clean is already running");
	}

	// UI thread. A clean that starts toasts its own summary at the end
	void runSet(const char* pfx) {
		if (!appRunSet(pfx)) ui::toast(appCleaning() ? "A clean is already running" : "The clean set is empty, tick methods in Settings > Auto-Clean");
	}

	std::string keyName(int k) {
		if (k < 12) return "F" + std::to_string(k + 1);
		return std::string(1,k < 38 ? (char)('A' + k - 12) : (char)('0' + k - 38));
	}

	// UI thread, the window's own. One try per combo, toggling the option tries again
	void hotkey() {
		int want = 0;
		if (cfg::getb("hkOn",false)) want = (std::min(std::max(cfg::geti("hkMods",0),0),5) + 1) << 8 | std::min(std::max(cfg::geti("hkKey",11),0),KeyN - 1);
		if (want == g_hkTried) return;
		g_hkTried = want;
		if (g_hkUp) UnregisterHotKey(g_main,HotId);
		g_hkUp = false;
		if (!want) return;
		int md = (want >> 8) - 1;
		int k = want & 255;
		UINT vk = k < 12 ? VK_F1 + k : k < 38 ? 'A' + k - 12 : '0' + k - 38;
		std::string nm = std::string(kMods[md]) + "+" + keyName(k);
		// Ctrl or Alt alone would take Ctrl+C, Alt+F4 and the like from every program
		if (md >= 4 && (k >= 12 || k == 3)) {
			appLog("[!] Hotkey " + nm + " : failed (programs use it, Ctrl or Alt alone only with F1 to F12, not F4)");
			return;
		}
		if (RegisterHotKey(g_main,HotId,kModBits[md] | MOD_NOREPEAT,vk)) {
			g_hkUp = true;
			appLog("Hotkey " + nm + " : OK");
		} else {
			DWORD e = GetLastError();
			appLog("[!] Hotkey " + nm + " : failed (" + (e == ERROR_HOTKEY_ALREADY_REGISTERED ? std::string("another program has it") : "error " + std::to_string(e)) + ")");
		}
	}

	void handle(uint32_t a) {
		auto on = [a](int i) { return (a & (1u << i)) != 0; };
		if (on(ActToggle)) {
			bool vis = IsWindowVisible(g_main) && !IsIconic(g_main);
			if (vis && cfg::getb("trayToggle",true) && anyIcon()) hideMain();
			else showMain();
		}
		if (on(ActShowHide)) {
			if (IsWindowVisible(g_main) && !IsIconic(g_main)) hideMain();
			else showMain();
		}
		if (on(ActShow)) showMain();
		if (on(ActMemory)) {
			nav::go("memory");
			showMain();
		}
		if (on(ActSettings)) {
			nav::go("settings");
			showMain();
		}
		if (on(ActMiniCommit)) {
			surf::openMini(surf::SecCommit);
			showMain();
		}
		if (on(ActMiniPhys)) {
			surf::openMini(surf::SecPhys);
			showMain();
		}
		if (on(ActGpu)) {
			nav::go("gpu");
			showMain();
		}
		if (on(ActFlush)) runClean(true,false,false,false);
		if (on(ActEmpty)) runClean(false,true,false,false);
		if (on(ActPurge)) runClean(false,false,true,false);
		if (on(ActModified)) runClean(false,false,false,true);
		if (on(ActFull)) runClean(true,true,true,true);
		if (on(ActSet)) runSet("[tray] ");
		if (on(ActExit)) {
			g_exit = true;
			PostMessageW(g_main,WM_CLOSE,0,0);
		}
	}

}

namespace surf {

	void setHwnd(void* h) {
		g_main = (HWND)h;
	}

	// any thread, app.cpp calls it at the end of a clean while trayNotify is on. No tray thread, no balloon
	void notify(const std::string& title,const std::string& text,bool warn) {
		HWND w = g_tw;
		if (!w) return;
		{
			std::lock_guard<std::mutex> lk(g_noteM);
			g_noteTitle = title;
			g_noteText = text;
			g_noteWarn = warn;
		}
		PostMessageW(w,WM_NOTE,0,0);
	}

	// the hotkey works with no icon too, so it goes before the early return
	bool winMsg(void* hwnd,unsigned msg,unsigned long long wp,long long lp,long long& res) {
		if ((HWND)hwnd != g_main) return false;
		if (msg == WM_HOTKEY && wp == HotId) {
			runSet("[hotkey] ");
			res = 0;
			return true;
		}
		if (g_exit || g_up.load() <= 0) return false;
		if (msg == WM_CLOSE && cfg::getb("trayClose",false)) {
			hideMain();
			res = 0;
			return true;
		}
		if (msg == WM_SIZE && wp == SIZE_MINIMIZED && cfg::getb("trayMin",false)) g_hideNext = true;
		return false;
	}

	namespace os {

		void opts() {
			const char* g = "Tray and taskbar";
			const char* off = "Off by default. Icons sit next to the clock, right click one for its menu.";
			cfg::optBool("trayPhysHist",g,"Physical memory history icon",off,false);
			cfg::optBool("trayCommitHist",g,"Commit charge history icon","",false);
			cfg::optBool("trayPhysText",g,"Physical usage icon, percent as text","",false);
			cfg::optBool("trayCommitText",g,"Commit usage icon, percent as text","",false);
#ifdef TRAY_GPU
			cfg::optBool("trayGpuHist",g,"GPU memory history icon","Dedicated video memory, not system RAM. Needs Enable GPU monitoring. A GPU without its own memory shows nothing.",false);
			cfg::optBool("trayGpuText",g,"GPU memory icon, percent as text","",false);
#endif
			cfg::optBool("trayTransparent",g,"Transparent icons","The black graph background becomes see through.",false);
			cfg::optColor("trayColText",g,"Percent icon text color","Automatic follows Physical memory color or Commit charge color.","");
			cfg::optColor("trayColBg",g,"Percent icon background","Automatic is black, or see through with Transparent icons.","");
			cfg::optBool("trayNoBg",g,"Percent icons without background","Only the digits show.",false);
			cfg::optEnum("trayFont",g,"Percent icon font","",0,{"Tahoma","Segoe UI","Consolas","Lucida Console","Arial"});
			cfg::optBool("trayAA",g,"Smooth digits","Off draws the digits without antialiasing.",true);
			cfg::optBool("trayBorder",g,"Frame around percent icons","1 px in the text color.",false);
			cfg::optBool("trayRound",g,"Round percent icons","",false);
			cfg::optBool("trayHl",g,"Icons follow the highlight thresholds","At or above the Warning and Critical levels in Memory highlights, the digits or the graph line take the colors below.",false);
			cfg::optBool("trayLvlBg",g,"Thresholds color the background","Percent icons : the background takes the threshold color, the digits keep theirs.",false);
			cfg::optColor("trayColWarn",g,"Icon warning color","Automatic is the theme's orange.","");
			cfg::optColor("trayColBad",g,"Icon critical color","Automatic is the theme's red.","");
			cfg::optBool("traySingleClick",g,"Single click icons","One click toggles the window. Otherwise one click shows the window on Live view at that icon's list and a double click toggles it.",false);
			cfg::optBool("trayToggle",g,"Icon click toggles visibility","A click on the icon also hides FRS when it is showing.",true);
			cfg::optEnum("trayMidAct",g,"Middle click on an icon","Clean set and Full Clean start at once, no prompt.",0,{"Nothing","Show or hide","Clean set","Full Clean","Live view"});
			cfg::optBool("trayClose",g,"Close to tray","The close button hides FRS instead of exiting, Exit is in the icon's menu. Only while an icon is showing.",false);
			cfg::optBool("trayMin",g,"Minimize to tray","Only while an icon is showing.",false);
			cfg::optBool("trayStartHidden",g,"Start hidden","Starts with just the icon. Only with an icon on.",false);
			cfg::optBool("startMin",g,"Start minimized","Starts minimized on the taskbar, no icon needed. With Minimize to tray and an icon it goes to the tray. From the next start.",false);
			cfg::optBool("trayNotify",g,"Notify after a clean","What the clean freed, from the tray icon. Needs an icon, one every 30 s at most.",false);
			cfg::optBool("trayNotifySound",g,"Notification sound","",true);
			cfg::optEnum("taskOverlay",g,"Taskbar overlay","Live graph over the corner of the FRS taskbar button.",0,{"None","Physical memory history","Commit charge history"});
			cfg::optBool("taskProgress",g,"Taskbar progress","The taskbar button fills with memory use, colored by the highlight thresholds. Moves while a clean runs.",false);
			cfg::optBool("uiOnTop",g,"Always on top","Keeps the FRS window above other windows.",false);

			std::vector<std::string> keys;
			for (int i = 0; i < KeyN; i++) keys.push_back(keyName(i));
			cfg::optBool("hkOn","Clean set","Clean hotkey","Runs the clean set from any program, no prompt, also while FRS is hidden. While on, no other program gets this combo, so Ctrl or Alt alone only go with F1 to F12, not F4.",false);
			cfg::optEnum("hkMods","Clean set","Hotkey modifiers","",0,{kMods[0],kMods[1],kMods[2],kMods[3],kMods[4],kMods[5]});
			cfg::optEnum("hkKey","Clean set","Hotkey key","",11,keys);
		}

		void update() {
			uint32_t a = g_act.exchange(0);
			if (a && g_main) handle(a);
			if (g_hideNext.exchange(false) && g_main) hideMain();
			uint64_t now = util::nowMs();
			if (now < g_nextCheck) return;
			g_nextCheck = now + 200;
			if (g_state == 2) {
				g_th.join();
				g_state = 0;
				g_nextStart = now + 1000;
			}
			if (g_state == 0 && wanted() && !g_stop && now >= g_nextStart && g_main) {
				g_state = 1;
				try {
					g_th = std::thread(shellThread);
				} catch (...) {
					g_state = 0;
					g_nextStart = now + 30000;
					appLog("Tray : failed (could not start a thread)");
				}
			}
			if (!g_main) return;
			hotkey();
			// hides once an icon really is up, with Explorer gone there would be no way back. Started minimized with Minimize to tray goes there too
			if (!g_started) {
				bool hide = cfg::getb("trayStartHidden",false) || (cfg::getb("trayMin",false) && IsIconic(g_main));
				if (!hide || !anyIcon()) g_started = true;
				else if (g_up > 0) {
					g_started = true;
					hideMain();
				}
			}
			// settings changed from outside while hidden, with no icon left there would be no way back
			if (!IsWindowVisible(g_main) && !anyIcon()) showMain();
			// the Ask prompt only draws in a frame, hidden or minimized has none and the call would wait out its 60 s unseen
			if ((!IsWindowVisible(g_main) || IsIconic(g_main)) && mcp::askPending()) showMain();
		}

		void shutdown() {
			if (g_hkUp) UnregisterHotKey(g_main,HotId);
			g_hkUp = false;
			g_hkTried = 0;
			g_stop = true;
			if (g_state == 0) return;
			HWND w = g_tw;
			if (w) PostMessageW(w,WM_STOP,0,0);
			for (int i = 0; i < 100 && g_state != 2; i++) Sleep(20);
			// Explorer hung : the process is on its way out, the thread goes with it
			if (g_state == 2) g_th.join();
			else g_th.detach();
			g_state = 0;
		}

		void setOnTop(bool on) {
			// from a process that is not foreground Windows answers ok and still ignores a lone HWND_TOPMOST, top of the normal band first gets it through
			if (!g_main || ((GetWindowLongW(g_main,GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) == on) return;
			SetWindowPos(g_main,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			if (on) SetWindowPos(g_main,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}

		int screenW() {
			return GetSystemMetrics(SM_CXVIRTUALSCREEN);
		}

	}

}
#endif // _WIN32
