/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux sanity check for the advertised HWCAP families.
 *
 * G125 advertises a family only where an engine has committed to executing it,
 * but binary analysis of Geekbench 5, 6 and 7 found that several of those
 * families never appear in any of those binaries: zero LDAPR, LDAPUR, STLUR,
 * FCADD, FCMLA, CFINV, RMIF, SETF, AXFLAG, XAFLAG, SQRDMLAH, FMLAL, FJCVTZS and FRINT32/64 across all three. A
 * benchmark therefore cannot tell us whether those advertisements are honest -
 * a flat score is what both "correct" and "broken" look like when nothing
 * executes the instruction.
 *
 * This executes one instance of each advertised family with inputs whose
 * results are checked against constants derived by hand from DDI0487, and
 * exits with a distinct status per family so a failure names itself. It reads
 * AT_HWCAP and AT_HWCAP2 from its auxiliary vector and skips each family the
 * kernel does not advertise, so an engine is held to exactly what it
 * advertises. It is a correctness check, not a benchmark.
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
	.equ E_CFINV,		13
	.equ E_RMIF,		14
	.equ E_SETF,		15
	.equ E_AXFLAG,		16
	.equ E_XAFLAG,		17
	.equ E_USCAT,		18
	.equ E_SB,		19
	.equ E_DIT,		20
	.equ E_AUXV,		21
	.equ E_JSCVT_VALUE,	22
	.equ E_JSCVT_NZCV,	23
	.equ E_JSCVT_FPSR,	24
	.equ JSCVT_CASES,	16
	.equ E_ECV,		25
	.equ E_CSSC,		26
	.equ E_MOPS,		27

	.equ AT_HWCAP,		16
	.equ AT_HWCAP2,		26

	/* Bit numbers in AT_HWCAP and AT_HWCAP2, from arm64 Linux's uapi hwcap.h. */
	.equ HWCAP_FP,		0
	.equ HWCAP_ASIMD,	1
	.equ HWCAP_ATOMICS,	8
	.equ HWCAP_FPHP,	9
	.equ HWCAP_ASIMDRDM,	12
	.equ HWCAP_JSCVT,	13
	.equ HWCAP_FCMA,	14
	.equ HWCAP_LRCPC,	15
	.equ HWCAP_ASIMDDP,	20
	.equ HWCAP_ASIMDFHM,	23
	.equ HWCAP_DIT,		24
	.equ HWCAP_USCAT,	25
	.equ HWCAP_ILRCPC,	26
	.equ HWCAP_FLAGM,	27
	.equ HWCAP_SB,		29
	.equ HWCAP2_FLAGM2,	7
	.equ HWCAP2_FRINT,	8
	.equ HWCAP2_ECV,	19
	.equ HWCAP2_CSSC,	34
	.equ HWCAP2_MOPS,	43

	/* The engine smoke accepts the payload only when it prints exactly "OK"
	 * and exits zero, so a failure reports the family by name on stdout
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
	/* A static program starts with sp at argc, then argv[] and its NULL,
	 * envp[] and its NULL, and the auxiliary vector's (type, value) pairs up
	 * to AT_NULL. x28 and x29 keep AT_HWCAP and AT_HWCAP2. */
	ldr	x0, [sp]
	mov	x1, sp
	add	x1, x1, x0, lsl #3
	add	x1, x1, #16
.Lskip_environment:
	ldr	x2, [x1], #8
	cbnz	x2, .Lskip_environment
	mov	x28, #0
	mov	x29, #0
.Lread_auxv:
	ldp	x2, x3, [x1], #16
	cbz	x2, .Lread_auxv_done
	cmp	x2, #AT_HWCAP
	csel	x28, x3, x28, eq
	cmp	x2, #AT_HWCAP2
	csel	x29, x3, x29, eq
	b	.Lread_auxv
