/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for direct-mapped gadget-TLB aliases.
 *
 * The two touched pages are exactly 128 MiB apart.  They collide in a
 * 13-bit direct index with 16 KiB pages, but not in a 14-bit direct index.
 * Alternating the pages on every iteration makes accidental set aliasing
 * immediately visible without depending on a large application workload.
 */

#include "probe_harness.inc"

	.equ ITERATIONS,	5000000
	.equ ALIAS_DISTANCE,	(128 * 1024 * 1024)
	.equ GUEST_PAGE_SIZE,	16384

	probe_start
	adrp	x0, first_page
	add	x0, x0, :lo12:first_page
	adrp	x1, second_page
	add	x1, x1, :lo12:second_page
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Lloop:
	ldr	x2, [x0]
	add	x2, x2, #1
	str	x2, [x0]
	ldr	x3, [x1]
	add	x3, x3, #1
	str	x3, [x1]
	subs	x19, x19, #1
	b.ne	.Lloop

	ldr	x2, [x0]
	ldr	x3, [x1]
	movz	x4, #(ITERATIONS & 0xffff)
	movk	x4, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x2, x4
	b.ne	.Lfailure
	cmp	x3, x4
	b.ne	.Lfailure

	probe_finish

	probe_end

	.section .bss
	.p2align 14
first_page:
	.skip GUEST_PAGE_SIZE
	.skip ALIAS_DISTANCE - GUEST_PAGE_SIZE
second_page:
	.skip GUEST_PAGE_SIZE
