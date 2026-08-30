/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scalar transfers, effective addresses, stack operations, and memory control transfers. */
.macro scalar_move_rr name, load, store, load_register, store_register
gadget \name
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \load_register\()11, [_cpu, x10]
	\store \store_register\()11, [_cpu, x9]
	retire_and_next
.endm

.macro scalar_move_ri name, store, store_register
gadget \name
	ldp x8, x11, [_stream], #16
	and x9, x8, #0xffff
	\store \store_register\()11, [_cpu, x9]
	retire_and_next
.endm

scalar_move_rr move_8_rr, ldrb, strb, w, w
scalar_move_rr move_16_rr, ldrh, strh, w, w
scalar_move_rr move_32_rr, ldr, str, w, x
scalar_move_rr move_64_rr, ldr, str, x, x
scalar_move_ri move_8_ri, strb, w
scalar_move_ri move_16_ri, strh, w
scalar_move_ri move_32_ri, str, x
scalar_move_ri move_64_ri, str, x

.purgem scalar_move_ri
.purgem scalar_move_rr

.macro memory_move_load width, load, host_register, store, store_register
gadget move_\width\()_rm
	effective_address
	mov x25, x7
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_load_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()11, [x7]
.Lx86_load_write\@:
	and x8, x23, #0xffff
	\store \store_register\()11, [_cpu, x8]
	retire_and_next
.Lx86_load_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x11, x0
	b .Lx86_load_write\@
.endm

.macro memory_move_store form, width, load, host_register, store
gadget move_\width\()_m\form
	effective_address
	mov x25, x7
	.ifc \form,r
	and x8, x23, #0xffff
	\load \host_register\()24, [_cpu, x8]
	.endif
	.ifc \form,i
	ldr x24, [_stream], #8
	.endif
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_store_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()24, [x7]
	finish_store (\width / 8)
.Lx86_store_cross\@:
	mov x0, _context
	mov x1, x25
	mov x2, x24
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	finish_store (\width / 8)
.endm

memory_move_load 8, ldrb, w, strb, w
memory_move_load 16, ldrh, w, strh, w
memory_move_load 32, ldr, w, str, x
memory_move_load 64, ldr, x, str, x
memory_move_store r, 8, ldrb, w, strb
memory_move_store r, 16, ldrh, w, strh
memory_move_store r, 32, ldr, w, str
memory_move_store r, 64, ldr, x, str
memory_move_store i, 8, ldrb, w, strb
memory_move_store i, 16, ldrh, w, strh
memory_move_store i, 32, ldr, w, str
memory_move_store i, 64, ldr, x, str

.purgem memory_move_store
.purgem memory_move_load

gadget lea_16
	effective_address
	and x8, x23, #0xffff
	strh w7, [_cpu, x8]
	retire_and_next

gadget lea_32
	effective_address
	and x8, x23, #0xffff
	mov w7, w7
	str x7, [_cpu, x8]
	retire_and_next

gadget lea_64
	effective_address
	and x8, x23, #0xffff
	str x7, [_cpu, x8]
	retire_and_next

.macro push_register width, load, host_register, store
gadget push_\width\()_r
	ldp x23, x22, [_stream], #16
	and x8, x23, #0xffff
	\load \host_register\()24, [_cpu, x8]
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	sub x25, x25, #(\width / 8)
	mov x7, x25
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_push_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()24, [x7]
.Lx86_push_done\@:
	str x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	finish_store (\width / 8)
.Lx86_push_cross\@:
	mov x0, _context
	mov x1, x25
	mov x2, x24
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	b .Lx86_push_done\@
.endm

.macro push_immediate width, host_register, store
gadget push_\width\()_i
	ldp x23, x24, [_stream], #16
	ldr x22, [_stream], #8
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	sub x25, x25, #(\width / 8)
	mov x7, x25
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_push_immediate_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()24, [x7]
.Lx86_push_immediate_done\@:
	str x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	finish_store (\width / 8)
.Lx86_push_immediate_cross\@:
	mov x0, _context
	mov x1, x25
	mov x2, x24
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	b .Lx86_push_immediate_done\@
.endm

.macro pop_register width, load, host_register, store, store_register
gadget pop_\width\()_r
	ldp x23, x22, [_stream], #16
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	mov x7, x25
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_pop_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
.Lx86_pop_done\@:
	add x9, x25, #(\width / 8)
	str x9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	and x8, x23, #0xffff
	\store \store_register\()24, [_cpu, x8]
	retire_and_next
.Lx86_pop_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
	b .Lx86_pop_done\@
.endm

push_register 16, ldrh, w, strh
push_register 64, ldr, x, str
push_immediate 16, w, strh
push_immediate 64, x, str
pop_register 16, ldrh, w, strh, w
pop_register 64, ldr, x, str, x

.purgem pop_register
.purgem push_immediate
.purgem push_register

