/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux fixed-GPR linked-block throughput control.
 *
 * The backward edge repeatedly links a block whose arithmetic keeps guest
 * x0/x1 live. This isolates the native JIT's fixed cross-block register bank
 * from guest-memory traffic.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 1000000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	.section .text
	.global _start
	.type _start, %function
_start:
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x0, xzr
	mov	x1, xzr

.Lloop:
	add	x0, x0, #1
	eor	x1, x1, x0
	subs	x19, x19, #1
	b.ne	.Lloop

	movz	x20, #(ITERATIONS & 0xffff)
	movk	x20, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x0, x20
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
