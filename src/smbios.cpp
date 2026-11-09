#include "smbios.h"
#include <cstdio>
#include <cstring>

namespace smbios {

	namespace {

		const char* const kLoc[] = {"","Other","Unknown","Motherboard","ISA","EISA","PCI","MCA","PCMCIA","Proprietary","NuBus"};
		const char* const kLoc98[] = {"PC-98/C20","PC-98/C24","PC-98/E","PC-98/Local bus","CXL"};
		const char* const kUse[] = {"","Other","Unknown","System","Video","Flash","Non-volatile","Cache"};
		const char* const kEcc[] = {"","Other","Unknown","None","Parity","Single-bit ECC","Multi-bit ECC","CRC"};
		const char* const kForm[] = {"","Other","Unknown","SIMM","SIP","Chip","DIP","ZIP","Proprietary","DIMM","TSOP","Row of chips","RIMM","SODIMM","SRIMM","FB-DIMM","Die","CAMM"};
		const char* const kType[] = {
			"","Other","Unknown","DRAM","EDRAM","VRAM","SRAM","RAM","ROM","FLASH","EEPROM","FEPROM","EPROM","CDRAM","3DRAM","SDRAM","SGRAM","RDRAM",
			"DDR","DDR2","DDR2 FB-DIMM","","","","DDR3","FBD2","DDR4","LPDDR","LPDDR2","LPDDR3","LPDDR4","Logical non-volatile device","HBM","HBM2","DDR5","LPDDR5","HBM3"
		};
		const char* const kTech[] = {"","Other","Unknown","DRAM","NVDIMM-N","NVDIMM-F","NVDIMM-P","Intel Optane","MRDIMM"};
		const char* const kErrT[] = {"","Other","Unknown","Ok","Bad read","Parity","Single-bit","Double-bit","Multi-bit","Nibble","Checksum","CRC","Corrected single-bit","Corrected","Uncorrectable"};
		const char* const kErrG[] = {"","Other","Unknown","Device","Partition"};
		const char* const kErrO[] = {"","Other","Unknown","Read","Write","Partial write"};
		const char* const kDetail[] = {
			"Reserved","Other","Unknown","Fast-paged","Static column","Pseudo-static","RAMBUS","Synchronous",
			"CMOS","EDO","Window DRAM","Cache DRAM","Non-volatile","Buffered","Unbuffered","LRDIMM"
		};
		const char* const kMode[] = {"Reserved","Other","Unknown","Volatile","Byte-accessible persistent","Block-accessible persistent"};

		template<size_t N> std::string en(const char* const (&t)[N],uint8_t v) {
			if (v < N && *t[v]) return t[v];
			char b[32];
			snprintf(b,sizeof(b),"Undefined (0x%02X)",v);
			return b;
		}

		std::string locName(uint8_t v) {
			if (v >= 160 && v < 165) return kLoc98[v - 160];
			return en(kLoc,v);
		}

		std::string hx(uint64_t v,int w) {
			char b[32];
			snprintf(b,sizeof(b),"0x%0*llX",w,(unsigned long long)v);
			return b;
		}

		// one record : bounds checked reads plus the field list
		struct R {
			const uint8_t* p;
			int len;
			const std::vector<std::string>& strs;
			Rec& out;

			bool has(int off,int n) const { return off + n <= len; }
			uint8_t u8(int o) const { return p[o]; }
			uint16_t u16(int o) const { return (uint16_t)(p[o] | p[o + 1] << 8); }
			uint32_t u32(int o) const { return (uint32_t)u16(o) | (uint32_t)u16(o + 2) << 16; }
			uint64_t u64(int o) const { return (uint64_t)u32(o) | (uint64_t)u32(o + 4) << 32; }

