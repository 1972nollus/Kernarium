/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The arrays, setup and check shared by the FP16 multiply-accumulate
 * probes: c[i] += a[i] * b[i] over 4,096 L1-resident FP16 elements, the
 * shape of Geekbench's SVE FP16 kernels, as a NEON loop of one Q register
 * per iteration and as the SVE loop a compiler writes. Both run the same
 * passes on the same data, so their times compare an engine's SVE path
 * with its NEON path.
 *
 * fmla_setup leaves a, b and c in x0, x1 and x2, a and b all 1.0 and c
 * all 0.0, and the pass count in x19. Each pass adds 1.0 to every c[i],
 * which stops at 2048.0, where FP16 rounds 2049 back to even;
 * fmla_check requires that value everywhere.
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 400000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS
	.equ FMLA_ELEMENTS,	4096
	.equ FMLA_BYTES,	FMLA_ELEMENTS * 2

	.if ITERATIONS < 2048
	.error "the check needs at least 2048 passes"
	.endif

.macro fmla_setup
	adrp	x0, fmla_arrays
	add	x0, x0, :lo12:fmla_arrays
	add	x1, x0, #FMLA_BYTES
	add	x2, x1, #FMLA_BYTES
	fmov	v0.8h, #1.0
	mov	x3, #0
1:
	str	q0, [x0, x3]
	str	q0, [x1, x3]
	add	x3, x3, #16
	cmp	x3, #FMLA_BYTES
	b.lo	1b
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.endm

.macro fmla_check
	mov	x3, #0
	mov	w5, #0x6800
1:
	ldrh	w4, [x2, x3]
	cmp	w4, w5
	b.ne	.Lfailure
	add	x3, x3, #2
	cmp	x3, #FMLA_BYTES
	b.lo	1b
.endm

.macro fmla_arrays_section
	.section .bss
	.balign	64
fmla_arrays:
	.skip	3 * FMLA_BYTES
.endm
