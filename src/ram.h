#pragma once
#include <windows.h>
#include <string>

struct RamStats {
	DWORDLONG total,used,free; // MB
};

RamStats getRamStats();
std::string flushStandby(int szMB);
std::string emptyWorkSets();
bool isElevated();
