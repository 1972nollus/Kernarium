/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Legacy segment, table, random-number, and address-translation operations. */

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
	tbz x23, #0, .Lx86_xlat_address_64
	add w7, w7, w9
	mov w7, w7
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
