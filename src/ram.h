#pragma once
#include <windows.h>
#include <string>

struct RamStats {
	DWORDLONG total,used,free; // MB
};

RamStats getRamStats();
std::string flushStandby(int sz);
std::string emptyWorkSets();
std::string purgeStandby();
std::string flushModified();
bool isElevated();