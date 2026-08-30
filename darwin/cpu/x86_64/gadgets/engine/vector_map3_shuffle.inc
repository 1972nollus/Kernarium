/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Immediate shuffle, blend, align, and 128-bit lane-control operations. */

.macro vector_map3_index_sequence output
	mov x14, #0x0100
	movk x14, #0x0302, lsl #16
	movk x14, #0x0504, lsl #32
	movk x14, #0x0706, lsl #48
	fmov d\output, x14
	mov x14, #0x0908
	movk x14, #0x0b0a, lsl #16
	movk x14, #0x0d0c, lsl #32
	movk x14, #0x0f0e, lsl #48
	mov v\output\().d[1], x14
.endm

.macro vector_map3_qword_indices output, first
	mov x16, #0x0101
	movk x16, #0x0101, lsl #16
	movk x16, #0x0101, lsl #32
	movk x16, #0x0101, lsl #48
	mov x17, #0x0100
	movk x17, #0x0302, lsl #16
	movk x17, #0x0504, lsl #32
	movk x17, #0x0706, lsl #48
	.irp lane, 0, 1
	lsr w14, w13, #((\first + \lane) * 2)
	and w14, w14, #3
	lsl w14, w14, #3
	mul x14, x14, x16
	add x14, x14, x17
	mov v\output\().d[\lane], x14
	.endr
.endm

.macro vector_map3_shuffle_dword_indices
	mov w16, #0x0101
	movk w16, #0x0101, lsl #16
	mov w17, #0x0100
	movk w17, #0x0302, lsl #16
	.irp lane, 0, 1, 2, 3
	lsr w14, w13, #(\lane * 2)
	and w14, w14, #3
	lsl w14, w14, #2
	mul w14, w14, w16
	add w14, w14, w17
	mov v6.s[\lane], w14
	.endr
.endm

.macro vector_map3_mask_s output, first
	movi v\output\().16b, #0
	.irp lane, 0, 1, 2, 3
	ubfx w14, w13, #(\first + \lane), #1
	neg w14, w14
	mov v\output\().s[\lane], w14
	.endr
.endm

.macro vector_map3_mask_d output, first
	movi v\output\().16b, #0
	.irp lane, 0, 1
	ubfx x14, x13, #(\first + \lane), #1
	neg x14, x14
	mov v\output\().d[\lane], x14
	.endr
.endm

.macro vector_map3_mask_h output
	movi v\output\().16b, #0
	.irp lane, 0, 1, 2, 3, 4, 5, 6, 7
	ubfx w14, w13, #\lane, #1
	neg w14, w14
	mov v\output\().h[\lane], w14
	.endr
.endm

.macro vector_map3_select_128 output, shift
	ubfx w14, w13, #\shift, #4
	tbnz w14, #3, .Lx86_vector_map3_select_zero\@
	and w14, w14, #3
	cmp w14, #1
	b.eq .Lx86_vector_map3_select_left_high\@
	cmp w14, #2
	b.eq .Lx86_vector_map3_select_right_low\@
	cmp w14, #3
	b.eq .Lx86_vector_map3_select_right_high\@
	mov v\output\().16b, v0.16b
	b .Lx86_vector_map3_select_done\@
.Lx86_vector_map3_select_left_high\@:
	mov v\output\().16b, v2.16b
	b .Lx86_vector_map3_select_done\@
.Lx86_vector_map3_select_right_low\@:
	mov v\output\().16b, v1.16b
	b .Lx86_vector_map3_select_done\@
.Lx86_vector_map3_select_right_high\@:
	mov v\output\().16b, v3.16b
	b .Lx86_vector_map3_select_done\@
.Lx86_vector_map3_select_zero\@:
	movi v\output\().16b, #0
.Lx86_vector_map3_select_done\@:
.endm

