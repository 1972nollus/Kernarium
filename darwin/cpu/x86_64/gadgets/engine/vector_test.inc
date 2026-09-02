/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PTEST/VPTEST and VTESTPS/VTESTPD flag calculations. */

.macro vector_test_flags width, stride
	mov w11, wzr
	mov w12, wzr
	mov x13, #(\stride - 1)
.Lx86_vector_test_loop\@:
	ldrb w14, [x24, x13]
	ldrb w15, [x25, x13]
	and w16, w14, w15
	bic w17, w15, w14
	.if \stride != 1
	and w16, w16, #0x80
	and w17, w17, #0x80
	.endif
	orr w11, w11, w16
	orr w12, w12, w17
	add x13, x13, #\stride
	cmp x13, #(\width / 8)
	b.lo .Lx86_vector_test_loop\@
	cmp w11, #0
	cset w24, eq
	cmp w12, #0
	cset w25, eq
	bl uml_x86_64_gadget_materialize_flags
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	mov x10, #0x8d5
	bic x9, x9, x10
	orr x9, x9, #2
	cbz w24, .Lx86_vector_test_not_zero\@
	orr x9, x9, #0x40
.Lx86_vector_test_not_zero\@:
	cbz w25, .Lx86_vector_test_not_carry\@
	orr x9, x9, #1
.Lx86_vector_test_not_carry\@:
	str x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	retire_and_next
.endm

.macro vector_test_register width, stride
gadget vector_test_\width\()_\stride\()_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	add x24, _cpu, x9
	add x25, _cpu, x10
	vector_test_flags \width, \stride
.endm

.macro vector_test_memory width, stride
gadget vector_test_\width\()_\stride\()_rm
	effective_address
	mov x25, x7
	page_cross_check (\width / 8), .Lx86_vector_test_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	mov x25, x7
	b .Lx86_vector_test_loaded\@
.Lx86_vector_test_cross\@:
	sub sp, sp, #32
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_vector_load)
	cbnz w0, .Lx86_vector_test_cross_loaded\@
	add sp, sp, #32
	b .Lx86_memory_fault
.Lx86_vector_test_cross_loaded\@:
	mov x25, sp
	mov w27, #1
	b .Lx86_vector_test_pointers\@
.Lx86_vector_test_loaded\@:
	mov w27, wzr
.Lx86_vector_test_pointers\@:
	and x8, x23, #0xffff
	add x24, _cpu, x8
	.if \stride == 1
	mov w11, wzr
	mov w12, wzr
	mov x13, #(\stride - 1)
.Lx86_vector_test_memory_loop\@:
	ldrb w14, [x24, x13]
	ldrb w15, [x25, x13]
	and w16, w14, w15
	bic w17, w15, w14
	orr w11, w11, w16
	orr w12, w12, w17
	add x13, x13, #\stride
	cmp x13, #(\width / 8)
	b.lo .Lx86_vector_test_memory_loop\@
	.else
	mov w11, wzr
	mov w12, wzr
	mov x13, #(\stride - 1)
.Lx86_vector_test_memory_loop\@:
	ldrb w14, [x24, x13]
	ldrb w15, [x25, x13]
	and w16, w14, w15
	bic w17, w15, w14
	and w16, w16, #0x80
	and w17, w17, #0x80
	orr w11, w11, w16
	orr w12, w12, w17
	add x13, x13, #\stride
	cmp x13, #(\width / 8)
	b.lo .Lx86_vector_test_memory_loop\@
	.endif
	cmp w11, #0
	cset w24, eq
	cmp w12, #0
	cset w25, eq
	cbz w27, .Lx86_vector_test_memory_stack_ready\@
	add sp, sp, #32
.Lx86_vector_test_memory_stack_ready\@:
	bl uml_x86_64_gadget_materialize_flags
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	mov x10, #0x8d5
	bic x9, x9, x10
	orr x9, x9, #2
	cbz w24, .Lx86_vector_test_memory_not_zero\@
	orr x9, x9, #0x40
.Lx86_vector_test_memory_not_zero\@:
	cbz w25, .Lx86_vector_test_memory_not_carry\@
	orr x9, x9, #1
.Lx86_vector_test_memory_not_carry\@:
	str x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	retire_and_next
.endm

.macro vector_test_family width, stride
vector_test_register \width, \stride
vector_test_memory \width, \stride
.endm

vector_test_family 128, 1
vector_test_family 256, 1
vector_test_family 128, 4
vector_test_family 256, 4
vector_test_family 128, 8
vector_test_family 256, 8

.purgem vector_test_family
.purgem vector_test_memory
.purgem vector_test_register
.purgem vector_test_flags
