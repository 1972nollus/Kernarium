/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Packed and scalar SSE/AVX floating-point arithmetic and comparisons. */

.macro vector_fp_flush_subnormals vector, element
	.if \element == 32
	mov w14, #0x7f800000
	dup v16.4s, w14
	and v17.16b, v\vector\().16b, v16.16b
	cmeq v17.4s, v17.4s, #0
	mov w14, #0x80000000
	dup v16.4s, w14
	.else
	mov x14, #0x7ff0000000000000
	dup v16.2d, x14
	and v17.16b, v\vector\().16b, v16.16b
	cmeq v17.2d, v17.2d, #0
	mov x14, #0x8000000000000000
	dup v16.2d, x14
	.endif
	and v16.16b, v\vector\().16b, v16.16b
	bsl v17.16b, v16.16b, v\vector\().16b
	mov v\vector\().16b, v17.16b
.endm

.macro vector_fp_compare_mask predicate, element, output, left, right, temporary
	.if \element == 32
	.if \predicate == 0
	fcmeq v\output\().4s, v\left\().4s, v\right\().4s
	.elseif \predicate == 1
	fcmgt v\output\().4s, v\right\().4s, v\left\().4s
	.elseif \predicate == 2
	fcmge v\output\().4s, v\right\().4s, v\left\().4s
	.elseif \predicate == 3
	fcmeq v\output\().4s, v\left\().4s, v\left\().4s
	fcmeq v\temporary\().4s, v\right\().4s, v\right\().4s
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	mvn v\output\().16b, v\output\().16b
	.elseif \predicate == 4
	fcmeq v\output\().4s, v\left\().4s, v\right\().4s
	mvn v\output\().16b, v\output\().16b
	.elseif \predicate == 5
	fcmgt v\output\().4s, v\right\().4s, v\left\().4s
	mvn v\output\().16b, v\output\().16b
	.elseif \predicate == 6
	fcmge v\output\().4s, v\right\().4s, v\left\().4s
	mvn v\output\().16b, v\output\().16b
	.elseif \predicate == 7
	fcmeq v\output\().4s, v\left\().4s, v\left\().4s
	fcmeq v\temporary\().4s, v\right\().4s, v\right\().4s
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	.elseif \predicate == 8
	fcmeq v\output\().4s, v\left\().4s, v\left\().4s
	fcmeq v\temporary\().4s, v\right\().4s, v\right\().4s
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	mvn v\output\().16b, v\output\().16b
	fcmeq v\temporary\().4s, v\left\().4s, v\right\().4s
	orr v\output\().16b, v\output\().16b, v\temporary\().16b
	.elseif \predicate == 9
	fcmge v\output\().4s, v\left\().4s, v\right\().4s
	mvn v\output\().16b, v\output\().16b
	.elseif \predicate == 10
	fcmgt v\output\().4s, v\left\().4s, v\right\().4s
	mvn v\output\().16b, v\output\().16b
	.elseif \predicate == 11
	movi v\output\().16b, #0
	.elseif \predicate == 12
	fcmeq v\output\().4s, v\left\().4s, v\left\().4s
	fcmeq v\temporary\().4s, v\right\().4s, v\right\().4s
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	fcmeq v\temporary\().4s, v\left\().4s, v\right\().4s
	bic v\output\().16b, v\output\().16b, v\temporary\().16b
	.elseif \predicate == 13
	fcmge v\output\().4s, v\left\().4s, v\right\().4s
	.elseif \predicate == 14
	fcmgt v\output\().4s, v\left\().4s, v\right\().4s
	.else
	movi v\output\().16b, #0xff
	.endif
	.else
	.if \predicate == 0
	fcmeq v\output\().2d, v\left\().2d, v\right\().2d
	.elseif \predicate == 1
	fcmgt v\output\().2d, v\right\().2d, v\left\().2d
	.elseif \predicate == 2
	fcmge v\output\().2d, v\right\().2d, v\left\().2d
	.elseif \predicate == 3
	fcmeq v\output\().2d, v\left\().2d, v\left\().2d
	fcmeq v\temporary\().2d, v\right\().2d, v\right\().2d
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	mvn v\output\().16b, v\output\().16b
	.elseif \predicate == 4
	fcmeq v\output\().2d, v\left\().2d, v\right\().2d
	mvn v\output\().16b, v\output\().16b
	.elseif \predicate == 5
	fcmgt v\output\().2d, v\right\().2d, v\left\().2d
	mvn v\output\().16b, v\output\().16b
	.elseif \predicate == 6
	fcmge v\output\().2d, v\right\().2d, v\left\().2d
	mvn v\output\().16b, v\output\().16b
	.elseif \predicate == 7
	fcmeq v\output\().2d, v\left\().2d, v\left\().2d
	fcmeq v\temporary\().2d, v\right\().2d, v\right\().2d
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	.elseif \predicate == 8
	fcmeq v\output\().2d, v\left\().2d, v\left\().2d
	fcmeq v\temporary\().2d, v\right\().2d, v\right\().2d
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	mvn v\output\().16b, v\output\().16b
	fcmeq v\temporary\().2d, v\left\().2d, v\right\().2d
	orr v\output\().16b, v\output\().16b, v\temporary\().16b
	.elseif \predicate == 9
	fcmge v\output\().2d, v\left\().2d, v\right\().2d
	mvn v\output\().16b, v\output\().16b
	.elseif \predicate == 10
	fcmgt v\output\().2d, v\left\().2d, v\right\().2d
	mvn v\output\().16b, v\output\().16b
	.elseif \predicate == 11
	movi v\output\().16b, #0
	.elseif \predicate == 12
	fcmeq v\output\().2d, v\left\().2d, v\left\().2d
	fcmeq v\temporary\().2d, v\right\().2d, v\right\().2d
	and v\output\().16b, v\output\().16b, v\temporary\().16b
	fcmeq v\temporary\().2d, v\left\().2d, v\right\().2d
	bic v\output\().16b, v\output\().16b, v\temporary\().16b
	.elseif \predicate == 13
	fcmge v\output\().2d, v\left\().2d, v\right\().2d
	.elseif \predicate == 14
	fcmgt v\output\().2d, v\left\().2d, v\right\().2d
	.else
	movi v\output\().16b, #0xff
	.endif
	.endif
