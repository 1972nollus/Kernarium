// SPDX-License-Identifier: GPL-2.0-or-later
#include "uml_darwin_runtime.h"

extern "C" int darwin_uml_enter_guest(void *registers, void *fp_registers,
				       unsigned long fp_size,
				       unsigned long generation,
				       unsigned long *tls)
{
	return darwin_uml_static_enter_guest(
		registers, fp_registers, fp_size, generation, tls);
}

extern "C" int darwin_uml_static_dispatch_enabled(void)
{
	return 1;
}
