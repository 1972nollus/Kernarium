/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Cache-maintenance execution-boundary control for exact gadget validation.
 *
 * DC CVAU retains the sensitive probe's dispatch and ordering boundary but
 * does not advance the guest code-change sequence.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 5000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	.section .text
	.global _start
	.type _start, %function
_start:
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, xzr
	adrp	x2, .Lunrelated
	add	x2, x2, :lo12:.Lunrelated

.Lloop:
	add	x20, x20, #1
	dc	cvau, x2
	subs	x19, x19, #1
	b.ne	.Lloop

	movz	x21, #(ITERATIONS & 0xffff)
	movk	x21, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x20, x21
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
.Lunrelated:
	nop
	brk	#0

	.size _start, .-_start

	.section .rodata
success_message:
	.ascii "OK\n"
