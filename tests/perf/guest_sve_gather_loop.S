/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux SVE gather, as a compiler vectorizes
 * acc[i % 4] += table[index[i]] (sve_gather_probe.inc): LD1W of the
 * indices, a gather LD1W with a scaled unsigned 32-bit index, and an ADD
 * per vector, over an all-true predicate. It runs only where the guest has
 * SVE, under the guest-SVE policy; guest_sve_gather_neon_loop is its
 * control. No SVE state is live across a system call.
 */

#include "probe_harness.inc"
#include "sve_gather_probe.inc"

	probe_start
	gather_setup
	mov	x6, #GATHER_WORDS
	ptrue	p0.s
	mov	z2.s, #0

.Lpass:
	mov	x4, #0
.Lloop:
	ld1w	{z1.s}, p0/z, [x1, x4, lsl #2]
	ld1w	{z0.s}, p0/z, [x0, z1.s, uxtw #2]
	add	z2.s, z2.s, z0.s
	incw	x4
	cmp	x4, x6
	b.lo	.Lloop
	subs	x19, x19, #1
	b.ne	.Lpass

	gather_check
	probe_finish

	probe_end
	gather_tables_section
