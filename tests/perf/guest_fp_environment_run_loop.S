/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux native FP-environment run-throughput control.
 *
 * Sixteen dependent FMULs share one translated block.  This distinguishes a
 * program-entry environment check from an instruction-local guard while the
 * multiply-by-one result remains exactly self-checking.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
	.equ ITERATIONS,	400000000

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
	.rept 16
	fmul	d0, d0, d1
	.endr
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
