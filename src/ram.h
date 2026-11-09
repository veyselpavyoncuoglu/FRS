#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

struct RamStats {
	uint64_t total,used,free; // MB
};

struct ProcMem {
	uint32_t pid;
	std::string name;
	uint64_t ws,priv; // bytes
};

RamStats getRamStats();
// labeled breakdown (standby/cache, modified, commit, kernel...), "_mb" keys are MB, others are counts
std::vector<std::pair<std::string,uint64_t>> getMemInfo();
std::vector<ProcMem> getProcs();
std::string flushStandby(int sz);
std::string emptyWorkSets();
std::string purgeStandby();
std::string flushModified();
bool isElevated();
