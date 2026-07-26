/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for translation-predecoded GPR operands.
 *
 * The unrolled body is dominated by ADD-immediate and EOR shifted-register
 * gadgets while retaining one linked loop edge per sixteen measured ops.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 500000000
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

.Lloop:
	add	x20, x20, #1
	eor	x21, x21, x20
	add	x20, x20, #3
	eor	x21, x21, x20
	add	x20, x20, #5
	eor	x21, x21, x20
	add	x20, x20, #7
	eor	x21, x21, x20
	add	x20, x20, #9
	eor	x21, x21, x20
	add	x20, x20, #11
	eor	x21, x21, x20
	add	x20, x20, #13
	eor	x21, x21, x20
	add	x20, x20, #15
	eor	x21, x21, x20
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
