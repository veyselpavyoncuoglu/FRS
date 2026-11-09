#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui.h"
#include "cfg.h"
#include "util.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

	float g_dpi = 1.0f;
	ImFont* g_font = nullptr;
	ImFont* g_bold = nullptr;
	ImFont* g_mono = nullptr;
	std::string g_ask;

	struct Toast {
		std::string msg;
		uint64_t t;
	};
	std::mutex g_tm;
	std::vector<Toast> g_toasts;

	ImVec4 rgb(unsigned v,float a = 1.0f) {
		return ImVec4(((v >> 16) & 255) / 255.0f,((v >> 8) & 255) / 255.0f,(v & 255) / 255.0f,a);
	}

	ImVec4 mix(ImVec4 a,ImVec4 b,float t) {
		return ImVec4(a.x + (b.x - a.x) * t,a.y + (b.y - a.y) * t,a.z + (b.z - a.z) * t,1.0f);
	}

	ImVec4 alpha(ImVec4 a,float w) {
		return ImVec4(a.x,a.y,a.z,w);
	}

	ImVec4 light(ImVec4 a,float t) {
		return mix(a,ImVec4(1,1,1,1),t);
	}

	ui::Pal makePal() {
		ui::Pal p;
		p.bg = rgb(0x0F1115);
		p.panel = rgb(0x161922);
		p.panel2 = rgb(0x1C2030);
		p.line = rgb(0x2A2F3E);
		p.text = rgb(0xE6E8EE);
		p.dim = rgb(0x8A90A2);
		p.accent = rgb(0x4CC9F0);
		p.good = rgb(0x3DDC84);
		p.warn = rgb(0xFFB020);
		p.bad = rgb(0xFF5C5C);
		p.info = rgb(0x5B9CFF);
		p.purple = rgb(0xA78BFA);
		p.gold = rgb(0xFFD166);
		p.commit = rgb(0xF2C14E);
		p.phys = rgb(0x4CC9F0);
		p.priv = rgb(0xFF8FA3);
		p.ws = rgb(0x7BD389);
		p.cache = rgb(0x5B9CFF);
		p.kernel = rgb(0xE879F9); // magenta, purple already means Image regions and dedicated GPU memory
		p.standby = rgb(0x3A86FF);
		p.modified = rgb(0xFF9F1C);
		p.freeMem = rgb(0x2EC4B6);
		p.zeroed = rgb(0x6C757D);
		return p;
	}

	void setColors(ImVec4* co) {
		const ui::Pal& c = ui::c;
		ImVec4 clear(0,0,0,0);
		ImVec4 black(0,0,0,1);
		co[ImGuiCol_Text] = c.text;
		co[ImGuiCol_TextDisabled] = c.dim;
		co[ImGuiCol_WindowBg] = c.bg;
		co[ImGuiCol_ChildBg] = clear;
		co[ImGuiCol_PopupBg] = c.panel2;
		co[ImGuiCol_Border] = mix(c.line,c.dim,0.6f); // about 3:1 on a card, frames need it. Cards push their own softer one.
		co[ImGuiCol_BorderShadow] = clear;
		co[ImGuiCol_FrameBg] = c.panel2;
		co[ImGuiCol_FrameBgHovered] = light(c.panel2,0.06f);
		co[ImGuiCol_FrameBgActive] = mix(c.panel2,c.accent,0.18f);
		co[ImGuiCol_TitleBg] = c.panel;
		co[ImGuiCol_TitleBgActive] = c.panel2;
		co[ImGuiCol_TitleBgCollapsed] = c.panel;
		co[ImGuiCol_MenuBarBg] = c.panel;
		co[ImGuiCol_ScrollbarBg] = clear;
		co[ImGuiCol_ScrollbarGrab] = mix(c.line,c.dim,0.6f); // about 3:1 on a card, it can be the only sign of more content
		co[ImGuiCol_ScrollbarGrabHovered] = mix(c.line,c.dim,0.8f);
		co[ImGuiCol_ScrollbarGrabActive] = c.dim;
		co[ImGuiCol_CheckMark] = c.accent;
		co[ImGuiCol_SliderGrab] = c.accent;
		co[ImGuiCol_SliderGrabActive] = light(c.accent,0.25f);
		co[ImGuiCol_Button] = mix(c.panel2,c.line,0.8f);
		co[ImGuiCol_ButtonHovered] = mix(c.line,c.accent,0.22f);
		co[ImGuiCol_ButtonActive] = mix(c.line,c.accent,0.42f);
		co[ImGuiCol_Header] = mix(c.panel2,c.line,0.6f);
		co[ImGuiCol_HeaderHovered] = mix(c.panel2,c.accent,0.18f);
		co[ImGuiCol_HeaderActive] = mix(c.panel2,c.accent,0.3f);
		co[ImGuiCol_Separator] = c.line;
		co[ImGuiCol_SeparatorHovered] = alpha(c.accent,0.6f);
		co[ImGuiCol_SeparatorActive] = c.accent;
		co[ImGuiCol_ResizeGrip] = alpha(c.line,0.6f);
		co[ImGuiCol_ResizeGripHovered] = alpha(c.accent,0.6f);
		co[ImGuiCol_ResizeGripActive] = c.accent;
		co[ImGuiCol_InputTextCursor] = c.accent;
		co[ImGuiCol_TabHovered] = mix(c.panel2,c.accent,0.25f);
		co[ImGuiCol_Tab] = c.panel;
		co[ImGuiCol_TabSelected] = mix(c.panel2,c.accent,0.14f);
		co[ImGuiCol_TabSelectedOverline] = c.accent;
		co[ImGuiCol_TabDimmed] = c.panel;
		co[ImGuiCol_TabDimmedSelected] = c.panel2;
		co[ImGuiCol_TabDimmedSelectedOverline] = c.line;
		co[ImGuiCol_PlotLines] = c.accent;
		co[ImGuiCol_PlotLinesHovered] = c.warn;
		co[ImGuiCol_PlotHistogram] = c.accent;
		co[ImGuiCol_PlotHistogramHovered] = c.warn;
		co[ImGuiCol_TableHeaderBg] = c.panel2;
		co[ImGuiCol_TableBorderStrong] = c.line;
		co[ImGuiCol_TableBorderLight] = alpha(c.line,0.6f);
		co[ImGuiCol_TableRowBg] = clear;
		co[ImGuiCol_TableRowBgAlt] = ImVec4(1,1,1,0.03f);
		co[ImGuiCol_TextLink] = c.accent;
		co[ImGuiCol_TextSelectedBg] = alpha(c.accent,0.3f);
		co[ImGuiCol_TreeLines] = c.line;
		co[ImGuiCol_DragDropTarget] = c.accent;
		co[ImGuiCol_DragDropTargetBg] = clear;
		co[ImGuiCol_UnsavedMarker] = c.text;
		co[ImGuiCol_NavCursor] = c.accent;
		co[ImGuiCol_NavWindowingHighlight] = alpha(c.text,0.7f);
		co[ImGuiCol_NavWindowingDimBg] = alpha(black,0.45f);
		co[ImGuiCol_ModalWindowDimBg] = alpha(black,0.55f);
	}

	ImFont* tryFont(ImFontAtlas* a,const std::vector<std::string>& paths,float dpi) {
		ImFontConfig fc;
		fc.Flags |= ImFontFlags_NoLoadError;
		if (dpi < 1.25f) { // thin and soft at 100 %, 150 % and up are crisp as they are
			fc.RasterizerMultiply = 1.15f;
			fc.PixelSnapH = true;
		}
		for (auto& p : paths) if (ImFont* f = a->AddFontFromFileTTF(p.c_str(),0.0f,&fc)) return f;
		return nullptr;
	}

	std::vector<std::string> fontPaths(const std::vector<const char*>& names) {
		std::vector<std::string> out;
#ifdef _WIN32
		const char* w = getenv("WINDIR");
		std::string base = std::string(w && *w ? w : "C:\\Windows") + "\\Fonts\\";
		for (auto n : names) out.push_back(base + n);
#else
		// distros disagree on the folder per font
		const char* dirs[] = {
			"/usr/share/fonts/truetype/dejavu/","/usr/share/fonts/truetype/noto/","/usr/share/fonts/truetype/liberation/",
			"/usr/share/fonts/truetype/liberation2/","/usr/share/fonts/truetype/","/usr/share/fonts/TTF/",
			"/usr/share/fonts/dejavu/","/usr/share/fonts/dejavu-sans-fonts/","/usr/share/fonts/dejavu-sans-mono-fonts/",
			"/usr/share/fonts/noto/","/usr/share/fonts/google-noto/","/usr/share/fonts/liberation/","/usr/share/fonts/"
		};
		for (auto n : names) for (auto d : dirs) out.push_back(std::string(d) + n);
#endif
		return out;
	}

