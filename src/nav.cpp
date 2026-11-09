#include "ui.h"
#include "cfg.h"
#include "domains.h"
#include <string>
#include <vector>

namespace {

	struct Page {
		std::string id,title;
		ui::Icon ico;
		nav::Group grp;
		void (*draw)();
	};

	struct Cmd {
		std::string title;
		std::function<void()> fn;
		std::string keys; // extra words the palette search matches, not shown
	};

	std::vector<Page> g_pages;
	std::vector<Cmd> g_cmds;
	std::function<void()> g_header;
	int g_cur = 0;
	int g_shown = -1; // page the row list last followed, so wheeling the selected row away sticks
	bool g_palOpen = false;

	ImVec2 add(ImVec2 a,float x,float y) { return ImVec2(a.x + x,a.y + y); }

	// shell panels are square, cards inside the pages keep their rounding
	void beginPanel(const char* id,ImVec2 sz,ImGuiChildFlags fl,ImGuiWindowFlags wf,bool bg) {
		if (bg) ImGui::PushStyleColor(ImGuiCol_ChildBg,ui::c.panel);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,0.0f);
		ImGui::BeginChild(id,sz,fl,wf);
		ImGui::PopStyleVar();
		if (bg) ImGui::PopStyleColor();
	}

	bool sideRow(const char* id,float x,float w,float h,bool sel,bool small,const char* tip) {
		ImGui::SetCursorPosX(x);
		ImVec2 p = ImGui::GetCursorScreenPos();
		bool hit = ImGui::InvisibleButton(id,ImVec2(w,h));
		bool hov = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (sel) dl->AddRectFilled(p,add(p,w,h),ImGui::GetColorU32(ui::c.panel2),ui::px(6.0f));
		else if (hov) dl->AddRectFilled(p,add(p,w,h),ImGui::GetColorU32(ImVec4(ui::c.panel2.x,ui::c.panel2.y,ui::c.panel2.z,0.6f)),ui::px(6.0f));
		if (sel) dl->AddRectFilled(add(p,0,ui::px(6.0f)),add(p,ui::px(3.0f),h - ui::px(6.0f)),ImGui::GetColorU32(ui::c.accent),ui::px(1.5f));
		if (small && tip && hov) {
			ImGui::SetNextWindowPos(add(p,w + ui::px(8.0f),h * 0.5f),ImGuiCond_Always,ImVec2(0.0f,0.5f)); // beside the row, not over the page
			ImGui::SetTooltip("%s",tip);
		}
		return hit;
	}

	void drawSide(bool small,float sw) {
		float pad = ui::px(8.0f);
		float rowW = sw - pad * 2.0f;
		float rowH = ui::px(31.0f);
		float isz = ui::px(17.0f);

		ImGui::SetCursorPosY(ui::px(12.0f));
		ImGui::SetCursorPosX(small ? (sw - ui::titleW("FRS")) * 0.5f : pad + ui::px(6.0f));
		ImGui::PushStyleColor(ImGuiCol_Text,ui::c.accent);
		ui::title("FRS");
		ImGui::PopStyleColor();
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui::px(6.0f));

		// search and the collapse toggle stay at the bottom, the page rows scroll above them on a short window
		float h = ui::px(30.0f);
		float botH = h * 2.0f + ImGui::GetStyle().ItemSpacing.y + ui::px(8.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,ui::px(6.0f)); // fits in the gap right of the rows
		ImGui::BeginChild("##rows",ImVec2(0,-botH));
		ImGui::PopStyleVar();

		const char* names[] = {"MAIN","INSPECT","APP"};
		for (int g = 0; g < 3; g++) {
			bool any = false;
			for (auto& p : g_pages) if (p.grp == g) any = true;
			if (!any) continue;
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui::px(4.0f));
			if (small) {
				ImVec2 q = ImGui::GetCursorScreenPos();
				ImGui::GetWindowDrawList()->AddLine(add(q,pad * 1.5f,0),add(q,sw - pad * 1.5f,0),ImGui::GetColorU32(ui::c.line));
				ImGui::Dummy(ImVec2(1,ui::px(2.0f)));
			} else {
				ImGui::SetCursorPosX(pad + ui::px(6.0f));
				ImGui::PushFont(nullptr,ImGui::GetStyle().FontSizeBase * 0.8f);
				ImGui::TextDisabled("%s",names[g]);
				ImGui::PopFont();
			}
			for (int i = 0; i < (int)g_pages.size(); i++) {
				Page& p = g_pages[i];
				if (p.grp != g) continue;
				bool sel = i == g_cur;
				ImVec2 q = add(ImGui::GetCursorScreenPos(),pad,0);
				ImGui::PushID(i);
				if (sideRow("##nav",pad,rowW,rowH,sel,small,p.title.c_str())) g_cur = i;
				bool hov = ImGui::IsItemHovered();
				if (sel && i != g_shown && !ImGui::IsItemVisible()) ImGui::SetScrollHereY();
				ImGui::PopID();
				ImU32 col = ImGui::GetColorU32(sel ? ui::c.accent : hov ? ui::c.text : ui::c.dim);
				float ix = small ? (rowW - isz) * 0.5f : ui::px(12.0f);
				ui::icon(p.ico,add(q,ix,(rowH - isz) * 0.5f),isz,col);
				if (!small) {
					ImU32 tc = ImGui::GetColorU32(sel || hov ? ui::c.text : ui::c.dim);
					float ty = (rowH - ImGui::GetFontSize()) * 0.5f;
					ImGui::GetWindowDrawList()->AddText(add(q,ix + isz + ui::px(10.0f),ty),tc,p.title.c_str());
				}
			}
		}

		g_shown = g_cur;
		ImGui::EndChild();

		ImGui::SetCursorPosY(ImGui::GetWindowHeight() - botH);
		ImVec2 sq = add(ImGui::GetCursorScreenPos(),pad,0);
		if (sideRow("##search",pad,rowW,h,false,small,"Search  (Ctrl+K)")) g_palOpen = true;
		ImU32 sc = ImGui::GetColorU32(ImGui::IsItemHovered() ? ui::c.text : ui::c.dim);
		ui::icon(ui::IcoSearch,add(sq,small ? (rowW - isz) * 0.5f : ui::px(12.0f),(h - isz) * 0.5f),isz,sc);
		if (!small) {
			float ty = (h - ImGui::GetFontSize()) * 0.5f;
			const char* kh = "Ctrl+K";
			ImGui::GetWindowDrawList()->AddText(add(sq,ui::px(12.0f) + isz + ui::px(10.0f),ty),sc,"Search");
			ImGui::GetWindowDrawList()->AddText(add(sq,rowW - ImGui::CalcTextSize(kh).x - ui::px(8.0f),ty),ImGui::GetColorU32(ui::c.dim),kh);
		}

		ImVec2 q = add(ImGui::GetCursorScreenPos(),pad,0);
		bool hit = sideRow("##collapse",pad,rowW,h,false,small,"Expand");
		ImU32 col = ImGui::GetColorU32(ImGui::IsItemHovered() ? ui::c.text : ui::c.dim);
		float cx = small ? rowW * 0.5f : ui::px(20.0f);
		float cy = h * 0.5f;
		float d = ui::px(4.0f) * (small ? 1.0f : -1.0f);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddLine(add(q,cx - d * 0.5f,cy - ui::px(4.0f)),add(q,cx + d * 0.5f,cy),col,ui::px(1.5f));
		dl->AddLine(add(q,cx + d * 0.5f,cy),add(q,cx - d * 0.5f,cy + ui::px(4.0f)),col,ui::px(1.5f));
		if (!small) {
			float ty = (h - ImGui::GetFontSize()) * 0.5f;
			dl->AddText(add(q,ui::px(12.0f) + isz + ui::px(10.0f),ty),col,"Collapse");
		}
		if (hit) cfg::setb("uiSideSmall",!small);

		ImVec2 wp = ImGui::GetWindowPos();
		dl->AddLine(add(wp,sw - 0.5f,0),add(wp,sw - 0.5f,ImGui::GetWindowHeight()),ImGui::GetColorU32(ui::c.line));
	}

	void drawPalette() {
		static char q[128] = "";
		static int sel = 0;
		static bool moved = false;
		if (g_palOpen) {
			g_palOpen = false;
			q[0] = 0;
			sel = 0;
			ImGui::OpenPopup("##palette");
		}
		ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f,io.DisplaySize.y * 0.14f),ImGuiCond_Always,ImVec2(0.5f,0.0f));
		ImGui::SetNextWindowSize(ImVec2(ui::px(480.0f),0));
		if (!ImGui::BeginPopup("##palette",ImGuiWindowFlags_NoSavedSettings)) return;

		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		if (ui::search("##palq",q,sizeof(q),"Go to a page or run a command")) sel = 0;

		std::vector<int> hits; // >= 0 page, < 0 command as -1 - idx. Pages in sidebar order
		for (int g = 0; g < 3; g++) for (int i = 0; i < (int)g_pages.size(); i++) if (g_pages[i].grp == g && util::has(g_pages[i].title,q)) hits.push_back(i);
		for (int i = 0; i < (int)g_cmds.size(); i++) if (util::has(g_cmds[i].title + " " + g_cmds[i].keys,q)) hits.push_back(-1 - i);

		int n = (int)hits.size();
		if (n > 0 && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
			sel = (sel + 1) % n;
			moved = true;
		}
		if (n > 0 && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
			sel = (sel + n - 1) % n;
			moved = true;
		}
		if (sel >= n) sel = n > 0 ? n - 1 : 0;
		int run = -1;
		if (n > 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) run = sel;

		float rowH = ImGui::GetTextLineHeightWithSpacing();
		float h = std::min(rowH * (float)std::max(n,1) + ui::px(8.0f),ui::px(300.0f));
		ImGui::BeginChild("##palist",ImVec2(0,h));
		if (!n) ImGui::TextDisabled("Nothing matches");
		for (int i = 0; i < n; i++) {
			int k = hits[i];
			const char* t = k >= 0 ? g_pages[k].title.c_str() : g_cmds[-1 - k].title.c_str();
			ImGui::PushID(i);
			if (ImGui::Selectable(t,i == sel)) run = i;
			if (i == sel && moved) ImGui::SetScrollHereY();
			ImGui::PopID();
			if (k >= 0) {
				ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Page").x);
				ImGui::TextDisabled("Page");
			}
		}
		moved = false;
		ImGui::EndChild();

		std::function<void()> fn;
		if (run >= 0) {
			int k = hits[run];
			if (k >= 0) g_cur = k;
			else fn = g_cmds[-1 - k].fn;
			ImGui::CloseCurrentPopup();
		} else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		if (fn) fn();
	}

}

