/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scalar MUL and IMUL forms. */

/* Inputs are x24 and x15. x27 receives the raw low product, x16 receives the
 * raw high half where needed, and w19 reports whether CF/OF must be set.
 */
.macro multiply_compute signed, width
	.if \signed
	.if \width == 8
	sxtb w24, w24
	sxtb w15, w15
	mul w27, w24, w15
	sxtb w8, w27
	cmp w27, w8
	cset w19, ne
	lsr w16, w27, #8
	.elseif \width == 16
	sxth w24, w24
	sxth w15, w15
	mul w27, w24, w15
	sxth w8, w27
	cmp w27, w8
	cset w19, ne
	lsr w16, w27, #16
	.elseif \width == 32
	smull x27, w24, w15
	sxtw x8, w27
	cmp x27, x8
	cset w19, ne
	lsr x16, x27, #32
	.else
	mul x27, x24, x15
	smulh x16, x24, x15
	asr x8, x27, #63
	cmp x16, x8
	cset w19, ne
	.endif
	.else
	.if \width == 8
	and w24, w24, #0xff
	and w15, w15, #0xff
	mul w27, w24, w15
	lsr w16, w27, #8
	cmp w16, #0
	cset w19, ne
	.elseif \width == 16
	uxth w24, w24
	uxth w15, w15
	mul w27, w24, w15
	lsr w16, w27, #16
	cmp w16, #0
	cset w19, ne
	.elseif \width == 32
	umull x27, w24, w15
	lsr x16, x27, #32
	cmp x16, #0
	cset w19, ne
	.else
	mul x27, x24, x15
	umulh x16, x24, x15
	cmp x16, #0
	cset w19, ne
	.endif
	.endif
.endm

.macro multiply_flags
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	mov x9, #0x801
	bic x8, x8, x9
	cbz w19, .Lx86_multiply_flags_done\@
	orr x8, x8, x9
.Lx86_multiply_flags_done\@:
	orr x8, x8, #0x2
	str x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
.endm

.macro multiply_write_pair width
	.if \width == 8
	strh w27, [_cpu, #UML_X86_64_CPU_REGISTERS]
	.elseif \width == 16
	strh w27, [_cpu, #UML_X86_64_CPU_REGISTERS]
	strh w16, [_cpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	.elseif \width == 32
	mov w8, w27
	str x8, [_cpu, #UML_X86_64_CPU_REGISTERS]
	mov w8, w16
	str x8, [_cpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	.else
	str x27, [_cpu, #UML_X86_64_CPU_REGISTERS]
	str x16, [_cpu, #(UML_X86_64_CPU_REGISTERS + 2 * 8)]
	.endif
.endm

.macro multiply_implicit_register name, signed, width, load, host_register
gadget \name\()_\width\()_r
	ldr x8, [_stream], #8
	\load \host_register\()15, [_cpu, x8]
	\load \host_register\()24, [_cpu, #UML_X86_64_CPU_REGISTERS]
	multiply_compute \signed, \width
	multiply_write_pair \width
	multiply_flags
	retire_and_next
.endm

.macro multiply_implicit_memory name, signed, width, load, host_register
gadget \name\()_\width\()_m
	effective_address
	mov x25, x7
	page_cross_check (\width / 8), .Lx86_multiply_read_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()15, [x7]
	b .Lx86_multiply_loaded\@
.Lx86_multiply_read_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x15, x0
.Lx86_multiply_loaded\@:
	\load \host_register\()24, [_cpu, #UML_X86_64_CPU_REGISTERS]
	multiply_compute \signed, \width
	multiply_write_pair \width
	multiply_flags
	retire_and_next
.endm

.macro multiply_implicit_width name, signed, width, load, host_register
	multiply_implicit_register \name, \signed, \width, \load, \host_register
	multiply_implicit_memory \name, \signed, \width, \load, \host_register
.endm

.macro multiply_implicit_family name, signed
	multiply_implicit_width \name, \signed, 8, ldrb, w
	multiply_implicit_width \name, \signed, 16, ldrh, w
	multiply_implicit_width \name, \signed, 32, ldr, w
	multiply_implicit_width \name, \signed, 64, ldr, x
.endm

multiply_implicit_family mul, 0
multiply_implicit_family imul, 1

.macro multiply_explicit_register width, load, store, host_register, store_register
gadget imul_\width\()_rr
	ldr x8, [_stream], #8
	and x25, x8, #0xffff
	ubfx x9, x8, #16, #16
	\load \host_register\()24, [_cpu, x25]
	\load \host_register\()15, [_cpu, x9]
	multiply_compute 1, \width
	.if \width == 32
	mov w27, w27
	.endif
	\store \store_register\()27, [_cpu, x25]
	multiply_flags
	retire_and_next

gadget imul_\width\()_rri
	ldp x8, x15, [_stream], #16
	and x25, x8, #0xffff
	ubfx x9, x8, #16, #16
	\load \host_register\()24, [_cpu, x9]
	multiply_compute 1, \width
	.if \width == 32
	mov w27, w27
	.endif
	\store \store_register\()27, [_cpu, x25]
	multiply_flags
	retire_and_next
.endm

.macro multiply_explicit_memory width, load, store, host_register, store_register, form
gadget imul_\width\()_rm\form
	effective_address
	mov x25, x7
	page_cross_check (\width / 8), .Lx86_imul_read_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
	b .Lx86_imul_loaded\@
.Lx86_imul_read_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
.Lx86_imul_loaded\@:
	and x8, x23, #0xffff
	mov x25, x8
	.ifc \form,r
	\load \host_register\()15, [_cpu, x25]
	.else
	ldr x15, [_stream], #8
	.endif
	multiply_compute 1, \width
	.if \width == 32
	mov w27, w27
	.endif
	\store \store_register\()27, [_cpu, x25]
	multiply_flags
	retire_and_next
.endm

.macro multiply_explicit_width width, load, store, host_register, store_register
	multiply_explicit_register \width, \load, \store, \host_register, \store_register
	multiply_explicit_memory \width, \load, \store, \host_register, \store_register, r
	multiply_explicit_memory \width, \load, \store, \host_register, \store_register, i
.endm

multiply_explicit_width 16, ldrh, strh, w, w
multiply_explicit_width 32, ldr, str, w, x
multiply_explicit_width 64, ldr, str, x, x

.purgem multiply_explicit_width
.purgem multiply_explicit_memory
.purgem multiply_explicit_register
.purgem multiply_implicit_family
.purgem multiply_implicit_width
.purgem multiply_implicit_memory
.purgem multiply_implicit_register
.purgem multiply_write_pair
.purgem multiply_flags
.purgem multiply_compute
