/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Condition evaluation and direct conditional/unconditional control flow. */
.macro evaluate_condition condition
	ldr x10, [_cpu, #UML_X86_64_CPU_RFLAGS]
	.ifc \condition,o
	ubfx w11, w10, #11, #1
	.endif
	.ifc \condition,no
	ubfx w11, w10, #11, #1
	eor w11, w11, #1
	.endif
	.ifc \condition,b
	and w11, w10, #1
	.endif
	.ifc \condition,ae
	and w11, w10, #1
	eor w11, w11, #1
	.endif
	.ifc \condition,e
	ubfx w11, w10, #6, #1
	.endif
	.ifc \condition,ne
	ubfx w11, w10, #6, #1
	eor w11, w11, #1
	.endif
	.ifc \condition,be
	and w11, w10, #1
	ubfx w12, w10, #6, #1
	orr w11, w11, w12
	.endif
	.ifc \condition,a
	and w11, w10, #1
	ubfx w12, w10, #6, #1
	orr w11, w11, w12
	eor w11, w11, #1
	.endif
	.ifc \condition,s
	ubfx w11, w10, #7, #1
	.endif
	.ifc \condition,ns
	ubfx w11, w10, #7, #1
	eor w11, w11, #1
	.endif
	.ifc \condition,p
	ubfx w11, w10, #2, #1
	.endif
	.ifc \condition,np
	ubfx w11, w10, #2, #1
	eor w11, w11, #1
	.endif
	.ifc \condition,l
	ubfx w11, w10, #7, #1
	ubfx w12, w10, #11, #1
	eor w11, w11, w12
	.endif
	.ifc \condition,ge
	ubfx w11, w10, #7, #1
	ubfx w12, w10, #11, #1
	eor w11, w11, w12
	eor w11, w11, #1
	.endif
	.ifc \condition,le
	ubfx w11, w10, #7, #1
	ubfx w12, w10, #11, #1
	eor w11, w11, w12
	ubfx w12, w10, #6, #1
	orr w11, w11, w12
	.endif
	.ifc \condition,g
	ubfx w11, w10, #7, #1
	ubfx w12, w10, #11, #1
	eor w11, w11, w12
	ubfx w12, w10, #6, #1
	orr w11, w11, w12
	eor w11, w11, #1
	.endif
.endm

.macro evaluate_deferred_zero condition
	ldrh w10, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	tbz w10, #6, .Lx86_zero_from_deferred\@
	ldrh w10, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_VALUE]
	ubfx w11, w10, #6, #1
	.ifc \condition,ne
	eor w11, w11, #1
	.endif
	b .Lx86_zero_ready\@