.macro push_flags width, store, host_register
gadget push_flags_\width
	bl uml_x86_64_gadget_materialize_flags
	ldr x24, [_cpu, #UML_X86_64_CPU_RFLAGS]
	ldp x23, x22, [_stream], #16
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	sub x25, x25, #(\width / 8)
	mov x7, x25
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_push_flags_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()24, [x7]
.Lx86_push_flags_done\@:
	str x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	finish_store (\width / 8)
.Lx86_push_flags_cross\@:
	mov x0, _context
	mov x1, x25
	mov x2, x24
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	b .Lx86_push_flags_done\@
.endm

push_flags 16, strh, w
push_flags 64, str, x

.purgem push_flags

.macro pop_flags width, load, host_register
gadget pop_flags_\width
	ldr x22, [_stream], #8
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	mov x7, x25
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_pop_flags_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
.Lx86_pop_flags_done\@:
	bl uml_x86_64_gadget_materialize_flags
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	mov x10, #0x4dd5
	.if \width == 64
	movk x10, #0x24, lsl #16
	.endif
	bic x9, x9, x10
	and x24, x24, x10
	orr x9, x9, x24
	orr x9, x9, #0x2
	str x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	add x9, x25, #(\width / 8)
	str x9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	retire_and_next
.Lx86_pop_flags_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
	b .Lx86_pop_flags_done\@
.endm

pop_flags 16, ldrh, w
pop_flags 64, ldr, x

.purgem pop_flags

.macro leave width, load, store, host_register
gadget leave_\width
	ldr x22, [_stream], #8
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 5 * 8)]
	mov x7, x25
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_leave_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
.Lx86_leave_done\@:
	add x9, x25, #(\width / 8)
	str x9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	\store \host_register\()24, [_cpu, #(UML_X86_64_CPU_REGISTERS + 5 * 8)]
	retire_and_next
.Lx86_leave_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
	b .Lx86_leave_done\@
.endm

leave 16, ldrh, strh, w
leave 64, ldr, str, x

.purgem leave

.macro push_memory width, load, load_register, store, store_register
gadget push_\width\()_m
	effective_address
	mov x26, x7
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_push_memory_source_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \load_register\()24, [x7]
	b .Lx86_push_memory_source_done\@
.Lx86_push_memory_source_cross\@:
	mov x0, _context
	mov x1, x26
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
.Lx86_push_memory_source_done\@:
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	sub x25, x25, #(\width / 8)
	mov x7, x25
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_push_memory_stack_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \store_register\()24, [x7]
.Lx86_push_memory_done\@:
	str x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	finish_store (\width / 8)
.Lx86_push_memory_stack_cross\@:
	mov x0, _context
	mov x1, x25
	mov x2, x24
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	b .Lx86_push_memory_done\@
.endm

push_memory 16, ldrh, w, strh, w
push_memory 64, ldr, x, str, x

.purgem push_memory

.macro pop_memory width, load, load_register, store, store_register
gadget pop_\width\()_m
	ldr x22, [_stream, #16]
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	mov x7, x25
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_pop_memory_stack_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \load_register\()24, [x7]
	b .Lx86_pop_memory_stack_done\@
.Lx86_pop_memory_stack_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
.Lx86_pop_memory_stack_done\@:
	add x26, x25, #(\width / 8)
	str x26, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	effective_address
	str x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	mov x27, x7
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_pop_memory_destination_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \store_register\()24, [x7]
.Lx86_pop_memory_done\@:
	str x26, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	mov x25, x27
	finish_store (\width / 8)
.Lx86_pop_memory_destination_cross\@:
	mov x0, _context
	mov x1, x27
	mov x2, x24
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	b .Lx86_pop_memory_done\@
.endm

pop_memory 16, ldrh, w, strh, w
pop_memory 64, ldr, x, str, x

.purgem pop_memory

.macro invalidate_stack_write
	ldr x8, [_context, #UML_X86_64_CONTEXT_PROGRAM_END]
	cmp x25, x8
	b.hs .Lx86_stack_write_ready\@
	add x9, x25, #8
	ldr x10, [_context, #UML_X86_64_CONTEXT_PROGRAM_BEGIN]
	cmp x9, x10
	b.ls .Lx86_stack_write_ready\@
	mov x0, _context
	mov x1, x25
	bl NAME(uml_x86_64_gadget_invalidate_code)
.Lx86_stack_write_ready\@:
.endm

.macro call_push_and_exit
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	sub x25, x25, #8
	mov x7, x25
	add x9, x7, #7
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_call_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, 8
	str x24, [x7]
.Lx86_call_done\@:
	str x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	invalidate_stack_write
	str x26, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.Lx86_call_cross\@:
	mov x0, _context
	mov x1, x25
	mov x2, x24
	mov w3, #8
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	b .Lx86_call_done\@
.endm

gadget call
	ldp x26, x24, [_stream], #16
	ldr x22, [_stream], #8
	call_push_and_exit

gadget call_indirect_r
	ldp x8, x24, [_stream], #16
	ldr x22, [_stream], #8
	ldr x26, [_cpu, x8]
	call_push_and_exit

gadget call_indirect_m
	effective_address
	mov x25, x7
	add x9, x7, #7
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_call_indirect_source_cross
	translate 0, UML_X86_64_TLB_READ_PAGE, 8
	ldr x26, [x7]
	b .Lx86_call_indirect_source_done
.Lx86_call_indirect_source_cross:
	mov x0, _context
	mov x1, x25
	mov w2, #8
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x26, x0
.Lx86_call_indirect_source_done:
	ldr x24, [_stream], #8
	call_push_and_exit

.purgem call_push_and_exit
.purgem invalidate_stack_write

gadget return
	ldp x23, x22, [_stream], #16
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	mov x7, x25
	add x9, x7, #7
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_return_cross
	translate 0, UML_X86_64_TLB_READ_PAGE, 8
	ldr x24, [x7]
.Lx86_return_done:
	add x9, x25, #8
	add x9, x9, x23
	str x9, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	str x24, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.Lx86_return_cross:
	mov x0, _context
	mov x1, x25
	mov w2, #8
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
	b .Lx86_return_done

gadget jump_indirect_r
	ldr x8, [_stream]
	ldr x8, [_cpu, x8]
	str x8, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave

gadget jump_indirect_m
	effective_address
	mov x25, x7
	add x9, x7, #7
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_jump_indirect_cross
	translate 0, UML_X86_64_TLB_READ_PAGE, 8
	ldr x24, [x7]
	b .Lx86_jump_indirect_done
.Lx86_jump_indirect_cross:
	mov x0, _context
	mov x1, x25
	mov w2, #8
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
.Lx86_jump_indirect_done:
	str x24, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave

.macro scalar_extend name, load, load_register, store, store_register
gadget \name
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \load_register\()11, [_cpu, x10]
	\store \store_register\()11, [_cpu, x9]
	retire_and_next
.endm

scalar_extend zext_8_16_rr, ldrb, w, strh, w
scalar_extend zext_8_32_rr, ldrb, w, str, x
scalar_extend zext_8_64_rr, ldrb, w, str, x
scalar_extend zext_16_32_rr, ldrh, w, str, x
scalar_extend zext_16_64_rr, ldrh, w, str, x
scalar_extend zext_32_64_rr, ldr, w, str, x
scalar_extend sext_8_16_rr, ldrsb, w, strh, w
scalar_extend sext_8_32_rr, ldrsb, w, str, x
scalar_extend sext_8_64_rr, ldrsb, x, str, x
scalar_extend sext_16_32_rr, ldrsh, w, str, x
scalar_extend sext_16_64_rr, ldrsh, x, str, x
scalar_extend sext_32_16_rr, ldr, w, strh, w
scalar_extend sext_32_32_rr, ldr, w, str, x
scalar_extend sext_32_64_rr, ldrsw, x, str, x

.purgem scalar_extend

.macro cross_zext
.endm
.macro cross_sxtb_w
	sxtb w11, w11
.endm
.macro cross_sxtb_x
	sxtb x11, w11
.endm
.macro cross_sxth_w
	sxth w11, w11
.endm
.macro cross_sxth_x
	sxth x11, w11
.endm
.macro cross_sxtw_x
	sxtw x11, w11
.endm

.macro memory_extend name, source_width, load, load_register, cross_extend, destination_store, destination_register
gadget \name
	effective_address
	mov x25, x7
	add x9, x7, #(\source_width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_extend_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\source_width / 8)
	\load \load_register\()11, [x7]
	b .Lx86_extend_write\@
.Lx86_extend_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\source_width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x11, x0
	\cross_extend
.Lx86_extend_write\@:
	and x8, x23, #0xffff
	\destination_store \destination_register\()11, [_cpu, x8]
	retire_and_next
.endm

memory_extend zext_8_16_rm, 8, ldrb, w, cross_zext, strh, w
memory_extend zext_8_32_rm, 8, ldrb, w, cross_zext, str, x
memory_extend zext_8_64_rm, 8, ldrb, w, cross_zext, str, x
memory_extend zext_16_32_rm, 16, ldrh, w, cross_zext, str, x
memory_extend zext_16_64_rm, 16, ldrh, w, cross_zext, str, x
memory_extend zext_32_64_rm, 32, ldr, w, cross_zext, str, x
memory_extend sext_8_16_rm, 8, ldrsb, w, cross_sxtb_w, strh, w
memory_extend sext_8_32_rm, 8, ldrsb, w, cross_sxtb_w, str, x
memory_extend sext_8_64_rm, 8, ldrsb, x, cross_sxtb_x, str, x
memory_extend sext_16_32_rm, 16, ldrsh, w, cross_sxth_w, str, x
memory_extend sext_16_64_rm, 16, ldrsh, x, cross_sxth_x, str, x
memory_extend sext_32_16_rm, 32, ldr, w, cross_zext, strh, w
memory_extend sext_32_32_rm, 32, ldr, w, cross_zext, str, x
memory_extend sext_32_64_rm, 32, ldrsw, x, cross_sxtw_x, str, x

.purgem memory_extend
.purgem cross_sxtw_x
.purgem cross_sxth_x
.purgem cross_sxth_w
.purgem cross_sxtb_x
.purgem cross_sxtb_w
.purgem cross_zext
