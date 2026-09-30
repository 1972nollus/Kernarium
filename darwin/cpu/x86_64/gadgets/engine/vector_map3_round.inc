/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SSE4.1/AVX packed and scalar immediate floating-point rounding. */

.macro vector_map3_round_apply element, output, input, scalar
	and w14, w13, #3
	tbz w13, #2, .Lx86_vector_map3_round_mode_ready\@
	ldr w14, [_cpu, #UML_X86_64_CPU_MXCSR]
	ubfx w14, w14, #13, #2
.Lx86_vector_map3_round_mode_ready\@:
	cmp w14, #1
	b.eq .Lx86_vector_map3_round_down\@
	cmp w14, #2
	b.eq .Lx86_vector_map3_round_up\@
	cmp w14, #3
	b.eq .Lx86_vector_map3_round_zero\@
	.if \scalar
	.if \element == 32
	frintn s6, s\input
	.else
	frintn d6, d\input
	.endif
	.else
	.if \element == 32
	frintn v6.4s, v\input\().4s
	.else
	frintn v6.2d, v\input\().2d
	.endif
	.endif
	b .Lx86_vector_map3_round_done\@
.Lx86_vector_map3_round_down\@:
	.if \scalar
	.if \element == 32
	frintm s6, s\input
	.else
	frintm d6, d\input
	.endif
	.else
	.if \element == 32
	frintm v6.4s, v\input\().4s
	.else
	frintm v6.2d, v\input\().2d
	.endif
	.endif
	b .Lx86_vector_map3_round_done\@
.Lx86_vector_map3_round_up\@:
	.if \scalar
	.if \element == 32
	frintp s6, s\input
	.else
	frintp d6, d\input
	.endif
	.else
	.if \element == 32
	frintp v6.4s, v\input\().4s
	.else
	frintp v6.2d, v\input\().2d
	.endif
	.endif
	b .Lx86_vector_map3_round_done\@
.Lx86_vector_map3_round_zero\@:
	.if \scalar
	.if \element == 32
	frintz s6, s\input
	.else
	frintz d6, d\input
	.endif
	.else
	.if \element == 32
	frintz v6.4s, v\input\().4s
	.else
	frintz v6.2d, v\input\().2d
	.endif
	.endif
.Lx86_vector_map3_round_done\@:
	.if \scalar
	.if \element == 32
	mov v\output\().s[0], v6.s[0]
	.else
	mov v\output\().d[0], v6.d[0]
	.endif
	.else
	mov v\output\().16b, v6.16b
	.endif
.endm

/* Rounding raises invalid operation only for a signalling NaN, which it
 * leaves a NaN. */
.macro vector_map3_round_body opcode, width
	.if \opcode == 0x08
	vector_map3_round_apply 32, 4, 1, 0
	.if \width == 256
	vector_map3_round_apply 32, 5, 3, 0
	.endif
	simd_check_result 32, \width
	.elseif \opcode == 0x09
	vector_map3_round_apply 64, 4, 1, 0
	.if \width == 256
	vector_map3_round_apply 64, 5, 3, 0
	.endif
	simd_check_result 64, \width
	.elseif \opcode == 0x0a
	mov v4.16b, v0.16b
	vector_map3_round_apply 32, 4, 1, 1
	simd_check_scalar 32, 4
	.else
	mov v4.16b, v0.16b
	vector_map3_round_apply 64, 4, 1, 1
	simd_check_scalar 64, 4
	.endif
.endm

.macro vector_map3_round_register symbol, opcode, width
gadget vector_map3_round_\symbol\()_\width\()_rr
	ldp x8, x13, [_stream], #16
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x10, _cpu, x10
	add x11, _cpu, x11
	ldr q0, [x10]
	ldr q1, [x11]
	.if \width == 256
	ldr q3, [x11, #16]
	.endif
	vector_map3_round_body \opcode, \width
	vector_write_xmm \width, 8
	retire_and_next
.endm

.macro vector_map3_round_memory symbol, opcode, width, input_size
gadget vector_map3_round_\symbol\()_\width\()_rm
	effective_address
	ldp x26, x27, [_stream], #16
	mov x25, x7
	page_cross_check (\input_size), .Lx86_vector_map3_round_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, \input_size
	.if \input_size == 4
	ldr s1, [x7]
	.elseif \input_size == 8
	ldr d1, [x7]
	.else
	ldr q1, [x7]
	.if \input_size == 32
	ldr q3, [x7, #16]
	.endif
	.endif
	b .Lx86_vector_map3_round_loaded\@
.Lx86_vector_map3_round_cross\@:
	.if \input_size <= 8
	mov x0, _context
	mov x1, x25
	mov w2, #\input_size
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	.if \input_size == 4
	fmov s1, w0
	.else
	fmov d1, x0
	.endif
	.else
	sub sp, sp, #32
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #\input_size
	bl NAME(uml_x86_64_gadget_cross_page_vector_load)
	cbz w0, .Lx86_vector_map3_round_cross_fault\@
	ldr q1, [sp]
	.if \input_size == 32
	ldr q3, [sp, #16]
	.endif
	add sp, sp, #32
	.endif
.Lx86_vector_map3_round_loaded\@:
	mov w13, w27
	and x10, x26, #0xffff
	add x10, _cpu, x10
	ldr q0, [x10]
	vector_map3_round_body \opcode, \width
	vector_write_xmm \width, 23
	retire_and_next
	.if \input_size > 8
.Lx86_vector_map3_round_cross_fault\@:
	add sp, sp, #32
	b .Lx86_memory_fault
	.endif
.endm

vector_map3_round_register 08, 0x08, 128
vector_map3_round_register 08, 0x08, 256
vector_map3_round_memory 08, 0x08, 128, 16
vector_map3_round_memory 08, 0x08, 256, 32
vector_map3_round_register 09, 0x09, 128
vector_map3_round_register 09, 0x09, 256
vector_map3_round_memory 09, 0x09, 128, 16
vector_map3_round_memory 09, 0x09, 256, 32
vector_map3_round_register 0a, 0x0a, 128
vector_map3_round_memory 0a, 0x0a, 128, 4
vector_map3_round_register 0b, 0x0b, 128
vector_map3_round_memory 0b, 0x0b, 128, 8

.purgem vector_map3_round_memory
.purgem vector_map3_round_register
.purgem vector_map3_round_body
.purgem vector_map3_round_apply

/* F16C uses its immediate (or MXCSR) rounding mode rather than the host
 * thread's FPCR. Save and restore FPCR around the native narrowing operation.
 */
.macro vector_map3_half_body width
	and w14, w13, #3
	tbz w13, #2, .Lx86_vector_map3_half_mode_ready\@
	ldr w14, [_cpu, #UML_X86_64_CPU_MXCSR]
	ubfx w14, w14, #13, #2
.Lx86_vector_map3_half_mode_ready\@:
	and w15, w14, #1
	lsl w15, w15, #1
	ubfx w17, w14, #1, #1
	orr w15, w15, w17
	mrs x16, fpcr
	bic x17, x16, #(3 << 22)
	orr x17, x17, x15, lsl #22
	msr fpcr, x17
	fcvtn v4.4h, v1.4s
	.if \width == 256
	fcvtn v5.4h, v3.4s
	mov v4.d[1], v5.d[0]
	.else
	movi v5.16b, #0
	mov v4.d[1], v5.d[0]
	.endif
	msr fpcr, x16
	simd_check_packed 16, 4
.endm

.macro vector_map3_half_register width
gadget vector_map3_half_\width\()_r
	ldp x8, x13, [_stream], #16
	ubfx x10, x8, #16, #16
	add x10, _cpu, x10
	ldr q1, [x10]
	.if \width == 256
	ldr q3, [x10, #16]
	.endif
	vector_map3_half_body \width
	and x9, x8, #0xffff
	add x9, _cpu, x9
	str q4, [x9]
	movi v5.16b, #0
	str q5, [x9, #16]
	retire_and_next
.endm

.macro vector_map3_half_memory width, output_size
gadget vector_map3_half_\width\()_m
	effective_address
	ldr x13, [_stream], #8
	mov x25, x7
	and x10, x23, #0xffff
	add x10, _cpu, x10
	ldr q1, [x10]
	.if \width == 256
	ldr q3, [x10, #16]
	.endif
	vector_map3_half_body \width
	add x9, x25, #(\output_size - 1)
	eor x9, x9, x25
	tbnz x9, #12, .Lx86_vector_map3_half_cross\@
	mov x7, x25
	translate 1, UML_X86_64_TLB_WRITE_PAGE, \output_size
	.if \output_size == 8
	str d4, [x7]
	.else
	str q4, [x7]
	.endif
	finish_store \output_size
.Lx86_vector_map3_half_cross\@:
	.if \output_size == 8
	mov x0, _context
	mov x1, x25
	fmov x2, d4
	mov w3, #8
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	.else
	sub sp, sp, #16
	str q4, [sp]
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #16
	bl NAME(uml_x86_64_gadget_cross_page_vector_store)
	add sp, sp, #16
	cbz w0, .Lx86_memory_fault
	.endif
	finish_store \output_size
.endm

vector_map3_half_register 128
vector_map3_half_register 256
vector_map3_half_memory 128, 8
vector_map3_half_memory 256, 16

.purgem vector_map3_half_memory
.purgem vector_map3_half_register
.purgem vector_map3_half_body
