/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux register-offset scalar byte-transfer control.
 *
 * One-byte accesses cannot cross a guest page. The loop isolates the
 * production scalar byte gadget's valid TLB-hit path.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 200000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	.section .text
	.global _start
	.type _start, %function
_start:
	adrp	x0, data_page
	add	x0, x0, :lo12:data_page
	mov	x2, xzr
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Lloop:
	ldrb	w1, [x0, x2]
	add	w1, w1, #1
	strb	w1, [x0, x2]
	subs	x19, x19, #1
	b.ne	.Lloop

	ldrb	w20, [x0]
	mov	w21, #(ITERATIONS & 0xff)
	cmp	w20, w21
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
	.p2align 12
data_page:
	.byte 0
