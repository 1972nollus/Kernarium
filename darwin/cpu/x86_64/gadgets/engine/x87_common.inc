/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared x87 stack, tag, memory, and exact-math call machinery. */

.set Lx86_x87_tag_empty, 3
.set Lx86_x87_status_c0, 0x0100
.set Lx86_x87_status_c1, 0x0200
.set Lx86_x87_status_c2, 0x0400
.set Lx86_x87_status_c3, 0x4000

.macro x87_load_u64 destination, value
	movz \destination, #((\value) & 0xffff)
	movk \destination, #(((\value) >> 16) & 0xffff), lsl #16
	movk \destination, #(((\value) >> 32) & 0xffff), lsl #32
	movk \destination, #(((\value) >> 48) & 0xffff), lsl #48
.endm

/* While the guest has an x87 exception unmasked the runtime sets
 * `exception_checks`, and an instruction that raises one leaves before it
 * commits anything; the runtime runs it again, checked, which leaves the
 * exception pending for the next waiting instruction as x87 does. A helper
 * reports one in `exception_raised`, which its caller tests before it stores a
 * result: a load and a branch after each helper call, whether or not the guest
 * unmasks anything. */
.p2align 4
Lx86_x87_raised:
	b uml_x86_64_gadget_exception_check

.macro x87_leave_if_raised
	ldrb w9, [_cpu, #UML_X86_64_CPU_EXCEPTION_RAISED]
	cbnz w9, Lx86_x87_raised
.endm

/* A stack fault is an invalid operation: an unmasked one leaves the same way,
 * before the fault touches the status word or the stack. */
.macro x87_stack_fault_check
	ldrb w17, [_cpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w17, #0, Lx86_x87_stack_fault_masked\@
	ldrh w17, [_cpu, #UML_X86_64_CPU_X87_CONTROL]
	tbnz w17, #0, Lx86_x87_stack_fault_masked\@
	b uml_x86_64_gadget_exception_check
Lx86_x87_stack_fault_masked\@:
.endm

.macro x87_physical logical, physical
	ldrb \physical, [_cpu, #UML_X86_64_CPU_X87_TOP]
	add \physical, \physical, \logical
	and \physical, \physical, #7
.endm

.macro x87_load_stack logical, significand, exponent
	x87_physical \logical, w14
	ldrh w15, [_cpu, #UML_X86_64_CPU_X87_TAG]
	lsl w16, w14, #1
	lsr w15, w15, w16
	and w15, w15, #3
	cmp w15, #Lx86_x87_tag_empty
	b.ne Lx86_x87_load_present\@
	x87_stack_fault_check
	ldrh w15, [_cpu, #UML_X86_64_CPU_X87_STATUS]
	mov w17, #0x41
	orr w15, w15, w17
	strh w15, [_cpu, #UML_X86_64_CPU_X87_STATUS]
	mov \significand, #0xc000000000000000
	mov \exponent, #0xffff
	b Lx86_x87_load_done\@
Lx86_x87_load_present\@:
	add x15, _cpu, #UML_X86_64_CPU_X87
	add x15, x15, w14, uxtw #4
	ldr \significand, [x15]
	ldrh \exponent, [x15, #8]
Lx86_x87_load_done\@:
.endm

.macro x87_set_tag physical, tag
	ldrh w15, [_cpu, #UML_X86_64_CPU_X87_TAG]
	lsl w16, \physical, #1
	mov w17, #3
	lsl w17, w17, w16
	bic w15, w15, w17
	and w17, \tag, #3
	lsl w17, w17, w16
	orr w15, w15, w17
	strh w15, [_cpu, #UML_X86_64_CPU_X87_TAG]
.endm

.macro x87_store_physical physical, significand, exponent
	add x15, _cpu, #UML_X86_64_CPU_X87
	add x15, x15, \physical, uxtw #4
	str \significand, [x15]
	strh \exponent, [x15, #8]
	add x15, _cpu, #UML_X86_64_CPU_MMX
	str \significand, [x15, \physical, uxtw #3]

	/* Recreate the x87 two-bit classification tag. */
	mov w18, #2
	and w15, \exponent, #0x7fff
	cbz w15, Lx86_x87_store_zero_exp\@
	mov w17, #0x7fff
	cmp w15, w17
	b.eq Lx86_x87_store_classified\@
	tbz \significand, #63, Lx86_x87_store_classified\@
	mov w18, #0
	b Lx86_x87_store_classified\@
Lx86_x87_store_zero_exp\@:
	cbnz \significand, Lx86_x87_store_classified\@
	mov w18, #1
Lx86_x87_store_classified\@:
	x87_set_tag \physical, w18
.endm

.macro x87_store_stack logical, significand, exponent
	x87_physical \logical, w14
	x87_store_physical w14, \significand, \exponent
.endm

.macro x87_push significand, exponent
	ldrh w15, [_cpu, #UML_X86_64_CPU_X87_STATUS]
	bic w15, w15, #Lx86_x87_status_c1
	ldrb w14, [_cpu, #UML_X86_64_CPU_X87_TOP]
	sub w14, w14, #1
	and w14, w14, #7
	ldrh w16, [_cpu, #UML_X86_64_CPU_X87_TAG]
	lsl w17, w14, #1
	lsr w16, w16, w17
	and w16, w16, #3
	cmp w16, #Lx86_x87_tag_empty
	b.eq Lx86_x87_push_slot_ready\@
	x87_stack_fault_check
	mov w16, #0x241
	orr w15, w15, w16
Lx86_x87_push_slot_ready\@:
	strh w15, [_cpu, #UML_X86_64_CPU_X87_STATUS]
	strb w14, [_cpu, #UML_X86_64_CPU_X87_TOP]
	x87_store_physical w14, \significand, \exponent
.endm

/* FPTAN, FSINCOS and FXTRACT store ST0 before they push, so while the
 * gadgets check x87 exceptions they test for the stack overflow first. */
.macro x87_push_fault_check
	ldrb w14, [_cpu, #UML_X86_64_CPU_X87_TOP]
	sub w14, w14, #1
	and w14, w14, #7
	ldrh w16, [_cpu, #UML_X86_64_CPU_X87_TAG]
	lsl w17, w14, #1
	lsr w16, w16, w17
	and w16, w16, #3
	cmp w16, #Lx86_x87_tag_empty
	b.eq Lx86_x87_push_fault_none\@
	x87_stack_fault_check
Lx86_x87_push_fault_none\@:
.endm

.macro x87_pop
	ldrb w14, [_cpu, #UML_X86_64_CPU_X87_TOP]
	mov w18, #Lx86_x87_tag_empty
	x87_set_tag w14, w18
	add w14, w14, #1
	and w14, w14, #7
	strb w14, [_cpu, #UML_X86_64_CPU_X87_TOP]
.endm

.macro x87_store_value pointer, significand, exponent
	str \significand, [\pointer]
	strh \exponent, [\pointer, #8]
.endm

.macro x87_load_value pointer, significand, exponent
	ldr \significand, [\pointer]
	ldrh \exponent, [\pointer, #8]
.endm

.macro x87_set_status_compare result
	ldrh w9, [_cpu, #UML_X86_64_CPU_X87_STATUS]
	mov w10, #0x4700
	bic w9, w9, w10
	cmp \result, #1
	b.ne Lx86_x87_compare_not_less\@
	orr w9, w9, #Lx86_x87_status_c0
	b Lx86_x87_compare_done\@
Lx86_x87_compare_not_less\@:
	cmp \result, #2
	b.ne Lx86_x87_compare_not_equal\@
	orr w9, w9, #Lx86_x87_status_c3
	b Lx86_x87_compare_done\@
Lx86_x87_compare_not_equal\@:
	cmp \result, #3
	b.ne Lx86_x87_compare_done\@
	mov w10, #0x4500
	orr w9, w9, w10
Lx86_x87_compare_done\@:
	strh w9, [_cpu, #UML_X86_64_CPU_X87_STATUS]
.endm

.macro x87_set_integer_compare result
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	mov x10, #0x8d5
	bic x9, x9, x10
	cmp \result, #1
	b.ne Lx86_x87_integer_not_less\@
	orr x9, x9, #0x1
	b Lx86_x87_integer_compare_done\@
Lx86_x87_integer_not_less\@:
	cmp \result, #2
	b.ne Lx86_x87_integer_not_equal\@
	orr x9, x9, #0x40
	b Lx86_x87_integer_compare_done\@
Lx86_x87_integer_not_equal\@:
	cmp \result, #3
	b.ne Lx86_x87_integer_compare_done\@
	mov x10, #0x45
	orr x9, x9, x10
Lx86_x87_integer_compare_done\@:
	orr x9, x9, #0x2
	str x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
.endm

.macro x87_scalar_load size, destination
	page_cross_check (\size), Lx86_x87_scalar_load_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, \size
	.if \size == 2
	ldrh w9, [x7]
	.elseif \size == 4
	ldr w9, [x7]
	.else
	ldr x9, [x7]
	.endif
	b Lx86_x87_scalar_load_done\@
Lx86_x87_scalar_load_cross\@:
	mov x0, _context
	mov x1, x7
	mov w2, #\size
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, Lx86_memory_fault
	mov \destination, x0
Lx86_x87_scalar_load_done\@:
.endm

.macro x87_scalar_store size, source
	mov x24, \source
	mov x7, x25
	page_cross_check (\size), Lx86_x87_scalar_store_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, \size
	.if \size == 2
	strh w24, [x7]
	.elseif \size == 4
	str w24, [x7]
	.else
	str x24, [x7]
	.endif
	b Lx86_x87_scalar_store_done\@
Lx86_x87_scalar_store_cross\@:
	mov x0, _context
	mov x1, x25
	mov x2, x24
	mov w3, #\size
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, Lx86_memory_fault
Lx86_x87_scalar_store_done\@:
.endm

.macro x87_bulk_load size, buffer, fault
	x86_marks_guest_memory
	mov x0, _context
	mov x1, x25
	mov x2, \buffer
	mov w3, #\size
	bl NAME(uml_x86_64_gadget_bulk_load)
	cbz w0, \fault
.endm

.macro x87_bulk_store size, buffer, fault
	x86_marks_guest_memory
	mov x0, _context
	mov x1, x25
	mov x2, \buffer
	mov w3, #\size
	bl NAME(uml_x86_64_gadget_bulk_store)
	cbz w0, \fault
.endm