namespace nav {

	void addPage(const char* id,const char* title,ui::Icon ico,Group g,void(*draw)()) {
		g_pages.push_back({id,title,ico,g,draw});
	}

	void go(const char* id) {
		for (int i = 0; i < (int)g_pages.size(); i++) if (g_pages[i].id == id) g_cur = i;
	}

	const char* current() {
		return g_cur < (int)g_pages.size() ? g_pages[g_cur].id.c_str() : "";
	}

	const char* currentTitle() {
		return g_cur < (int)g_pages.size() ? g_pages[g_cur].title.c_str() : "";
	}

	float titleColW() {
		float w = 0.0f;
		for (auto& p : g_pages) w = std::max(w,ui::titleW(p.title.c_str()));
		return w;
	}

	void addCmd(const char* title,std::function<void()> fn,const char* keys) {
		g_cmds.push_back({title,std::move(fn),keys ? keys : ""});
	}

	void setHeader(std::function<void()> fn) {
		g_header = std::move(fn);
	}

	void drawShell() {
		static bool first = true;
		if (first) {
			first = false;
			const char* e = util::testEnv("FRS_TEST_PAGE");
			if (e && *e) go(e);
		}
		ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextWindowPos(ImVec2(0,0));
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0.0f);
		ImGui::Begin("##w",nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoBringToFrontOnFocus);
		ImGui::PopStyleVar(3);

