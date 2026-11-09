#pragma once
#include "imgui.h"
#include <cstdint>
#include <string>
#include <functional>

// Look and shared widgets. Pages use these instead of hardcoding colors and sizes, so the app
// reads as one thing. Sizes passed in are unscaled pixels, helpers apply the DPI scale. Anywhere
// you use a raw pixel number yourself wrap it in ui::px().
namespace ui {

	struct Pal {
		ImVec4 bg,panel,panel2,line,text,dim;
		ImVec4 accent,good,warn,bad,info,purple,gold;
		// memory series, same color everywhere : graph, bar, legend
		ImVec4 commit,phys,priv,ws,cache,kernel,standby,modified,freeMem,zeroed;
	};
	extern Pal c;

	enum Icon {
		IcoDash,IcoMem,IcoProc,IcoKernel,IcoGpu,IcoSettings,IcoPrivacy,IcoMcp,IcoCredits,
		IcoSearch,IcoRefresh,IcoClean,IcoWarn,IcoChip,IcoGraph,IcoNone
	};

	void applyTheme(float dpi); // after CreateContext, again on DPI change
	void loadFonts(float dpi); // Segoe UI + Consolas, or DejaVu/Noto/Liberation on Linux, ImGui default if none
	float px(float v); // unscaled to scaled
	void pushMono(); // hex dumps, addresses. Pair with popFont
	void pushMonoCell(); // pushMono for one line in a table row : moves it down onto the body text's baseline. Pair with popFont
	void pushBold();
	void pushBig(); // large numbers on cards
	void title(const char* txt); // page title
	float titleW(const char* txt); // its width when drawn with title
	void popFont();

	void icon(Icon i,ImVec2 pos,float sz,ImU32 col);
	void heading(const char* txt); // section title in accent, keeps old cyan headings' place
	void hint(const char* txt); // dim wrapped explanation line
	void textEllipsis(const char* txt); // one line in the text color, cut with ... at the cell edge. ui::tip works on it
	void empty(const char* txt); // dim line centered in what is left of the pane, for empty states
	bool fold(const char* label,bool defOpen); // thin chevron and a label, true while open. ##id works
	void help(const char* txt); // small (?) with a tooltip
	void badge(const char* txt,ImVec4 col);
	bool segmented(const char* id,const char* const* names,const ImVec4* cols,int n,int& cur); // joined choice buttons, cols tints the selected one, true on a change
	void tip(const char* txt); // tooltip on last item, wrapped
	bool iconBtn(const char* id,Icon i,const char* tooltip);
	void pushBtnColor(ImVec4 col); // tints the next buttons, pair with popBtnColor
	void popBtnColor();

	// rounded panel. Always pair with endCard, even when it returns false.
	bool beginCard(const char* id,const char* title,ImVec2 size = ImVec2(0,0));
	void endCard();

	// big number with a label, for card rows. col = 0 alpha means default text color
	void stat(const char* label,const std::string& val,ImVec4 col = ImVec4(0,0,0,0));
	void kv(const char* k,const std::string& v); // label left, value right aligned on one row
	void kvSize(const char* k,uint64_t bytes); // NA -> N/A

	// progress style bar, overlay text centered with a shadow
	void meter(float frac,const char* overlay,ImVec2 size,ImVec4 col);
	ImVec4 loadColor(float frac); // green to yellow to red, same curve as the RAM bar

	// stacked bar with a hover tooltip per part
	struct Seg { float v; ImVec4 col; const char* name; };
	void stackBar(const char* id,const Seg* s,int n,ImVec2 size);
	void legend(const Seg* s,int n); // color square and dim name for each part with v > 0, wraps when short of room

	// line + area graph, newest sample on the right. Hover shows a cursor and tip(idx) text.
	// cap : samples the full width stands for (the ring size), fewer fill in from the right. 0 stretches what there is
	struct Series { const float* v; int n; ImVec4 col; const char* label; };
	void graph(const char* id,const Series* s,int ns,float maxv,ImVec2 size,const std::function<std::string(int)>& tip = nullptr,int cap = 0);
	void sparkline(const char* id,const float* v,int n,float maxv,ImVec2 size,ImVec4 col);
	void graphHead(const char* title,const char* help,const char* val); // the row above a graph : title, (?) when help is set, val right aligned
	// graphHead and graph in a card w wide (scaled px), graph h high (unscaled). maxTxt : dim label top left in the plot. Says so while there are no samples
	void graphCard(const char* id,const char* title,const char* help,const char* val,const Series& s,float maxv,float h,const std::function<std::string(int)>& tip,int cap,float w,const char* maxTxt = nullptr);

	// ImGui table with FRS defaults : resizable, reorderable, hideable, sortable, scroll, borders.
	// Column order, width and visibility persist on their own through ui.ini.
	bool beginTable(const char* id,int cols,ImVec2 size = ImVec2(0,0),ImGuiTableFlags extra = 0);
	void endTable();
	void cellRight(const char* txt); // right aligned text in the current cell
	void cellSize(uint64_t bytes); // right aligned size, NA -> dim N/A
	// a TableHeader for a header row you build yourself. right : title flush with the cellRight values, the sort arrow left of it
	void headerCell(const char* title,const char* tip,bool right);

	// search box with an icon, true when the text changed
	bool search(const char* id,char* buf,size_t n,const char* hintTxt,float w = 0.0f);

	// yes/no modal : askOpen(id) when the button is clicked, askDraw(id,...) every frame, true once on Yes
	void askOpen(const char* id);
	bool askDraw(const char* id,const char* text,const char* yes = "Yes",bool danger = false);

	void toast(const std::string& msg); // any thread, short lived corner message
	void drawToasts(); // shell only

}

// sidebar pages, process windows and the Ctrl+K palette
namespace nav {

	enum Group { GrpMain,GrpInspect,GrpApp };
	void addPage(const char* id,const char* title,ui::Icon ico,Group g,void(*draw)());
	void go(const char* id);
	const char* current();
	const char* currentTitle();
	float titleColW(); // width of the longest page title, so what follows the title stays put between pages
	void addCmd(const char* title,std::function<void()> fn,const char* keys = nullptr); // shows in the palette, keys : extra search words
	void setHeader(std::function<void()> fn); // App only, top of the right side
	void drawShell(); // App::drawUI only

}
