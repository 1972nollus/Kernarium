/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux homogeneous SP-memory-run control.
 *
 * The first tight loop executes four adjacent 64-bit loads from SP before
 * any other memory operation. The second executes four adjacent 64-bit
 * stores to SP. Distinct values make every transfer independently checked.
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

	mov	x9, #1
	mov	x10, #2
	mov	x11, #3
	mov	x12, #4
	stp	x9, x10, [sp]
	stp	x11, x12, [sp, #16]

	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lload_loop:
	ldr	x0, [sp]
	ldr	x1, [sp, #8]
	ldr	x2, [sp, #16]
	ldr	x3, [sp, #24]
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
	str	x4, [sp]
	str	x5, [sp, #8]
	str	x6, [sp, #16]
	str	x7, [sp, #24]
	subs	x19, x19, #1
	b.ne	.Lstore_loop

	ldr	x0, [sp]
	ldr	x1, [sp, #8]
	ldr	x2, [sp, #16]
	ldr	x3, [sp, #24]
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