			Field& add(const char* label,const char* key) {
				out.f.emplace_back();
				Field& f = out.f.back();
				f.label = label;
				f.key = key;
				return f;
			}
			void text(const char* label,const char* key,const std::string& v) {
				add(label,key).val = v;
			}
			void numTxt(const char* label,const char* key,uint64_t v,const std::string& txt) {
				Field& f = add(label,key);
				f.isNum = true;
				if (v == NA) return;
				f.num = v;
				f.val = txt;
			}
			void size(const char* label,const char* key,uint64_t v) {
				numTxt(label,key,v,util::size(v));
			}
			void str(const char* label,const char* key,int o) {
				std::string v;
				if (has(o,1) && u8(o) && (size_t)u8(o) <= strs.size()) v = strs[u8(o) - 1];
				text(label,key,v);
			}
			void hex16(const char* label,const char* key,int o,uint16_t none) {
				std::string v;
				if (has(o,2) && u16(o) != none) v = hx(u16(o),4);
				text(label,key,v);
			}
			// handle fields of error records, 0xFFFE and 0xFFFF have a meaning of their own
			void errHandle(const char* label,const char* key,int o) {
				std::string v;
				if (has(o,2)) {
					uint16_t h = u16(o);
					v = h == 0xFFFE ? "Not provided" : h == 0xFFFF ? "No error detected" : hx(h,4);
				}
				text(label,key,v);
			}
			void flags(const char* label,const char* key,int o,const char* const* names,int n) {
				std::string v;
				if (has(o,2)) {
					for (int i = 0; i < n; i++) if (u16(o) >> i & 1) v += (v.empty() ? "" : ", ") + std::string(names[i]);
				}
				text(label,key,v);
			}
			// 16 bit speed, 0 unknown, 0xFFFF look at the 32 bit field
			void speed(const char* label,const char* key,int o,int ext) {
				uint64_t v = NA;
				if (has(o,2) && u16(o)) {
					if (u16(o) != 0xFFFF) v = u16(o);
					else if (has(ext,4) && (u32(ext) & 0x7FFFFFFF)) v = u32(ext) & 0x7FFFFFFF;
				}
				numTxt(label,key,v,std::to_string(v) + " MT/s");
			}
			void volts(const char* label,const char* key,int o) {
				uint64_t v = has(o,2) && u16(o) ? u16(o) : NA;
				char b[32];
				snprintf(b,sizeof(b),"%.2f V",(double)v / 1000.0);
				numTxt(label,key,v,b);
			}
			void q(const char* label,const char* key,int o) {
				uint64_t v = has(o,8) && u64(o) != UINT64_MAX ? u64(o) : NA;
				size(label,key,v);
			}
			void u32Unk(const char* label,const char* key,int o,uint32_t unk) {
				std::string v;
				if (has(o,4) && u32(o) != unk) v = hx(u32(o),8);
				text(label,key,v);
			}
			// row position and friends, 0 and 0xFF both mean no value
			void pos(const char* label,const char* key,int o) {
				uint64_t v = has(o,1) && u8(o) && u8(o) != 0xFF ? u8(o) : NA;
				numTxt(label,key,v,std::to_string(v));
			}
			// mapped address pair : KB at o, bytes at ext when the KB field is all ones.
			// The KB end names the last KB, so its last byte is 1023 further on.
			void range(int sOff,int eOff,int sExt,int eExt) {
				uint64_t a = NA;
				uint64_t b = NA;
				if (has(sOff,4)) {
					if (u32(sOff) != UINT32_MAX) a = (uint64_t)u32(sOff) * 1024;
					else if (has(sExt,8)) a = u64(sExt);
				}
				if (has(eOff,4)) {
					if (u32(eOff) != UINT32_MAX) b = (uint64_t)u32(eOff) * 1024 + 1023;
					else if (has(eExt,8)) b = u64(eExt);
				}
				text("Starting address","start_address",a == NA ? "" : util::hex(a));
				text("Ending address","end_address",b == NA ? "" : util::hex(b));
				size("Range size","range_bytes",a != NA && b != NA && b >= a ? b - a + 1 : NA);
			}
		};

		void array16(R& r) {
			r.text("Handle","handle",hx(r.out.handle,4));
			r.text("Location","location",r.has(4,1) ? locName(r.u8(4)) : "");
			r.text("Use","use",r.has(5,1) ? en(kUse,r.u8(5)) : "");
			r.text("Error correction","error_correction",r.has(6,1) ? en(kEcc,r.u8(6)) : "");
			uint64_t cap = NA;
			if (r.has(7,4)) {
				if (r.u32(7) != 0x80000000) cap = (uint64_t)r.u32(7) * 1024;
				else if (r.has(0x0F,8)) cap = r.u64(0x0F);
			}
			r.size("Maximum capacity","max_capacity_bytes",cap);
			r.errHandle("Error information handle","error_handle",0x0B);
			uint64_t n = r.has(0x0D,2) ? r.u16(0x0D) : NA;
			r.numTxt("Number of memory devices","device_count",n,std::to_string(n));
		}

