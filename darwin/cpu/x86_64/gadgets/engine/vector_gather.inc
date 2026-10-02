/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AVX2 vector-indexed gathers with architectural partial-fault updates. */

.macro vector_gather_zero_upper data_bytes
	.if \data_bytes == 8
	str xzr, [x19, #8]
	str xzr, [x24, #8]
	movi v0.16b, #0
	str q0, [x19, #16]
	str q0, [x24, #16]
	.elseif \data_bytes == 16
	movi v0.16b, #0
	str q0, [x19, #16]
	str q0, [x24, #16]
	.endif
.endm

.macro vector_gather_gadget data, index, register_bytes
	.set Lx86_vector_gather_count, (\register_bytes / \data)
	.if \index > \data
	.set Lx86_vector_gather_count, (\register_bytes / \index)
	.endif
	.set Lx86_vector_gather_data_bytes, (Lx86_vector_gather_count * \data)
gadget vector_gather_d\data\()_i\index\()_r\register_bytes
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	and x19, x23, #0xffff
	add x19, _cpu, x19
	and x24, x26, #0xffff
	add x24, _cpu, x24
	ubfx x27, x26, #16, #16
	add x27, _cpu, x27
	mov x26, #0
Lx86_vector_gather_loop\@:
	cmp x26, #Lx86_vector_gather_count
	b.hs Lx86_vector_gather_done\@
	.if \data == 4
	ldr w14, [x24, x26, lsl #2]
	tbz w14, #31, Lx86_vector_gather_next\@
	.else
	ldr x14, [x24, x26, lsl #3]
	tbz x14, #63, Lx86_vector_gather_next\@
	.endif
	.if \index == 4
	ldr w14, [x27, x26, lsl #2]
	sxtw x14, w14
	.else
	ldr x14, [x27, x26, lsl #3]
	.endif
	ubfx x15, x23, #48, #2
	lsl x14, x14, x15
	add x7, x25, x14
	tbz x23, #52, Lx86_vector_gather_address_ready\@
	mov w7, w7
Lx86_vector_gather_address_ready\@:
	page_cross_check (\data), Lx86_vector_gather_cross\@
	translate_plain 0, UML_X86_64_TLB_READ_PAGE, \data, Lx86_vector_gather_fault\@
	.if \data == 4
	ldr w14, [x7]
	str w14, [x19, x26, lsl #2]
	str wzr, [x24, x26, lsl #2]
	.else
	ldr x14, [x7]
	str x14, [x19, x26, lsl #3]
	str xzr, [x24, x26, lsl #3]
	.endif
	b Lx86_vector_gather_next\@
Lx86_vector_gather_cross\@:
	mov x0, _context
	mov x1, x7
	mov w2, #\data
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, Lx86_vector_gather_fault\@
	.if \data == 4
	str w0, [x19, x26, lsl #2]
	str wzr, [x24, x26, lsl #2]
	.else
	str x0, [x19, x26, lsl #3]
	str xzr, [x24, x26, lsl #3]
	.endif
Lx86_vector_gather_next\@:
	add x26, x26, #1
	b Lx86_vector_gather_loop\@
Lx86_vector_gather_done\@:
	vector_gather_zero_upper Lx86_vector_gather_data_bytes
	retire_and_next
Lx86_vector_gather_fault\@:
	vector_gather_zero_upper Lx86_vector_gather_data_bytes
	b Lx86_memory_fault
.endm

.irp data, 4, 8
.irp index, 4, 8
vector_gather_gadget \data, \index, 16
vector_gather_gadget \data, \index, 32
.endr
.endr

.purgem vector_gather_gadget
.purgem vector_gather_zero_upper
