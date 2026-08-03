/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Keep one translation unit so shared macros and local labels remain cheap. */
#include "engine/core_integer.inc"
/* Keep the FP/SIMD bank on its established 64-byte cache-line phase after
 * factoring the native-environment cold path into one shared routine. */
.space 16
#include "engine/floating_simd.inc"
#include "engine/system_atomic.inc"
#include "engine/load_store.inc"
#include "engine/control.inc"

.globl NAME(uml_arm64_gadget_restore_native_fp_environment)
.p2align 4
NAME(uml_arm64_gadget_restore_native_fp_environment):
	ldrb w8, [x0, #UML_GADGET_CPU_FP_ACTIVE]
	cbz w8, 97f
	mrs x9, fpsr
	ldr w10, [x0, #UML_GADGET_CPU_FPSR]
	and w10, w10, #0xf0000000
	mov w11, #0x009f
	movk w11, #0x0800, lsl #16
	and w9, w9, w11
	orr w9, w9, w10
	str w9, [x0, #UML_GADGET_CPU_FPSR]
	ldr x8, [x0, #UML_GADGET_CPU_HOST_FPCR]
	ldr x9, [x0, #UML_GADGET_CPU_HOST_FPSR]
	msr fpcr, x8
	msr fpsr, x9
	strb wzr, [x0, #UML_GADGET_CPU_FP_ACTIVE]
97:
	ret

.globl NAME(uml_arm64_gadget_restore_native_fpmr_environment)
.p2align 4
NAME(uml_arm64_gadget_restore_native_fpmr_environment):
	ldrb w8, [x0, #UML_GADGET_CPU_FPMR_ACTIVE]
	cbz w8, 98f
	ldr x9, [x0, #UML_GADGET_CPU_HOST_FPMR]
	.inst 0xd51b4449 /* msr FPMR, x9 */
	strb wzr, [x0, #UML_GADGET_CPU_FPMR_ACTIVE]
98:
	ret
