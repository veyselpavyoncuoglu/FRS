#include "app.h"
#include "ram.h"
#include "platform.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

namespace {

	// Monotonic milliseconds since process start (replaces Win32 GetTickCount64).
	uint64_t nowMs() {
		using namespace std::chrono;
		static const steady_clock::time_point t0 = steady_clock::now();
		return (uint64_t)duration_cast<milliseconds>(steady_clock::now() - t0).count();
	}

	std::string ts() {
		time_t t = time(nullptr);
		tm tm_;
#ifdef _WIN32
		localtime_s(&tm_,&t);
#else
		localtime_r(&t,&tm_);
#endif
		char buf[16];
		strftime(buf,sizeof(buf),"[%H:%M:%S]",&tm_);
		return buf;
	}

}

// All application state. Kept out of the header so backends only see App's three
// entry points.
struct App::State {
	RamStats stats{};
	bool elev = false;

	int flushSz = 512;
	std::vector<std::string> log;
	bool scrollLog = false;

	bool autoRefresh = true;
	float refreshSec = 1.0f;
	bool autoClean = false;
	int cleanThr = 20;
	bool acFS = false,acEW = true,acPS = false,acFM = false;
	int maxLog = 200;

	uint64_t lastRefresh = 0;
	bool startupEnabled = false;

	std::mutex logMtx;
	std::atomic<bool> cleaning{false};
	std::atomic<float> cleanProg{0.0f};
	std::vector<std::string> pendingLog;

	void trimLog() {
		while ((int)log.size() > maxLog) log.erase(log.begin());
	}
	void addLog(std::string s) {
		log.push_back(std::move(s));
		trimLog();
		scrollLog = true;
	}
	void flushPending() {
		std::lock_guard<std::mutex> lk(logMtx);
		for (auto& e : pendingLog) addLog(e);
		pendingLog.clear();
	}

	std::string serialize() const {
		char buf[512];
		snprintf(buf,sizeof(buf),
			"flushSz=%d\n"
			"autoRefresh=%d\n"
			"refreshSec=%.2f\n"
			"autoClean=%d\n"
			"cleanThr=%d\n"
			"acFS=%d\n"
			"acEW=%d\n"
			"acPS=%d\n"
			"acFM=%d\n"
			"maxLog=%d\n",
			flushSz,(int)autoRefresh,refreshSec,(int)autoClean,cleanThr,
			(int)acFS,(int)acEW,(int)acPS,(int)acFM,maxLog);
		return buf;
	}

	void save() {
		plat::writeSettings(serialize());
	}

