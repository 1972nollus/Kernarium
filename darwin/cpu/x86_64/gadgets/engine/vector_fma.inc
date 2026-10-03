/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FMA3 132/213/231 packed and scalar fused operations. */

.macro vector_fma_scalar form, operation, element
	.if \element == 32
	.if \form == 0x90
	.if \operation == 9
	fmadd s6, s0, s2, s1
	.elseif \operation == 0xb
	fnmsub s6, s0, s2, s1
	.elseif \operation == 0xd
	fmsub s6, s0, s2, s1
	.else
	fnmadd s6, s0, s2, s1
	.endif
	.elseif \form == 0xa0
	.if \operation == 9
	fmadd s6, s1, s0, s2
	.elseif \operation == 0xb
	fnmsub s6, s1, s0, s2
	.elseif \operation == 0xd
	fmsub s6, s1, s0, s2
	.else
	fnmadd s6, s1, s0, s2
	.endif
	.else
	.if \operation == 9
	fmadd s6, s1, s2, s0
	.elseif \operation == 0xb
	fnmsub s6, s1, s2, s0
	.elseif \operation == 0xd
	fmsub s6, s1, s2, s0
	.else
	fnmadd s6, s1, s2, s0
	.endif
	.endif
	mov v6.s[1], v0.s[1]
	mov v6.s[2], v0.s[2]
	mov v6.s[3], v0.s[3]
	.else
	.if \form == 0x90
	.if \operation == 9
	fmadd d6, d0, d2, d1
	.elseif \operation == 0xb
	fnmsub d6, d0, d2, d1
	.elseif \operation == 0xd
	fmsub d6, d0, d2, d1
	.else
	fnmadd d6, d0, d2, d1
	.endif
	.elseif \form == 0xa0
	.if \operation == 9
	fmadd d6, d1, d0, d2
	.elseif \operation == 0xb
	fnmsub d6, d1, d0, d2
	.elseif \operation == 0xd
	fmsub d6, d1, d0, d2
	.else
	fnmadd d6, d1, d0, d2
	.endif
	.else
	.if \operation == 9
	fmadd d6, d1, d2, d0
	.elseif \operation == 0xb
	fnmsub d6, d1, d2, d0
	.elseif \operation == 0xd
	fmsub d6, d1, d2, d0
	.else
	fnmadd d6, d1, d2, d0
	.endif
	.endif
	mov v6.d[1], v0.d[1]
	.endif
.endm

.macro vector_fma_packed_lane form, operation, element, output, destination, vvvv, rm
	.if \form == 0x90
	mov v\output\().16b, v\vvvv\().16b
	.elseif \form == 0xa0
	mov v\output\().16b, v\rm\().16b
	.else
	mov v\output\().16b, v\destination\().16b
	.endif
	.if \operation == 0xa || \operation == 0xb || \
		\operation == 0xe || \operation == 0xf
	fneg v\output\().\element, v\output\().\element
	.elseif \operation == 6 || \operation == 7
	.ifc \element,4s
	movi v16.4s, #0
	mov w14, #0x80000000
	.if \operation == 6
	mov v16.s[0], w14
	mov v16.s[2], w14
	.else
	mov v16.s[1], w14
	mov v16.s[3], w14
	.endif
	.else
	movi v16.2d, #0
	mov x14, #0x8000000000000000
	.if \operation == 6
	mov v16.d[0], x14
	.else
	mov v16.d[1], x14
	.endif
	.endif
	eor v\output\().16b, v\output\().16b, v16.16b
	.endif
	.if \form == 0x90
	.if \operation >= 0xc
	fmls v\output\().\element, v\destination\().\element, v\rm\().\element
	.else
	fmla v\output\().\element, v\destination\().\element, v\rm\().\element
	.endif
	.elseif \form == 0xa0
	.if \operation >= 0xc
	fmls v\output\().\element, v\vvvv\().\element, v\destination\().\element
	.else
	fmla v\output\().\element, v\vvvv\().\element, v\destination\().\element
	.endif
	.else
	.if \operation >= 0xc
	fmls v\output\().\element, v\vvvv\().\element, v\rm\().\element
	.else
	fmla v\output\().\element, v\vvvv\().\element, v\rm\().\element
	.endif
	.endif
.endm

.macro vector_fma_body form, operation, element_bits, width, scalar
	.if \scalar
	vector_fma_scalar \form, \operation, \element_bits
	simd_check_scalar \element_bits, 6
	.else
	.if \element_bits == 32
	vector_fma_packed_lane \form, \operation, 4s, 6, 0, 1, 2
	.if \width == 256
	vector_fma_packed_lane \form, \operation, 4s, 7, 3, 4, 5
	.endif
	.else
	vector_fma_packed_lane \form, \operation, 2d, 6, 0, 1, 2
	.if \width == 256
	vector_fma_packed_lane \form, \operation, 2d, 7, 3, 4, 5
	.endif
	.endif
	.if \width == 256
	simd_check_packed \element_bits, 6, 7
	.else
	simd_check_packed \element_bits, 6
	.endif
	.endif
.endm