.macro vector_map3_variable_blend element, output, left, right, mask
	.if \element == 8
	cmlt v\mask\().16b, v\mask\().16b, #0
	.elseif \element == 32
	cmlt v\mask\().4s, v\mask\().4s, #0
	.else
	cmlt v\mask\().2d, v\mask\().2d, #0
	.endif
	bsl v\mask\().16b, v\right\().16b, v\left\().16b
	mov v\output\().16b, v\mask\().16b
.endm

.macro vector_map3_shuffle_lane opcode, width
	.if \opcode == 0x00 || \opcode == 0x01
	vector_map3_qword_indices 6, 0
	mov v8.16b, v1.16b
	mov v9.16b, v3.16b
	tbl v4.16b, {v8.16b, v9.16b}, v6.16b
	.if \width == 256
	vector_map3_qword_indices 7, 2
	tbl v5.16b, {v8.16b, v9.16b}, v7.16b
	.endif
	.elseif \opcode == 0x02 || \opcode == 0x0c
	vector_map3_mask_s 6, 0
	bsl v6.16b, v1.16b, v0.16b
	mov v4.16b, v6.16b
	.if \width == 256
	vector_map3_mask_s 7, 4
	bsl v7.16b, v3.16b, v2.16b
	mov v5.16b, v7.16b
	.endif
	.elseif \opcode == 0x04
	vector_map3_shuffle_dword_indices
	tbl v4.16b, {v1.16b}, v6.16b
	.if \width == 256
	tbl v5.16b, {v3.16b}, v6.16b
	.endif
	.elseif \opcode == 0x05
	tbz w13, #0, .Lx86_vector_map3_pd_0_low\@
	mov v4.d[0], v1.d[1]
	b .Lx86_vector_map3_pd_0_done\@
.Lx86_vector_map3_pd_0_low\@:
	mov v4.d[0], v1.d[0]
.Lx86_vector_map3_pd_0_done\@:
	tbz w13, #1, .Lx86_vector_map3_pd_1_low\@
	mov v4.d[1], v1.d[1]
	b .Lx86_vector_map3_pd_1_done\@
.Lx86_vector_map3_pd_1_low\@:
	mov v4.d[1], v1.d[0]
.Lx86_vector_map3_pd_1_done\@:
	.if \width == 256
	tbz w13, #2, .Lx86_vector_map3_pd_2_low\@
	mov v5.d[0], v3.d[1]
	b .Lx86_vector_map3_pd_2_done\@
.Lx86_vector_map3_pd_2_low\@:
	mov v5.d[0], v3.d[0]
.Lx86_vector_map3_pd_2_done\@:
	tbz w13, #3, .Lx86_vector_map3_pd_3_low\@
	mov v5.d[1], v3.d[1]
	b .Lx86_vector_map3_pd_3_done\@
.Lx86_vector_map3_pd_3_low\@:
	mov v5.d[1], v3.d[0]
.Lx86_vector_map3_pd_3_done\@:
	.endif
	.elseif \opcode == 0x06 || \opcode == 0x46
	vector_map3_select_128 4, 0
	vector_map3_select_128 5, 4
	.elseif \opcode == 0x0d
	vector_map3_mask_d 6, 0
	bsl v6.16b, v1.16b, v0.16b
	mov v4.16b, v6.16b
	.if \width == 256
	vector_map3_mask_d 7, 2
	bsl v7.16b, v3.16b, v2.16b
	mov v5.16b, v7.16b
	.endif
	.elseif \opcode == 0x0e
	vector_map3_mask_h 6
	bsl v6.16b, v1.16b, v0.16b
	mov v4.16b, v6.16b
	.if \width == 256
	vector_map3_mask_h 7
	bsl v7.16b, v3.16b, v2.16b
	mov v5.16b, v7.16b
	.endif
	.elseif \opcode == 0x0f
	vector_map3_index_sequence 6
	dup v7.16b, w13
	add v6.16b, v6.16b, v7.16b
	.if \width == 64
	mov v7.d[0], v1.d[0]
	mov v7.d[1], v0.d[0]
	tbl v4.16b, {v7.16b}, v6.16b
	.else
	mov v8.16b, v1.16b
	mov v9.16b, v0.16b
	tbl v4.16b, {v8.16b, v9.16b}, v6.16b
	.if \width == 256
	mov v8.16b, v3.16b
	mov v9.16b, v2.16b
	tbl v5.16b, {v8.16b, v9.16b}, v6.16b
	.endif
	.endif
	.elseif \opcode == 0x18 || \opcode == 0x38
	mov v4.16b, v0.16b
	mov v5.16b, v2.16b
	tbz w13, #0, .Lx86_vector_map3_insert_low\@
	mov v5.16b, v1.16b
	b .Lx86_vector_map3_insert_done\@
