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
enum darwin_uml_execution_engine {
	DARWIN_UML_EXECUTION_AUTO = 0,
	DARWIN_UML_EXECUTION_GADGETS = 1,
	DARWIN_UML_EXECUTION_JIT = 2,
	DARWIN_UML_EXECUTION_INTERPRETER = 3,
};
int darwin_uml_set_execution_engine(enum darwin_uml_execution_engine engine);
enum darwin_uml_execution_engine darwin_uml_get_execution_engine(void);
unsigned long darwin_uml_guest_hwcap(void);
unsigned long darwin_uml_guest_hwcap2(void);
int darwin_uml_jit_available(void);
void darwin_uml_static_guest_interrupt_sequence(
	const unsigned int *sequence, unsigned int snapshot);

#ifdef __cplusplus
}
#endif

#endif
