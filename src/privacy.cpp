// Privacy/telemetry tweak data + Privacy tab UI. Platform-neutral: it only ever
// calls the privacy::backend registry interface (privacy_win.cpp /
// privacy_linux.cpp) and ImGui. No OS headers here.
//
// The tweaks are a faithful port of the registry operations in hellzerg/optimizer
// (Optimizer/OptimizeHelper.cs): the EnhancePrivacy / CompromisePrivacy bundle
// plus the registry-only privacy bits of DisableTelemetryServices and
// DisableCortana. Only pure value writes were taken; the upstream service-Start
// edits, scheduled-task scripts and command-line calls were intentionally left
// out so that every tweak reverts to the Windows default by simply deleting the
// value (which is exactly what CompromisePrivacy does).
#include "privacy.h"
#include "ram.h"      // isElevated()
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

	constexpr bool HKLM = true;
	constexpr bool HKCU = false;

	enum RType { RDWORD, RSTR };

	struct Op {
		bool hklm;
		const char* key;
		const char* name;
		RType type;
		uint32_t dw;
		const char* sv;
	};

	constexpr Op D(bool hklm, const char* key, const char* name, uint32_t dw) {
		return Op{ hklm, key, name, RDWORD, dw, nullptr };
	}
	constexpr Op S(bool hklm, const char* key, const char* name, const char* sv) {
		return Op{ hklm, key, name, RSTR, 0u, sv };
	}

	struct Tweak {
		const char* name;
		const char* desc;
		const Op* ops;
		int n;
	};

