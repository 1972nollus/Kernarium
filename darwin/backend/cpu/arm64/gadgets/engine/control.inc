gadget svc
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_SYSCALL
	b uml_arm64_gadget_leave

gadget interrupt
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_INTERRUPT
	b uml_arm64_gadget_leave

gadget fatal
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mvn w0, wzr
	b uml_arm64_gadget_leave

gadget illegal
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_ILLEGAL
	b uml_arm64_gadget_leave

#if UML_ENABLE_GADGET_INTERPRETER_RUNPATH
/* Explicit debug-only interpreter fallback. Production builds do not contain
 * this gadget or its C++ dispatcher bridge.
 */
gadget decoded
	ldr w3, [_stream], #8
	ldr x4, [_stream], #8
	str x4, [_cpu, #UML_GADGET_CPU_PC]
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x2
	mov w2, w3
	bl NAME(uml_arm64_gadget_execute_decoded)
	ldp x1, x2, [sp], #16
	cbz w0, 1f
	b uml_arm64_gadget_leave
1:	next_gadget
#endif

gadget branch
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget branch_link
	ldr x8, [_stream], #8
	ldr x9, [_stream]
	str x9, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget branch_register
	ldr x8, [_stream]
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x10, [x9, x8, lsl #3]
	str x10, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget branch_link_register
	ldr x8, [_stream], #8
	ldr x11, [_stream]
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x10, [x9, x8, lsl #3]
	str x11, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	str x10, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget return
	ldr x8, [_stream]
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x10, [x9, x8, lsl #3]
	str x10, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

/* B.cond's condition is fixed when its data-only program is built.  Selecting
 * one of these precompiled gadgets avoids a C++ helper call at every loop
 * edge.  EL0 may write NZCV, so CSEL can evaluate the original guest flags
 * directly without synthesizing a runtime condition-code switch.
 */
.macro conditional_branch name, condition
gadget branch_\name
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	ldr w8, [_cpu, #UML_GADGET_CPU_NZCV]
	msr nzcv, x8
	csel x9, x19, x20, \condition
	str x9, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
.endm

conditional_branch eq, eq
conditional_branch ne, ne
conditional_branch hs, hs
conditional_branch lo, lo
conditional_branch mi, mi
conditional_branch pl, pl
conditional_branch vs, vs
conditional_branch vc, vc
conditional_branch hi, hi
conditional_branch ls, ls
conditional_branch ge, ge
conditional_branch lt, lt
conditional_branch gt, gt
conditional_branch le, le

/* A64 treats both condition encodings 14 and 15 as always true. */
gadget branch_always
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget compare_branch
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	and x9, x8, #0x1f
	ubfx x10, x8, #8, #1
	ubfx x11, x8, #9, #1
	add x12, _cpu, #UML_GADGET_CPU_X0
	cmp x9, #31
	b.eq 1f
	ldr x13, [x12, x9, lsl #3]
	b 2f
1:	mov x13, xzr
2:	cbnz x10, 3f
	and x13, x13, #0xffffffff
3:	cmp x13, #0
	cset w14, eq
	cmp w11, #0
	csel w14, w14, wzr, eq
	/* nonzero branches invert the zero result */
	cbz w11, 4f
	cmp x13, #0
	cset w14, ne
4:	cmp w14, #0
	csel x15, x19, x20, ne
	str x15, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget test_branch
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	and x9, x8, #0x1f
	ubfx x10, x8, #8, #6
	ubfx x11, x8, #16, #1
	cmp x9, #31
	b.eq 1f
	add x12, _cpu, #UML_GADGET_CPU_X0
	ldr x13, [x12, x9, lsl #3]
	b 2f
1:	mov x13, xzr
2:	lsr x13, x13, x10
	and x13, x13, #1
	cmp x11, #0
	cset w14, ne
	cmp x13, x14
	csel x15, x19, x20, eq
	str x15, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
