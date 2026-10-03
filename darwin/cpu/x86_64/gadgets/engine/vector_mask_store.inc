/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MASKMOVQ/MASKMOVDQU byte-selecting stores through the implicit DS:(E/R)DI. */

.macro vector_mask_store width
/* Only the selected bytes are written, one byte store each: x86 leaves the
 * others untouched, so reading them back and storing them again would undo
 * another thread's write to them.  The mask's sign bits are gathered into a
 * bit set first, so an unselected byte costs nothing, and a write translation
 * is taken only for a page a selected byte lies on, so only such a page can
 * fault.  The address is DS:(E/R)DI with any segment override and address
 * size, from the descriptor.
 *
 * x19 holds the bytes still to store, x24 and x26 the last page translated
 * and its host displacement, x25 the address and x27 the data.
 */
gadget vector_mask_store_\width
	effective_address
	ldr x8, [_stream], #8
	mov x25, x7
	/* Multiplying the isolated sign bits by 2^0 + 2^7 + ... + 2^49 moves
	 * byte i's sign bit to bit 56 + i with no carries into the top byte. */
	mov x10, #0x4081
	movk x10, #0x1020, lsl #16
	movk x10, #0x0408, lsl #32
	movk x10, #0x0002, lsl #48
	ldr x9, [_cpu, x8]
	and x9, x9, #0x8080808080808080
	mul x9, x9, x10
	lsr x19, x9, #56
	.if \width == 128
	add x8, x8, #8
	ldr x9, [_cpu, x8]
	and x9, x9, #0x8080808080808080
	mul x9, x9, x10
	lsr x9, x9, #56
	orr x19, x19, x9, lsl #8
	.endif
	cbz x19, Lx86_vector_mask_store_done\@
	and x27, x23, #0xffff
	add x27, _cpu, x27
	/* No page is page-aligned plus one. */
	mov x24, #1
Lx86_vector_mask_store_loop\@:
	rbit x9, x19
	clz x9, x9
	mov x10, #1
	lsl x10, x10, x9
	bic x19, x19, x10
	add x7, x25, x9
	and x10, x7, #0xfffffffffffff000
	cmp x10, x24
	b.ne Lx86_vector_mask_store_translate\@
	add x7, x7, x26
Lx86_vector_mask_store_byte\@:
	ldrb w14, [x27, x9]
	strb w14, [x7]
	cbnz x19, Lx86_vector_mask_store_loop\@
Lx86_vector_mask_store_done\@:
	finish_store (\width / 8)
Lx86_vector_mask_store_translate\@:
	mov x24, x7
	translate_plain 1, UML_X86_64_TLB_WRITE_PAGE, 1, Lx86_memory_fault
	sub x26, x7, x24
	sub x9, x24, x25
	and x24, x24, #0xfffffffffffff000
	b Lx86_vector_mask_store_byte\@
.endm

vector_mask_store 64
vector_mask_store 128

.purgem vector_mask_store
