/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Repeated integer vector MLA run for destination-residency experiments.
 * Thirty-two consecutive operations retain guest v0 as the destination;
 * v8/v9 are read-only sources and cannot alias that dirty value.
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
	movi	v0.4s, #0
	movi	v8.4s, #1
	movi	v9.4s, #1

.Lloop:
	.rept 32
	mla	v0.4s, v8.4s, v9.4s
	.endr
	subs	x19, x19, #1
	b.ne	.Lloop

	lsl	w20, w22, #5
	umov	w21, v0.s[0]
	cmp	w21, w20
	b.ne	.Lfailure
	umov	w21, v0.s[1]
	cmp	w21, w20
	b.ne	.Lfailure
	umov	w21, v0.s[2]
	cmp	w21, w20
	b.ne	.Lfailure
	umov	w21, v0.s[3]
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
