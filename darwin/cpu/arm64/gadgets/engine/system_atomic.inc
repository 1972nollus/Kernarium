/* DC ZVA is fixed at 64 bytes by the virtual DCZID_EL0. Translation selects
 * the real-register and XZR forms, and the 64-byte alignment guarantees that
 * one successful guest-page TLB lookup covers every store. Stream:
 * [register byte offset, when present][guest PC]. */

/* FEAT_GCS helpers are fully selected by translation: operation, XZR
 * discard/source, and GCSSTR SP addressing never reach the runtime decoder.
 * The valid hot path retains one not-taken status branch. On any exception
 * _stream still points at the immutable guest-PC word consumed by the common
 * event gadgets. */
.macro gcs name, operand
gadget gcs_\name
	.if \operand
	ldr x8, [_stream], #8
	.else
	mov x8, xzr
	.endif
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x2
	mov x2, x8
	bl NAME(uml_arm64_gadget_gcs_transfer_\name)
	ldp x1, x2, [sp], #16
	cbnz w0, 97f
	add _stream, _stream, #8
	next_gadget
97:
	cmp w0, #1
	b.eq uml_arm64_gadget_memory_fault
	tbz w0, #0, 98f
	b NAME(uml_arm64_gadget_gcs_fault)
98:
	b NAME(uml_arm64_gadget_illegal)
.endm

gcs popm_r, 1
gcs popm_z, 0
gcs pushm_r, 1
gcs pushm_z, 0
gcs ss1_r, 1
gcs ss1_z, 0
gcs ss2_r, 1
gcs ss2_z, 0

.purgem gcs

.macro gcs_store name
gadget gcs_store_\name
	ldr x8, [_stream], #8
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x2
	mov x2, x8
	bl NAME(uml_arm64_gadget_gcs_transfer_store_\name)
	ldp x1, x2, [sp], #16
	cbnz w0, 97f
	add _stream, _stream, #8
	next_gadget
97:
	cmp w0, #1
	b.eq uml_arm64_gadget_memory_fault
	tbz w0, #0, 98f
	b NAME(uml_arm64_gadget_gcs_fault)
98:
	b NAME(uml_arm64_gadget_illegal)
.endm

gcs_store rr
gcs_store rz
gcs_store sr
gcs_store sz

.purgem gcs_store

