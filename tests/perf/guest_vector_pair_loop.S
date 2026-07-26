/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux same-page SIMD pair-transfer control.
 *
 * The two 128-bit transfers are deliberately adjacent and page-contained.
 * This isolates the production vector-pair gadget's TLB-hit path.
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
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x22, #1
	dup	v1.2d, x22

.Lloop:
	ldp	q0, q2, [x0]
	add	v0.2d, v0.2d, v1.2d
	stp	q0, q2, [x0]
	subs	x19, x19, #1
	b.ne	.Lloop

	umov	x20, v0.d[0]
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
	brk	#0

	.size _start, .-_start

	.section .rodata
success_message:
	.ascii "OK\n"

	.section .data
	.p2align 12
data_page:
	.quad 0, 0, 0, 0
