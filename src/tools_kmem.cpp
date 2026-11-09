#include "kmem.h"
#include "domains.h"
#include "mcp.h"
#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

	using namespace kmem;

	// Json::num and friends fall back to the default on a wrong type, the caller has to hear about it
	void needType(const Json& a,std::initializer_list<const char*> keys,Json::Type t,const char* name) {
		for (const char* k : keys) {
			const Json* v = a.get(k);
			if (v && v->t != Json::Null && v->t != t) throw std::runtime_error(std::string(k) + " must be a " + name);
		}
	}

	Json jv(int64_t v) {
		return v == UNK ? Json() : Json((double)v);
	}

	struct Slice {
		size_t off,end;
		int lim;
	};

	// same rules and shape as mcp::paged. Big lists build Json only for the page they return.
	Slice sliceOf(size_t n,const Json& a,int def,int mx) {
		double l = a.num("limit",def);
		double o = a.num("offset",0);
		Slice s;
		s.lim = l < 1.0 ? 1 : l > (double)mx ? mx : (int)l;
		s.off = o <= 0.0 ? 0 : o >= (double)n ? n : (size_t)o;
		s.end = std::min(n,s.off + (size_t)s.lim);
		return s;
	}

	Json pageJson(const Slice& s,size_t total,Json rows) {
		Json j = Json::obj();
		j.set("count",s.end - s.off).set("total_count",total).set("offset",s.off).set("limit",s.lim);
		j.set("truncated",s.end < total).set("rows",std::move(rows));
		return j;
	}

	// true for descending. def when the argument is absent
	bool orderOf(const Json& a,bool def) {
		std::string o = a.str("order","");
		if (o.empty()) return def;
		if (o == "desc") return true;
		if (o == "asc") return false;
		throw std::runtime_error("order must be asc or desc");
	}

	Json poolRowJson(const PoolRow& r) {
		Json j = Json::obj();
		j.set("tag",r.tag);
		j.set("tag_value",kWin ? Json(util::hex(r.id)) : Json());
		j.set("protected",r.prot);
		j.set("driver",r.driver.empty() ? Json() : Json(r.driver));
		j.set("description",r.desc.empty() ? Json() : Json(r.desc));
		for (int c = PCVal; c < PCN; c++) j.set(kPoolCols[c].key,jv(poolVal(r,c)));
		return j;
	}

	// the sample of the last read, pages after the first use it so rows can't move between pages
	std::mutex g_pinMu;
	std::shared_ptr<const PoolSnap> g_pin;
	double g_pinWin = 0;
	constexpr uint64_t kPinMs = 30000;

	Json poolTags(const Json& a) {
		needType(a,{"delta_ms","limit","offset"},Json::Num,"number");
		needType(a,{"sort_by","order","filter"},Json::Str,"string");
		double ms = a.num("delta_ms",0);
		if (ms < 0.0 || ms > 30000.0) throw std::runtime_error("delta_ms must be 0 to 30000");
		std::string key = a.str("sort_by","nonpaged_bytes");
		int col = -1;
		for (int c = 0; c < PCN; c++) if (key == kPoolCols[c].key) col = c;
		if (col < 0) throw std::runtime_error("Unknown sort_by. Use a counter or delta field such as nonpaged_bytes, bytes, nonpaged_bytes_delta, or tag, driver, description");
		bool desc = orderOf(a,col >= PCVal);

		wantPool(15000);
		std::shared_ptr<const PoolSnap> s;
		if (a.num("offset",0) > 0.0) {
			std::lock_guard<std::mutex> lk(g_pinMu);
			if (g_pin && g_pinWin == ms && util::nowMs() - g_pin->t <= kPinMs) s = g_pin;
		}
		if (!s) {
			if (ms > 0.0) {
				auto first = poolRead(nullptr);
				if (!first->err.empty()) throw std::runtime_error(first->err);
				auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)ms);
				while (std::chrono::steady_clock::now() < end) {
					if (!mcp::alive()) throw std::runtime_error("MCP server is stopping");
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
				s = poolRead(first.get());
			} else s = poolFresh();
			if (!s->err.empty()) throw std::runtime_error(s->err);
			std::lock_guard<std::mutex> lk(g_pinMu);
			g_pin = s;
			g_pinWin = ms;
		}

		std::string low = util::lower(a.str("filter",""));
		std::vector<int> idx;
		for (int i = 0; i < (int)s->rows.size(); i++) if (poolMatch(s->rows[i],low)) idx.push_back(i);
		const auto& rs = s->rows;
		std::sort(idx.begin(),idx.end(),[&](int x,int y) { return poolLess(rs[x],rs[y],col,desc); });
		int64_t pb = kWin ? 0 : UNK,nb = 0;
		for (int i : idx) {
			if (rs[i].v[VPagedBytes] != UNK) pb += rs[i].v[VPagedBytes];
			if (rs[i].v[VNpBytes] != UNK) nb += rs[i].v[VNpBytes];
		}

		Slice sl = sliceOf(idx.size(),a,100,2000);
		Json rows = Json::arr();
		for (size_t i = sl.off; i < sl.end; i++) rows.push(poolRowJson(rs[idx[i]]));
		Json j = pageJson(sl,idx.size(),std::move(rows));
		j.set("source",s->src);
		j.set("counter_bits",s->bits);
		j.set("delta_ms",s->dtMs ? Json(s->dtMs) : Json());
		uint64_t now = util::nowMs();
		j.set("sample_id",s->t).set("age_ms",now >= s->t ? now - s->t : 0);
		Json tot = Json::obj();
		tot.set("paged_bytes",jv(pb)).set("nonpaged_bytes",jv(nb));
		tot.set("bytes",(double)(pb == UNK ? 0 : pb) + (double)nb);
		j.set("matching_totals",tot);
		Json db = Json::obj();
		db.set("loaded",s->db.loaded).set("tags",s->db.n).set("path",s->db.path.empty() ? Json() : Json(s->db.path));
		db.set("problem",s->db.why.empty() ? Json() : Json(s->db.why));
		j.set("tag_file",db);
		j.set("field_note",kWin ? Json() : Json("Linux slab caches : nonpaged_allocs is num_objs, nonpaged_current is active_objs, nonpaged_bytes is active_objs times objsize. No paged counters."));
		return j;
	}

	const char* kBlanked = "The OS returned 0 for every address, this process may not see kernel addresses. Sizes, tags and pool type are real.";

	Json bigPools(const Json& a) {
		needType(a,{"limit","offset"},Json::Num,"number");
		needType(a,{"sort_by","order","tag","filter"},Json::Str,"string");
		needType(a,{"group_by_tag","refresh"},Json::Bool,"boolean");
		bool group = a.flag("group_by_tag",false);
		std::string key = a.str("sort_by",group ? "bytes" : "size");
		static const char* const lk[] = {"size","address","tag","nonpaged"};
		static const char* const gk[] = {"bytes","blocks","tag","largest","nonpaged_bytes","paged_bytes"};
		int col = -1;
		for (int i = 0; i < (group ? 6 : 4); i++) if (key == (group ? gk[i] : lk[i])) col = i;
		if (col < 0) throw std::runtime_error(group ? "Unknown sort_by. With group_by_tag use bytes, blocks, tag, largest, nonpaged_bytes or paged_bytes" : "Unknown sort_by. Use size, address, tag or nonpaged");
		bool desc = orderOf(a,key != "tag");

		auto s = bigLatest();
		bool fresh = a.flag("refresh",false) || !s || !s->err.empty() || util::nowMs() - s->t > 30000;
		if (fresh) s = bigRead();
		if (!s->err.empty()) throw std::runtime_error(s->err);

		std::string low = util::lower(a.str("filter",""));
		std::string exact = a.str("tag","");
		std::vector<char> pass(s->tags.size(),0);
		for (size_t i = 0; i < s->tags.size(); i++) {
			const BigTag& t = s->tags[i];
			pass[i] = (exact.empty() || t.tag == exact) && (low.empty() || util::lower(t.tag).find(low) != std::string::npos || util::lower(t.driver).find(low) != std::string::npos || util::lower(t.desc).find(low) != std::string::npos);
		}

		Json rows = Json::arr();
		Json j;
		uint64_t mN = 0,mBytes = 0;
		std::vector<int> idx;
		if (group) {
			for (int i = 0; i < (int)s->tags.size(); i++) if (pass[i]) {
				idx.push_back(i);
				mN += s->tags[i].n;
				mBytes += s->tags[i].bytes;
			}
			auto val = [&](const BigTag& t) -> uint64_t {
				switch (col) {
					case 0: return t.bytes;
					case 1: return t.n;
					case 3: return t.largest;
					case 4: return t.npBytes;
					case 5: return t.bytes - t.npBytes;
				}
				return 0;
			};
			std::sort(idx.begin(),idx.end(),[&](int x,int y) {
				if (col != 2) {
					uint64_t p = val(s->tags[x]);
					uint64_t q = val(s->tags[y]);
					if (p != q) return desc ? p > q : p < q;
				}
				return col == 2 && desc ? x > y : x < y;
			});
			Slice sl = sliceOf(idx.size(),a,100,2000);
			for (size_t i = sl.off; i < sl.end; i++) {
				const BigTag& t = s->tags[idx[i]];
				Json r = Json::obj();
				r.set("tag",t.tag).set("driver",t.driver.empty() ? Json() : Json(t.driver)).set("description",t.desc.empty() ? Json() : Json(t.desc));
				r.set("blocks",t.n).set("bytes",mcp::bytes(t.bytes)).set("nonpaged_blocks",t.npN).set("nonpaged_bytes",mcp::bytes(t.npBytes));
				r.set("paged_blocks",t.n - t.npN).set("paged_bytes",mcp::bytes(t.bytes - t.npBytes)).set("largest_bytes",mcp::bytes(t.largest));
				rows.push(std::move(r));
			}
			j = pageJson(sl,idx.size(),std::move(rows));
		} else {
			const auto& rs = s->rows;
			idx.reserve(rs.size());
			for (int i = 0; i < (int)rs.size(); i++) if (pass[rs[i].tag]) {
				idx.push_back(i);
				mN++;
				mBytes += rs[i].size;
			}
			std::sort(idx.begin(),idx.end(),[&](int x,int y) {
				const BigRow& p = rs[x];
				const BigRow& q = rs[y];
				if (col == 0 && p.size != q.size) return desc ? p.size > q.size : p.size < q.size;
				if (col == 1 && p.addr != q.addr) return desc ? p.addr > q.addr : p.addr < q.addr;
				if (col == 2 && p.tag != q.tag) return desc ? p.tag > q.tag : p.tag < q.tag;
				if (col == 3 && p.np != q.np) return desc ? p.np : q.np;
				return x < y;
			});
			Slice sl = sliceOf(idx.size(),a,100,5000);
			for (size_t i = sl.off; i < sl.end; i++) {
				const BigRow& x = rs[idx[i]];
				const BigTag& t = s->tags[x.tag];
				Json r = Json::obj();
				r.set("address",x.addr ? Json(util::hex(x.addr)) : Json()).set("size_bytes",mcp::bytes(x.size)).set("nonpaged",x.np);
				r.set("tag",t.tag).set("driver",t.driver.empty() ? Json() : Json(t.driver)).set("description",t.desc.empty() ? Json() : Json(t.desc));
				rows.push(std::move(r));
			}
			j = pageJson(sl,idx.size(),std::move(rows));
		}
		j.set("source",s->src).set("refreshed",fresh);
		uint64_t now = util::nowMs();
		j.set("age_ms",now >= s->t ? now - s->t : 0);
		j.set("addresses_blanked",s->blanked).set("address_note",s->blanked ? Json(kBlanked) : Json());
		j.set("matching_blocks",mN).set("matching_bytes",mcp::bytes(mBytes));
		j.set("total_blocks",s->rows.size()).set("total_bytes",mcp::bytes(s->bytes));
		j.set("nonpaged_blocks",s->npN).set("nonpaged_bytes",mcp::bytes(s->npBytes));
		j.set("paged_blocks",s->rows.size() - s->npN).set("paged_bytes",mcp::bytes(s->bytes - s->npBytes));
		return j;
	}

	Json pageFiles(const Json& a) {
		needType(a,{"limit","offset"},Json::Num,"number");
		auto s = pfRead(true);
		if (!s->err.empty()) throw std::runtime_error(s->err);
		std::vector<Json> rows;
		uint64_t sz = 0,used = 0;
		bool unk = false;
		for (auto& r : s->rows) {
			if (r.size == NA || r.used == NA) unk = true;
			Json j = Json::obj();
			j.set("path",r.path).set("size_bytes",mcp::bytes(r.size)).set("used_bytes",mcp::bytes(r.used)).set("peak_bytes",mcp::bytes(r.peak));
			j.set("min_bytes",mcp::bytes(r.min)).set("max_bytes",mcp::bytes(r.max));
			rows.push_back(std::move(j));
			sz += r.size;
			used += r.used;
		}
		Json j = mcp::paged(rows,a,100,1000);
		if (unk) sz = used = NA;
		j.set("total_size_bytes",mcp::bytes(sz)).set("total_used_bytes",mcp::bytes(used));
		j.set("free_bytes",mcp::bytes(unk ? NA : sz > used ? sz - used : 0));
		j.set("peak_scope",s->ownPeak ? "since FRS started, the kernel keeps no peak" : "since boot");
		return j;
	}

}

