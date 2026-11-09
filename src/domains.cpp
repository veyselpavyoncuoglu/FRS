#include "domains.h"

namespace domains {

	void initAll() {
		sysmem::init();
		procs::init();
		procdet::init();
		vmem::init();
		vmscan::init();
		vmdiag::init();
		kmem::init();
		extra::init();
		surf::init();
	}

	void regToolsAll() {
		sysmem::regTools();
		procs::regTools();
		procdet::regTools();
		vmem::regTools();
		vmscan::regTools();
		vmdiag::regTools();
		kmem::regTools();
		extra::regTools();
		surf::regTools();
	}

}