.endm

.macro vector_fp_regular_value operation, element, output, left, right
	.if \element == 32
	.ifc \operation,sqrt
	fsqrt s\output, s\right
	.endif
	.ifc \operation,rsqrt
	fsqrt s\output, s\right
	fmov s7, #1.0
	fdiv s\output, s7, s\output
	.endif
	.ifc \operation,rcp
	fmov s7, #1.0
	fdiv s\output, s7, s\right
	.endif
	.ifc \operation,add
	fadd s\output, s\left, s\right
	.endif
	.ifc \operation,mul
	fmul s\output, s\left, s\right
	.endif
	.ifc \operation,sub
	fsub s\output, s\left, s\right
	.endif
	.ifc \operation,div
	fdiv s\output, s\left, s\right
	.endif
	.ifc \operation,min
	fcmgt v\output\().4s, v\right\().4s, v\left\().4s
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.ifc \operation,max
	fcmgt v\output\().4s, v\left\().4s, v\right\().4s
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.else
	.ifc \operation,sqrt
	fsqrt d\output, d\right
	.endif
	.ifc \operation,rsqrt
	fsqrt d\output, d\right
	fmov d7, #1.0
	fdiv d\output, d7, d\output
	.endif
	.ifc \operation,rcp
	fmov d7, #1.0
	fdiv d\output, d7, d\right
	.endif
	.ifc \operation,add
	fadd d\output, d\left, d\right
	.endif
	.ifc \operation,mul
	fmul d\output, d\left, d\right
	.endif
	.ifc \operation,sub
	fsub d\output, d\left, d\right
	.endif
	.ifc \operation,div
	fdiv d\output, d\left, d\right
	.endif
	.ifc \operation,min
	fcmgt v\output\().2d, v\right\().2d, v\left\().2d
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.ifc \operation,max
	fcmgt v\output\().2d, v\left\().2d, v\right\().2d
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.endif
.endm