#ifndef _WIN32
	// fontconfig finds fonts outside those folders (NixOS, a variable Noto only), asked when none of them has one
	std::vector<std::string> fcMatch(const char* pat) {
		std::string r;
		if (FILE* f = popen((std::string("fc-match -f '%{file}' '") + pat + "' 2>/dev/null").c_str(),"r")) {
			char b[512];
			size_t n;
			while ((n = fread(b,1,sizeof(b),f)) > 0) r.append(b,n);
			pclose(f);
		}
		return {r};
	}
#endif

	ImU32 col32(ImVec4 v) {
		return ImGui::GetColorU32(v);
	}

	// polyline + gradient area for one series, newest sample on the right edge. Several samples per pixel keep their peak.
	void drawSeries(ImDrawList* dl,const float* v,int n,int maxN,float maxv,ImVec2 a,ImVec2 b,ImVec4 col,float thick) {
		if (!v || n <= 0) return;
		static std::vector<ImVec2> pts;
		static std::vector<char> gap; // a bucket with only unknown samples (negative) breaks the line
		float w = b.x - a.x;
		float h = b.y - a.y;
		float step = maxN > 1 ? w / (float)(maxN - 1) : w;
		int m = std::min(n,std::max(1,(int)w + 1));
		pts.resize(m);
		gap.resize(m);
		for (int j = 0; j < m; j++) {
			int i0 = (int)((int64_t)j * n / m);
			int i1 = std::max(i0 + 1,(int)((int64_t)(j + 1) * n / m));
			float pk = -1.0f;
			for (int i = i0; i < i1 && i < n; i++) if (v[i] > pk) pk = v[i];
			gap[j] = pk < 0.0f;
			float x = (float)(i0 + i1 - 1) * 0.5f;
			pts[j] = ImVec2(b.x - ((float)(n - 1) - x) * step,b.y - ImClamp(pk / maxv,0.0f,1.0f) * (h - 2.0f) - 1.0f);
		}
		if (m == 1) {
			pts.push_back(ImVec2(b.x,pts[0].y));
			gap.push_back(gap[0]);
			pts[0].x = a.x;
			m = 2;
		}
		ImU32 cTop = col32(alpha(col,0.26f));
		ImU32 cBot = col32(alpha(col,0.02f));
		ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
		for (int j = 0; j + 1 < m; j++) {
			if (gap[j] || gap[j + 1]) continue;
			dl->PrimReserve(6,4);
			ImDrawIdx i = (ImDrawIdx)dl->_VtxCurrentIdx;
			dl->PrimWriteVtx(pts[j],uv,cTop);
			dl->PrimWriteVtx(pts[j + 1],uv,cTop);
			dl->PrimWriteVtx(ImVec2(pts[j + 1].x,b.y),uv,cBot);
			dl->PrimWriteVtx(ImVec2(pts[j].x,b.y),uv,cBot);
			dl->PrimWriteIdx(i);
			dl->PrimWriteIdx((ImDrawIdx)(i + 1));
			dl->PrimWriteIdx((ImDrawIdx)(i + 2));
			dl->PrimWriteIdx(i);
			dl->PrimWriteIdx((ImDrawIdx)(i + 2));
			dl->PrimWriteIdx((ImDrawIdx)(i + 3));
		}
		for (int j = 0; j < m;) {
			if (gap[j]) { j++; continue; }
			int e = j;
			while (e + 1 < m && !gap[e + 1]) e++;
			if (e > j) dl->AddPolyline(&pts[j],e - j + 1,col32(col),ImDrawFlags_None,thick);
			j = e + 1;
		}
	}

	float seriesMax(const ui::Series* s,int ns) {
		float m = 0.0f;
		for (int i = 0; i < ns; i++) if (s[i].v) for (int k = 0; k < s[i].n; k++) if (s[i].v[k] > m) m = s[i].v[k];
		return m > 0.0f ? m * 1.1f : 1.0f;
	}

	ImVec2 itemSize(ImVec2 size,float defY) {
		ImVec2 av = ImGui::GetContentRegionAvail();
		float x = size.x > 0 ? ui::px(size.x) : av.x + ui::px(size.x);
		float y = size.y > 0 ? ui::px(size.y) : size.y < 0 ? av.y + ui::px(size.y) : ui::px(defY);
		return ImVec2(std::max(x,1.0f),std::max(y,1.0f));
	}

}

