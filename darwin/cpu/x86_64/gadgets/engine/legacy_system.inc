/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Legacy segment, table, random-number, and address-translation operations. */

.macro legacy_set_szp value
	mov x9, #0xc4
	bic x8, x8, x9
	and w14, \value, #0xff
	cbnz w14, .Lx86_legacy_not_zero\@
	orr x8, x8, #0x40
.Lx86_legacy_not_zero\@:
	tbz w14, #7, .Lx86_legacy_not_sign\@
	orr x8, x8, #0x80
.Lx86_legacy_not_sign\@:
	eor w15, w14, w14, lsr #4
	eor w15, w15, w15, lsr #2
	eor w15, w15, w15, lsr #1
	tbnz w15, #0, .Lx86_legacy_odd_parity\@
	orr x8, x8, #0x4
.Lx86_legacy_odd_parity\@:
.endm

.macro legacy_store_flags
	orr x8, x8, #2
	str x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
.endm

gadget daa
	bl uml_x86_64_gadget_materialize_flags
	ldr x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	and w11, w10, #0xff
	mov w12, w11
	and w13, w8, #1
	and w14, w12, #0xf
	cmp w14, #9
	b.hi .Lx86_daa_low_adjust
	tbnz x8, #4, .Lx86_daa_low_adjust
	bic x8, x8, #0x10
	b .Lx86_daa_high_adjust
.Lx86_daa_low_adjust:
	add w12, w12, #6
	and w12, w12, #0xff
	orr x8, x8, #0x10
.Lx86_daa_high_adjust:
	bic x8, x8, #1
	cmp w11, #0x99
	b.hi .Lx86_daa_set_carry
	cbz w13, .Lx86_daa_flags
.Lx86_daa_set_carry:
	add w12, w12, #0x60
	and w12, w12, #0xff
	orr x8, x8, #1
.Lx86_daa_flags:
	bfi x10, x12, #0, #8
	str x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	legacy_set_szp w12
	legacy_store_flags
	retire_and_next

gadget das
	bl uml_x86_64_gadget_materialize_flags
	ldr x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	and w11, w10, #0xff
	mov w12, w11
	and w13, w8, #1
	and w14, w12, #0xf
	cmp w14, #9
	b.hi .Lx86_das_low_adjust
	tbnz x8, #4, .Lx86_das_low_adjust
	bic x8, x8, #0x10
	b .Lx86_das_high_adjust
.Lx86_das_low_adjust:
	sub w12, w12, #6
	and w12, w12, #0xff
	orr x8, x8, #0x10
.Lx86_das_high_adjust:
	bic x8, x8, #1
	cmp w11, #0x99
	b.hi .Lx86_das_set_carry
	cbz w13, .Lx86_das_flags
.Lx86_das_set_carry:
	sub w12, w12, #0x60
	and w12, w12, #0xff
	orr x8, x8, #1
.Lx86_das_flags:
	bfi x10, x12, #0, #8
	str x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	legacy_set_szp w12
	legacy_store_flags
	retire_and_next

.macro ascii_adjust name, arithmetic
gadget \name
	bl uml_x86_64_gadget_materialize_flags
	ldr x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	uxth w12, w10
	and w13, w12, #0xf
	cmp w13, #9
	b.hi .Lx86_ascii_adjust\@
	tbnz x8, #4, .Lx86_ascii_adjust\@
	mov x9, #0x11
	bic x8, x8, x9
	b .Lx86_ascii_mask\@
.Lx86_ascii_adjust\@:
	\arithmetic w12, w12, #0x106
	uxth w12, w12
	mov x9, #0x11
	orr x8, x8, x9
.Lx86_ascii_mask\@:
	and w13, w12, #0xf
	bfi w12, w13, #0, #8
	bfi x10, x12, #0, #16
	str x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	legacy_store_flags
	retire_and_next
.endm

ascii_adjust aaa, add
ascii_adjust aas, sub

.purgem ascii_adjust

