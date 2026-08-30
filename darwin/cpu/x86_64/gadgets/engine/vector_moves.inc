/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Scalar-width vector transfers and MMX architectural synchronization. */

.macro vector_scalar_store width, load, host_register
gadget vector_move_\width\()_mr
	effective_address
	mov x25, x7
	and x8, x23, #0xffff
	\load \host_register\()24, [_cpu, x8]
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_vector_scalar_store_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	.if \width == 32
	str w24, [x7]
	.else
	str x24, [x7]
	.endif
	finish_store (\width / 8)
.Lx86_vector_scalar_store_cross\@:
	mov x0, _context
	mov x1, x25
	mov x2, x24
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
	finish_store (\width / 8)
.endm

vector_scalar_store 32, ldr, w
vector_scalar_store 64, ldr, x

.purgem vector_scalar_store

gadget vector_move_64_128_rm
	effective_address
	mov x25, x7
	add x9, x7, #7
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_vector_move_64_128_cross
	translate 0, UML_X86_64_TLB_READ_PAGE, 8
	ldr d0, [x7]
	b .Lx86_vector_move_64_128_write
.Lx86_vector_move_64_128_cross:
	mov x0, _context
	mov x1, x25
	mov w2, #8
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	fmov d0, x0
.Lx86_vector_move_64_128_write:
	and x8, x23, #0xffff
	add x8, _cpu, x8
	str q0, [x8]
	retire_and_next

gadget vector_move_64_128_rm_vex
	effective_address
	mov x25, x7
	add x9, x7, #7
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_vector_move_64_128_vex_cross
	translate 0, UML_X86_64_TLB_READ_PAGE, 8
	ldr d0, [x7]
	b .Lx86_vector_move_64_128_vex_write
