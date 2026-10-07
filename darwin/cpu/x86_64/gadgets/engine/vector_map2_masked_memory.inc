/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AVX/AVX2 per-element masked loads and stores. */

.macro vector_map2_mask_load element, width
gadget vector_map2_mask_load_\element\()_\width
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	add x26, _cpu, x26
	sub sp, sp, #32
	stp xzr, xzr, [sp]
	stp xzr, xzr, [sp, #16]
	mov x19, #0
Lx86_vector_map2_mask_load_loop\@:
	cmp x19, #(\width / 8)
	b.hs Lx86_vector_map2_mask_load_done\@
	.if \element == 32
	ldr w14, [x26, x19]
	tbz w14, #31, Lx86_vector_map2_mask_load_next\@
	.else
	ldr x14, [x26, x19]
	tbz x14, #63, Lx86_vector_map2_mask_load_next\@
	.endif
	add x7, x25, x19
	page_cross_check (\element / 8), Lx86_vector_map2_mask_load_cross\@
	translate_plain 0, UML_X86_64_TLB_READ_PAGE, (\element / 8), Lx86_vector_map2_mask_load_fault\@
	.if \element == 32
	ldr w24, [x7]
	str w24, [sp, x19]
	.else
	ldr x24, [x7]
	str x24, [sp, x19]
	.endif
	b Lx86_vector_map2_mask_load_next\@
Lx86_vector_map2_mask_load_cross\@:
	mov x0, _context
	add x1, x25, x19
	mov w2, #(\element / 8)
	helper_call uml_x86_64_gadget_cross_page_load
	cbz x1, Lx86_vector_map2_mask_load_fault\@
	.if \element == 32
	str w0, [sp, x19]
	.else
	str x0, [sp, x19]
	.endif
Lx86_vector_map2_mask_load_next\@:
	add x19, x19, #(\element / 8)
	b Lx86_vector_map2_mask_load_loop\@
Lx86_vector_map2_mask_load_done\@:
	ldr q4, [sp]
	.if \width == 256
	ldr q5, [sp, #16]
	.endif
	add sp, sp, #32
	and x9, x23, #0xffff
	add x9, _cpu, x9
	str q4, [x9]
	.if \width == 256
	str q5, [x9, #16]
	.else
	movi v5.16b, #0
	str q5, [x9, #16]
	.endif
	retire_and_next
Lx86_vector_map2_mask_load_fault\@:
	add sp, sp, #32
	b Lx86_memory_fault
.endm

.macro vector_map2_mask_store element, width
gadget vector_map2_mask_store_\element\()_\width
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	add x26, _cpu, x26
	and x27, x23, #0xffff
	add x27, _cpu, x27
	mov x19, #0
Lx86_vector_map2_mask_store_loop\@:
	cmp x19, #(\width / 8)
	b.hs Lx86_vector_map2_mask_store_done\@
	.if \element == 32
	ldr w14, [x26, x19]
	tbz w14, #31, Lx86_vector_map2_mask_store_next\@
	ldr w24, [x27, x19]
	.else
	ldr x14, [x26, x19]
	tbz x14, #63, Lx86_vector_map2_mask_store_next\@
	ldr x24, [x27, x19]
	.endif
	add x7, x25, x19
	page_cross_check (\element / 8), Lx86_vector_map2_mask_store_cross\@
	translate_plain 1, UML_X86_64_TLB_WRITE_PAGE, (\element / 8), Lx86_memory_fault
	.if \element == 32
	str w24, [x7]
	.else
	str x24, [x7]
	.endif
	b Lx86_vector_map2_mask_store_next\@
Lx86_vector_map2_mask_store_cross\@:
	mov x0, _context
	add x1, x25, x19
	mov x2, x24
	mov w3, #(\element / 8)
	helper_call uml_x86_64_gadget_cross_page_store
	cbz w0, Lx86_memory_fault
Lx86_vector_map2_mask_store_next\@:
	add x19, x19, #(\element / 8)
	b Lx86_vector_map2_mask_store_loop\@
Lx86_vector_map2_mask_store_done\@:
	finish_store (\width / 8)
.endm

.irp element, 32, 64
vector_map2_mask_load \element, 128
vector_map2_mask_load \element, 256
vector_map2_mask_store \element, 128
vector_map2_mask_store \element, 256
.endr

.purgem vector_map2_mask_store
.purgem vector_map2_mask_load
