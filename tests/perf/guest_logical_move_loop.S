/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for the logical-register MOV alias.
 *
 * Every measured MOV encodes as ORR Xd, XZR, Xm. The dependency ring keeps
 * all results live while one linked branch is amortized over sixteen gadgets.
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
	mov	x21, #2
	mov	x22, #3
	mov	x23, #4

.Lloop:
	mov	x20, x21
	mov	x21, x22
	mov	x22, x23
	mov	x23, x20
	mov	x20, x22
	mov	x22, x21
	mov	x21, x23
	mov	x23, x20
	mov	x20, x21
	mov	x21, x22
	mov	x22, x23
	mov	x23, x20
	mov	x20, x22
	mov	x22, x21
	mov	x21, x23
	mov	x23, x20
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
