/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux sanity check for the advertised HWCAP families.
 *
 * G125 advertises a family only where an engine has committed to executing it,
 * but binary analysis of Geekbench 5, 6 and 7 found that several of those
 * families never appear in any of those binaries: zero LDAPR, LDAPUR, STLUR,
 * FCADD, FCMLA, SQRDMLAH, FMLAL, FJCVTZS and FRINT32/64 across all three. A
 * benchmark therefore cannot tell us whether those advertisements are honest -
 * a flat score is what both "correct" and "broken" look like when nothing
 * executes the instruction.
 *
 * This executes one instance of each advertised family with inputs whose
 * results are checked against constants derived by hand from DDI0487, and
 * exits with a distinct status per family so a failure names itself. It is a
 * correctness check, not a benchmark.
 *
 * Exit status: 0 all families correct, otherwise the failing family's number.
 */

/* Armv8.5 already implies FCMA, LRCPC2, JSCVT and FRINTTS, so only the
 * genuinely optional extensions are named. The spellings that are not implied
 * differ between toolchains - Apple clang rejects `jscvt`, `fcma` and
 * `rcpc-immo`, which the Homebrew LLVM accepts - so relying on the
 * architecture level rather than naming them keeps this assembling under both.
 * An assembler directive rather than a -march flag, so the file builds the
 * same way from any entry point. */
	.arch armv8.5-a+fp16+dotprod+rdm+fp16fml+rcpc

	.equ NR_WRITE,		64
	.equ NR_EXIT_GROUP,	94

	.equ E_LDAPR,		1
	.equ E_LDAPUR,		2
	.equ E_STLUR,		3
	.equ E_FCADD,		4
	.equ E_FCMLA,		5
	.equ E_SDOT,		6
	.equ E_UDOT,		7
	.equ E_SQRDMLAH,	8
	.equ E_FJCVTZS,		9
	.equ E_FRINT32Z,	10
	.equ E_FP16,		11
	.equ E_FMLAL,		12

	/* The shared probe runner accepts a payload only when it prints exactly
	 * "OK" and exits zero, so a failure reports the family by name on stdout
	 * where the runner will echo it, rather than by exit status alone. */
	.macro fail code, label
	adrp	x1, \label
	add	x1, x1, :lo12:\label
	mov	x2, #16
	mov	x0, #1
	mov	x8, #NR_WRITE
	svc	#0
	mov	x0, #\code
	b	.Lexit
	.endm

	.section .text
	.global _start
	.type _start, %function
