/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Matched control for the gadget stream-index reactivation probe.
 *
 * Identical to guest_gadget_reindex_loop, including the full-block integer
 * body, except that the timed loop executes DC CVAU. That retains the same
 * cache-maintenance execution boundary without advancing the guest code-change
 * sequence, so the block stays linked and is never revalidated or reactivated
 * and no stream index walk ever runs. A change to how reactivation re-derives
 * the index must move the sensitive probe and leave this one still.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 2000000
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
	mov	x3, #1
	mov	x4, #2
	mov	x5, #3
	mov	x6, #4

.Lloop:
	add	x20, x20, #1
	dc	cvau, x2
	/* Fifty-six instructions of ordinary integer work, enough that the
	 * translated block spans many stream index granules. They are pure
	 * register arithmetic so the timed loop measures the reactivation walk
	 * rather than memory behaviour.
	 */
	.rept 14
	add	x3, x3, x4
	eor	x5, x5, x6
	sub	x4, x4, x3
	orr	x6, x6, x5
	.endr
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
