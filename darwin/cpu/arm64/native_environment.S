/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/arm64/layout/assembly_offsets.h"

#if __APPLE__
#define NAME(value) _##value
#else
#define NAME(value) value
#endif

/* Shared execution-boundary helpers.  Both precompiled gadgets and native
 * translated blocks may retain guest floating-point state in architectural
 * registers, so neither implementation owns restoration of the host state. */
.globl NAME(uml_arm64_restore_native_fp_environment)
.p2align 4
NAME(uml_arm64_restore_native_fp_environment):
	ldrb w8, [x0, #UML_ARM64_CPU_FP_ACTIVE]
	cbz w8, 97f
	mrs x9, fpsr
	ldr w10, [x0, #UML_ARM64_CPU_FPSR]
	and w10, w10, #0xf0000000
	mov w11, #0x009f
	movk w11, #0x0800, lsl #16
	and w9, w9, w11
	orr w9, w9, w10
	str w9, [x0, #UML_ARM64_CPU_FPSR]
	ldr x8, [x0, #UML_ARM64_CPU_HOST_FPCR]
	ldr x9, [x0, #UML_ARM64_CPU_HOST_FPSR]
	msr fpcr, x8
	msr fpsr, x9
	strb wzr, [x0, #UML_ARM64_CPU_FP_ACTIVE]
97:
	ret