		bool small = cfg::getb("uiSideSmall",false);
		float sw = ui::px(small ? 56.0f : 204.0f);
		const ImGuiWindowFlags still = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

		beginPanel("##side",ImVec2(sw,0),ImGuiChildFlags_None,still,true);
		drawSide(small,sw);
		ImGui::EndChild();

		ImGui::SameLine(0,0);
		beginPanel("##right",ImVec2(0,0),ImGuiChildFlags_None,still,false);
		float gap = ImGui::GetStyle().ItemSpacing.y;
		bool sb = cfg::getb("sbOn",true);
		float barH = sb ? ui::px(28.0f) : 0.0f;

		beginPanel("##hdr",ImVec2(0,0),ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,still,true);
		if (g_header) g_header();
		ImGui::EndChild();

		if (g_cur < (int)g_pages.size()) {
			Page& p = g_pages[g_cur];
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() - gap); // scrolled content clips right at the header, no dark band
			ImGui::BeginChild(("##pg_" + p.id).c_str(),ImVec2(0,sb ? -(barH + gap) : 0.0f),ImGuiChildFlags_AlwaysUseWindowPadding);
			p.draw();
			ImGui::EndChild();
		}

		if (sb) {
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(ui::px(12.0f),ui::px(5.0f)));
			beginPanel("##status",ImVec2(0,barH),ImGuiChildFlags_AlwaysUseWindowPadding,still,true);
			ImGui::PopStyleVar();
			surf::drawStatusBar();
			ImGui::EndChild();
		}

		ImGui::EndChild();

		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_K) && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)) g_palOpen = true;
		drawPalette();
		ImGui::End();
	}

}