.macro vector_fp_packed_value operation, element, output, left, right
	.if \element == 32
	.ifc \operation,sqrt
	fsqrt v\output\().4s, v\right\().4s
	.endif
	.ifc \operation,rsqrt
	fsqrt v\output\().4s, v\right\().4s
	fmov v7.4s, #1.0
	fdiv v\output\().4s, v7.4s, v\output\().4s
	.endif
	.ifc \operation,rcp
	fmov v7.4s, #1.0
	fdiv v\output\().4s, v7.4s, v\right\().4s
	.endif
	.ifc \operation,add
	fadd v\output\().4s, v\left\().4s, v\right\().4s
	.endif
	.ifc \operation,mul
	fmul v\output\().4s, v\left\().4s, v\right\().4s
	.endif
	.ifc \operation,sub
	fsub v\output\().4s, v\left\().4s, v\right\().4s
	.endif
	.ifc \operation,div
	fdiv v\output\().4s, v\left\().4s, v\right\().4s
	.endif
	.ifc \operation,min
	fcmgt v\output\().4s, v\right\().4s, v\left\().4s
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.ifc \operation,max
	fcmgt v\output\().4s, v\left\().4s, v\right\().4s
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.else
	.ifc \operation,sqrt
	fsqrt v\output\().2d, v\right\().2d
	.endif
	.ifc \operation,rsqrt
	fsqrt v\output\().2d, v\right\().2d
	fmov v7.2d, #1.0
	fdiv v\output\().2d, v7.2d, v\output\().2d
	.endif
	.ifc \operation,rcp
	fmov v7.2d, #1.0
	fdiv v\output\().2d, v7.2d, v\right\().2d
	.endif
	.ifc \operation,add
	fadd v\output\().2d, v\left\().2d, v\right\().2d
	.endif
	.ifc \operation,mul
	fmul v\output\().2d, v\left\().2d, v\right\().2d
	.endif
	.ifc \operation,sub
	fsub v\output\().2d, v\left\().2d, v\right\().2d
	.endif
	.ifc \operation,div
	fdiv v\output\().2d, v\left\().2d, v\right\().2d
	.endif
	.ifc \operation,min
	fcmgt v\output\().2d, v\right\().2d, v\left\().2d
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.ifc \operation,max
	fcmgt v\output\().2d, v\left\().2d, v\right\().2d
	bsl v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.endif
.endm

.macro vector_fp_fix_indefinite element, output, left, right, binary, scalar
	/* AArch64 produces its positive default NaN for invalid operations.
	 * x86 instead requires the negative quiet-NaN indefinite when no input
	 * operand was already a NaN.  Preserve native payload propagation for
	 * input NaNs and replace only NaNs generated by the operation itself.
	 */
	.if \scalar
	.if \element == 32
	fcmeq s17, s\output, s\output
	fmov w14, s17
	cbnz w14, .Lx86_vector_fp_no_generated_nan\@
	fcmeq s17, s\right, s\right
	fmov w14, s17
	cbz w14, .Lx86_vector_fp_no_generated_nan\@
	.if \binary
	fcmeq s17, s\left, s\left
	fmov w14, s17
	cbz w14, .Lx86_vector_fp_no_generated_nan\@
	.endif
	mov w14, #0xffc00000
	fmov s\output, w14
	.else
	fcmeq d17, d\output, d\output
	fmov x14, d17
	cbnz x14, .Lx86_vector_fp_no_generated_nan\@
	fcmeq d17, d\right, d\right
	fmov x14, d17
	cbz x14, .Lx86_vector_fp_no_generated_nan\@
	.if \binary
	fcmeq d17, d\left, d\left
	fmov x14, d17
	cbz x14, .Lx86_vector_fp_no_generated_nan\@
	.endif
	mov x14, #0xfff8000000000000
	fmov d\output, x14
	.endif
	.else
	.if \element == 32
	fcmeq v17.4s, v\output\().4s, v\output\().4s
	uminv s18, v17.4s
	fmov w14, s18
	cbnz w14, .Lx86_vector_fp_no_generated_nan\@
	mvn v17.16b, v17.16b
	fcmeq v18.4s, v\right\().4s, v\right\().4s
	and v17.16b, v17.16b, v18.16b
	.if \binary
	fcmeq v18.4s, v\left\().4s, v\left\().4s
	and v17.16b, v17.16b, v18.16b
	.endif
	mov w14, #0xffc00000
	dup v16.4s, w14
	.else
	fcmeq v17.2d, v\output\().2d, v\output\().2d
	umov x14, v17.d[0]
	umov x15, v17.d[1]
	and x14, x14, x15
	cbnz x14, .Lx86_vector_fp_no_generated_nan\@
	mvn v17.16b, v17.16b
	fcmeq v18.2d, v\right\().2d, v\right\().2d
	and v17.16b, v17.16b, v18.16b
	.if \binary
	fcmeq v18.2d, v\left\().2d, v\left\().2d
	and v17.16b, v17.16b, v18.16b
	.endif
	mov x14, #0xfff8000000000000
	dup v16.2d, x14
	.endif
	bsl v17.16b, v16.16b, v\output\().16b
	mov v\output\().16b, v17.16b
	.endif
