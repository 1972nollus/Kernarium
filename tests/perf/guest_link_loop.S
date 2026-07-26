/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux branch-link throughput control.
 *
 * The conditional backward edge remains entirely in translated guest code,
 * making this a focused measurement of cached gadget-block successor links.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 300000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	.section .text
	.global _start
	.type _start, %function
_start:
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, xzr
	mov	x21, xzr

.Lloop:
	add	x20, x20, #1
	eor	x21, x21, x20
	subs	x19, x19, #1
	b.ne	.Lloop

	movz	x22, #(ITERATIONS & 0xffff)
	movk	x22, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x20, x22
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