.macro cache_zero name, zero
gadget cache_zero_\name
	.if \zero
	mov x7, xzr
	.else
	ldr x8, [_stream], #8
	ldr x7, [_cpu, x8]
	ldrh w9, [_cpu, #UML_GADGET_CPU_TAGGED_ADDRESS_STATE]
	cbz w9, .Lcache_zero_untagged\@
	ubfx x7, x7, #0, #56
.Lcache_zero_untagged\@:
	and x7, x7, #0xffffffffffffffc0
	.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	stp xzr, xzr, [x7]
	stp xzr, xzr, [x7, #16]
	stp xzr, xzr, [x7, #32]
	stp xzr, xzr, [x7, #48]
	add _stream, _stream, #8
	next_gadget
.endm

cache_zero r, 0
cache_zero z, 1

.purgem cache_zero

/*
 * The guest instruction-cache synchronization point is deliberately a cold
 * execution boundary.  The helper publishes a translated-code sequence
 * change, then the executor resumes at the following guest instruction so
 * no stale suffix from the current data-only program can execute.
 * Stream: [register byte offset, when present][fallthrough guest PC].
 */
.macro code_synchronize name, zero
gadget code_synchronize_\name
	.if \zero
	mov x0, xzr
	.else
	ldr x8, [_stream], #8
	ldr x0, [_cpu, x8]
	ldrh w9, [_cpu, #UML_GADGET_CPU_TAGGED_ADDRESS_STATE]
	cbz w9, .Lcode_synchronize_untagged\@
	ubfx x0, x0, #0, #56
.Lcode_synchronize_untagged\@:
	.endif
	stp x1, x2, [sp, #-16]!
	bl NAME(uml_arm64_guest_code_synchronize)
	ldp x1, x2, [sp], #16
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
.endm

code_synchronize r, 0
code_synchronize z, 1

.purgem code_synchronize

/* FEAT_LS64 helpers serialize the 64-byte transaction and validate the TLB
 * before committing architectural state. Translation specializes operation,
 * SP addressing, and discarded status results; only the fault branch remains.
 */
.macro ls64 name
gadget ls64_\name
	ldr x8, [_stream], #8
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x2
	mov x2, x8
	bl NAME(uml_arm64_gadget_ls64_transfer_\name)
	ldp x1, x2, [sp], #16
	cbz w0, uml_arm64_gadget_memory_fault
	add _stream, _stream, #8
	next_gadget
.endm

ls64 load_r
ls64 load_s
ls64 store_r
ls64 store_s
ls64 st64bv_rr
ls64 st64bv_rz
ls64 st64bv_sr
ls64 st64bv_sz
ls64 st64bv0_rr
ls64 st64bv0_rz
ls64 st64bv0_sr
ls64 st64bv0_sz

/* FEAT_MOPS uses architectural option B. Translation specializes the family
 * and stage. Prologue formatting has no fault path; main is the ordinary nop
 * gadget; epilogues use the largest permitted 64-byte, page-contained host
 * bulk operations and retain only fault and malformed-stage-state branches. */
.macro mops_prologue name
gadget mops_prologue_\name
	ldr x8, [_stream], #8
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x8
	bl NAME(uml_arm64_gadget_mops_prologue_transfer_\name)
	ldp x1, x2, [sp], #16
	ldr w27, [_cpu, #UML_GADGET_CPU_NZCV]
	next_gadget
.endm

.macro mops_epilogue name
gadget mops_epilogue_\name
	ldr x8, [_stream], #8
	stp x1, x2, [sp, #-16]!
	str w27, [_cpu, #UML_GADGET_CPU_NZCV]
	mov x0, x1
	mov x1, x2
	mov x2, x8
	bl NAME(uml_arm64_gadget_mops_epilogue_transfer_\name)
	ldp x1, x2, [sp], #16
	ldr w27, [_cpu, #UML_GADGET_CPU_NZCV]
	cbz w0, uml_arm64_gadget_memory_fault
	tbz w0, #1, 1f
	b NAME(uml_arm64_gadget_illegal)
1:
	add _stream, _stream, #8
	next_gadget
.endm

mops_prologue copy_forward
mops_prologue copy_general
mops_prologue set
mops_epilogue copy_forward
mops_epilogue copy_general
mops_epilogue set_r
mops_epilogue set_z

gadget mops_set_tag_prologue
	ldr x8, [_stream], #8
	stp x1, x2, [sp, #-16]!
	str w27, [_cpu, #UML_GADGET_CPU_NZCV]
	mov x0, x1
	mov x1, x2
	mov x2, x8
	bl NAME(uml_arm64_gadget_mops_set_tag_prologue_transfer)
	ldp x1, x2, [sp], #16
	ldr w27, [_cpu, #UML_GADGET_CPU_NZCV]
	cbz w0, uml_arm64_gadget_memory_fault
	add _stream, _stream, #8
	next_gadget

.macro mops_set_tag_epilogue name
gadget mops_set_tag_epilogue_\name
	ldr x8, [_stream], #8
	stp x1, x2, [sp, #-16]!
	str w27, [_cpu, #UML_GADGET_CPU_NZCV]
	mov x0, x1
	mov x1, x2
	mov x2, x8
	bl NAME(uml_arm64_gadget_mops_set_tag_epilogue_transfer_\name)
	ldp x1, x2, [sp], #16
	ldr w27, [_cpu, #UML_GADGET_CPU_NZCV]
	cbz w0, uml_arm64_gadget_memory_fault
	tbz w0, #1, 1f
	b NAME(uml_arm64_gadget_illegal)
1:	add _stream, _stream, #8
	next_gadget
.endm

mops_set_tag_epilogue r
mops_set_tag_epilogue z

.purgem mops_set_tag_epilogue

/* FEAT_MTE tag-memory operations are split by address mode, SP alignment,
 * and discarded STGP/LDG registers at translation time. The C++ helpers use
 * direct CpuState offsets and host bulk stores, leaving only dynamic alignment
 * and mapping-fault branches in valid production execution.
 */
.macro mte_memory name
gadget mte_memory_\name
	ldr x8, [_stream], #8
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x2
	mov x2, x8
	bl NAME(uml_arm64_gadget_mte_memory_transfer_\name)
	ldp x1, x2, [sp], #16
	cbz w0, uml_arm64_gadget_memory_fault
	add _stream, _stream, #8
	next_gadget
.endm

mte_memory ldg_rr
mte_memory ldg_rz
mte_memory ldg_sr
mte_memory ldg_sz

.macro mte_tag_store_modes name
	mte_memory \name\()_post_r
	mte_memory \name\()_post_s
	mte_memory \name\()_offset_r
	mte_memory \name\()_offset_s
	mte_memory \name\()_pre_r
	mte_memory \name\()_pre_s
.endm

mte_tag_store_modes stg
mte_tag_store_modes st2g
mte_tag_store_modes stzg
mte_tag_store_modes stz2g

.purgem mte_tag_store_modes

.macro mte_stgp_registers mode, base
	mte_memory stgp_\mode\()_\base\()rr
	mte_memory stgp_\mode\()_\base\()rz
	mte_memory stgp_\mode\()_\base\()zr
	mte_memory stgp_\mode\()_\base\()zz
.endm

.macro mte_stgp_mode mode
	mte_stgp_registers \mode, r
	mte_stgp_registers \mode, s
.endm

mte_stgp_mode post
mte_stgp_mode offset
mte_stgp_mode pre

.purgem mte_stgp_mode
.purgem mte_stgp_registers
.purgem mte_memory

gadget pc_relative
	ldr x8, [_stream], #8
	ldr x9, [_stream], #8
	str x9, [_cpu, x8]
	next_gadget

/* Translation-time-specialized EL0 system-register accesses.  Operands are
 * direct byte offsets into CpuState::registers, and XZR cases use separate
 * gadgets (or NOP for discarded reads), leaving no register-number or opcode
 * branches in the production hot path. */
.macro sysreg_read64 name, field
gadget sysreg_\name\()_read
	ldr x8, [_stream], #8
	ldr x9, [_cpu, #\field]
	str x9, [_cpu, x8]
	next_gadget
.endm

.macro sysreg_write64 name, field
gadget sysreg_\name\()_write_r
	ldr x8, [_stream], #8
	ldr x9, [_cpu, x8]
	str x9, [_cpu, #\field]
	next_gadget
gadget sysreg_\name\()_write_z
	str xzr, [_cpu, #\field]
	next_gadget
.endm

sysreg_read64 tls, UML_GADGET_CPU_TLS
sysreg_write64 tls, UML_GADGET_CPU_TLS

gadget sysreg_nzcv_read
	ldr x8, [_stream], #8
	str x27, [_cpu, x8]
	next_gadget

.purgem sysreg_write64
.purgem sysreg_read64

gadget sysreg_fpcr_read
	ldr x8, [_stream], #8
	ldr w9, [_cpu, #UML_GADGET_CPU_FPCR]
	mov w10, #0x9f07
	movk w10, #0x07f7, lsl #16
	and w9, w9, w10
	str x9, [_cpu, x8]
	next_gadget

gadget sysreg_dczid_read
	ldr x8, [_stream], #8
	mov x9, #4
	str x9, [_cpu, x8]
	next_gadget

gadget sysreg_zero_read
	ldr x8, [_stream], #8
	str xzr, [_cpu, x8]
	next_gadget

gadget sysreg_ctr_read
	ldr x8, [_stream], #8
	mov w9, #0x8004
	movk w9, #0x8444, lsl #16
	str x9, [_cpu, x8]
	next_gadget

gadget sysreg_cntfrq_read
	ldr x8, [_stream], #8
	mrs x9, cntfrq_el0
	str x9, [_cpu, x8]
	next_gadget

gadget sysreg_cntvct_read
	ldr x8, [_stream], #8
	mrs x9, cntvct_el0
	str x9, [_cpu, x8]
	next_gadget

gadget sysreg_fpsr_read
	ldr x8, [_stream], #8
	ldrb w11, [_cpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbz x11, 8f
	mrs x9, fpsr
	b 9f
8:	ldr w9, [_cpu, #UML_GADGET_CPU_FPSR]
9:
	mov w10, #0x9f
	movk w10, #0xf800, lsl #16
	and w9, w9, w10
	str w9, [_cpu, #UML_GADGET_CPU_FPSR]
	str x9, [_cpu, x8]
	next_gadget

.macro sysreg_fp_write name, field, system, low_mask, high_mask
gadget sysreg_\name\()_write_r
	ldr x8, [_stream], #8
	.ifc \name,fpcr
	ldr x13, [_stream], #8
	mov w12, #0x9f00
	.endif
	ldr w9, [_cpu, x8]
	mov w10, #\low_mask
	movk w10, #\high_mask, lsl #16
	and w9, w9, w10
	str w9, [_cpu, #\field]
	ldrb w11, [_cpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbz x11, 8f
	.ifc \name,fpcr
	bic w10, w9, w12
	msr \system, x10
	.else
	msr \system, x9
	.endif
8:
	.ifc \name,fpcr
	tst w9, w12
	b.eq 9f
	str _stream, [_cpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x13, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
9:
	.endif
	next_gadget
gadget sysreg_\name\()_write_z
	str wzr, [_cpu, #\field]
	ldrb w11, [_cpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbz x11, 9f
	msr \system, xzr
9:
	next_gadget
.endm

sysreg_fp_write fpcr, UML_GADGET_CPU_FPCR, fpcr, 0x9f07, 0x07f7
sysreg_fp_write fpsr, UML_GADGET_CPU_FPSR, fpsr, 0x009f, 0xf800

.purgem sysreg_fp_write

/* FPMR is guest architectural state even when the host lacks FEAT_FP8.  FP8
 * native gadgets synchronize it separately; ordinary reads and writes remain
 * direct and translation-time-specialized. */
gadget sysreg_fpmr_read
	ldr x8, [_stream], #8
	ldr x9, [_cpu, #UML_GADGET_CPU_FPMR]
	mov x10, #0xc1ff
	movk x10, #0xff7f, lsl #16
	movk x10, #0x003f, lsl #32
	and x9, x9, x10
	str x9, [_cpu, x8]
	next_gadget

gadget sysreg_fpmr_write_r
	ldr x8, [_stream], #8
	ldr x9, [_cpu, x8]
	mov x10, #0xc1ff
	movk x10, #0xff7f, lsl #16
	movk x10, #0x003f, lsl #32
	and x9, x9, x10
	str x9, [_cpu, #UML_GADGET_CPU_FPMR]
	next_gadget

gadget sysreg_fpmr_write_z
	str xzr, [_cpu, #UML_GADGET_CPU_FPMR]
	next_gadget

gadget sysreg_nzcv_write_r
	ldr x8, [_stream], #8
	ldr w9, [_cpu, x8]
	and w9, w9, #0xf0000000
	mov w27, w9
	next_gadget

gadget sysreg_nzcv_write_z
	mov w27, wzr
	next_gadget

/* DIT is PSTATE.DIT presented as a system register: the architecture defines
 * the read as Zeros(39):PSTATE.DIT:Zeros(24) and the write as
 * PSTATE.DIT = X[t]<24>, so bit 24 is the only live position in either
 * direction.  Go's runtime reads it through MRS whenever HWCAP_DIT is
 * advertised, so the register form has to exist wherever the MSR immediate
 * form does. */
gadget sysreg_dit_read
	ldr x8, [_stream], #8
	ldrb w9, [_cpu, #UML_GADGET_CPU_PSTATE_DIT]
	lsl x9, x9, #24
	str x9, [_cpu, x8]
	next_gadget

gadget sysreg_dit_write_r
	ldr x8, [_stream], #8
	ldr x9, [_cpu, x8]
	ubfx x9, x9, #24, #1
	strb w9, [_cpu, #UML_GADGET_CPU_PSTATE_DIT]
	next_gadget

gadget sysreg_dit_write_z
	strb wzr, [_cpu, #UML_GADGET_CPU_PSTATE_DIT]
	next_gadget

.macro pstate_store name, offset, value
gadget pstate_\name\()_\value
.if \value
	mov w8, #1
	strb w8, [_cpu, #\offset]
.else
	strb wzr, [_cpu, #\offset]
.endif
	next_gadget
.endm

.macro pstate_store_field name, offset
	pstate_store \name, \offset, 0
	pstate_store \name, \offset, 1
.endm

pstate_store_field ssbs, UML_GADGET_CPU_PSTATE_SSBS
pstate_store_field dit, UML_GADGET_CPU_PSTATE_DIT
pstate_store_field tco, UML_GADGET_CPU_PSTATE_TCO
pstate_store_field sm, UML_GADGET_CPU_PSTATE_SM
pstate_store_field za, UML_GADGET_CPU_PSTATE_ZA

.purgem pstate_store_field
.purgem pstate_store

.macro pstate_smza value
gadget pstate_smza_\value
.if \value
	mov w8, #1
	strb w8, [_cpu, #UML_GADGET_CPU_PSTATE_SM]
	strb w8, [_cpu, #UML_GADGET_CPU_PSTATE_ZA]
.else
	strb wzr, [_cpu, #UML_GADGET_CPU_PSTATE_SM]
	strb wzr, [_cpu, #UML_GADGET_CPU_PSTATE_ZA]
.endif
	next_gadget
.endm

pstate_smza 0
pstate_smza 1

.purgem pstate_smza

.macro pstate_daif name, operation, immediate
gadget pstate_daif_\name\()_\immediate
	ldrb w9, [_cpu, #UML_GADGET_CPU_PSTATE_DAIF_ACCESS]
	cbz w9, NAME(uml_arm64_gadget_illegal)
.if \immediate
	ldrb w9, [_cpu, #UML_GADGET_CPU_PSTATE_DAIF]
	mov w10, #\immediate
	\operation w9, w9, w10
	strb w9, [_cpu, #UML_GADGET_CPU_PSTATE_DAIF]
.endif
	/* The following inline word is the fault PC used only by the gated path. */
	add _stream, _stream, #8
	next_gadget
.endm

.macro pstate_daif_immediate immediate
	pstate_daif set, orr, \immediate
	pstate_daif clear, bic, \immediate
.endm

pstate_daif_immediate 0
pstate_daif_immediate 1
pstate_daif_immediate 2
pstate_daif_immediate 3
pstate_daif_immediate 4
pstate_daif_immediate 5
pstate_daif_immediate 6
pstate_daif_immediate 7
pstate_daif_immediate 8
pstate_daif_immediate 9
pstate_daif_immediate 10
pstate_daif_immediate 11
pstate_daif_immediate 12
pstate_daif_immediate 13
pstate_daif_immediate 14
pstate_daif_immediate 15

.purgem pstate_daif_immediate
.purgem pstate_daif

/* Barrier option is selected during translation, so each hot entry executes
 * exactly one native barrier and the threaded dispatch. */
.macro memory_barrier kind, option
gadget \kind\()_\option
	\kind #\option
	next_gadget
.endm

.macro memory_barrier_option option
	memory_barrier dmb, \option
	memory_barrier dsb, \option
.endm

memory_barrier_option 0
memory_barrier_option 1
memory_barrier_option 2
memory_barrier_option 3
memory_barrier_option 4
memory_barrier_option 5
memory_barrier_option 6
memory_barrier_option 7
memory_barrier_option 8
memory_barrier_option 9
memory_barrier_option 10
memory_barrier_option 11
memory_barrier_option 12
memory_barrier_option 13
memory_barrier_option 14
memory_barrier_option 15

.purgem memory_barrier_option
.purgem memory_barrier

gadget isb
	isb
	next_gadget

gadget speculation_barrier
	/* Native SB; selected only after host-feature detection at translation. */
	.inst 0xd50330ff
	next_gadget

gadget speculation_barrier_fallback
	/* Stronger branch-free sequence for hosts without native SB. */
	dsb sy
	isb
	next_gadget

.macro memory_nxs_barrier option
gadget dsb_nxs_\option
	.inst (0xd503323f | (\option << 10))
	next_gadget
.endm

memory_nxs_barrier 0
memory_nxs_barrier 1
memory_nxs_barrier 2
memory_nxs_barrier 3

.purgem memory_nxs_barrier

/* CLREX's CRm immediate is ignored, so all encodings share this branch-free
 * native implementation. Clear the contiguous software-monitor image with
 * three paired stores; the final pair also covers exclusive_size and padding.
 */
gadget clear_exclusive
	clrex
	add x8, _cpu, #UML_GADGET_CPU_EXCLUSIVE_ADDRESS
	mov x9, #-1
	stp x9, xzr, [x8]
	stp xzr, xzr, [x8, #16]
	stp xzr, xzr, [x8, #32]
	next_gadget

gadget yield
	yield
	next_gadget

gadget csdb
	/* Consumption speculation barrier, encoded as HINT #20. */
	.inst 0xd503229f
	next_gadget

gadget dgh
	/* FEAT_DGH hosts execute this directly; earlier hosts treat it as a hint. */
	.inst 0xd50320df
	next_gadget

gadget clrbhb
	/* FEAT_CLRBHB hosts clear their branch history. Earlier hosts are required
	 * to execute this architectural HINT encoding as a NOP.
	 */
	.inst 0xd50322df
	next_gadget

/* Translation-specialized AdvSIMD multiple-structure transfers. The symbol
 * fixes direction, register count, contiguous versus interleaved layout,
 * arrangement, and writeback mode. The packed operand holds Rt in bits 4:0,
 * Rn's CpuState byte offset in 31:16, Rm's offset in 47:32, and the decoded
 * architectural element size in 55:48. Mode 0 has no writeback, mode 1 uses
 * the architecturally implied byte count, and mode 2 reads the post-index
 * increment from Rm. */
.macro simd_structure_cpu_load count
	add x8, _cpu, #UML_GADGET_CPU_V0
	and w9, w19, #31
	ldr q0, [x8, x9, lsl #4]
	.if \count > 1
	add w9, w9, #1
	and w9, w9, #31
	ldr q1, [x8, x9, lsl #4]
	.endif
	.if \count > 2
	add w9, w9, #1
	and w9, w9, #31
	ldr q2, [x8, x9, lsl #4]
	.endif
	.if \count > 3
	add w9, w9, #1
	and w9, w9, #31
	ldr q3, [x8, x9, lsl #4]
	.endif
.endm

.macro simd_structure_cpu_store count
	add x8, _cpu, #UML_GADGET_CPU_V0
	and w9, w19, #31
	str q0, [x8, x9, lsl #4]
	.if \count > 1
	add w9, w9, #1
	and w9, w9, #31
	str q1, [x8, x9, lsl #4]
	.endif
	.if \count > 2
	add w9, w9, #1
	and w9, w9, #31
	str q2, [x8, x9, lsl #4]
	.endif
	.if \count > 3
	add w9, w9, #1
	and w9, w9, #31
	str q3, [x8, x9, lsl #4]
	.endif
.endm

.macro simd_structure_memory_load count, elements, q, size
	.if \elements == 1
		.if \q
	ldr q0, [x7]
			.if \count > 1
	ldr q1, [x7, #16]
			.endif
			.if \count > 2
	ldr q2, [x7, #32]
			.endif
			.if \count > 3
	ldr q3, [x7, #48]
			.endif
		.else
	ldr d0, [x7]
			.if \count > 1
	ldr d1, [x7, #8]
			.endif
			.if \count > 2
	ldr d2, [x7, #16]
			.endif
			.if \count > 3
	ldr d3, [x7, #24]
			.endif
		.endif
	.elseif \elements == 2
		.if \q
			.if \size == 0
	ld2 {v0.16b, v1.16b}, [x7]
			.elseif \size == 1
	ld2 {v0.8h, v1.8h}, [x7]
			.elseif \size == 2
	ld2 {v0.4s, v1.4s}, [x7]
			.else
	ld2 {v0.2d, v1.2d}, [x7]
			.endif
		.else
			.if \size == 0
	ld2 {v0.8b, v1.8b}, [x7]
			.elseif \size == 1
	ld2 {v0.4h, v1.4h}, [x7]
			.else
	ld2 {v0.2s, v1.2s}, [x7]
			.endif
		.endif
	.elseif \elements == 3
		.if \q
			.if \size == 0
	ld3 {v0.16b, v1.16b, v2.16b}, [x7]
			.elseif \size == 1
	ld3 {v0.8h, v1.8h, v2.8h}, [x7]
			.elseif \size == 2
	ld3 {v0.4s, v1.4s, v2.4s}, [x7]
			.else
	ld3 {v0.2d, v1.2d, v2.2d}, [x7]
			.endif
		.else
			.if \size == 0
	ld3 {v0.8b, v1.8b, v2.8b}, [x7]
			.elseif \size == 1
	ld3 {v0.4h, v1.4h, v2.4h}, [x7]
			.else
	ld3 {v0.2s, v1.2s, v2.2s}, [x7]
			.endif
		.endif
	.else
		.if \q
			.if \size == 0
	ld4 {v0.16b, v1.16b, v2.16b, v3.16b}, [x7]
			.elseif \size == 1
	ld4 {v0.8h, v1.8h, v2.8h, v3.8h}, [x7]
			.elseif \size == 2
	ld4 {v0.4s, v1.4s, v2.4s, v3.4s}, [x7]
			.else
	ld4 {v0.2d, v1.2d, v2.2d, v3.2d}, [x7]
			.endif
		.else
			.if \size == 0
	ld4 {v0.8b, v1.8b, v2.8b, v3.8b}, [x7]
			.elseif \size == 1
	ld4 {v0.4h, v1.4h, v2.4h, v3.4h}, [x7]
			.else
	ld4 {v0.2s, v1.2s, v2.2s, v3.2s}, [x7]
			.endif
		.endif
	.endif
.endm

.macro simd_structure_memory_store count, elements, q, size
	.if \elements == 1
		.if \q
	str q0, [x7]
			.if \count > 1
	str q1, [x7, #16]
			.endif
			.if \count > 2
	str q2, [x7, #32]
			.endif
			.if \count > 3
	str q3, [x7, #48]
			.endif
		.else
	str d0, [x7]
			.if \count > 1
	str d1, [x7, #8]
			.endif
			.if \count > 2
	str d2, [x7, #16]
			.endif
			.if \count > 3
	str d3, [x7, #24]
			.endif
		.endif
	.elseif \elements == 2
		.if \q
			.if \size == 0
	st2 {v0.16b, v1.16b}, [x7]
			.elseif \size == 1
	st2 {v0.8h, v1.8h}, [x7]
			.elseif \size == 2
	st2 {v0.4s, v1.4s}, [x7]
			.else
	st2 {v0.2d, v1.2d}, [x7]
			.endif
		.else
			.if \size == 0
	st2 {v0.8b, v1.8b}, [x7]
			.elseif \size == 1
	st2 {v0.4h, v1.4h}, [x7]
			.else
	st2 {v0.2s, v1.2s}, [x7]
			.endif
		.endif
	.elseif \elements == 3
		.if \q
			.if \size == 0
	st3 {v0.16b, v1.16b, v2.16b}, [x7]
			.elseif \size == 1
	st3 {v0.8h, v1.8h, v2.8h}, [x7]
			.elseif \size == 2
	st3 {v0.4s, v1.4s, v2.4s}, [x7]
			.else
	st3 {v0.2d, v1.2d, v2.2d}, [x7]
			.endif
		.else
			.if \size == 0
	st3 {v0.8b, v1.8b, v2.8b}, [x7]
			.elseif \size == 1
	st3 {v0.4h, v1.4h, v2.4h}, [x7]
			.else
	st3 {v0.2s, v1.2s, v2.2s}, [x7]
			.endif
		.endif
	.else
		.if \q
			.if \size == 0
	st4 {v0.16b, v1.16b, v2.16b, v3.16b}, [x7]
			.elseif \size == 1
	st4 {v0.8h, v1.8h, v2.8h, v3.8h}, [x7]
			.elseif \size == 2
	st4 {v0.4s, v1.4s, v2.4s, v3.4s}, [x7]
			.else
	st4 {v0.2d, v1.2d, v2.2d, v3.2d}, [x7]
			.endif
		.else
			.if \size == 0
	st4 {v0.8b, v1.8b, v2.8b, v3.8b}, [x7]
			.elseif \size == 1
	st4 {v0.4h, v1.4h, v2.4h, v3.4h}, [x7]
			.else
	st4 {v0.2s, v1.2s, v2.2s, v3.2s}, [x7]
			.endif
		.endif
	.endif
.endm

.macro simd_structure_fast name, load, count, elements, q, size, mode, base_sp
gadget simd_structure_\name
	ldr x19, [_stream], #8
	ubfx x24, x19, #16, #16
	ldr x20, [_cpu, x24]
	.if \base_sp
	tst x20, #0xf
	b.ne .Lsimd_structure_alignment\@
	.endif
	.if \base_sp == 0 || \mode != 0
	ldrh w8, [_cpu, #UML_GADGET_CPU_TAGGED_ADDRESS_STATE]
	cbnz w8, .Lsimd_structure_tagged\@
	.endif
	branch_if_cross_page x20, (\count * (8 << \q)), x8, .Lsimd_structure_slow\@
	mov x7, x20
.Lsimd_structure_translate\@:
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	simd_structure_memory_load \count, \elements, \q, \size
	simd_structure_cpu_store \count
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	simd_structure_cpu_load \count
	simd_structure_memory_store \count, \elements, \q, \size
	.endif
	.if \mode == 1
	add x20, x20, #(\count * (8 << \q))
	str x20, [_cpu, x24]
	.elseif \mode == 2
	ubfx x8, x19, #32, #16
	ldr x8, [_cpu, x8]
	add x20, x20, x8
	str x20, [_cpu, x24]
	.endif
	add _stream, _stream, #8
	next_gadget

	.if \base_sp == 0 || \mode != 0
.Lsimd_structure_tagged\@:
	/* MTE, the halfword's low byte, needs the allocation-tag helper. TBI
	 * alone strips the logical tag and rejoins the fast path. */
	tbnz w8, #0, .Lsimd_structure_slow\@
	branch_if_cross_page x20, (\count * (8 << \q)), x8, .Lsimd_structure_slow\@
	ubfx x7, x20, #0, #56
	b .Lsimd_structure_translate\@
	.endif

.Lsimd_structure_slow\@:
	sub sp, sp, #80
	stp x1, x2, [sp, #64]
	.if \load
	mov x0, x1
	mov x1, x2
	mov x2, x20
	mov x3, sp
	mov x4, #(\count * (8 << \q))
	mov w5, #0
	mov w6, #\base_sp
	ubfx x7, x19, #48, #8
	lsl w7, w7, #8
	.if \base_sp == 0 || \mode != 0
	orr w7, w7, #1
	.endif
	bl NAME(uml_arm64_simd_structure_memory_transfer)
	ldp x1, x2, [sp, #64]
	cbz w0, .Lsimd_structure_fault\@
	mov x7, sp
	simd_structure_memory_load \count, \elements, \q, \size
	simd_structure_cpu_store \count
	.else
	mov x7, sp
	simd_structure_cpu_load \count
	simd_structure_memory_store \count, \elements, \q, \size
	mov x0, x1
	mov x1, x2
	mov x2, x20
	mov x3, sp
	mov x4, #(\count * (8 << \q))
	mov w5, #1
	mov w6, #\base_sp
	ubfx x7, x19, #48, #8
	lsl w7, w7, #8
	.if \base_sp == 0 || \mode != 0
	orr w7, w7, #1
	.endif
	bl NAME(uml_arm64_simd_structure_memory_transfer)
	ldp x1, x2, [sp, #64]
	cbz w0, .Lsimd_structure_fault\@
	.endif
	add sp, sp, #80
	.if \mode == 1
	add x20, x20, #(\count * (8 << \q))
	str x20, [_cpu, x24]
	.elseif \mode == 2
	ubfx x8, x19, #32, #16
	ldr x8, [_cpu, x8]
	add x20, x20, x8
	str x20, [_cpu, x24]
	.endif
	add _stream, _stream, #8
	next_gadget

.Lsimd_structure_fault\@:
	add sp, sp, #80
	b uml_arm64_gadget_memory_fault

	.if \base_sp
.Lsimd_structure_alignment\@:
	mov x7, x20
		.if \load
	b uml_arm64_gadget_atomic_alignment_fault_read
		.else
	b uml_arm64_gadget_atomic_alignment_fault_write
		.endif
	.endif
.endm

.macro simd_structure_modes base, load, count, elements, q, size
	simd_structure_fast \base\()_0, \load, \count, \elements, \q, \size, 0, 0
	simd_structure_fast \base\()_1, \load, \count, \elements, \q, \size, 1, 0
	simd_structure_fast \base\()_2, \load, \count, \elements, \q, \size, 2, 0
	simd_structure_fast \base\()_s0, \load, \count, \elements, \q, \size, 0, 1
	simd_structure_fast \base\()_s1, \load, \count, \elements, \q, \size, 1, 1
	simd_structure_fast \base\()_s2, \load, \count, \elements, \q, \size, 2, 1
.endm

.macro simd_structure_directions base, count, elements, q, size
	simd_structure_modes \base\()_store, 0, \count, \elements, \q, \size
	simd_structure_modes \base\()_load, 1, \count, \elements, \q, \size
.endm

.macro simd_structure_contiguous count
	simd_structure_directions contig_d_\count, \count, 1, 0, 0
	simd_structure_directions contig_q_\count, \count, 1, 1, 0
.endm

simd_structure_contiguous 1
simd_structure_contiguous 2
simd_structure_contiguous 3
simd_structure_contiguous 4

.macro simd_structure_interleaved count
	simd_structure_directions interleaved_d_b_\count, \count, \count, 0, 0
	simd_structure_directions interleaved_d_h_\count, \count, \count, 0, 1
	simd_structure_directions interleaved_d_s_\count, \count, \count, 0, 2
	simd_structure_directions interleaved_q_b_\count, \count, \count, 1, 0
	simd_structure_directions interleaved_q_h_\count, \count, \count, 1, 1
	simd_structure_directions interleaved_q_s_\count, \count, \count, 1, 2
	simd_structure_directions interleaved_q_d_\count, \count, \count, 1, 3
.endm

simd_structure_interleaved 2
simd_structure_interleaved 3
simd_structure_interleaved 4

.purgem simd_structure_interleaved
.purgem simd_structure_contiguous
.purgem simd_structure_directions
.purgem simd_structure_modes
.purgem simd_structure_fast
.purgem simd_structure_memory_store
.purgem simd_structure_memory_load
.purgem simd_structure_cpu_store
.purgem simd_structure_cpu_load

/* Translation-specialized LD1R/LD2R/LD3R/LD4R. Element width, destination
 * count, vector width, and post-index mode are compile-time properties. */
.macro simd_replicate_one width, q
	.if \width == 1
	ldrb w9, [x7]
		.if \q
	dup v0.16b, w9
		.else
	dup v0.8b, w9
		.endif
	.elseif \width == 2
	ldrh w9, [x7]
		.if \q
	dup v0.8h, w9
		.else
	dup v0.4h, w9
		.endif
	.elseif \width == 4
	ldr w9, [x7]
		.if \q
	dup v0.4s, w9
		.else
	dup v0.2s, w9
		.endif
	.else
	ldr x9, [x7]
		.if \q
	dup v0.2d, x9
		.else
	fmov d0, x9
		.endif
	.endif
	and w9, w26, #31
	add x10, _cpu, #UML_GADGET_CPU_V0
	add x10, x10, x9, lsl #4
	str q0, [x10]
.endm

.macro simd_replicate_fast name, count, width, q, mode, base_sp
gadget simd_replicate_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #16, #16
	ldr x21, [_cpu, x20]
	.if \base_sp
	tst x21, #0xf
	b.ne .Lsimd_replicate_alignment\@
	.endif
	.if \base_sp == 0 || \mode != 0
	ldrh w8, [_cpu, #UML_GADGET_CPU_TAGGED_ADDRESS_STATE]
	cbnz w8, .Lsimd_replicate_tagged\@
	.endif
	.if \count * \width > 1
	branch_if_cross_page x21, (\count * \width), x8, .Lsimd_replicate_slow\@
	.endif
	mov x7, x21
.Lsimd_replicate_translate\@:
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	and w26, w19, #31
	simd_replicate_one \width, \q
	.if \count > 1
	add x7, x7, #\width
	add w26, w26, #1
	simd_replicate_one \width, \q
	.endif
	.if \count > 2
	add x7, x7, #\width
	add w26, w26, #1
	simd_replicate_one \width, \q
	.endif
	.if \count > 3
	add x7, x7, #\width
	add w26, w26, #1
	simd_replicate_one \width, \q
	.endif
	.if \mode == 1
	add x21, x21, #(\count * \width)
	str x21, [_cpu, x20]
	.elseif \mode == 2
	ubfx x8, x19, #32, #16
	ldr x8, [_cpu, x8]
	add x21, x21, x8
	str x21, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget

	.if \base_sp == 0 || \mode != 0
.Lsimd_replicate_tagged\@:
	/* MTE, the halfword's low byte, needs the allocation-tag helper. TBI
	 * alone strips the logical tag and rejoins the fast path. */
	tbnz w8, #0, .Lsimd_replicate_slow\@
	.if \count * \width > 1
	branch_if_cross_page x21, (\count * \width), x8, .Lsimd_replicate_slow\@
	.endif
	ubfx x7, x21, #0, #56
	b .Lsimd_replicate_translate\@
	.endif

.Lsimd_replicate_slow\@:
	sub sp, sp, #48
	stp x1, x2, [sp, #32]
	mov x0, x1
	mov x1, x2
	mov x2, x21
	mov x3, sp
	mov x4, #(\count * \width)
	mov w5, #0
	mov w6, #\base_sp
	mov w7, #((\width << 8) | (\base_sp == 0 || \mode != 0))
	bl NAME(uml_arm64_simd_structure_memory_transfer)
	ldp x1, x2, [sp, #32]
	cbz w0, .Lsimd_replicate_fault\@
	mov x7, sp
	and w26, w19, #31
	simd_replicate_one \width, \q
	.if \count > 1
	add x7, x7, #\width
	add w26, w26, #1
	simd_replicate_one \width, \q
	.endif
	.if \count > 2
	add x7, x7, #\width
	add w26, w26, #1
	simd_replicate_one \width, \q
	.endif
	.if \count > 3
	add x7, x7, #\width
	add w26, w26, #1
	simd_replicate_one \width, \q
	.endif
	add sp, sp, #48
	.if \mode == 1
	add x21, x21, #(\count * \width)
	str x21, [_cpu, x20]
	.elseif \mode == 2
	ubfx x8, x19, #32, #16
	ldr x8, [_cpu, x8]
	add x21, x21, x8
	str x21, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget

.Lsimd_replicate_fault\@:
	add sp, sp, #48
	b uml_arm64_gadget_memory_fault

	.if \base_sp
.Lsimd_replicate_alignment\@:
	mov x7, x21
	b uml_arm64_gadget_atomic_alignment_fault_read
	.endif
.endm

.macro simd_replicate_modes base, count, width, q
	simd_replicate_fast \base\()_0, \count, \width, \q, 0, 0
	simd_replicate_fast \base\()_1, \count, \width, \q, 1, 0
	simd_replicate_fast \base\()_2, \count, \width, \q, 2, 0
	simd_replicate_fast \base\()_s0, \count, \width, \q, 0, 1
	simd_replicate_fast \base\()_s1, \count, \width, \q, 1, 1
	simd_replicate_fast \base\()_s2, \count, \width, \q, 2, 1
.endm

.macro simd_replicate_counts type, width, q
	simd_replicate_modes \type\()_1, 1, \width, \q
	simd_replicate_modes \type\()_2, 2, \width, \q
	simd_replicate_modes \type\()_3, 3, \width, \q
	simd_replicate_modes \type\()_4, 4, \width, \q
.endm

simd_replicate_counts b_d, 1, 0
simd_replicate_counts b_q, 1, 1
simd_replicate_counts h_d, 2, 0
simd_replicate_counts h_q, 2, 1
simd_replicate_counts s_d, 4, 0
simd_replicate_counts s_q, 4, 1
simd_replicate_counts d_d, 8, 0
simd_replicate_counts d_q, 8, 1

.purgem simd_replicate_counts
.purgem simd_replicate_modes
.purgem simd_replicate_fast
.purgem simd_replicate_one

/* Translation-specialized single-structure lane transfers. The lane byte
 * offset is precomputed into operand bits 63:48; width, register count,
 * direction, and writeback mode are fixed by the selected symbol. */
.macro simd_lane_one load, width
	and w9, w26, #31
	add x10, _cpu, #UML_GADGET_CPU_V0
	add x10, x10, x9, lsl #4
	add x10, x10, x23
	.if \load
		.if \width == 1
	ldrb w9, [x7]
	strb w9, [x10]
		.elseif \width == 2
	ldrh w9, [x7]
	strh w9, [x10]
		.elseif \width == 4
	ldr w9, [x7]
	str w9, [x10]
		.else
	ldr x9, [x7]
	str x9, [x10]
		.endif
	.else
		.if \width == 1
	ldrb w9, [x10]
	strb w9, [x7]
		.elseif \width == 2
	ldrh w9, [x10]
	strh w9, [x7]
		.elseif \width == 4
	ldr w9, [x10]
	str w9, [x7]
		.else
	ldr x9, [x10]
	str x9, [x7]
		.endif
	.endif
.endm

.macro simd_lane_transfer load, count, width
	and w26, w19, #31
	simd_lane_one \load, \width
	.if \count > 1
	add x7, x7, #\width
	add w26, w26, #1
	simd_lane_one \load, \width
	.endif
	.if \count > 2
	add x7, x7, #\width
	add w26, w26, #1
	simd_lane_one \load, \width
	.endif
	.if \count > 3
	add x7, x7, #\width
	add w26, w26, #1
	simd_lane_one \load, \width
	.endif
.endm

.macro simd_lane_fast name, load, count, width, mode, base_sp
gadget simd_lane_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #16, #16
	ubfx x23, x19, #48, #16
	ldr x21, [_cpu, x20]
	.if \base_sp
	tst x21, #0xf
	b.ne .Lsimd_lane_alignment\@
	.endif
	.if \base_sp == 0 || \mode != 0
	ldrh w8, [_cpu, #UML_GADGET_CPU_TAGGED_ADDRESS_STATE]
	cbnz w8, .Lsimd_lane_tagged\@
	.endif
	.if \count * \width > 1
	branch_if_cross_page x21, (\count * \width), x8, .Lsimd_lane_slow\@
	.endif
	mov x7, x21
.Lsimd_lane_translate\@:
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	.endif
	simd_lane_transfer \load, \count, \width
	.if \mode == 1
	add x21, x21, #(\count * \width)
	str x21, [_cpu, x20]
	.elseif \mode == 2
	ubfx x8, x19, #32, #16
	ldr x8, [_cpu, x8]
	add x21, x21, x8
	str x21, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget

	.if \base_sp == 0 || \mode != 0
.Lsimd_lane_tagged\@:
	/* MTE, the halfword's low byte, needs the allocation-tag helper. TBI
	 * alone strips the logical tag and rejoins the fast path. */
	tbnz w8, #0, .Lsimd_lane_slow\@
	.if \count * \width > 1
	branch_if_cross_page x21, (\count * \width), x8, .Lsimd_lane_slow\@
	.endif
	ubfx x7, x21, #0, #56
	b .Lsimd_lane_translate\@
	.endif

.Lsimd_lane_slow\@:
	sub sp, sp, #48
	stp x1, x2, [sp, #32]
	.if \load
	mov x0, x1
	mov x1, x2
	mov x2, x21
	mov x3, sp
	mov x4, #(\count * \width)
	mov w5, #0
	mov w6, #\base_sp
	mov w7, #((\width << 8) | (\base_sp == 0 || \mode != 0))
	bl NAME(uml_arm64_simd_structure_memory_transfer)
	ldp x1, x2, [sp, #32]
	cbz w0, .Lsimd_lane_fault\@
	mov x7, sp
	simd_lane_transfer \load, \count, \width
	.else
	mov x7, sp
	simd_lane_transfer \load, \count, \width
	mov x0, x1
	mov x1, x2
	mov x2, x21
	mov x3, sp
	mov x4, #(\count * \width)
	mov w5, #1
	mov w6, #\base_sp
	mov w7, #((\width << 8) | (\base_sp == 0 || \mode != 0))
	bl NAME(uml_arm64_simd_structure_memory_transfer)
	ldp x1, x2, [sp, #32]
	cbz w0, .Lsimd_lane_fault\@
	.endif
	add sp, sp, #48
	.if \mode == 1
	add x21, x21, #(\count * \width)
	str x21, [_cpu, x20]
	.elseif \mode == 2
	ubfx x8, x19, #32, #16
	ldr x8, [_cpu, x8]
	add x21, x21, x8
	str x21, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget

.Lsimd_lane_fault\@:
	add sp, sp, #48
	b uml_arm64_gadget_memory_fault

	.if \base_sp
.Lsimd_lane_alignment\@:
	mov x7, x21
		.if \load
	b uml_arm64_gadget_atomic_alignment_fault_read
		.else
	b uml_arm64_gadget_atomic_alignment_fault_write
		.endif
	.endif
.endm

.macro simd_lane_modes base, load, count, width
	simd_lane_fast \base\()_0, \load, \count, \width, 0, 0
	simd_lane_fast \base\()_1, \load, \count, \width, 1, 0
	simd_lane_fast \base\()_2, \load, \count, \width, 2, 0
	simd_lane_fast \base\()_s0, \load, \count, \width, 0, 1
	simd_lane_fast \base\()_s1, \load, \count, \width, 1, 1
	simd_lane_fast \base\()_s2, \load, \count, \width, 2, 1
.endm

.macro simd_lane_directions type, count, width
	simd_lane_modes \type\()_\count\()_store, 0, \count, \width
	simd_lane_modes \type\()_\count\()_load, 1, \count, \width
.endm

.macro simd_lane_counts type, width
	simd_lane_directions \type, 1, \width
	simd_lane_directions \type, 2, \width
	simd_lane_directions \type, 3, \width
	simd_lane_directions \type, 4, \width
.endm

simd_lane_counts b, 1
simd_lane_counts h, 2
simd_lane_counts s, 4
simd_lane_counts d, 8

.purgem simd_lane_counts
.purgem simd_lane_directions
.purgem simd_lane_modes
.purgem simd_lane_fast
.purgem simd_lane_transfer
.purgem simd_lane_one

/* FEAT_LRCPC3 ordered SIMD&FP transfers. Translation specializes SP versus
 * GPR addressing, width, direction, and lane, leaving only alignment/fault
 * branches on a valid hot path. Scalar widths deliberately use the older
 * host LDAPR/STLR family: moving through a GPR is fewer instructions than
 * materializing a full host vector. A host-RCPC3 Q transfer is selected at
 * build time; the portable Q fallback uses one stronger barrier. Stream:
 * packed destination/source, base, signed byte offset; guest PC. */
.macro simd_ordered_address sp_base, alignment, write
	ldr x19, [_stream], #8
	.if \sp_base
	ldr x7, [_cpu, #UML_GADGET_CPU_SP]
	.else
	ubfx w8, w19, #5, #5
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x9, x8, lsl #3]
	.endif
	asr x8, x19, #10
	add x7, x7, x8
	.if \alignment
	tst x7, #\alignment
		.if \write
		b.ne uml_arm64_gadget_atomic_alignment_fault_write
		.else
		b.ne uml_arm64_gadget_atomic_alignment_fault_read
		.endif
	.endif
.endm

.macro simd_ordered_load_scalar name, instruction, host_register, alignment, sp_base
gadget simd_ordered_load_\name
	simd_ordered_address \sp_base, \alignment, 0
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()9, [x7]
	and w8, w19, #0x1f
	add x10, _cpu, #UML_GADGET_CPU_V0
	add x10, x10, x8, lsl #4
	stp x9, xzr, [x10]
	add _stream, _stream, #8
	next_gadget
.endm

.macro simd_ordered_store_scalar name, instruction, host_register, alignment, sp_base
gadget simd_ordered_store_\name
	simd_ordered_address \sp_base, \alignment, 1
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	and w8, w19, #0x1f
	add x10, _cpu, #UML_GADGET_CPU_V0
	add x10, x10, x8, lsl #4
	ldr x9, [x10]
	\instruction \host_register\()9, [x7]
	add _stream, _stream, #8
	next_gadget
.endm

.macro simd_ordered_scalar_modes size, load_instruction, store_instruction, host_register, alignment
	simd_ordered_load_scalar \size\()_r, \load_instruction, \host_register, \alignment, 0
	simd_ordered_load_scalar \size\()_s, \load_instruction, \host_register, \alignment, 1
	simd_ordered_store_scalar \size\()_r, \store_instruction, \host_register, \alignment, 0
	simd_ordered_store_scalar \size\()_s, \store_instruction, \host_register, \alignment, 1
.endm

simd_ordered_scalar_modes b, ldaprb, stlrb, w, 0
simd_ordered_scalar_modes h, ldaprh, stlrh, w, 1
simd_ordered_scalar_modes s, ldapr, stlr, w, 3
simd_ordered_scalar_modes d, ldapr, stlr, x, 7

.purgem simd_ordered_scalar_modes
.purgem simd_ordered_store_scalar
.purgem simd_ordered_load_scalar

.macro simd_ordered_load_q name, sp_base, native
gadget simd_ordered_load_q_\name
	simd_ordered_address \sp_base, 15, 0
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	.if \native
	/* LDAPUR Q0, [X7] (FEAT_LRCPC3). */
	.inst 0x1dc008e0
	.else
	ldp x9, x10, [x7]
	dmb ishld
	.endif
	and w8, w19, #0x1f
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x11, x11, x8, lsl #4
	.if \native
	str q0, [x11]
	.else
	stp x9, x10, [x11]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro simd_ordered_store_q name, sp_base, native
gadget simd_ordered_store_q_\name
	simd_ordered_address \sp_base, 15, 1
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	and w8, w19, #0x1f
	add x11, _cpu, #UML_GADGET_CPU_V0
	add x11, x11, x8, lsl #4
	.if \native
	ldr q0, [x11]
	/* STLUR Q0, [X7] (FEAT_LRCPC3). */
	.inst 0x1d8008e0
	.else
	ldp x9, x10, [x11]
	dmb ish
	stp x9, x10, [x7]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

simd_ordered_load_q r_fallback, 0, 0
simd_ordered_load_q s_fallback, 1, 0
simd_ordered_load_q r_native, 0, 1
simd_ordered_load_q s_native, 1, 1
simd_ordered_store_q r_fallback, 0, 0
simd_ordered_store_q s_fallback, 1, 0
simd_ordered_store_q r_native, 0, 1
simd_ordered_store_q s_native, 1, 1

.purgem simd_ordered_store_q
.purgem simd_ordered_load_q

.macro simd_ordered_lane name, load, lane, sp_base
gadget simd_ordered_\name
	simd_ordered_address \sp_base, 7, (1 - \load)
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	ldapr x9, [x7]
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	.endif
	and w8, w19, #0x1f
	add x10, _cpu, #UML_GADGET_CPU_V0
	add x10, x10, x8, lsl #4
	.if \load
	str x9, [x10, #(8 * \lane)]
	.else
	ldr x9, [x10, #(8 * \lane)]
	stlr x9, [x7]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

simd_ordered_lane load_lane0_r, 1, 0, 0
simd_ordered_lane load_lane0_s, 1, 0, 1
simd_ordered_lane load_lane1_r, 1, 1, 0
simd_ordered_lane load_lane1_s, 1, 1, 1
simd_ordered_lane store_lane0_r, 0, 0, 0
simd_ordered_lane store_lane0_s, 0, 0, 1
simd_ordered_lane store_lane1_r, 0, 1, 0
simd_ordered_lane store_lane1_s, 0, 1, 1

.purgem simd_ordered_lane
.purgem simd_ordered_address

/* FEAT_LRCPC3 STLR pre-index and LDAPR post-index. The guest encoding fixes
 * the displacement to one element, so the hot path needs no immediate decode
 * beyond the element width. */
/* Translation-specialized ordered writeback transfers. Store forms are
 * pre-decrement and load forms post-increment; width, direction, ZR, and the
 * load-overlap WBSUPPRESS choice are fixed by the selected symbol. */
.macro ordered_writeback_fast name, load, instruction, host_register, bytes, transfer_zero, suppress_writeback
gadget ordered_writeback_\name
	ldr x19, [_stream], #8
	and x21, x19, #0xffff
	ubfx x20, x19, #16, #16
	ldr x24, [_cpu, x20]
	.if \load
	mov x7, x24
	add x25, x24, #\bytes
	.else
	sub x7, x24, #\bytes
	mov x25, x7
	.endif
	.if \bytes > 1
	tst x7, #(\bytes - 1)
		.if \load
		b.ne uml_arm64_gadget_atomic_alignment_fault_read
		.else
		b.ne uml_arm64_gadget_atomic_alignment_fault_write
		.endif
	.endif
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()15, [x7]
		.if !\transfer_zero
	str x15, [_cpu, x21]
		.endif
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
		.if \transfer_zero
	mov x15, xzr
		.else
	ldr x15, [_cpu, x21]
		.endif
	\instruction \host_register\()15, [x7]
	.endif
	.if !\suppress_writeback
	str x25, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

ordered_writeback_fast store_w_r, 0, stlr, w, 4, 0, 0
ordered_writeback_fast store_w_z, 0, stlr, w, 4, 1, 0
ordered_writeback_fast store_x_r, 0, stlr, x, 8, 0, 0
ordered_writeback_fast store_x_z, 0, stlr, x, 8, 1, 0
ordered_writeback_fast load_w_r, 1, ldapr, w, 4, 0, 0
ordered_writeback_fast load_w_z, 1, ldapr, w, 4, 1, 0
ordered_writeback_fast load_w_suppress, 1, ldapr, w, 4, 0, 1
ordered_writeback_fast load_x_r, 1, ldapr, x, 8, 0, 0
ordered_writeback_fast load_x_z, 1, ldapr, x, 8, 1, 0
ordered_writeback_fast load_x_suppress, 1, ldapr, x, 8, 0, 1

.purgem ordered_writeback_fast

/* FEAT_LSFE floating-point atomics. The builder specializes precision,
 * operation, ordering, and return/discard behavior. Native variants execute
 * one LSFE instruction after translation. Hosts without LSFE call an equally
 * specialized CAS helper; neither valid hot path decodes the guest opcode.
 * Rn is specialized while the threaded program is built. GPR-base hot paths
 * add only the effective-MTE test needed to select the checked tag/permission
 * bridge; SP variants perform CheckSPAlignment without carrying that branch.
 * Stream: packed source/destination/base, guest PC.
 */
.macro atomic_floating_address bytes, alignment, base_sp
	ldr x19, [_stream], #8
	.if \base_sp
	ldr x7, [_cpu, #UML_GADGET_CPU_SP]
	tst x7, #0xf
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	.else
	ubfx w20, w19, #16, #5
	ldr x7, [_cpu, x20, lsl #3]
	/* Tagged-address handling is cold for normal untagged programs. */
	ldrh w8, [_cpu, #UML_GADGET_CPU_TAGGED_ADDRESS_STATE]
	cbnz w8, 89f
		.if \alignment
	tst x7, #\alignment
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
		.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	b 90f
89:
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x2
	mov x2, x7
	mov x3, #\bytes
	mov w4, #0
	bl NAME(uml_arm64_atomic_floating_pointer)
	ldp x1, x2, [sp], #16
	cbz x0, uml_arm64_gadget_memory_fault
	mov x7, x0
90:
	.endif
.endm

.macro atomic_floating_software name, bytes, alignment, base_sp
	.if \base_sp
gadget atomic_floating_software_\name\()_sp
	.else
gadget atomic_floating_software_\name
	.endif
	atomic_floating_address \bytes, \alignment, \base_sp
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x7
	mov x2, x19
	bl NAME(uml_arm64_atomic_floating_transfer_\name)
	ldp x1, x2, [sp], #16
	add _stream, _stream, #8
	next_gadget
.endm

.macro atomic_floating_native name, transfer, bytes, alignment, encoding, return_old, base_sp
	.if \base_sp
gadget atomic_floating_native_\name\()_sp
	.else
gadget atomic_floating_native_\name
	.endif
	atomic_floating_address \bytes, \alignment, \base_sp
	and w20, w19, #0x1f
	add x8, _cpu, #UML_GADGET_CPU_V0
	add x8, x8, x20, lsl #4
	ldr \transfer\()15, [x8]
	ensure_fp_environment
	.inst \encoding
	.if \return_old
	ubfx w20, w19, #8, #5
	add x8, _cpu, #UML_GADGET_CPU_V0
	add x8, x8, x20, lsl #4
	str q16, [x8]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro atomic_floating_variant size_name, size_bits, transfer, bytes, alignment, operation_name, opc, variant, acquire, release, return_old
	atomic_floating_software \size_name\()_\operation_name\()_\variant, \
		\bytes, \alignment, 0
	atomic_floating_software \size_name\()_\operation_name\()_\variant, \
		\bytes, \alignment, 1
	.if \return_old
	atomic_floating_native \size_name\()_\operation_name\()_\variant, \
		\transfer, \bytes, \alignment, \
		(0x3c200000 | \size_bits | (\acquire << 23) | \
		 (\release << 22) | (15 << 16) | (\opc << 12) | \
		 (7 << 5) | 16), 1, 0
	atomic_floating_native \size_name\()_\operation_name\()_\variant, \
		\transfer, \bytes, \alignment, \
		(0x3c200000 | \size_bits | (\acquire << 23) | \
		 (\release << 22) | (15 << 16) | (\opc << 12) | \
		 (7 << 5) | 16), 1, 1
	.else
	atomic_floating_native \size_name\()_\operation_name\()_\variant, \
		\transfer, \bytes, \alignment, \
		(0x3c200000 | \size_bits | (\release << 22) | (15 << 16) | \
		 (1 << 15) | (\opc << 12) | (7 << 5) | 31), 0, 0
	atomic_floating_native \size_name\()_\operation_name\()_\variant, \
		\transfer, \bytes, \alignment, \
		(0x3c200000 | \size_bits | (\release << 22) | (15 << 16) | \
		 (1 << 15) | (\opc << 12) | (7 << 5) | 31), 0, 1
	.endif
.endm

.macro atomic_floating_variants size_name, size_bits, transfer, bytes, alignment, operation_name, opc
	atomic_floating_variant \size_name, \size_bits, \transfer, \bytes, \alignment, \
		\operation_name, \opc, load_relaxed, 0, 0, 1
	atomic_floating_variant \size_name, \size_bits, \transfer, \bytes, \alignment, \
		\operation_name, \opc, load_release, 0, 1, 1
	atomic_floating_variant \size_name, \size_bits, \transfer, \bytes, \alignment, \
		\operation_name, \opc, load_acquire, 1, 0, 1
	atomic_floating_variant \size_name, \size_bits, \transfer, \bytes, \alignment, \
		\operation_name, \opc, load_acquire_release, 1, 1, 1
	atomic_floating_variant \size_name, \size_bits, \transfer, \bytes, \alignment, \
		\operation_name, \opc, store_relaxed, 0, 0, 0
	atomic_floating_variant \size_name, \size_bits, \transfer, \bytes, \alignment, \
		\operation_name, \opc, store_release, 0, 1, 0
.endm

.macro atomic_floating_operations size_name, size_bits, transfer, bytes, alignment
	atomic_floating_variants \size_name, \size_bits, \transfer, \bytes, \alignment, add, 0
	atomic_floating_variants \size_name, \size_bits, \transfer, \bytes, \alignment, max, 4
	atomic_floating_variants \size_name, \size_bits, \transfer, \bytes, \alignment, maxnm, 6
	atomic_floating_variants \size_name, \size_bits, \transfer, \bytes, \alignment, min, 5
	atomic_floating_variants \size_name, \size_bits, \transfer, \bytes, \alignment, minnm, 7
.endm

atomic_floating_operations bf, 0x00000000, h, 2, 1
atomic_floating_operations h,  0x40000000, h, 2, 1
atomic_floating_operations s,  0x80000000, s, 4, 3
atomic_floating_operations d,  0xc0000000, d, 8, 7

.purgem atomic_floating_operations
.purgem atomic_floating_variants
.purgem atomic_floating_variant
.purgem atomic_floating_native
.purgem atomic_floating_software
.purgem atomic_floating_address

/*
 * CheckSPAlignment is selected solely from Rn at translation time. Ordinary
 * GPR-base atomic gadgets retain their existing hot paths; SP forms receive
 * this small prefix. The selected memory gadget and packed operand precede the
 * guest PC, so a fault consumes those two words before entering the shared
 * fault exit. SIMD structure gadgets fold this check into SP-specialized
 * symbols to avoid an additional threaded dispatch.
 */
.macro atomic_sp_alignment name
gadget atomic_sp_alignment_\name
	ldr x7, [_cpu, #UML_GADGET_CPU_SP]
	tst x7, #0xf
	b.eq 1f
	/* _stream currently addresses the selected atomic gadget pointer,
	 * followed by its packed operand and guest PC. */
	add _stream, _stream, #16
	b uml_arm64_gadget_atomic_alignment_fault_\name
1:
	next_gadget
.endm

atomic_sp_alignment read
atomic_sp_alignment write

.purgem atomic_sp_alignment

.p2align 4
uml_arm64_gadget_atomic_alignment_fault_read:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x7
	mov w2, #0
	bl NAME(uml_arm64_alignment_fault)
	ldp x1, x2, [sp], #16
	b uml_arm64_gadget_memory_fault

.p2align 4
uml_arm64_gadget_atomic_alignment_fault_write:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x7
	mov w2, #1
	bl NAME(uml_arm64_alignment_fault)
	ldp x1, x2, [sp], #16
	b uml_arm64_gadget_memory_fault

/*
 * FEAT_LSE scalar atomic read-modify-write operations. Width, operation,
 * ordering, source-ZR handling, and result-discard handling are all selected
 * while the threaded program is built. The valid hot path therefore contains
 * no opcode/ordering dispatch and maps one guest operation to one native host
 * LSE instruction. Stream: original instruction, guest PC.
 */
.macro atomic_rmw_gadget name, instruction, host_register, alignment, source_zero, discard_result
gadget atomic_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #32, #16
	ldr x7, [_cpu, x20]
	.if \alignment
	tst x7, #\alignment
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
	.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	.if \source_zero
	mov x15, xzr
	.else
	ubfx x20, x19, #16, #16
	ldr x15, [_cpu, x20]
	.endif
	\instruction \host_register\()15, \host_register\()16, [x7]
	.if !\discard_result
	and x21, x19, #0xffff
	str x16, [_cpu, x21]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro atomic_rmw_register_modes name, instruction, host_register, alignment
	atomic_rmw_gadget \name\()_rr, \instruction, \host_register, \alignment, 0, 0
	atomic_rmw_gadget \name\()_rz, \instruction, \host_register, \alignment, 0, 1
	atomic_rmw_gadget \name\()_zr, \instruction, \host_register, \alignment, 1, 0
	atomic_rmw_gadget \name\()_zz, \instruction, \host_register, \alignment, 1, 1
.endm

.macro atomic_rmw_sizes name, byte_instruction, half_instruction, word_instruction, double_instruction
	atomic_rmw_register_modes \name\()_b, \byte_instruction, w, 0
	atomic_rmw_register_modes \name\()_h, \half_instruction, w, 1
	atomic_rmw_register_modes \name\()_w, \word_instruction, w, 3
	atomic_rmw_register_modes \name\()_x, \double_instruction, x, 7
.endm

.macro atomic_rmw_family operation
	atomic_rmw_sizes \operation\()_relaxed, \operation\()b, \operation\()h, \operation, \operation
	atomic_rmw_sizes \operation\()_release, \operation\()lb, \operation\()lh, \operation\()l, \operation\()l
	atomic_rmw_sizes \operation\()_acquire, \operation\()ab, \operation\()ah, \operation\()a, \operation\()a
	atomic_rmw_sizes \operation\()_acquire_release, \operation\()alb, \operation\()alh, \operation\()al, \operation\()al
.endm

atomic_rmw_family ldadd
atomic_rmw_family ldclr
atomic_rmw_family ldeor
atomic_rmw_family ldset
atomic_rmw_family ldsmax
atomic_rmw_family ldsmin
atomic_rmw_family ldumax
atomic_rmw_family ldumin
atomic_rmw_family swp

.purgem atomic_rmw_family
.purgem atomic_rmw_sizes
.purgem atomic_rmw_register_modes
.purgem atomic_rmw_gadget

/* FEAT_LSE CAS/CAST. As with scalar RMW operations, every encoding-constant
 * choice is made by the builder. The mode suffix describes compare/result and
 * replacement registers: r is a GPR and z is ZR/discard. */
.macro compare_swap_gadget name, instruction, host_register, alignment, compare_zero, replacement_zero
gadget compare_swap_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #32, #16
	ldr x7, [_cpu, x20]
	.if \alignment
	tst x7, #\alignment
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
	.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	.if \compare_zero
	mov x15, xzr
	.else
	and x20, x19, #0xffff
	ldr x15, [_cpu, x20]
	.endif
	.if \replacement_zero
	mov x16, xzr
	.else
	ubfx x21, x19, #16, #16
	ldr x16, [_cpu, x21]
	.endif
	\instruction \host_register\()15, \host_register\()16, [x7]
	.if !\compare_zero
	str x15, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro compare_swap_register_modes name, instruction, host_register, alignment
	compare_swap_gadget \name\()_rr, \instruction, \host_register, \alignment, 0, 0
	compare_swap_gadget \name\()_rz, \instruction, \host_register, \alignment, 0, 1
	compare_swap_gadget \name\()_zr, \instruction, \host_register, \alignment, 1, 0
	compare_swap_gadget \name\()_zz, \instruction, \host_register, \alignment, 1, 1
.endm

.macro compare_swap_sizes name, byte_instruction, half_instruction, word_instruction, double_instruction
	compare_swap_register_modes \name\()_b, \byte_instruction, w, 0
	compare_swap_register_modes \name\()_h, \half_instruction, w, 1
	compare_swap_register_modes \name\()_w, \word_instruction, w, 3
	compare_swap_register_modes \name\()_x, \double_instruction, x, 7
.endm

compare_swap_sizes relaxed, casb, cash, cas, cas
compare_swap_sizes release, caslb, caslh, casl, casl
compare_swap_sizes acquire, casab, casah, casa, casa
compare_swap_sizes acquire_release, casalb, casalh, casal, casal

.purgem compare_swap_sizes
.purgem compare_swap_register_modes
.purgem compare_swap_gadget

/*
 * FEAT_THE scalar Read-Check-Write operations. The userspace architecture
 * keeps translation-table protection disabled and uses a maximally permissive
 * RCWS mask, leaving descriptor bit zero as the only software state check.
 * Operation, hard/software checks, ordering, and ZR modes are all selected at
 * translation time. The valid hot path only branches for alignment/TLB,
 * architecturally meaningful compare/check outcomes, and exclusive retry.
 * Stream: packed base/source/result CpuState offsets, guest PC.
 */
.macro rcw_scalar_gadget name, operation, load_instruction, store_instruction, soft, source_zero, discard_result
gadget rcw_scalar_\name
	ldr x19, [_stream], #8
	and w20, w19, #0xffff
	/* Translation packed the direct CpuState offset, including SP. */
	ldr x7, [_cpu, x20]
	tst x7, #7
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	.if \source_zero
	mov x15, xzr
	.else
	ubfx x20, x19, #16, #16
	ldr x15, [_cpu, x20]
	.endif
	.if \operation == 0
		.if \discard_result
		mov x13, xzr
		.else
		ubfx x21, x19, #32, #16
		ldr x13, [_cpu, x21]
		.endif
	.endif
1:	\load_instruction x16, [x7]
	.if \operation == 0
		cmp x16, x13
		.if \soft
		/* allowed = compare passed && descriptor bit zero is unchanged. */
		cset w9, eq
		eor x8, x16, x15
		and w10, w8, #1
		bic w10, w9, w10
		cmp w10, #0
		csel x17, x15, x16, ne
		lsl w12, w10, #29
		mov w8, #0xa0000000
		cmp w9, #0
		csel w12, w12, w8, ne
		.else
		csel x17, x15, x16, eq
		mov w12, #0x20000000
		mov w8, #0xa0000000
		csel w12, w12, w8, eq
		.endif
	.elseif \operation == 1
	bic x17, x16, x15
	.elseif \operation == 2
	mov x17, x15
	.else
	orr x17, x16, x15
	.endif
	.if \operation != 0
		.if \soft
		eor x8, x16, x17
		tst x8, #1
		csel x17, x17, x16, eq
		mov w12, #0x20000000
		csel w12, w12, wzr, eq
		.else
		mov w12, #0x20000000
		.endif
	.endif
	\store_instruction w11, x17, [x7]
	cbnz w11, 1b
	.if !\discard_result
	ubfx x21, x19, #32, #16
	str x16, [_cpu, x21]
	.endif
	mov w27, w12
	add _stream, _stream, #8
	next_gadget
.endm

.macro rcw_scalar_full_modes name, operation, load_instruction, store_instruction, soft
	rcw_scalar_gadget \name\()_rr, \operation, \load_instruction, \store_instruction, \soft, 0, 0
	rcw_scalar_gadget \name\()_rz, \operation, \load_instruction, \store_instruction, \soft, 0, 1
	rcw_scalar_gadget \name\()_zr, \operation, \load_instruction, \store_instruction, \soft, 1, 0
	rcw_scalar_gadget \name\()_zz, \operation, \load_instruction, \store_instruction, \soft, 1, 1
.endm

.macro rcw_scalar_live_result_modes name, operation, load_instruction, store_instruction, soft
	rcw_scalar_gadget \name\()_rr, \operation, \load_instruction, \store_instruction, \soft, 0, 0
	rcw_scalar_gadget \name\()_zr, \operation, \load_instruction, \store_instruction, \soft, 1, 0
.endm

.macro rcw_scalar_orderings operation_name, operation, soft_name, soft
	rcw_scalar_full_modes \operation_name\()_\soft_name\()_relaxed, \operation, ldxr, stxr, \soft
	rcw_scalar_full_modes \operation_name\()_\soft_name\()_release, \operation, ldxr, stlxr, \soft
	rcw_scalar_live_result_modes \operation_name\()_\soft_name\()_acquire, \operation, ldaxr, stxr, \soft
	rcw_scalar_live_result_modes \operation_name\()_\soft_name\()_acquire_release, \operation, ldaxr, stlxr, \soft
.endm

.macro rcw_scalar_family operation_name, operation
	rcw_scalar_orderings \operation_name, \operation, hard, 0
	rcw_scalar_orderings \operation_name, \operation, soft, 1
.endm

rcw_scalar_family cas, 0
rcw_scalar_family bic, 1
rcw_scalar_family swp, 2
rcw_scalar_family orr, 3

.purgem rcw_scalar_family
.purgem rcw_scalar_orderings
.purgem rcw_scalar_live_result_modes
.purgem rcw_scalar_full_modes
.purgem rcw_scalar_gadget

/* FEAT_D128/FEAT_THE pair Read-Check-Write operations. A native CASP loop
 * validates the speculative pair snapshot and commits one indivisible
 * 128-bit write. Operation, hard/software checking, ordering, and implied
 * pair-ZR cases are selected at translation time. Compare/check outcomes use
 * conditional selects, leaving only the exclusive CAS retry in the mapped
 * hot path. Stream: packed base/source/result CpuState offsets, guest PC. */
.macro rcw_pair_gadget name, operation, compare_swap, compare_exchange, soft, source_second_zero, result_second_zero
gadget rcw_pair_\name
	ldr x19, [_stream], #8
	and w20, w19, #0xffff
	/* Translation packed the direct CpuState offset, including SP. */
	ldr x7, [_cpu, x20]
	tst x7, #15
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	ubfx x20, x19, #16, #16
	.if \compare_swap
		.if \source_second_zero
		ldr x24, [_cpu, x20]
		mov x25, xzr
		.else
		add x8, _cpu, x20
		ldp x24, x25, [x8]
		.endif
		ubfx x20, x19, #32, #16
		.if \result_second_zero
		ldr x26, [_cpu, x20]
		mov x6, xzr
		.else
		add x8, _cpu, x20
		ldp x26, x6, [x8]
		.endif
	.else
		ldr x24, [_cpu, x20]
		ubfx x21, x19, #32, #16
		ldr x25, [_cpu, x21]
	.endif
	ldp x12, x13, [x7]
1:	mov x22, x12
	mov x23, x13
	.if \operation == 0
		cmp x12, x26
		ccmp x13, x6, #0, eq
		.if \soft
		cset w9, eq
		eor x8, x12, x24
		and w10, w8, #1
		bic w10, w9, w10
		cmp w10, #0
		csel x14, x24, x12, ne
		csel x15, x25, x13, ne
		lsl w11, w10, #29
		mov w8, #0xa0000000
		cmp w9, #0
		csel w11, w11, w8, ne
		.else
		csel x14, x24, x12, eq
		csel x15, x25, x13, eq
		mov w11, #0x20000000
		mov w8, #0xa0000000
		csel w11, w11, w8, eq
		.endif
	.elseif \operation == 1
		bic x14, x12, x24
		bic x15, x13, x25
	.elseif \operation == 2
		mov x14, x24
		mov x15, x25
	.else
		orr x14, x12, x24
		orr x15, x13, x25
	.endif
	.if \operation != 0
		.if \soft
		eor x8, x12, x14
		tst x8, #1
		csel x14, x14, x12, eq
		csel x15, x15, x13, eq
		mov w11, #0x20000000
		csel w11, w11, wzr, eq
		.else
		mov w11, #0x20000000
		.endif
	.endif
	\compare_exchange x12, x13, x14, x15, [x7]
	cmp x12, x22
	ccmp x13, x23, #0, eq
	b.ne 1b
	.if \compare_swap
		ubfx x20, x19, #32, #16
		.if !\result_second_zero
		add x8, _cpu, x20
		stp x12, x13, [x8]
		.else
		str x12, [_cpu, x20]
		.endif
	.else
		ubfx x20, x19, #16, #16
		ubfx x21, x19, #32, #16
		str x12, [_cpu, x20]
		str x13, [_cpu, x21]
	.endif
	mov w27, w11
	add _stream, _stream, #8
	next_gadget
.endm

.macro rcw_pair_cas_modes name, compare_exchange, soft
	rcw_pair_gadget \name\()_rr, 0, 1, \compare_exchange, \soft, 0, 0
	rcw_pair_gadget \name\()_rz, 0, 1, \compare_exchange, \soft, 0, 1
	rcw_pair_gadget \name\()_zr, 0, 1, \compare_exchange, \soft, 1, 0
	rcw_pair_gadget \name\()_zz, 0, 1, \compare_exchange, \soft, 1, 1
.endm

.macro rcw_pair_cas_orders soft_name, soft
	rcw_pair_cas_modes cas_\soft_name\()_relaxed, casp, \soft
	rcw_pair_cas_modes cas_\soft_name\()_release, caspl, \soft
	rcw_pair_cas_modes cas_\soft_name\()_acquire, caspa, \soft
	rcw_pair_cas_modes cas_\soft_name\()_acquire_release, caspal, \soft
.endm

.macro rcw_pair_modify_orders operation_name, operation, soft_name, soft
	rcw_pair_gadget \operation_name\()_\soft_name\()_relaxed, \operation, 0, casp, \soft, 0, 0
	rcw_pair_gadget \operation_name\()_\soft_name\()_release, \operation, 0, caspl, \soft, 0, 0
	rcw_pair_gadget \operation_name\()_\soft_name\()_acquire, \operation, 0, caspa, \soft, 0, 0
	rcw_pair_gadget \operation_name\()_\soft_name\()_acquire_release, \operation, 0, caspal, \soft, 0, 0
.endm

rcw_pair_cas_orders hard, 0
rcw_pair_cas_orders soft, 1
rcw_pair_modify_orders bic, 1, hard, 0
rcw_pair_modify_orders bic, 1, soft, 1
rcw_pair_modify_orders swp, 2, hard, 0
rcw_pair_modify_orders swp, 2, soft, 1
rcw_pair_modify_orders orr, 3, hard, 0
rcw_pair_modify_orders orr, 3, soft, 1

.purgem rcw_pair_modify_orders
.purgem rcw_pair_cas_orders
.purgem rcw_pair_cas_modes
.purgem rcw_pair_gadget

/* FEAT_LSE CASP/CASPT. Pair starts are even, so only register 30's second
 * element can be ZR. That fact is specialized independently for the compare
 * and replacement pairs. */
.macro compare_swap_pair_gadget name, instruction, host_register, alignment, compare_second_zero, replacement_second_zero
gadget compare_swap_pair_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #32, #16
	ldr x7, [_cpu, x20]
	tst x7, #\alignment
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	and x20, x19, #0xffff
	ubfx x21, x19, #16, #16
	ldr x14, [_cpu, x20]
	.if \compare_second_zero
	mov x15, xzr
	.else
	add x22, x20, #8
	ldr x15, [_cpu, x22]
	.endif
	ldr x16, [_cpu, x21]
	.if \replacement_second_zero
	mov x17, xzr
	.else
	add x23, x21, #8
	ldr x17, [_cpu, x23]
	.endif
	\instruction \host_register\()14, \host_register\()15, \
		\host_register\()16, \host_register\()17, [x7]
	str x14, [_cpu, x20]
	.if !\compare_second_zero
	str x15, [_cpu, x22]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro compare_swap_pair_register_modes name, instruction, host_register, alignment
	compare_swap_pair_gadget \name\()_rr, \instruction, \host_register, \alignment, 0, 0
	compare_swap_pair_gadget \name\()_rz, \instruction, \host_register, \alignment, 0, 1
	compare_swap_pair_gadget \name\()_zr, \instruction, \host_register, \alignment, 1, 0
	compare_swap_pair_gadget \name\()_zz, \instruction, \host_register, \alignment, 1, 1
.endm

.macro compare_swap_pair_sizes name, word_instruction, double_instruction
	compare_swap_pair_register_modes \name\()_w, \word_instruction, w, 7
	compare_swap_pair_register_modes \name\()_x, \double_instruction, x, 15
.endm

compare_swap_pair_sizes relaxed, casp, casp
compare_swap_pair_sizes release, caspl, caspl
compare_swap_pair_sizes acquire, caspa, caspa
compare_swap_pair_sizes acquire_release, caspal, caspal

.purgem compare_swap_pair_sizes
.purgem compare_swap_pair_register_modes
.purgem compare_swap_pair_gadget

/* Scalar exclusive load/store operations. The builder chooses load vs store,
 * width, ordering, and ZR/status-discard variants. */
.macro exclusive_load_gadget name, instruction, host_register, alignment, discard_result
gadget exclusive_load_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #16, #16
	ldr x7, [_cpu, x20]
	.if \alignment
	tst x7, #\alignment
	b.ne uml_arm64_gadget_atomic_alignment_fault_read
	.endif
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()15, [x7]
	.if !\discard_result
	and x21, x19, #0xffff
	str x15, [_cpu, x21]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro exclusive_load_modes name, instruction, host_register, alignment
	exclusive_load_gadget \name\()_r, \instruction, \host_register, \alignment, 0
	exclusive_load_gadget \name\()_z, \instruction, \host_register, \alignment, 1
.endm

.macro exclusive_load_sizes name, byte_instruction, half_instruction, word_instruction, double_instruction
	exclusive_load_modes \name\()_b, \byte_instruction, w, 0
	exclusive_load_modes \name\()_h, \half_instruction, w, 1
	exclusive_load_modes \name\()_w, \word_instruction, w, 3
	exclusive_load_modes \name\()_x, \double_instruction, x, 7
.endm

exclusive_load_sizes relaxed, ldxrb, ldxrh, ldxr, ldxr
exclusive_load_sizes acquire, ldaxrb, ldaxrh, ldaxr, ldaxr

.purgem exclusive_load_sizes
.purgem exclusive_load_modes
.purgem exclusive_load_gadget

.macro exclusive_store_gadget name, instruction, host_register, alignment, source_zero, discard_status
gadget exclusive_store_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #32, #16
	ldr x7, [_cpu, x20]
	.if \alignment
	tst x7, #\alignment
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
	.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	.if \source_zero
	mov x15, xzr
	.else
	and x21, x19, #0xffff
	ldr x15, [_cpu, x21]
	.endif
	\instruction w14, \host_register\()15, [x7]
	.if !\discard_status
	ubfx x20, x19, #16, #16
	str x14, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro exclusive_store_modes name, instruction, host_register, alignment
	exclusive_store_gadget \name\()_rr, \instruction, \host_register, \alignment, 0, 0
	exclusive_store_gadget \name\()_rz, \instruction, \host_register, \alignment, 0, 1
	exclusive_store_gadget \name\()_zr, \instruction, \host_register, \alignment, 1, 0
	exclusive_store_gadget \name\()_zz, \instruction, \host_register, \alignment, 1, 1
.endm

.macro exclusive_store_sizes name, byte_instruction, half_instruction, word_instruction, double_instruction
	exclusive_store_modes \name\()_b, \byte_instruction, w, 0
	exclusive_store_modes \name\()_h, \half_instruction, w, 1
	exclusive_store_modes \name\()_w, \word_instruction, w, 3
	exclusive_store_modes \name\()_x, \double_instruction, x, 7
.endm

exclusive_store_sizes relaxed, stxrb, stxrh, stxr, stxr
exclusive_store_sizes release, stlxrb, stlxrh, stlxr, stlxr

.purgem exclusive_store_sizes
.purgem exclusive_store_modes
.purgem exclusive_store_gadget

/* Pair exclusive operations. Fixed host registers capture both load results;
 * translation-time variants only commit architecturally visible destinations
 * and materialize ZR store operands without runtime branches. */
.macro exclusive_pair_load_gadget name, instruction, host_register, alignment, discard_first, discard_second
gadget exclusive_pair_load_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #32, #16
	ldr x7, [_cpu, x20]
	tst x7, #\alignment
	b.ne uml_arm64_gadget_atomic_alignment_fault_read
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()15, \host_register\()16, [x7]
	.if !\discard_first
	and x21, x19, #0xffff
	str x15, [_cpu, x21]
	.endif
	.if !\discard_second
	ubfx x22, x19, #16, #16
	str x16, [_cpu, x22]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro exclusive_pair_load_modes name, instruction, host_register, alignment
	exclusive_pair_load_gadget \name\()_rr, \instruction, \host_register, \alignment, 0, 0
	exclusive_pair_load_gadget \name\()_rz, \instruction, \host_register, \alignment, 0, 1
	exclusive_pair_load_gadget \name\()_zr, \instruction, \host_register, \alignment, 1, 0
	exclusive_pair_load_gadget \name\()_zz, \instruction, \host_register, \alignment, 1, 1
.endm

.macro exclusive_pair_load_sizes name, word_instruction, double_instruction
	exclusive_pair_load_modes \name\()_w, \word_instruction, w, 7
	exclusive_pair_load_modes \name\()_x, \double_instruction, x, 15
.endm

exclusive_pair_load_sizes relaxed, ldxp, ldxp
exclusive_pair_load_sizes acquire, ldaxp, ldaxp

.purgem exclusive_pair_load_sizes
.purgem exclusive_pair_load_modes
.purgem exclusive_pair_load_gadget

.macro exclusive_pair_store_gadget name, instruction, host_register, alignment, first_zero, second_zero, discard_status
gadget exclusive_pair_store_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #48, #16
	ldr x7, [_cpu, x20]
	tst x7, #\alignment
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	.if \first_zero
	mov x15, xzr
	.else
	and x21, x19, #0xffff
	ldr x15, [_cpu, x21]
	.endif
	.if \second_zero
	mov x16, xzr
	.else
	ubfx x22, x19, #16, #16
	ldr x16, [_cpu, x22]
	.endif
	\instruction w14, \host_register\()15, \host_register\()16, [x7]
	.if !\discard_status
	ubfx x20, x19, #32, #16
	str x14, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro exclusive_pair_store_modes name, instruction, host_register, alignment
	exclusive_pair_store_gadget \name\()_rrr, \instruction, \host_register, \alignment, 0, 0, 0
	exclusive_pair_store_gadget \name\()_rrz, \instruction, \host_register, \alignment, 0, 0, 1
	exclusive_pair_store_gadget \name\()_rzr, \instruction, \host_register, \alignment, 0, 1, 0
	exclusive_pair_store_gadget \name\()_rzz, \instruction, \host_register, \alignment, 0, 1, 1
	exclusive_pair_store_gadget \name\()_zrr, \instruction, \host_register, \alignment, 1, 0, 0
	exclusive_pair_store_gadget \name\()_zrz, \instruction, \host_register, \alignment, 1, 0, 1
	exclusive_pair_store_gadget \name\()_zzr, \instruction, \host_register, \alignment, 1, 1, 0
	exclusive_pair_store_gadget \name\()_zzz, \instruction, \host_register, \alignment, 1, 1, 1
.endm

.macro exclusive_pair_store_sizes name, word_instruction, double_instruction
	exclusive_pair_store_modes \name\()_w, \word_instruction, w, 7
	exclusive_pair_store_modes \name\()_x, \double_instruction, x, 15
.endm

exclusive_pair_store_sizes relaxed, stxp, stxp
exclusive_pair_store_sizes release, stlxp, stlxp

.purgem exclusive_pair_store_sizes
.purgem exclusive_pair_store_modes
.purgem exclusive_pair_store_gadget

/* Ordered scalar loads/stores (LDAR/LDLAR and STLR/STLLR families). */
.macro ordered_load_gadget name, instruction, host_register, alignment, discard_result
gadget ordered_load_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #16, #16
	ldr x7, [_cpu, x20]
	.if \alignment
	tst x7, #\alignment
	b.ne uml_arm64_gadget_atomic_alignment_fault_read
	.endif
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()15, [x7]
	.if !\discard_result
	and x21, x19, #0xffff
	str x15, [_cpu, x21]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro ordered_load_modes name, instruction, host_register, alignment
	ordered_load_gadget \name\()_r, \instruction, \host_register, \alignment, 0
	ordered_load_gadget \name\()_z, \instruction, \host_register, \alignment, 1
.endm

.macro ordered_load_sizes name, byte_instruction, half_instruction, word_instruction, double_instruction
	ordered_load_modes \name\()_b, \byte_instruction, w, 0
	ordered_load_modes \name\()_h, \half_instruction, w, 1
	ordered_load_modes \name\()_w, \word_instruction, w, 3
	ordered_load_modes \name\()_x, \double_instruction, x, 7
.endm

ordered_load_sizes relaxed, ldlarb, ldlarh, ldlar, ldlar
ordered_load_sizes acquire, ldarb, ldarh, ldar, ldar

.purgem ordered_load_sizes
.purgem ordered_load_modes
.purgem ordered_load_gadget

.macro ordered_store_gadget name, instruction, host_register, alignment, source_zero
gadget ordered_store_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #16, #16
	ldr x7, [_cpu, x20]
	.if \alignment
	tst x7, #\alignment
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
	.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	.if \source_zero
	mov x15, xzr
	.else
	and x21, x19, #0xffff
	ldr x15, [_cpu, x21]
	.endif
	\instruction \host_register\()15, [x7]
	add _stream, _stream, #8
	next_gadget
.endm

.macro ordered_store_modes name, instruction, host_register, alignment
	ordered_store_gadget \name\()_r, \instruction, \host_register, \alignment, 0
	ordered_store_gadget \name\()_z, \instruction, \host_register, \alignment, 1
.endm

.macro ordered_store_sizes name, byte_instruction, half_instruction, word_instruction, double_instruction
	ordered_store_modes \name\()_b, \byte_instruction, w, 0
	ordered_store_modes \name\()_h, \half_instruction, w, 1
	ordered_store_modes \name\()_w, \word_instruction, w, 3
	ordered_store_modes \name\()_x, \double_instruction, x, 7
.endm

ordered_store_sizes limited, stllrb, stllrh, stllr, stllr
ordered_store_sizes release, stlrb, stlrh, stlr, stlr

.purgem ordered_store_sizes
.purgem ordered_store_modes
.purgem ordered_store_gadget

/* FEAT_LRCPC/2 LDAPR and LDAPUR plus FEAT_LRCPC2 STLUR. The builder packs
 * the signed byte offset and chooses width, extension, and ZR handling, so a
 * valid hot path only calculates the address, translates it, and executes the
 * exact host RCpc/release instruction. Stream: packed operand, guest PC. */
.macro rcpc_load_gadget name, instruction, host_register, alignment, extension, discard
gadget rcpc_load_\name
	ldr x19, [_stream], #8
	and w20, w19, #0x1f
	ubfx w21, w19, #5, #5
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x8, x20, lsl #3]
	asr x9, x19, #10
	add x7, x7, x9
	.if \alignment
	tst x7, #\alignment
	b.ne uml_arm64_gadget_atomic_alignment_fault_read
	.endif
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()15, [x7]
	.if \extension == 1
	sxtb w15, w15
	.elseif \extension == 2
	sxtb x15, w15
	.elseif \extension == 3
	sxth w15, w15
	.elseif \extension == 4
	sxth x15, w15
	.elseif \extension == 5
	sxtw x15, w15
	.endif
	.if !\discard
	add x8, _cpu, #UML_GADGET_CPU_X0
	str x15, [x8, x21, lsl #3]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro rcpc_load_modes name, instruction, host_register, alignment, extension
	rcpc_load_gadget \name\()_r, \instruction, \host_register, \alignment, \extension, 0
	rcpc_load_gadget \name\()_z, \instruction, \host_register, \alignment, \extension, 1
.endm

rcpc_load_modes zero_b, ldaprb, w, 0, 0
rcpc_load_modes zero_h, ldaprh, w, 1, 0
rcpc_load_modes zero_w, ldapr, w, 3, 0
rcpc_load_modes zero_x, ldapr, x, 7, 0
rcpc_load_modes sb_w, ldaprb, w, 0, 1
rcpc_load_modes sb_x, ldaprb, w, 0, 2
rcpc_load_modes sh_w, ldaprh, w, 1, 3
rcpc_load_modes sh_x, ldaprh, w, 1, 4
rcpc_load_modes sw_x, ldapr, w, 3, 5

.purgem rcpc_load_modes
.purgem rcpc_load_gadget

.macro rcpc_store_gadget name, instruction, host_register, alignment, source_zero
gadget rcpc_store_\name
	ldr x19, [_stream], #8
	and w20, w19, #0x1f
	ubfx w21, w19, #5, #5
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x8, x20, lsl #3]
	asr x9, x19, #10
	add x7, x7, x9
	.if \alignment
	tst x7, #\alignment
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
	.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	.if \source_zero
	mov x15, xzr
	.else
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x15, [x8, x21, lsl #3]
	.endif
	\instruction \host_register\()15, [x7]
	add _stream, _stream, #8
	next_gadget
.endm

.macro rcpc_store_modes name, instruction, host_register, alignment
	rcpc_store_gadget \name\()_r, \instruction, \host_register, \alignment, 0
	rcpc_store_gadget \name\()_z, \instruction, \host_register, \alignment, 1
.endm

rcpc_store_modes b, stlrb, w, 0
rcpc_store_modes h, stlrh, w, 1
rcpc_store_modes w, stlr, w, 3
rcpc_store_modes x, stlr, x, 7

.purgem rcpc_store_modes
.purgem rcpc_store_gadget

/* FEAT_LRCPC3 LDIAPP/STILP. Hosts predating RCPC3 execute two native ordered
 * scalar accesses. The first LDAR orders itself before the second load; the
 * second STLR orders the first store before itself. Pre-index STILP reverses
 * the two stores exactly as the architecture requires. Stream: packed
 * registers, guest PC. */
.macro ordered_pair_load_gadget name, host_register, element_size, postindex, discard_first, discard_second
gadget ordered_pair_load_\name
	ldr x19, [_stream], #8
	and w20, w19, #0x1f
	ubfx w21, w19, #5, #5
	ubfx w22, w19, #10, #5
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x8, x20, lsl #3]
	tst x7, #(\element_size - 1)
	b.ne uml_arm64_gadget_atomic_alignment_fault_read
	.if \postindex
	add x24, x7, #(2 * \element_size)
	.endif
	add x25, x7, #\element_size
	eor x23, x7, x25
	lsr x23, x23, #UML_GADGET_GUEST_PAGE_SHIFT
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	cbz x23, 70f
	mov x26, x7
	mov x7, x25
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	mov x9, x7
	mov x7, x26
	b 71f
70:
	add x9, x7, #\element_size
71:
	ldar \host_register\()12, [x7]
	ldar \host_register\()13, [x9]
	add x8, _cpu, #UML_GADGET_CPU_X0
	.if !\discard_first
	str x12, [x8, x21, lsl #3]
	.endif
	.if !\discard_second
	str x13, [x8, x22, lsl #3]
	.endif
	.if \postindex
	str x24, [x8, x20, lsl #3]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro ordered_pair_load_register_modes size_name, host_register, element_size, address_name, postindex
	ordered_pair_load_gadget \size_name\()_\address_name\()_rr, \host_register, \element_size, \postindex, 0, 0
	ordered_pair_load_gadget \size_name\()_\address_name\()_rz, \host_register, \element_size, \postindex, 0, 1
	ordered_pair_load_gadget \size_name\()_\address_name\()_zr, \host_register, \element_size, \postindex, 1, 0
	ordered_pair_load_gadget \size_name\()_\address_name\()_zz, \host_register, \element_size, \postindex, 1, 1
.endm

.macro ordered_pair_load_size size_name, host_register, element_size
	ordered_pair_load_register_modes \size_name, \host_register, \element_size, no, 0
	ordered_pair_load_register_modes \size_name, \host_register, \element_size, post, 1
.endm

ordered_pair_load_size w, w, 4
ordered_pair_load_size x, x, 8

.purgem ordered_pair_load_size
.purgem ordered_pair_load_register_modes
.purgem ordered_pair_load_gadget

.macro ordered_pair_store_gadget name, host_register, element_size, preindex, zero_first, zero_second
gadget ordered_pair_store_\name
	ldr x19, [_stream], #8
	and w20, w19, #0x1f
	ubfx w21, w19, #5, #5
	ubfx w22, w19, #10, #5
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x8, x20, lsl #3]
	.if \preindex
	sub x24, x7, #(2 * \element_size)
	mov x7, x24
	.endif
	tst x7, #(\element_size - 1)
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
	add x25, x7, #\element_size
	eor x23, x7, x25
	lsr x23, x23, #UML_GADGET_GUEST_PAGE_SHIFT
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	cbz x23, 70f
	mov x26, x7
	mov x7, x25
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	mov x9, x7
	mov x7, x26
	b 71f
70:
	add x9, x7, #\element_size
71:
	.if \zero_first
	mov x12, xzr
	.else
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x12, [x8, x21, lsl #3]
	.endif
	.if \zero_second
	mov x13, xzr
	.else
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x13, [x8, x22, lsl #3]
	.endif
	.if \preindex
	stlr \host_register\()13, [x9]
	stlr \host_register\()12, [x7]
	.else
	stlr \host_register\()12, [x7]
	stlr \host_register\()13, [x9]
	.endif
	.if \preindex
	add x8, _cpu, #UML_GADGET_CPU_X0
	str x24, [x8, x20, lsl #3]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro ordered_pair_store_register_modes size_name, host_register, element_size, address_name, preindex
	ordered_pair_store_gadget \size_name\()_\address_name\()_rr, \host_register, \element_size, \preindex, 0, 0
	ordered_pair_store_gadget \size_name\()_\address_name\()_rz, \host_register, \element_size, \preindex, 0, 1
	ordered_pair_store_gadget \size_name\()_\address_name\()_zr, \host_register, \element_size, \preindex, 1, 0
	ordered_pair_store_gadget \size_name\()_\address_name\()_zz, \host_register, \element_size, \preindex, 1, 1
.endm

.macro ordered_pair_store_size size_name, host_register, element_size
	ordered_pair_store_register_modes \size_name, \host_register, \element_size, no, 0
	ordered_pair_store_register_modes \size_name, \host_register, \element_size, pre, 1
.endm

ordered_pair_store_size w, w, 4
ordered_pair_store_size x, x, 8

.purgem ordered_pair_store_size
.purgem ordered_pair_store_register_modes
.purgem ordered_pair_store_gadget

/* FEAT_LSCP LDAP, LDAPP, and STLP. A naturally 16-byte-aligned pair maps to
 * one host single-copy-atomic access: LDAXP/LDXP followed by CLREX for loads,
 * and a CASPL loop for the release store. An address that is only element
 * aligned is allowed by the pair-access rules but can cross the host's 16-byte
 * single-copy-atomic granule; that uncommon case uses two ordered scalar
 * accesses after translating both pages. Register/ZR and RCsc/RCpc choices are
 * all specialized by the builder. Stream: packed registers, guest PC. */
.macro consistent_pair_load_gadget name, scalar_load, ordered, discard_first, discard_second
gadget consistent_pair_\name
	ldr x19, [_stream], #8
	and w20, w19, #0x1f
	ubfx w21, w19, #5, #5
	ubfx w22, w19, #10, #5
	add x8, _cpu, #UML_GADGET_CPU_X0
	/* CpuState::sp follows x30, so Rn==31 remains branch-free. */
	ldr x7, [x8, x20, lsl #3]
	tst x7, #7
	b.ne uml_arm64_gadget_atomic_alignment_fault_read
	add x25, x7, #8
	eor x23, x7, x25
	lsr x23, x23, #UML_GADGET_GUEST_PAGE_SHIFT
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	/* The 16-byte-aligned hot path cannot cross a guest page. */
	tbnz x7, #3, 70f
	.if \ordered
	ldaxp x12, x13, [x7]
	.else
	ldxp x12, x13, [x7]
	.endif
	clrex
	b 73f
70:
	cbz x23, 71f
	mov x26, x7
	mov x7, x25
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	mov x9, x7
	mov x7, x26
	b 72f
71:
	add x9, x7, #8
72:
	\scalar_load x12, [x7]
	\scalar_load x13, [x9]
73:
	add x8, _cpu, #UML_GADGET_CPU_X0
	.if !\discard_first
	str x12, [x8, x21, lsl #3]
	.endif
	.if !\discard_second
	str x13, [x8, x22, lsl #3]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro consistent_pair_load_modes name, scalar_load
	consistent_pair_load_gadget \name\()_rr, \scalar_load, 1, 0, 0
	/* LDAP/LDAPP set acquire only when both architectural results are
	 * visible. These three forms therefore use relaxed host loads. */
	consistent_pair_load_gadget \name\()_rz, ldr, 0, 0, 1
	consistent_pair_load_gadget \name\()_zr, ldr, 0, 1, 0
	consistent_pair_load_gadget \name\()_zz, ldr, 0, 1, 1
.endm

consistent_pair_load_modes ldap, ldar
consistent_pair_load_modes ldapp, ldapr

.purgem consistent_pair_load_modes
.purgem consistent_pair_load_gadget

.macro consistent_pair_store_gadget name, zero_first, zero_second
gadget consistent_pair_\name
	ldr x19, [_stream], #8
	and w20, w19, #0x1f
	ubfx w21, w19, #5, #5
	ubfx w22, w19, #10, #5
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x8, x20, lsl #3]
	tst x7, #7
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
	add x25, x7, #8
	eor x23, x7, x25
	lsr x23, x23, #UML_GADGET_GUEST_PAGE_SHIFT
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	.if \zero_first
	mov x12, xzr
	.else
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x12, [x8, x21, lsl #3]
	.endif
	.if \zero_second
	mov x13, xzr
	.else
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x13, [x8, x22, lsl #3]
	.endif
	tbnz x7, #3, 70f
	/* Prime the compare pair cheaply. CASPL validates it atomically, so a
	 * concurrent change merely takes another loop iteration. */
	ldp x14, x15, [x7]
80:
	mov x10, x14
	mov x11, x15
	caspl x14, x15, x12, x13, [x7]
	cmp x14, x10
	ccmp x15, x11, #0, eq
	b.ne 80b
	b 73f
70:
	cbz x23, 71f
	mov x26, x7
	mov x7, x25
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	mov x9, x7
	mov x7, x26
	b 72f
71:
	add x9, x7, #8
72:
	stlr x12, [x7]
	stlr x13, [x9]
73:
	add _stream, _stream, #8
	next_gadget
.endm

consistent_pair_store_gadget store_rr, 0, 0
consistent_pair_store_gadget store_rz, 0, 1
consistent_pair_store_gadget store_zr, 1, 0
consistent_pair_store_gadget store_zz, 1, 1

.purgem consistent_pair_store_gadget

/* FEAT_LSE128 LDCLRP, LDSETP, and SWPP. Apple hosts provide CASP, so each
 * encoding-specialized gadget uses a native 128-bit CAS loop. Operation and
 * ordering never branch in the valid hot path. */
.macro atomic_pair128_gadget name, operation, compare_swap
gadget atomic_pair128_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #32, #16
	ldr x7, [_cpu, x20]
	tst x7, #15
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	and x20, x19, #0xffff
	ubfx x21, x19, #16, #16
	ldr x24, [_cpu, x20]
	ldr x25, [_cpu, x21]
	ldp x12, x13, [x7]
80:
	mov x22, x12
	mov x23, x13
	.if \operation == 1
	bic x14, x12, x24
	bic x15, x13, x25
	.elseif \operation == 3
	orr x14, x12, x24
	orr x15, x13, x25
	.else
	mov x14, x24
	mov x15, x25
	.endif
	\compare_swap x12, x13, x14, x15, [x7]
	cmp x12, x22
	ccmp x13, x23, #0, eq
	b.ne 80b
	str x12, [_cpu, x20]
	str x13, [_cpu, x21]
	add _stream, _stream, #8
	next_gadget
.endm

.macro atomic_pair128_orders name, operation
	atomic_pair128_gadget \name\()_relaxed, \operation, casp
	atomic_pair128_gadget \name\()_release, \operation, caspl
	atomic_pair128_gadget \name\()_acquire, \operation, caspa
	atomic_pair128_gadget \name\()_acquire_release, \operation, caspal
.endm

atomic_pair128_orders clr, 1
atomic_pair128_orders set, 3
atomic_pair128_orders swp, 8

.purgem atomic_pair128_orders
.purgem atomic_pair128_gadget
