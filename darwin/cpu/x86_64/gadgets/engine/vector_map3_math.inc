/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SSE4.1/AVX dot products and multiple packed byte sums of differences. */

.macro vector_map3_dpps_lane output, left, right
	fmul v6.4s, v\left\().4s, v\right\().4s
	vector_map3_mask_s 7, 4
	and v6.16b, v6.16b, v7.16b
	faddp v6.4s, v6.4s, v6.4s
	dup v7.4s, v6.s[1]
	fadd s6, s6, s7
	fmov w14, s6
	dup v\output\().4s, w14
	vector_map3_mask_s 7, 0
	and v\output\().16b, v\output\().16b, v7.16b
.endm

.macro vector_map3_dppd_lane output, left, right
	fmul v6.2d, v\left\().2d, v\right\().2d
	vector_map3_mask_d 7, 4
	and v6.16b, v6.16b, v7.16b
	dup v7.2d, v6.d[1]
	fadd d6, d6, d7
	fmov x14, d6
	dup v\output\().2d, x14
	vector_map3_mask_d 7, 0
	and v\output\().16b, v\output\().16b, v7.16b
.endm

.macro vector_map3_mpsadbw_lane output, left, right, shift
	lsr w14, w13, #\shift
	and w14, w14, #7
	and w15, w14, #3
	lsl w15, w15, #2
	ubfx w16, w14, #2, #1
	lsl w16, w16, #2
	vector_map3_index_sequence 6
	dup v7.16b, w15
	add v7.16b, v6.16b, v7.16b
	tbl v9.16b, {v\right\().16b}, v7.16b
	dup v7.16b, w16
	add v7.16b, v6.16b, v7.16b
	tbl v8.16b, {v\left\().16b}, v7.16b
	dup v10.8b, v9.b[0]
	uabdl v\output\().8h, v8.8b, v10.8b
	ext v11.16b, v8.16b, v8.16b, #1
	dup v10.8b, v9.b[1]
	uabal v\output\().8h, v11.8b, v10.8b
	ext v11.16b, v8.16b, v8.16b, #2
	dup v10.8b, v9.b[2]
	uabal v\output\().8h, v11.8b, v10.8b
	ext v11.16b, v8.16b, v8.16b, #3
	dup v10.8b, v9.b[3]
	uabal v\output\().8h, v11.8b, v10.8b
.endm

/* A dot product can overflow and cancel on the way to its result, so while
 * the gadgets check SIMD exceptions one always runs checked. */
.macro vector_map3_math_body opcode, width
	.if \opcode == 0x40
	simd_leave_if_checking
	vector_map3_dpps_lane 4, 0, 1
	.if \width == 256
	vector_map3_dpps_lane 5, 2, 3
	.endif
	.elseif \opcode == 0x41
	simd_leave_if_checking
	vector_map3_dppd_lane 4, 0, 1
	.if \width == 256
	vector_map3_dppd_lane 5, 2, 3
	.endif
	.else
	vector_map3_mpsadbw_lane 4, 0, 1, 0
	.if \width == 256
	vector_map3_mpsadbw_lane 5, 2, 3, 3
	.endif
	.endif
.endm

.macro vector_map3_math_register symbol, opcode, width
gadget vector_map3_math_\symbol\()_\width\()_rr
	ldp x8, x13, [_stream], #16
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x10, _cpu, x10
	add x11, _cpu, x11
	ldr q0, [x10]
	ldr q1, [x11]
	.if \width == 256
	ldr q2, [x10, #16]
	ldr q3, [x11, #16]
	.endif
	vector_map3_math_body \opcode, \width
	vector_write_xmm \width, 8
	retire_and_next
.endm

.macro vector_map3_math_memory symbol, opcode, width
gadget vector_map3_math_\symbol\()_\width\()_rm
	effective_address
	ldp x26, x27, [_stream], #16
	mov x25, x7
	page_cross_check (\width / 8), Lx86_vector_map3_math_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	ldr q1, [x7]
	.if \width == 256
	ldr q3, [x7, #16]
	.endif
	b Lx86_vector_map3_math_loaded\@
Lx86_vector_map3_math_cross\@:
	sub sp, sp, #32
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_vector_load)
	cbz w0, Lx86_vector_map3_math_cross_fault\@
	ldr q1, [sp]
	.if \width == 256
	ldr q3, [sp, #16]
	.endif
	add sp, sp, #32
Lx86_vector_map3_math_loaded\@:
	mov w13, w27
	and x10, x26, #0xffff
	add x10, _cpu, x10
	ldr q0, [x10]
	.if \width == 256
	ldr q2, [x10, #16]
	.endif
	vector_map3_math_body \opcode, \width
	vector_write_xmm \width, 23
	retire_and_next
Lx86_vector_map3_math_cross_fault\@:
	add sp, sp, #32
	b Lx86_memory_fault
.endm

.macro vector_map3_math_family symbol, opcode
	vector_map3_math_register \symbol, \opcode, 128
	vector_map3_math_memory \symbol, \opcode, 128
	vector_map3_math_register \symbol, \opcode, 256
	vector_map3_math_memory \symbol, \opcode, 256
.endm

vector_map3_math_family 40, 0x40
vector_map3_math_family 41, 0x41
vector_map3_math_family 42, 0x42

.purgem vector_map3_math_family
.purgem vector_map3_math_memory
.purgem vector_map3_math_register
.purgem vector_map3_math_body
.purgem vector_map3_mpsadbw_lane
.purgem vector_map3_dppd_lane
.purgem vector_map3_dpps_lane
