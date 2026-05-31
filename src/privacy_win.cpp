// Windows registry back-end for the privacy/telemetry tweaks (see privacy.h).
// Thin wrappers over the Reg* API. All subkeys/names/values handled here are
// ASCII, but the process is built UNICODE, so we widen to UTF-16 before each
// call. The 64-bit view is forced (KEY_WOW64_64KEY) to match optimizer, which
// uses RegistryView.Registry64 for the machine hive.
#include "privacy.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vector>

namespace {

	std::wstring widen(const char* s) {
		if (!s || !*s) return std::wstring();
		int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
		if (n <= 0) return std::wstring();
		std::wstring w(n - 1, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
		return w;
	}

	HKEY rootKey(bool hklm) {
		return hklm ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
	}

	// 64-bit view so we read/write the same place the Windows UI uses.
	const REGSAM kView = KEY_WOW64_64KEY;

}

namespace privacy { namespace backend {

	bool supported() { return true; }

	bool setDword(bool hklm, const char* subkey, const char* name, uint32_t value) {
		HKEY h = nullptr;
		std::wstring wk = widen(subkey);
		LONG r = RegCreateKeyExW(rootKey(hklm), wk.c_str(), 0, nullptr,
			REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | kView, nullptr, &h, nullptr);
		if (r != ERROR_SUCCESS) return false;
		DWORD v = (DWORD)value;
		r = RegSetValueExW(h, widen(name).c_str(), 0, REG_DWORD,
			reinterpret_cast<const BYTE*>(&v), sizeof(v));
		RegCloseKey(h);
		return r == ERROR_SUCCESS;
	}

	bool setString(bool hklm, const char* subkey, const char* name, const char* value) {
		HKEY h = nullptr;
		std::wstring wk = widen(subkey);
		LONG r = RegCreateKeyExW(rootKey(hklm), wk.c_str(), 0, nullptr,
			REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | kView, nullptr, &h, nullptr);
		if (r != ERROR_SUCCESS) return false;
		std::wstring wv = widen(value);
		DWORD cb = (DWORD)((wv.size() + 1) * sizeof(wchar_t));
		r = RegSetValueExW(h, widen(name).c_str(), 0, REG_SZ,
			reinterpret_cast<const BYTE*>(wv.c_str()), cb);
		RegCloseKey(h);
		return r == ERROR_SUCCESS;
	}

	bool deleteValue(bool hklm, const char* subkey, const char* name) {
		HKEY h = nullptr;
		LONG r = RegOpenKeyExW(rootKey(hklm), widen(subkey).c_str(), 0,
			KEY_SET_VALUE | kView, &h);
		if (r == ERROR_FILE_NOT_FOUND) return true; // key absent -> nothing to revert
		if (r != ERROR_SUCCESS) return false;
		r = RegDeleteValueW(h, widen(name).c_str());
		RegCloseKey(h);
		return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
	}

	bool getDword(bool hklm, const char* subkey, const char* name, uint32_t* out) {
		HKEY h = nullptr;
		if (RegOpenKeyExW(rootKey(hklm), widen(subkey).c_str(), 0,
				KEY_QUERY_VALUE | kView, &h) != ERROR_SUCCESS)
			return false;
		DWORD type = 0, data = 0, cb = sizeof(data);
		LONG r = RegQueryValueExW(h, widen(name).c_str(), nullptr, &type,
			reinterpret_cast<BYTE*>(&data), &cb);
		RegCloseKey(h);
		if (r == ERROR_SUCCESS && type == REG_DWORD) {
			if (out) *out = (uint32_t)data;
			return true;
		}
		return false;
	}

	bool getString(bool hklm, const char* subkey, const char* name, std::string* out) {
		HKEY h = nullptr;
		if (RegOpenKeyExW(rootKey(hklm), widen(subkey).c_str(), 0,
				KEY_QUERY_VALUE | kView, &h) != ERROR_SUCCESS)
			return false;
		wchar_t wname[256];
		std::wstring wn = widen(name);
		lstrcpynW(wname, wn.c_str(), 256);
		DWORD type = 0, cb = 0;
		LONG r = RegQueryValueExW(h, wname, nullptr, &type, nullptr, &cb);
		if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
			RegCloseKey(h);
			return false;
		}
		std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 1, L'\0');
		r = RegQueryValueExW(h, wname, nullptr, &type,
			reinterpret_cast<BYTE*>(buf.data()), &cb);
		RegCloseKey(h);
		if (r != ERROR_SUCCESS) return false;
		if (out) {
			std::wstring ws(buf.data());
			int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
			if (n > 0) {
				std::string s(n - 1, '\0');
				WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], n, nullptr, nullptr);
				*out = std::move(s);
			} else {
				out->clear();
			}
		}
		return true;
	}

}}
