/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MASKMOVQ/MASKMOVDQU byte-selecting stores through the implicit RDI address. */

.macro vector_mask_store width
gadget vector_mask_store_\width
	ldp x23, x22, [_stream], #16
	ldr x25, [_cpu, #(7 * 8)]
	mov x7, x25
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_vector_mask_store_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	.if \width == 64
	ldr d2, [x7]
	.else
	ldr q2, [x7]
	.endif
	mov x7, x25
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	and x9, x23, #0xffff
	ubfx x10, x23, #16, #16
	.if \width == 64
	ldr d0, [_cpu, x9]
	ldr d1, [_cpu, x10]
	.else
	ldr q0, [_cpu, x9]
	ldr q1, [_cpu, x10]
	.endif
	sshr v1.16b, v1.16b, #7
	bsl v1.16b, v0.16b, v2.16b
	.if \width == 64
	str d1, [x7]
	.else
	str q1, [x7]
	.endif
	finish_store (\width / 8)
.Lx86_vector_mask_store_cross\@:
	sub sp, sp, #16
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_vector_load)
	cbz w0, .Lx86_vector_mask_store_cross_read_fault\@
	and x9, x23, #0xffff
	ubfx x10, x23, #16, #16
	.if \width == 64
	ldr d0, [_cpu, x9]
	ldr d1, [_cpu, x10]
	ldr d2, [sp]
	.else
	ldr q0, [_cpu, x9]
	ldr q1, [_cpu, x10]
	ldr q2, [sp]
	.endif
	sshr v1.16b, v1.16b, #7
	bsl v1.16b, v0.16b, v2.16b
	.if \width == 64
	str d1, [sp]
	.else
	str q1, [sp]
	.endif
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_vector_store)
	add sp, sp, #16
	cbz w0, .Lx86_memory_fault
	finish_store (\width / 8)
.Lx86_vector_mask_store_cross_read_fault\@:
	add sp, sp, #16
	b .Lx86_memory_fault
.endm

vector_mask_store 64
vector_mask_store 128

.purgem vector_mask_store
