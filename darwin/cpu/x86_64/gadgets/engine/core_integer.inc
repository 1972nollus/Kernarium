/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scalar arithmetic, logical, unary, byte-order, and flag operations. */
.macro normalize width, destination, source
	.if \width == 8
	and \destination, \source, #0xff
	.elseif \width == 16
	uxth \destination, \source
	.elseif \width == 32
	mov \destination, \source
	.elseif \width == 64
	mov \destination, \source
	.endif
.endm

.macro record_deferred operation, width
	add x15, _cpu, #UML_X86_64_CPU_DEFERRED_LEFT
	stp x11, x12, [x15]
	str x13, [x15, #16]
	mov w14, #\operation
	strb w14, [x15, #24]
	/* DeferredFlags stores operand bytes, while assembly specialization names
	 * widths in bits.
	 */
	mov w14, #(\width / 8)
	strb w14, [x15, #25]
	strb wzr, [x15, #26]
.endm

.macro scalar_arithmetic_rr name, instruction, flag_operation, writes, load, store, host_register, store_register, width
gadget \name
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \host_register\()11, [_cpu, x9]
	\load \host_register\()12, [_cpu, x10]
	\instruction \host_register\()13, \host_register\()11, \host_register\()12
	normalize \width, \host_register\()13, \host_register\()13
	.if \writes
	\store \store_register\()13, [_cpu, x9]
	.endif
	record_deferred \flag_operation, \width
	retire_and_next
.endm

.macro scalar_arithmetic_ri name, instruction, flag_operation, writes, load, store, host_register, store_register, width
gadget \name
	ldp x8, x12, [_stream], #16
	and x9, x8, #0xffff
	\load \host_register\()11, [_cpu, x9]
	normalize \width, \host_register\()12, \host_register\()12
	\instruction \host_register\()13, \host_register\()11, \host_register\()12
	normalize \width, \host_register\()13, \host_register\()13
	.if \writes
	\store \store_register\()13, [_cpu, x9]
	.endif
	record_deferred \flag_operation, \width
	retire_and_next
.endm

.macro arithmetic_width operation, instruction, flag_operation, writes, width, load, store, host_register, store_register
scalar_arithmetic_rr \operation\()_\width\()_rr, \instruction, \flag_operation, \writes, \load, \store, \host_register, \store_register, \width
scalar_arithmetic_ri \operation\()_\width\()_ri, \instruction, \flag_operation, \writes, \load, \store, \host_register, \store_register, \width
.endm

.macro arithmetic_family operation, instruction, flag_operation, writes
arithmetic_width \operation, \instruction, \flag_operation, \writes, 8, ldrb, strb, w, w
arithmetic_width \operation, \instruction, \flag_operation, \writes, 16, ldrh, strh, w, w
arithmetic_width \operation, \instruction, \flag_operation, \writes, 32, ldr, str, w, x
arithmetic_width \operation, \instruction, \flag_operation, \writes, 64, ldr, str, x, x
.endm

/* DeferredFlagOperation: add=1, subtract=2, logical=3. */
arithmetic_family add, add, 1, 1
arithmetic_family sub, sub, 2, 1
arithmetic_family and, and, 3, 1
arithmetic_family or, orr, 3, 1
arithmetic_family xor, eor, 3, 1
arithmetic_family cmp, sub, 2, 0
arithmetic_family test, and, 3, 0

.macro scalar_carry_rr name, instruction, flag_operation, width, load, store, host_register, store_register
gadget \name\()_\width\()_rr
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \host_register\()11, [_cpu, x9]
	\load \host_register\()12, [_cpu, x10]
	ldr x16, [_cpu, #UML_X86_64_CPU_RFLAGS]
	and w16, w16, #1
	\instruction \host_register\()13, \host_register\()11, \host_register\()12
	\instruction \host_register\()13, \host_register\()13, \host_register\()16
	normalize \width, \host_register\()13, \host_register\()13
	\store \store_register\()13, [_cpu, x9]
	record_deferred \flag_operation, \width
	strb w16, [_cpu, #UML_X86_64_CPU_DEFERRED_CARRY]
	retire_and_next
.endm

.macro scalar_carry_ri name, instruction, flag_operation, width, load, store, host_register, store_register
gadget \name\()_\width\()_ri
	bl uml_x86_64_gadget_materialize_flags
	ldp x8, x12, [_stream], #16
	and x9, x8, #0xffff
	\load \host_register\()11, [_cpu, x9]
	normalize \width, \host_register\()12, \host_register\()12
	ldr x16, [_cpu, #UML_X86_64_CPU_RFLAGS]
	and w16, w16, #1
	\instruction \host_register\()13, \host_register\()11, \host_register\()12
	\instruction \host_register\()13, \host_register\()13, \host_register\()16
	normalize \width, \host_register\()13, \host_register\()13
	\store \store_register\()13, [_cpu, x9]
	record_deferred \flag_operation, \width
	strb w16, [_cpu, #UML_X86_64_CPU_DEFERRED_CARRY]
	retire_and_next
.endm

.macro scalar_carry_width name, instruction, flag_operation, width, load, store, host_register, store_register
	scalar_carry_rr \name, \instruction, \flag_operation, \width, \load, \store, \host_register, \store_register
	scalar_carry_ri \name, \instruction, \flag_operation, \width, \load, \store, \host_register, \store_register
.endm

.macro scalar_carry_family name, instruction, flag_operation
	scalar_carry_width \name, \instruction, \flag_operation, 8, ldrb, strb, w, w
	scalar_carry_width \name, \instruction, \flag_operation, 16, ldrh, strh, w, w
	scalar_carry_width \name, \instruction, \flag_operation, 32, ldr, str, w, x
	scalar_carry_width \name, \instruction, \flag_operation, 64, ldr, str, x, x
.endm

scalar_carry_family adc, add, 6
scalar_carry_family sbb, sub, 7

.purgem scalar_carry_family
.purgem scalar_carry_width
.purgem scalar_carry_ri
.purgem scalar_carry_rr

.purgem arithmetic_family
.purgem arithmetic_width
.purgem scalar_arithmetic_ri
.purgem scalar_arithmetic_rr

.macro scalar_exchange width, load, store, load_register, store_register
gadget exchange_\width\()_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \load_register\()11, [_cpu, x9]
	\load \load_register\()12, [_cpu, x10]
	\store \store_register\()12, [_cpu, x9]
	\store \store_register\()11, [_cpu, x10]
	retire_and_next
.endm

scalar_exchange 8, ldrb, strb, w, w
scalar_exchange 16, ldrh, strh, w, w
scalar_exchange 32, ldr, str, w, x
scalar_exchange 64, ldr, str, x, x

.purgem scalar_exchange

.macro scalar_unary name, instruction, flag_operation, materialize, width, load, store, host_register, store_register
gadget \name\()_\width\()_r
	.if \materialize
	bl uml_x86_64_gadget_materialize_flags
	.endif
	ldr x8, [_stream], #8
	\load \host_register\()11, [_cpu, x8]
	.ifc \instruction,add
	add \host_register\()13, \host_register\()11, #1
	mov x12, #1
	.endif
	.ifc \instruction,sub
	sub \host_register\()13, \host_register\()11, #1
	mov x12, #1
	.endif
	.ifc \instruction,neg
	neg \host_register\()13, \host_register\()11
	mov x12, x11
	mov x11, xzr
	.endif
	normalize \width, \host_register\()13, \host_register\()13
	\store \store_register\()13, [_cpu, x8]
	record_deferred \flag_operation, \width
	retire_and_next
.endm

.macro scalar_not width, load, store, host_register, store_register
gadget not_\width\()_r
	ldr x8, [_stream], #8
	\load \host_register\()11, [_cpu, x8]
	mvn \host_register\()11, \host_register\()11
	\store \store_register\()11, [_cpu, x8]
	retire_and_next
.endm

.macro unary_width width, load, store, host_register, store_register
scalar_unary increment, add, 4, 1, \width, \load, \store, \host_register, \store_register
scalar_unary decrement, sub, 5, 1, \width, \load, \store, \host_register, \store_register
scalar_unary negate, neg, 2, 0, \width, \load, \store, \host_register, \store_register
scalar_not \width, \load, \store, \host_register, \store_register
.endm

unary_width 8, ldrb, strb, w, w
unary_width 16, ldrh, strh, w, w
unary_width 32, ldr, str, w, x
unary_width 64, ldr, str, x, x

.macro memory_arithmetic_rm operation, instruction, flag_operation, writes, width, load, store, host_register, store_register
gadget \operation\()_\width\()_rm
	effective_address
	mov x25, x7
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_arithmetic_rm_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()12, [x7]
.Lx86_arithmetic_rm_loaded\@:
	and x8, x23, #0xffff
	\load \host_register\()11, [_cpu, x8]
	\instruction \host_register\()13, \host_register\()11, \host_register\()12
	normalize \width, \host_register\()13, \host_register\()13
	.if \writes
	\store \store_register\()13, [_cpu, x8]
	.endif
	record_deferred \flag_operation, \width
	retire_and_next
.Lx86_arithmetic_rm_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x12, x0
	b .Lx86_arithmetic_rm_loaded\@
.endm

.macro memory_arithmetic_msource operation, instruction, flag_operation, writes, form, width, load, store, host_register
gadget \operation\()_\width\()_m\form
	effective_address
	mov x25, x7
	.ifc \form,r
	and x8, x23, #0xffff
	\load \host_register\()26, [_cpu, x8]
	.endif
	.ifc \form,i
	ldr x26, [_stream], #8
	normalize \width, \host_register\()26, \host_register\()26
	.endif
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_arithmetic_msource_read_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
.Lx86_arithmetic_msource_read\@:
	\instruction \host_register\()27, \host_register\()24, \host_register\()26
	normalize \width, \host_register\()27, \host_register\()27
	.if !\writes
	mov x11, x24
	mov x12, x26
	mov x13, x27
	record_deferred \flag_operation, \width
	retire_and_next
	.endif
	.if \writes
	mov x7, x25
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_arithmetic_msource_write_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()27, [x7]
.Lx86_arithmetic_msource_written\@:
	mov x11, x24
	mov x12, x26
	mov x13, x27
	record_deferred \flag_operation, \width
	finish_store (\width / 8)
.Lx86_arithmetic_msource_write_cross\@:
	mov x0, _context
	mov x1, x25
	mov x2, x27
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	b .Lx86_arithmetic_msource_written\@
	.endif
.Lx86_arithmetic_msource_read_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
	b .Lx86_arithmetic_msource_read\@
.endm

.macro memory_arithmetic_width operation, instruction, flag_operation, writes, width, load, store, host_register, store_register
memory_arithmetic_rm \operation, \instruction, \flag_operation, \writes, \width, \load, \store, \host_register, \store_register
memory_arithmetic_msource \operation, \instruction, \flag_operation, \writes, r, \width, \load, \store, \host_register
memory_arithmetic_msource \operation, \instruction, \flag_operation, \writes, i, \width, \load, \store, \host_register
.endm

.macro memory_arithmetic_family operation, instruction, flag_operation, writes
memory_arithmetic_width \operation, \instruction, \flag_operation, \writes, 8, ldrb, strb, w, w
memory_arithmetic_width \operation, \instruction, \flag_operation, \writes, 16, ldrh, strh, w, w
memory_arithmetic_width \operation, \instruction, \flag_operation, \writes, 32, ldr, str, w, x
memory_arithmetic_width \operation, \instruction, \flag_operation, \writes, 64, ldr, str, x, x
.endm

memory_arithmetic_family add, add, 1, 1
memory_arithmetic_family sub, sub, 2, 1
memory_arithmetic_family and, and, 3, 1
memory_arithmetic_family or, orr, 3, 1
memory_arithmetic_family xor, eor, 3, 1
memory_arithmetic_family cmp, sub, 2, 0
memory_arithmetic_family test, and, 3, 0

.macro memory_carry_rm name, instruction, flag_operation, width, load, store, host_register, store_register
gadget \name\()_\width\()_rm
	bl uml_x86_64_gadget_materialize_flags
	ldr x19, [_cpu, #UML_X86_64_CPU_RFLAGS]
	and w19, w19, #1
	effective_address
	mov x25, x7
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_carry_rm_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()12, [x7]
	b .Lx86_carry_rm_loaded\@
.Lx86_carry_rm_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x12, x0
.Lx86_carry_rm_loaded\@:
	and x8, x23, #0xffff
	\load \host_register\()11, [_cpu, x8]
	\instruction \host_register\()13, \host_register\()11, \host_register\()12
	\instruction \host_register\()13, \host_register\()13, \host_register\()19
	normalize \width, \host_register\()13, \host_register\()13
	\store \store_register\()13, [_cpu, x8]
	record_deferred \flag_operation, \width
	strb w19, [_cpu, #UML_X86_64_CPU_DEFERRED_CARRY]
	retire_and_next
.endm

.macro memory_carry_msource name, instruction, flag_operation, form, width, load, store, host_register
gadget \name\()_\width\()_m\form
	bl uml_x86_64_gadget_materialize_flags
	ldr x19, [_cpu, #UML_X86_64_CPU_RFLAGS]
	and w19, w19, #1
	effective_address
	mov x25, x7
	.ifc \form,r
	and x8, x23, #0xffff
	\load \host_register\()26, [_cpu, x8]
	.endif
	.ifc \form,i
	ldr x26, [_stream], #8
	normalize \width, \host_register\()26, \host_register\()26
	.endif
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_carry_msource_read_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
	b .Lx86_carry_msource_read\@
.Lx86_carry_msource_read_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
.Lx86_carry_msource_read\@:
	\instruction \host_register\()27, \host_register\()24, \host_register\()26
	\instruction \host_register\()27, \host_register\()27, \host_register\()19
	normalize \width, \host_register\()27, \host_register\()27
	mov x7, x25
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_carry_msource_write_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()27, [x7]
	b .Lx86_carry_msource_written\@
.Lx86_carry_msource_write_cross\@:
	mov x0, _context
	mov x1, x25
	mov x2, x27
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
.Lx86_carry_msource_written\@:
	mov x11, x24
	mov x12, x26
	mov x13, x27
	record_deferred \flag_operation, \width
	strb w19, [_cpu, #UML_X86_64_CPU_DEFERRED_CARRY]
	finish_store (\width / 8)
.endm

.macro memory_carry_width name, instruction, flag_operation, width, load, store, host_register, store_register
	memory_carry_rm \name, \instruction, \flag_operation, \width, \load, \store, \host_register, \store_register
	memory_carry_msource \name, \instruction, \flag_operation, r, \width, \load, \store, \host_register
	memory_carry_msource \name, \instruction, \flag_operation, i, \width, \load, \store, \host_register
.endm

.macro memory_carry_family name, instruction, flag_operation
	memory_carry_width \name, \instruction, \flag_operation, 8, ldrb, strb, w, w
	memory_carry_width \name, \instruction, \flag_operation, 16, ldrh, strh, w, w
	memory_carry_width \name, \instruction, \flag_operation, 32, ldr, str, w, x
	memory_carry_width \name, \instruction, \flag_operation, 64, ldr, str, x, x
.endm

memory_carry_family adc, add, 6
memory_carry_family sbb, sub, 7

.purgem memory_carry_family
.purgem memory_carry_width
.purgem memory_carry_msource
.purgem memory_carry_rm

.macro memory_exchange width, load, store, host_register, register_store, register_store_register
gadget exchange_\width\()_mr
	effective_address
	mov x25, x7
	and x8, x23, #0xffff
	\load \host_register\()26, [_cpu, x8]
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_exchange_read_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
	b .Lx86_exchange_loaded\@
.Lx86_exchange_read_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
.Lx86_exchange_loaded\@:
	mov x7, x25
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_exchange_write_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()26, [x7]
	b .Lx86_exchange_written\@
.Lx86_exchange_write_cross\@:
	mov x0, _context
	mov x1, x25
	mov x2, x26
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
.Lx86_exchange_written\@:
	and x8, x23, #0xffff
	\register_store \register_store_register\()24, [_cpu, x8]
	finish_store (\width / 8)
.endm

memory_exchange 8, ldrb, strb, w, strb, w
memory_exchange 16, ldrh, strh, w, strh, w
memory_exchange 32, ldr, str, w, str, x
memory_exchange 64, ldr, str, x, str, x

.purgem memory_exchange

.macro memory_unary operation, instruction, flag_operation, materialize, width, load, store, host_register
gadget \operation\()_\width\()_m
	effective_address
	mov x25, x7
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_unary_read_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
	b .Lx86_unary_loaded\@
.Lx86_unary_read_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
.Lx86_unary_loaded\@:
	.if \materialize
	bl uml_x86_64_gadget_materialize_flags
	.endif
	.ifc \instruction,add
	add \host_register\()27, \host_register\()24, #1
	.endif
	.ifc \instruction,sub
	sub \host_register\()27, \host_register\()24, #1
	.endif
	.ifc \instruction,neg
	neg \host_register\()27, \host_register\()24
	.endif
	.ifc \instruction,not
	mvn \host_register\()27, \host_register\()24
	.endif
	normalize \width, \host_register\()27, \host_register\()27
	mov x7, x25
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_unary_write_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()27, [x7]
	b .Lx86_unary_written\@
.Lx86_unary_write_cross\@:
	mov x0, _context
	mov x1, x25
	mov x2, x27
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
.Lx86_unary_written\@:
	.if \flag_operation
	.ifc \instruction,neg
	mov x11, xzr
	mov x12, x24
	.else
	mov x11, x24
	mov x12, #1
	.endif
	mov x13, x27
	record_deferred \flag_operation, \width
	.endif
	finish_store (\width / 8)
.endm

.macro memory_unary_width width, load, store, host_register
	memory_unary increment, add, 4, 1, \width, \load, \store, \host_register
	memory_unary decrement, sub, 5, 1, \width, \load, \store, \host_register
	memory_unary negate, neg, 2, 0, \width, \load, \store, \host_register
	memory_unary not, not, 0, 0, \width, \load, \store, \host_register
.endm

memory_unary_width 8, ldrb, strb, w
memory_unary_width 16, ldrh, strh, w
memory_unary_width 32, ldr, str, w
memory_unary_width 64, ldr, str, x

.purgem memory_unary_width
.purgem memory_unary

.purgem memory_arithmetic_family
.purgem memory_arithmetic_width
.purgem memory_arithmetic_msource
.purgem memory_arithmetic_rm

.purgem unary_width
.purgem scalar_not
.purgem scalar_unary
.purgem record_deferred
.purgem normalize

gadget byte_swap_32
	ldr x8, [_stream], #8
	ldr w9, [_cpu, x8]
	rev w9, w9
	str x9, [_cpu, x8]
	retire_and_next

gadget byte_swap_64
	ldr x8, [_stream], #8
	ldr x9, [_cpu, x8]
	rev x9, x9
	str x9, [_cpu, x8]
	retire_and_next

gadget sign_extend_accumulator_16
	ldrsb w8, [_cpu, #UML_X86_64_CPU_REGISTERS]
	strh w8, [_cpu, #UML_X86_64_CPU_REGISTERS]
	retire_and_next

gadget sign_extend_accumulator_32
	ldrsh w8, [_cpu, #UML_X86_64_CPU_REGISTERS]
	str x8, [_cpu, #UML_X86_64_CPU_REGISTERS]
	retire_and_next

gadget sign_extend_accumulator_64
	ldrsw x8, [_cpu, #UML_X86_64_CPU_REGISTERS]
	str x8, [_cpu, #UML_X86_64_CPU_REGISTERS]
	retire_and_next

gadget sign_extend_data_16
	ldrsh w8, [_cpu, #UML_X86_64_CPU_REGISTERS]
	asr w8, w8, #15
	strh w8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	retire_and_next

gadget sign_extend_data_32
	ldr w8, [_cpu, #UML_X86_64_CPU_REGISTERS]
	asr w8, w8, #31
	str x8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	retire_and_next

gadget sign_extend_data_64
	ldr x8, [_cpu, #UML_X86_64_CPU_REGISTERS]
	asr x8, x8, #63
	str x8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	retire_and_next

.macro flag_control name, instruction, mask
gadget \name
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	\instruction x8, x8, #\mask
	orr x8, x8, #0x2
	str x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	retire_and_next
.endm

flag_control carry_clear, bic, 0x1
flag_control carry_set, orr, 0x1
flag_control carry_complement, eor, 0x1
flag_control direction_clear, bic, 0x400
flag_control direction_set, orr, 0x400

.purgem flag_control

gadget lahf
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	mov x9, #0xd5
	and x8, x8, x9
	orr x8, x8, #0x2
	ldr x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	bfi x10, x8, #8, #8
	str x10, [_cpu, #UML_X86_64_CPU_REGISTERS]
	retire_and_next

gadget sahf
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_cpu, #UML_X86_64_CPU_REGISTERS]
	ubfx x8, x8, #8, #8
	mov x9, #0xd5
	and x8, x8, x9
	ldr x10, [_cpu, #UML_X86_64_CPU_RFLAGS]
	bic x10, x10, x9
	orr x10, x10, x8
	orr x10, x10, #0x2
	str x10, [_cpu, #UML_X86_64_CPU_RFLAGS]
	retire_and_next
