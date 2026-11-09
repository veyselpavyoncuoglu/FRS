#include "kmem.h"
#include <algorithm>

// Parser for the optional pooltag.txt. The file is Microsoft's, FRS never ships it.
// Lines are "<tag> - <binary> - <description>". Everything else in the file is skipped, there is no header to find.
namespace kmem {

	namespace {

		std::string trim(const std::string& s) {
			size_t a = s.find_first_not_of(" \t\r\n");
			if (a == std::string::npos) return "";
			return s.substr(a,s.find_last_not_of(" \t\r\n") - a + 1);
		}

	}

	void parseTags(const std::string& txt,TagDb& db) {
		size_t p = txt.compare(0,3,"\xEF\xBB\xBF") == 0 ? 3 : 0;
		while (p < txt.size()) {
			size_t e = txt.find('\n',p);
			if (e == std::string::npos) e = txt.size();
			std::string ln = txt.substr(p,e - p);
			p = e + 1;
			std::replace(ln.begin(),ln.end(),'\t',' ');
			ln = trim(ln);
			if (ln.compare(0,2,"//") == 0) continue;
			// only the first two separators split, a description can have its own " - "
			size_t a = ln.find(" - ");
			if (a == std::string::npos) continue;
			std::string tag = trim(ln.substr(0,a));
			if (tag.empty() || tag.size() > 4) continue;
			size_t b = ln.find(" - ",a + 3);
			TagInfo ti;
			ti.driver = trim(b == std::string::npos ? ln.substr(a + 3) : ln.substr(a + 3,b - a - 3));
			if (b != std::string::npos) ti.desc = trim(ln.substr(b + 3));
			if (tag.find('?') != std::string::npos) {
				bool dup = false;
				for (auto& w : db.wild) if (w.first == tag) dup = true;
				if (!dup) db.wild.emplace_back(tag,std::move(ti));
			} else db.exact.emplace(tag,std::move(ti)); // first one stays
		}
	}

	const TagInfo* TagDb::find(const std::string& t) const {
		auto it = exact.find(t);
		if (it != exact.end()) return &it->second;
		const TagInfo* best = nullptr;
		int bestN = -1;
		for (auto& w : wild) {
			const std::string& pat = w.first;
			if (pat.size() < t.size()) continue;
			int lit = 0;
			bool hit = true;
			for (size_t i = 0; i < pat.size() && hit; i++) {
				if (pat[i] == '?') continue;
				hit = i < t.size() && t[i] == pat[i];
				lit++;
			}
			if (hit && lit > bestN) {
				best = &w.second;
				bestN = lit;
			}
		}
		return best;
	}

}
