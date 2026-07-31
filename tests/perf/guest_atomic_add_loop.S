/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux scalar atomic translation control.
 *
 * Each process owns its counter. Parallel copies therefore measure direct
 * lowering and vCPU scaling without introducing guest-visible contention.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 500000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	.section .text
	.global _start
	.type _start, %function
_start:
	adrp	x20, counter
	add	x20, x20, :lo12:counter
	mov	x1, #1
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Lloop:
	ldaddal	x1, x0, [x20]
	subs	x19, x19, #1
	b.ne	.Lloop

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
