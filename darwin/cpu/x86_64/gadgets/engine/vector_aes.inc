/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AES-NI round and inverse-mix operations mapped to arm64 crypto. */

.arch_extension crypto

.macro vector_aes_lane opcode, output, state, key
	.if \opcode == 0xdb
	aesimc v\output\().16b, v\key\().16b
	.elseif \opcode == 0xdc
	movi v16.16b, #0
	mov v\output\().16b, v\state\().16b
	aese v\output\().16b, v16.16b
	aesmc v\output\().16b, v\output\().16b
	eor v\output\().16b, v\output\().16b, v\key\().16b
	.elseif \opcode == 0xdd
	movi v16.16b, #0
	mov v\output\().16b, v\state\().16b
	aese v\output\().16b, v16.16b
	eor v\output\().16b, v\output\().16b, v\key\().16b
	.elseif \opcode == 0xde
	movi v16.16b, #0
	mov v\output\().16b, v\state\().16b
	aesd v\output\().16b, v16.16b
	aesimc v\output\().16b, v\output\().16b
	eor v\output\().16b, v\output\().16b, v\key\().16b
	.else
	movi v16.16b, #0
	mov v\output\().16b, v\state\().16b
	aesd v\output\().16b, v16.16b
	eor v\output\().16b, v\output\().16b, v\key\().16b
	.endif
.endm

.macro vector_aes_body opcode, width
	vector_aes_lane \opcode, 4, 0, 1
	.if \width == 256
	vector_aes_lane \opcode, 5, 2, 3
	.endif
.endm

.macro vector_aes_write width, descriptor
	and x9, x\descriptor, #0xffff
	add x9, _cpu, x9
	str q4, [x9]
	.if \width == 256
	str q5, [x9, #16]
	.else
	tbz x\descriptor, #63, Lx86_vector_aes_write_done\@
	movi v16.16b, #0
	str q16, [x9, #16]
Lx86_vector_aes_write_done\@:
	.endif
.endm

.macro vector_aes_register opcode, width
gadget vector_aes_\opcode\()_\width\()_rr
	ldr x8, [_stream], #8
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
	vector_aes_body 0x\opcode, \width
	vector_aes_write \width, 8
	retire_and_next
.endm

.macro vector_aes_memory opcode, width
gadget vector_aes_\opcode\()_\width\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	page_cross_check (\width / 8), Lx86_vector_aes_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	ldr q1, [x7]
	.if \width == 256
	ldr q3, [x7, #16]
	.endif
	b Lx86_vector_aes_loaded\@
Lx86_vector_aes_cross\@:
	sub sp, sp, #32
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_vector_load)
	cbz w0, Lx86_vector_aes_cross_fault\@
	ldr q1, [sp]
	.if \width == 256
	ldr q3, [sp, #16]
	.endif
	add sp, sp, #32
Lx86_vector_aes_loaded\@:
	and x10, x26, #0xffff
	add x10, _cpu, x10
	ldr q0, [x10]
	.if \width == 256
	ldr q2, [x10, #16]
	.endif
	vector_aes_body 0x\opcode, \width
	vector_aes_write \width, 23
	retire_and_next
Lx86_vector_aes_cross_fault\@:
	add sp, sp, #32
	b Lx86_memory_fault
.endm

.macro vector_aes_family opcode, width
	vector_aes_register \opcode, \width
	vector_aes_memory \opcode, \width
.endm

vector_aes_family db, 128
vector_aes_family dc, 128
vector_aes_family dc, 256
vector_aes_family dd, 128
vector_aes_family dd, 256
vector_aes_family de, 128
vector_aes_family de, 256
vector_aes_family df, 128
vector_aes_family df, 256

.purgem vector_aes_family
.purgem vector_aes_memory
.purgem vector_aes_register
.purgem vector_aes_write
.purgem vector_aes_body
.purgem vector_aes_lane