namespace kmem {

	void regTools() {
		mcp::add("Kernel","pool_tags",
			"Kernel pool use per tag, driver leaks show here and in no process. Deltas compare two samples delta_ms apart, null on a tag's first sample. A call with offset above 0 reuses the last sample for up to 30 s if delta_ms is the same, a changed sample_id means rows moved. Allocs and frees are 32 bit below 26H1 and wrap, current stays right. Driver and description need an optional pooltag.txt next to the exe, else null. Linux : slab caches, root only.",
			R"j({"type":"object","properties":{
				"sort_by":{"type":"string","description":"A counter or delta field, tag, driver or description, default nonpaged_bytes. Examples : bytes, nonpaged_bytes_delta, paged_current"},
				"order":{"type":"string","enum":["asc","desc"],"description":"Default desc for numbers, asc for text"},
				"filter":{"type":"string","description":"Case-insensitive match on tag, driver, description"},
				"delta_ms":{"type":"number","description":"0 to 30000. Waits this long between two samples so deltas cover that window. Default 0 : latest sample"},
				"limit":{"type":"integer","description":"Rows per call, default 100, max 2000"},
				"offset":{"type":"integer","description":"Rows to skip"}
			}})j",
			mcp::Read,poolTags);
		mcp::add("Kernel","big_pools",
			"Single big kernel pool blocks, often 100k rows, or one row per tag with group_by_tag. Re-read only on refresh or when the last read failed or is over 30 s old, so pages line up. Windows may blank addresses : address is null and addresses_blanked true, sizes and pool type stay real. pool_tags bytes already include these blocks, do not add them. Linux : /proc/vmallocinfo, root only.",
			R"j({"type":"object","properties":{
				"group_by_tag":{"type":"boolean","description":"One row per tag with block count, bytes, largest"},
				"sort_by":{"type":"string","description":"List : size, address, tag, nonpaged. Grouped : bytes, blocks, tag, largest, nonpaged_bytes, paged_bytes"},
				"order":{"type":"string","enum":["asc","desc"],"description":"Default desc, asc for tag"},
				"tag":{"type":"string","description":"Only this exact tag"},
				"filter":{"type":"string","description":"Case-insensitive match on tag, driver, description"},
				"refresh":{"type":"boolean","description":"Read again now"},
				"limit":{"type":"integer","description":"Rows per call, default 100, max 5000, grouped max 2000"},
				"offset":{"type":"integer","description":"Rows to skip"}
			}})j",
			mcp::Read,bigPools);
		mcp::add("Kernel","page_files",
			"Active page files, swap on Linux : path, size, used, peak. Windows peak is since boot, Linux peak only since FRS started. Min and max are the configured limits, null on Linux. Windows does not list swapfile.sys here. An empty list means none is active. free_bytes is total size minus used, page files can still grow up to max.",
			R"j({"type":"object","properties":{
				"limit":{"type":"integer","description":"Rows per call, default 100, max 1000"},
				"offset":{"type":"integer","description":"Rows to skip"}
			}})j",
			mcp::Read,pageFiles);
	}

}
