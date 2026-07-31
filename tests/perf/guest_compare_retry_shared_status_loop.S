/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux compare-and-retry control whose load result and
 * store status intentionally share one guest register. This is the common
 * library shape which must stay on the coordinated semantic path.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 50000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	.section .text
	.global _start
	.type _start, %function
_start:
	adrp	x20, counter
	add	x20, x20, :lo12:counter
	mov	x2, #0
	mov	x3, #1
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Lretry:
	ldxr	x0, [x20]
	cmp	x0, x2
	b.ne	.Lcompared
	stxr	w0, x3, [x20]
	cbnz	w0, .Lretry
.Lcompared:
	b.ne	.Lfailure
	add	x2, x2, #1
	add	x3, x3, #1
	subs	x19, x19, #1
	b.ne	.Lretry

	ldr	x4, [x20]
	movz	x5, #(ITERATIONS & 0xffff)
	movk	x5, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x4, x5
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

	.section .data
	.balign 16
counter:
	.xword 0
