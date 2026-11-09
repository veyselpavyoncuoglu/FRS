#ifndef _WIN32
#include "dotnetmem.h"

namespace dotnetmem { namespace be {

	void read(const ProcRef& r,ClrMem& m) {
		(void)r;
		m.kind = ClrMem::NotHere;
		m.why = ".NET memory is not available on Linux, the CLR block it reads only exists on Windows.";
	}

} }
#endif
