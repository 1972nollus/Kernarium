/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux indirect branch/return throughput control.
 *
 * Each iteration executes one monomorphic BLR and one monomorphic RET. The
 * arithmetic and direct loop edge remain unchanged between emulator builds,
 * isolating data-only indirect successor-link performance.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
	.equ ITERATIONS,	50000000

	.section .text
	.global _start
	.type _start, %function
_start:
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, xzr
	adr	x22, .Lcallee

.Lloop:
	blr	x22
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
	brk	#0

.Lcallee:
	add	x20, x20, #1
	ret

	.size _start, .-_start

	.section .rodata
success_message:
	.ascii "OK\n"