_start:
	adrp	x19, scratch
	add	x19, x19, :lo12:scratch

	/* FEAT_LRCPC: LDAPR returns the value most recently stored. */
	movz	x1, #0x7788
	movk	x1, #0x5566, lsl #16
	movk	x1, #0x3344, lsl #32
	movk	x1, #0x1122, lsl #48
	str	x1, [x19]
	ldapr	x2, [x19]
	cmp	x2, x1
	b.ne	.Lfail_ldapr

	/* FEAT_LRCPC2: STLUR writes at an unscaled offset, LDAPUR reads it back.
	 * Offset 12 is deliberately unaligned to the 8-byte access size, which is
	 * the case the unscaled form exists to serve. */
	movz	x3, #0xbeef
	movk	x3, #0xdead, lsl #16
	stlur	w3, [x19, #12]
	ldapur	w4, [x19, #12]
	cmp	w4, w3
	b.ne	.Lfail_stlur
	ldr	w5, [x19, #12]
	cmp	w5, w3
	b.ne	.Lfail_ldapur

	/* FEAT_FCMA: FCADD Vd.2S, Vn.2S, Vm.2S, #90 computes, per complex pair,
	 * d.re = n.re - m.im and d.im = n.im + m.re. With n = (1.0, 2.0) and
	 * m = (3.0, 4.0) that is (1 - 4, 2 + 3) = (-3.0, 5.0). */
	adrp	x6, fcadd_n
	add	x6, x6, :lo12:fcadd_n
	ldr	d0, [x6]
	ldr	d1, [x6, #8]
	fcadd	v2.2s, v0.2s, v1.2s, #90
	adrp	x7, fcadd_expected
	add	x7, x7, :lo12:fcadd_expected
	ldr	d3, [x7]
	fcmp	s2, s3
	b.ne	.Lfail_fcadd
	mov	v4.s[0], v2.s[1]
	mov	v5.s[0], v3.s[1]
	fcmp	s4, s5
	b.ne	.Lfail_fcadd

	/* FCMLA Vd.2S, Vn.2S, Vm.2S, #0 accumulates d.re += n.re * m.re and
	 * d.im += n.re * m.im. Both lanes multiply by Vn's *real* element rather
	 * than by the matching one, which is easy to get backwards; the expected
	 * pair below was confirmed by running the same instruction natively on an
	 * arm64 host. From d = (0,0), n = (1,2), m = (3,4) that is (3.0, 4.0). */
	movi	v6.2d, #0
	fcmla	v6.2s, v0.2s, v1.2s, #0
	adrp	x8, fcmla_expected
	add	x8, x8, :lo12:fcmla_expected
	ldr	d7, [x8]
	fcmp	s6, s7
	b.ne	.Lfail_fcmla
	mov	v16.s[0], v6.s[1]
	mov	v17.s[0], v7.s[1]
	fcmp	s16, s17
	b.ne	.Lfail_fcmla

	/* FEAT_DotProd: SDOT Vd.2S, Vn.8B, Vm.8B sums four signed byte products
	 * into each 32-bit lane. Bytes 1,2,3,4 against 5,6,7,8 give
	 * 5 + 12 + 21 + 32 = 70 in lane 0. */
	adrp	x9, dot_a
	add	x9, x9, :lo12:dot_a
	ldr	d18, [x9]
	ldr	d19, [x9, #8]
	movi	v20.2d, #0
	sdot	v20.2s, v18.8b, v19.8b
	umov	w10, v20.s[0]
	cmp	w10, #70
	b.ne	.Lfail_sdot

	movi	v21.2d, #0
	udot	v21.2s, v18.8b, v19.8b
	umov	w11, v21.s[0]
	cmp	w11, #70
	b.ne	.Lfail_udot

	/* FEAT_RDM: SQRDMLAH Vd.4H, Vn.4H, Vm.4H rounds and doubles the product
	 * before accumulating. With d = 0 and n = m = 0x4000 (0.5 in Q15) the
	 * saturating doubled rounded product is 0x2000. */
	adrp	x12, rdm_input
	add	x12, x12, :lo12:rdm_input
	ldr	d22, [x12]
	movi	v23.2d, #0
	sqrdmlah	v23.4h, v22.4h, v22.4h
	umov	w13, v23.h[0]
	mov	w14, #0x2000
	cmp	w13, w14
	b.ne	.Lfail_sqrdmlah

	/* FEAT_JSCVT: FJCVTZS truncates a double toward zero into a 32-bit
	 * result. -2.5 becomes -2. */
	adrp	x15, jscvt_input
	add	x15, x15, :lo12:jscvt_input
	ldr	d24, [x15]
	fjcvtzs	w16, d24
	cmn	w16, #2
	b.ne	.Lfail_fjcvtzs

	/* FEAT_FRINTTS: FRINT32Z rounds toward zero to a 32-bit integral float.
	 * 2.7 becomes 2.0. */
	adrp	x17, frint_input
	add	x17, x17, :lo12:frint_input
	ldr	d25, [x17]
	frint32z	d26, d25
	adrp	x18, frint_expected
	add	x18, x18, :lo12:frint_expected
	ldr	d27, [x18]
	fcmp	d26, d27
	b.ne	.Lfail_frint32z

	/* FEAT_FP16: half-precision scalar add, 1.5 + 2.5 = 4.0. */
	adrp	x20, fp16_input
	add	x20, x20, :lo12:fp16_input
	ldr	h28, [x20]
	ldr	h29, [x20, #2]
	fadd	h30, h28, h29
	ldr	h31, [x20, #4]
	fcmp	h30, h31
	b.ne	.Lfail_fp16

	/* FEAT_FHM: FMLAL Vd.4S, Vn.4H, Vm.4H widens half-precision products into
	 * single precision and accumulates. 1.5 * 2.5 = 3.75 from zero. */
	adrp	x21, fhm_input
	add	x21, x21, :lo12:fhm_input
	ldr	d0, [x21]
	ldr	d1, [x21, #8]
	movi	v2.2d, #0
	fmlal	v2.4s, v0.4h, v1.4h
	adrp	x22, fhm_expected
	add	x22, x22, :lo12:fhm_expected
	ldr	s3, [x22]
	fcmp	s2, s3
	b.ne	.Lfail_fmlal

	adrp	x1, success_message
	add	x1, x1, :lo12:success_message
	mov	x0, #1
	mov	x2, #3
	mov	x8, #NR_WRITE
	svc	#0
	mov	x0, #0
	b	.Lexit

.Lfail_ldapr:		fail E_LDAPR, msg_ldapr
.Lfail_ldapur:		fail E_LDAPUR, msg_ldapur
.Lfail_stlur:		fail E_STLUR, msg_stlur
.Lfail_fcadd:		fail E_FCADD, msg_fcadd
.Lfail_fcmla:		fail E_FCMLA, msg_fcmla
.Lfail_sdot:		fail E_SDOT, msg_sdot
.Lfail_udot:		fail E_UDOT, msg_udot
.Lfail_sqrdmlah:	fail E_SQRDMLAH, msg_sqrdmlah
.Lfail_fjcvtzs:		fail E_FJCVTZS, msg_fjcvtzs
.Lfail_frint32z:	fail E_FRINT32Z, msg_frint32z
.Lfail_fp16:		fail E_FP16, msg_fp16
.Lfail_fmlal:		fail E_FMLAL, msg_fmlal

.Lexit:
	mov	x8, #NR_EXIT_GROUP
	svc	#0
	brk	#0

	.size _start, .-_start

	.section .rodata
	.p2align 3
fcadd_n:
	.float 1.0, 2.0			/* n = (re 1.0, im 2.0) */
	.float 3.0, 4.0			/* m = (re 3.0, im 4.0) */
fcadd_expected:
	.float -3.0, 5.0
fcmla_expected:
	.float 3.0, 4.0
dot_a:
	.byte 1, 2, 3, 4, 0, 0, 0, 0
	.byte 5, 6, 7, 8, 0, 0, 0, 0
rdm_input:
	.hword 0x4000, 0, 0, 0
jscvt_input:
	.double -2.5
frint_input:
	.double 2.7
frint_expected:
	.double 2.0
fp16_input:
	.hword 0x3e00			/* 1.5 */
	.hword 0x4100			/* 2.5 */
	.hword 0x4400			/* 4.0 */
	.hword 0
fhm_input:
	.hword 0x3e00, 0, 0, 0		/* 1.5 */
	.hword 0x4100, 0, 0, 0		/* 2.5 */
fhm_expected:
	.float 3.75
msg_ldapr:
	.ascii "FAIL ldapr      \n"
msg_ldapur:
	.ascii "FAIL ldapur     \n"
msg_stlur:
	.ascii "FAIL stlur      \n"
msg_fcadd:
	.ascii "FAIL fcadd      \n"
msg_fcmla:
	.ascii "FAIL fcmla      \n"
msg_sdot:
	.ascii "FAIL sdot       \n"
msg_udot:
	.ascii "FAIL udot       \n"
msg_sqrdmlah:
	.ascii "FAIL sqrdmlah   \n"
msg_fjcvtzs:
	.ascii "FAIL fjcvtzs    \n"
msg_frint32z:
	.ascii "FAIL frint32z   \n"
msg_fp16:
	.ascii "FAIL fp16       \n"
msg_fmlal:
	.ascii "FAIL fmlal      \n"
success_message:
	.ascii "OK\n"

	.section .bss
	.p2align 4
scratch:
	.space 64
