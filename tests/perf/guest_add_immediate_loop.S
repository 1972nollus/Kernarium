/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for ordinary ADD-immediate gadgets.
 *
 * The dependency chain keeps every result live while one linked branch is
 * amortized over sixteen measured instructions.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 100000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	.section .text
	.global _start
	.type _start, %function
_start:
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, #1

.Lloop:
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
	b.ne	.Lloop

	adrp	x1, success_message
	add	x1, x1, :lo12:success_message
	mov	x0, #1
	mov	x2, #3
	mov	x8, #NR_WRITE
	svc	#0
	mov	x0, #0
	mov	x8, #NR_EXIT_GROUP
	svc	#0
	brk	#0

	.size _start, .-_start

	.section .rodata
success_message:
	.ascii "OK\n"