#define NOPS(a) (int)(sizeof(a) / sizeof((a)[0]))

	// --- 1. Telemetry & diagnostics ----------------------------------------
	const Op ops_telemetry[] = {
		D(HKLM, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\DataCollection", "AllowTelemetry", 0),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", "AllowTelemetry", 0),
		D(HKLM, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\DataCollection", "MaxTelemetryAllowed", 0),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", "DoNotShowFeedbackNotifications", 1),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\SQMClient\\Windows", "CEIPEnable", 0),
		D(HKCU, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Diagnostics\\DiagTrack", "ShowedToastAtLevel", 1),
		D(HKLM, "SYSTEM\\CurrentControlSet\\Control\\WMI\\AutoLogger\\AutoLogger-Diagtrack-Listener", "Start", 0),
		D(HKLM, "SYSTEM\\ControlSet001\\Control\\WMI\\AutoLogger\\AutoLogger-Diagtrack-Listener", "Start", 0),
	};

	// --- 2. Activity history & timeline ------------------------------------
	const Op ops_activity[] = {
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\System", "PublishUserActivities", 0),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\System", "UploadUserActivities", 0),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\System", "EnableActivityFeed", 0),
	};

	// --- 3. Location tracking ----------------------------------------------
	const Op ops_location[] = {
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\LocationAndSensors", "DisableLocation", 1),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\LocationAndSensors", "DisableLocationScripting", 1),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\LocationAndSensors", "DisableWindowsLocationProvider", 1),
		S(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\location", "Value", "Deny"),
		D(HKLM, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Sensor\\Overrides\\{BFA794E4-F964-4FDB-90F6-51056BFE4B44}", "SensorPermissionState", 0),
		D(HKLM, "System\\CurrentControlSet\\Services\\lfsvc\\Service\\Configuration", "Status", 0),
	};

	// --- 4. Advertising ID -------------------------------------------------
	const Op ops_advertising[] = {
		D(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", "Enabled", 0),
		D(HKLM, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", "Enabled", 0),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\AdvertisingInfo", "DisabledByGroupPolicy", 1),
	};

	// --- 5. Tailored experiences & Spotlight -------------------------------
	const Op ops_tailored[] = {
		D(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", "RotatingLockScreenOverlayEnabled", 0),
		D(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", "RotatingLockScreenEnabled", 0),
		D(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", "DisableWindowsSpotlightFeatures", 1),
		D(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", "DisableTailoredExperiencesWithDiagnosticData", 1),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent", "DisableCloudOptimizedContent", 1),
		D(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\Privacy", "TailoredExperiencesWithDiagnosticDataEnabled", 0),
	};

	// --- 6. Feedback requests ----------------------------------------------
	const Op ops_feedback[] = {
		D(HKCU, "Software\\Microsoft\\Siuf\\Rules", "PeriodInNanoSeconds", 0),
		D(HKCU, "Software\\Microsoft\\Siuf\\Rules", "NumberOfSIUFInPeriod", 0),
	};

	// --- 7. Speech, inking & typing ----------------------------------------
	const Op ops_inktype[] = {
		D(HKLM, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\TextInput", "AllowLinguisticDataCollection", 0),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\InputPersonalization", "AllowInputPersonalization", 0),
		D(HKCU, "SOFTWARE\\Microsoft\\Personalization\\Settings", "AcceptedPrivacyPolicy", 0),
		D(HKCU, "SOFTWARE\\Microsoft\\InputPersonalization", "RestrictImplicitTextCollection", 1),
		D(HKCU, "SOFTWARE\\Microsoft\\InputPersonalization", "RestrictImplicitInkCollection", 1),
		D(HKCU, "SOFTWARE\\Microsoft\\InputPersonalization\\TrainedDataStore", "HarvestContacts", 0),
		D(HKCU, "SOFTWARE\\Microsoft\\Input\\TIPC", "Enabled", 0),
		D(HKCU, "Software\\Microsoft\\Speech_OneCore\\Settings\\OnlineSpeechPrivacy", "HasAccepted", 0),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\TabletPC", "PreventHandwritingDataSharing", 1),
	};

	// --- 8. Cloud clipboard & shared experiences ---------------------------
	const Op ops_clipboard[] = {
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\System", "AllowCrossDeviceClipboard", 0),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\System", "EnableCdp", 0),
		D(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\CDP", "CdpSessionUserAuthzPolicy", 0),
		D(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\CDP", "NearShareChannelUserAuthzPolicy", 0),
		D(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\CDP", "RomeSdkChannelUserAuthzPolicy", 0),
		D(HKLM, "Software\\Policies\\Microsoft\\Windows\\Messaging", "AllowMessageSync", 0),
	};

	// --- 9. App-compat telemetry & inventory -------------------------------
	const Op ops_appcompat[] = {
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\AppCompat", "AITEnable", 0),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\AppCompat", "DisableInventory", 1),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\AppCompat", "DisablePCA", 1),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\AppCompat", "DisableUAR", 1),
		D(HKLM, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Device Metadata", "PreventDeviceMetadataFromNetwork", 1),
		D(HKLM, "SOFTWARE\\Microsoft\\PolicyManager\\current\\device\\System", "AllowExperimentation", 0),
	};

	// --- 10. Cortana & web search ------------------------------------------
	const Op ops_cortana[] = {
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search", "AllowCortana", 0),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search", "DisableWebSearch", 1),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search", "ConnectedSearchUseWeb", 0),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search", "ConnectedSearchUseWebOverMeteredConnections", 0),
		D(HKLM, "SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search", "AllowCloudSearch", 0),
		D(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\Search", "BingSearchEnabled", 0),
		D(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\Search", "CortanaConsent", 0),
		D(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\Search", "AllowSearchToUseLocation", 0),
		D(HKCU, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Search", "DeviceHistoryEnabled", 0),
		D(HKCU, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Search", "HistoryViewEnabled", 0),
		D(HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\SearchSettings", "IsDeviceSearchHistoryEnabled", 0),
	};

	const Tweak kTweaks[] = {
		{ "Telemetry & Diagnostics",
		  "Lowest diagnostic-data level, no feedback prompts, CEIP off, DiagTrack autologger stopped.",
		  ops_telemetry, NOPS(ops_telemetry) },
		{ "Activity History & Timeline",
		  "Stops collection and upload of your activity history (Timeline).",
		  ops_activity, NOPS(ops_activity) },
		{ "Location Tracking",
		  "Turns off the location platform, location sensors and per-user location consent.",
		  ops_location, NOPS(ops_location) },
		{ "Advertising ID",
		  "Disables the per-user advertising ID used to personalise ads across apps.",
		  ops_advertising, NOPS(ops_advertising) },
		{ "Tailored Experiences & Spotlight",
		  "Disables Spotlight, lock-screen suggestions and diagnostics-based tailored experiences.",
		  ops_tailored, NOPS(ops_tailored) },
		{ "Feedback Requests",
		  "Sets the Windows feedback frequency to never.",
		  ops_feedback, NOPS(ops_feedback) },
		{ "Speech, Inking & Typing",
		  "Stops collection of handwriting, typing and online-speech samples for personalisation.",
		  ops_inktype, NOPS(ops_inktype) },
		{ "Cloud Clipboard & Shared Experiences",
		  "Disables cross-device clipboard, Shared Experiences (CDP) and message sync.",
		  ops_clipboard, NOPS(ops_clipboard) },
		{ "App-Compat Telemetry & Inventory",
		  "Disables the application-compatibility inventory/telemetry agent and device-metadata fetch.",
		  ops_appcompat, NOPS(ops_appcompat) },
		{ "Cortana & Web Search",
		  "Disables Cortana, Bing/web results and search history in Start-menu search.",
		  ops_cortana, NOPS(ops_cortana) },
	};

	const int NTWEAKS = NOPS(kTweaks);

	enum Status { OFF, PARTIAL, ON };

	// UI state. File-scope so it persists across frames; the tab is the only
	// consumer and runs on the UI thread, so no synchronisation is needed.
	Status g_status[NTWEAKS];
	bool g_statusValid = false;
	std::vector<std::string> g_results;

	bool opApplied(const Op& o) {
		if (o.type == RDWORD) {
			uint32_t v = 0;
			return privacy::backend::getDword(o.hklm, o.key, o.name, &v) && v == o.dw;
		}
		std::string v;
		return privacy::backend::getString(o.hklm, o.key, o.name, &v) && v == o.sv;
	}

	Status tweakStatus(const Tweak& t) {
		int applied = 0;
		for (int i = 0; i < t.n; ++i)
			if (opApplied(t.ops[i])) ++applied;
		if (applied == 0) return OFF;
		if (applied == t.n) return ON;
		return PARTIAL;
	}

	int applyTweak(const Tweak& t) {
		int ok = 0;
		for (int i = 0; i < t.n; ++i) {
			const Op& o = t.ops[i];
			bool r = (o.type == RDWORD)
				? privacy::backend::setDword(o.hklm, o.key, o.name, o.dw)
				: privacy::backend::setString(o.hklm, o.key, o.name, o.sv);
			if (r) ++ok;
		}
		return ok;
	}

	int revertTweak(const Tweak& t) {
		int ok = 0;
		for (int i = 0; i < t.n; ++i)
			if (privacy::backend::deleteValue(t.ops[i].hklm, t.ops[i].key, t.ops[i].name)) ++ok;
		return ok;
	}

	void refreshAll() {
		for (int i = 0; i < NTWEAKS; ++i) g_status[i] = tweakStatus(kTweaks[i]);
		g_statusValid = true;
	}

	void addResult(const std::string& s) {
		g_results.push_back(s);
		while (g_results.size() > 200) g_results.erase(g_results.begin());
	}

}

namespace privacy {

	void drawTab() {
		ImGui::Spacing();

		const ImVec4 cyan(0.35f, 0.85f, 1.0f, 1.0f);
		const ImVec4 green(0.3f, 1.0f, 0.5f, 1.0f);
		const ImVec4 yellow(1.0f, 0.75f, 0.2f, 1.0f);
		const ImVec4 red(1.0f, 0.35f, 0.35f, 1.0f);
		const ImVec4 gray(0.7f, 0.7f, 0.7f, 1.0f);

		ImGui::TextColored(cyan, "Windows Privacy & Telemetry");

		if (!backend::supported()) {
			ImGui::Spacing();
			ImGui::TextWrapped("These are Windows registry tweaks and are not available on this platform.");
			return;
		}

		if (!g_statusValid) refreshAll();

		ImGui::TextDisabled("Registry tweaks ported from hellzerg/optimizer. Changes apply system-wide.");
		ImGui::TextDisabled("Apply writes the values; Revert deletes them to restore Windows defaults.");

		if (!isElevated()) {
			ImGui::TextColored(red, "[!] Not elevated - machine-wide (HKLM) tweaks will fail. Relaunch as admin.");
		}

		ImGui::Spacing();

		if (ImGui::Button("Refresh Status")) refreshAll();
		ImGui::SameLine();

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.2f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.75f, 0.28f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.45f, 0.15f, 1.0f));
		if (ImGui::Button("Apply All")) {
			int ok = 0, total = 0;
			for (int i = 0; i < NTWEAKS; ++i) { ok += applyTweak(kTweaks[i]); total += kTweaks[i].n; }
			refreshAll();
			char b[96];
			snprintf(b, sizeof(b), "Apply All: %d/%d values written.", ok, total);
			addResult(b);
		}
		ImGui::PopStyleColor(3);
		ImGui::SameLine();
		if (ImGui::Button("Revert All")) {
			int ok = 0, total = 0;
			for (int i = 0; i < NTWEAKS; ++i) { ok += revertTweak(kTweaks[i]); total += kTweaks[i].n; }
			refreshAll();
			char b[96];
			snprintf(b, sizeof(b), "Revert All: %d/%d values cleared.", ok, total);
			addResult(b);
		}

		ImGui::Spacing();
		ImGui::Separator();

		ImGui::BeginChild("##privlist", ImVec2(0, -150.0f), true);
		for (int i = 0; i < NTWEAKS; ++i) {
			const Tweak& t = kTweaks[i];
			Status s = g_status[i];
			const char* badge = (s == ON) ? "ON" : (s == PARTIAL) ? "PARTIAL" : "OFF";
			const ImVec4& col = (s == ON) ? green : (s == PARTIAL) ? yellow : gray;

			ImGui::PushID(i);
			ImGui::TextColored(col, "[%s]", badge);
			ImGui::SameLine(90.0f);
			ImGui::TextUnformatted(t.name);

			float btnsW = 130.0f;
			ImGui::SameLine(ImGui::GetContentRegionMax().x - btnsW);
			if (ImGui::SmallButton("Apply")) {
				int ok = applyTweak(t);
				g_status[i] = tweakStatus(t);
				char b[128];
				snprintf(b, sizeof(b), "%s: applied %d/%d.", t.name, ok, t.n);
				addResult(b);
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Revert")) {
				int ok = revertTweak(t);
				g_status[i] = tweakStatus(t);
				char b[128];
				snprintf(b, sizeof(b), "%s: reverted %d/%d.", t.name, ok, t.n);
				addResult(b);
			}

			ImGui::PushStyleColor(ImGuiCol_Text, gray);
			ImGui::PushTextWrapPos(0.0f);
			ImGui::Text("        %s  (%d keys)", t.desc, t.n);
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();

			ImGui::Spacing();
			ImGui::PopID();
		}
		ImGui::EndChild();

		ImGui::Text("Results :");
		ImGui::SameLine();
		if (ImGui::SmallButton("Clear##privres")) g_results.clear();

		ImGui::BeginChild("##privresults", ImVec2(0, 0), true);
		for (auto& e : g_results) {
			const char* c = e.c_str();
			ImVec4 lc = gray;
			if (strstr(c, "Apply") || strstr(c, "applied")) lc = green;
			else if (strstr(c, "Revert") || strstr(c, "reverted")) lc = yellow;
			ImGui::TextColored(lc, "%s", c);
		}
		ImGui::EndChild();
	}

}
