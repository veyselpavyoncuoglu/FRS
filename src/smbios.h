#pragma once
#include "util.h"
#include <string>
#include <vector>

// RAM records of the SMBIOS table (types 16, 17, 18, 19, 20). One parser, both OSes hand it the raw table.
namespace smbios {

	struct Field {
		std::string key,label,val; // val empty : unknown, UI N/A, MCP null
		uint64_t num = NA;
		bool isNum = false; // MCP sends num, otherwise val
	};

	struct Rec {
		uint16_t handle = 0;
		std::string title;
		std::vector<Field> f;
		const Field* get(const char* key) const;
		std::string str(const char* key) const; // "" when unknown
		uint64_t num(const char* key) const; // NA when unknown
	};

	struct Table {
		bool loaded = false,ok = false;
		std::string err;
		int major = 0,minor = 0;
		std::vector<Rec> arrays,devices,errors,arrayMaps,deviceMaps;
		int slotsUsed = 0,slotsTotal = 0; // system memory only, video and flash arrays don't count
		uint64_t installed = NA;
		uint64_t speed = NA; // highest across sticks, MT/s
		std::string formFactor,memType,tech; // "Mixed" when the sticks differ
	};

	// n bytes of structures, header stripped. Never reads past n.
	Table parse(const uint8_t* d,size_t n,int major,int minor);

	void load(); // first call reads the table, later calls return at once. Any thread, not the UI one.
	std::shared_ptr<const Table> latest(); // never null, loaded false until load finished

	// OS part in sysmem_win.cpp and sysmem_linux.cpp : raw structures, false with err filled
	bool osRead(std::vector<uint8_t>& out,int& major,int& minor,std::string& err);

}