.macro vector_fma_write width, descriptor
	and x9, x\descriptor, #0xffff
	add x9, _cpu, x9
	str q6, [x9]
	.if \width == 256
	str q7, [x9, #16]
	.else
	movi v16.16b, #0
	str q16, [x9, #16]
	.endif
.endm

.macro vector_fma_register name, form, operation, element, shape, width, scalar
gadget vector_fma_\name\()_\element\()_\shape\()_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x9, _cpu, x9
	add x10, _cpu, x10
	add x11, _cpu, x11
	ldr q0, [x9]
	ldr q1, [x10]
	ldr q2, [x11]
	.if \width == 256
	ldr q3, [x9, #16]
	ldr q4, [x10, #16]
	ldr q5, [x11, #16]
	.endif
	vector_fma_body \form, \operation, \element, \width, \scalar
	vector_fma_write \width, 8
	retire_and_next
.endm

.macro vector_fma_memory name, form, operation, element, shape, width, scalar
gadget vector_fma_\name\()_\element\()_\shape\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	.if \scalar
	.set Lx86_vector_fma_input_size\@, (\element / 8)
	.else
	.set Lx86_vector_fma_input_size\@, (\width / 8)
	.endif
	page_cross_check (Lx86_vector_fma_input_size\@), Lx86_vector_fma_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, Lx86_vector_fma_input_size\@
	.if \scalar
	.if \element == 32
	ldr s2, [x7]
	.else
	ldr d2, [x7]
	.endif
	.else
	ldr q2, [x7]
	.if \width == 256
	ldr q5, [x7, #16]
	.endif
	.endif
	b Lx86_vector_fma_loaded\@
Lx86_vector_fma_cross\@:
	.if \scalar
	cross_page_load Lx86_vector_fma_input_size\@
	.if \element == 32
	fmov s2, w0
	.else
	fmov d2, x0
	.endif
	.else
	sub sp, sp, #32
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #Lx86_vector_fma_input_size\@
	bl NAME(uml_x86_64_gadget_cross_page_vector_load)
	cbz w0, Lx86_vector_fma_cross_fault\@
	ldr q2, [sp]
	.if \width == 256
	ldr q5, [sp, #16]
	.endif
	add sp, sp, #32
	.endif
Lx86_vector_fma_loaded\@:
	and x9, x23, #0xffff
	and x10, x26, #0xffff
	add x9, _cpu, x9
	add x10, _cpu, x10
	ldr q0, [x9]
	ldr q1, [x10]
	.if \width == 256
	ldr q3, [x9, #16]
	ldr q4, [x10, #16]
	.endif
	vector_fma_body \form, \operation, \element, \width, \scalar
	vector_fma_write \width, 23
	retire_and_next
	.if !\scalar
Lx86_vector_fma_cross_fault\@:
	add sp, sp, #32
	b Lx86_memory_fault
	.endif
.endm

.macro vector_fma_family name, form, operation, element, shape, width, scalar
	vector_fma_register \name, \form, \operation, \element, \shape, \width, \scalar
	vector_fma_memory \name, \form, \operation, \element, \shape, \width, \scalar
.endm

.macro vector_fma_opcode name, form, operation
	vector_fma_family \name, \form, \operation, 32, 128, 128, 0
	vector_fma_family \name, \form, \operation, 32, 256, 256, 0
	vector_fma_family \name, \form, \operation, 32, scalar, 128, 1
	vector_fma_family \name, \form, \operation, 64, 128, 128, 0
	vector_fma_family \name, \form, \operation, 64, 256, 256, 0
	vector_fma_family \name, \form, \operation, 64, scalar, 128, 1
.endm

vector_fma_opcode 96, 0x90, 6
vector_fma_opcode 97, 0x90, 7
vector_fma_opcode 98, 0x90, 8
vector_fma_opcode 99, 0x90, 9
vector_fma_opcode 9a, 0x90, 0xa
vector_fma_opcode 9b, 0x90, 0xb
vector_fma_opcode 9c, 0x90, 0xc
vector_fma_opcode 9d, 0x90, 0xd
vector_fma_opcode 9e, 0x90, 0xe
vector_fma_opcode 9f, 0x90, 0xf
vector_fma_opcode a6, 0xa0, 6
vector_fma_opcode a7, 0xa0, 7
vector_fma_opcode a8, 0xa0, 8
vector_fma_opcode a9, 0xa0, 9
vector_fma_opcode aa, 0xa0, 0xa
vector_fma_opcode ab, 0xa0, 0xb
vector_fma_opcode ac, 0xa0, 0xc
vector_fma_opcode ad, 0xa0, 0xd
vector_fma_opcode ae, 0xa0, 0xe
vector_fma_opcode af, 0xa0, 0xf
vector_fma_opcode b6, 0xb0, 6
vector_fma_opcode b7, 0xb0, 7
vector_fma_opcode b8, 0xb0, 8
vector_fma_opcode b9, 0xb0, 9
vector_fma_opcode ba, 0xb0, 0xa
vector_fma_opcode bb, 0xb0, 0xb
vector_fma_opcode bc, 0xb0, 0xc
vector_fma_opcode bd, 0xb0, 0xd
vector_fma_opcode be, 0xb0, 0xe
vector_fma_opcode bf, 0xb0, 0xf

.purgem vector_fma_opcode
.purgem vector_fma_family
.purgem vector_fma_memory
.purgem vector_fma_register
.purgem vector_fma_write
.purgem vector_fma_body
.purgem vector_fma_packed_lane
.purgem vector_fma_scalar
