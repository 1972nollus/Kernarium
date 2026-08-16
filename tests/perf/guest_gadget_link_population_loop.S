/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Gadget link-table reset scope probe.
 *
 * The guest code-change sequence carries no range, so every advance has to
 * drop every link activation. What that costs depends on how the reset is
 * scoped: over every record the translator ever created, or over the records
 * that actually hold an activation. The two diverge with the size of the
 * translated code, because the translator names a link record for every branch
 * target it emits while only a resident block can hold an activation. A real
 * browser guest sits about an order of magnitude apart.
 *
 * A hot loop alone cannot see that -- its guest names a handful of targets.
 * This probe walks a long chain of distinct single-branch blocks once, which
 * leaves thousands of named targets behind and never executes them again, and
 * only then enters the timed IC IVAU loop. The timed work is identical to
 * guest_gadget_code_invalidation_loop; the standing link population is the
 * whole difference. guest_gadget_link_population_control_loop builds the same
 * population and substitutes DC CVAU, so it holds the setup fixed while
 * removing the sequence advance that triggers a reset.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94
#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 500000
#endif
#ifndef BENCH_LINK_TARGETS
#define BENCH_LINK_TARGETS 4096
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	.section .text
	.global _start
	.type _start, %function
_start:
	b	.Lchain_last

/* Each block branches to the instruction before it, so the target always lies
 * behind the translator's forward fetch window and has to become a link rather
 * than being folded into the same block. Entering at the end therefore walks
 * the chain backwards, translating BENCH_LINK_TARGETS distinct blocks and
 * naming that many distinct targets.
 */
.Lchain_exit:
	b	.Lhot_entry
	.rept (BENCH_LINK_TARGETS - 1)
	b	. - 4
	.endr
.Lchain_last:
	b	. - 4

.Lhot_entry:
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, xzr
	adrp	x2, .Lunrelated
	add	x2, x2, :lo12:.Lunrelated

.Lloop:
	add	x20, x20, #1
	ic	ivau, x2
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
