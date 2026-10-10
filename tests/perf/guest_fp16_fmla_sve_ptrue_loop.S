/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux FP16 multiply-accumulate in SVE under a PTRUE
 * predicate. Geekbench's Background Blur SVE kernel runs its main loops this
 * way: LD1H, LD1B and FMLA governed by a PTRUE, the loop closed by an add and
 * a CMP with B.LO, and WHILELO only in the tail. This probe differs from
 * guest_fp16_fmla_sve_loop only in that loop control, so the pair separates
 * what a WHILELO-governed loop costs from what the predicated LD1H, FMLA
 * and ST1H cost; guest_fp16_fmla_neon_loop is the control for both. It runs
 * only where the guest has SVE, under the guest-SVE policy.
 */

#include "probe_harness.inc"
#include "fp16_fmla_probe.inc"

	probe_start
	fmla_setup
	mov	x5, #FMLA_ELEMENTS
	ptrue	p0.h

.Lpass:
	mov	x4, #0
.Lloop:
	ld1h	{z0.h}, p0/z, [x0, x4, lsl #1]
	ld1h	{z1.h}, p0/z, [x1, x4, lsl #1]
	ld1h	{z2.h}, p0/z, [x2, x4, lsl #1]
	fmla	z2.h, p0/m, z0.h, z1.h
	st1h	{z2.h}, p0, [x2, x4, lsl #1]
	inch	x4
	cmp	x4, x5
	b.lo	.Lloop
	subs	x19, x19, #1
	b.ne	.Lpass

	fmla_check
	probe_finish

	probe_end
	fmla_arrays_section