.Lx86_zero_from_deferred\@:
	ldrb w10, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	cbz w10, .Lx86_zero_from_flags\@
	ldr x10, [_cpu, #UML_X86_64_CPU_DEFERRED_RESULT]
	cmp x10, #0
	.ifc \condition,e
	cset w11, eq
	.else
	cset w11, ne
	.endif
	b .Lx86_zero_ready\@
.Lx86_zero_from_flags\@:
	ldr x10, [_cpu, #UML_X86_64_CPU_RFLAGS]
	ubfx w11, w10, #6, #1
	.ifc \condition,ne
	eor w11, w11, #1
	.endif
.Lx86_zero_ready\@:
.endm

gadget link_exit
	ldr x9, [_stream]
	str x9, [_cpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave

.macro set_condition condition
gadget set_\condition
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_stream], #8
	evaluate_condition \condition
	strb w11, [_cpu, x8]
	retire_and_next
.endm

.macro set_condition_memory condition
gadget set_\condition\()_m
	bl uml_x86_64_gadget_materialize_flags
	effective_address
	mov x25, x7
	translate 1, UML_X86_64_TLB_WRITE_PAGE, 1
	evaluate_condition \condition
	strb w11, [x7]
	finish_store 1
.endm

.macro conditional_move condition, width, load, store, load_register, store_register
gadget cmov_\condition\()_\width\()_rr
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x13, x8, #16, #16
	evaluate_condition \condition
	cbz w11, .Lx86_cmov_done\@
	\load \load_register\()12, [_cpu, x13]
	\store \store_register\()12, [_cpu, x9]
.Lx86_cmov_done\@:
	retire_and_next
.endm

.macro conditional_move_memory condition, width, load, host_register, store, store_register
gadget cmov_\condition\()_\width\()_rm
	effective_address
	mov x25, x7
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_cmov_memory_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
	b .Lx86_cmov_memory_loaded\@
.Lx86_cmov_memory_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
.Lx86_cmov_memory_loaded\@:
	bl uml_x86_64_gadget_materialize_flags
	evaluate_condition \condition
	cbz w11, .Lx86_cmov_memory_done\@
	and x8, x23, #0xffff
	\store \store_register\()24, [_cpu, x8]
.Lx86_cmov_memory_done\@:
	retire_and_next
.endm

.macro jump_condition condition
gadget jump_\condition
	.ifc \condition,e
	evaluate_deferred_zero \condition
	.else
	.ifc \condition,ne
	evaluate_deferred_zero \condition
	.else
	bl uml_x86_64_gadget_materialize_flags
	evaluate_condition \condition
	.endif
	.endif
	ldp x12, x13, [_stream], #16
	cmp w11, #0
	csel x8, x12, x13, ne
	str x8, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.endm

.macro jump_condition_linked condition
gadget jump_\condition\()_linked
	.ifc \condition,e
	evaluate_deferred_zero \condition
	.else
	.ifc \condition,ne
	evaluate_deferred_zero \condition
	.else
	bl uml_x86_64_gadget_materialize_flags
	evaluate_condition \condition
	.endif
	.endif
	ldp x12, x13, [_stream], #16
	cmp w11, #0
	csel x8, x12, x13, ne
	add _executed, _executed, #1
	follow_block_link x8
.endm

.macro condition_family condition
set_condition \condition
set_condition_memory \condition
conditional_move \condition, 16, ldrh, strh, w, w
conditional_move \condition, 32, ldr, str, w, x
conditional_move \condition, 64, ldr, str, x, x
conditional_move_memory \condition, 16, ldrh, w, strh, w
conditional_move_memory \condition, 32, ldr, w, str, x
conditional_move_memory \condition, 64, ldr, x, str, x
jump_condition \condition
jump_condition_linked \condition
.endm

condition_family o
condition_family no
condition_family b
condition_family ae
condition_family e
condition_family ne
condition_family be
condition_family a
condition_family s
condition_family ns
condition_family p
condition_family np
condition_family l
condition_family ge
condition_family le
condition_family g

.purgem condition_family
.purgem jump_condition_linked
.purgem jump_condition
.purgem conditional_move_memory
.purgem conditional_move
.purgem set_condition_memory
.purgem set_condition
.purgem evaluate_deferred_zero
.purgem evaluate_condition

gadget jump
	ldr x8, [_stream]
	str x8, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave

gadget jump_linked
	ldr x8, [_stream]
	add _executed, _executed, #1
	follow_block_link x8

.purgem follow_block_link
.purgem follow_indirect_link

.macro loop_gadget opcode, width, host_register
gadget loop_\opcode\()_\width
	ldp x26, x27, [_stream]
	.if \width == 16
	ldrh w24, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	.else
	ldr \host_register\()24, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	.endif
	.ifc \opcode,e3
	cbz \host_register\()24, .Lx86_loop_taken\@
	b .Lx86_loop_fallthrough\@
	.else
	sub \host_register\()24, \host_register\()24, #1
	.if \width == 16
	ldr x8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	bfi x8, x24, #0, #16
	str x8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	.else
	str x24, [_cpu, #(UML_X86_64_CPU_REGISTERS + 1 * 8)]
	.endif
	.ifc \opcode,e2
	cbnz \host_register\()24, .Lx86_loop_taken\@
	b .Lx86_loop_fallthrough\@
	.else
	bl uml_x86_64_gadget_materialize_flags
	cbz \host_register\()24, .Lx86_loop_fallthrough\@
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	.ifc \opcode,e1
	tbz x8, #6, .Lx86_loop_fallthrough\@
	.else
	tbnz x8, #6, .Lx86_loop_fallthrough\@
	.endif
	b .Lx86_loop_taken\@
	.endif
	.endif
.Lx86_loop_taken\@:
	str x26, [_cpu, #UML_X86_64_CPU_RIP]
	b .Lx86_loop_leave\@
.Lx86_loop_fallthrough\@:
	str x27, [_cpu, #UML_X86_64_CPU_RIP]
.Lx86_loop_leave\@:
	add _executed, _executed, #1
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.endm

.macro loop_family opcode
	loop_gadget \opcode, 16, w
	loop_gadget \opcode, 32, w
	loop_gadget \opcode, 64, x
.endm

loop_family e0
loop_family e1
loop_family e2
loop_family e3

.purgem loop_family
.purgem loop_gadget
