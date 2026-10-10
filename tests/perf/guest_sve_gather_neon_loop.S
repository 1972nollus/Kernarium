/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NEON control of the SVE gather probe (sve_gather_probe.inc): the same
 * accumulation with a vector load of four indices, four scalar loads
 * through them and lane inserts, as a loop without SVE is written.
 */

#include "probe_harness.inc"
#include "sve_gather_probe.inc"

	probe_start
	gather_setup
	mov	x6, #GATHER_WORDS
	movi	v2.4s, #0

.Lpass:
	mov	x4, #0
.Lloop:
	add	x11, x1, x4, lsl #2
	ldr	q1, [x11]
	umov	w5, v1.s[0]
	ldr	w7, [x0, x5, lsl #2]
	umov	w5, v1.s[1]
	ldr	w8, [x0, x5, lsl #2]
	umov	w5, v1.s[2]
	ldr	w9, [x0, x5, lsl #2]
	umov	w5, v1.s[3]
	ldr	w10, [x0, x5, lsl #2]
	mov	v0.s[0], w7
	mov	v0.s[1], w8
	mov	v0.s[2], w9
	mov	v0.s[3], w10
	add	v2.4s, v2.4s, v0.4s
	add	x4, x4, #4
	cmp	x4, x6
	b.lo	.Lloop
	subs	x19, x19, #1
	b.ne	.Lpass

	gather_check
	probe_finish

	probe_end
	gather_tables_section
