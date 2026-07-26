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

gadget breakpoint
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_BREAKPOINT
	b uml_arm64_gadget_leave

/* A BlockLink contains {stream, instruction_count, guest_pc}. An unlinked
 * target points at its own {link_exit, guest_pc} stream, so the hot path needs
 * no linked/unlinked test. Resolved edges defer the architectural PC store
 * until an actual exit. Bound chains at 4,096 guest instructions; the outer
 * dispatcher still observes pending shootdowns/IPIs sixteen times within its
 * 64K fairness limit. */
.macro follow_block_link link
	ldp _stream, x11, [\link]
	add x29, x29, x11
	cmp x29, #1, lsl #12
	b.lo 98f
	ldr x9, [\link, #16]
	str x9, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
98:
	next_gadget
.endm

gadget link_exit
	ldr x9, [_stream]
	str x9, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

/*
 * Translation points FP/SIMD BlockLinks at a private
 * {fp_link_entry, target_stream} prefix. Most entries need one predicted
 * loaded-state branch and one dirty-byte store. Only a freshly deferred
 * kernel frame calls the cold materialization helper.
 */
gadget fp_link_entry
	ldrb w8, [_cpu, #UML_GADGET_CPU_FP_LOADED]
	cbz w8, 97f
	mov w8, #1
	strb w8, [_cpu, #UML_GADGET_CPU_FP_DIRTY]
98:
	ldr _stream, [_stream]
	next_gadget
97:
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	bl NAME(uml_arm64_gadget_access_fp_state)
	ldp x1, x2, [sp], #16
	b 98b

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
	ldr w27, [_cpu, #UML_GADGET_CPU_NZCV]
	cbz w0, 1f
	b uml_arm64_gadget_leave
1:	next_gadget

/* Debug-only decoded control-flow instructions terminate the translated
 * block so the interpreter-selected PC cannot be overwritten by its trailer.
 */
gadget decoded_control
	ldr w3, [_stream], #8
	ldr x4, [_stream], #8
	str x4, [_cpu, #UML_GADGET_CPU_PC]
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x2
	mov w2, w3
	bl NAME(uml_arm64_gadget_execute_decoded)
	ldp x1, x2, [sp], #16
	ldr w27, [_cpu, #UML_GADGET_CPU_NZCV]
	b uml_arm64_gadget_leave
#endif

gadget branch
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget branch_linked
	ldr x8, [_stream]
	follow_block_link x8

gadget branch_link
	ldr x8, [_stream], #8
	ldr x9, [_stream]
	str x9, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget branch_link_linked
	ldr x8, [_stream], #8
	ldr x9, [_stream]
	str x9, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	follow_block_link x8

gadget branch_register
	ldr x8, [_stream]
	ldr x10, [_cpu, x8]
	str x10, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget branch_link_register
	ldr x8, [_stream], #8
	ldr x11, [_stream]
	ldr x10, [_cpu, x8]
	str x11, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	str x10, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget return
	ldr x8, [_stream]
	ldr x10, [_cpu, x8]
	str x10, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

/* Two-entry data-only indirect target caches. Translation fixes the source
 * register offset and cache-cell address. A hit immediately follows the
 * already-safe BlockLink; only a miss commits the target and identifies the
 * cell for cold C++ training. */
.macro follow_indirect_link
	ldp x11, x12, [x9]
	cmp x10, x11
	b.eq 98f
	ldp x11, x12, [x9, #16]
	cmp x10, x11
	b.eq 98f
	str x9, [_cpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x10, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
98:
	follow_block_link x12
.endm

gadget branch_register_cached
	ldp x8, x9, [_stream]
	ldr x10, [_cpu, x8]
	follow_indirect_link

gadget branch_link_register_cached
	ldp x8, x9, [_stream], #16
	ldr x11, [_stream]
	ldr x10, [_cpu, x8]
	str x11, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	follow_indirect_link

gadget return_cached
	ldp x8, x9, [_stream]
	ldr x10, [_cpu, x8]
	follow_indirect_link

.purgem follow_indirect_link

/* Authenticated indirect branches. Key, link behavior, zero modifier, and
 * XZR target are fixed by the builder. The ordinary register/register path
 * is straight-line and contains no guest-state validity branch. */
.macro pauth_branch name, authentication, zero_authentication, link, target_zero, modifier_zero
gadget pauth_branch_\name
	.if !\target_zero || !\modifier_zero
	ldr x8, [_stream], #8
	.endif
	.if \target_zero
	mov x9, xzr
	.else
	and w10, w8, #0xffff
	ldr x9, [_cpu, x10]
	.endif
	.if \modifier_zero
	\zero_authentication x9
	.else
	ubfx w11, w8, #16, #16
	ldr x12, [_cpu, x11]
	\authentication x9, x12
	.endif
	.if \link
	ldr x13, [_stream]
	str x13, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	.endif
	str x9, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
.endm

pauth_branch bra_a_rr, autia, autiza, 0, 0, 0
pauth_branch bra_a_rz, autia, autiza, 0, 0, 1
pauth_branch bra_a_zr, autia, autiza, 0, 1, 0
pauth_branch bra_a_zz, autia, autiza, 0, 1, 1
pauth_branch bra_b_rr, autib, autizb, 0, 0, 0
pauth_branch bra_b_rz, autib, autizb, 0, 0, 1
pauth_branch bra_b_zr, autib, autizb, 0, 1, 0
pauth_branch bra_b_zz, autib, autizb, 0, 1, 1
pauth_branch blra_a_rr, autia, autiza, 1, 0, 0
pauth_branch blra_a_rz, autia, autiza, 1, 0, 1
pauth_branch blra_a_zr, autia, autiza, 1, 1, 0
pauth_branch blra_a_zz, autia, autiza, 1, 1, 1
pauth_branch blra_b_rr, autib, autizb, 1, 0, 0
pauth_branch blra_b_rz, autib, autizb, 1, 0, 1
pauth_branch blra_b_zr, autib, autizb, 1, 1, 0
pauth_branch blra_b_zz, autib, autizb, 1, 1, 1

.purgem pauth_branch

/* B.cond's condition is fixed when its data-only program is built.  Selecting
 * one of these precompiled gadgets avoids a C++ helper call at every loop
 * edge.  EL0 may write NZCV, so CSEL can evaluate the original guest flags
 * directly without synthesizing a runtime condition-code switch.
 */
.macro conditional_branch name, condition
gadget branch_\name
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	msr nzcv, x27
	csel x9, x19, x20, \condition
	str x9, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
.endm

.macro conditional_branch_linked name, condition
gadget branch_\name\()_linked
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	msr nzcv, x27
	csel x8, x19, x20, \condition
	follow_block_link x8
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

conditional_branch_linked eq, eq
conditional_branch_linked ne, ne
conditional_branch_linked hs, hs
conditional_branch_linked lo, lo
conditional_branch_linked mi, mi
conditional_branch_linked pl, pl
conditional_branch_linked vs, vs
conditional_branch_linked vc, vc
conditional_branch_linked hi, hi
conditional_branch_linked ls, ls
conditional_branch_linked ge, ge
conditional_branch_linked lt, lt
conditional_branch_linked gt, gt
conditional_branch_linked le, le

/* FEAT_CMPBR register/immediate branches. The builder fixes the comparison,
 * width, condition, and XZR roles. Equal-register and XZR/immediate constant
 * cases are folded to branch_always before reaching these branch-free forms.
 */
.macro cmpbr_finish condition
	csel x15, x19, x20, \condition
	str x15, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
.endm

.macro cmpbr_immediate size_name, size, name, condition
gadget cmpbr_i_\size_name\()_\name
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	and w9, w8, #0x1f
	ubfx w13, w8, #8, #6
	add x17, _cpu, #UML_GADGET_CPU_X0
	ldr x12, [x17, x9, lsl #3]
	.if \size == 32
	mov w12, w12
	cmp w12, w13
	.else
	cmp x12, x13
	.endif
	cmpbr_finish \condition
.endm

.macro cmpbr_immediate_size size_name, size
cmpbr_immediate \size_name, \size, sgt, gt
cmpbr_immediate \size_name, \size, slt, lt
cmpbr_immediate \size_name, \size, ugt, hi
cmpbr_immediate \size_name, \size, ult, lo
cmpbr_immediate \size_name, \size, eq, eq
cmpbr_immediate \size_name, \size, ne, ne
.endm

cmpbr_immediate_size w, 32
cmpbr_immediate_size x, 64

.macro cmpbr_register size_name, size, name, condition, signed, t_zero, m_zero, suffix
gadget cmpbr_r_\size_name\()_\name\()_\suffix
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	.if \t_zero
	mov x12, xzr
	.else
	and w9, w8, #0x1f
	add x17, _cpu, #UML_GADGET_CPU_X0
	ldr x12, [x17, x9, lsl #3]
	.endif
	.if \m_zero
	mov x13, xzr
	.else
	ubfx w10, w8, #8, #5
	add x17, _cpu, #UML_GADGET_CPU_X0
	ldr x13, [x17, x10, lsl #3]
	.endif
	.if \size == 8
		.if \signed
		sxtb w12, w12
		sxtb w13, w13
		.else
		uxtb w12, w12
		uxtb w13, w13
		.endif
		cmp w12, w13
	.elseif \size == 16
		.if \signed
		sxth w12, w12
		sxth w13, w13
		.else
		uxth w12, w12
		uxth w13, w13
		.endif
		cmp w12, w13
	.elseif \size == 32
	cmp w12, w13
	.else
	cmp x12, x13
	.endif
	cmpbr_finish \condition
.endm

.macro cmpbr_register_modes size_name, size, name, condition, signed
cmpbr_register \size_name, \size, \name, \condition, \signed, 0, 0, rr
cmpbr_register \size_name, \size, \name, \condition, \signed, 0, 1, rz
cmpbr_register \size_name, \size, \name, \condition, \signed, 1, 0, zr
.endm

.macro cmpbr_register_size size_name, size
cmpbr_register_modes \size_name, \size, sgt, gt, 1
cmpbr_register_modes \size_name, \size, sge, ge, 1
cmpbr_register_modes \size_name, \size, ugt, hi, 0
cmpbr_register_modes \size_name, \size, uge, hs, 0
cmpbr_register_modes \size_name, \size, eq, eq, 0
cmpbr_register_modes \size_name, \size, ne, ne, 0
.endm

cmpbr_register_size b, 8
cmpbr_register_size h, 16
cmpbr_register_size w, 32
cmpbr_register_size x, 64

/* A64 treats both condition encodings 14 and 15 as always true. */
gadget branch_always
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget branch_always_linked
	ldr x8, [_stream]
	follow_block_link x8

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

gadget compare_branch_linked
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
	cbz w11, 4f
	cmp x13, #0
	cset w14, ne
4:	cmp w14, #0
	csel x8, x19, x20, ne
	follow_block_link x8

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

gadget test_branch_linked
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
	csel x8, x19, x20, eq
	follow_block_link x8

.purgem conditional_branch_linked
.purgem follow_block_link