gadget aam
	ldp x23, x22, [_stream], #16
	bl uml_x86_64_gadget_materialize_flags
	and w23, w23, #0xff
	cbz w23, .Lx86_aam_divide_error
	ldr x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	and w11, w10, #0xff
	udiv w12, w11, w23
	msub w13, w12, w23, w11
	bfi x10, x13, #0, #8
	bfi x10, x12, #8, #8
	str x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	legacy_set_szp w13
	legacy_store_flags
	retire_and_next
.Lx86_aam_divide_error:
	str x22, [_cpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_FLOATING_POINT_TRAP
	b uml_x86_64_gadget_leave

gadget aad
	ldp x23, x22, [_stream], #16
	bl uml_x86_64_gadget_materialize_flags
	and w23, w23, #0xff
	ldr x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	and w11, w10, #0xff
	ubfx w12, w10, #8, #8
	madd w13, w12, w23, w11
	and w13, w13, #0xff
	bfi x10, x13, #0, #8
	bfi x10, xzr, #8, #8
	str x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	legacy_set_szp w13
	legacy_store_flags
	retire_and_next

gadget salc
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	tst x8, #1
	csetm w11, ne
	ldr x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	bfi x10, x11, #0, #8
	str x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	retire_and_next

gadget into
	ldr x22, [_stream], #8
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	tbz x8, #11, .Lx86_into_not_taken
	str x22, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	mov w0, #UML_X86_64_EVENT_OVERFLOW
	b uml_x86_64_gadget_leave
.Lx86_into_not_taken:
	retire_and_next

.purgem legacy_store_flags
.purgem legacy_set_szp

.macro arpl_flags changed
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	bic x8, x8, #0x40
	.if \changed
	orr x8, x8, #0x40
	.endif
	orr x8, x8, #2
	str x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
.endm

gadget arpl_16_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ldrh w24, [_cpu, x9]
	ldrh w25, [_cpu, x10]
	and w11, w24, #3
	and w12, w25, #3
	cmp w11, w12
	b.hs .Lx86_arpl_register_unchanged
	bfi w24, w25, #0, #2
	strh w24, [_cpu, x9]
	arpl_flags 1
	retire_and_next
.Lx86_arpl_register_unchanged:
	arpl_flags 0
	retire_and_next

gadget arpl_16_mr
	effective_address
	mov x25, x7
	and x8, x23, #0xffff
	ldrh w24, [_cpu, x8]
	add x9, x7, #1
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_arpl_read_cross
	translate 0, UML_X86_64_TLB_READ_PAGE, 2
	ldrh w26, [x7]
	b .Lx86_arpl_memory_loaded
.Lx86_arpl_read_cross:
	mov x0, _context
	mov x1, x25
	mov w2, #2
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov w26, w0
.Lx86_arpl_memory_loaded:
	and w11, w26, #3
	and w12, w24, #3
	cmp w11, w12
	b.hs .Lx86_arpl_memory_unchanged
	bfi w26, w24, #0, #2
	mov x7, x25
	add x9, x7, #1
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_arpl_write_cross
	translate 1, UML_X86_64_TLB_WRITE_PAGE, 2
	strh w26, [x7]
	b .Lx86_arpl_memory_written
.Lx86_arpl_write_cross:
	mov x0, _context
	mov x1, x25
	mov x2, x26
	mov w3, #2
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
.Lx86_arpl_memory_written:
	arpl_flags 1
	finish_store 2
.Lx86_arpl_memory_unchanged:
	arpl_flags 0
	retire_and_next

.purgem arpl_flags

.macro push_all width
gadget push_all_\width
	ldp x23, x22, [_stream], #16
	ldr x26, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	sub w25, w26, #(8 * (\width / 8))
	mov x7, x25
	add x9, x7, #(8 * (\width / 8) - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_push_all_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (8 * (\width / 8))
	.if \width == 32
	ldr w8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 7 * 8)]
	ldr w9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 6 * 8)]
	ldr w10, [_cpu, #(UML_X86_64_CPU_REGISTERS + 5 * 8)]
	mov w11, w26
	ldr w12, [_cpu, #(UML_X86_64_CPU_REGISTERS + 3 * 8)]
	ldr w13, [_cpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	ldr w14, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	ldr w15, [_cpu, #UML_X86_64_CPU_REGISTERS]
	stp w8, w9, [x7]
	stp w10, w11, [x7, #8]
	stp w12, w13, [x7, #16]
	stp w14, w15, [x7, #24]
	.else
	ldrh w8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 7 * 8)]
	ldrh w9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 6 * 8)]
	ldrh w10, [_cpu, #(UML_X86_64_CPU_REGISTERS + 5 * 8)]
	uxth w11, w26
	ldrh w12, [_cpu, #(UML_X86_64_CPU_REGISTERS + 3 * 8)]
	ldrh w13, [_cpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	ldrh w14, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	ldrh w15, [_cpu, #UML_X86_64_CPU_REGISTERS]
	strh w8, [x7]
	strh w9, [x7, #2]
	strh w10, [x7, #4]
	strh w11, [x7, #6]
	strh w12, [x7, #8]
	strh w13, [x7, #10]
	strh w14, [x7, #12]
	strh w15, [x7, #14]
	.endif
	b .Lx86_push_all_done\@
.Lx86_push_all_cross\@:
	.if \width == 32
	ldr w8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 7 * 8)]
	ldr w9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 6 * 8)]
	orr x2, x8, x9, lsl #32
	mov x0, _context
	mov x1, x25
	mov w3, #8
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	ldr w8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 5 * 8)]
	mov w9, w26
	orr x2, x8, x9, lsl #32
	mov x0, _context
	add x1, x25, #8
	mov w3, #8
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	ldr w8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 3 * 8)]
	ldr w9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	orr x2, x8, x9, lsl #32
	mov x0, _context
	add x1, x25, #16
	mov w3, #8
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	ldr w8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	ldr w9, [_cpu, #UML_X86_64_CPU_REGISTERS]
	orr x2, x8, x9, lsl #32
	mov x0, _context
	add x1, x25, #24
	mov w3, #8
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	.else
	ldrh w8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 7 * 8)]
	ldrh w9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 6 * 8)]
	ldrh w10, [_cpu, #(UML_X86_64_CPU_REGISTERS + 5 * 8)]
	uxth w11, w26
	orr x2, x8, x9, lsl #16
	orr x2, x2, x10, lsl #32
	orr x2, x2, x11, lsl #48
	mov x0, _context
	mov x1, x25
	mov w3, #8
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	ldrh w8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 3 * 8)]
	ldrh w9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	ldrh w10, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	ldrh w11, [_cpu, #UML_X86_64_CPU_REGISTERS]
	orr x2, x8, x9, lsl #16
	orr x2, x2, x10, lsl #32
	orr x2, x2, x11, lsl #48
	mov x0, _context
	add x1, x25, #8
	mov w3, #8
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	.endif
.Lx86_push_all_done\@:
	str x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	finish_store (8 * (\width / 8))
.endm

.macro pop_all width
gadget pop_all_\width
	ldr x22, [_stream], #8
	ldr w25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	mov x7, x25
	add x9, x7, #(8 * (\width / 8) - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_pop_all_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (8 * (\width / 8))
	.if \width == 32
	ldp w8, w9, [x7]
	ldp w10, w11, [x7, #8]
	ldp w12, w13, [x7, #16]
	ldp w14, w15, [x7, #24]
	.else
	ldrh w8, [x7]
	ldrh w9, [x7, #2]
	ldrh w10, [x7, #4]
	ldrh w11, [x7, #6]
	ldrh w12, [x7, #8]
	ldrh w13, [x7, #10]
	ldrh w14, [x7, #12]
	ldrh w15, [x7, #14]
	.endif
	b .Lx86_pop_all_commit\@
.Lx86_pop_all_cross\@:
	sub sp, sp, #32
	mov x0, _context
	mov x1, x25
	mov w2, #8
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_pop_all_cross_fault\@
	str x0, [sp]
	mov x0, _context
	add x1, x25, #8
	mov w2, #8
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_pop_all_cross_fault\@
	str x0, [sp, #8]
	.if \width == 32
	mov x0, _context
	add x1, x25, #16
	mov w2, #8
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_pop_all_cross_fault\@
	str x0, [sp, #16]
	mov x0, _context
	add x1, x25, #24
	mov w2, #8
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_pop_all_cross_fault\@
	str x0, [sp, #24]
	ldr w8, [sp]
	ldr w9, [sp, #4]
	ldr w10, [sp, #8]
	ldr w11, [sp, #12]
	ldr w12, [sp, #16]
	ldr w13, [sp, #20]
	ldr w14, [sp, #24]
	ldr w15, [sp, #28]
	.else
	ldrh w8, [sp]
	ldrh w9, [sp, #2]
	ldrh w10, [sp, #4]
	ldrh w11, [sp, #6]
	ldrh w12, [sp, #8]
	ldrh w13, [sp, #10]
	ldrh w14, [sp, #12]
	ldrh w15, [sp, #14]
	.endif
	add sp, sp, #32
	b .Lx86_pop_all_commit\@
.Lx86_pop_all_cross_fault\@:
	add sp, sp, #32
	b .Lx86_memory_fault
.Lx86_pop_all_commit\@:
	.if \width == 32
	str x8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 7 * 8)]
	str x9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 6 * 8)]
	str x10, [_cpu, #(UML_X86_64_CPU_REGISTERS + 5 * 8)]
	str x12, [_cpu, #(UML_X86_64_CPU_REGISTERS + 3 * 8)]
	str x13, [_cpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	str x14, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	str x15, [_cpu, #UML_X86_64_CPU_REGISTERS]
	.else
	strh w8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 7 * 8)]
	strh w9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 6 * 8)]
	strh w10, [_cpu, #(UML_X86_64_CPU_REGISTERS + 5 * 8)]
	strh w12, [_cpu, #(UML_X86_64_CPU_REGISTERS + 3 * 8)]
	strh w13, [_cpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	strh w14, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	strh w15, [_cpu, #UML_X86_64_CPU_REGISTERS]
	.endif
	add w25, w25, #(8 * (\width / 8))
	str x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	retire_and_next
.endm

push_all 16
push_all 32
pop_all 16
pop_all 32

.purgem pop_all
.purgem push_all

.macro push_segment_compat32
gadget push_segment_32
	ldp x23, x22, [_stream], #16
	and x8, x23, #0xffff
	ldrh w24, [_cpu, x8]
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	sub w25, w25, #4
	mov x7, x25
	add x9, x7, #3
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_push_segment_32_cross
	translate 1, UML_X86_64_TLB_WRITE_PAGE, 4
	str w24, [x7]
.Lx86_push_segment_32_done:
	str x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	finish_store 4
.Lx86_push_segment_32_cross:
	mov x0, _context
	mov x1, x25
	mov x2, x24
	mov w3, #4
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	b .Lx86_push_segment_32_done
.endm

push_segment_compat32
.purgem push_segment_compat32

gadget push_segment_64
	ldp x23, x22, [_stream], #16
	and x8, x23, #0xffff
	ldrh w24, [_cpu, x8]
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	sub x25, x25, #8
	mov x7, x25
	add x9, x7, #7
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_push_segment_64_cross
	translate 1, UML_X86_64_TLB_WRITE_PAGE, 8
	str x24, [x7]
.Lx86_push_segment_64_done:
	str x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	finish_store 8
.Lx86_push_segment_64_cross:
	mov x0, _context
	mov x1, x25
	mov x2, x24
	mov w3, #8
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	b .Lx86_push_segment_64_done

gadget pop_segment_32
	ldp x23, x22, [_stream], #16
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	mov w25, w25
	mov x7, x25
	add x9, x7, #3
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_pop_segment_32_cross
	translate 0, UML_X86_64_TLB_READ_PAGE, 4
	ldr w24, [x7]
.Lx86_pop_segment_32_done:
	add w9, w25, #4
	str x9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	and x8, x23, #0xffff
	strh w24, [_cpu, x8]
	retire_and_next
.Lx86_pop_segment_32_cross:
	mov x0, _context
	mov x1, x25
	mov w2, #4
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
	b .Lx86_pop_segment_32_done

gadget pop_segment_64
	ldp x23, x22, [_stream], #16
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	mov x7, x25
	add x9, x7, #7
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_pop_segment_64_cross
	translate 0, UML_X86_64_TLB_READ_PAGE, 8
	ldr x24, [x7]
.Lx86_pop_segment_64_done:
	add x9, x25, #8
	str x9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	and x8, x23, #0xffff
	strh w24, [_cpu, x8]
	retire_and_next
.Lx86_pop_segment_64_cross:
	mov x0, _context
	mov x1, x25
	mov w2, #8
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
	b .Lx86_pop_segment_64_done

gadget xlat
	ldp x23, x22, [_stream], #16
	ldr x7, [_cpu, #(UML_X86_64_CPU_REGISTERS + 3 * 8)]
	ldrb w9, [_cpu, #UML_X86_64_CPU_REGISTERS]
	tbnz x23, #3, .Lx86_xlat_address_16
	tbz x23, #0, .Lx86_xlat_address_64
	add w7, w7, w9
	mov w7, w7
	b .Lx86_xlat_segment
.Lx86_xlat_address_16:
	add w7, w7, w9
	uxth w7, w7
	b .Lx86_xlat_segment
.Lx86_xlat_address_64:
	add x7, x7, x9
.Lx86_xlat_segment:
	ubfx w9, w23, #1, #2
	cmp w9, #1
	b.ne .Lx86_xlat_not_fs
	ldr x10, [_cpu, #UML_X86_64_CPU_FS_BASE]
	add x7, x7, x10
	b .Lx86_xlat_load
.Lx86_xlat_not_fs:
	cmp w9, #2
	b.ne .Lx86_xlat_load
	ldr x10, [_cpu, #UML_X86_64_CPU_GS_BASE]
	add x7, x7, x10
.Lx86_xlat_load:
	translate 0, UML_X86_64_TLB_READ_PAGE, 1
	ldrb w11, [x7]
	ldr x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	bfi x10, x11, #0, #8
	str x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	retire_and_next

.macro random_value width, store, host_register
gadget random_\width
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_stream], #8
	mrs x11, cntvct_el0
	eor x11, x11, _cpu
	eor x11, x11, x11, lsl #13
	eor x11, x11, x11, lsr #7
	eor x11, x11, x11, lsl #17
	cbnz x11, .Lx86_random_nonzero\@
	mov x11, #1
.Lx86_random_nonzero\@:
	.if \width == 32
	mov w11, w11
	.endif
	\store \host_register\()11, [_cpu, x8]
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	mov x10, #0x8d5
	bic x9, x9, x10
	orr x9, x9, #3
	str x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	retire_and_next
.endm

random_value 16, strh, w
random_value 32, str, x
random_value 64, str, x

.purgem random_value

gadget store_gdtr
	effective_address
	mov x25, x7
	mov x24, #0xfffffffffffe0000
	add x9, x7, #9
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_store_gdtr_cross
	translate 1, UML_X86_64_TLB_WRITE_PAGE, 10
	strh wzr, [x7]
	stur x24, [x7, #2]
.Lx86_store_gdtr_done:
	finish_store 10
.Lx86_store_gdtr_cross:
	sub sp, sp, #16
	strh wzr, [sp]
	stur x24, [sp, #2]
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #10
	bl NAME(uml_x86_64_gadget_cross_page_vector_store)
	add sp, sp, #16
	cbz w0, .Lx86_memory_fault
	b .Lx86_store_gdtr_done
