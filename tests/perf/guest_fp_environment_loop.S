/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux native FP-environment throughput control.
 *
 * FMUL by 1.0 keeps the expected result stable while exercising one native
 * floating-point gadget and its environment guard per iteration.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
	.equ ITERATIONS,	500000000

	.section .text
	.global _start
	.type _start, %function
_start:
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	movz	x20, #0x3ff0, lsl #48
	fmov	d0, x20
	fmov	d1, x20

.Lloop:
	fmul	d0, d0, d1
	subs	x19, x19, #1
	b.ne	.Lloop

	fmov	x21, d0
	cmp	x21, x20
	b.ne	.Lfailure

	adrp	x1, success_message
	add	x1, x1, :lo12:success_message
	mov	x0, #1
	mov	x2, #3
	mov	x8, #NR_WRITE
	svc	#0
	mov	x0, #0
	b	.Lexit

.Lfailure:
	mov	x0, #1
.Lexit:
	mov	x8, #NR_EXIT_GROUP
	svc	#0
	brk	#0

	.size _start, .-_start

	.section .rodata
success_message:
	.ascii "OK\n"