.Lread_auxv_done:
	/* Every arm64 kernel advertises FP and ASIMD, so without them the walk
	 * did not find AT_HWCAP and every family would be skipped. */
	tbz	x28, #HWCAP_FP, .Lfail_auxv
	tbz	x28, #HWCAP_ASIMD, .Lfail_auxv

	adrp	x19, scratch
	add	x19, x19, :lo12:scratch

	tbz	x28, #HWCAP_LRCPC, .Lskip_lrcpc
	/* FEAT_LRCPC: LDAPR returns the value most recently stored. */
	movz	x1, #0x7788
	movk	x1, #0x5566, lsl #16
	movk	x1, #0x3344, lsl #32
	movk	x1, #0x1122, lsl #48
	str	x1, [x19]
	ldapr	x2, [x19]
	cmp	x2, x1
	b.ne	.Lfail_ldapr
.Lskip_lrcpc:

	tbz	x28, #HWCAP_ILRCPC, .Lskip_ilrcpc
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
.Lskip_ilrcpc:

	tbz	x28, #HWCAP_FCMA, .Lskip_fcma
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
.Lskip_fcma:

	tbz	x28, #HWCAP_ASIMDDP, .Lskip_asimddp
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
.Lskip_asimddp:

	tbz	x28, #HWCAP_ASIMDRDM, .Lskip_asimdrdm
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
.Lskip_asimdrdm:

	tbz	x28, #HWCAP_JSCVT, .Lskip_jscvt
	/* FEAT_JSCVT: FJCVTZS truncates a double toward zero into a 32-bit
	 * result. -2.5 becomes -2. */
	adrp	x15, jscvt_input
	add	x15, x15, :lo12:jscvt_input
	ldr	d24, [x15]
	fjcvtzs	w16, d24
	cmn	w16, #2
	b.ne	.Lfail_fjcvtzs

	/* The special inputs, each from NZCV all set and FPSR clear. FJCVTZS
	 * sets NZCV to 0:Z:0:0, where Z says the result is exact: FPToFixedJS
	 * clears it for a NaN, an out-of-range or inexact value, and for any
	 * zero but +0.0, including a subnormal that FPCR.FZ flushes. Some
	 * cores, such as the M1, set Z for a flushed positive subnormal, and an
	 * engine that forwards that is wrong. */
	adrp	x15, jscvt_cases
	add	x15, x15, :lo12:jscvt_cases
	mov	x14, #JSCVT_CASES
