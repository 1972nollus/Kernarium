/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SSE4a EXTRQ and INSERTQ. */

/* Stream words:
 *   descriptor: destination offset in 15:0, source offset in 31:16
 *   control: bit 0 extracts, bit 1 selects immediate controls,
 *            raw length in 15:8, raw index in 23:16
 */
gadget vector_bitfield
	ldp x8, x13, [_stream], #16
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	add x9, _cpu, x9
	add x10, _cpu, x10
	ldr x11, [x9]
	ldr x12, [x10]
	tbz x13, #1, Lx86_vector_bitfield_register_control
	ubfx x14, x13, #8, #8
	ubfx x15, x13, #16, #8
	b Lx86_vector_bitfield_control_ready
Lx86_vector_bitfield_register_control:
	tbz x13, #0, Lx86_vector_bitfield_insert_control
	ldrb w14, [x10]
	ldrb w15, [x10, #1]
	b Lx86_vector_bitfield_control_ready
Lx86_vector_bitfield_insert_control:
	ldrb w14, [x10, #8]
	ldrb w15, [x10, #9]
Lx86_vector_bitfield_control_ready:
	and x14, x14, #0x3f
	and x15, x15, #0x3f
	cbz x14, Lx86_vector_bitfield_full_mask
	mov x16, #1
	lsl x16, x16, x14
	sub x16, x16, #1
	b Lx86_vector_bitfield_mask_ready
Lx86_vector_bitfield_full_mask:
	mov x16, #-1
Lx86_vector_bitfield_mask_ready:
	tbz x13, #0, Lx86_vector_bitfield_insert
	lsr x11, x11, x15
	and x11, x11, x16
	b Lx86_vector_bitfield_store
Lx86_vector_bitfield_insert:
	lsl x16, x16, x15
	lsl x12, x12, x15
	bic x11, x11, x16
	and x12, x12, x16
	orr x11, x11, x12
Lx86_vector_bitfield_store:
	str x11, [x9]
	str xzr, [x9, #8]
	retire_and_next