namespace ui {

	Pal c = makePal();

	void applyTheme(float dpi) {
		g_dpi = dpi > 0.0f ? dpi : 1.0f;
		ImGuiStyle& st = ImGui::GetStyle();
		st = ImGuiStyle();
		ImGui::StyleColorsDark(&st);
		setColors(st.Colors);
		st.WindowRounding = 8.0f;
		st.ChildRounding = 8.0f;
		st.FrameRounding = 5.0f;
		st.GrabRounding = 5.0f;
		st.TabRounding = 5.0f;
		st.PopupRounding = 6.0f;
		st.ScrollbarRounding = 8.0f;
		st.WindowPadding = ImVec2(12.0f,10.0f);
		st.FramePadding = ImVec2(8.0f,3.0f);
		st.ItemSpacing = ImVec2(8.0f,6.0f);
		st.ScrollbarSize = 12.0f;
		st.WindowBorderSize = 1.0f;
		st.ChildBorderSize = 1.0f;
		st.PopupBorderSize = 1.0f;
		st.FrameBorderSize = 1.0f;
		st.ScaleAllSizes(g_dpi);
		st.FontScaleDpi = g_dpi;
		st.FontSizeBase = 15.0f;
	}

	void loadFonts(float dpi) {
		static bool done = false;
		if (done) return;
		done = true;
		ImFontAtlas* a = ImGui::GetIO().Fonts;
#ifdef _WIN32
		g_font = tryFont(a,fontPaths({"segoeui.ttf"}),dpi);
		if (!g_font) g_font = a->AddFontDefault();
		g_bold = tryFont(a,fontPaths({"seguisb.ttf","segoeuib.ttf"}),dpi);
		g_mono = tryFont(a,fontPaths({"consola.ttf"}),dpi);
#else
		g_font = tryFont(a,fontPaths({"DejaVuSans.ttf","NotoSans-Regular.ttf","LiberationSans-Regular.ttf"}),dpi);
		if (!g_font) g_font = tryFont(a,fcMatch("sans-serif:style=Regular"),dpi);
		if (!g_font) g_font = a->AddFontDefault();
		g_bold = tryFont(a,fontPaths({"DejaVuSans-Bold.ttf","NotoSans-Bold.ttf","LiberationSans-Bold.ttf"}),dpi);
		if (!g_bold) g_bold = tryFont(a,fcMatch("sans-serif:style=Bold"),dpi);
		g_mono = tryFont(a,fontPaths({"DejaVuSansMono.ttf","NotoSansMono-Regular.ttf","LiberationMono-Regular.ttf"}),dpi);
		if (!g_mono) g_mono = tryFont(a,fcMatch("monospace:style=Regular"),dpi);
#endif
		if (!g_bold) g_bold = g_font;
		if (!g_mono) g_mono = g_font;
	}

	float px(float v) {
		return v * g_dpi;
	}

	// Consolas and DejaVu Mono look about 10 % bigger than the body font at the same size
	void pushMono() {
		ImGui::PushFont(g_mono,ImGui::GetStyle().FontSizeBase * 0.9f);
	}

	// the smaller mono font drawn from the row top sits higher than the text beside it
	void pushMonoCell() {
		float asc = ImGui::GetFontBaked()->Ascent;
		float h = ImGui::GetTextLineHeight();
		pushMono();
		float dy = std::min(asc - ImGui::GetFontBaked()->Ascent,h - ImGui::GetTextLineHeight());
		if (dy >= 1.0f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor(dy));
	}

	void pushBold() {
		ImGui::PushFont(g_bold,0.0f);
	}

	void pushBig() {
		ImGui::PushFont(g_bold,ImGui::GetStyle().FontSizeBase * 1.6f);
	}

	void popFont() {
		ImGui::PopFont();
	}

	void title(const char* txt) {
		ImGui::PushFont(g_bold,ImGui::GetStyle().FontSizeBase * 1.25f);
		ImGui::TextUnformatted(txt);
		ImGui::PopFont();
	}

	float titleW(const char* txt) {
		ImGui::PushFont(g_bold,ImGui::GetStyle().FontSizeBase * 1.25f);
		float w = ImGui::CalcTextSize(txt).x;
		ImGui::PopFont();
		return w;
	}

