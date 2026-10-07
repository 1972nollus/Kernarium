/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ENTER and legacy far/interrupt control transfers. */

/* ENTER keeps the stack pointer it moves in x25 and writes RSP only once
 * every access has succeeded: a fault in a push or in a nesting level's
 * frame-pointer load leaves RSP and RBP as the instruction found them. */
.macro enter_push width, store, host_register
	sub x25, x25, #(\width / 8)
	.if \width == 32
	mov w25, w25
	.endif
	mov x7, x25
	page_cross_check (\width / 8), Lx86_enter_push_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()27, [x7]
	b Lx86_enter_push_done\@
Lx86_enter_push_cross\@:
	cross_page_store x27, (\width / 8)
Lx86_enter_push_done\@:
	publish_pending_code x25, (\width / 8)
.endm

.macro enter_load width, load, host_register
	mov x7, x24
	page_cross_check (\width / 8), Lx86_enter_load_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()27, [x7]
	b Lx86_enter_load_done\@
Lx86_enter_load_cross\@:
	mov x0, _context
	mov x1, x24
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, Lx86_memory_fault
	mov x27, x0
Lx86_enter_load_done\@:
.endm

.macro enter_gadget width, load, load_register, store, store_register
gadget enter_\width
	ldp x23, x22, [_stream], #16
	ldr x27, [_cpu, #(UML_X86_64_CPU_REGISTERS + 5 * 8)]
	mov x24, x27
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	enter_push \width, \store, \store_register
	mov x26, x25
	and w8, w23, #0x1f
	cbz w8, Lx86_enter_nesting_done\@
	mov w19, #1
Lx86_enter_nesting_loop\@:
	and w8, w23, #0x1f
	cmp w19, w8
	b.hs Lx86_enter_push_frame\@
	sub x24, x24, #(\width / 8)
	.if \width == 32
	mov w24, w24
	.endif
	enter_load \width, \load, \load_register
	enter_push \width, \store, \store_register
	add w19, w19, #1
	b Lx86_enter_nesting_loop\@
Lx86_enter_push_frame\@:
	mov x27, x26
	enter_push \width, \store, \store_register
Lx86_enter_nesting_done\@:
	/* A 32-bit frame writes EBP and ESP zero-extended, as every other
	 * 32-bit stack operation does; its stack wraps at 4 GiB. */
	.if \width == 32
	str x26, [_cpu, #(UML_X86_64_CPU_REGISTERS + 5 * 8)]
	.else
	\store \store_register\()26, [_cpu, #(UML_X86_64_CPU_REGISTERS + 5 * 8)]
	.endif
	ubfx x8, x23, #16, #16
	sub x25, x25, x8
	.if \width == 32
	mov w25, w25
	.endif
	str x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	ubfx x8, x23, #56, #4
	add x8, x22, x8
	str x8, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.endm

enter_gadget 16, ldrh, w, strh, w
enter_gadget 32, ldr, w, str, w
enter_gadget 64, ldr, x, str, x

.purgem enter_gadget
.purgem enter_load
.purgem enter_push

.macro legacy_load width, load, host_register, destination
	mov x7, x25
	page_cross_check (\width / 8), Lx86_legacy_load_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()\destination, [x7]
	b Lx86_legacy_load_done\@
Lx86_legacy_load_cross\@:
	cross_page_load (\width / 8)
	mov x\destination, x0
Lx86_legacy_load_done\@:
.endm

.macro far_transfer kind, width, load, host_register
gadget far_\kind\()_\width
	effective_address
	mov x25, x7
	legacy_load \width, \load, \host_register, 26
	add x25, x25, #(\width / 8)
	legacy_load 16, ldrh, w, 27
	.ifc \kind,call
	ubfx x8, x23, #56, #4
	add x24, x22, x8
	.if \width == 16
	uxth w24, w24
	.endif
	.if \width == 32
	mov w24, w24
	.endif
	ldrh w19, [_cpu, #(UML_X86_64_CPU_SEGMENT_SELECTORS + 2)]
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	sub x25, x25, #(\width / 4)
	mov x7, x25
	page_cross_check (\width / 4), Lx86_far_call_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 4)
	.if \width == 16
	bfi x24, x19, #16, #16
	str w24, [x7]
	.endif
	.if \width == 32
	bfi x24, x19, #32, #32
	str x24, [x7]
	.endif
	.if \width == 64
	stp x24, x19, [x7]
	.endif
	b Lx86_far_call_done\@
Lx86_far_call_cross\@:
	.if \width == 16
	bfi x24, x19, #16, #16
	mov x0, _context
	mov x1, x25
	mov x2, x24
	mov w3, #4
	bl NAME(uml_x86_64_gadget_cross_page_store)
	.endif
	.if \width == 32
	bfi x24, x19, #32, #32
	mov x0, _context
	mov x1, x25
	mov x2, x24
	mov w3, #8
	bl NAME(uml_x86_64_gadget_cross_page_store)
	.endif
	.if \width == 64
	sub sp, sp, #16
	stp x24, x19, [sp]
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #16
	bl NAME(uml_x86_64_gadget_cross_page_vector_store)
	add sp, sp, #16
	.endif
	cbz w0, Lx86_memory_fault
Lx86_far_call_done\@:
	str x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	publish_pending_code x25, (\width / 4)
	.endif
	strh w27, [_cpu, #(UML_X86_64_CPU_SEGMENT_SELECTORS + 2)]
	str x26, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.endm

far_transfer call, 16, ldrh, w
far_transfer call, 32, ldr, w
far_transfer call, 64, ldr, x
far_transfer jump, 16, ldrh, w
far_transfer jump, 32, ldr, w
far_transfer jump, 64, ldr, x

.purgem far_transfer

.macro far_return width, load, host_register
gadget far_return_\width
	ldp x23, x22, [_stream], #16
	ldr x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	legacy_load \width, \load, \host_register, 26
	add x25, x25, #(\width / 8)
	legacy_load \width, \load, \host_register, 27
	add x25, x25, #(\width / 8)
	add x25, x25, x23
	str x25, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	strh w27, [_cpu, #(UML_X86_64_CPU_SEGMENT_SELECTORS + 2)]
	str x26, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.endm

far_return 16, ldrh, w
far_return 32, ldr, w
far_return 64, ldr, x

.purgem far_return
.purgem legacy_load

.macro iret_load width, load, host_register, destination, offset
	ldr x7, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	add x7, x7, #((\width / 8) * \offset)
	page_cross_check (\width / 8), Lx86_iret_load_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()\destination, [x7]
	b Lx86_iret_load_done\@
Lx86_iret_load_cross\@:
	mov x0, _context
	mov x1, x7
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, Lx86_memory_fault
	mov x\destination, x0
Lx86_iret_load_done\@:
.endm

/* \long_mode names the 64-bit-mode forms. There IRET pops SS:RSP at every
 * operand size and whatever the privilege change, and a narrower EIP, CS,
 * EFLAGS, ESP or SS is zero-extended by the load. In compat32 SS:ESP is
 * popped only on a return to an outer privilege level. */
.macro interrupt_return name, width, load, host_register, long_mode
gadget interrupt_return_\name
	ldr x22, [_stream], #8
	ldr x23, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	iret_load \width, \load, \host_register, 19, 0
	iret_load \width, \load, \host_register, 24, 1
	iret_load \width, \load, \host_register, 25, 2
	.if \long_mode
	/* Wine relies on the five-slot frame to switch from its syscall stack
	 * to the initial user stack even though both selectors have ring-three
	 * privilege. */
	iret_load \width, \load, \host_register, 26, 3
	iret_load \width, \load, \host_register, 27, 4
	mov w15, #1
	.else
	ldrh w8, [_cpu, #(UML_X86_64_CPU_SEGMENT_SELECTORS + 2)]
	and w8, w8, #3
	and w9, w24, #3
	cmp w9, w8
	b.hi Lx86_iret_outer_privilege\@
	mov w15, wzr
	add x26, x23, #((\width / 8) * 3)
	.if \width == 32
	mov w26, w26
	.endif
	b Lx86_iret_state_ready\@
Lx86_iret_outer_privilege\@:
	iret_load \width, \load, \host_register, 26, 3
	iret_load \width, \load, \host_register, 27, 4
	mov w15, #1
Lx86_iret_state_ready\@:
	.endif
	/* At CPL 3 IRET loads what POPF loads, plus RF: IF, IOPL, VM, VIF and
	 * VIP keep their values and the reserved bits stay fixed.  None of the
	 * kept bits is an arithmetic flag, so a deferred result need not be
	 * materialized first.  RF lasts until the next instruction starts.
	 * IRET always leaves the engine, and `execute` clears RF as it enters
	 * to start that instruction, so no gadget has to.  As for POPF, setting
	 * TF arms #DB after the next instruction, which the runtime then runs
	 * alone. */
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	mov x10, #0x4dd5
	.if \width != 16
	movk x10, #0x25, lsl #16
	.endif
	bic x11, x9, x10
	and x25, x25, x10
	orr x25, x11, x25
	orr x25, x25, #2
	bic x12, x25, x9
	str x19, [_cpu, #UML_X86_64_CPU_RIP]
	strh w24, [_cpu, #(UML_X86_64_CPU_SEGMENT_SELECTORS + 2)]
	str x25, [_cpu, #UML_X86_64_CPU_RFLAGS]
	str x26, [_cpu, #(UML_X86_64_CPU_REGISTERS + 4 * 8)]
	cbz w15, Lx86_iret_keep_stack_segment\@
	strh w27, [_cpu, #(UML_X86_64_CPU_SEGMENT_SELECTORS + 4)]
Lx86_iret_keep_stack_segment\@:
	str xzr, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	tbz x12, #8, Lx86_iret_leave\@
	mov w0, #UML_X86_64_EVENT_TRAP_FLAG
Lx86_iret_leave\@:
	b uml_x86_64_gadget_leave
.endm

interrupt_return 16, 16, ldrh, w, 0
interrupt_return 32, 32, ldr, w, 0
interrupt_return 64, 64, ldr, x, 1
interrupt_return long_16, 16, ldrh, w, 1
interrupt_return long_32, 32, ldr, w, 1

.purgem interrupt_return
.purgem iret_load
