/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux homogeneous allocated-base memory-run benchmark.
 *
 * Each tight loop executes four adjacent scalar accesses through x13.  The
 * base is kept unchanged by every transfer so the JIT can prove one guest
 * page, perform one permission lookup, and emit the four remapped native
 * instructions directly.
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
	sub	sp, sp, #64
	mov	x13, sp

	mov	x9, #1
	mov	x10, #2
	mov	x11, #3
	mov	x12, #4
	stp	x9, x10, [x13]
	stp	x11, x12, [x13, #16]

	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lload_loop:
	ldr	x0, [x13]
	ldr	x1, [x13, #8]
	ldr	x2, [x13, #16]
	ldr	x3, [x13, #24]
	subs	x19, x19, #1
	b.ne	.Lload_loop

	cmp	x0, #1
	b.ne	.Lfailure
	cmp	x1, #2
	b.ne	.Lfailure
	cmp	x2, #3
	b.ne	.Lfailure
	cmp	x3, #4
	b.ne	.Lfailure

	mov	x4, #5
	mov	x5, #6
	mov	x6, #7
	mov	x7, #8
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lstore_loop:
	str	x4, [x13]
	str	x5, [x13, #8]
	str	x6, [x13, #16]
	str	x7, [x13, #24]
	subs	x19, x19, #1
	b.ne	.Lstore_loop

	ldr	x0, [x13]
	ldr	x1, [x13, #8]
	ldr	x2, [x13, #16]
	ldr	x3, [x13, #24]
	cmp	x0, #5
	b.ne	.Lfailure
	cmp	x1, #6
	b.ne	.Lfailure
	cmp	x2, #7
	b.ne	.Lfailure
	cmp	x3, #8
	b.ne	.Lfailure

	add	sp, sp, #64
	adrp	x1, success_message
	add	x1, x1, :lo12:success_message
	mov	x0, #1
	mov	x2, #3
	mov	x8, #NR_WRITE
	svc	#0
	mov	x0, #0
	b	.Lexit

.Lfailure:
	add	sp, sp, #64
	mov	x0, #1
.Lexit:
	mov	x8, #NR_EXIT_GROUP
	svc	#0
	brk	#0

	.size _start, .-_start

	.section .rodata
success_message:
	.ascii "OK\n"

	.section .note.GNU-stack, "", %progbits