	void icon(Icon i,ImVec2 pos,float sz,ImU32 col) {
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 o(std::floor(pos.x),std::floor(pos.y));
		float s = sz;
		float th = std::max(1.0f,s * 0.09f);
		auto P = [&](float x,float y) { return ImVec2(o.x + x * s,o.y + y * s); };
		auto line = [&](float x0,float y0,float x1,float y1,float t = 0.0f) { dl->AddLine(P(x0,y0),P(x1,y1),col,t > 0.0f ? t : th); };
		auto box = [&](float x0,float y0,float x1,float y1,float r) { dl->AddRect(P(x0,y0),P(x1,y1),col,r * s,0,th); };
		auto fill = [&](float x0,float y0,float x1,float y1,float r) { dl->AddRectFilled(P(x0,y0),P(x1,y1),col,r * s); };
		auto star = [&](float cx,float cy,float r,float in,int tips,bool solid) {
			ImVec2 p[24];
			int n = tips * 2;
			for (int k = 0; k < n; k++) {
				float a = -IM_PI * 0.5f + (float)k * IM_PI / (float)tips;
				float rr = k % 2 ? in : r;
				p[k] = P(cx + std::cos(a) * rr,cy + std::sin(a) * rr);
			}
			if (solid) dl->AddConcavePolyFilled(p,n,col);
			else dl->AddPolyline(p,n,col,ImDrawFlags_Closed,th);
		};
		switch (i) {
			case IcoDash:
				box(0.06f,0.06f,0.42f,0.42f,0.09f);
				box(0.58f,0.06f,0.94f,0.42f,0.09f);
				box(0.06f,0.58f,0.42f,0.94f,0.09f);
				box(0.58f,0.58f,0.94f,0.94f,0.09f);
				break;
			case IcoMem:
				box(0.04f,0.24f,0.96f,0.66f,0.07f);
				fill(0.15f,0.36f,0.31f,0.54f,0.0f);
				fill(0.42f,0.36f,0.58f,0.54f,0.0f);
				fill(0.69f,0.36f,0.85f,0.54f,0.0f);
				for (float x : {0.14f,0.26f,0.38f,0.62f,0.74f,0.86f}) line(x,0.66f,x,0.84f);
				break;
			case IcoProc: {
				float ys[3] = {0.2f,0.5f,0.8f};
				float xs[3] = {0.94f,0.72f,0.84f};
				for (int k = 0; k < 3; k++) {
					fill(0.06f,ys[k] - 0.08f,0.22f,ys[k] + 0.08f,0.03f);
					line(0.34f,ys[k],xs[k],ys[k],th * 1.3f);
				}
				break;
			}
			case IcoKernel:
				box(0.24f,0.24f,0.76f,0.76f,0.06f);
				fill(0.4f,0.4f,0.6f,0.6f,0.02f);
				for (float t : {0.36f,0.5f,0.64f}) {
					line(t,0.06f,t,0.24f);
					line(t,0.76f,t,0.94f);
					line(0.06f,t,0.24f,t);
					line(0.76f,t,0.94f,t);
				}
				break;
			case IcoChip:
				box(0.26f,0.26f,0.74f,0.74f,0.07f);
				for (float t : {0.38f,0.62f}) {
					line(t,0.08f,t,0.26f);
					line(t,0.74f,t,0.92f);
					line(0.08f,t,0.26f,t);
					line(0.74f,t,0.92f,t);
				}
				break;
			case IcoGraph: {
				ImVec2 g[5] = {P(0.06f,0.7f),P(0.3f,0.42f),P(0.5f,0.58f),P(0.72f,0.2f),P(0.94f,0.36f)};
				dl->AddPolyline(g,5,col,ImDrawFlags_None,th * 1.3f);
				line(0.06f,0.88f,0.94f,0.88f);
				break;
			}
			case IcoGpu:
				box(0.04f,0.2f,0.96f,0.7f,0.06f);
				dl->AddCircle(P(0.37f,0.45f),0.14f * s,col,0,th);
				fill(0.62f,0.34f,0.84f,0.56f,0.02f);
				fill(0.2f,0.7f,0.66f,0.84f,0.0f);
				break;
			case IcoSettings:
				dl->AddCircle(P(0.5f,0.5f),0.25f * s,col,0,th * 1.3f);
				for (int k = 0; k < 6; k++) {
					float a = (float)k * IM_PI / 3.0f;
					float cx = std::cos(a);
					float sn = std::sin(a);
					line(0.5f + cx * 0.28f,0.5f + sn * 0.28f,0.5f + cx * 0.46f,0.5f + sn * 0.46f,th * 2.2f);
				}
				break;
			case IcoPrivacy:
				dl->PathLineTo(P(0.5f,0.05f));
				dl->PathLineTo(P(0.88f,0.18f));
				dl->PathLineTo(P(0.88f,0.48f));
				dl->PathBezierCubicCurveTo(P(0.88f,0.72f),P(0.68f,0.87f),P(0.5f,0.96f));
				dl->PathBezierCubicCurveTo(P(0.32f,0.87f),P(0.12f,0.72f),P(0.12f,0.48f));
				dl->PathLineTo(P(0.12f,0.18f));
				dl->PathStroke(col,ImDrawFlags_Closed,th);
				line(0.33f,0.5f,0.46f,0.63f);
				line(0.46f,0.63f,0.68f,0.36f);
				break;
			case IcoMcp:
				line(0.2f,0.5f,0.8f,0.2f);
				line(0.2f,0.5f,0.8f,0.8f);
				dl->AddCircle(P(0.2f,0.5f),0.15f * s,col,0,th);
				dl->AddCircle(P(0.8f,0.2f),0.13f * s,col,0,th);
				dl->AddCircle(P(0.8f,0.8f),0.13f * s,col,0,th);
				break;
			case IcoCredits:
				star(0.5f,0.52f,0.44f,0.19f,5,false);
				break;
			case IcoSearch:
				dl->AddCircle(P(0.42f,0.42f),0.29f * s,col,0,th * 1.2f);
				line(0.63f,0.63f,0.92f,0.92f,th * 1.4f);
				break;
			case IcoRefresh: {
				float a0 = -IM_PI * 0.25f;
				float a1 = IM_PI * 1.35f;
				dl->PathArcTo(P(0.5f,0.5f),0.36f * s,a0,a1);
				dl->PathStroke(col,ImDrawFlags_None,th * 1.2f);
				ImVec2 e(0.5f + std::cos(a0) * 0.36f,0.5f + std::sin(a0) * 0.36f);
				ImVec2 d(std::sin(a0),-std::cos(a0));
				ImVec2 nrm(-d.y,d.x);
				dl->AddTriangleFilled(P(e.x + d.x * 0.24f,e.y + d.y * 0.24f),P(e.x + nrm.x * 0.17f,e.y + nrm.y * 0.17f),P(e.x - nrm.x * 0.17f,e.y - nrm.y * 0.17f),col);
				break;
			}
			case IcoClean:
				star(0.42f,0.56f,0.42f,0.11f,4,true);
				star(0.8f,0.2f,0.2f,0.05f,4,true);
				break;
			case IcoWarn:
				dl->PathLineTo(P(0.5f,0.08f));
				dl->PathLineTo(P(0.96f,0.9f));
				dl->PathLineTo(P(0.04f,0.9f));
				dl->PathStroke(col,ImDrawFlags_Closed,th);
				line(0.5f,0.38f,0.5f,0.62f);
				dl->AddCircleFilled(P(0.5f,0.75f),th * 0.7f,col);
				break;
			case IcoNone:
				break;
		}
	}

	void heading(const char* txt) {
		ImGui::PushFont(g_bold,ImGui::GetStyle().FontSizeBase * 1.08f);
		ImGui::PushStyleColor(ImGuiCol_Text,c.accent);
		ImGui::TextUnformatted(txt);
		ImGui::PopStyleColor();
		popFont();
	}

