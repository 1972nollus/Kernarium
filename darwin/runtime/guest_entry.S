/* SPDX-License-Identifier: GPL-2.0 */

#include "direct_gateway.h"

		.text
		.p2align 2

	.macro save_neon base
	stp q0, q1, [\base, #0]
	stp q2, q3, [\base, #32]
	stp q4, q5, [\base, #64]
	stp q6, q7, [\base, #96]
	stp q8, q9, [\base, #128]
	stp q10, q11, [\base, #160]
	stp q12, q13, [\base, #192]
	stp q14, q15, [\base, #224]
	stp q16, q17, [\base, #256]
	stp q18, q19, [\base, #288]
	stp q20, q21, [\base, #320]
	stp q22, q23, [\base, #352]
	stp q24, q25, [\base, #384]
	stp q26, q27, [\base, #416]
	stp q28, q29, [\base, #448]
	stp q30, q31, [\base, #480]
	mrs x9, fpsr
	str w9, [\base, #512]
	mrs x9, fpcr
	str w9, [\base, #516]
	.endm

	.macro load_neon base
	ldp q0, q1, [\base, #0]
	ldp q2, q3, [\base, #32]
	ldp q4, q5, [\base, #64]
	ldp q6, q7, [\base, #96]
	ldp q8, q9, [\base, #128]
	ldp q10, q11, [\base, #160]
	ldp q12, q13, [\base, #192]
	ldp q14, q15, [\base, #224]
	ldp q16, q17, [\base, #256]
	ldp q18, q19, [\base, #288]
	ldp q20, q21, [\base, #320]
	ldp q22, q23, [\base, #352]
	ldp q24, q25, [\base, #384]
	ldp q26, q27, [\base, #416]
	ldp q28, q29, [\base, #448]
	ldp q30, q31, [\base, #480]
	ldr w9, [\base, #512]
	msr fpsr, x9
	ldr w9, [\base, #516]
	msr fpcr, x9
	.endm

		.globl _darwin_uml_arch_enter_saved
_darwin_uml_arch_enter_saved:
	/* Save a continuation on the native pthread stack before taking the
	 * initial entry trap.  A later guest exception resumes this continuation
	 * through darwin_uml_context_switch and returns to the C caller.
	 */
	stp x19, x20, [x0, #0]
	stp x21, x22, [x0, #16]
	stp x23, x24, [x0, #32]
	stp x25, x26, [x0, #48]
	stp x27, x28, [x0, #64]
	stp x29, x30, [x0, #80]
	mov x9, sp
	str x9, [x0, #96]
	mov x0, x1
	mov x1, x2
	mov x2, x3
	mov x3, x4
	b _darwin_uml_arch_enter

		.globl _darwin_uml_arch_enter
_darwin_uml_arch_enter:
	/* Enter through the Mach exception server so it can install all 31
	 * general registers and the PC atomically.  A normal indirect branch
	 * necessarily consumes one guest register as scratch (historically x16),
	 * which violates the Linux syscall ABI and breaks AArch64 PLT code.
	 */
	/* x2 is guest FP state and x3 is the native state save area. */
	save_neon x3
	load_neon x2
	msr tpidr_el0, x1
	brk #0x555
	brk #0x556

	.p2align 2
	.globl _darwin_uml_return_trampoline
_darwin_uml_return_trampoline:
	/* x0 is guest_context, x1 native TLS, x2 guest FP save area, and x3
	 * native FP state.  Save/restore on this same thread to avoid Mach RPCs.
	 */
	save_neon x2
	load_neon x3
	msr tpidr_el0, x1
	b _darwin_uml_guest_return_c

	.p2align 2
	.globl _darwin_uml_save_fp
_darwin_uml_save_fp:
	save_neon x0
	ret

	.p2align 2
	.globl _darwin_uml_restore_fp
_darwin_uml_restore_fp:
	load_neon x0
	ret

	/* The generated entry stub has already spilled guest x16.  Use x16—not
	 * Darwin's reserved x18—to acquire the active state, save the guest
	 * registers needed as host temporaries, and only then dedicate x19 to the
	 * coroutine.  This is deliberately duplicated for each immediate event;
	 * no guest register may carry the selector before its value is captured.
	 */
	.macro direct_entry event
	adrp x16, _darwin_uml_active_direct_gateway@GOTPAGE
	ldr x16, [x16, _darwin_uml_active_direct_gateway@GOTPAGEOFF]
	ldr x16, [x16]
	ldr x16, [x16, #DG_REGISTERS]
	str x17, [x16, #ARM64_STATE_X17]
	stp x19, x20, [x16, #152]
	adrp x19, _darwin_uml_active_direct_gateway@GOTPAGE
	ldr x19, [x19, _darwin_uml_active_direct_gateway@GOTPAGEOFF]
	ldr x19, [x19]
	mov w17, #\event
	str w17, [x19, #DG_EVENT]
	b Ldirect_gateway_common
	.endm

	.p2align 2
	.globl _darwin_uml_direct_syscall
_darwin_uml_direct_syscall:
	direct_entry 1

	.p2align 2
	.globl _darwin_uml_direct_indirect
_darwin_uml_direct_indirect:
	direct_entry 3

Ldirect_gateway_common:
	/* x16 is the register snapshot and x19 is the direct-gateway state. */
	stp x0, x1, [x16, #0]
	stp x2, x3, [x16, #16]
	stp x4, x5, [x16, #32]
	stp x6, x7, [x16, #48]
	stp x8, x9, [x16, #64]
	stp x10, x11, [x16, #80]
	stp x12, x13, [x16, #96]
	stp x14, x15, [x16, #112]
	ldr x17, [x19, #DG_SCRATCH_X16]
	str x17, [x16, #ARM64_STATE_X16]
	ldr x17, [x19, #DG_GUEST_X18]
	str x17, [x16, #ARM64_STATE_X18]
	stp x21, x22, [x16, #168]
	stp x23, x24, [x16, #184]
	stp x25, x26, [x16, #200]
	stp x27, x28, [x16, #216]
	stp x29, x30, [x16, #ARM64_STATE_FP]
	mov x17, sp
	str x17, [x16, #ARM64_STATE_SP]
	ldr x17, [x19, #DG_GUEST_PC]
	str x17, [x16, #ARM64_STATE_PC]
	mrs x17, nzcv
	str w17, [x16, #ARM64_STATE_CPSR]

	ldr x17, [x19, #DG_FP_REGISTERS]
	save_neon x17
	ldr x17, [x19, #DG_HOST_FP_REGISTERS]
	load_neon x17
	ldr x17, [x19, #DG_HOST_TLS]
	msr tpidr_el0, x17
	mov w17, #1
	str w17, [x19, #DG_SUSPENDED]

	/* Preserve the gateway pointer in the direct coroutine, but restore the
	 * Darwin platform register before resuming ordinary host-compiled code.
	 */
	ldr x18, [x19, #DG_HOST_X18]
	ldr x0, [x19, #DG_DIRECT_SWITCH]
	ldr x1, [x19, #DG_KERNEL_SWITCH]
	bl _darwin_uml_context_switch

	/* The UML kernel may have replaced the complete register state (notably
	 * after execve or signal return).  Physical x18 is runtime-owned, so it
	 * carries the saved Linux PC through the register-complete branch.
	 */
	str wzr, [x19, #DG_SUSPENDED]
	ldr x16, [x19, #DG_FP_REGISTERS]
	load_neon x16
	ldr x17, [x19, #DG_GUEST_TLS]
	msr tpidr_el0, x17
	ldr x16, [x19, #DG_REGISTERS]
	/* Most syscalls resume at the instruction following SVC.  An exec,
	 * signal, or restart can replace PC; use a full-register Mach resume for
	 * that uncommon case instead of forcing it through a static return thunk.
	 */
	ldr x17, [x16, #ARM64_STATE_PC]
	ldr x9, [x19, #DG_GUEST_PC]
	cmp x17, x9
	b.ne Ldirect_mach_resume
	ldr w17, [x16, #ARM64_STATE_CPSR]
	msr nzcv, x17
	ldp x0, x1, [x16, #0]
	ldp x2, x3, [x16, #16]
	ldp x4, x5, [x16, #32]
	ldp x6, x7, [x16, #48]
	ldp x8, x9, [x16, #64]
	ldp x10, x11, [x16, #80]
	ldp x12, x13, [x16, #96]
	ldp x14, x15, [x16, #112]
	ldp x21, x22, [x16, #168]
	ldp x23, x24, [x16, #184]
	ldp x25, x26, [x16, #200]
	ldp x27, x28, [x16, #216]
	ldp x29, x30, [x16, #ARM64_STATE_FP]
	ldr x17, [x16, #ARM64_STATE_SP]
	mov sp, x17
	ldr x17, [x16, #ARM64_STATE_X17]
	ldr x16, [x19, #DG_RESUME_STUB]
	mov x18, x19
	ldr x19, [x19, #DG_REGISTERS]
	ldp x19, x20, [x19, #152]
	br x16

Ldirect_mach_resume:
	brk #0x557
	brk #0x558

	.p2align 2
	.globl _darwin_uml_context_switch
_darwin_uml_context_switch:
	/* Save and restore the AArch64 callee-saved integer context.  The signal
	 * frame and kernel stack each retain their caller-saved state according
	 * to the C ABI; FP state is exchanged explicitly by the caller.
	 */
	stp x19, x20, [x0, #0]
	stp x21, x22, [x0, #16]
	stp x23, x24, [x0, #32]
	stp x25, x26, [x0, #48]
	stp x27, x28, [x0, #64]
	stp x29, x30, [x0, #80]
	mov x9, sp
	str x9, [x0, #96]
	ldp x19, x20, [x1, #0]
	ldp x21, x22, [x1, #16]
	ldp x23, x24, [x1, #32]
	ldp x25, x26, [x1, #48]
	ldp x27, x28, [x1, #64]
	ldp x29, x30, [x1, #80]
	ldr x9, [x1, #96]
	ldr x0, [x1, #104]
	mov sp, x9
	ret