	void load() {
		std::string text = plat::readSettings();
		if (text.empty()) return;
		size_t pos = 0;
		while (pos < text.size()) {
			size_t eol = text.find('\n',pos);
			if (eol == std::string::npos) eol = text.size();
			std::string line = text.substr(pos,eol - pos);
			pos = eol + 1;
			char key[64]; float fval;
			if (sscanf(line.c_str(),"%63[^=]=%f",key,&fval) == 2) {
				int ival = (int)fval;
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
	}

	void runClean(bool fs,bool ew,bool ps,bool fm,const char* pfx = "") {
		if (cleaning.load()) return;
		int total = (int)fs + (int)ew + (int)ps + (int)fm;
		if (total == 0) return;
		cleaning.store(true);
		cleanProg.store(0.0f);
		int sz = flushSz;
		std::string pre = pfx;
		std::thread([this,fs,ew,ps,fm,total,sz,pre]() {
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
	}
};

App::App() : s(new State()) {}
App::~App() { delete s; }

void App::init() {
	s->stats = getRamStats();
	s->elev = isElevated();
	s->lastRefresh = nowMs();
	s->load();
	s->startupEnabled = plat::startupEnabled();
}

void App::update() {
	s->flushPending();
	uint64_t now = nowMs();
	if (s->autoRefresh && (now - s->lastRefresh) >= (uint64_t)(s->refreshSec * 1000.0f)) {
		s->lastRefresh = now;
		s->stats = getRamStats();
		if (s->autoClean && !s->cleaning.load()) {
			float pct = (s->stats.total > 0) ? (float)s->stats.used / (float)s->stats.total * 100.0f : 0.0f;
			if (pct >= (float)s->cleanThr) {
				s->runClean(s->acFS,s->acEW,s->acPS,s->acFM,"[auto] ");
			}
		}
	}
}

void App::drawUI() {
	State& st = *s;
	ImGuiIO& io = ImGui::GetIO();

	ImGui::SetNextWindowPos(ImVec2(0,0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::Begin("##w",nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoSavedSettings);

	ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"FRS");
	ImGui::SameLine();
	char sub[64];
	snprintf(sub,sizeof(sub),"Free RAM System  (%s)",plat::osName());
	ImGui::TextDisabled("%s",sub);
	ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("[NOT ELEVATED]").x);
	if (st.elev) {
		ImGui::TextColored(ImVec4(0.3f,1.0f,0.45f,1.0f),"[ADMIN]");
	} else {
		ImGui::TextColored(ImVec4(1.0f,0.3f,0.3f,1.0f),"[NOT ELEVATED]");
	}

	ImGui::Separator();

	float used = (float)st.stats.used;
	float total = (float)st.stats.total;
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
	snprintf(ovl,sizeof(ovl),"%.0f / %.0f MB   (%.0f MB free | %.1f%%)",used,total,(float)st.stats.free,frac * 100.0f);
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
		st.stats = getRamStats();
		st.elev = isElevated();
		st.lastRefresh = nowMs();
	}
	ImGui::SameLine();
	if (st.autoRefresh) {
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
			if (ImGui::InputInt("##fsz",&st.flushSz,64,512)) st.save();
			ImGui::SameLine();
			if (ImGui::Button("Max")) {
				st.stats = getRamStats();
				st.flushSz = (int)st.stats.total;
			}
			if (st.flushSz < 64) st.flushSz = 64;
			if (st.flushSz > (int)st.stats.total && st.stats.total > 0) st.flushSz = (int)st.stats.total;

			ImGui::Spacing();

			bool busy = st.cleaning.load();
			if (busy) ImGui::BeginDisabled();

			if (ImGui::Button("Flush Standby")) st.runClean(true,false,false,false);
			ImGui::SameLine();
			if (ImGui::Button("Empty Working Sets")) st.runClean(false,true,false,false);
			ImGui::SameLine();
			if (ImGui::Button("Purge Standby")) st.runClean(false,false,true,false);
			ImGui::SameLine();
			if (ImGui::Button("Flush Modified")) st.runClean(false,false,false,true);

			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.15f,0.55f,0.2f,1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.2f,0.75f,0.28f,1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(0.1f,0.45f,0.15f,1.0f));
			if (ImGui::Button("Full Clean  (all methods)",ImVec2(-1.0f,28.0f))) {
				st.runClean(true,true,true,true);
			}
			ImGui::PopStyleColor(3);

			if (busy) ImGui::EndDisabled();

			ImGui::Spacing();
			if (busy) {
				float p = st.cleanProg.load();
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
			if (ImGui::SmallButton("Clear")) st.log.clear();

			ImGui::BeginChild("##log",ImVec2(0,-1.0f),true);
			for (auto& e : st.log) {
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
			if (st.scrollLog) {
				ImGui::SetScrollHereY(1.0f);
				st.scrollLog = false;
			}
			ImGui::EndChild();

			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Settings")) {
			ImGui::Spacing();
			bool changed = false;

			ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"Auto-Refresh");
			changed |= ImGui::Checkbox("Enabled##ar",&st.autoRefresh);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(70.0f);
			changed |= ImGui::InputFloat("sec##ri",&st.refreshSec,0.1f,1.0f,"%.1f");
			if (st.refreshSec < 0.1f) st.refreshSec = 0.1f;
			if (st.refreshSec > 60.0f) st.refreshSec = 60.0f;
			ImGui::TextDisabled("  Polls RAM stats every N seconds.");

			ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

			ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"Auto-Clean");
			changed |= ImGui::Checkbox("Enabled##ac",&st.autoClean);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(70.0f);
			changed |= ImGui::InputInt("%% threshold##ct",&st.cleanThr,1,5);
			if (st.cleanThr < 1) st.cleanThr = 1;
			if (st.cleanThr > 100) st.cleanThr = 100;
			ImGui::TextDisabled("  Triggers clean when RAM usage >= threshold.");

			ImGui::Spacing();
			ImGui::Text("Methods :");
			changed |= ImGui::Checkbox("Flush Standby##acm",&st.acFS);
			ImGui::SameLine(220.0f);
			changed |= ImGui::Checkbox("Empty Working Sets##acm",&st.acEW);
			changed |= ImGui::Checkbox("Purge Standby##acm",&st.acPS);
			ImGui::SameLine(220.0f);
			changed |= ImGui::Checkbox("Flush Modified##acm",&st.acFM);

			ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

			ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"Log");
			ImGui::SetNextItemWidth(100.0f);
			changed |= ImGui::InputInt("max entries##ml",&st.maxLog,10,50);
			if (st.maxLog < 10) st.maxLog = 10;
			if (st.maxLog > 9999) st.maxLog = 9999;
			ImGui::SameLine();
			if (ImGui::Button("Clear Log")) st.log.clear();
			ImGui::TextDisabled("  Oldest entries removed when limit is exceeded.");

			if (changed) st.save();

			if (plat::startupSupported()) {
				ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

				ImGui::TextColored(ImVec4(0.35f,0.85f,1.0f,1.0f),"Startup");
				bool startupCopy = st.startupEnabled;
				if (ImGui::Checkbox("Run at startup (no prompt)##st",&startupCopy)) {
					if (plat::setStartupEnabled(startupCopy)) {
						st.startupEnabled = startupCopy;
					}
				}
				ImGui::TextDisabled("  Launches FRS automatically when you log in.");
			}

			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}

	ImGui::End();
}
