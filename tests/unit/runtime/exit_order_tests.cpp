// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/context.hpp"
#include "uml_darwin_static_mmu.h"

#include <cstdio>
#include <cstdlib>

int main()
{
	/* Construct the process-lifetime legacy Context before the software MMU
	 * registry. CTest observes the subsequent static-destruction phase, which
	 * used to let Context::~Context() call a destroyed registry. */
	if (uml::runtime::current_context() == nullptr) {
		std::fputs("legacy context is unavailable\n", stderr);
		return EXIT_FAILURE;
	}
	if (darwin_uml_static_vm_activate(991) != 0) {
		std::fputs("software MMU registry is unavailable\n", stderr);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
