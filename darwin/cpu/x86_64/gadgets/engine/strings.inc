/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Implicit-register string operations and REP/REPE/REPNE loops. */

.macro string_source_address
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 6 * 8)]
	tbnz x23, #3, .Lx86_string_source_16\@
	tbz x23, #0, .Lx86_string_source_64\@
	mov w25, w25
	b .Lx86_string_source_64\@
.Lx86_string_source_16\@:
	uxth w25, w25
.Lx86_string_source_64\@:
	ubfx w8, w23, #1, #2
	cmp w8, #1
	b.ne .Lx86_string_source_not_fs\@
	ldr x9, [_cpu, #UML_X86_64_CPU_FS_BASE]
	add x25, x25, x9
	b .Lx86_string_source_done\@
.Lx86_string_source_not_fs\@:
	cmp w8, #2
	b.ne .Lx86_string_source_done\@
	ldr x9, [_cpu, #UML_X86_64_CPU_GS_BASE]
	add x25, x25, x9
.Lx86_string_source_done\@:
.endm

.macro string_destination_address
	ldr x26, [_cpu, #(UML_X86_64_CPU_REGISTERS + 7 * 8)]
	tbnz x23, #3, .Lx86_string_destination_16\@
	tbz x23, #0, .Lx86_string_destination_64\@
	mov w26, w26
	b .Lx86_string_destination_64\@
.Lx86_string_destination_16\@:
	uxth w26, w26
.Lx86_string_destination_64\@:
.endm

.macro string_load width, load, host_register, address, destination
	mov x7, x\address
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_string_load_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()\destination, [x7]
	b .Lx86_string_load_done\@
.Lx86_string_load_cross\@:
	mov x0, _context
	mov x1, x\address
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x\destination, x0
.Lx86_string_load_done\@:
.endm

.macro string_store width, store, host_register
	mov x7, x26
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_string_store_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()27, [x7]
	b .Lx86_string_store_done\@
.Lx86_string_store_cross\@:
	mov x0, _context
	mov x1, x26
	mov x2, x27
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
.Lx86_string_store_done\@:
	ldrb w8, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cbz w8, .Lx86_string_store_check_overlap\@
	publish_pending_code x26, (\width / 8)
	orr x23, x23, #0x100000000
	b .Lx86_string_store_ready\@
.Lx86_string_store_check_overlap\@:
	ldr x8, [_context, #UML_X86_64_CONTEXT_PROGRAM_END]
	cmp x26, x8
	b.hs .Lx86_string_store_ready\@
	add x9, x26, #(\width / 8)
	ldr x10, [_context, #UML_X86_64_CONTEXT_PROGRAM_BEGIN]
	cmp x9, x10
	b.ls .Lx86_string_store_ready\@
	mov x0, _context
	mov x1, x26
	bl NAME(uml_x86_64_gadget_invalidate_code)
	orr x23, x23, #0x100000000
.Lx86_string_store_ready\@:
.endm

.macro string_update_pointer width, offset
	ldr x8, [_cpu, #(UML_X86_64_CPU_REGISTERS + \offset)]
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	tbnz x9, #10, .Lx86_string_pointer_decrement\@
	tbnz x23, #3, .Lx86_string_pointer_increment_16\@
	tbz x23, #0, .Lx86_string_pointer_increment_64\@
	add w8, w8, #(\width / 8)
	mov w8, w8
	b .Lx86_string_pointer_store\@
.Lx86_string_pointer_increment_16\@:
	add w10, w8, #(\width / 8)
	bfi x8, x10, #0, #16
	b .Lx86_string_pointer_store\@
.Lx86_string_pointer_increment_64\@:
	add x8, x8, #(\width / 8)
	b .Lx86_string_pointer_store\@
.Lx86_string_pointer_decrement\@:
	tbnz x23, #3, .Lx86_string_pointer_decrement_16\@
	tbz x23, #0, .Lx86_string_pointer_decrement_64\@
	sub w8, w8, #(\width / 8)
	mov w8, w8
	b .Lx86_string_pointer_store\@
.Lx86_string_pointer_decrement_16\@:
	sub w10, w8, #(\width / 8)
	bfi x8, x10, #0, #16
	b .Lx86_string_pointer_store\@
.Lx86_string_pointer_decrement_64\@:
	sub x8, x8, #(\width / 8)
.Lx86_string_pointer_store\@:
	str x8, [_cpu, #(UML_X86_64_CPU_REGISTERS + \offset)]
.endm


.macro string_record_deferred width
	add x15, _cpu, #UML_X86_64_CPU_DEFERRED_LEFT
	stp x11, x12, [x15]
	str x13, [x15, #16]
	mov w14, #2
	strb w14, [x15, #24]
	mov w14, #(\width / 8)
	strb w14, [x15, #25]
	strb wzr, [x15, #26]
	str wzr, [x15, #28]
.endm

.macro string_compare_flags width, host_register
	mov x11, x27
	mov x12, x19
	sub \host_register\()13, \host_register\()11, \host_register\()12
	normalize_result \width, \host_register\()13
	string_record_deferred \width
	cmp \host_register\()13, #0
	cset w19, eq
.endm

.macro string_gadget operation, width, load, store, host_register, store_register
gadget string_\operation\()_\width
	ldp x23, x22, [_stream], #16
	ubfx w8, w23, #8, #8
	cbz w8, .Lx86_string_nonrepeating\@
	/* Long REP operations must periodically return to the executor so an
	 * MMU mapping-change interrupt can release its execution read gate. */
	mov w9, #4096
	str w9, [sp, #0x68]
	tbnz x23, #3, .Lx86_string_count_16\@
	tbz x23, #0, .Lx86_string_count_64\@
	ldr w24, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	b .Lx86_string_count_ready\@
.Lx86_string_count_16\@:
	ldrh w24, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	b .Lx86_string_count_ready\@
.Lx86_string_count_64\@:
	ldr x24, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
.Lx86_string_count_ready\@:
	cbz x24, .Lx86_string_finish\@
	b .Lx86_string_iteration\@
.Lx86_string_nonrepeating\@:
	mov x24, #1
.Lx86_string_iteration\@:
	.ifc \operation,move
	string_source_address
	string_load \width, \load, \host_register, 25, 27
	string_destination_address
	string_store \width, \store, \host_register
	string_update_pointer \width, (6 * 8)
	string_update_pointer \width, (7 * 8)
	.endif
	.ifc \operation,store
	\load \host_register\()27, [_cpu, #UML_X86_64_CPU_REGISTERS]
	string_destination_address
	string_store \width, \store, \host_register
	string_update_pointer \width, (7 * 8)
	.endif
	.ifc \operation,load
	string_source_address
	string_load \width, \load, \host_register, 25, 27
	\store \store_register\()27, [_cpu, #UML_X86_64_CPU_REGISTERS]
	string_update_pointer \width, (6 * 8)
	.endif
	.ifc \operation,compare
	string_source_address
	string_load \width, \load, \host_register, 25, 27
	string_destination_address
	string_load \width, \load, \host_register, 26, 19
	string_compare_flags \width, \host_register
	string_update_pointer \width, (6 * 8)
	string_update_pointer \width, (7 * 8)
	.endif
	.ifc \operation,scan
	\load \host_register\()27, [_cpu, #UML_X86_64_CPU_REGISTERS]
	string_destination_address
	string_load \width, \load, \host_register, 26, 19
	string_compare_flags \width, \host_register
	string_update_pointer \width, (7 * 8)
	.endif
	ubfx w8, w23, #8, #8
	cbz w8, .Lx86_string_finish\@
	sub x24, x24, #1
	tbnz x23, #3, .Lx86_string_store_count_16\@
	tbz x23, #0, .Lx86_string_store_count_64\@
	mov w24, w24
.Lx86_string_store_count_64\@:
	str x24, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	b .Lx86_string_count_stored\@
.Lx86_string_store_count_16\@:
	ldr x8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	bfi x8, x24, #0, #16
	str x8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
.Lx86_string_count_stored\@:
	cbz x24, .Lx86_string_finish\@
	.ifc \operation,compare
	cmp w8, #0xf3
	b.eq .Lx86_string_repeat_equal\@
	cbnz w19, .Lx86_string_finish\@
	b .Lx86_string_repeat_continue\@
.Lx86_string_repeat_equal\@:
	cbz w19, .Lx86_string_finish\@
	.endif
	.ifc \operation,scan
	cmp w8, #0xf3
	b.eq .Lx86_string_repeat_equal\@
	cbnz w19, .Lx86_string_finish\@
	b .Lx86_string_repeat_continue\@
.Lx86_string_repeat_equal\@:
	cbz w19, .Lx86_string_finish\@
	.endif
	.Lx86_string_repeat_continue\@:
	ldr w9, [sp, #0x68]
	subs w9, w9, #1
	str w9, [sp, #0x68]
	b.ne .Lx86_string_iteration\@
	str x22, [_cpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.Lx86_string_finish\@:
	tbz x23, #32, .Lx86_string_continue\@
	ubfx x8, x23, #56, #4
	add x8, x22, x8
	str x8, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.Lx86_string_continue\@:
	retire_and_next
.endm

.macro string_width operation, width, load, store, host_register, store_register
string_gadget \operation, \width, \load, \store, \host_register, \store_register
.endm

.macro string_family operation
string_width \operation, 8, ldrb, strb, w, w
string_width \operation, 16, ldrh, strh, w, w
string_width \operation, 32, ldr, str, w, x
string_width \operation, 64, ldr, str, x, x
.endm

string_family move
string_family compare
string_family store
string_family load
string_family scan

.purgem string_family
.purgem string_width
.purgem string_gadget
.purgem string_compare_flags
.purgem string_record_deferred
.purgem string_update_pointer
.purgem string_store
.purgem string_load
.purgem string_destination_address
.purgem string_source_address
