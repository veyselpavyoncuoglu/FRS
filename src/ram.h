#pragma once
#include <cstdint>
#include <string>

struct RamStats {
	uint64_t total,used,free; // MB
};

RamStats getRamStats();
std::string flushStandby(int sz);
std::string emptyWorkSets();
std::string purgeStandby();
std::string flushModified();
bool isElevated();
