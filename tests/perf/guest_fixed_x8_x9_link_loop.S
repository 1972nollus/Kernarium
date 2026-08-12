/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux fixed-x8/x9 linked-block throughput probe.
 *
 * The backward edge repeatedly links a block whose arithmetic keeps the
 * exact guest x8/x9 carry-bank pair live. Compare it with the historical
 * x0/x1 fixed-GPR loop to separate carry benefit from entry/exit overhead.
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
	mov	x8, xzr
	mov	x9, xzr

.Lloop:
	add	x8, x8, #1
	eor	x9, x9, x8
	subs	x19, x19, #1
	b.ne	.Lloop

	movz	x20, #(ITERATIONS & 0xffff)
	movk	x20, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x8, x20
	b.ne	.Lfailure
	and	x21, x20, #3
	cbnz	x21, .Lxor_nonzero
	mov	x21, x20
	b	.Lxor_ready
.Lxor_nonzero:
	cmp	x21, #1
	b.eq	.Lxor_one
	cmp	x21, #2
	b.eq	.Lxor_plus_one
	mov	x21, xzr
	b	.Lxor_ready
.Lxor_one:
	mov	x21, #1
	b	.Lxor_ready
.Lxor_plus_one:
	add	x21, x20, #1
.Lxor_ready:
	cmp	x9, x21
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
