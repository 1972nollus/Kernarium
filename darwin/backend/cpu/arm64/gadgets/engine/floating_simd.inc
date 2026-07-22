/* FMOV (immediate), decoded once by the builder into IEEE bits. */
gadget floating_move_immediate
	ldr x8, [_stream], #8
	ldr x9, [_stream], #8
	add x10, _cpu, #UML_GADGET_CPU_V0
	add x10, x10, x8, lsl #4
	str x9, [x10]
	str xzr, [x10, #8]
	next_gadget

/* Operation-specific scalar FP gadgets receive a predecoded operand:
 * destination[4:0], left/source[12:8], right[20:16]. Selecting these at build
 * time removes all guest-opcode branches from the arithmetic hot path.
 */
.macro floating_move_register name, bytes
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	add x13, x11, x9, lsl #4
	.if \bytes == 4
	ldr w14, [x12]
	str w14, [x13]
	str wzr, [x13, #4]
	.else
	ldr x14, [x12]
	str x14, [x13]
	.endif
	str xzr, [x13, #8]
	next_gadget
.endm

floating_move_register floating_move_single, 4
floating_move_register floating_move_double, 8

.macro floating_unary_body source, destination, operation
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	add x13, x11, x9, lsl #4
	movi v2.16b, #0
	ldr \source, [x12]
	\operation \destination, \source
	str q2, [x13]
	next_gadget
.endm

.macro floating_unary_exact name, source, destination, operation
gadget \name
	floating_unary_body \source, \destination, \operation
.endm

.macro floating_unary name, source, destination, operation
gadget \name
	ensure_fp_environment
	floating_unary_body \source, \destination, \operation
.endm

floating_unary_exact floating_absolute_single, s0, s2, fabs
floating_unary_exact floating_absolute_double, d0, d2, fabs
floating_unary_exact floating_negate_single, s0, s2, fneg
floating_unary_exact floating_negate_double, d0, d2, fneg
floating_unary floating_sqrt_single, s0, s2, fsqrt
floating_unary floating_sqrt_double, d0, d2, fsqrt

.macro floating_round_operation name, operation
floating_round_\name:
	tbnz x8, #22, 1f
	\operation s2, s0
	b floating_round_store
1:	\operation d2, d0
	b floating_round_store
.endm

/* FRINT* uses the host FP rounding mode and exception behavior. */
gadget floating_round_integral
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #15, #6
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	ldr d0, [x13]
	movi v2.16b, #0
	cmp w11, #8
	b.eq floating_round_n
	cmp w11, #9
	b.eq floating_round_p
	cmp w11, #10
	b.eq floating_round_m
	cmp w11, #11
	b.eq floating_round_z
	cmp w11, #12
	b.eq floating_round_a
	cmp w11, #14
	b.eq floating_round_x
	b floating_round_i

floating_round_operation n, frintn
floating_round_operation p, frintp
floating_round_operation m, frintm
floating_round_operation z, frintz
floating_round_operation a, frinta
floating_round_operation x, frintx
floating_round_operation i, frinti

floating_round_store:
	add x13, x12, x9, lsl #4
	str q2, [x13]
	next_gadget

.macro floating_binary name, left, right, destination, operation
gadget \name
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #5
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x14, x12, x11, lsl #4
	add x15, x12, x9, lsl #4
	movi v2.16b, #0
	ldr \left, [x13]
	ldr \right, [x14]
	\operation \destination, \left, \right
	str q2, [x15]
	next_gadget
.endm

floating_binary floating_multiply_single, s0, s1, s2, fmul
floating_binary floating_multiply_double, d0, d1, d2, fmul
floating_binary floating_divide_single, s0, s1, s2, fdiv
floating_binary floating_divide_double, d0, d1, d2, fdiv
floating_binary floating_add_single, s0, s1, s2, fadd
floating_binary floating_add_double, d0, d1, d2, fadd
floating_binary floating_subtract_single, s0, s1, s2, fsub
floating_binary floating_subtract_double, d0, d1, d2, fsub
floating_binary floating_negative_multiply_single, s0, s1, s2, fnmul
floating_binary floating_negative_multiply_double, d0, d1, d2, fnmul

/* Scalar FABD maps directly to the host and clears bits above the result. */
gadget floating_absolute_difference
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x14, x12, x11, lsl #4
	movi v2.16b, #0
	tbnz x8, #22, 1f
	ldr s0, [x13]
	ldr s1, [x14]
	fabd s2, s0, s1
	b 2f
1:	ldr d0, [x13]
	ldr d1, [x14]
	fabd d2, d0, d1
2:	add x13, x12, x9, lsl #4
	str q2, [x13]
	next_gadget

/* FCMP/FCMPE use host floating-point comparison and capture its NZCV. */
gadget floating_compare
	ensure_fp_environment
	ldr x8, [_stream], #8
	ubfx w9, w8, #5, #5
	ubfx w10, w8, #16, #5
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x12, x11, x9, lsl #4
	add x13, x11, x10, lsl #4
	tbnz x8, #22, floating_compare_double
	ldr s0, [x12]
	tbnz x8, #3, 1f
	ldr s1, [x13]
	tbnz x8, #4, 2f
	fcmp s0, s1
	b floating_compare_store
2:	fcmpe s0, s1
	b floating_compare_store
1:	tbnz x8, #4, 3f
	fcmp s0, #0.0
	b floating_compare_store
3:	fcmpe s0, #0.0
	b floating_compare_store
floating_compare_double:
	ldr d0, [x12]
	tbnz x8, #3, 4f
	ldr d1, [x13]
	tbnz x8, #4, 5f
	fcmp d0, d1
	b floating_compare_store
5:	fcmpe d0, d1
	b floating_compare_store
4:	tbnz x8, #4, 6f
	fcmp d0, #0.0
	b floating_compare_store
6:	fcmpe d0, #0.0
floating_compare_store:
	mrs x14, nzcv
	str w14, [_cpu, #UML_GADGET_CPU_NZCV]
	next_gadget

.macro floating_conditional_compare name, condition
gadget floating_conditional_compare_\name
	ensure_fp_environment
	ldr x8, [_stream], #8
	ldr w9, [_cpu, #UML_GADGET_CPU_NZCV]
	msr nzcv, x9
	b.\condition 1f
	and w9, w8, #0xf
	lsl w9, w9, #28
	str w9, [_cpu, #UML_GADGET_CPU_NZCV]
	next_gadget
1:	ubfx w9, w8, #5, #5
	ubfx w10, w8, #16, #5
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x12, x11, x9, lsl #4
	add x13, x11, x10, lsl #4
	tbnz x8, #22, 2f
	ldr s0, [x12]
	ldr s1, [x13]
	tbnz x8, #4, 3f
	fcmp s0, s1
	b 5f
3:	fcmpe s0, s1
	b 5f
2:	ldr d0, [x12]
	ldr d1, [x13]
	tbnz x8, #4, 4f
	fcmp d0, d1
	b 5f
4:	fcmpe d0, d1
5:	mrs x14, nzcv
	str w14, [_cpu, #UML_GADGET_CPU_NZCV]
	next_gadget
.endm

floating_conditional_compare eq, eq
floating_conditional_compare ne, ne
floating_conditional_compare hs, hs
floating_conditional_compare lo, lo
floating_conditional_compare mi, mi
floating_conditional_compare pl, pl
floating_conditional_compare vs, vs
floating_conditional_compare vc, vc
floating_conditional_compare hi, hi
floating_conditional_compare ls, ls
floating_conditional_compare ge, ge
floating_conditional_compare lt, lt
floating_conditional_compare gt, gt
floating_conditional_compare le, le
floating_conditional_compare always, al

/* Scalar FCVT between single and double precision uses the host converter. */
gadget floating_convert_precision
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	movi v2.16b, #0
	tbnz x8, #22, 1f
	ldr s0, [x12]
	fcvt d2, s0
	b 2f
1:	ldr d0, [x12]
	fcvt s2, d0
2:	add x12, x11, x9, lsl #4
	str q2, [x12]
	next_gadget

.macro floating_to_integer_operation name, operation
floating_integer_convert_\name:
	tbnz x8, #31, 1f
	tbnz x8, #22, 2f
	\operation w15, s0
	b floating_integer_convert_gpr_store
2:	\operation w15, d0
	b floating_integer_convert_gpr_store
1:	tbnz x8, #22, 3f
	\operation x15, s0
	b floating_integer_convert_gpr_store
3:	\operation x15, d0
	b floating_integer_convert_gpr_store
.endm

/* Integer/scalar-FP conversion maps directly to the host conversion family.
 * This covers every non-fixed-point rounding mode accepted by the decoder. */
gadget floating_integer_convert
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #3
	cmp w11, #2
	b.eq floating_integer_convert_to_float
	cmp w11, #3
	b.eq floating_integer_convert_to_float
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x12, x12, x10, lsl #4
	ldr d0, [x12]
	ubfx w12, w8, #19, #2
	cmp w11, #4
	b.hs 1f
	cbz w12, 2f
	cmp w12, #1
	b.eq 3f
	cmp w12, #2
	b.eq 4f
	tbnz x8, #16, floating_integer_convert_zu
	b floating_integer_convert_zs
2:	tbnz x8, #16, floating_integer_convert_nu
	b floating_integer_convert_ns
3:	tbnz x8, #16, floating_integer_convert_pu
	b floating_integer_convert_ps
4:	tbnz x8, #16, floating_integer_convert_mu
	b floating_integer_convert_ms
1:	tbnz x8, #16, floating_integer_convert_au
	b floating_integer_convert_as

floating_to_integer_operation ns, fcvtns
floating_to_integer_operation nu, fcvtnu
floating_to_integer_operation ps, fcvtps
floating_to_integer_operation pu, fcvtpu
floating_to_integer_operation ms, fcvtms
floating_to_integer_operation mu, fcvtmu
floating_to_integer_operation zs, fcvtzs
floating_to_integer_operation zu, fcvtzu
floating_to_integer_operation as, fcvtas
floating_to_integer_operation au, fcvtau

floating_integer_convert_gpr_store:
	cmp w9, #31
	b.eq 5f
	add x12, _cpu, #UML_GADGET_CPU_X0
	str x15, [x12, x9, lsl #3]
5:	next_gadget

floating_integer_convert_to_float:
	add x12, _cpu, #UML_GADGET_CPU_X0
	cmp w10, #31
	b.eq 6f
	ldr x15, [x12, x10, lsl #3]
	b 7f
6:	mov x15, xzr
7:	movi v0.16b, #0
	tbnz x8, #16, 8f
	tbnz x8, #31, 9f
	tbnz x8, #22, 10f
	scvtf s0, w15
	b floating_integer_convert_vector_store
10:	scvtf d0, w15
	b floating_integer_convert_vector_store
9:	tbnz x8, #22, 11f
	scvtf s0, x15
	b floating_integer_convert_vector_store
11:	scvtf d0, x15
	b floating_integer_convert_vector_store
8:	tbnz x8, #31, 12f
	tbnz x8, #22, 13f
	ucvtf s0, w15
	b floating_integer_convert_vector_store
13:	ucvtf d0, w15
	b floating_integer_convert_vector_store
12:	tbnz x8, #22, 14f
	ucvtf s0, x15
	b floating_integer_convert_vector_store
14:	ucvtf d0, x15
floating_integer_convert_vector_store:
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x12, x12, x9, lsl #4
	str q0, [x12]
	next_gadget

/* The fixed-point scale is encoded in the host opcode. This specialized hot
 * encoding keeps the LTP instruction native without a software fallback. */
gadget floating_to_integer_fixed_s32_9
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x11, x11, x10, lsl #4
	ldr s0, [x11]
	fcvtzs w12, s0, #9
	cmp w9, #31
	b.eq 1f
	add x11, _cpu, #UML_GADGET_CPU_X0
	str x12, [x11, x9, lsl #3]
1:	next_gadget

/* Fused operand: destination[4:0], first[12:8], addend[20:16],
 * second[28:24]. */
.macro floating_fused name, first, second, addend, destination, operation
gadget \name
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #24, #5
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	add x15, x13, x12, lsl #4
	add x16, x13, x11, lsl #4
	add x17, x13, x9, lsl #4
	movi v3.16b, #0
	ldr \first, [x14]
	ldr \second, [x15]
	ldr \addend, [x16]
	\operation \destination, \first, \second, \addend
	str q3, [x17]
	next_gadget
.endm

floating_fused floating_madd_single, s0, s1, s2, s3, fmadd
floating_fused floating_madd_double, d0, d1, d2, d3, fmadd
floating_fused floating_msub_single, s0, s1, s2, s3, fmsub
floating_fused floating_msub_double, d0, d1, d2, d3, fmsub
floating_fused floating_nmadd_single, s0, s1, s2, s3, fnmadd
floating_fused floating_nmadd_double, d0, d1, d2, d3, fnmadd
floating_fused floating_nmsub_single, s0, s1, s2, s3, fnmsub
floating_fused floating_nmsub_double, d0, d1, d2, d3, fnmsub

.macro simd_integer_float_operation name, operation
simd_integer_float_\name:
	tbnz x8, #28, 1f
	tbnz x8, #22, 2f
	tbnz x8, #30, 3f
	\operation v0.2s, v1.2s
	b simd_integer_float_store
3:	\operation v0.4s, v1.4s
	b simd_integer_float_store
2:	\operation v0.2d, v1.2d
	b simd_integer_float_store
1:	tbnz x8, #22, 4f
	\operation s0, s1
	b simd_integer_float_store
4:	\operation d0, d1
	b simd_integer_float_store
.endm

/* Scalar/vector SCVTF, UCVTF, FCVTZS, and FCVTZU use host FP/AdvSIMD. */
gadget simd_integer_float_convert
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	ldr q1, [x12]
	movi v0.16b, #0
	tbnz x8, #23, 1f
	tbnz x8, #29, simd_integer_float_ucvtf
	b simd_integer_float_scvtf
1:	tbnz x8, #29, simd_integer_float_fcvtzu
	b simd_integer_float_fcvtzs

simd_integer_float_operation scvtf, scvtf
simd_integer_float_operation ucvtf, ucvtf
simd_integer_float_operation fcvtzs, fcvtzs
simd_integer_float_operation fcvtzu, fcvtzu

simd_integer_float_store:
	add x12, x11, x9, lsl #4
	str q0, [x12]
	next_gadget

.macro simd_arithmetic_wide_operation name, operation, wide
simd_arithmetic_wide_\name:
	cmp w12, #0
	b.eq 1f
	cmp w12, #1
	b.eq 2f
	.if \wide
	\operation v0.2d, v1.2d, v2.2s
	.else
	\operation v0.2d, v1.2s, v2.2s
	.endif
	b simd_arithmetic_wide_store
1:	.if \wide
	\operation v0.8h, v1.8h, v2.8b
	.else
	\operation v0.8h, v1.8b, v2.8b
	.endif
	b simd_arithmetic_wide_store
2:	.if \wide
	\operation v0.4s, v1.4s, v2.4h
	.else
	\operation v0.4s, v1.4h, v2.4h
	.endif
	b simd_arithmetic_wide_store
.endm

/* Widening add/sub families use host AdvSIMD. Upper-half forms first move the
 * selected narrow lanes into the low half, avoiding a gadget matrix. */
gadget simd_arithmetic_wide
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #22, #2
	ubfx w13, w8, #12, #4
	add x14, _cpu, #UML_GADGET_CPU_V0
	add x15, x14, x10, lsl #4
	ldr q1, [x15]
	add x15, x14, x11, lsl #4
	ldr q2, [x15]
	tbz x8, #30, 1f
	ext v2.16b, v2.16b, v2.16b, #8
	tbz w13, #0, 2f
	b 1f
2:	ext v1.16b, v1.16b, v1.16b, #8
1:	cmp w13, #12
	b.eq simd_multiply_long
	cmp w13, #0
	b.eq 3f
	cmp w13, #1
	b.eq 4f
	cmp w13, #2
	b.eq 5f
	tbnz x8, #29, simd_arithmetic_wide_usubw
	b simd_arithmetic_wide_ssubw
3:	tbnz x8, #29, simd_arithmetic_wide_uaddl
	b simd_arithmetic_wide_saddl
4:	tbnz x8, #29, simd_arithmetic_wide_uaddw
	b simd_arithmetic_wide_saddw
5:	tbnz x8, #29, simd_arithmetic_wide_usubl
	b simd_arithmetic_wide_ssubl
simd_multiply_long:
	tbnz x8, #29, simd_arithmetic_wide_umull
	b simd_arithmetic_wide_smull

simd_arithmetic_wide_operation saddl, saddl, 0
simd_arithmetic_wide_operation uaddl, uaddl, 0
simd_arithmetic_wide_operation ssubl, ssubl, 0
simd_arithmetic_wide_operation usubl, usubl, 0
simd_arithmetic_wide_operation saddw, saddw, 1
simd_arithmetic_wide_operation uaddw, uaddw, 1
simd_arithmetic_wide_operation ssubw, ssubw, 1
simd_arithmetic_wide_operation usubw, usubw, 1
simd_arithmetic_wide_operation smull, smull, 0
simd_arithmetic_wide_operation umull, umull, 0

simd_arithmetic_wide_store:
	add x15, x14, x9, lsl #4
	str q0, [x15]
	next_gadget

/* Vector FABS/FNEG map directly to host AdvSIMD. */
gadget simd_floating_unary
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	ldr q1, [x12]
	movi v0.16b, #0
	tbnz x8, #22, 1f
	tbnz x8, #29, 2f
	tbnz x8, #30, 3f
	fabs v0.2s, v1.2s
	b 5f
3:	fabs v0.4s, v1.4s
	b 5f
2:	tbnz x8, #30, 4f
	fneg v0.2s, v1.2s
	b 5f
4:	fneg v0.4s, v1.4s
	b 5f
1:	tbnz x8, #29, 6f
	fabs v0.2d, v1.2d
	b 5f
6:	fneg v0.2d, v1.2d
5:	add x12, x11, x9, lsl #4
	str q0, [x12]
	next_gadget

/* Advanced SIMD bitwise operations. Operand: destination[4:0], left[12:8],
 * right[20:16], Q[24]. All sources are captured before an aliased destination
 * is written. */
.macro simd_bitwise name, operation
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #5
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x14, x12, x11, lsl #4
	add x15, x12, x9, lsl #4
	ldr q0, [x13]
	ldr q1, [x14]
	ldr q2, [x15]
	\operation v2.16b, v0.16b, v1.16b
	tbz x8, #24, 1f
	str q2, [x15]
	b 2f
1:	str d2, [x15]
	str xzr, [x15, #8]
2:	next_gadget
.endm

simd_bitwise simd_and, and
simd_bitwise simd_bic, bic
simd_bitwise simd_orr, orr
simd_bitwise simd_orn, orn
simd_bitwise simd_eor, eor
simd_bitwise simd_bsl, bsl
simd_bitwise simd_bit, bit
simd_bitwise simd_bif, bif

/* NOT/MVN occupies the same major opcode as CNT but is distinguished by U.
 * Keep it native and separate so the two unary operations cannot alias. */
gadget simd_not
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	ldr q1, [x12]
	not v0.16b, v1.16b
	add x12, x11, x9, lsl #4
	tbnz x8, #30, 1f
	str d0, [x12]
	str xzr, [x12, #8]
	b 2f
1:	str q0, [x12]
2:	next_gadget

/* Integer vector comparisons against zero map directly to host AdvSIMD. */
gadget simd_compare_zero
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #22, #2
	ubfx w12, w8, #12, #5
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	ldr q1, [x14]
	cmp w12, #8
	b.eq simd_compare_zero_gt_ge
	cmp w12, #9
	b.eq simd_compare_zero_eq_le
	b simd_compare_zero_lt
simd_compare_zero_gt_ge:
	tbnz x8, #29, simd_compare_zero_ge
	cmp w11, #0
	b.eq 1f
	cmp w11, #1
	b.eq 2f
	cmp w11, #2
	b.eq 3f
	cmgt v0.2d, v1.2d, #0
	b simd_compare_zero_store
1:	cmgt v0.16b, v1.16b, #0
	b simd_compare_zero_store
2:	cmgt v0.8h, v1.8h, #0
	b simd_compare_zero_store
3:	cmgt v0.4s, v1.4s, #0
	b simd_compare_zero_store
simd_compare_zero_ge:
	cmp w11, #0
	b.eq 4f
	cmp w11, #1
	b.eq 5f
	cmp w11, #2
	b.eq 6f
	cmge v0.2d, v1.2d, #0
	b simd_compare_zero_store
4:	cmge v0.16b, v1.16b, #0
	b simd_compare_zero_store
5:	cmge v0.8h, v1.8h, #0
	b simd_compare_zero_store
6:	cmge v0.4s, v1.4s, #0
	b simd_compare_zero_store
simd_compare_zero_eq_le:
	tbnz x8, #29, simd_compare_zero_le
	cmp w11, #0
	b.eq 7f
	cmp w11, #1
	b.eq 8f
	cmp w11, #2
	b.eq 9f
	cmeq v0.2d, v1.2d, #0
	b simd_compare_zero_store
7:	cmeq v0.16b, v1.16b, #0
	b simd_compare_zero_store
8:	cmeq v0.8h, v1.8h, #0
	b simd_compare_zero_store
9:	cmeq v0.4s, v1.4s, #0
	b simd_compare_zero_store
simd_compare_zero_le:
	cmp w11, #0
	b.eq 10f
	cmp w11, #1
	b.eq 11f
	cmp w11, #2
	b.eq 12f
	cmle v0.2d, v1.2d, #0
	b simd_compare_zero_store
10:	cmle v0.16b, v1.16b, #0
	b simd_compare_zero_store
11:	cmle v0.8h, v1.8h, #0
	b simd_compare_zero_store
12:	cmle v0.4s, v1.4s, #0
	b simd_compare_zero_store
simd_compare_zero_lt:
	cmp w11, #0
	b.eq 13f
	cmp w11, #1
	b.eq 14f
	cmp w11, #2
	b.eq 15f
	cmlt v0.2d, v1.2d, #0
	b simd_compare_zero_store
13:	cmlt v0.16b, v1.16b, #0
	b simd_compare_zero_store
14:	cmlt v0.8h, v1.8h, #0
	b simd_compare_zero_store
15:	cmlt v0.4s, v1.4s, #0
simd_compare_zero_store:
	add x14, x13, x9, lsl #4
	tbnz x8, #28, 18f
	tbnz x8, #30, 16f
	18:
	str d0, [x14]
	str xzr, [x14, #8]
	b 17f
16:	str q0, [x14]
17:	next_gadget

.macro simd_compare_register_operation name, operation
simd_compare_register_\name:
	cmp w12, #0
	b.eq 1f
	cmp w12, #1
	b.eq 2f
	cmp w12, #2
	b.eq 3f
	\operation v0.2d, v1.2d, v2.2d
	b simd_compare_register_store
1:	\operation v0.16b, v1.16b, v2.16b
	b simd_compare_register_store
2:	\operation v0.8h, v1.8h, v2.8h
	b simd_compare_register_store
3:	\operation v0.4s, v1.4s, v2.4s
	b simd_compare_register_store
.endm

/* Integer vector compare/test forms map one-for-one to host AdvSIMD. */
gadget simd_compare_register
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #22, #2
	ubfx w15, w8, #11, #5
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	ldr q1, [x14]
	add x14, x13, x11, lsl #4
	ldr q2, [x14]
	cmp w15, #17
	b.eq 1f
	cmp w15, #6
	b.eq 2f
	tbnz x8, #29, simd_compare_register_hs
	b simd_compare_register_ge
2:	tbnz x8, #29, simd_compare_register_hi
	b simd_compare_register_gt
1:	tbnz x8, #29, simd_compare_register_eq
	b simd_compare_register_test

simd_compare_register_operation gt, cmgt
simd_compare_register_operation ge, cmge
simd_compare_register_operation hi, cmhi
simd_compare_register_operation hs, cmhs
simd_compare_register_operation eq, cmeq
simd_compare_register_operation test, cmtst

simd_compare_register_store:
	add x14, x13, x9, lsl #4
	tbnz x8, #30, 3f
	str d0, [x14]
	str xzr, [x14, #8]
	b 4f
3:	str q0, [x14]
4:	next_gadget

.macro simd_min_max_operation name, operation
simd_min_max_\name:
	cmp w12, #0
	b.eq 1f
	cmp w12, #1
	b.eq 2f
	\operation v0.4s, v1.4s, v2.4s
	b simd_min_max_store
1:	\operation v0.16b, v1.16b, v2.16b
	b simd_min_max_store
2:	\operation v0.8h, v1.8h, v2.8h
	b simd_min_max_store
.endm

/* Signed/unsigned vector minimum and maximum, including pairwise forms. */
gadget simd_min_max
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #22, #2
	ubfx w15, w8, #11, #5
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	ldr q1, [x14]
	add x14, x13, x11, lsl #4
	ldr q2, [x14]
	tbnz x8, #29, 1f
	tbnz w15, #4, 2f
	tbnz w15, #0, simd_min_max_smin
	b simd_min_max_smax
2:	tbnz w15, #0, simd_min_max_sminp
	b simd_min_max_smaxp
1:	tbnz w15, #4, 3f
	tbnz w15, #0, simd_min_max_umin
	b simd_min_max_umax
3:	tbnz w15, #0, simd_min_max_uminp
	b simd_min_max_umaxp

simd_min_max_operation smax, smax
simd_min_max_operation smin, smin
simd_min_max_operation umax, umax
simd_min_max_operation umin, umin
simd_min_max_operation smaxp, smaxp
simd_min_max_operation sminp, sminp
simd_min_max_operation umaxp, umaxp
simd_min_max_operation uminp, uminp

simd_min_max_store:
	add x14, x13, x9, lsl #4
	tbnz x8, #30, 4f
	str d0, [x14]
	str xzr, [x14, #8]
	b 5f
4:	str q0, [x14]
5:	next_gadget

gadget simd_count_bits
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	ldr q1, [x12]
	cnt v0.16b, v1.16b
	add x12, x11, x9, lsl #4
	tbnz x8, #30, 1f
	str d0, [x12]
	str xzr, [x12, #8]
	b 2f
1:	str q0, [x12]
2:	next_gadget

gadget simd_addv
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #22, #2
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	ldr q1, [x13]
	movi v0.16b, #0
	cmp w11, #0
	b.eq 1f
	cmp w11, #1
	b.eq 3f
	addv s0, v1.4s
	b 5f
1:	tbnz x8, #30, 2f
	addv b0, v1.8b
	b 5f
2:	addv b0, v1.16b
	b 5f
3:	tbnz x8, #30, 4f
	addv h0, v1.4h
	b 5f
4:	addv h0, v1.8h
5:	add x13, x12, x9, lsl #4
	str q0, [x13]
	next_gadget

.macro simd_extract_q offset
gadget simd_extract_q\offset
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #5
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x14, x12, x11, lsl #4
	ldr q1, [x13]
	ldr q2, [x14]
	ext v0.16b, v1.16b, v2.16b, #\offset
	add x13, x12, x9, lsl #4
	str q0, [x13]
	next_gadget
.endm

.macro simd_extract_d offset
gadget simd_extract_d\offset
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #5
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x14, x12, x11, lsl #4
	ldr d1, [x13]
	ldr d2, [x14]
	ext v0.8b, v1.8b, v2.8b, #\offset
	add x13, x12, x9, lsl #4
	str d0, [x13]
	str xzr, [x13, #8]
	next_gadget
.endm

simd_extract_q 0
simd_extract_q 1
simd_extract_q 2
simd_extract_q 3
simd_extract_q 4
simd_extract_q 5
simd_extract_q 6
simd_extract_q 7
simd_extract_q 8
simd_extract_q 9
simd_extract_q 10
simd_extract_q 11
simd_extract_q 12
simd_extract_q 13
simd_extract_q 14
simd_extract_q 15
simd_extract_d 0
simd_extract_d 1
simd_extract_d 2
simd_extract_d 3
simd_extract_d 4
simd_extract_d 5
simd_extract_d 6
simd_extract_d 7

/* REV64 reverses elements independently in each 64-bit block. */
gadget simd_reverse_64
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #22, #2
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	ldr q1, [x13]
	cmp w11, #0
	b.eq 1f
	cmp w11, #1
	b.eq 2f
	rev64 v0.4s, v1.4s
	b 3f
1:	rev64 v0.16b, v1.16b
	b 3f
2:	rev64 v0.8h, v1.8h
3:	add x13, x12, x9, lsl #4
	tbnz x8, #30, 4f
	str d0, [x13]
	str xzr, [x13, #8]
	b 5f
4:	str q0, [x13]
5:	next_gadget

/* REV32 reverses byte or halfword elements within each 32-bit block. */
gadget simd_reverse_32
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #22, #2
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	ldr q1, [x13]
	cbnz w11, 1f
	rev32 v0.16b, v1.16b
	b 2f
1:	rev32 v0.8h, v1.8h
2:	add x13, x12, x9, lsl #4
	tbnz x8, #30, 3f
	str d0, [x13]
	str xzr, [x13, #8]
	b 4f
3:	str q0, [x13]
4:	next_gadget

/* REV16 has only the byte-element form. */
gadget simd_reverse_16
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	ldr q1, [x12]
	rev16 v0.16b, v1.16b
	add x12, x11, x9, lsl #4
	tbnz x8, #30, 1f
	str d0, [x12]
	str xzr, [x12, #8]
	b 2f
1:	str q0, [x12]
2:	next_gadget

/* The six two-source permutes share their size/Q dispatch. */
.macro simd_permute_operation name, operation
simd_permute_\name:
	tbnz x8, #30, simd_permute_\name\()_q
	cmp w12, #0
	b.eq 1f
	cmp w12, #1
	b.eq 2f
	\operation v0.2s, v1.2s, v2.2s
	b simd_permute_store
1:	\operation v0.8b, v1.8b, v2.8b
	b simd_permute_store
2:	\operation v0.4h, v1.4h, v2.4h
	b simd_permute_store
simd_permute_\name\()_q:
	cmp w12, #0
	b.eq 3f
	cmp w12, #1
	b.eq 4f
	cmp w12, #2
	b.eq 5f
	\operation v0.2d, v1.2d, v2.2d
	b simd_permute_store
3:	\operation v0.16b, v1.16b, v2.16b
	b simd_permute_store
4:	\operation v0.8h, v1.8h, v2.8h
	b simd_permute_store
5:	\operation v0.4s, v1.4s, v2.4s
	b simd_permute_store
.endm

gadget simd_permute
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #22, #2
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	ldr q1, [x14]
	add x14, x13, x11, lsl #4
	ldr q2, [x14]
	ubfx w15, w8, #12, #3
	cmp w15, #1
	b.eq simd_permute_uzp1
	cmp w15, #2
	b.eq simd_permute_trn1
	cmp w15, #3
	b.eq simd_permute_zip1
	cmp w15, #5
	b.eq simd_permute_uzp2
	cmp w15, #6
	b.eq simd_permute_trn2
	b simd_permute_zip2

simd_permute_operation uzp1, uzp1
simd_permute_operation trn1, trn1
simd_permute_operation zip1, zip1
simd_permute_operation uzp2, uzp2
simd_permute_operation trn2, trn2
simd_permute_operation zip2, zip2

simd_permute_store:
	add x14, x13, x9, lsl #4
	tbnz x8, #30, 1f
	str d0, [x14]
	str xzr, [x14, #8]
	b 2f
1:	str q0, [x14]
2:	next_gadget

/* SSHR/USHR use the variable host shift instruction with a replicated
 * negative count, avoiding an immediate-specialized gadget matrix. */
gadget simd_shift_right
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #7
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	ldr q1, [x14]
	cmp w11, #64
	b.hs 1f
	cmp w11, #32
	b.hs 2f
	cmp w11, #16
	b.hs 3f
	mov w12, #16
	sub w12, w11, w12
	dup v2.16b, w12
	tbnz x8, #29, 4f
	sshl v0.16b, v1.16b, v2.16b
	b simd_shift_right_store
4:	ushl v0.16b, v1.16b, v2.16b
	b simd_shift_right_store
3:	mov w12, #32
	sub w12, w11, w12
	dup v2.8h, w12
	tbnz x8, #29, 5f
	sshl v0.8h, v1.8h, v2.8h
	b simd_shift_right_store
5:	ushl v0.8h, v1.8h, v2.8h
	b simd_shift_right_store
2:	mov w12, #64
	sub w12, w11, w12
	dup v2.4s, w12
	tbnz x8, #29, 6f
	sshl v0.4s, v1.4s, v2.4s
	b simd_shift_right_store
6:	ushl v0.4s, v1.4s, v2.4s
	b simd_shift_right_store
1:	mov w12, #128
	sub w12, w11, w12
	dup v2.2d, x12
	tbnz x8, #29, 7f
	sshl v0.2d, v1.2d, v2.2d
	b simd_shift_right_store
7:	ushl v0.2d, v1.2d, v2.2d
simd_shift_right_store:
	add x14, x13, x9, lsl #4
	/* Scalar SSHR/USHR use bit 28 to distinguish their D-register form
	 * from the vector encoding. */
	tbnz x8, #28, 10f
	tbnz x8, #30, 8f
	10:
	str d0, [x14]
	str xzr, [x14, #8]
	b 9f
8:	str q0, [x14]
9:	next_gadget

/* SHL uses SSHL with a replicated positive count, covering every immediate
 * without generating one gadget per lane width and shift. */
gadget simd_shift_left
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #7
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	ldr q1, [x14]
	cmp w11, #64
	b.hs 1f
	cmp w11, #32
	b.hs 2f
	cmp w11, #16
	b.hs 3f
	sub w12, w11, #8
	dup v2.16b, w12
	sshl v0.16b, v1.16b, v2.16b
	b 4f
3:	sub w12, w11, #16
	dup v2.8h, w12
	sshl v0.8h, v1.8h, v2.8h
	b 4f
2:	sub w12, w11, #32
	dup v2.4s, w12
	sshl v0.4s, v1.4s, v2.4s
	b 4f
1:	sub w12, w11, #64
	dup v2.2d, x12
	sshl v0.2d, v1.2d, v2.2d
4:	add x14, x13, x9, lsl #4
	tbnz x8, #30, 5f
	str d0, [x14]
	str xzr, [x14, #8]
	b 6f
5:	str q0, [x14]
6:	next_gadget

/* SSHLL/USHLL (including SXTL/UXTL aliases) widen natively. A variable host
 * shift then handles nonzero immediates; Q selects the upper source half. */
gadget simd_shift_left_long
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #7
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	ldr q1, [x14]
	tbz x8, #30, 1f
	ext v1.16b, v1.16b, v1.16b, #8
1:	cmp w11, #32
	b.hs simd_shift_left_long_32
	cmp w11, #16
	b.hs simd_shift_left_long_16
	sub w12, w11, #8
	tbnz x8, #29, 2f
	sshll v0.8h, v1.8b, #0
	b 3f
2:	ushll v0.8h, v1.8b, #0
3:	cbz w12, simd_shift_left_long_store
	dup v2.8h, w12
	sshl v0.8h, v0.8h, v2.8h
	b simd_shift_left_long_store
simd_shift_left_long_16:
	sub w12, w11, #16
	tbnz x8, #29, 4f
	sshll v0.4s, v1.4h, #0
	b 5f
4:	ushll v0.4s, v1.4h, #0
5:	cbz w12, simd_shift_left_long_store
	dup v2.4s, w12
	sshl v0.4s, v0.4s, v2.4s
	b simd_shift_left_long_store
simd_shift_left_long_32:
	sub w12, w11, #32
	tbnz x8, #29, 6f
	sshll v0.2d, v1.2s, #0
	b 7f
6:	ushll v0.2d, v1.2s, #0
7:	cbz w12, simd_shift_left_long_store
	dup v2.2d, x12
	sshl v0.2d, v0.2d, v2.2d
simd_shift_left_long_store:
	add x14, x13, x9, lsl #4
	str q0, [x14]
	next_gadget

.macro simd_shift_register_operation name, operation
simd_shift_register_\name:
	cmp w12, #0
	b.eq 1f
	cmp w12, #1
	b.eq 2f
	cmp w12, #2
	b.eq 3f
	\operation v0.2d, v1.2d, v2.2d
	b simd_shift_register_store
1:	\operation v0.16b, v1.16b, v2.16b
	b simd_shift_register_store
2:	\operation v0.8h, v1.8h, v2.8h
	b simd_shift_register_store
3:	\operation v0.4s, v1.4s, v2.4s
	b simd_shift_register_store
.endm

gadget simd_shift_register
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #22, #2
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	ldr q1, [x14]
	add x14, x13, x11, lsl #4
	ldr q2, [x14]
	tbnz x8, #29, 1f
	tbnz x8, #12, simd_shift_register_srshl
	b simd_shift_register_sshl
1:	tbnz x8, #12, simd_shift_register_urshl
	b simd_shift_register_ushl

simd_shift_register_operation sshl, sshl
simd_shift_register_operation ushl, ushl
simd_shift_register_operation srshl, srshl
simd_shift_register_operation urshl, urshl

simd_shift_register_store:
	add x14, x13, x9, lsl #4
	tbnz x8, #30, 2f
	str d0, [x14]
	str xzr, [x14, #8]
	b 3f
2:	str q0, [x14]
3:	next_gadget

/* TBL/TBX uses host table lookup after copying up to four wrapped guest table
 * registers into a consecutive host list. */
gadget simd_table_lookup
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #13, #2
	add w12, w12, #1
	add x13, _cpu, #UML_GADGET_CPU_V0
	and w14, w10, #31
	add x15, x13, x14, lsl #4
	ldr q0, [x15]
	cmp w12, #1
	b.eq 1f
	add w14, w10, #1
	and w14, w14, #31
	add x15, x13, x14, lsl #4
	ldr q1, [x15]
	cmp w12, #2
	b.eq 1f
	add w14, w10, #2
	and w14, w14, #31
	add x15, x13, x14, lsl #4
	ldr q2, [x15]
	cmp w12, #3
	b.eq 1f
	add w14, w10, #3
	and w14, w14, #31
	add x15, x13, x14, lsl #4
	ldr q3, [x15]
1:	add x15, x13, x11, lsl #4
	ldr q4, [x15]
	tbnz x8, #12, simd_table_lookup_extend
	cmp w12, #1
	b.eq 2f
	cmp w12, #2
	b.eq 3f
	cmp w12, #3
	b.eq 4f
	tbl v5.16b, {v0.16b-v3.16b}, v4.16b
	b simd_table_lookup_store
2:	tbl v5.16b, {v0.16b}, v4.16b
	b simd_table_lookup_store
3:	tbl v5.16b, {v0.16b-v1.16b}, v4.16b
	b simd_table_lookup_store
4:	tbl v5.16b, {v0.16b-v2.16b}, v4.16b
	b simd_table_lookup_store
simd_table_lookup_extend:
	add x15, x13, x9, lsl #4
	ldr q5, [x15]
	cmp w12, #1
	b.eq 5f
	cmp w12, #2
	b.eq 6f
	cmp w12, #3
	b.eq 7f
	tbx v5.16b, {v0.16b-v3.16b}, v4.16b
	b simd_table_lookup_store
5:	tbx v5.16b, {v0.16b}, v4.16b
	b simd_table_lookup_store
6:	tbx v5.16b, {v0.16b-v1.16b}, v4.16b
	b simd_table_lookup_store
7:	tbx v5.16b, {v0.16b-v2.16b}, v4.16b
simd_table_lookup_store:
	add x15, x13, x9, lsl #4
	tbnz x8, #30, 8f
	str d5, [x15]
	str xzr, [x15, #8]
	b 9f
8:	str q5, [x15]
9:	next_gadget

/* DUP (element) uses a scalar host load and native DUP. This keeps the lane
 * selection dynamic without returning to the instruction interpreter. */
gadget simd_duplicate_element
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	rbit w12, w11
	clz w12, w12
	add w13, w12, #1
	lsr w13, w11, w13
	lsl w13, w13, w12
	add x14, _cpu, #UML_GADGET_CPU_V0
	add x15, x14, x10, lsl #4
	add x15, x15, x13
	cmp w12, #0
	b.eq 1f
	cmp w12, #1
	b.eq 2f
	cmp w12, #2
	b.eq 3f
	ldr x16, [x15]
	dup v0.2d, x16
	b 4f
1:	ldrb w16, [x15]
	dup v0.16b, w16
	b 4f
2:	ldrh w16, [x15]
	dup v0.8h, w16
	b 4f
3:	ldr w16, [x15]
	dup v0.4s, w16
4:	add x15, x14, x9, lsl #4
	tbnz x8, #28, simd_duplicate_element_scalar
	tbnz x8, #30, 5f
	str d0, [x15]
	str xzr, [x15, #8]
	b 6f
5:	str q0, [x15]
6:	next_gadget
simd_duplicate_element_scalar:
	movi v1.16b, #0
	str q1, [x15]
	cmp w12, #0
	b.eq 7f
	cmp w12, #1
	b.eq 8f
	cmp w12, #2
	b.eq 9f
	str d0, [x15]
	b 10f
7:	str b0, [x15]
	b 10f
8:	str h0, [x15]
	b 10f
9:	str s0, [x15]
10:	next_gadget

/* SHRN/RSHRN and SHRN2/RSHRN2. Variable host vector shifts cover every
 * immediate encoding, followed by native narrowing into the selected half. */
gadget simd_shift_right_narrow
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #7
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	ldr q1, [x14]
	tbnz x8, #30, 1f
	movi v0.16b, #0
	b 2f
1:	add x14, x13, x9, lsl #4
	ldr q0, [x14]
2:	cmp w11, #32
	b.hs simd_shift_narrow_64
	cmp w11, #16
	b.hs simd_shift_narrow_32
	mov w12, #16
	sub w12, w11, w12
	dup v2.8h, w12
	tbnz x8, #11, 3f
	ushl v1.8h, v1.8h, v2.8h
	b 4f
3:	urshl v1.8h, v1.8h, v2.8h
4:	tbnz x8, #30, 5f
	xtn v0.8b, v1.8h
	b simd_shift_narrow_store
5:	xtn2 v0.16b, v1.8h
	b simd_shift_narrow_store
simd_shift_narrow_32:
	mov w12, #32
	sub w12, w11, w12
	dup v2.4s, w12
	tbnz x8, #11, 6f
	ushl v1.4s, v1.4s, v2.4s
	b 7f
6:	urshl v1.4s, v1.4s, v2.4s
7:	tbnz x8, #30, 8f
	xtn v0.4h, v1.4s
	b simd_shift_narrow_store
8:	xtn2 v0.8h, v1.4s
	b simd_shift_narrow_store
simd_shift_narrow_64:
	mov w12, #64
	sub w12, w11, w12
	dup v2.2d, x12
	tbnz x8, #11, 9f
	ushl v1.2d, v1.2d, v2.2d
	b 10f
9:	urshl v1.2d, v1.2d, v2.2d
10:	tbnz x8, #30, 11f
	xtn v0.2s, v1.2d
	b simd_shift_narrow_store
11:	xtn2 v0.4s, v1.2d
simd_shift_narrow_store:
	add x14, x13, x9, lsl #4
	str q0, [x14]
	next_gadget

/* XTN/XTN2 are pure truncating narrows and map directly to the host. */
gadget simd_narrow_move
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #22, #2
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	ldr q1, [x13]
	tbnz x8, #30, simd_narrow_move_high
	movi v0.16b, #0
	cmp w11, #0
	b.eq 1f
	cmp w11, #1
	b.eq 2f
	xtn v0.2s, v1.2d
	b simd_narrow_move_store
1:	xtn v0.8b, v1.8h
	b simd_narrow_move_store
2:	xtn v0.4h, v1.4s
	b simd_narrow_move_store
simd_narrow_move_high:
	add x13, x12, x9, lsl #4
	ldr q0, [x13]
	cmp w11, #0
	b.eq 3f
	cmp w11, #1
	b.eq 4f
	xtn2 v0.4s, v1.2d
	b simd_narrow_move_store
3:	xtn2 v0.16b, v1.8h
	b simd_narrow_move_store
4:	xtn2 v0.8h, v1.4s
simd_narrow_move_store:
	add x13, x12, x9, lsl #4
	str q0, [x13]
	next_gadget

.macro simd_arithmetic_narrow_operation name, operation
simd_arithmetic_narrow_\name:
	cmp w12, #0
	b.eq 1f
	cmp w12, #1
	b.eq 2f
	tbnz x8, #30, 3f
	\operation v0.2s, v1.2d, v2.2d
	b simd_arithmetic_narrow_store
1:	tbnz x8, #30, 4f
	\operation v0.8b, v1.8h, v2.8h
	b simd_arithmetic_narrow_store
2:	tbnz x8, #30, 5f
	\operation v0.4h, v1.4s, v2.4s
	b simd_arithmetic_narrow_store
3:	\operation\()2 v0.4s, v1.2d, v2.2d
	b simd_arithmetic_narrow_store
4:	\operation\()2 v0.16b, v1.8h, v2.8h
	b simd_arithmetic_narrow_store
5:	\operation\()2 v0.8h, v1.4s, v2.4s
	b simd_arithmetic_narrow_store
.endm

/* ADDHN/RADDHN/SUBHN/RSUBHN map directly to the host AdvSIMD operation. */
gadget simd_arithmetic_narrow
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #22, #2
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	ldr q1, [x14]
	add x14, x13, x11, lsl #4
	ldr q2, [x14]
	tbz x8, #30, 1f
	add x14, x13, x9, lsl #4
	ldr q0, [x14]
	b 2f
1:	movi v0.16b, #0
2:	tbnz x8, #29, 3f
	tbnz x8, #13, simd_arithmetic_narrow_sub
	b simd_arithmetic_narrow_add
3:	tbnz x8, #13, simd_arithmetic_narrow_rsub
	b simd_arithmetic_narrow_radd

simd_arithmetic_narrow_operation add, addhn
simd_arithmetic_narrow_operation sub, subhn
simd_arithmetic_narrow_operation radd, raddhn
simd_arithmetic_narrow_operation rsub, rsubhn

simd_arithmetic_narrow_store:
	add x14, x13, x9, lsl #4
	str q0, [x14]
	next_gadget

.macro simd_add_sub_operation name, operation
simd_add_sub_\name:
	cmp w12, #0
	b.eq 1f
	cmp w12, #1
	b.eq 2f
	cmp w12, #2
	b.eq 3f
	\operation v0.2d, v1.2d, v2.2d
	b simd_add_sub_store
1:	\operation v0.16b, v1.16b, v2.16b
	b simd_add_sub_store
2:	\operation v0.8h, v1.8h, v2.8h
	b simd_add_sub_store
3:	\operation v0.4s, v1.4s, v2.4s
	b simd_add_sub_store
.endm

.macro simd_addp_operation size, arrangement
simd_addp_\size:
	addp v0.\arrangement, v1.\arrangement, v2.\arrangement
	b simd_add_sub_store
.endm

/* Integer vector ADD/SUB and pairwise ADD map directly to host AdvSIMD. */
gadget simd_add_sub_pairwise
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #22, #2
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	ldr q1, [x14]
	add x14, x13, x11, lsl #4
	ldr q2, [x14]
	ubfx w15, w8, #11, #5
	cmp w15, #23
	b.eq simd_addp_dispatch
	tbnz x8, #29, simd_add_sub_sub
	b simd_add_sub_add

simd_add_sub_operation add, add
simd_add_sub_operation sub, sub

simd_addp_dispatch:
	cmp w12, #0
	b.eq simd_addp_8
	cmp w12, #1
	b.eq simd_addp_16
	cmp w12, #2
	b.eq simd_addp_32
	b simd_addp_64
simd_addp_operation 8, 16b
simd_addp_operation 16, 8h
simd_addp_operation 32, 4s
simd_addp_operation 64, 2d

simd_add_sub_store:
	add x14, x13, x9, lsl #4
	tbnz x8, #28, 1f
	tbnz x8, #30, 2f
1:	str d0, [x14]
	str xzr, [x14, #8]
	b 3f
2:	str q0, [x14]
3:	next_gadget

/* Integer vector MUL uses the host lane-wise multiplier. */
gadget simd_multiply
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #22, #2
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	ldr q1, [x14]
	add x14, x13, x11, lsl #4
	ldr q2, [x14]
	cmp w12, #0
	b.eq 1f
	cmp w12, #1
	b.eq 2f
	mul v0.4s, v1.4s, v2.4s
	b 3f
1:	mul v0.16b, v1.16b, v2.16b
	b 3f
2:	mul v0.8h, v1.8h, v2.8h
3:	add x14, x13, x9, lsl #4
	tbnz x8, #30, 4f
	str d0, [x14]
	str xzr, [x14, #8]
	b 5f
4:	str q0, [x14]
5:	next_gadget

/* Builder-predecoded MOVI/MVNI/ORR/BIC immediate. Operand:
 * destination[4:0], Q[8], action[17:16], followed by the repeated pattern. */
gadget simd_modified_immediate
	ldr x8, [_stream], #8
	ldr x9, [_stream], #8
	and w10, w8, #0x1f
	ubfx w11, w8, #16, #2
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x12, x12, x10, lsl #4
	cbz w11, simd_immediate_store
	ldr x13, [x12]
	cmp w11, #1
	b.eq simd_immediate_or_low
	bic x13, x13, x9
	b simd_immediate_low_ready
simd_immediate_or_low:
	orr x13, x13, x9
simd_immediate_low_ready:
	str x13, [x12]
	tbz x8, #8, simd_immediate_zero_high
	ldr x13, [x12, #8]
	cmp w11, #1
	b.eq simd_immediate_or_high
	bic x13, x13, x9
	b simd_immediate_high_ready
simd_immediate_or_high:
	orr x13, x13, x9
simd_immediate_high_ready:
	str x13, [x12, #8]
	b simd_immediate_done
simd_immediate_store:
	str x9, [x12]
	tbnz x8, #8, simd_immediate_store_high
simd_immediate_zero_high:
	str xzr, [x12, #8]
	b simd_immediate_done
simd_immediate_store_high:
	str x9, [x12, #8]
simd_immediate_done:
	next_gadget

/* Exact FMOV forms between the integer file and scalar/vector bit patterns.
 * Operand: destination[4:0], source[12:8]. */
.p2align 4
gadget simd_vector_element_to_gpr
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	rbit w12, w11
	clz w12, w12
	add w13, w12, #1
	lsr w13, w11, w13
	lsl w13, w13, w12
	add x14, _cpu, #UML_GADGET_CPU_V0
	add x14, x14, x10, lsl #4
	add x14, x14, x13
	cmp w12, #0
	b.eq 1f
	cmp w12, #1
	b.eq 2f
	cmp w12, #2
	b.eq 3f
	ldr x15, [x14]
	b 4f
1:	ldrb w15, [x14]
	tbnz x8, #12, 4f
	sxtb x15, w15
	b 4f
2:	ldrh w15, [x14]
	tbnz x8, #12, 4f
	sxth x15, w15
	b 4f
3:	ldr w15, [x14]
	tbnz x8, #12, 4f
	sxtw x15, w15
4:	tbnz x8, #30, 5f
	mov w15, w15
5:	cmp w9, #31
	b.eq 6f
	add x14, _cpu, #UML_GADGET_CPU_X0
	str x15, [x14, x9, lsl #3]
6:	next_gadget

gadget simd_gpr_to_vector_element
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	rbit w12, w11
	clz w12, w12
	add w13, w12, #1
	lsr w13, w11, w13
	lsl w13, w13, w12
	add x14, _cpu, #UML_GADGET_CPU_X0
	cmp w10, #31
	b.eq 1f
	ldr x15, [x14, x10, lsl #3]
	b 2f
1:	mov x15, xzr
2:	add x14, _cpu, #UML_GADGET_CPU_V0
	add x14, x14, x9, lsl #4
	add x14, x14, x13
	cmp w12, #0
	b.eq 3f
	cmp w12, #1
	b.eq 4f
	cmp w12, #2
	b.eq 5f
	str x15, [x14]
	b 6f
3:	strb w15, [x14]
	b 6f
4:	strh w15, [x14]
	b 6f
5:	str w15, [x14]
6:	next_gadget

/* INS (element) copies one lane while preserving every other destination
 * byte. Load before store so aliased source/destination vectors are safe. */
gadget simd_insert_element
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	rbit w12, w11
	clz w12, w12
	add w13, w12, #1
	lsr w13, w11, w13
	lsl w13, w13, w12
	ubfx w14, w8, #11, #4
	add x15, _cpu, #UML_GADGET_CPU_V0
	add x16, x15, x10, lsl #4
	add x16, x16, x14
	add x15, x15, x9, lsl #4
	add x15, x15, x13
	cmp w12, #0
	b.eq 1f
	cmp w12, #1
	b.eq 2f
	cmp w12, #2
	b.eq 3f
	ldr x17, [x16]
	str x17, [x15]
	b 4f
1:	ldrb w17, [x16]
	strb w17, [x15]
	b 4f
2:	ldrh w17, [x16]
	strh w17, [x15]
	b 4f
3:	ldr w17, [x16]
	str w17, [x15]
4:	next_gadget

.macro vector_to_gpr name, offset, bytes
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	cmp w9, #31
	b.eq 1f
	ubfx w10, w8, #8, #5
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x11, x11, x10, lsl #4
	.if \bytes == 4
	ldr w12, [x11, #\offset]
	.else
	ldr x12, [x11, #\offset]
	.endif
	add x13, _cpu, #UML_GADGET_CPU_X0
	str x12, [x13, x9, lsl #3]
1:	next_gadget
.endm

.macro gpr_to_vector name, offset, bytes
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	cmp w10, #31
	b.eq 1f
	add x11, _cpu, #UML_GADGET_CPU_X0
	ldr x12, [x11, x10, lsl #3]
	b 2f
1:	mov x12, xzr
2:	add x13, _cpu, #UML_GADGET_CPU_V0
	add x13, x13, x9, lsl #4
	.if \bytes == 4
	str w12, [x13, #\offset]
	str wzr, [x13, #4]
	str xzr, [x13, #8]
	.else
	str x12, [x13, #\offset]
	.if \offset == 0
	str xzr, [x13, #8]
	.endif
	.endif
	next_gadget
.endm

vector_to_gpr vector_to_gpr_64_low, 0, 8
gpr_to_vector gpr_to_vector_64_low, 0, 8
vector_to_gpr vector_to_gpr_32_low, 0, 4
gpr_to_vector gpr_to_vector_32_low, 0, 4
vector_to_gpr vector_to_gpr_64_high, 8, 8
gpr_to_vector gpr_to_vector_64_high, 8, 8

/* FCSEL's condition is fixed by the builder. Operand: destination[4:0],
 * true-source[12:8], false-source[20:16], double[24]. */
.macro floating_select name, condition
gadget floating_select_\name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #5
	ldr w12, [_cpu, #UML_GADGET_CPU_NZCV]
	msr nzcv, x12
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x14, x12, x11, lsl #4
	csel x13, x13, x14, \condition
	add x14, x12, x9, lsl #4
	tbz x8, #24, 1f
	ldr x15, [x13]
	str x15, [x14]
	b 2f
1:	ldr w15, [x13]
	str w15, [x14]
	str wzr, [x14, #4]
2:	str xzr, [x14, #8]
	next_gadget
.endm

floating_select eq, eq
floating_select ne, ne
floating_select hs, hs
floating_select lo, lo
floating_select mi, mi
floating_select pl, pl
floating_select vs, vs
floating_select vc, vc
floating_select hi, hi
floating_select ls, ls
floating_select ge, ge
floating_select lt, lt
floating_select gt, gt
floating_select le, le

gadget floating_select_always
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x14, x12, x9, lsl #4
	tbz x8, #24, 1f
	ldr x15, [x13]
	str x15, [x14]
	b 2f
1:	ldr w15, [x13]
	str w15, [x14]
	str wzr, [x14, #4]
2:	str xzr, [x14, #8]
	next_gadget

/* Scalar FMOV/FABS/FNEG/FSQRT and FMUL/FDIV/FADD/FSUB/FNMUL under the guest's
 * native A64 FPCR. Stream: [instruction][guest PC].
 */
gadget floating_arithmetic
	ldr x8, [_stream]
	ensure_fp_environment
	add _stream, _stream, #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	add x12, _cpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x14, x12, x9, lsl #4
	movi v2.16b, #0
	tbnz x8, #22, floating_arithmetic_load_double
	ldr s0, [x13]
	tbnz x8, #14, floating_arithmetic_unary_single
	add x13, x12, x11, lsl #4
	ldr s1, [x13]
	ubfx w15, w8, #12, #4
	cmp w15, #0
	b.eq floating_multiply_single
	cmp w15, #1
	b.eq floating_divide_single
	cmp w15, #2
	b.eq floating_add_single
	cmp w15, #3
	b.eq floating_subtract_single
	fnmul s2, s0, s1
	b floating_arithmetic_store
floating_multiply_single:
	fmul s2, s0, s1
	b floating_arithmetic_store
floating_divide_single:
	fdiv s2, s0, s1
	b floating_arithmetic_store
floating_add_single:
	fadd s2, s0, s1
	b floating_arithmetic_store
floating_subtract_single:
	fsub s2, s0, s1
	b floating_arithmetic_store
floating_arithmetic_unary_single:
	ubfx w15, w8, #15, #6
	cmp w15, #0
	b.eq floating_move_single
	cmp w15, #1
	b.eq floating_absolute_single
	cmp w15, #2
	b.eq floating_negate_single
	fsqrt s2, s0
	b floating_arithmetic_store
floating_move_single:
	fmov s2, s0
	b floating_arithmetic_store
floating_absolute_single:
	fabs s2, s0
	b floating_arithmetic_store
floating_negate_single:
	fneg s2, s0
	b floating_arithmetic_store

floating_arithmetic_load_double:
	ldr d0, [x13]
	tbnz x8, #14, floating_arithmetic_unary_double
	add x13, x12, x11, lsl #4
	ldr d1, [x13]
	ubfx w15, w8, #12, #4
	cmp w15, #0
	b.eq floating_multiply_double
	cmp w15, #1
	b.eq floating_divide_double
	cmp w15, #2
	b.eq floating_add_double
	cmp w15, #3
	b.eq floating_subtract_double
	fnmul d2, d0, d1
	b floating_arithmetic_store
floating_multiply_double:
	fmul d2, d0, d1
	b floating_arithmetic_store
floating_divide_double:
	fdiv d2, d0, d1
	b floating_arithmetic_store
floating_add_double:
	fadd d2, d0, d1
	b floating_arithmetic_store
floating_subtract_double:
	fsub d2, d0, d1
	b floating_arithmetic_store
floating_arithmetic_unary_double:
	ubfx w15, w8, #15, #6
	cmp w15, #0
	b.eq floating_move_double
	cmp w15, #1
	b.eq floating_absolute_double
	cmp w15, #2
	b.eq floating_negate_double
	fsqrt d2, d0
	b floating_arithmetic_store
floating_move_double:
	fmov d2, d0
	b floating_arithmetic_store
floating_absolute_double:
	fabs d2, d0
	b floating_arithmetic_store
floating_negate_double:
	fneg d2, d0
floating_arithmetic_store:
	str q2, [x14]
	add _stream, _stream, #8
	next_gadget

/* Scalar FMADD/FMSUB/FNMADD/FNMSUB. Stream: [instruction][guest PC]. */
gadget floating_multiply_add
	ldr x8, [_stream]
	ensure_fp_environment
	add _stream, _stream, #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #10, #5
	ubfx w12, w8, #16, #5
	add x13, _cpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	add x15, x13, x12, lsl #4
	add x16, x13, x11, lsl #4
	add x17, x13, x9, lsl #4
	movi v3.16b, #0
	tbnz x8, #22, floating_multiply_add_double
	ldr s0, [x14]
	ldr s1, [x15]
	ldr s2, [x16]
	tbnz x8, #21, floating_multiply_add_negative_single
	tbnz x8, #15, floating_multiply_subtract_single
	fmadd s3, s0, s1, s2
	b floating_multiply_add_store
floating_multiply_subtract_single:
	fmsub s3, s0, s1, s2
	b floating_multiply_add_store
floating_multiply_add_negative_single:
	tbnz x8, #15, floating_multiply_subtract_negative_single
	fnmadd s3, s0, s1, s2
	b floating_multiply_add_store
floating_multiply_subtract_negative_single:
	fnmsub s3, s0, s1, s2
	b floating_multiply_add_store
floating_multiply_add_double:
	ldr d0, [x14]
	ldr d1, [x15]
	ldr d2, [x16]
	tbnz x8, #21, floating_multiply_add_negative_double
	tbnz x8, #15, floating_multiply_subtract_double
	fmadd d3, d0, d1, d2
	b floating_multiply_add_store
floating_multiply_subtract_double:
	fmsub d3, d0, d1, d2
	b floating_multiply_add_store
floating_multiply_add_negative_double:
	tbnz x8, #15, floating_multiply_subtract_negative_double
	fnmadd d3, d0, d1, d2
	b floating_multiply_add_store
floating_multiply_subtract_negative_double:
	fnmsub d3, d0, d1, d2
floating_multiply_add_store:
	str q3, [x17]
	add _stream, _stream, #8
	next_gadget