.Lx86_vector_move_64_128_vex_cross:
	mov x0, _context
	mov x1, x25
	mov w2, #8
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	fmov d0, x0
.Lx86_vector_move_64_128_vex_write:
	and x8, x23, #0xffff
	add x8, _cpu, x8
	str q0, [x8]
	movi v1.16b, #0
	str q1, [x8, #16]
	retire_and_next

.macro synchronize_mmx offset, value
	sub x12, x\offset, #UML_X86_64_CPU_MMX
	lsr x12, x12, #3
	add x13, _cpu, #UML_X86_64_CPU_X87
	add x13, x13, x12, lsl #4
	str x\value, [x13]
	mov w14, #0xffff
	strh w14, [x13, #8]
	strh wzr, [_cpu, #UML_X86_64_CPU_X87_TAG]
	strb wzr, [_cpu, #UML_X86_64_CPU_X87_TOP]
.endm

gadget vector_move_64_mmx_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ldr x11, [_cpu, x10]
	str x11, [_cpu, x9]
	synchronize_mmx 9, 11
	retire_and_next

gadget vector_move_64_mmx_rm
	effective_address
	mov x25, x7
	add x9, x7, #7
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_vector_move_64_mmx_cross
	translate 0, UML_X86_64_TLB_READ_PAGE, 8
	ldr x24, [x7]
	b .Lx86_vector_move_64_mmx_write
.Lx86_vector_move_64_mmx_cross:
	mov x0, _context
	mov x1, x25
	mov w2, #8
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
.Lx86_vector_move_64_mmx_write:
	and x9, x23, #0xffff
	str x24, [_cpu, x9]
	synchronize_mmx 9, 24
	retire_and_next

.purgem synchronize_mmx

.macro vector_gpr_to_xmm width, load, host_register, vex
gadget vector_gpr_to_xmm_\width\()\vex
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \host_register\()11, [_cpu, x10]
	add x9, _cpu, x9
	movi v0.16b, #0
	str q0, [x9]
	.if \width == 32
	str w11, [x9]
	.else
	str x11, [x9]
	.endif
	.ifc \vex,_vex
	str q0, [x9, #16]
	.endif
	retire_and_next
.endm

vector_gpr_to_xmm 32, ldr, w,
vector_gpr_to_xmm 64, ldr, x,
vector_gpr_to_xmm 32, ldr, w, _vex
vector_gpr_to_xmm 64, ldr, x, _vex

.purgem vector_gpr_to_xmm

.macro vector_gpr_to_mmx width, load, host_register
gadget vector_gpr_to_mmx_\width
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \host_register\()11, [_cpu, x10]
	.if \width == 32
	mov w11, w11
	.endif
	str x11, [_cpu, x9]
	sub x12, x9, #UML_X86_64_CPU_MMX
	lsr x12, x12, #3
	add x13, _cpu, #UML_X86_64_CPU_X87
	add x13, x13, x12, lsl #4
	str x11, [x13]
	mov w14, #0xffff
	strh w14, [x13, #8]
	strh wzr, [_cpu, #UML_X86_64_CPU_X87_TAG]
	strb wzr, [_cpu, #UML_X86_64_CPU_X87_TOP]
	retire_and_next
.endm

vector_gpr_to_mmx 32, ldr, w
vector_gpr_to_mmx 64, ldr, x

.purgem vector_gpr_to_mmx

gadget vector_memory_32_to_mmx
	effective_address
	mov x25, x7
	add x9, x7, #3
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_vector_memory_32_to_mmx_cross
	translate 0, UML_X86_64_TLB_READ_PAGE, 4
	ldr w24, [x7]
	b .Lx86_vector_memory_32_to_mmx_write
.Lx86_vector_memory_32_to_mmx_cross:
	mov x0, _context
	mov x1, x25
	mov w2, #4
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov w24, w0
.Lx86_vector_memory_32_to_mmx_write:
	and x9, x23, #0xffff
	str x24, [_cpu, x9]
	sub x12, x9, #UML_X86_64_CPU_MMX
	lsr x12, x12, #3
	add x13, _cpu, #UML_X86_64_CPU_X87
	add x13, x13, x12, lsl #4
	str x24, [x13]
	mov w14, #0xffff
	strh w14, [x13, #8]
	strh wzr, [_cpu, #UML_X86_64_CPU_X87_TAG]
	strb wzr, [_cpu, #UML_X86_64_CPU_X87_TOP]
	retire_and_next

.macro vector_scalar_register width, load, store, host_register, vex
gadget vector_scalar_move_\width\()_rr\vex
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	\load \host_register\()11, [_cpu, x10]
	.ifc \vex,_vex
	ubfx x12, x8, #32, #16
	add x12, _cpu, x12
	ldr q0, [x12]
	add x9, _cpu, x9
	str q0, [x9]
	\store \host_register\()11, [x9]
	movi v1.16b, #0
	str q1, [x9, #16]
	.else
	\store \host_register\()11, [_cpu, x9]
	.endif
	retire_and_next
.endm

vector_scalar_register 32, ldr, str, w,
vector_scalar_register 64, ldr, str, x,
vector_scalar_register 32, ldr, str, w, _vex
vector_scalar_register 64, ldr, str, x, _vex

.purgem vector_scalar_register

.macro vector_scalar_load width, load, host_register, vex
gadget vector_scalar_move_\width\()_rm\vex
	effective_address
	mov x25, x7
	add x9, x7, #(\width / 8 - 1)
	eor x9, x9, x7
	tbnz x9, #12, .Lx86_vector_scalar_load_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
	b .Lx86_vector_scalar_load_write\@
.Lx86_vector_scalar_load_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
.Lx86_vector_scalar_load_write\@:
	and x8, x23, #0xffff
	add x8, _cpu, x8
	movi v0.16b, #0
	str q0, [x8]
	.if \width == 32
	str w24, [x8]
	.else
	str x24, [x8]
	.endif
	.ifc \vex,_vex
	str q0, [x8, #16]
	.endif
	retire_and_next
.endm

vector_scalar_load 32, ldr, w,
vector_scalar_load 64, ldr, x,
vector_scalar_load 32, ldr, w, _vex
vector_scalar_load 64, ldr, x, _vex

.purgem vector_scalar_load