	void hint(const char* txt) {
		ImGui::PushStyleColor(ImGuiCol_Text,c.dim);
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(txt);
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
	}

	void textEllipsis(const char* txt) {
		ImVec2 p = ImGui::GetCursorScreenPos();
		const char* end = txt + strlen(txt); // data, a ## in it is not an id
		ImVec2 ts = ImGui::CalcTextSize(txt,end,false);
		float w = std::max(ImGui::GetContentRegionAvail().x,1.0f);
		ImGui::Dummy(ImVec2(w,ts.y));
		ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(),p,ImVec2(p.x + w,p.y + ts.y),p.x + w,txt,end,&ts);
	}

	void empty(const char* txt) {
		ImVec2 av = ImGui::GetContentRegionAvail();
		ImVec2 ts = ImGui::CalcTextSize(txt);
		ImVec2 p = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(p.x + std::max(0.0f,(av.x - ts.x) * 0.5f),p.y + std::max(0.0f,(av.y - ts.y) * 0.5f)));
		ImGui::TextDisabled("%s",txt);
	}

	bool fold(const char* label,bool defOpen) {
		ImGuiStorage* st = ImGui::GetStateStorage();
		ImGuiID id = ImGui::GetID(label);
		bool open = st->GetBool(id,defOpen);
		ImVec2 ts = ImGui::CalcTextSize(label,nullptr,true);
		float h = ImGui::GetFrameHeight();
		float cw = px(16.0f);
		ImVec2 p = ImGui::GetCursorScreenPos();
		if (ImGui::InvisibleButton(label,ImVec2(cw + ts.x + px(4.0f),h))) {
			open = !open;
			st->SetBool(id,open);
		}
		ImU32 col = col32(ImGui::IsItemHovered() ? c.text : c.dim);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		float cx = p.x + px(6.0f);
		float cy = p.y + h * 0.5f;
		float d = px(3.5f);
		if (open) {
			dl->PathLineTo(ImVec2(cx - d,cy - d * 0.5f));
			dl->PathLineTo(ImVec2(cx,cy + d * 0.5f));
			dl->PathLineTo(ImVec2(cx + d,cy - d * 0.5f));
		} else {
			dl->PathLineTo(ImVec2(cx - d * 0.5f,cy - d));
			dl->PathLineTo(ImVec2(cx + d * 0.5f,cy));
			dl->PathLineTo(ImVec2(cx - d * 0.5f,cy + d));
		}
		dl->PathStroke(col,ImDrawFlags_None,px(1.5f));
		dl->AddText(ImVec2(p.x + cw,p.y + (h - ts.y) * 0.5f),col32(c.text),label,ImGui::FindRenderedTextEnd(label));
		return open;
	}

	void help(const char* txt) {
		ImGui::TextDisabled("(?)");
		tip(txt);
	}

	void badge(const char* txt,ImVec4 col) {
		ImVec2 ts = ImGui::CalcTextSize(txt);
		float pad = px(6.0f);
		ImGuiWindow* w = ImGui::GetCurrentWindow();
		ImVec2 p(w->DC.CursorPos.x,w->DC.CursorPos.y + w->DC.CurrLineTextBaseOffset);
		ImVec2 sz(ts.x + pad * 2.0f,ts.y);
		ImGui::ItemSize(sz,0.0f);
		if (!ImGui::ItemAdd(ImRect(p,p + sz),0)) return;
		w->DrawList->AddRectFilled(p,p + sz,col32(alpha(col,0.18f)),px(5.0f));
		w->DrawList->AddText(ImVec2(p.x + pad,p.y),col32(col),txt);
	}

	bool segmented(const char* id,const char* const* names,const ImVec4* cols,int n,int& cur) {
		bool r = false;
		float h = ImGui::GetFontSize();
		float pad = ImGui::GetStyle().FramePadding.x;
		float rnd = ImGui::GetStyle().FrameRounding;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImGui::PushID(id);
		for (int i = 0; i < n; i++) {
			if (i) ImGui::SameLine(0.0f,0.0f);
			ImVec2 ts = ImGui::CalcTextSize(names[i]);
			ImVec2 sz(ts.x + pad * 2.0f,h);
			ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::PushID(i);
			if (ImGui::InvisibleButton("##s",sz) && cur != i) {
				cur = i;
				r = true;
			}
			bool hov = ImGui::IsItemHovered();
			ImGui::PopID();
			bool sel = cur == i;
			ImDrawFlags fl = i == 0 ? ImDrawFlags_RoundCornersLeft : i == n - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone;
			ImVec4 bg = sel ? mix(c.panel2,cols[i],0.4f) : hov ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered) : ImGui::GetStyleColorVec4(ImGuiCol_Button);
			dl->AddRectFilled(p,p + sz,col32(bg),rnd,fl);
			dl->AddText(ImVec2(p.x + pad,p.y),col32(sel || hov ? c.text : c.dim),names[i]);
		}
		ImGui::PopID();
		return r;
	}

	void tip(const char* txt) {
		if (!ImGui::BeginItemTooltip()) return;
		ImGui::PushTextWrapPos(px(420.0f));
		ImGui::TextUnformatted(txt);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

	bool iconBtn(const char* id,Icon i,const char* tooltip) {
		float h = ImGui::GetFrameHeight();
		ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));
		bool r = ImGui::Button((std::string("##") + id).c_str(),ImVec2(h,h));
		ImGui::PopStyleColor();
		float sz = std::floor(h * 0.62f);
		ImVec2 mn = ImGui::GetItemRectMin();
		float off = (h - sz) * 0.5f;
		icon(i,ImVec2(mn.x + off,mn.y + off),sz,col32(ImGui::IsItemHovered() ? c.text : c.dim));
		if (tooltip) tip(tooltip);
		return r;
	}

	void pushBtnColor(ImVec4 col) {
		ImGui::PushStyleColor(ImGuiCol_Button,mix(c.panel2,col,0.4f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered,mix(c.panel2,col,0.5f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,mix(c.panel2,col,0.3f));
		ImGui::PushStyleColor(ImGuiCol_Border,mix(c.panel2,col,0.7f));
	}

	void popBtnColor() {
		ImGui::PopStyleColor(4);
	}

	bool beginCard(const char* id,const char* title,ImVec2 size) {
		ImGuiChildFlags fl = ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders;
		if (size.y == 0.0f) fl |= ImGuiChildFlags_AutoResizeY;
		ImGui::PushStyleColor(ImGuiCol_ChildBg,c.panel);
		ImGui::PushStyleColor(ImGuiCol_Border,alpha(c.line,0.7f));
		bool r = ImGui::BeginChild(id,ImVec2(px(size.x),px(size.y)),fl);
		ImGui::PopStyleColor(2);
		if (title && *title) heading(title);
		return r;
	}

	void endCard() {
		ImGui::EndChild();
	}

	void stat(const char* label,const std::string& val,ImVec4 col) {
		ImGui::BeginGroup();
		ImGui::TextDisabled("%s",label);
		pushBig();
		if (col.w > 0.0f) ImGui::TextColored(col,"%s",val.c_str());
		else ImGui::TextUnformatted(val.c_str());
		popFont();
		ImGui::EndGroup();
	}

	void kv(const char* k,const std::string& v) {
		ImGui::TextDisabled("%s",k);
		ImGui::SameLine();
		float av = ImGui::GetContentRegionAvail().x;
		float w = ImGui::CalcTextSize(v.c_str()).x;
		if (av > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + av - w);
		ImGui::TextUnformatted(v.c_str());
	}

	void kvSize(const char* k,uint64_t bytes) {
		kv(k,util::size(bytes));
	}

	void meter(float frac,const char* overlay,ImVec2 size,ImVec4 col) {
		ImVec2 sz(size.x > 0.0f ? px(size.x) : -1.0f,px(size.y > 0.0f ? size.y : 18.0f));
		bool txt = overlay && *overlay;
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0.0f);
		// darker under text, white on it stays above 4.5:1 over the whole load curve
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram,txt ? mix(col,c.bg,0.45f) : col);
		ImGui::ProgressBar(frac,sz,"");
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
		if (!txt) return;
		ImVec2 a = ImGui::GetItemRectMin();
		ImVec2 b = ImGui::GetItemRectMax();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 ts = ImGui::CalcTextSize(overlay);
		ImVec2 tp(a.x + (b.x - a.x - ts.x) * 0.5f,a.y + (b.y - a.y - ts.y) * 0.5f);
		float so = std::floor(px(1.0f));
		dl->AddText(ImVec2(tp.x + so,tp.y + so),IM_COL32(0,0,0,130),overlay);
		dl->AddText(tp,IM_COL32(255,255,255,255),overlay);
	}

	ImVec4 loadColor(float frac) {
		ImVec4 green(0.2f,0.78f,0.3f,1.0f);
		ImVec4 yellow(1.0f,0.7f,0.1f,1.0f);
		ImVec4 red(1.0f,0.25f,0.25f,1.0f);
		frac = ImClamp(frac,0.0f,1.0f);
		return frac < 0.6f ? mix(green,yellow,frac / 0.6f) : mix(yellow,red,(frac - 0.6f) / 0.4f);
	}

	void stackBar(const char* id,const Seg* s,int n,ImVec2 size) {
		ImVec2 sz = itemSize(size,14.0f);
		ImGui::InvisibleButton(id,sz);
		ImVec2 a = ImGui::GetItemRectMin();
		ImVec2 b = ImGui::GetItemRectMax();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		float r = std::min(px(5.0f),sz.y * 0.5f);
		dl->AddRectFilled(a,b,col32(c.panel2),r);
		float sum = 0.0f;
		int first = -1;
		int last = -1;
		for (int i = 0; i < n; i++) if (s[i].v > 0.0f) {
			sum += s[i].v;
			if (first < 0) first = i;
			last = i;
		}
		if (sum <= 0.0f) return;
		bool hov = ImGui::IsItemHovered();
		float mx = ImGui::GetIO().MousePos.x;
		float x = a.x;
		for (int i = 0; i < n; i++) {
			if (s[i].v <= 0.0f) continue;
			float x1 = i == last ? b.x : x + (b.x - a.x) * s[i].v / sum;
			ImDrawFlags fl = i == first && i == last ? ImDrawFlags_RoundCornersAll : i == first ? ImDrawFlags_RoundCornersLeft : i == last ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone;
			dl->AddRectFilled(ImVec2(x,a.y),ImVec2(x1,b.y),col32(s[i].col),r,fl);
			if (hov && mx >= x && mx <= x1) {
				ImGui::BeginTooltip();
				ImGui::ColorButton("##c",s[i].col,ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,ImVec2(ImGui::GetTextLineHeight(),ImGui::GetTextLineHeight()));
				ImGui::SameLine();
				ImGui::Text("%s  (%.*f%%)",s[i].name ? s[i].name : "",std::max(0,std::min(3,cfg::geti("uiPctPrec",1))),s[i].v * 100.0f / sum);
				ImGui::EndTooltip();
			}
			x = x1;
		}
	}

	void legend(const Seg* s,int n) {
		float d = std::floor(ImGui::GetFontSize() * 0.6f);
		bool first = true;
		for (int i = 0; i < n; i++) {
			if (s[i].v <= 0.0f || !s[i].name) continue;
			if (!first) {
				ImGui::SameLine(0.0f,px(14.0f));
				if (ImGui::GetContentRegionAvail().x < d + px(5.0f) + ImGui::CalcTextSize(s[i].name).x) ImGui::NewLine();
			}
			first = false;
			ImGui::Dummy(ImVec2(d,1.0f));
			float x = ImGui::GetItemRectMin().x;
			ImGui::SameLine(0.0f,px(5.0f));
			ImGui::TextDisabled("%s",s[i].name);
			float y = std::floor(ImGui::GetItemRectMin().y + (ImGui::GetItemRectSize().y - d) * 0.5f);
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x,y),ImVec2(x + d,y + d),col32(s[i].col),px(2.0f));
		}
	}

	void graph(const char* id,const Series* s,int ns,float maxv,ImVec2 size,const std::function<std::string(int)>& tipFn,int cap) {
		ImVec2 sz = itemSize(size,90.0f);
		ImGui::InvisibleButton(id,sz);
		ImVec2 a = ImGui::GetItemRectMin();
		ImVec2 b = ImGui::GetItemRectMax();
		bool hov = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(a,b,col32(c.bg),px(5.0f));
		dl->PushClipRect(a,b,true);
		ImU32 grid = col32(alpha(c.line,0.55f));
		for (int i = 1; i < 4; i++) {
			float y = std::floor(a.y + sz.y * (float)i * 0.25f) + 0.5f;
			dl->AddLine(ImVec2(a.x,y),ImVec2(b.x,y),grid);
		}
		for (int i = 1; i < 6; i++) {
			float x = std::floor(a.x + sz.x * (float)i / 6.0f) + 0.5f;
			dl->AddLine(ImVec2(x,a.y),ImVec2(x,b.y),grid);
		}
		int nMax = 0;
		for (int i = 0; i < ns; i++) if (s[i].v && s[i].n > nMax) nMax = s[i].n;
		int maxN = std::max(nMax,cap);
		if (maxv <= 0.0f) maxv = seriesMax(s,ns);
		for (int i = 0; i < ns; i++) drawSeries(dl,s[i].v,s[i].n,maxN,maxv,a,b,s[i].col,px(1.5f));
		int idx = -1;
		float step = maxN > 1 ? sz.x / (float)(maxN - 1) : sz.x;
		if (hov && maxN > 0) {
			float k = (float)(maxN - 1) - (b.x - ImGui::GetIO().MousePos.x) / step;
			idx = ImClamp((int)std::round(k),0,maxN - 1);
			float cx = b.x - (float)(maxN - 1 - idx) * step;
			dl->AddLine(ImVec2(cx,a.y),ImVec2(cx,b.y),col32(alpha(c.text,0.5f)));
			for (int i = 0; i < ns; i++) {
				int j = idx - (maxN - s[i].n);
				if (!s[i].v || j < 0 || j >= s[i].n || s[i].v[j] < 0.0f) continue;
				float y = b.y - ImClamp(s[i].v[j] / maxv,0.0f,1.0f) * (sz.y - 2.0f) - 1.0f;
				dl->AddCircleFilled(ImVec2(cx,y),px(3.0f),col32(s[i].col));
			}
		}
		dl->PopClipRect();
		dl->AddRect(a,b,col32(c.line),px(5.0f));
		if (idx < maxN - nMax) return; // not hovered, or the empty part left of the samples
		if (tipFn) {
			std::string t = tipFn(idx - (maxN - nMax));
			if (!t.empty()) {
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(t.c_str());
				ImGui::EndTooltip();
			}
			return;
		}
		ImGui::BeginTooltip();
		for (int i = 0; i < ns; i++) {
			int j = idx - (maxN - s[i].n);
			if (!s[i].v || j < 0 || j >= s[i].n || s[i].v[j] < 0.0f) continue;
			ImGui::TextColored(s[i].col,"%s : %.1f",s[i].label ? s[i].label : "",s[i].v[j]);
		}
		ImGui::EndTooltip();
	}

	void sparkline(const char* id,const float* v,int n,float maxv,ImVec2 size,ImVec4 col) {
		(void)id;
		ImVec2 sz = itemSize(size,20.0f);
		ImGui::Dummy(sz);
		if (!ImGui::IsItemVisible()) return;
		ImVec2 a = ImGui::GetItemRectMin();
		ImVec2 b = ImGui::GetItemRectMax();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->PushClipRect(a,b,true);
		if (maxv <= 0.0f) {
			Series s{v,n,col,nullptr};
			maxv = seriesMax(&s,1);
		}
		drawSeries(dl,v,n,n,maxv,a,b,col,px(1.25f));
		dl->PopClipRect();
	}

	void graphHead(const char* title,const char* hlp,const char* val) {
		ImGui::TextUnformatted(title);
		if (hlp && *hlp) {
			ImGui::SameLine();
			help(hlp);
		}
		ImGui::SameLine();
		cellRight(val);
	}

	void graphCard(const char* id,const char* title,const char* hlp,const char* val,const Series& s,float maxv,float h,const std::function<std::string(int)>& tipFn,int cap,float w,const char* maxTxt) {
		beginCard(id,nullptr,ImVec2(w / px(1.0f),0.0f));
		graphHead(title,hlp,val);
		std::string gid = std::string(id) + "g";
		graph(gid.c_str(),&s,1,maxv,ImVec2(0.0f,h),tipFn,cap);
		ImVec2 a = ImGui::GetItemRectMin();
		ImVec2 b = ImGui::GetItemRectMax();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (maxTxt && *maxTxt) {
			float pad = px(5.0f);
			ImVec2 tp(a.x + pad,a.y + pad * 0.5f);
			ImVec2 ts = ImGui::CalcTextSize(maxTxt);
			dl->AddRectFilled(ImVec2(tp.x - px(2.0f),tp.y),ImVec2(tp.x + ts.x + px(2.0f),tp.y + ts.y),col32(alpha(c.bg,0.8f)),px(3.0f));
			dl->AddText(tp,col32(c.dim),maxTxt);
		}
		if (s.n <= 0) {
			const char* e = "No samples yet";
			ImVec2 es = ImGui::CalcTextSize(e);
			dl->AddText(ImVec2((a.x + b.x - es.x) * 0.5f,(a.y + b.y - es.y) * 0.5f),col32(c.dim),e);
		}
		endCard();
	}

	bool beginTable(const char* id,int cols,ImVec2 size,ImGuiTableFlags extra) {
		ImGuiTableFlags fl = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable |
			ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | extra;
		return ImGui::BeginTable(id,cols,fl,ImVec2(px(size.x),px(size.y)));
	}

	void endTable() {
		ImGui::EndTable();
	}

	void cellRight(const char* txt) {
		float av = ImGui::GetContentRegionAvail().x;
		float w = ImGui::CalcTextSize(txt).x;
		if (av > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + av - w);
		ImGui::TextUnformatted(txt);
	}

	void cellSize(uint64_t bytes) {
		if (bytes == NA) ImGui::PushStyleColor(ImGuiCol_Text,c.dim);
		cellRight(util::size(bytes).c_str());
		if (bytes == NA) ImGui::PopStyleColor();
	}

	// TableHeader keeps click, sort, drag and the context menu, its own label and arrow are drawn see through
	void headerCell(const char* title,const char* tipTxt,bool right) {
		ImGuiTable* t = ImGui::GetCurrentTable();
		ImGui::PushID(t ? t->CurrentColumn : 0);
		std::string full;
		if (!right || !t || t->CurrentColumn < 0) ImGui::TableHeader(title);
		else {
			ImGuiTableColumn& col = t->Columns[t->CurrentColumn];
			ImVec2 p = ImGui::GetCursorScreenPos();
			float rx = p.x + ImGui::GetContentRegionAvail().x;
			const char* end = ImGui::FindRenderedTextEnd(title);
			ImVec2 ts = ImGui::CalcTextSize(title,end);
			bool sortable = (t->Flags & ImGuiTableFlags_Sortable) && !(col.Flags & ImGuiTableColumnFlags_NoSort);
			float aw = sortable ? std::floor(ImGui::GetFontSize() * 0.65f + ImGui::GetStyle().FramePadding.x) : 0.0f;
			ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0,0,0,0));
			ImGui::TableHeader("##h");
			ImGui::PopStyleColor();
			col.ContentMaxXHeadersIdeal = std::max(col.ContentMaxXHeadersIdeal,p.x + ts.x + aw);
			if (ImGui::IsItemVisible()) {
				bool arrow = sortable && col.SortOrder != -1 && col.SortDirection != ImGuiSortDirection_None;
				float x = std::max(p.x + (arrow ? aw : 0.0f),rx - ts.x);
				ImDrawList* dl = ImGui::GetWindowDrawList();
				ImGui::RenderTextEllipsis(dl,ImVec2(x,p.y),ImVec2(rx,p.y + ImGui::GetFontSize()),rx,title,end,&ts);
				if (arrow) ImGui::RenderArrow(dl,ImVec2(x - aw,p.y),ImGui::GetColorU32(ImGuiCol_Text),col.SortDirection == ImGuiSortDirection_Ascending ? ImGuiDir_Up : ImGuiDir_Down,0.65f);
				if ((!tipTxt || !*tipTxt) && x + ts.x > rx) {
					full.assign(title,end);
					tipTxt = full.c_str();
				}
			}
		}
		if (tipTxt && *tipTxt) tip(tipTxt);
		ImGui::PopID();
	}

	bool search(const char* id,char* buf,size_t n,const char* hintTxt,float w) {
		ImGuiStyle& st = ImGui::GetStyle();
		float isz = std::floor(ImGui::GetFontSize() * 0.9f);
		ImGui::SetNextItemWidth(w != 0.0f ? px(w) : -FLT_MIN);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(st.FramePadding.x + isz + px(5.0f),st.FramePadding.y));
		bool r = ImGui::InputTextWithHint(id,hintTxt,buf,n);
		ImGui::PopStyleVar();
		ImVec2 a = ImGui::GetItemRectMin();
		ImVec2 b = ImGui::GetItemRectMax();
		icon(IcoSearch,ImVec2(a.x + st.FramePadding.x,a.y + (b.y - a.y - isz) * 0.5f),isz,col32(c.dim));
		return r;
	}

	void askOpen(const char* id) {
		g_ask = id;
	}

	bool askDraw(const char* id,const char* text,const char* yes,bool danger) {
		if (g_ask == id) {
			ImGui::OpenPopup(id);
			g_ask.clear();
		}
		bool r = false;
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),ImGuiCond_Appearing,ImVec2(0.5f,0.5f));
		if (ImGui::BeginPopupModal(id,nullptr,ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::PushTextWrapPos(px(420.0f));
			ImGui::TextUnformatted(text);
			ImGui::PopTextWrapPos();
			ImGui::Spacing();
			if (danger) pushBtnColor(c.bad);
			if (ImGui::Button(yes,ImVec2(px(110.0f),0))) {
				r = true;
				ImGui::CloseCurrentPopup();
			}
			if (danger) popBtnColor();
			ImGui::SameLine();
			if (ImGui::Button("Cancel",ImVec2(px(110.0f),0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		return r;
	}

	void toast(const std::string& msg) {
		std::lock_guard<std::mutex> lk(g_tm);
		g_toasts.push_back({msg,util::nowMs()});
		if (g_toasts.size() > 16) g_toasts.erase(g_toasts.begin());
	}

	void drawToasts() {
		const uint64_t life = 3000;
		const uint64_t fade = 500;
		std::vector<Toast> v;
		uint64_t now = util::nowMs();
		{
			std::lock_guard<std::mutex> lk(g_tm);
			g_toasts.erase(std::remove_if(g_toasts.begin(),g_toasts.end(),[&](const Toast& t) { return now - t.t > life + fade; }),g_toasts.end());
			v = g_toasts;
		}
		if (v.empty()) return;
		ImGuiIO& io = ImGui::GetIO();
		ImDrawList* dl = ImGui::GetForegroundDrawList();
		ImFont* f = ImGui::GetFont();
		float fs = ImGui::GetFontSize();
		float pad = px(10.0f);
		float wrap = px(380.0f);
		float y = io.DisplaySize.y - px(38.0f);
		int shown = 0;
		for (int i = (int)v.size() - 1; i >= 0 && shown < 5; i--,shown++) {
			float el = (float)(now - v[i].t);
			float al = el > (float)life ? 1.0f - (el - (float)life) / (float)fade : 1.0f;
			al = ImClamp(al,0.0f,1.0f);
			ImVec2 ts = f->CalcTextSizeA(fs,FLT_MAX,wrap,v[i].msg.c_str());
			ImVec2 b(io.DisplaySize.x - px(14.0f),y);
			ImVec2 a(b.x - ts.x - pad * 2.0f,y - ts.y - pad * 1.4f);
			dl->AddRectFilled(a,b,ImGui::ColorConvertFloat4ToU32(alpha(c.panel2,0.97f * al)),px(6.0f));
			dl->AddRect(a,b,ImGui::ColorConvertFloat4ToU32(alpha(c.line,al)),px(6.0f));
			dl->AddRectFilled(ImVec2(a.x,a.y + px(4.0f)),ImVec2(a.x + px(3.0f),b.y - px(4.0f)),ImGui::ColorConvertFloat4ToU32(alpha(c.accent,al)),px(1.5f));
			dl->AddText(f,fs,ImVec2(a.x + pad,a.y + pad * 0.7f),ImGui::ColorConvertFloat4ToU32(alpha(c.text,al)),v[i].msg.c_str(),nullptr,wrap);
			y = a.y - px(6.0f);
		}
	}

}
