/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Callable AArch64 body shared by the native and ideal gadget ceilings.
 *
 * Keep the labeled region instruction-for-instruction equivalent to
 * guest_add_immediate_loop.S. The ideal harness reads these words directly,
 * so its Builder input and the native CPU's instruction stream cannot drift.
 */

	.text
	.p2align 6
	.globl uml_gadget_ceiling_native_workload
uml_gadget_ceiling_native_workload:
	stp	x19, x20, [sp, #-16]!
	mov	x19, x0
	mov	x20, #1

	.globl uml_gadget_ceiling_body_begin
uml_gadget_ceiling_body_begin:
	add	x20, x20, #1
	add	x20, x20, #2
	add	x20, x20, #3
	add	x20, x20, #4
	add	x20, x20, #5
	add	x20, x20, #6
	add	x20, x20, #7
	add	x20, x20, #8
	add	x20, x20, #9
	add	x20, x20, #10
	add	x20, x20, #11
	add	x20, x20, #12
	add	x20, x20, #13
	add	x20, x20, #14
	add	x20, x20, #15
	add	x20, x20, #16
	subs	x19, x19, #1
	b.ne	uml_gadget_ceiling_body_begin
	.globl uml_gadget_ceiling_body_end
uml_gadget_ceiling_body_end:

	mov	x0, x20
	ldp	x19, x20, [sp], #16
	ret
