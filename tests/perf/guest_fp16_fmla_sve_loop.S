/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux FP16 multiply-accumulate in SVE, as a compiler
 * vectorizes c[i] += a[i] * b[i] (fp16_fmla_probe.inc): WHILELO, three
 * LD1H, a predicated FMLA, ST1H and INCH per vector, and a WHILELO with
 * B.FIRST closing the loop. It runs only where the guest has SVE, under
 * the guest-SVE policy; guest_fp16_fmla_neon_loop is its control. No
 * predicate is live across a system call, so a guest kernel that keeps
 * no SVE state does not disturb it.
 */

#include "probe_harness.inc"
#include "fp16_fmla_probe.inc"

	probe_start
	fmla_setup
	mov	x5, #FMLA_ELEMENTS

.Lpass:
	mov	x4, #0
	whilelo	p0.h, x4, x5
.Lloop:
	ld1h	{z0.h}, p0/z, [x0, x4, lsl #1]
	ld1h	{z1.h}, p0/z, [x1, x4, lsl #1]
	ld1h	{z2.h}, p0/z, [x2, x4, lsl #1]
	fmla	z2.h, p0/m, z0.h, z1.h
	st1h	{z2.h}, p0, [x2, x4, lsl #1]
	inch	x4
	whilelo	p0.h, x4, x5
	b.first	.Lloop
	subs	x19, x19, #1
	b.ne	.Lpass

	fmla_check
	probe_finish

	probe_end
	fmla_arrays_section
