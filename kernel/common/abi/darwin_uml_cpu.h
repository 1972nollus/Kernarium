/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_CPU_ABI_H
#define UML_DARWIN_CPU_ABI_H

#include "uml_darwin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

int darwin_uml_enter_guest(void *registers, void *fp_registers,
			   unsigned long fp_size, unsigned long generation,
			   unsigned long *tls);
unsigned long darwin_uml_guest_hwcap(void);
unsigned long darwin_uml_guest_hwcap2(void);
/* The virtual CPU's model name, for /proc/cpuinfo. Static storage owned by
 * the emulator, valid for the lifetime of the process, and identical for
 * every guest architecture the host presents. */
const char *darwin_uml_guest_cpu_model_name(void);
int darwin_uml_static_guest_interrupt_sequence(
	const unsigned int *sequence, unsigned int snapshot);

#ifdef __cplusplus
}
#endif

#endif
