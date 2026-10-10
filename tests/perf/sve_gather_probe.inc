/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The tables, setup and check shared by the gather probes: acc[l] +=
 * table[index[4j + l]] over 1,024 L1-resident words, the shape of a table
 * lookup or a sparse dot product, as an SVE gather and as the four scalar
 * loads a NEON loop makes. Both run the same passes on the same data, so
 * their times compare an engine's SVE gather with its NEON equivalent.
 *
 * gather_setup leaves the table in x0, the indices in x1 and the pass count
 * in x19. table[i] = 3i + 1 and index[i] = 37i + 5 modulo 1,024, a
 * permutation, so a pass adds the sum of the whole table, 1,572,352, to the
 * four accumulator lanes together; gather_check requires N passes' worth,
 * modulo 2^32, in the sum of the lanes (v2 on exit from the loop).
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 200000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS
	.equ GATHER_WORDS,	1024
	.equ GATHER_BYTES,	GATHER_WORDS * 4
	.equ PASS_SUM,		1572352

.macro gather_setup
	adrp	x0, gather_tables
	add	x0, x0, :lo12:gather_tables
	add	x1, x0, #GATHER_BYTES
	mov	x3, #0
	mov	w5, #5
1:
	add	w4, w3, w3, lsl #1
	add	w4, w4, #1
	str	w4, [x0, x3, lsl #2]
	and	w5, w5, #(GATHER_WORDS - 1)
	str	w5, [x1, x3, lsl #2]
	add	w5, w5, #37
	add	x3, x3, #1
	cmp	x3, #GATHER_WORDS
	b.lo	1b
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.endm

/* The lanes' sum, x20 = ITERATIONS * PASS_SUM modulo 2^32, compared. */
.macro gather_check
	addv	s3, v2.4s
	mov	w3, v3.s[0]
	movz	x4, #(ITERATIONS & 0xffff)
	movk	x4, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	w5, #(PASS_SUM & 0xffff)
	movk	w5, #(PASS_SUM >> 16), lsl #16
	mul	w4, w4, w5
	cmp	w3, w4
	b.ne	.Lfailure
.endm

.macro gather_tables_section
	.section .bss
	.balign	64
gather_tables:
	.skip	2 * GATHER_BYTES
.endm