		void device17(R& r) {
			r.text("Handle","handle",hx(r.out.handle,4));
			r.str("Device locator","device_locator",0x10);
			r.str("Bank locator","bank_locator",0x11);
			r.hex16("Physical array handle","array_handle",4,0xFFFF);
			r.errHandle("Memory error information handle","error_handle",6);
			uint64_t tw = r.has(8,2) && r.u16(8) != 0xFFFF ? r.u16(8) : NA;
			uint64_t dw = r.has(0x0A,2) && r.u16(0x0A) != 0xFFFF ? r.u16(0x0A) : NA;
			r.numTxt("Total width","total_width_bits",tw,std::to_string(tw) + " bits");
			r.numTxt("Data width","data_width_bits",dw,std::to_string(dw) + " bits");

			// 0 empty slot, 0xFFFF unknown, 0x7FFF look at the 32 bit MB field, bit 15 picks KB or MB
			uint64_t sz = NA;
			if (r.has(0x0C,2)) {
				uint16_t v = r.u16(0x0C);
				if (v == 0) sz = 0;
				else if (v == 0x7FFF) {
					if (r.has(0x1C,4)) sz = (uint64_t)(r.u32(0x1C) & 0x7FFFFFFF) << 20;
				} else if (v != 0xFFFF) sz = v & 0x8000 ? (uint64_t)(v & 0x7FFF) << 10 : (uint64_t)v << 20;
			}
			r.numTxt("Size","size_bytes",sz,sz == 0 ? "No module installed" : util::size(sz));

			r.text("Form factor","form_factor",r.has(0x0E,1) ? en(kForm,r.u8(0x0E)) : "");
			uint64_t set = r.has(0x0F,1) && r.u8(0x0F) && r.u8(0x0F) != 0xFF ? r.u8(0x0F) : NA;
			r.numTxt("Device set","device_set",set,std::to_string(set));
			r.text("Memory type","memory_type",r.has(0x12,1) ? en(kType,r.u8(0x12)) : "");
			r.flags("Type detail","type_detail",0x13,kDetail,16);
			r.speed("Speed","speed_mts",0x15,0x54);
			r.speed("Configured speed","configured_speed_mts",0x20,0x58);
			r.str("Manufacturer","manufacturer",0x17);
			r.str("Serial number","serial_number",0x18);
			r.str("Asset tag","asset_tag",0x19);
			r.str("Part number","part_number",0x1A);
			uint64_t rank = r.has(0x1B,1) && (r.u8(0x1B) & 0x0F) ? r.u8(0x1B) & 0x0F : NA;
			r.numTxt("Rank","rank",rank,std::to_string(rank));
			r.volts("Minimum voltage","min_voltage_mv",0x22);
			r.volts("Maximum voltage","max_voltage_mv",0x24);
			r.volts("Configured voltage","configured_voltage_mv",0x26);
			r.text("Technology","technology",r.has(0x28,1) ? en(kTech,r.u8(0x28)) : "");
			r.flags("Mode capabilities","mode_capabilities",0x29,kMode,6);
			r.str("Firmware version","firmware_version",0x2B);
			r.hex16("Module manufacturer ID","module_manufacturer_id",0x2C,0);
			r.hex16("Module product ID","module_product_id",0x2E,0);
			r.hex16("Subsystem controller manufacturer ID","subsystem_manufacturer_id",0x30,0);
			r.hex16("Subsystem controller product ID","subsystem_product_id",0x32,0);
			r.q("Non-volatile size","non_volatile_bytes",0x34);
			r.q("Volatile size","volatile_bytes",0x3C);
			r.q("Cache size","cache_bytes",0x44);
			r.q("Logical size","logical_bytes",0x4C);
			r.hex16("PMIC0 manufacturer ID","pmic_manufacturer_id",0x5C,0);
			r.hex16("PMIC0 revision","pmic_revision",0x5E,0xFF00);
			r.hex16("RCD manufacturer ID","rcd_manufacturer_id",0x60,0);
			r.hex16("RCD revision","rcd_revision",0x62,0xFF00);

			std::string loc = r.out.str("device_locator");
			r.out.title = loc.empty() ? "Memory device " + hx(r.out.handle,4) : loc;
		}

		void error18(R& r) {
			r.text("Handle","handle",hx(r.out.handle,4));
			r.text("Type","type",r.has(4,1) ? en(kErrT,r.u8(4)) : "");
			r.text("Granularity","granularity",r.has(5,1) ? en(kErrG,r.u8(5)) : "");
			r.text("Operation","operation",r.has(6,1) ? en(kErrO,r.u8(6)) : "");
			r.u32Unk("Vendor syndrome","vendor_syndrome",7,0);
			r.u32Unk("Array error address","array_error_address",0x0B,0x80000000);
			r.u32Unk("Device error address","device_error_address",0x0F,0x80000000);
			r.u32Unk("Error resolution","error_resolution",0x13,0x80000000);
		}

		void map19(R& r) {
			r.text("Handle","handle",hx(r.out.handle,4));
			r.range(4,8,0x0F,0x17);
			r.hex16("Array handle","array_handle",0x0C,0xFFFF);
			uint64_t w = r.has(0x0E,1) ? r.u8(0x0E) : NA;
			r.numTxt("Partition width","partition_width",w,std::to_string(w));
		}