.Lx86_vector_map3_insert_low\@:
	mov v4.16b, v1.16b
.Lx86_vector_map3_insert_done\@:
	.elseif \opcode == 0x4a || \opcode == 0x4b || \opcode == 0x4c
	lsr w14, w13, #4
	and w14, w14, #0xf
	lsl x14, x14, #5
	add x14, _cpu, x14
	add x14, x14, #UML_X86_64_CPU_VECTORS
	ldr q6, [x14]
	.if \opcode == 0x4a
	vector_map3_variable_blend 32, 4, 0, 1, 6
	.elseif \opcode == 0x4b
	vector_map3_variable_blend 64, 4, 0, 1, 6
	.else
	vector_map3_variable_blend 8, 4, 0, 1, 6
	.endif
	.if \width == 256
	ldr q7, [x14, #16]
	.if \opcode == 0x4a
	vector_map3_variable_blend 32, 5, 2, 3, 7
	.elseif \opcode == 0x4b
	vector_map3_variable_blend 64, 5, 2, 3, 7
	.else
	vector_map3_variable_blend 8, 5, 2, 3, 7
	.endif
	.endif
	.endif
.endm

.macro vector_map3_shuffle_write width, descriptor
	and x9, x\descriptor, #0xffff
	.if \width == 64
	str d4, [_cpu, x9]
	sub x10, x9, #UML_X86_64_CPU_MMX
	lsr x10, x10, #3
	add x11, _cpu, #UML_X86_64_CPU_X87
	add x11, x11, x10, lsl #4
	str d4, [x11]
	mov w12, #0xffff
	strh w12, [x11, #8]
	strh wzr, [_cpu, #UML_X86_64_CPU_X87_TAG]
	strb wzr, [_cpu, #UML_X86_64_CPU_X87_TOP]
	.else
	add x9, _cpu, x9
	str q4, [x9]
	.if \width == 256
	str q5, [x9, #16]
	.else
	tbz x\descriptor, #63, .Lx86_vector_map3_shuffle_write_done\@
	movi v6.16b, #0
	str q6, [x9, #16]
.Lx86_vector_map3_shuffle_write_done\@:
	.endif
	.endif
.endm

.macro vector_map3_shuffle_input_size opcode, width
	.set .Lx86_vector_map3_shuffle_input_size, (\width / 8)
	.if \opcode == 0x18 || \opcode == 0x38
	.set .Lx86_vector_map3_shuffle_input_size, 16
	.endif
.endm

.macro vector_map3_shuffle_register symbol, opcode, width
gadget vector_map3_shuffle_\symbol\()_\width\()_rr
	ldp x8, x13, [_stream], #16
	mov x23, x8
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	.if \width == 64
	ldr d0, [_cpu, x10]
	ldr d1, [_cpu, x11]
	.else
	add x10, _cpu, x10
	add x11, _cpu, x11
	ldr q0, [x10]
	ldr q1, [x11]
	.if \width == 256
	ldr q2, [x10, #16]
	vector_map3_shuffle_input_size \opcode, \width
	.if .Lx86_vector_map3_shuffle_input_size == 32
	ldr q3, [x11, #16]
	.else
	movi v3.16b, #0
	.endif
	.endif
	.endif
	vector_map3_shuffle_lane \opcode, \width
	vector_map3_shuffle_write \width, 23
	retire_and_next
.endm

.macro vector_map3_shuffle_memory symbol, opcode, width
gadget vector_map3_shuffle_\symbol\()_\width\()_rm
	effective_address
	ldp x26, x27, [_stream], #16
	mov x25, x7
	vector_map3_shuffle_input_size \opcode, \width
	add x9, x7, #(.Lx86_vector_map3_shuffle_input_size - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_vector_map3_shuffle_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, .Lx86_vector_map3_shuffle_input_size
	.if .Lx86_vector_map3_shuffle_input_size == 8
	ldr d1, [x7]
	.else
	ldr q1, [x7]
	.endif
	.if .Lx86_vector_map3_shuffle_input_size == 32
	ldr q3, [x7, #16]
	.elseif \width == 256
	movi v3.16b, #0
	.endif
	b .Lx86_vector_map3_shuffle_loaded\@
.Lx86_vector_map3_shuffle_cross\@:
	sub sp, sp, #32
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #.Lx86_vector_map3_shuffle_input_size
	bl NAME(uml_x86_64_gadget_cross_page_vector_load)
	cbz w0, .Lx86_vector_map3_shuffle_cross_fault\@
	.if .Lx86_vector_map3_shuffle_input_size == 8
	ldr d1, [sp]
	.else
	ldr q1, [sp]
	.endif
	.if .Lx86_vector_map3_shuffle_input_size == 32
	ldr q3, [sp, #16]
	.elseif \width == 256
	movi v3.16b, #0
	.endif
	add sp, sp, #32
.Lx86_vector_map3_shuffle_loaded\@:
	mov w13, w27
	and x10, x26, #0xffff
	.if \width == 64
	ldr d0, [_cpu, x10]
	.else
	add x10, _cpu, x10
	ldr q0, [x10]
	.if \width == 256
	ldr q2, [x10, #16]
	.endif
	.endif
	vector_map3_shuffle_lane \opcode, \width
	vector_map3_shuffle_write \width, 23
	retire_and_next
.Lx86_vector_map3_shuffle_cross_fault\@:
	add sp, sp, #32
	b .Lx86_memory_fault
.endm

.macro vector_map3_shuffle_family opcode
	vector_map3_shuffle_register \opcode, 0x\opcode, 64
	vector_map3_shuffle_memory \opcode, 0x\opcode, 64
	vector_map3_shuffle_register \opcode, 0x\opcode, 128
	vector_map3_shuffle_memory \opcode, 0x\opcode, 128
	vector_map3_shuffle_register \opcode, 0x\opcode, 256
	vector_map3_shuffle_memory \opcode, 0x\opcode, 256
.endm

vector_map3_shuffle_family 00
vector_map3_shuffle_family 01
vector_map3_shuffle_family 02
vector_map3_shuffle_family 04
vector_map3_shuffle_family 05
vector_map3_shuffle_family 06
vector_map3_shuffle_family 0c
vector_map3_shuffle_family 0d
vector_map3_shuffle_family 0e
vector_map3_shuffle_family 0f
vector_map3_shuffle_family 18
vector_map3_shuffle_family 38
vector_map3_shuffle_family 46
vector_map3_shuffle_family 4a
vector_map3_shuffle_family 4b
vector_map3_shuffle_family 4c

.purgem vector_map3_shuffle_family
.purgem vector_map3_shuffle_memory
.purgem vector_map3_shuffle_register
.purgem vector_map3_shuffle_input_size
.purgem vector_map3_shuffle_write
.purgem vector_map3_shuffle_lane
.purgem vector_map3_variable_blend
.purgem vector_map3_select_128
.purgem vector_map3_mask_h
.purgem vector_map3_mask_d
.purgem vector_map3_mask_s
.purgem vector_map3_shuffle_dword_indices
.purgem vector_map3_qword_indices
.purgem vector_map3_index_sequence
