/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux software-reservation publication control.
 *
 * The harmless register moves deliberately keep the reservation sequence
 * outside the JIT's bounded native three-instruction pattern. Ten private
 * copies therefore exercise the correctness fallback and its helper/store
 * phase without guest-visible data sharing.
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
	adrp	x21, mirror
	add	x21, x21, :lo12:mirror
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Literation:
	ldxr	x0, [x20]
	mov	x0, x0
	mov	x0, x0
	add	x0, x0, #1
	stxr	w1, x0, [x20]
	cbnz	w1, .Literation
	str	x0, [x21]
	subs	x19, x19, #1
	b.ne	.Literation

	ldr	x2, [x20]
	ldr	x3, [x21]
	movz	x4, #(ITERATIONS & 0xffff)
	movk	x4, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x2, x4
	b.ne	.Lfailure
	cmp	x3, x4
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
mirror:
	.xword 0