.Lx86_vector_fp_no_generated_nan\@:
.endm

.macro vector_fp_fix_generated_nan operation, element, output, left, right, scalar
	.ifc \operation,sqrt
	vector_fp_fix_indefinite \element, \output, \left, \right, 0, \scalar
	.endif
	.ifc \operation,rsqrt
	vector_fp_fix_indefinite \element, \output, \left, \right, 0, \scalar
	.endif
	.ifc \operation,add
	vector_fp_fix_indefinite \element, \output, \left, \right, 1, \scalar
	.endif
	.ifc \operation,mul
	vector_fp_fix_indefinite \element, \output, \left, \right, 1, \scalar
	.endif
	.ifc \operation,sub
	vector_fp_fix_indefinite \element, \output, \left, \right, 1, \scalar
	.endif
	.ifc \operation,div
	vector_fp_fix_indefinite \element, \output, \left, \right, 1, \scalar
	.endif
.endm

.macro vector_fp_compute operation, element, width, scalar, predicate
	ldr w12, [_cpu, #UML_X86_64_CPU_MXCSR]
	.if \scalar
	mov v4.16b, v0.16b
	.endif
	tbz w12, #6, .Lx86_vector_fp_inputs_ready\@
	vector_fp_flush_subnormals 0, \element
	vector_fp_flush_subnormals 1, \element
	.if \width == 256
	vector_fp_flush_subnormals 2, \element
	vector_fp_flush_subnormals 3, \element
	.endif
.Lx86_vector_fp_inputs_ready\@:
	.ifc \operation,compare
	vector_fp_compare_mask \predicate, \element, 6, 0, 1, 7
	.if \scalar
	.if \element == 32
	mov v4.s[0], v6.s[0]
	.else
	mov v4.d[0], v6.d[0]
	.endif
	.else
	mov v4.16b, v6.16b
	.if \width == 256
	vector_fp_compare_mask \predicate, \element, 6, 2, 3, 7
	mov v5.16b, v6.16b
	.endif
	.endif
	.else
	.if \scalar
	vector_fp_regular_value \operation, \element, 6, 0, 1
	vector_fp_fix_generated_nan \operation, \element, 6, 0, 1, 1
	tbz w12, #15, .Lx86_vector_fp_scalar_ftz_ready\@
	vector_fp_flush_subnormals 6, \element
.Lx86_vector_fp_scalar_ftz_ready\@:
	.if \element == 32
	mov v4.s[0], v6.s[0]
	.else
	mov v4.d[0], v6.d[0]
	.endif
	.else
	vector_fp_packed_value \operation, \element, 4, 0, 1
	vector_fp_fix_generated_nan \operation, \element, 4, 0, 1, 0
	.if \width == 256
	vector_fp_packed_value \operation, \element, 5, 2, 3
	vector_fp_fix_generated_nan \operation, \element, 5, 2, 3, 0
	.endif
	tbz w12, #15, .Lx86_vector_fp_packed_ftz_ready\@
	vector_fp_flush_subnormals 4, \element
	.if \width == 256
	vector_fp_flush_subnormals 5, \element
	.endif
.Lx86_vector_fp_packed_ftz_ready\@:
	.endif
	.endif
.endm

.macro vector_fp_write width, descriptor
	and x9, x\descriptor, #0xffff
	add x9, _cpu, x9
	str q4, [x9]
	.if \width == 256
	str q5, [x9, #16]
	.else
	tbz x\descriptor, #63, .Lx86_vector_fp_write_done\@
	movi v6.16b, #0
	str q6, [x9, #16]
.Lx86_vector_fp_write_done\@:
	.endif
.endm

.macro vector_fp_register name, operation, element, shape, width, scalar, predicate
gadget vector_fp_\name\()_\element\()_\shape\()_rr
	ldr x8, [_stream], #8
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x10, _cpu, x10
	add x11, _cpu, x11
	ldr q0, [x10]
	ldr q1, [x11]
	.if \width == 256
	ldr q2, [x10, #16]
	ldr q3, [x11, #16]
	.endif
	vector_fp_compute \operation, \element, \width, \scalar, \predicate
	vector_fp_write \width, 8
	retire_and_next
.endm

.macro vector_fp_memory name, operation, element, shape, width, scalar, predicate
gadget vector_fp_\name\()_\element\()_\shape\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	.if \scalar
	.set .Lx86_vector_fp_input_size\@, (\element / 8)
	.else
	.set .Lx86_vector_fp_input_size\@, (\width / 8)
	.endif
	page_cross_check (.Lx86_vector_fp_input_size\@), .Lx86_vector_fp_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, .Lx86_vector_fp_input_size\@
	.if \scalar
	.if \element == 32
	ldr s1, [x7]
	.else
	ldr d1, [x7]
	.endif
	.else
	ldr q1, [x7]
	.if \width == 256
	ldr q3, [x7, #16]
	.endif
	.endif
	b .Lx86_vector_fp_loaded\@
.Lx86_vector_fp_cross\@:
	.if \scalar
	mov x0, _context
	mov x1, x25
	mov w2, #.Lx86_vector_fp_input_size\@
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	.if \element == 32
	fmov s1, w0
	.else
	fmov d1, x0
	.endif
	.else
	sub sp, sp, #32
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #.Lx86_vector_fp_input_size\@
	bl NAME(uml_x86_64_gadget_cross_page_vector_load)
	cbz w0, .Lx86_vector_fp_cross_fault\@
	ldr q1, [sp]
	.if \width == 256
	ldr q3, [sp, #16]
	.endif
	add sp, sp, #32
	.endif
.Lx86_vector_fp_loaded\@:
	and x10, x26, #0xffff
	add x10, _cpu, x10
	ldr q0, [x10]
	.if \width == 256
	ldr q2, [x10, #16]
	.endif
	vector_fp_compute \operation, \element, \width, \scalar, \predicate
	vector_fp_write \width, 23
	retire_and_next
	.if !\scalar
.Lx86_vector_fp_cross_fault\@:
	add sp, sp, #32
	b .Lx86_memory_fault
	.endif
.endm

.macro vector_fp_family name, operation, element, shape, width, scalar, predicate=-1
	vector_fp_register \name, \operation, \element, \shape, \width, \scalar, \predicate
	vector_fp_memory \name, \operation, \element, \shape, \width, \scalar, \predicate
.endm

.macro vector_fp_operation operation
	vector_fp_family \operation, \operation, 32, 128, 128, 0
	vector_fp_family \operation, \operation, 32, 256, 256, 0
	vector_fp_family \operation, \operation, 32, scalar, 128, 1
	vector_fp_family \operation, \operation, 64, 128, 128, 0
	vector_fp_family \operation, \operation, 64, 256, 256, 0
	vector_fp_family \operation, \operation, 64, scalar, 128, 1
.endm

vector_fp_operation sqrt
vector_fp_operation rsqrt
vector_fp_operation rcp
vector_fp_operation add
vector_fp_operation mul
vector_fp_operation sub
vector_fp_operation min
vector_fp_operation div
vector_fp_operation max

.macro vector_fp_predicate predicate
	vector_fp_family cmp_\predicate, compare, 32, 128, 128, 0, \predicate
	vector_fp_family cmp_\predicate, compare, 32, 256, 256, 0, \predicate
	vector_fp_family cmp_\predicate, compare, 32, scalar, 128, 1, \predicate
	vector_fp_family cmp_\predicate, compare, 64, 128, 128, 0, \predicate
	vector_fp_family cmp_\predicate, compare, 64, 256, 256, 0, \predicate
	vector_fp_family cmp_\predicate, compare, 64, scalar, 128, 1, \predicate
.endm

vector_fp_predicate 0
vector_fp_predicate 1
vector_fp_predicate 2
vector_fp_predicate 3
vector_fp_predicate 4
vector_fp_predicate 5
vector_fp_predicate 6
vector_fp_predicate 7
vector_fp_predicate 8
vector_fp_predicate 9
vector_fp_predicate 10
vector_fp_predicate 11
vector_fp_predicate 12
vector_fp_predicate 13
vector_fp_predicate 14
vector_fp_predicate 15

.purgem vector_fp_predicate
.purgem vector_fp_operation
.purgem vector_fp_family
.purgem vector_fp_memory
.purgem vector_fp_register
.purgem vector_fp_write
.purgem vector_fp_compute
.purgem vector_fp_fix_generated_nan
.purgem vector_fp_fix_indefinite
.purgem vector_fp_packed_value
.purgem vector_fp_regular_value
.purgem vector_fp_compare_mask
.purgem vector_fp_flush_subnormals
