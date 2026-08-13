/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Matched control for destination residency. The timed topology is identical
 * to guest_simd_destination_residency_loop.S, but destination v8 is outside
 * the experimental v0-v7 bank and sources v16/v17 remain read-only.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 20000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	.section .text
	.global _start
	.type _start, %function
_start:
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x22, x19
	movi	v8.4s, #0
	movi	v16.4s, #1
	movi	v17.4s, #1

.Lloop:
	.rept 32
	mla	v8.4s, v16.4s, v17.4s
	.endr
	subs	x19, x19, #1
	b.ne	.Lloop

	lsl	w20, w22, #5
	umov	w21, v8.s[0]
	cmp	w21, w20
	b.ne	.Lfailure
	umov	w21, v8.s[1]
	cmp	w21, w20
	b.ne	.Lfailure
	umov	w21, v8.s[2]
	cmp	w21, w20
	b.ne	.Lfailure
	umov	w21, v8.s[3]
	cmp	w21, w20
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
