#pragma once
#include <cstdint>
#include <string>

// Windows privacy / telemetry registry tweaks, ported from hellzerg/optimizer
// (EnhancePrivacy / CompromisePrivacy and the registry-only parts of
// DisableTelemetryServices / DisableCortana). Self-contained add-on module:
// the only contact point with the shared core is drawTab(), called from one
// extra BeginTabItem in app.cpp. Nothing here touches the existing FRS code.
//
// "Apply" writes the privacy values; "Revert" deletes them so Windows falls
// back to its defaults (exactly what optimizer's CompromisePrivacy does). Every
// tweak is a plain registry value, so reverting is always a clean delete.
namespace privacy {

	// Emit the Privacy tab body. Assumes an ImGui frame/window is already active
	// (it is invoked from inside the app's tab bar). No-op-safe on Linux, where
	// it just explains that the feature is Windows-only.
	void drawTab();

	// Registry back-end. Platform-specific: implemented in privacy_win.cpp on
	// Windows and stubbed in privacy_linux.cpp. The data model (privacy.cpp)
	// talks only to these; it never includes any OS header. All subkeys are
	// given relative to the chosen root (hklm == true -> HKEY_LOCAL_MACHINE,
	// false -> HKEY_CURRENT_USER), matching optimizer's localMachine flag.
	namespace backend {

		// True only where the registry exists (Windows). The UI hides the
		// controls and shows a notice when this is false.
		bool supported();

		// Create the subkey if needed and write the value. Return true on success.
		bool setDword(bool hklm, const char* subkey, const char* name, uint32_t value);
		bool setString(bool hklm, const char* subkey, const char* name, const char* value);

		// Delete a single value. Treated as success when the key/value is already
		// absent (nothing to revert).
		bool deleteValue(bool hklm, const char* subkey, const char* name);

		// Read helpers for status detection. Return true only when the value
		// exists with the matching type; *out then holds its data.
		bool getDword(bool hklm, const char* subkey, const char* name, uint32_t* out);
		bool getString(bool hklm, const char* subkey, const char* name, std::string* out);

	}

}
