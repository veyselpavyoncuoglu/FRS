#include "procs.h"
#include "ui.h"
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

namespace {

	struct Tab {
		std::string title;
		int order;
		procwin::TabFn fn;
	};

	struct Win {
		ProcRef r;
		std::string title;
		bool open = true;
		bool focus = true;
		bool fresh = true;
		std::string pick; // tab to select once
		ImVec2 pos,sz; // last frame
	};

	std::vector<Tab> g_tabs;
	std::vector<void(*)()> g_floats;
	std::deque<Win> g_wins; // a tab may open another window while the loop runs

	const size_t MAXWIN = 8;

	// env FRS_TEST_PROC waits for the first process snapshot, then opens something so the window can be looked at
	void testOpen() {
		static bool done = false;
		static uint64_t t0 = util::nowMs();
		if (done) return;
		const char* e = util::testEnv("FRS_TEST_PROC");
		if (!e || !*e) {
			done = true;
			return;
		}
		uint32_t pid = (uint32_t)strtoul(e,nullptr,10);
		auto list = procs::latest();
		const ProcRow* row = list->find(pid);
		if (!row && util::nowMs() - t0 < 5000) return;
		done = true;
		ProcRef r;
		r.pid = pid;
		r.name = "pid " + std::to_string(pid);
		procwin::open(row ? procs::ref(*row) : r,util::testEnv("FRS_TEST_TAB"));
	}

}

namespace procwin {

	void addTab(const char* title,int order,TabFn fn) {
		g_tabs.push_back({title,order,fn});
		std::stable_sort(g_tabs.begin(),g_tabs.end(),[](const Tab& a,const Tab& b) { return a.order < b.order; });
	}

	void addFloat(void(*fn)()) {
		g_floats.push_back(fn);
	}

	void open(const ProcRef& r,const char* tab) {
		for (auto& w : g_wins) if (w.r == r) {
			w.focus = true;
			if (tab) w.pick = tab;
			return;
		}
		if (g_wins.size() >= MAXWIN) {
			ui::toast("Too many process windows, close one first");
			return;
		}
		Win w;
		w.r = r;
		if (tab) w.pick = tab;
		std::string nm = util::safe(r.name);
		std::replace(nm.begin(),nm.end(),'#','_');
		w.title = nm + " (" + std::to_string(r.pid) + ")##" + std::to_string(r.pid) + "_" + std::to_string(r.start);
		g_wins.push_back(std::move(w));
	}

	void openPid(uint32_t pid) {
		auto l = procs::latest();
		const ProcRow* row = l->find(pid);
		if (row) open(procs::ref(*row));
		else ui::toast("Process " + std::to_string(pid) + " not found");
	}

	void select(const ProcRef& r,const char* title) {
		for (auto& w : g_wins) if (w.r == r) w.pick = title;
	}

	bool isOpen(const ProcRef& r) {
		for (auto& w : g_wins) if (w.open && w.r == r) return true;
		return false;
	}

	void draw() {
		testOpen();
		ImGuiIO& io = ImGui::GetIO();
		bool vis = io.DisplaySize.x >= 1.0f && io.DisplaySize.y >= 1.0f; // a frame of a minimized main window has no size, nothing gets squeezed by it
		for (size_t i = 0; i < g_wins.size(); i++) {
			Win& w = g_wins[i];
			if (vis && w.fresh) {
				ImVec2 sz(std::min(ui::px(900.0f),io.DisplaySize.x * 0.95f),std::min(ui::px(620.0f),io.DisplaySize.y * 0.95f));
				float off = ui::px(24.0f) * (float)(i % 6);
				ImGui::SetNextWindowSize(sz,ImGuiCond_Always);
				ImGui::SetNextWindowPos(ImVec2(std::max(0.0f,(io.DisplaySize.x - sz.x) * 0.5f) + off,std::max(0.0f,(io.DisplaySize.y - sz.y) * 0.5f) + off),ImGuiCond_Always);
				w.fresh = false;
			} else if (vis) {
				// the main window got smaller or this one got dragged out : back inside, never bigger than the display
				float sx = std::min(w.sz.x,io.DisplaySize.x);
				float sy = std::min(w.sz.y,io.DisplaySize.y);
				ImVec2 p(std::max(0.0f,std::min(w.pos.x,io.DisplaySize.x - sx)),std::max(0.0f,std::min(w.pos.y,io.DisplaySize.y - sy)));
				if (p.x != w.pos.x || p.y != w.pos.y) ImGui::SetNextWindowPos(p);
				ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(ui::px(480.0f),io.DisplaySize.x),std::min(ui::px(240.0f),io.DisplaySize.y)),io.DisplaySize);
			}
			if (w.focus) {
				ImGui::SetNextWindowFocus();
				w.focus = false;
			}
			bool shown = ImGui::Begin(w.title.c_str(),&w.open,ImGuiWindowFlags_NoCollapse);
			w.pos = ImGui::GetWindowPos();
			w.sz = ImGui::GetWindowSize();
			if (shown) {
				if (!procs::alive(w.r)) {
					ui::icon(ui::IcoWarn,ImGui::GetCursorScreenPos(),ImGui::GetFontSize(),ImGui::GetColorU32(ui::c.warn));
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetFontSize() + ui::px(6.0f));
					ImGui::TextColored(ui::c.warn,"This process has exited. Showing what was last known.");
				}
				if (g_tabs.empty()) ImGui::TextDisabled("Nothing to show for a process yet.");
				else if (ImGui::BeginTabBar("##ptabs",ImGuiTabBarFlags_FittingPolicyScroll)) {
					// a tab can pick another one while it draws, that one acts next frame
					std::string pick;
					pick.swap(w.pick);
					for (auto& t : g_tabs) {
						ImGuiTabItemFlags fl = !pick.empty() && pick == t.title ? ImGuiTabItemFlags_SetSelected : 0;
						if (ImGui::BeginTabItem(t.title.c_str(),nullptr,fl)) {
							t.fn(w.r);
							ImGui::EndTabItem();
						}
					}
					ImGui::EndTabBar();
				}
			}
			ImGui::End();
		}
		g_wins.erase(std::remove_if(g_wins.begin(),g_wins.end(),[](const Win& w) { return !w.open; }),g_wins.end());
		for (auto fn : g_floats) fn();
	}

}
