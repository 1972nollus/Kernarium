/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux indexed vector FMLA throughput control.
 *
 * Zero operands keep the architecturally verified result stable while the
 * destination remains live across every iteration.
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
	movi	v0.16b, #0
	movi	v1.16b, #0
	movi	v2.16b, #0

.Lloop:
	fmla	v0.4s, v1.4s, v2.s[0]
	subs	x19, x19, #1
	b.ne	.Lloop

	umov	x20, v0.d[0]
	cbnz	x20, .Lfailure
	umov	x20, v0.d[1]
	cbnz	x20, .Lfailure

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
