// Linux stub for the privacy back-end. There is no Windows registry here, so
// every operation reports "unsupported". privacy.cpp checks supported() and
// shows a Windows-only notice in the tab instead of any controls.
#include "privacy.h"

namespace privacy { namespace backend {

	bool supported() { return false; }

	bool setDword(bool, const char*, const char*, uint32_t) { return false; }
	bool setString(bool, const char*, const char*, const char*) { return false; }
	bool deleteValue(bool, const char*, const char*) { return false; }
	bool getDword(bool, const char*, const char*, uint32_t*) { return false; }
	bool getString(bool, const char*, const char*, std::string*) { return false; }

}}
