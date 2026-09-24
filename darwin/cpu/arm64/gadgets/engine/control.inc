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

gadget illegal
	ldr x8, [_stream]
	str _stream, [_cpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_ILLEGAL
	b uml_arm64_gadget_leave

gadget breakpoint
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_BREAKPOINT
	b uml_arm64_gadget_leave

gadget gcs_fault
	ldr x8, [_stream]
	str _stream, [_cpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_GCS_FAULT
	b uml_arm64_gadget_leave

/* A BlockLink contains {stream, instruction_count, guest_pc}. An unlinked
 * target points at the one-word {link_exit} stream in front of it, so the hot
 * path needs no linked/unlinked test. Resolved edges defer the architectural PC store
 * until an actual exit. Bound chains at 4,096 guest instructions; the
 * executor still observes pending shootdowns/IPIs sixteen times within its
 * 64K fairness limit. */
.macro follow_block_link link
	ldp _stream, x11, [\link]
	add x29, x29, x11
	/* x29 is biased by -4,096 at entry, so a negative total remains below
	 * the retirement limit. Unlike testing bit 12 directly, this stays correct
	 * when callers enter with counts above 8,191. */
	tbnz x29, #63, 98f
	ldr x9, [\link, #16]
	str x9, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
98:
	next_gadget
.endm

/* The stream an unlinked BlockLink names is the link_exit word right in front
 * of that BlockLink, so the guest PC is the BlockLink's own. */
gadget link_exit
	ldr x9, [_stream, #16]
	str x9, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

/* A direct call whose callee was translated inline behind it.
 *
 * All that remains of the call is the link register it sets; control simply
 * continues into the callee's translation, which follows in the stream.
 */
gadget call_inline
	ldr x8, [_stream], #8
	str x8, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	next_gadget

/* The return of an inlined callee.
 *
 * The link register is what the guest will actually branch through, and
 * nothing guarantees the callee left it alone, so the inlined continuation is
 * taken only when it still holds the address the call pushed. Otherwise the
 * block gives back the precharge for everything translated behind this point
 * and falls into the ordinary cached return, whose operands follow.
 */
gadget return_guarded
	ldp x8, x9, [_stream], #16
	ldr x10, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	cmp x10, x8
	b.ne 99f
	add _stream, _stream, #16
	next_gadget
99:
	sub x29, x29, x9
	b NAME(uml_arm64_gadget_return_cached)

/*
 * A block that accesses FP/SIMD state keeps this entry word in front of its
 * stream, and links to the block name that word, so the entry falls through
 * into the block. Most entries need one predicted loaded-state branch and one
 * dirty-byte store. Only a freshly deferred kernel frame calls the cold
 * materialization helper.
 */
gadget fp_link_entry
	ldrb w8, [_cpu, #UML_GADGET_CPU_FP_LOADED]
	cbz w8, 97f
	mov w8, #1
	strb w8, [_cpu, #UML_GADGET_CPU_FP_DIRTY]
98:
	next_gadget
97:
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	bl NAME(uml_arm64_gadget_access_fp_state)
	ldp x1, x2, [sp], #16
	b 98b

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

/* Return predictor: a linked call pushes {return pc, return BlockLink} on a
 * sixteen-record ring in CpuState. Seven instructions per call; the ring is
 * private engine state that a cached return consults only after its site
 * cells miss, so a wrong or stale record costs nothing but the old exit. */
.macro push_return_record pc, link
	ldrb w12, [_cpu, #UML_GADGET_CPU_RETURN_TOP]
	add x13, _cpu, #UML_GADGET_CPU_RETURN_STACK
	add x13, x13, x12, lsl #4
	stp \pc, \link, [x13]
	add w12, w12, #1
	and w12, w12, #UML_GADGET_CPU_RETURN_MASK
	strb w12, [_cpu, #UML_GADGET_CPU_RETURN_TOP]
.endm

/* With the return ring built in, the call's second operand is the return
 * block's link rather than the return address, which that BlockLink holds. */
gadget branch_link_linked
	ldr x8, [_stream], #8
#if UML_GADGET_RETURN_STACK
	ldr x11, [_stream]
	ldr x9, [x11, #16]
	str x9, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	push_return_record x9, x11
	follow_block_link x8
#else
	ldr x9, [_stream]
	str x9, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	follow_block_link x8
	/* Matched-layout control for the return-stack build; unreachable. */
	.space 32
#endif

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
.macro follow_indirect_link ras=0
	ldp x11, x12, [x9]
	cmp x10, x11
	b.eq 98f
	ldp x11, x12, [x9, #16]
	cmp x10, x11
	b.eq 98f
#if UML_GADGET_INDIRECT_ENTRIES >= 4
	ldp x11, x12, [x9, #32]
	cmp x10, x11
	b.eq 98f
	ldp x11, x12, [x9, #48]
	cmp x10, x11
	b.eq 98f
#endif
	.if \ras
#if UML_GADGET_RETURN_STACK
	/* The newest return record is popped only when it names the live link
	 * register; anything else leaves the ring alone and exits as before. */
	ldrb w13, [_cpu, #UML_GADGET_CPU_RETURN_TOP]
	sub w13, w13, #1
	and w13, w13, #UML_GADGET_CPU_RETURN_MASK
	add x14, _cpu, #UML_GADGET_CPU_RETURN_STACK
	add x14, x14, x13, lsl #4
	ldp x11, x12, [x14]
	cmp x10, x11
	b.ne 97f
	strb w13, [_cpu, #UML_GADGET_CPU_RETURN_TOP]
	/* Train the site's next cell as the cold helper would, so the ordinary
	 * cell path hits this target from now on and the ring is consulted
	 * only when the site is more polymorphic than its cells. */
	ldrb w13, [x9, #(16 * UML_GADGET_INDIRECT_ENTRIES)]
	and w13, w13, #(UML_GADGET_INDIRECT_ENTRIES - 1)
	add x14, x9, x13, lsl #4
	stp x10, x12, [x14]
	add w13, w13, #1
	and w13, w13, #(UML_GADGET_INDIRECT_ENTRIES - 1)
	strb w13, [x9, #(16 * UML_GADGET_INDIRECT_ENTRIES)]
	b 98f
97:
#endif
	.endif
	str x9, [_cpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x10, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
98:
	follow_block_link x12
#if UML_GADGET_INDIRECT_ENTRIES < 4
	/* Matched-layout control: keep every later entry at the address the
	 * four-cell build gives it; unreachable. */
	.space 24
#endif
	.if \ras
#if !UML_GADGET_RETURN_STACK
	/* Matched-layout control for the return-stack build; unreachable. */
	.space 68
#endif
	.endif
.endm

gadget branch_register_cached
	ldp x8, x9, [_stream]
	ldr x10, [_cpu, x8]
	follow_indirect_link

/* As branch_link_linked, the return address comes out of the return block's
 * link when the ring is built in. */
gadget branch_link_register_cached
	ldp x8, x9, [_stream], #16
#if UML_GADGET_RETURN_STACK
	ldr x14, [_stream]
	ldr x10, [_cpu, x8]
	ldr x11, [x14, #16]
	str x11, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	push_return_record x11, x14
	follow_indirect_link
#else
	ldr x11, [_stream]
	ldr x10, [_cpu, x8]
	str x11, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	follow_indirect_link
	/* Matched-layout control for the return-stack build; unreachable. */
	.space 32
#endif

gadget return_cached
	ldp x8, x9, [_stream]
	ldr x10, [_cpu, x8]
	follow_indirect_link 1

.purgem follow_indirect_link
.purgem push_return_record

/* Authenticated indirect branches. Key, link behavior, zero modifier, and
 * XZR target are fixed by the builder. The ordinary register/register path
 * is straight-line and contains no guest-state validity branch. */
.macro pauth_branch name, operation, link, target_zero, modifier_zero
gadget pauth_branch_\name
	.if !\target_zero || !\modifier_zero
	ldr x8, [_stream], #8
	.endif
	.if \target_zero
	mov x9, xzr
	.elseif \modifier_zero
	ldr x9, [_cpu, x8]
	.else
	and w10, w8, #0xffff
	ldr x9, [_cpu, x10]
	.endif
	.if \modifier_zero
	software_pointer_auth x9, xzr, \operation
	.elseif \target_zero
	ldr x12, [_cpu, x8]
	software_pointer_auth x9, x12, \operation
	.else
	ubfx w11, w8, #16, #16
	ldr x12, [_cpu, x11]
	software_pointer_auth x9, x12, \operation
	.endif
	.if \link
	ldr x13, [_stream]
	str x13, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	.endif
	str x9, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
.endm

pauth_branch bra_a_rr, 4, 0, 0, 0
pauth_branch bra_a_rz, 4, 0, 0, 1
pauth_branch bra_a_zr, 4, 0, 1, 0
pauth_branch bra_a_zz, 4, 0, 1, 1
pauth_branch bra_b_rr, 5, 0, 0, 0
pauth_branch bra_b_rz, 5, 0, 0, 1
pauth_branch bra_b_zr, 5, 0, 1, 0
pauth_branch bra_b_zz, 5, 0, 1, 1
pauth_branch blra_a_rr, 4, 1, 0, 0
pauth_branch blra_a_rz, 4, 1, 0, 1
pauth_branch blra_a_zr, 4, 1, 1, 0
pauth_branch blra_a_zz, 4, 1, 1, 1
pauth_branch blra_b_rr, 5, 1, 0, 0
pauth_branch blra_b_rz, 5, 1, 0, 1
pauth_branch blra_b_zr, 5, 1, 1, 0
pauth_branch blra_b_zz, 5, 1, 1, 1

.purgem pauth_branch

/* Guarded authenticated calls are selected only for a guest profile which
 * exposes the optional control stack. Translation still fixes the key and
 * zero-register forms, while the normal call entries above remain unchanged.
 * The helper validates all pages before committing a record.
 */
.macro pauth_guarded_call name, operation, target_zero, modifier_zero
gadget pauth_branch_blra_gcs_\name
	.if !\target_zero || !\modifier_zero
	ldr x8, [_stream], #8
	.endif
	.if \target_zero
	mov x9, xzr
	.elseif \modifier_zero
	ldr x9, [_cpu, x8]
	.else
	and w10, w8, #0xffff
	ldr x9, [_cpu, x10]
	.endif
	.if \modifier_zero
	software_pointer_auth x9, xzr, \operation
	.elseif \target_zero
	ldr x12, [_cpu, x8]
	software_pointer_auth x9, x12, \operation
	.else
	ubfx w11, w8, #16, #16
	ldr x12, [_cpu, x11]
	software_pointer_auth x9, x12, \operation
	.endif
	str x9, [_cpu, #UML_GADGET_CPU_ZERO]
	ldr x13, [_stream]
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x2
	mov x2, x13
	bl NAME(uml_arm64_guarded_call_record)
	ldp x1, x2, [sp], #16
	cbnz w0, 97f
	ldr x9, [_cpu, #UML_GADGET_CPU_ZERO]
	ldr x13, [_stream]
	str x13, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	str x9, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
97:
	ldr x8, [_stream]
	sub x8, x8, #4
	str _stream, [_cpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	cmp w0, #1
	mov w8, #UML_GADGET_EVENT_MEMORY_FAULT
	mov w9, #UML_GADGET_EVENT_GCS_FAULT
	csel w0, w8, w9, eq
	b uml_arm64_gadget_leave
.endm

pauth_guarded_call a_rr, 4, 0, 0
pauth_guarded_call a_rz, 4, 0, 1
pauth_guarded_call a_zr, 4, 1, 0
pauth_guarded_call a_zz, 4, 1, 1
pauth_guarded_call b_rr, 5, 0, 0
pauth_guarded_call b_rz, 5, 0, 1
pauth_guarded_call b_zr, 5, 1, 0
pauth_guarded_call b_zz, 5, 1, 1

.purgem pauth_guarded_call

/* Guarded authenticated returns always use LR and SP. The checked target is
 * kept in the private zero slot across the helper call. On a failed read or
 * record comparison, _stream already addresses the faulting guest PC.
 */
.macro pauth_guarded_return name, operation
gadget pauth_return_gcs_\name
	ldr x8, [_stream], #8
	and w10, w8, #0xffff
	ldr x9, [_cpu, x10]
	ubfx w11, w8, #16, #16
	ldr x12, [_cpu, x11]
	software_pointer_auth x9, x12, \operation
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x2
	mov x2, x9
	bl NAME(uml_arm64_guarded_return_record)
	ldp x1, x2, [sp], #16
	cbnz w0, 97f
	ldr x9, [_cpu, #UML_GADGET_CPU_ZERO]
	str x9, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
97:
	cmp w0, #1
	b.eq uml_arm64_gadget_memory_fault
	b NAME(uml_arm64_gadget_gcs_fault)
.endm

pauth_guarded_return a, 4
pauth_guarded_return b, 5

.purgem pauth_guarded_return

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

/* A conditional branch whose fall-through is translated inline behind it.
 *
 * A block otherwise ends at its first branch, which on a browser guest means a
 * mean block of five instructions -- too short for the per-block costs to
 * amortize against. Continuing through the fall-through keeps the guest
 * addresses contiguous, so nothing about validation or fault accounting
 * changes, and the not-taken case stops being an indirect jump through a link
 * at all.
 *
 * The stream holds the taken link and, where the fall-through link used to sit,
 * the number of instructions translated after this one. Entering a block
 * precharges its whole length, so a taken branch leaving from the middle has to
 * give that suffix back before following its link.
 */
.macro conditional_branch_inline name, condition
gadget branch_\name\()_inline
	ldr x19, [_stream], #8
	ldr x20, [_stream], #8
	msr nzcv, x27
	b.\condition 99f
	next_gadget
99:
	sub x29, x29, x20
	follow_block_link x19
.endm

/* G-2 superinstruction: guest CMP of two registers followed by B.cond.
 *
 * G116's histogram over a full Geekbench run ranks this pair fourth and ninth
 * of 47,494 - `branch_eq_linked` after it at 2.65% and `branch_ne_linked` at
 * 0.84%, 3.49% together - while the immediate form fused in G111 does not reach
 * the table at all. Same mechanism, the half of the instruction class that
 * actually occurs.
 *
 * Stream operand is the compare's own encoding, whose left and right register
 * byte offsets sit at bits 16 and 32, followed by the taken and fall-through
 * words. Only the unshifted register-register form with an XZR destination is
 * fused; a shift, an extension or a real destination falls through to the pair.
 */
.macro compare_register_branch bits64, width, name, condition
gadget compare_register_branch_\width\()_\name
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	ubfx x10, x8, #16, #16
	ldr x15, [_cpu, x10]
	ubfx x11, x8, #32, #16
	ldr x16, [_cpu, x11]
	.if \bits64
	subs xzr, x15, x16
	.else
	subs wzr, w15, w16
	.endif
	mrs x27, nzcv
	csel x9, x19, x20, \condition
	str x9, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
.endm

.macro compare_register_branch_linked bits64, width, name, condition
gadget compare_register_branch_\width\()_\name\()_linked
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	ubfx x10, x8, #16, #16
	ldr x15, [_cpu, x10]
	ubfx x11, x8, #32, #16
	ldr x16, [_cpu, x11]
	.if \bits64
	subs xzr, x15, x16
	.else
	subs wzr, w15, w16
	.endif
	mrs x27, nzcv
	csel x8, x19, x20, \condition
	follow_block_link x8
.endm

/* Fall-through translated inline; see conditional_branch_inline. */
.macro compare_register_branch_inline bits64, width, name, condition
gadget compare_register_branch_\width\()_\name\()_inline
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	lsr x20, x8, #48
	ubfx x10, x8, #16, #16
	ldr x15, [_cpu, x10]
	ubfx x11, x8, #32, #16
	ldr x16, [_cpu, x11]
	.if \bits64
	subs xzr, x15, x16
	.else
	subs wzr, w15, w16
	.endif
	mrs x27, nzcv
	b.\condition 99f
	next_gadget
99:
	sub x29, x29, x20
	follow_block_link x19
.endm

.macro compare_register_branch_pair name, condition
compare_register_branch 1, x, \name, \condition
compare_register_branch 0, w, \name, \condition
compare_register_branch_linked 1, x, \name, \condition
compare_register_branch_linked 0, w, \name, \condition
compare_register_branch_inline 1, x, \name, \condition
compare_register_branch_inline 0, w, \name, \condition
.endm

compare_register_branch_pair eq, eq
compare_register_branch_pair ne, ne
compare_register_branch_pair hs, hs
compare_register_branch_pair lo, lo
compare_register_branch_pair mi, mi
compare_register_branch_pair pl, pl
compare_register_branch_pair vs, vs
compare_register_branch_pair vc, vc
compare_register_branch_pair hi, hi
compare_register_branch_pair ls, ls
compare_register_branch_pair ge, ge
compare_register_branch_pair lt, lt
compare_register_branch_pair gt, gt
compare_register_branch_pair le, le

.purgem compare_register_branch_pair
.purgem compare_register_branch
.purgem compare_register_branch_linked
.purgem compare_register_branch_inline

/* G-2 superinstruction: guest CMP-immediate immediately followed by B.cond.
 *
 * That pair is the most common control-flow shape there is, and unfused it
 * costs a full threaded dispatch between two gadgets that have nothing to say
 * to each other except the flags. Worse, the flags make the round trip through
 * a system register twice - the compare ends with `mrs x27, nzcv` and the
 * branch begins with `msr nzcv, x27` - purely because the two halves are
 * separate entries.
 *
 * Fusing removes the dispatch and the `msr`. The `mrs` stays: the guest's NZCV
 * is architecturally live after the branch, so the block must leave it in x27
 * whichever way the branch goes. MRS does not disturb PSTATE, so it can sit
 * between the SUBS and the CSEL that reads its flags.
 *
 * Stream operand is the compare's own encoding - Rn at bits 8..12, immediate
 * from bit 16 - followed by the taken and fall-through words the conditional
 * branch already took. Rn == 31 means SP for this instruction class and is left
 * to the unfused path rather than given a variant.
 */
.macro compare_immediate_branch bits64, width, name, condition
gadget compare_immediate_branch_\width\()_\name
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	ubfx w10, w8, #8, #5
	ldr x15, [_cpu, x10, lsl #3]
	lsr x11, x8, #16
	.if \bits64
	subs xzr, x15, x11
	.else
	subs wzr, w15, w11
	.endif
	mrs x27, nzcv
	csel x9, x19, x20, \condition
	str x9, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
.endm

.macro compare_immediate_branch_linked bits64, width, name, condition
gadget compare_immediate_branch_\width\()_\name\()_linked
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	ldr x20, [_stream]
	ubfx w10, w8, #8, #5
	ldr x15, [_cpu, x10, lsl #3]
	lsr x11, x8, #16
	.if \bits64
	subs xzr, x15, x11
	.else
	subs wzr, w15, w11
	.endif
	mrs x27, nzcv
	csel x8, x19, x20, \condition
	follow_block_link x8
.endm

/* Fall-through translated inline; see conditional_branch_inline. */
.macro compare_immediate_branch_inline bits64, width, name, condition
gadget compare_immediate_branch_\width\()_\name\()_inline
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	lsr x20, x8, #40
	ubfx w10, w8, #8, #5
	ldr x15, [_cpu, x10, lsl #3]
	ubfx x11, x8, #16, #24
	.if \bits64
	subs xzr, x15, x11
	.else
	subs wzr, w15, w11
	.endif
	mrs x27, nzcv
	b.\condition 99f
	next_gadget
99:
	sub x29, x29, x20
	follow_block_link x19
.endm

.macro compare_immediate_branch_pair name, condition
compare_immediate_branch 1, x, \name, \condition
compare_immediate_branch 0, w, \name, \condition
compare_immediate_branch_linked 1, x, \name, \condition
compare_immediate_branch_linked 0, w, \name, \condition
compare_immediate_branch_inline 1, x, \name, \condition
compare_immediate_branch_inline 0, w, \name, \condition
.endm

compare_immediate_branch_pair eq, eq
compare_immediate_branch_pair ne, ne
compare_immediate_branch_pair hs, hs
compare_immediate_branch_pair lo, lo
compare_immediate_branch_pair mi, mi
compare_immediate_branch_pair pl, pl
compare_immediate_branch_pair vs, vs
compare_immediate_branch_pair vc, vc
compare_immediate_branch_pair hi, hi
compare_immediate_branch_pair ls, ls
compare_immediate_branch_pair ge, ge
compare_immediate_branch_pair lt, lt
compare_immediate_branch_pair gt, gt
compare_immediate_branch_pair le, le

.purgem compare_immediate_branch_pair
.purgem compare_immediate_branch
.purgem compare_immediate_branch_linked
.purgem compare_immediate_branch_inline

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
conditional_branch_inline eq, eq
conditional_branch_inline ne, ne
conditional_branch_inline hs, hs
conditional_branch_inline lo, lo
conditional_branch_inline mi, mi
conditional_branch_inline pl, pl
conditional_branch_inline vs, vs
conditional_branch_inline vc, vc
conditional_branch_inline hi, hi
conditional_branch_inline ls, ls
conditional_branch_inline ge, ge
conditional_branch_inline lt, lt
conditional_branch_inline gt, gt
conditional_branch_inline le, le

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

/* CBZ/CBNZ width, polarity, source offset, and both successors are fixed by
 * translation.  The valid runtime path is straight-line and contains no
 * opcode extraction, register-number test, or conditional branch. */
.macro compare_branch width_name, width, polarity, condition
gadget compare_branch_\width_name\()_\polarity
	ldr x8, [_stream], #8
	ldp x19, x20, [_stream]
	ldr \width\()9, [_cpu, x8]
	cmp \width\()9, #0
	csel x10, x19, x20, \condition
	str x10, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget compare_branch_\width_name\()_\polarity\()_linked
	ldr x8, [_stream], #8
	ldp x19, x20, [_stream]
	ldr \width\()9, [_cpu, x8]
	cmp \width\()9, #0
	csel x8, x19, x20, \condition
	follow_block_link x8

/* Fall-through translated inline; see conditional_branch_inline. */
gadget compare_branch_\width_name\()_\polarity\()_inline
	ldr x8, [_stream], #8
	ldr x19, [_stream], #8
	lsr x20, x8, #16
	and x8, x8, #0xffff
	ldr \width\()9, [_cpu, x8]
	cmp \width\()9, #0
	b.\condition 99f
	next_gadget
99:
	sub x29, x29, x20
	follow_block_link x19
.endm

compare_branch w, w, z, eq
compare_branch w, w, nz, ne
compare_branch x, x, z, eq
compare_branch x, x, nz, ne

/* TBZ/TBNZ use a translation-computed one-bit mask.  This removes all
 * instruction-field extraction and the variable shift from the hot path. */
.macro test_branch polarity, condition
gadget test_branch_\polarity
	ldp x8, x9, [_stream], #16
	ldp x19, x20, [_stream]
	ldr x10, [_cpu, x8]
	tst x10, x9
	csel x11, x19, x20, \condition
	str x11, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget test_branch_\polarity\()_linked
	ldp x8, x9, [_stream], #16
	ldp x19, x20, [_stream]
	ldr x10, [_cpu, x8]
	tst x10, x9
	csel x8, x19, x20, \condition
	follow_block_link x8

/* Fall-through translated inline; see conditional_branch_inline. */
gadget test_branch_\polarity\()_inline
	ldr x8, [_stream], #8
	ldr x9, [_stream], #8
	ldr x19, [_stream], #8
	lsr x20, x8, #16
	and x8, x8, #0xffff
	ldr x10, [_cpu, x8]
	tst x10, x9
	b.\condition 99f
	next_gadget
99:
	sub x29, x29, x20
	follow_block_link x19
.endm

test_branch z, eq
test_branch nz, ne

.purgem test_branch
.purgem compare_branch

.purgem conditional_branch_linked
.purgem follow_block_link