		void map20(R& r) {
			r.text("Handle","handle",hx(r.out.handle,4));
			r.range(4,8,0x13,0x1B);
			r.hex16("Device handle","device_handle",0x0C,0xFFFF);
			r.hex16("Array mapped address handle","array_mapped_handle",0x0E,0xFFFF);
			r.pos("Partition row position","partition_row",0x10);
			r.pos("Interleave position","interleave_position",0x11);
			r.pos("Interleave data depth","interleave_depth",0x12);
		}

		// strings end with an empty one. Firmware text, padded and untrusted.
		std::vector<std::string> readStrs(const uint8_t* p,const uint8_t* end,const uint8_t** next) {
			std::vector<std::string> v;
			const uint8_t* q = p;
			while (q < end && *q && v.size() < 255) {
				const uint8_t* e = q;
				while (e < end && *e) e++;
				std::string s = util::safe(std::string((const char*)q,e - q));
				while (!s.empty() && s.back() == ' ') s.pop_back();
				v.push_back(std::move(s));
				q = e < end ? e + 1 : end;
			}
			if (q == p && q < end) q++; // no strings : the double null starts right here
			if (q < end && !*q) q++;
			*next = q;
			return v;
		}

		// same value on every stick, "Mixed" otherwise. Max of an enum means nothing.
		void merge(std::string& cur,const std::string& v,bool& first) {
			if (first) cur = v;
			else if (cur != v) cur = "Mixed";
			first = false;
		}

		Snap<Table> g_tbl;

	}

	const Field* Rec::get(const char* key) const {
		for (auto& x : f) if (x.key == key) return &x;
		return nullptr;
	}

	std::string Rec::str(const char* key) const {
		const Field* x = get(key);
		return x ? x->val : "";
	}

	uint64_t Rec::num(const char* key) const {
		const Field* x = get(key);
		return x ? x->num : NA;
	}

	Table parse(const uint8_t* d,size_t n,int major,int minor) {
		Table t;
		t.loaded = true;
		t.ok = true;
		t.major = major;
		t.minor = minor;
		const uint8_t* end = d + n;
		const uint8_t* p = d;
		while (p + 4 <= end) {
			uint8_t type = p[0];
			int len = p[1];
			if (len < 4 || p + len > end) break;
			const uint8_t* next = end;
			std::vector<std::string> strs = readStrs(p + len,end,&next);
			Rec rec;
			rec.handle = (uint16_t)(p[2] | p[3] << 8);
			R r{p,len,strs,rec};
			if (type == 16) {
				array16(r);
				rec.title = "Physical memory array " + hx(rec.handle,4);
				t.arrays.push_back(std::move(rec));
			} else if (type == 17) {
				device17(r);
				t.devices.push_back(std::move(rec));
			} else if (type == 18) {
				error18(r);
				rec.title = "32-bit memory error " + hx(rec.handle,4);
				t.errors.push_back(std::move(rec));
			} else if (type == 19) {
				map19(r);
				rec.title = "Memory array mapped address " + hx(rec.handle,4);
				t.arrayMaps.push_back(std::move(rec));
			} else if (type == 20) {
				map20(r);
				rec.title = "Memory device mapped address " + hx(rec.handle,4);
				t.deviceMaps.push_back(std::move(rec));
			}
			if (type == 127) break;
			p = next;
		}

		bool fF = true;
		bool fT = true;
		bool fC = true;
		for (auto& dev : t.devices) {
			bool sys = true;
			for (auto& a : t.arrays) if (a.str("handle") == dev.str("array_handle") && a.str("use") != "System" && a.str("use") != "Unknown") sys = false;
			if (!sys) continue;
			t.slotsTotal++;
			uint64_t sz = dev.num("size_bytes");
			if (sz == 0) continue;
			t.slotsUsed++;
			if (sz != NA) t.installed = (t.installed == NA ? 0 : t.installed) + sz;
			merge(t.formFactor,dev.str("form_factor"),fF);
			merge(t.memType,dev.str("memory_type"),fT);
			merge(t.tech,dev.str("technology"),fC);
			uint64_t sp = dev.num("configured_speed_mts");
			if (sp == NA) sp = dev.num("speed_mts");
			if (sp != NA && (t.speed == NA || sp > t.speed)) t.speed = sp;
		}
		return t;
	}

	void load() {
		static std::mutex m;
		static bool done = false;
		std::lock_guard<std::mutex> lk(m);
		if (done) return;
		done = true;
		std::vector<uint8_t> raw;
		int ma = 0;
		int mi = 0;
		std::string err;
		auto t = std::make_shared<Table>();
		if (osRead(raw,ma,mi,err)) *t = parse(raw.data(),raw.size(),ma,mi);
		else {
			t->loaded = true;
			t->err = err;
		}
		g_tbl.put(t);
	}

	std::shared_ptr<const Table> latest() {
		static auto first = std::make_shared<const Table>();
		auto p = g_tbl.get();
		return p ? p : first;
	}

}