.Ljscvt_case:
	ldp	x9, x10, [x15]
	msr	fpcr, x10
	msr	fpsr, xzr
	mov	x11, #0xf0000000
	msr	nzcv, x11
	fmov	d24, x9
	fjcvtzs	w16, d24
	mrs	x11, nzcv
	mrs	x12, fpsr
	msr	fpcr, xzr
	msr	fpsr, xzr
	ldp	x9, x10, [x15, #16]
	cmp	w16, w9
	b.ne	.Lfail_jscvt_value
	cmp	x11, x10
	b.ne	.Lfail_jscvt_nzcv
	ldr	x9, [x15, #32]
	cmp	x12, x9
	b.ne	.Lfail_jscvt_fpsr
	add	x15, x15, #40
	subs	x14, x14, #1
	b.ne	.Ljscvt_case
.Lskip_jscvt:

	tbz	x29, #HWCAP2_FRINT, .Lskip_frint
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
.Lskip_frint:

	tbz	x28, #HWCAP_FPHP, .Lskip_fphp
	/* FEAT_FP16: half-precision scalar add, 1.5 + 2.5 = 4.0. */
	adrp	x20, fp16_input
	add	x20, x20, :lo12:fp16_input
	ldr	h28, [x20]
	ldr	h29, [x20, #2]
	fadd	h30, h28, h29
	ldr	h31, [x20, #4]
	fcmp	h30, h31
	b.ne	.Lfail_fp16
.Lskip_fphp:

	tbz	x28, #HWCAP_ASIMDFHM, .Lskip_asimdfhm
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
.Lskip_asimdfhm:

	tbz	x28, #HWCAP_FLAGM, .Lskip_flagm
	/* FEAT_FlagM: CFINV inverts PSTATE.C and leaves the rest alone, so from
	 * all-clear the result is C set. Every expected NZCV word below was taken
	 * from running the same instruction natively on an arm64 host rather than
	 * derived by reading, because these encode flag semantics that are easy to
	 * transcribe backwards. */
	msr	nzcv, xzr
	cfinv
	mrs	x23, nzcv
	movz	x24, #0x2000, lsl #16
	cmp	x23, x24
	b.ne	.Lfail_cfinv

	/* RMIF rotates its source right by the shift then inserts the low four
	 * bits into NZCV under the mask. Source 5 with shift 0 and mask 0b1111
	 * gives Z and V set. */
	msr	nzcv, xzr
	mov	x25, #5
	rmif	x25, #0, #15
	mrs	x23, nzcv
	movz	x24, #0x5000, lsl #16
	cmp	x23, x24
	b.ne	.Lfail_rmif

	/* SETF8 sets N, Z and V from an 8-bit value and leaves C. 0x80 is
	 * negative and does not fit, so N and V set. SETF16 does the same for a
	 * 16-bit value. */
	msr	nzcv, xzr
	mov	w26, #0x80
	setf8	w26
	mrs	x23, nzcv
	movz	x24, #0x9000, lsl #16
	cmp	x23, x24
	b.ne	.Lfail_setf
	msr	nzcv, xzr
	movz	w26, #0x8000
	setf16	w26
	mrs	x23, nzcv
	cmp	x23, x24
	b.ne	.Lfail_setf
.Lskip_flagm:

	tbz	x29, #HWCAP2_FLAGM2, .Lskip_flagm2
	/* FEAT_FlagM2: AXFLAG rewrites the Arm floating-point unordered encoding
	 * (C and V set) into the alternate form (Z set). */
	movz	x27, #0x3000, lsl #16
	msr	nzcv, x27
	axflag
	mrs	x23, nzcv
	movz	x24, #0x4000, lsl #16
	cmp	x23, x24
	b.ne	.Lfail_axflag

	/* XAFLAG is the inverse direction: Z back to C and V. */
	movz	x27, #0x4000, lsl #16
	msr	nzcv, x27
	xaflag
	mrs	x23, nzcv
	movz	x24, #0x3000, lsl #16
	cmp	x23, x24
	b.ne	.Lfail_xaflag
.Lskip_flagm2:

	tbz	x28, #HWCAP_USCAT, .Lskip_uscat
	tbz	x28, #HWCAP_ATOMICS, .Lskip_uscat
	/* FEAT_LSE2 (HWCAP_USCAT): an LSE atomic on an address that is not
	 * naturally aligned must still complete single-copy atomically rather than
	 * fault. Offset 4 from a 16-byte aligned base is 4-byte aligned and so is
	 * unaligned for an 8-byte access. Verified natively before use. */
	add	x23, x19, #4
	movz	x24, #0x4444
	movk	x24, #0x3333, lsl #16
	movk	x24, #0x2222, lsl #32
	movk	x24, #0x1111, lsl #48
	str	x24, [x23]
	mov	x25, #1
	ldadd	x25, x26, [x23]
	cmp	x26, x24
	b.ne	.Lfail_uscat
	ldr	x27, [x23]
	add	x24, x24, #1
	cmp	x27, x24
	b.ne	.Lfail_uscat
.Lskip_uscat:

	tbz	x28, #HWCAP_SB, .Lskip_sb
	/* FEAT_SB: the speculation barrier must execute and retire. */
	sb
.Lskip_sb:

	tbz	x28, #HWCAP_DIT, .Lskip_dit
	/* FEAT_DIT: setting PSTATE.DIT must be observable through MRS. */
	msr	dit, #1
	mrs	x23, dit
	movz	x24, #0x100, lsl #16
	cmp	x23, x24
	b.ne	.Lfail_dit
	msr	dit, #0
.Lskip_dit:

	tbz	x29, #HWCAP2_ECV, .Lskip_ecv
	/* FEAT_ECV: CNTVCTSS_EL0 reads the same count as CNTVCT_EL0, so it
	 * lies between two reads of that, and ID_AA64MMFR0_EL1.ECV, the field
	 * Linux derives HWCAP2_ECV from, reads 1. */
	isb
	mrs	x23, cntvct_el0
	isb
	mrs	x24, s3_3_c14_c0_6	/* CNTVCTSS_EL0 */
	isb
	mrs	x25, cntvct_el0
	cmp	x24, x23
	b.lo	.Lfail_ecv
	cmp	x25, x24
	b.lo	.Lfail_ecv
	mrs	x26, id_aa64mmfr0_el1
	lsr	x26, x26, #60
	cmp	x26, #1
	b.ne	.Lfail_ecv
.Lskip_ecv:

	tbz	x29, #HWCAP2_CSSC, .Lskip_cssc
	/* FEAT_CSSC: one form of each operation, and ID_AA64ISAR2_EL1.CSSC,
	 * the field Linux derives HWCAP2_CSSC from, reads 1. The words are
	 * spelled out for assemblers that predate the extension. */
	mov	x23, #0x0100
	movk	x23, #0x00f0, lsl #48	/* 0x00f0000000000100 */
	mov	x25, #-5
	.inst	0xdac01ef8		/* cnt x24, x23 */
	cmp	x24, #5
	b.ne	.Lfail_cssc
	.inst	0xdac01af8		/* ctz x24, x23 */
	cmp	x24, #8
	b.ne	.Lfail_cssc
	.inst	0xdac02338		/* abs x24, x25 */
	cmp	x24, #5
	b.ne	.Lfail_cssc
	.inst	0x91c00f38		/* smax x24, x25, #3 */
	cmp	x24, #3
	b.ne	.Lfail_cssc
	.inst	0x9ad76f38		/* umin x24, x25, x23 */
	cmp	x24, x23
	b.ne	.Lfail_cssc
	mrs	x26, s3_0_c0_c6_2	/* ID_AA64ISAR2_EL1 */
	ubfx	x26, x26, #52, #4
	cmp	x26, #1
	b.ne	.Lfail_cssc
.Lskip_cssc:

	tbz	x29, #HWCAP2_MOPS, .Lskip_mops
	/* FEAT_MOPS: a CPY whose destination overlaps above its source, which
	 * must copy as if backward, then a SET, each as the prologue, main and
	 * epilogue a compiler emits; ID_AA64ISAR2_EL1.MOPS, the field Linux
	 * derives HWCAP2_MOPS from, reads 1. The scratch bytes start as their
	 * own offsets. */
	adrp	x22, scratch
	add	x22, x22, :lo12:scratch
	mov	x23, #0
1:	strb	w23, [x22, x23]
	add	x23, x23, #1
	cmp	x23, #64
	b.ne	1b
	add	x23, x22, #8		/* bytes 0-31 to 8-39 */
	mov	x25, x22
	mov	x24, #32
	.inst	0x1d190717		/* cpyp [x23]!, [x25]!, x24! */
	.inst	0x1d590717		/* cpym [x23]!, [x25]!, x24! */
	.inst	0x1d990717		/* cpye [x23]!, [x25]!, x24! */
	cbnz	x24, .Lfail_mops
	add	x23, x22, #48		/* bytes 48-55 to 0xa5 */
	mov	x24, #8
	mov	x25, #0xa5
	.inst	0x19d90717		/* setp [x23]!, x24!, x25 */
	.inst	0x19d94717		/* setm [x23]!, x24!, x25 */
	.inst	0x19d98717		/* sete [x23]!, x24!, x25 */
	cbnz	x24, .Lfail_mops
	add	x26, x22, #56
	cmp	x23, x26
	b.ne	.Lfail_mops
	mov	x23, #0
2:	ldrb	w24, [x22, x23]
	mov	x25, x23		/* 0-7, 40-47 and 56-63 are untouched */
	cmp	x23, #8
	b.lo	3f
	sub	x25, x23, #8		/* 8-39 hold 0-31 */
	cmp	x23, #40
	b.lo	3f
	mov	x25, x23
	cmp	x23, #48
	b.lo	3f
	mov	x26, #0xa5
	cmp	x23, #56
	csel	x25, x26, x25, lo
3:	cmp	x24, x25
	b.ne	.Lfail_mops
	add	x23, x23, #1
	cmp	x23, #64
	b.ne	2b
	mrs	x26, s3_0_c0_c6_2	/* ID_AA64ISAR2_EL1 */
	ubfx	x26, x26, #16, #4
	cmp	x26, #1
	b.ne	.Lfail_mops
.Lskip_mops:

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
.Lfail_cfinv:		fail E_CFINV, msg_cfinv
.Lfail_rmif:		fail E_RMIF, msg_rmif
.Lfail_setf:		fail E_SETF, msg_setf
.Lfail_axflag:		fail E_AXFLAG, msg_axflag
.Lfail_xaflag:		fail E_XAFLAG, msg_xaflag
.Lfail_uscat:		fail E_USCAT, msg_uscat
.Lfail_dit:		fail E_DIT, msg_dit
.Lfail_auxv:		fail E_AUXV, msg_auxv
.Lfail_jscvt_value:	fail E_JSCVT_VALUE, msg_jscvt_value
.Lfail_jscvt_nzcv:	fail E_JSCVT_NZCV, msg_jscvt_nzcv
.Lfail_jscvt_fpsr:	fail E_JSCVT_FPSR, msg_jscvt_fpsr
.Lfail_ecv:		fail E_ECV, msg_ecv
.Lfail_cssc:		fail E_CSSC, msg_cssc
.Lfail_mops:		fail E_MOPS, msg_mops

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
	/* input, FPCR, result, NZCV, FPSR; FZ is FPCR bit 24, and the FPSR
	 * flags are IOC 0x01, IXC 0x10 and IDC 0x80. Derived from the Arm ARM's
	 * FPToFixedJS and FPUnpack. */
	.equ FZ, 1 << 24
	.equ JSCVT_Z, 0x40000000
jscvt_cases:
	.quad 0x0000000000000001, FZ, 0, 0, 0x80		/* +subnormal, flushed */
	.quad 0x000fffffffffffff, FZ, 0, 0, 0x80		/* largest +subnormal */
	.quad 0x8000000000000001, FZ, 0, 0, 0x80		/* -subnormal, flushed */
	.quad 0x0000000000000001, 0, 0, 0, 0x10		/* +subnormal, inexact */
	.quad 0x8000000000000001, 0, 0, 0, 0x10		/* -subnormal, inexact */
	.quad 0x0000000000000000, FZ, 0, JSCVT_Z, 0		/* +0.0 is exact */
	.quad 0x8000000000000000, FZ, 0, 0, 0		/* -0.0 is not */
	.quad 0x7ff8000000000000, FZ, 0, 0, 0x01		/* quiet NaN */
	.quad 0x7ff4000000000000, FZ, 0, 0, 0x01		/* signalling NaN */
	.quad 0x7ff0000000000000, FZ, 0, 0, 0x01		/* +infinity */
	.quad 0xfff0000000000000, FZ, 0, 0, 0x01		/* -infinity */
	.quad 0x41e0000000000000, FZ, 0x80000000, 0, 0x01	/* 2^31 overflows */
	.quad 0xc1e0000000000000, FZ, 0x80000000, JSCVT_Z, 0	/* -2^31 fits */
	.quad 0xc1e0000000200000, FZ, 0x7fffffff, 0, 0x01	/* -2^31 - 1 */
	.quad 0x41f0000000500000, FZ, 5, 0, 0x01		/* 2^32 + 5 wraps */
	.quad 0x4014000000000000, FZ, 5, JSCVT_Z, 0		/* 5.0 */
	.if (. - jscvt_cases) != JSCVT_CASES * 40
	.error "JSCVT_CASES does not count jscvt_cases"
	.endif
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
msg_cfinv:
	.ascii "FAIL cfinv      \n"
msg_rmif:
	.ascii "FAIL rmif       \n"
msg_setf:
	.ascii "FAIL setf       \n"
msg_axflag:
	.ascii "FAIL axflag     \n"
msg_xaflag:
	.ascii "FAIL xaflag     \n"
msg_uscat:
	.ascii "FAIL uscat      \n"
msg_dit:
	.ascii "FAIL dit        \n"
msg_auxv:
	.ascii "FAIL auxv       \n"
msg_jscvt_value:
	.ascii "FAIL jscvt value\n"
msg_jscvt_nzcv:
	.ascii "FAIL jscvt nzcv \n"
msg_jscvt_fpsr:
	.ascii "FAIL jscvt fpsr \n"
msg_ecv:
	.ascii "FAIL ecv        \n"
msg_cssc:
	.ascii "FAIL cssc       \n"
msg_mops:
	.ascii "FAIL mops       \n"
success_message:
	.ascii "OK\n"

	.section .bss
	.p2align 4
scratch:
	.space 64
