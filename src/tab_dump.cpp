#include "vmscan.h"
#include "ui.h"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace {

	using namespace vmscan;

	const uint32_t DEF_CUSTOM = 0;
#ifdef _WIN32
	const char* const DUMP_EXT = "dmp";
#else
	const char* const DUMP_EXT = "core";
#endif

	struct Job {
		Prog pg;
		DumpRes res;
	};

	struct DS {
		ProcRef r;
		bool init = false;
		int preset = 2;
		uint32_t custom = DEF_CUSTOM;
		bool snap = false;
		char path[1024] = "";
		std::string autoPath;
		std::shared_ptr<Job> job;
		std::shared_ptr<task::T> t;
		bool seenEnd = true;
		char rAddr[32] = "";
		char rSize[32] = "";
		char rPath[1024] = "";
		std::string rAuto;
		std::shared_ptr<Job> rJob;
		std::shared_ptr<task::T> rT;
		bool rSeenEnd = true;
		std::string rWhy;
		// free disk and process size, asked off the UI thread
		std::shared_ptr<std::atomic<uint64_t>> est,fr;
		std::shared_ptr<task::T> estT,frT;
		std::string frPath;
		uint64_t frAt = 0,estAt = 0;
	};

	std::map<std::pair<uint32_t,uint64_t>,DS> g_ds;

	void askFree(DS& d) {
		std::string p = d.path;
		uint64_t now = util::nowMs();
		if (p == d.frPath && d.fr && now - d.frAt < 5000) return;
		if (d.frT && !d.frT->done) return;
		if (p != d.frPath && now - d.frAt < 400) return;
		d.frPath = p;
		d.frAt = now;
		auto f = d.fr ? d.fr : std::make_shared<std::atomic<uint64_t>>(NA);
		d.fr = f;
		d.frT = task::run([f,p](task::T&) {
			f->store(os::freeDisk(p));
			return std::string();
		},false);
	}

	// the process grows and shrinks, so the size is asked again like the free space
	void askEst(DS& d) {
		uint64_t now = util::nowMs();
		if ((d.estT && !d.estT->done) || (d.est && now - d.estAt < 5000)) return;
		d.estAt = now;
		auto est = d.est ? d.est : std::make_shared<std::atomic<uint64_t>>(NA);
		d.est = est;
		ProcRef r = d.r;
		d.estT = task::run([est,r](task::T&) {
			est->store(readableBytes(r));
			return std::string();
		},false);
	}

	uint32_t curFlags(const DS& d) {
		return d.preset < kPresetN ? kPresets[d.preset].flags : d.custom;
	}

	void startDump(DS& d) {
		auto job = std::make_shared<Job>();
		d.job = job;
		d.seenEnd = false;
		ProcRef r = d.r;
		std::string path = d.path;
		uint32_t fl = curFlags(d);
		bool snap = d.snap;
		d.t = task::run([job,r,path,fl,snap](task::T& t) -> std::string {
			job->res = writeDump(r,path,fl,snap,job->pg,[&t]() -> const char* {
				if (t.cancel.load()) return "cancelled";
				if (sampler::stopping()) return "FRS is closing";
				return nullptr;
			});
			std::string lb = "Create dump (pid " + std::to_string(r.pid) + ")";
			return lb + (job->res.ok ? " : OK" : " : failed (" + util::safe(job->res.err) + ")");
		});
	}

	bool parseSize(const char* t,uint64_t& out) {
		if (!*t) {
			out = 0;
			return true;
		}
		if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) return util::parseHex(t,out);
		char* e = nullptr;
		unsigned long long v = strtoull(t,&e,10);
		if (!e || *e || *t == '-') return false;
		out = v;
		return true;
	}

	void startRegion(DS& d) {
		d.rWhy.clear();
		uint64_t addr = 0,size = 0;
		if (!util::parseHex(d.rAddr,addr)) {
			d.rWhy = "Address has to be hex, like 0x7ff6a0000000";
			return;
		}
		if (!parseSize(d.rSize,size)) {
			d.rWhy = "Size has to be a number, or 0x hex, or empty for the whole region";
			return;
		}
		auto job = std::make_shared<Job>();
		d.rJob = job;
		d.rSeenEnd = false;
		ProcRef r = d.r;
		std::string path = d.rPath;
		d.rT = task::run([job,r,addr,size,path](task::T& t) -> std::string {
			job->res = saveRegion(r,addr,size,path,job->pg,[&t]() -> const char* {
				if (t.cancel.load()) return "cancelled";
				if (sampler::stopping()) return "FRS is closing";
				return nullptr;
			});
			std::string lb = "Save region (pid " + std::to_string(r.pid) + ", " + util::hex(addr) + ")";
			return lb + (job->res.ok ? " : OK" : " : failed (" + util::safe(job->res.err) + ")");
		});
	}

	void resultLine(const DumpRes& res) {
		if (res.ok) {
			ImGui::TextColored(ui::c.good,"Done, %s",util::size(res.size).c_str());
			ImGui::SameLine();
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(res.path.c_str());
			ImGui::PopTextWrapPos();
			if (res.unread) ImGui::TextColored(ui::c.warn,"%s could not be read and is zeros in the file",util::size(res.unread).c_str());
			if (!res.note.empty()) ui::hint(res.note.c_str());
			return;
		}
		if (res.cancelled) {
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(ui::c.warn,"Stopped (%s). %s",res.err.c_str(),res.note.empty() ? "The partial file was deleted." : res.note.c_str());
			ImGui::PopTextWrapPos();
			return;
		}
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextColored(ui::c.bad,"Unable to create the file : %s",res.err.c_str());
		ImGui::PopTextWrapPos();
	}

	// true the first time it sees the job ended
	bool endToast(const std::shared_ptr<Job>& j,bool& seen,const char* what) {
		if (seen) return false;
		seen = true;
		if (j->res.ok) ui::toast(std::string(what) + " written : " + util::size(j->res.size));
		else if (!j->res.cancelled) ui::toast(std::string(what) + " failed");
		return true;
	}

	void drawMini(DS& d,bool alive) {
		bool busy = d.t && !d.t->done;
		bool ended = d.t && d.t->done && endToast(d.job,d.seenEnd,"Dump");
		ui::beginCard("##dmini","Create dump file");
#ifdef _WIN32
		static const char* const names[] = {"Minimal","Limited","Normal","Full","Custom"};
		const ImVec4 tints[5] = {ui::c.accent,ui::c.accent,ui::c.accent,ui::c.accent,ui::c.accent};
		ui::segmented("##dpre",names,tints,5,d.preset);
		ui::hint(d.preset < kPresetN ? kPresets[d.preset].tip : "Pick what goes in. Normal adds nothing on its own, it is the basic dump every dump starts from.");
		if (d.preset == kPresetN) {
			if (ImGui::BeginTable("##dfl",2,ImGuiTableFlags_SizingStretchSame)) {
				for (int i = 0; i < kFlagN; i++) {
					if (!kFlags[i].common) continue;
					ImGui::TableNextColumn();
					bool on = kFlags[i].v ? (d.custom & kFlags[i].v) != 0 : d.custom == 0;
					if (ImGui::Checkbox(kFlags[i].label,&on)) {
						if (!kFlags[i].v) d.custom = 0;
						else if (on) d.custom |= kFlags[i].v;
						else d.custom &= ~kFlags[i].v;
					}
				}
				ImGui::EndTable();
			}
		}
		ImGui::Checkbox("Use process snapshot##d",&d.snap);
		ImGui::SameLine();
		ui::help("Dumps a frozen clone, so the process itself stops for a much shorter time. Falls back to the live process when the OS refuses, and says so.");
		bool full = (curFlags(d) & FULLMEM) != 0;
		const char* fullCap = "Memory in this dump, about";
#else
		ui::hint("Writes a core file with gcore, which comes with gdb. The process stops while it runs. The Windows minidump presets and flags don't apply here.");
		bool full = true;
		const char* fullCap = "Core file, at most about";
#endif
		ImGui::TextDisabled("Save to");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##dpath",d.path,sizeof(d.path));
#ifndef _WIN32
		ImGui::TextDisabled("gcore adds the pid, the file is %s.%u",d.path,d.r.pid);
#endif
		askFree(d);
		if (full && alive) askEst(d);
		uint64_t fr = d.fr ? d.fr->load() : NA;
		uint64_t est = d.est ? d.est->load() : NA;
		ImGui::TextDisabled("Free on that disk");
		ImGui::SameLine();
		ImGui::TextUnformatted(util::size(fr).c_str());
		if (full) {
			ImGui::SameLine(0.0f,ui::px(18.0f));
			ImGui::TextDisabled("%s",fullCap);
			ImGui::SameLine();
			ImGui::TextUnformatted(util::size(est).c_str());
		}
		bool tight = full && est != NA && fr != NA && est > fr;
		if (tight) ImGui::TextColored(ui::c.warn,"That may not fit. A failed dump deletes its file.");
		ImGui::BeginDisabled(busy || !alive);
		if (ImGui::Button("Save##d")) {
			if (tight) ui::askOpen("##dfit");
			else startDump(d);
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!busy);
		if (ImGui::Button("Cancel##d")) d.t->cancel = true;
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(busy);
		if (ImGui::Button("New name##d")) {
			d.autoPath = newName(d.r,DUMP_EXT);
			snprintf(d.path,sizeof(d.path),"%s",d.autoPath.c_str());
		}
		ImGui::EndDisabled();
		std::string askTxt = "This dump could need about " + util::size(est) + " and that disk has " + util::size(fr) + " free. Try anyway?";
		if (ui::askDraw("##dfit",askTxt.c_str(),"Try anyway")) startDump(d);
		if (busy) {
			std::string m = d.t->cancel.load() ? "Cancelling..." : d.job->pg.text();
			ui::meter(d.job->pg.frac.load(),m.c_str(),ImVec2(0.0f,0.0f),ui::c.accent);
		} else if (d.t && d.t->done) {
			resultLine(d.job->res);
			if (ended && d.job->res.ok && std::string(d.path) == d.autoPath) {
				d.autoPath = newName(d.r,DUMP_EXT);
				snprintf(d.path,sizeof(d.path),"%s",d.autoPath.c_str());
			}
		}
		ImGui::Spacing();
		ui::hint("A dump holds everything the process had in memory, passwords too. It never overwrites a file, an existing name fails. Closing this window cancels a running dump.");
		ui::endCard();
	}

	void drawRegionCard(DS& d,bool alive) {
		bool busy = d.rT && !d.rT->done;
		bool ended = d.rT && d.rT->done && endToast(d.rJob,d.rSeenEnd,"Region");
		ui::beginCard("##dreg","Save a memory region");
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Address");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ui::px(170.0f));
		ImGui::InputTextWithHint("##draddr","0x7ff6a0000000",d.rAddr,sizeof(d.rAddr));
		ImGui::SameLine(0.0f,ui::px(14.0f));
		ImGui::TextDisabled("Size");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ui::px(170.0f));
		ImGui::InputTextWithHint("##drsize","empty : whole region",d.rSize,sizeof(d.rSize));
		ImGui::TextDisabled("Save to");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##drpath",d.rPath,sizeof(d.rPath));
		ImGui::BeginDisabled(busy || !alive);
		if (ImGui::Button("Save##dr")) startRegion(d);
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!busy);
		if (ImGui::Button("Cancel##dr")) d.rT->cancel = true;
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(busy);
		if (ImGui::Button("New name##dr")) {
			d.rAuto = newName(d.r,"bin");
			snprintf(d.rPath,sizeof(d.rPath),"%s",d.rAuto.c_str());
		}
		ImGui::EndDisabled();
		if (!d.rWhy.empty()) ImGui::TextColored(ui::c.bad,"%s",d.rWhy.c_str());
		if (busy) ui::meter(d.rJob->pg.frac.load(),d.rT->cancel.load() ? "Cancelling..." : "Saving region...",ImVec2(0.0f,0.0f),ui::c.accent);
		else if (d.rT && d.rT->done) {
			resultLine(d.rJob->res);
			if (ended && d.rJob->res.ok && std::string(d.rPath) == d.rAuto) {
				d.rAuto = newName(d.r,"bin");
				snprintf(d.rPath,sizeof(d.rPath),"%s",d.rAuto.c_str());
			}
		}
		ImGui::Spacing();
		ui::hint("Raw bytes as they sit in memory. Pages that can't be read become zeros and get counted. Closing this window cancels a running save.");
		ui::endCard();
	}

	void drawTab(const ProcRef& r) {
		if (g_ds.size() > 64) {
			for (auto it = g_ds.begin(); it != g_ds.end();) {
				DS& x = it->second;
				bool idle = (!x.t || x.t->done) && (!x.rT || x.rT->done) && (!x.estT || x.estT->done) && (!x.frT || x.frT->done);
				bool mine = x.r == r;
				if (idle && !mine && !procs::alive(x.r)) it = g_ds.erase(it);
				else ++it;
			}
		}
		DS& d = g_ds[std::make_pair(r.pid,r.start)];
		if (!d.init) {
			d.init = true;
			d.r = r;
			d.autoPath = newName(r,DUMP_EXT);
			snprintf(d.path,sizeof(d.path),"%s",d.autoPath.c_str());
			d.rAuto = newName(r,"bin");
			snprintf(d.rPath,sizeof(d.rPath),"%s",d.rAuto.c_str());
		}
		bool alive = procs::alive(r);
		ImGui::BeginChild("##dscroll",ImVec2(0,0));
		if (dumpAvail()) {
			if (r.pid == procs::be::selfPid()) {
				ui::beginCard("##dself","Create dump file");
#ifdef _WIN32
				ui::hint("FRS can't write a minidump of itself : dbghelp freezes every other thread of the process it runs in, and a frozen thread can hold a lock it needs. Saving a region below works.");
#else
				ui::hint("FRS can't write a core file of itself : gcore would stop the process that waits for it. Saving a region below works.");
#endif
				ui::endCard();
			} else drawMini(d,alive);
		} else {
			ui::beginCard("##dno","Create dump file");
			ui::hint("Core files need gcore, not available : install gdb and restart FRS. Saving a region below reads /proc/pid/mem and works.");
			ui::endCard();
		}
		ImGui::Spacing();
		drawRegionCard(d,alive);
		ImGui::EndChild();
	}

}

namespace vmscan {

	void initDump() {
		procwin::addTab("Dump",50,drawTab);
	}

	void dumpWins() {
		for (auto& kv : g_ds) {
			DS& d = kv.second;
			bool run = d.t && !d.t->done,rRun = d.rT && !d.rT->done;
			if ((!run && !rRun) || procwin::isOpen(d.r)) continue;
			if (run) d.t->cancel = true;
			if (rRun) d.rT->cancel = true;
		}
	}

}
