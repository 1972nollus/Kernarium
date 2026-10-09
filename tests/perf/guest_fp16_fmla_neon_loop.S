/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux FP16 multiply-accumulate, the NEON control of
 * guest_fp16_fmla_sve_loop: c[i] += a[i] * b[i], one Q register of eight
 * elements per iteration (fp16_fmla_probe.inc).
 */

#include "probe_harness.inc"
#include "fp16_fmla_probe.inc"

	probe_start
	fmla_setup

.Lpass:
	mov	x4, #0
.Lloop:
	ldr	q0, [x0, x4]
	ldr	q1, [x1, x4]
	ldr	q2, [x2, x4]
	fmla	v2.8h, v0.8h, v1.8h
	str	q2, [x2, x4]
	add	x4, x4, #16
	cmp	x4, #FMLA_BYTES
	b.lo	.Lloop
	subs	x19, x19, #1
	b.ne	.Lpass

	fmla_check
	probe_finish

	probe_end
	fmla_arrays_section
