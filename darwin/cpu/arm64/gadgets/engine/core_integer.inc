/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Derived from iSH Asbestos' ARM64 guest gadgets. Runtime blocks contain only
 * pointers to these precompiled routines and inline integer operands.
 */
#include "../../layout/assembly_offsets.h"

/* Apple Silicon exposes the Armv8 CRC and cryptographic extensions. Keep the
 * assembler aware of them so extension-backed gadgets can use native ops. */
.arch armv8.6-a+crc+crypto+sha3+sm4+lor+rcpc+i8mm+fp16

#if __APPLE__
#define NAME(value) _##value
#else
#define NAME(value) value
#endif

_cpu .req x1
_tlb .req x2
_stream .req x28

.macro gadget name
	.globl NAME(uml_arm64_gadget_\name)
	.p2align 4
NAME(uml_arm64_gadget_\name):
.endm

#include "scalar_contract.inc"
#include "vector_contract.inc"

.macro next_gadget
	ldr x8, [_stream], #8
	br x8
.endm

/* Native scalar FP gadgets share a host floating-point environment for the
 * whole threaded block. The first saves the caller's state and selects the
 * guest FPCR; later gadgets notice guest FPCR writes. The execution boundary
 * restores the host state without leaking guest exception flags. */
.macro ensure_fp_environment
	/* Production retains the native guest environment across internal block
	 * exits. FPCR writes update it eagerly, so the steady path is one load
	 * and one predicted branch. Keep the cold system-register setup out of
	 * every generated gadget to minimize code footprint. */
	ldrb w6, [_cpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbnz w6, 99f
	bl uml_arm64_gadget_activate_native_fp_environment
99:
.endm

/* Native FEAT_FP8 instructions consume FPMR.  Save it only for blocks that
 * actually execute such a gadget, then load the current guest value directly
 * before the instruction.  The production builder never selects these paths
 * on a host without the corresponding FP8 feature. */
.macro ensure_fpmr_environment
	ldrb w6, [sp, #0x84]
	cbnz w6, 97f
	.inst 0xd53b4446 /* mrs x6, FPMR */
	str x6, [sp, #0x88]
	mov w6, #1
	strb w6, [sp, #0x84]
97:
	ldr x6, [_cpu, #UML_GADGET_CPU_FPMR]
	.inst 0xd51b4446 /* msr FPMR, x6 */
.endm

.macro restore_fpmr_environment
	ldrb w6, [sp, #0x84]
	cbz w6, 98f
	ldr x6, [sp, #0x88]
	.inst 0xd51b4446 /* msr FPMR, x6 */
	strb wzr, [sp, #0x84]
98:
.endm

/* Translate guest address x7 through the persistent iSH-style TLB. The 16 KiB
 * build uses 16,384 two-way direct sets; the 4 KiB fallback retains 8,192
 * one-way folded entries. On success x7 is the host address. The miss helper
 * updates the entry.
 */
.macro translate access, page_offset, forward=0, fault=uml_arm64_gadget_memory_fault
	/* Entries are keyed by guest page number (address >> page shift). */
	lsr x8, x7, #UML_GADGET_GUEST_PAGE_SHIFT
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
	ubfx x9, x7, #UML_GADGET_GUEST_PAGE_SHIFT, #UML_GADGET_TLB_INDEX_BITS
	add x10, _tlb, x9, lsl #6
#else
	ubfx x9, x7, #UML_GADGET_GUEST_PAGE_SHIFT, #UML_GADGET_TLB_INDEX_BITS
	eor x9, x9, x7, lsr #UML_GADGET_TLB_FOLD_SHIFT
	and w9, w9, #UML_GADGET_TLB_INDEX_MASK
	add x10, _tlb, x9, lsl #5
#endif
	/* Each access-specific tag is paired with its backing delta, so the
	 * successful way needs one cache-line-local load pair and no later load. */
	ldp x11, x12, [x10, #\page_offset]
	cmp x8, x11
	b.eq 91f
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
	.if \access
	ldp x11, x12, [x10, #UML_GADGET_TLB_SECOND_WRITE_PAGE]
	.else
	ldp x11, x12, [x10, #UML_GADGET_TLB_SECOND_READ_PAGE]
	.endif
	cmp x8, x11
	b.eq 93f
#endif
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x7
	mov w2, #\access
	bl NAME(uml_arm64_tlb_miss)
	ldp x1, x2, [sp], #16
	.if \forward
	/* Cross-section conditional relocations are not available on Mach-O.
	 * Only the cold miss path needs this long-branch trampoline. */
	cbnz x0, 94f
	b \fault
94:
	.else
	cbz x0, \fault
	.endif
	mov x7, x0
	b 92f
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
93:
	add x7, x12, x7
	b 92f
#endif
91:
	add x7, x12, x7
92:
.endm

/* A translated transfer is at most 64 bytes, so adding its final byte can
 * cross at most one guest-page boundary.  The xor therefore needs only the
 * page-index low bit, avoiding a separate mask/compare instruction. */
.macro branch_if_cross_page address, bytes, scratch, target
	add \scratch, \address, #(\bytes - 1)
	eor \scratch, \scratch, \address
	tbnz \scratch, #UML_GADGET_GUEST_PAGE_SHIFT, \target
.endm

/* Hot-path translation of a same-page transfer of \bytes bytes at \address,
 * which becomes the host address on the fall-through path. The pair
 * transfer_translate / transfer_translate_cold replaces branch_if_cross_page
 * plus translate; the cold half is placed after the gadget's dispatch and
 * resumes at \resume. \check1 reproduces the control's check of one-byte
 * transfers; the fused build never checks them because they cannot cross.
 *
 * With UML_GADGET_FUSED_TRANSLATE the 16 KiB build keys the lookup by the
 * page of the transfer's final byte while indexing by its first: pages n and
 * n + 1 never share a direct-mapped set, so a crossing transfer can only miss
 * and the cold path is where it is recognized. The folded 4 KiB index can map
 * neighbouring pages to one set, so that build keeps the explicit check in
 * front. The key is shifted before a plain compare: a shifted-operand compare
 * resolved the branch late enough to cost two percent on transfer-bound
 * loops. Scratch: x8-x12.
 */
.macro transfer_translate access, page_offset, bytes, address, cold, resume, cross, forward=0, check1=0, fault=uml_arm64_gadget_memory_fault
#if UML_GADGET_FUSED_TRANSLATE
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
	.if \bytes > 1
	add x8, \address, #(\bytes - 1)
	.endif
	ubfx x9, \address, #UML_GADGET_GUEST_PAGE_SHIFT, #UML_GADGET_TLB_INDEX_BITS
	add x10, _tlb, x9, lsl #6
	ldp x11, x12, [x10, #\page_offset]
	.if \bytes > 1
	lsr x8, x8, #UML_GADGET_GUEST_PAGE_SHIFT
	.else
	lsr x8, \address, #UML_GADGET_GUEST_PAGE_SHIFT
	.endif
#else
	.if \bytes > 1
	branch_if_cross_page \address, \bytes, x9, \cross
	.endif
	ubfx x9, \address, #UML_GADGET_GUEST_PAGE_SHIFT, #UML_GADGET_TLB_INDEX_BITS
	eor x9, x9, \address, lsr #UML_GADGET_TLB_FOLD_SHIFT
	and w9, w9, #UML_GADGET_TLB_INDEX_MASK
	add x10, _tlb, x9, lsl #5
	ldp x11, x12, [x10, #\page_offset]
	lsr x8, \address, #UML_GADGET_GUEST_PAGE_SHIFT
#endif
	cmp x11, x8
	b.ne \cold
	add \address, x12, \address
\resume:
#else
	.if \bytes > 1 || \check1
	branch_if_cross_page \address, \bytes, x9, \cross
	.endif
	.ifnc \address, x7
	mov x7, \address
	.endif
	translate \access, \page_offset, \forward, \fault
	.ifnc \address, x7
	mov \address, x7
	.endif
#endif
.endm

/* Second way, crossing, miss and fault handling for transfer_translate. The
 * two builds emit different word counts per entry; whichever is shorter pads
 * the difference after its last branch, so both keep every later entry at the
 * same address for matched A/B layouts. */
/* outofline=1 when the caller places this tail in a section of its own: a
 * conditional branch cannot leave its section, so the fault exit takes the
 * same trampoline the forwarding arm already uses. */
.macro transfer_translate_cold access, page_offset, bytes, address, cold, resume, cross, forward=0, check1=0, outofline=0, fault=uml_arm64_gadget_memory_fault
#if UML_GADGET_FUSED_TRANSLATE
\cold:
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
	.if \access
	ldp x11, x12, [x10, #UML_GADGET_TLB_SECOND_WRITE_PAGE]
	.else
	ldp x11, x12, [x10, #UML_GADGET_TLB_SECOND_READ_PAGE]
	.endif
	cmp x11, x8
	b.eq 93f
	.if \bytes > 1
	branch_if_cross_page \address, \bytes, x9, \cross
	.endif
#endif
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, \address
	mov w2, #\access
	bl NAME(uml_arm64_tlb_miss)
	ldp x1, x2, [sp], #16
	.if \forward || \outofline
	cbnz x0, 94f
	b \fault
94:
	.else
	cbz x0, \fault
	.endif
	mov \address, x0
	b \resume
#if DARWIN_UML_GUEST_PAGE_SIZE == 16384
93:
	add \address, x12, \address
	b \resume
#endif
	/* Words the control has and this build lacks: three when the control
	 * checked a one-byte transfer, two for the pair moves, minus one for the
	 * relocated cross-page check of a wider transfer (16 KiB). */
	.set .Ltt_pad\@, 0
	.if \bytes == 1 && \check1
	.set .Ltt_pad\@, .Ltt_pad\@ + 3
	.endif
	.ifnc \address, x7
	.set .Ltt_pad\@, .Ltt_pad\@ + 2
	.endif
	.if \bytes > 1 && DARWIN_UML_GUEST_PAGE_SIZE == 16384
	.set .Ltt_pad\@, .Ltt_pad\@ - 1
	.endif
	.if .Ltt_pad\@ > 0
	.space 4 * .Ltt_pad\@
	.endif
#elif DARWIN_UML_GUEST_PAGE_SIZE == 16384
	/* The fused build's wider x7 transfers carry one word more; pad the
	 * control so the A/B layouts match. */
	.if \bytes > 1
	.ifc \address, x7
	.space 4
	.endif
	.endif
#endif
.endm

/* Rare cross-page accesses stay on the gadget path. The helper validates all
 * pages before stores, so a fault cannot leave a partially modified range.
 * q0/q1 carry up to 32 bytes to or from the helper.
 */
.macro transfer_slow access, address, size, fault=uml_arm64_gadget_memory_fault
	sub sp, sp, #48
	stp x1, x2, [sp, #32]
	.if \access
	stp q0, q1, [sp]
	.else
	movi v0.16b, #0
	movi v1.16b, #0
	stp q0, q1, [sp]
	.endif
	mov x0, x2
	mov x1, \address
	mov x2, sp
	mov x3, #\size
	mov w4, #\access
	bl NAME(uml_arm64_memory_transfer)
	ldp x1, x2, [sp, #32]
	cbz w0, 97f
	.if !\access
	ldp q0, q1, [sp]
	.endif
	add sp, sp, #48
	b 98f
97:
	add sp, sp, #48
	b \fault
98:
.endm

/*
 * Execute pointer signing/authentication using guest-owned key state. The
 * helper is intentionally outside the normal integer hot path: these
 * instructions are uncommon, and an EL0 process cannot install guest keys
 * into the host's protected key registers.
 *
 * Operations:
 *   0..3 sign instruction-A/B and data-A/B
 *   4..7 authenticate instruction-A/B and data-A/B
 *   8 generic, 9 strip instruction, 10 strip data
 */
.macro software_pointer_auth value, modifier, operation
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, \value
	mov x2, \modifier
	mov w3, #\operation
	bl NAME(uml_arm64_guest_pointer_auth)
	mov \value, x0
	ldp x1, x2, [sp], #16
.endm

.text
.p2align 4
uml_arm64_gadget_activate_native_fp_environment:
	mrs x6, fpcr
	mrs x5, fpsr
	str x6, [_cpu, #UML_GADGET_CPU_HOST_FPCR]
	str x5, [_cpu, #UML_GADGET_CPU_HOST_FPSR]
	mov w6, #1
	strb w6, [_cpu, #UML_GADGET_CPU_FP_ACTIVE]
	ldr w6, [_cpu, #UML_GADGET_CPU_FPCR]
	mov w5, #0x9f00
	bic w6, w6, w5
	msr fpcr, x6
	ldr w6, [_cpu, #UML_GADGET_CPU_FPSR]
	msr fpsr, x6
	ret

.globl NAME(uml_arm64_gadget_enter)
.p2align 4
NAME(uml_arm64_gadget_enter):
	/* x0 = data-only program stream, x1 = CpuState, x2 = TLB,
	 * x3 = initial guest instruction count. x29 is otherwise unused by the
	 * engine and remains live across helper calls. The two-register result
	 * returns the event in w0 and completed count in x1 without a hot-boundary
	 * memory store. Apple reserves x18, so it remains untouched.
	 */
	stp x18, x19, [sp, #-0x90]!
	stp x20, x21, [sp, #0x10]
	stp x22, x23, [sp, #0x20]
	stp x24, x25, [sp, #0x30]
	stp x26, x27, [sp, #0x40]
	stp x28, x29, [sp, #0x50]
	str lr, [sp, #0x60]
#if UML_GADGET_EXECUTION_HISTOGRAM
	str x4, [sp, #0x70]
#endif
	str wzr, [sp, #0x84]
	mov _stream, x0
	/* Bias the retirement accumulator by the linked-chain limit. This lets
	 * follow_block_link test its sign bit in one instruction while preserving
	 * the exact architectural count at the execution boundary. */
	sub x29, x3, #1, lsl #12
	/* Keep the architectural flags in a callee-saved host register. Gadget
	 * scratch comparisons may clobber native PSTATE, so consumers still issue
	 * MSR NZCV, but no per-instruction CpuState load/store is required. */
	ldr w27, [_cpu, #UML_GADGET_CPU_NZCV]
	next_gadget

.p2align 4
uml_arm64_gadget_leave:
	str w27, [_cpu, #UML_GADGET_CPU_NZCV]
	restore_fpmr_environment
	add x1, x29, #1, lsl #12
	ldr lr, [sp, #0x60]
	ldp x28, x29, [sp, #0x50]
	ldp x26, x27, [sp, #0x40]
	ldp x24, x25, [sp, #0x30]
	ldp x22, x23, [sp, #0x20]
	ldp x20, x21, [sp, #0x10]
	ldp x18, x19, [sp], #0x90
	ret
	/* Retain the established alignment of the core integer gadget bank. */
	.space 32

/* MOVZ/MOVN are folded to a full constant by the translator.  Rd==XZR is
 * folded to NOP, so the live entry is a three-load straight-line store. */
scalar_entry move_wide_constant, 0, 0, scalar_offset0, scalar_none, scalar_none, scalar_none, 2
	ldr x8, [_stream], #8
	ldr _scalar_x11, [_stream], #8
	add x13, _cpu, #UML_GADGET_CPU_X0
	str _scalar_x11, [x13, x8]
	next_gadget

/* MOVK keeps only Rd and imm16 in the stream. Width and halfword position are
 * separate entries; invalid 32-bit halfwords and XZR were rejected/folded by
 * the translator. */
/* BFI is exactly MOVK's semantics: insert 16 bits at a fixed position, leave
 * the rest. The mask-shift-clear-insert sequence it replaces was three to five
 * instructions with a bic->orr dependency three deep. For the 32-bit entries it
 * also subsumes the zero-extend, because BFI on a w register writes bits 63:32
 * as zero on its own.
 *
 * The trailing .space keeps every later gadget at the address and 64-byte phase
 * it had before, so this is measured against an otherwise identical bank - the
 * G96 rule, after G17 and G36 were discarded for what turned out to be layout
 * movement. Removing the padding to collect the I-cache saving is a separate
 * trial with its own measurement. */
.macro move_wide_keep bits64, shift, suffix, forward=0
	.set .Lsd\@, scalar_register0
	.set .Lsn\@, scalar_register0
scalar_entry move_wide_keep_\suffix, \forward, 0, .Lsd\@, .Lsn\@, scalar_none, scalar_none, 1
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx x11, x8, #8, #16
	add x13, _cpu, #UML_GADGET_CPU_X0
	.if \forward & 1
	mov _scalar_x14, x0
	.else
	ldr _scalar_x14, [x13, x9, lsl #3]
	.endif
	.if \bits64
	bfi _scalar_x14, x11, #\shift, #16
	.else
	bfi _scalar_w14, w11, #\shift, #16
	.endif
	str _scalar_x14, [x13, x9, lsl #3]
	next_gadget
	.if \bits64
		.if \shift
	.space 16
		.else
	.space 8
		.endif
	.else
		.if \shift
	.space 20
		.else
	.space 12
		.endif
	.endif
	scalar_variants \forward, .Lsn\@, scalar_none, scalar_none, move_wide_keep, \bits64, \shift, \suffix
.endm

move_wide_keep 0, 0, w0
move_wide_keep 0, 16, w1
move_wide_keep 1, 0, x0
move_wide_keep 1, 16, x1
move_wide_keep 1, 32, x2
move_wide_keep 1, 48, x3

.purgem move_wide_keep

/* Production ADD/SUB-immediate entries.  Width, operation, flag update and
 * both architectural register-31 meanings are fixed by the translator.  The
 * compact operand is Rd[4:0], Rn[12:8], immediate[39:16].  These variants
 * deliberately contain no opcode tests or conditional branches. */
.macro add_sub_immediate_fast bits64, subtract, flags, source_sp, destination_special, suffix, forward=0
	.set .Lsd\@, scalar_register0
	.if \destination_special && !\flags
	.set .Lsd\@, scalar_sp
	.endif
	.if \destination_special && \flags
	.set .Lsd\@, scalar_none
	.endif
	.set .Lsn\@, scalar_register8
	.if \source_sp
	.set .Lsn\@, scalar_sp
	.endif
scalar_entry add_sub_immediate_\suffix, \forward, 1, .Lsd\@, .Lsn\@, scalar_none, scalar_none, 1
#if UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST
	/* This family is non-faulting and leaves x14 free. Builder always emits a
	 * terminal after a translated fallthrough, so the successor word exists
	 * even when this is the final guest instruction in the block. */
	ldp x8, x14, [_stream], #16
#else
	ldr x8, [_stream], #8
#endif
	.if \source_sp
	scalar_load_sp \forward, 1, x15
	.else
	scalar_load \forward, 1, x15, 10, 8, 8, 5, 3, _cpu
	.endif
	lsr x11, x8, #16
	.if \bits64
		.if \subtract
			.if \flags
	subs _scalar_x16, x15, x11
			.else
	sub _scalar_x16, x15, x11
			.endif
		.else
			.if \flags
	adds _scalar_x16, x15, x11
			.else
	add _scalar_x16, x15, x11
			.endif
		.endif
	.else
		.if \subtract
			.if \flags
	subs _scalar_w16, w15, w11
			.else
	sub _scalar_w16, w15, w11
			.endif
		.else
			.if \flags
	adds _scalar_w16, w15, w11
			.else
	add _scalar_w16, w15, w11
			.endif
		.endif
	.endif
	.if \flags
	mrs x27, nzcv
	.endif
	.if \destination_special
		.if !\flags
	str _scalar_x16, [_cpu, #UML_GADGET_CPU_SP]
		.endif
	.else
	and w9, w8, #0x1f
	str _scalar_x16, [_cpu, x9, lsl #3]
	.endif
#if UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST
	br x14
	/* The removed successor LDR is restored as unreachable padding. Every
	 * affected entry therefore retains its baseline size and all later entry
	 * addresses and 64-byte phases remain unchanged. */
	.space 4
#else
	next_gadget
#endif
	scalar_variants \forward, .Lsn\@, scalar_none, scalar_none, add_sub_immediate_fast, \bits64, \subtract, \flags, \source_sp, \destination_special, \suffix
.endm

.macro add_sub_immediate_modes bits64, subtract, flags, prefix
add_sub_immediate_fast \bits64, \subtract, \flags, 0, 0, \prefix\()_rr
add_sub_immediate_fast \bits64, \subtract, \flags, 0, 1, \prefix\()_rs
add_sub_immediate_fast \bits64, \subtract, \flags, 1, 0, \prefix\()_sr
add_sub_immediate_fast \bits64, \subtract, \flags, 1, 1, \prefix\()_ss
.endm

add_sub_immediate_modes 0, 0, 0, w_add_n
add_sub_immediate_modes 0, 0, 1, w_add_s
add_sub_immediate_modes 0, 1, 0, w_sub_n
add_sub_immediate_modes 0, 1, 1, w_sub_s
add_sub_immediate_modes 1, 0, 0, x_add_n
add_sub_immediate_modes 1, 0, 1, x_add_s
add_sub_immediate_modes 1, 1, 0, x_sub_n
add_sub_immediate_modes 1, 1, 1, x_sub_s

.purgem add_sub_immediate_modes
.purgem add_sub_immediate_fast

/* Production ADD/SUB shifted/extended register entries. CpuState byte offsets
 * for ordinary registers/SP and the shift amount are prepacked by Builder.
 * Width, arithmetic, flags, transform, XZR inputs, and discard/SP destination
 * behavior are all entry-point constants. */
.macro add_sub_register_finish bits64, subtract, flags, destination_discard
	.if \bits64
		.if \subtract
			.if \flags
	subs _scalar_x7, x15, x16
			.else
	sub _scalar_x7, x15, x16
			.endif
		.else
			.if \flags
	adds _scalar_x7, x15, x16
			.else
	add _scalar_x7, x15, x16
			.endif
		.endif
	.else
		.if \subtract
			.if \flags
	subs _scalar_w7, w15, w16
			.else
	sub _scalar_w7, w15, w16
			.endif
		.else
			.if \flags
	adds _scalar_w7, w15, w16
			.else
	add _scalar_w7, w15, w16
			.endif
		.endif
	.endif
	.if \flags
	mrs x27, nzcv
	.endif
	.if !\destination_discard
	and x9, x8, #0xffff
	str _scalar_x7, [_cpu, x9]
	.endif
	next_gadget
.endm

.macro add_sub_register_shifted_fast bits64, subtract, flags, shift_kind, left_zero, right_zero, destination_discard, suffix, forward=0
	.set .Lsd\@, scalar_offset0
	.if \destination_discard
	.set .Lsd\@, scalar_none
	.endif
	.set .Lsn\@, scalar_offset16
	.if \left_zero
	.set .Lsn\@, scalar_none
	.endif
	.set .Lsm\@, scalar_offset32
	.if \right_zero
	.set .Lsm\@, scalar_none
	.endif
scalar_entry add_sub_register_\suffix, \forward, 2, .Lsd\@, .Lsn\@, .Lsm\@, scalar_none, 1
	ldr x8, [_stream], #8
	.if \left_zero
	mov x15, xzr
	.else
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _cpu
	.endif
	.if \right_zero
	mov x16, xzr
	.else
	scalar_load \forward, 2, x16, 11, 8, 32, 16, 0, _cpu
	.endif
	.if \shift_kind != 0
	ubfx x6, x8, #48, #6
		.if \bits64
			.if \shift_kind == 1
	lsl x16, x16, x6
			.elseif \shift_kind == 2
	lsr x16, x16, x6
			.else
	asr x16, x16, x6
			.endif
		.else
			.if \shift_kind == 1
	lsl w16, w16, w6
			.elseif \shift_kind == 2
	lsr w16, w16, w6
			.else
	asr w16, w16, w6
			.endif
		.endif
	.endif
	add_sub_register_finish \bits64, \subtract, \flags, \destination_discard
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, add_sub_register_shifted_fast, \bits64, \subtract, \flags, \shift_kind, \left_zero, \right_zero, \destination_discard, \suffix
.endm

.macro add_sub_register_extended_fast bits64, subtract, flags, extension, shift_nonzero, right_zero, destination_discard, suffix, forward=0
	.set .Lsd\@, scalar_offset0
	.if \destination_discard
	.set .Lsd\@, scalar_none
	.endif
	.set .Lsn\@, scalar_offset16
	.set .Lsm\@, scalar_offset32
	.if \right_zero
	.set .Lsm\@, scalar_none
	.endif
scalar_entry add_sub_register_\suffix, \forward, 3, .Lsd\@, .Lsn\@, .Lsm\@, scalar_none, 1
	ldr x8, [_stream], #8
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _cpu
	.if \right_zero
	mov x16, xzr
	.else
	scalar_load \forward, 2, x16, 11, 8, 32, 16, 0, _cpu
	.endif
	.if \extension == 0
	and x16, x16, #0xff
	.elseif \extension == 1
	and x16, x16, #0xffff
	.elseif \extension == 2
	mov w16, w16
	.elseif \extension == 4
	sxtb x16, w16
	.elseif \extension == 5
	sxth x16, w16
	.elseif \extension == 6
	sxtw x16, w16
	.endif
	.if \shift_nonzero
	ubfx x6, x8, #48, #3
	lsl x16, x16, x6
	.endif
	add_sub_register_finish \bits64, \subtract, \flags, \destination_discard
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, add_sub_register_extended_fast, \bits64, \subtract, \flags, \extension, \shift_nonzero, \right_zero, \destination_discard, \suffix
.endm

.macro add_sub_shifted_destinations bits64, subtract, flags, shift_kind, left_zero, right_zero, prefix
add_sub_register_shifted_fast \bits64, \subtract, \flags, \shift_kind, \left_zero, \right_zero, 0, \prefix\()_r
add_sub_register_shifted_fast \bits64, \subtract, \flags, \shift_kind, \left_zero, \right_zero, 1, \prefix\()_d
.endm
.macro add_sub_shifted_sources bits64, subtract, flags, shift_kind, prefix
add_sub_shifted_destinations \bits64, \subtract, \flags, \shift_kind, 0, 0, \prefix\()_rr
add_sub_shifted_destinations \bits64, \subtract, \flags, \shift_kind, 0, 1, \prefix\()_rz
add_sub_shifted_destinations \bits64, \subtract, \flags, \shift_kind, 1, 0, \prefix\()_zr
add_sub_shifted_destinations \bits64, \subtract, \flags, \shift_kind, 1, 1, \prefix\()_zz
.endm
.macro add_sub_shifted_transforms bits64, subtract, flags, prefix
add_sub_shifted_sources \bits64, \subtract, \flags, 0, \prefix\()_n
add_sub_shifted_sources \bits64, \subtract, \flags, 1, \prefix\()_l
add_sub_shifted_sources \bits64, \subtract, \flags, 2, \prefix\()_r
add_sub_shifted_sources \bits64, \subtract, \flags, 3, \prefix\()_a
.endm

.macro add_sub_extended_destinations bits64, subtract, flags, extension, shift_nonzero, right_zero, prefix
add_sub_register_extended_fast \bits64, \subtract, \flags, \extension, \shift_nonzero, \right_zero, 0, \prefix\()_r
add_sub_register_extended_fast \bits64, \subtract, \flags, \extension, \shift_nonzero, \right_zero, 1, \prefix\()_d
.endm
.macro add_sub_extended_sources bits64, subtract, flags, extension, shift_nonzero, prefix
add_sub_extended_destinations \bits64, \subtract, \flags, \extension, \shift_nonzero, 0, \prefix\()_r
add_sub_extended_destinations \bits64, \subtract, \flags, \extension, \shift_nonzero, 1, \prefix\()_z
.endm
.macro add_sub_extended_transforms bits64, subtract, flags, prefix
add_sub_extended_sources \bits64, \subtract, \flags, 0, 0, \prefix\()_e0n
add_sub_extended_sources \bits64, \subtract, \flags, 0, 1, \prefix\()_e0s
add_sub_extended_sources \bits64, \subtract, \flags, 1, 0, \prefix\()_e1n
add_sub_extended_sources \bits64, \subtract, \flags, 1, 1, \prefix\()_e1s
add_sub_extended_sources \bits64, \subtract, \flags, 2, 0, \prefix\()_e2n
add_sub_extended_sources \bits64, \subtract, \flags, 2, 1, \prefix\()_e2s
add_sub_extended_sources \bits64, \subtract, \flags, 3, 0, \prefix\()_e3n
add_sub_extended_sources \bits64, \subtract, \flags, 3, 1, \prefix\()_e3s
add_sub_extended_sources \bits64, \subtract, \flags, 4, 0, \prefix\()_e4n
add_sub_extended_sources \bits64, \subtract, \flags, 4, 1, \prefix\()_e4s
add_sub_extended_sources \bits64, \subtract, \flags, 5, 0, \prefix\()_e5n
add_sub_extended_sources \bits64, \subtract, \flags, 5, 1, \prefix\()_e5s
add_sub_extended_sources \bits64, \subtract, \flags, 6, 0, \prefix\()_e6n
add_sub_extended_sources \bits64, \subtract, \flags, 6, 1, \prefix\()_e6s
add_sub_extended_sources \bits64, \subtract, \flags, 7, 0, \prefix\()_e7n
add_sub_extended_sources \bits64, \subtract, \flags, 7, 1, \prefix\()_e7s
.endm

.macro add_sub_register_all bits64, subtract, flags, prefix
add_sub_shifted_transforms \bits64, \subtract, \flags, \prefix\()_sh
add_sub_extended_transforms \bits64, \subtract, \flags, \prefix\()_ex
.endm

add_sub_register_all 0, 0, 0, w_add_n
add_sub_register_all 0, 0, 1, w_add_s
add_sub_register_all 0, 1, 0, w_sub_n
add_sub_register_all 0, 1, 1, w_sub_s
add_sub_register_all 1, 0, 0, x_add_n
add_sub_register_all 1, 0, 1, x_add_s
add_sub_register_all 1, 1, 0, x_sub_n
add_sub_register_all 1, 1, 1, x_sub_s

.purgem add_sub_register_all
.purgem add_sub_extended_transforms
.purgem add_sub_extended_sources
.purgem add_sub_extended_destinations
.purgem add_sub_shifted_transforms
.purgem add_sub_shifted_sources
.purgem add_sub_shifted_destinations
.purgem add_sub_register_extended_fast
.purgem add_sub_register_shifted_fast
.purgem add_sub_register_finish

/* Production ADC/SBC entries. All instruction-invariant choices, including
 * XZR, equal sources, and a discarded destination, are translation-selected.
 * Stream operand: destination byte offset[15:0], left byte offset[31:16],
 * right byte offset[47:32]. */
.macro add_sub_carry_fast bits64, subtract, flags, left_zero, right_zero, same_source, destination_zero, suffix, forward=0
	.set .Lsd\@, scalar_offset0
	.if \destination_zero
	.set .Lsd\@, scalar_none
	.endif
	.set .Lsn\@, scalar_offset16
	.if \left_zero
	.set .Lsn\@, scalar_none
	.endif
	.set .Lsm\@, scalar_offset32
	.if \right_zero || \same_source
	.set .Lsm\@, scalar_none
	.endif
scalar_entry add_sub_carry_\suffix, \forward, 4, .Lsd\@, .Lsn\@, .Lsm\@, scalar_none, 1
	ldr x8, [_stream], #8
	.if \left_zero
	mov x15, xzr
	.else
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _cpu
	.endif
	.if \right_zero
	mov x16, xzr
	.elseif \same_source
	mov x16, x15
	.else
	scalar_load \forward, 2, x16, 11, 8, 32, 16, 0, _cpu
	.endif
	msr nzcv, x27
	.if \bits64
		.if \subtract
			.if \flags
	sbcs _scalar_x7, x15, x16
			.else
	sbc _scalar_x7, x15, x16
			.endif
		.else
			.if \flags
	adcs _scalar_x7, x15, x16
			.else
	adc _scalar_x7, x15, x16
			.endif
		.endif
	.else
		.if \subtract
			.if \flags
	sbcs _scalar_w7, w15, w16
			.else
	sbc _scalar_w7, w15, w16
			.endif
		.else
			.if \flags
	adcs _scalar_w7, w15, w16
			.else
	adc _scalar_w7, w15, w16
			.endif
		.endif
	.endif
	.if \flags
	mrs x27, nzcv
	.endif
	.if !\destination_zero
	uxth w9, w8
	str _scalar_x7, [_cpu, x9]
	.endif
	next_gadget
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, add_sub_carry_fast, \bits64, \subtract, \flags, \left_zero, \right_zero, \same_source, \destination_zero, \suffix
.endm

.macro add_sub_carry_destinations bits64, subtract, flags, left_zero, right_zero, same_source, prefix
add_sub_carry_fast \bits64, \subtract, \flags, \left_zero, \right_zero, \same_source, 0, \prefix\()_r
	.if \flags
add_sub_carry_fast \bits64, \subtract, \flags, \left_zero, \right_zero, \same_source, 1, \prefix\()_z
	.endif
.endm

.macro add_sub_carry_source_modes bits64, subtract, flags, prefix
add_sub_carry_destinations \bits64, \subtract, \flags, 0, 0, 0, \prefix\()_rr
add_sub_carry_destinations \bits64, \subtract, \flags, 0, 0, 1, \prefix\()_eq
add_sub_carry_destinations \bits64, \subtract, \flags, 0, 1, 0, \prefix\()_rz
add_sub_carry_destinations \bits64, \subtract, \flags, 1, 0, 0, \prefix\()_zr
add_sub_carry_destinations \bits64, \subtract, \flags, 1, 1, 0, \prefix\()_zz
.endm

add_sub_carry_source_modes 0, 0, 0, w_adc_n
add_sub_carry_source_modes 0, 0, 1, w_adc_s
add_sub_carry_source_modes 0, 1, 0, w_sbc_n
add_sub_carry_source_modes 0, 1, 1, w_sbc_s
add_sub_carry_source_modes 1, 0, 0, x_adc_n
add_sub_carry_source_modes 1, 0, 1, x_adc_s
add_sub_carry_source_modes 1, 1, 0, x_sbc_n
add_sub_carry_source_modes 1, 1, 1, x_sbc_s

.purgem add_sub_carry_source_modes
.purgem add_sub_carry_destinations
.purgem add_sub_carry_fast

/* FEAT_MTE GMI/SUBP/SUBPS. Encoding-constant SP/ZR choices select one of
 * eight straight-line variants for each operation. No decoder checks or
 * conditional branches remain in the valid production hot path.
 * Translation packs direct CpuState byte offsets:
 * destination[15:0], first[31:16], second[47:32].
 */
.macro mte_pointer opname, operation, n_special, m_special, d_zero, suffix
gadget mte_\opname\()_\suffix
	ldr x8, [_stream], #8
	.if !\n_special
	ubfx x10, x8, #16, #16
	ldr x12, [_cpu, x10]
	.else
	ldr x12, [_cpu, #UML_GADGET_CPU_SP]
	.endif
	.if !\m_special
	ubfx x11, x8, #32, #16
	ldr x13, [_cpu, x11]
	.elseif \operation == 0
	mov x13, xzr
	.else
	ldr x13, [_cpu, #UML_GADGET_CPU_SP]
	.endif

	.if \operation == 0
	ubfx x14, x12, #56, #4
	mov x15, #1
	lsl x15, x15, x14
	orr x14, x13, x15
	.else
	sbfx x12, x12, #0, #56
	sbfx x13, x13, #0, #56
	.if \operation == 2
	subs x14, x12, x13
	mrs x27, nzcv
	.else
	sub x14, x12, x13
	.endif
	.endif

	.if !\d_zero
	uxth w9, w8
	str x14, [_cpu, x9]
	.endif
	next_gadget
.endm

.macro mte_pointer_variants opname, operation
mte_pointer \opname, \operation, 0, 0, 0, rrr
mte_pointer \opname, \operation, 0, 0, 1, rrz
mte_pointer \opname, \operation, 0, 1, 0, rsr
mte_pointer \opname, \operation, 0, 1, 1, rsz
mte_pointer \opname, \operation, 1, 0, 0, srr
mte_pointer \opname, \operation, 1, 0, 1, srz
mte_pointer \opname, \operation, 1, 1, 0, ssr
mte_pointer \opname, \operation, 1, 1, 1, ssz
.endm

mte_pointer_variants gmi, 0
mte_pointer_variants subp, 1
mte_pointer_variants subps, 2

/* ADDG/SUBG use a CpuState table derived from effective MTE enablement and
 * GCR_EL1.Exclude. Source-SP and destination-SP choices are fixed while the
 * stream is built, leaving each production variant entirely branch-free.
 * Translation packs direct CpuState offsets and immediates:
 * destination[15:0], source[31:16], tag offset[35:32], imm6[45:40].
 */
.macro mte_tag_add_sub opname, subtract, source_sp, destination_sp, suffix
gadget mte_\opname\()_\suffix
	ldr x8, [_stream], #8
	.if !\source_sp
	ubfx x10, x8, #16, #16
	ldr x12, [_cpu, x10]
	.else
	ldr x12, [_cpu, #UML_GADGET_CPU_SP]
	.endif
	ubfx x13, x8, #32, #4
	ubfx x14, x12, #56, #4
	orr w13, w13, w14, lsl #4
	add x15, _cpu, #UML_GADGET_CPU_MTE_TAG_CHOICES
	ldrb w13, [x15, x13]
	ubfx x14, x8, #40, #6
	lsl x14, x14, #4
	.if \subtract
	sub x14, x12, x14
	.else
	add x14, x12, x14
	.endif
	bfi x14, x13, #56, #4
	.if \destination_sp
	str x14, [_cpu, #UML_GADGET_CPU_SP]
	.else
	uxth w9, w8
	str x14, [_cpu, x9]
	.endif
	next_gadget
.endm

.macro mte_tag_add_sub_variants opname, subtract
mte_tag_add_sub \opname, \subtract, 0, 0, rr
mte_tag_add_sub \opname, \subtract, 0, 1, rs
mte_tag_add_sub \opname, \subtract, 1, 0, sr
mte_tag_add_sub \opname, \subtract, 1, 1, ss
.endm

mte_tag_add_sub_variants addg, 0
mte_tag_add_sub_variants subg, 1

/* Select the zero-based requested set bit from w15. A 256-byte popcount
 * table in x6 and four branch-free binary-search stages avoid an IRG loop.
 */
.macro irg_select_stage group_mask, shift, result_shift
	and w10, w15, #\group_mask
	ldrb w10, [x6, x10]
	cmp w9, w10
	cset w11, hs
	sub w10, w9, w10
	csel w9, w10, w9, hs
	lsr w10, w15, #\shift
	csel w15, w10, w15, hs
	orr w14, w14, w11, lsl #\result_shift
.endm

/* IRG. Encoding-constant source-SP, mask-XZR, and destination-SP choices are
 * specialized. The two early branches model disabled MTE and the all-excluded
 * case; the RRND branch selects the architecturally runtime GCR mode. Tag
 * selection itself is branch-free. Stream: [direct-offset operand][popcount
 * table].
 */
.macro mte_irg n_sp, m_zero, d_sp, suffix
gadget mte_irg_\suffix
	ldr x8, [_stream], #8
	ldr x6, [_stream], #8
	.if !\n_sp
	ubfx x10, x8, #16, #16
	ldr x12, [_cpu, x10]
	.else
	ldr x12, [_cpu, #UML_GADGET_CPU_SP]
	.endif
	.if !\m_zero
	ubfx x11, x8, #32, #16
	ldr x13, [_cpu, x11]
	.else
	mov x13, xzr
	.endif
	ldrb w14, [_cpu, #UML_GADGET_CPU_MTE_ENABLED]
	cbz w14, .Lirg_zero\@
	ldrh w14, [_cpu, #UML_GADGET_CPU_MTE_EXCLUDE]
	orr w13, w13, w14
	mvn w15, w13
	and w15, w15, #0xffff
	cbz w15, .Lirg_zero\@

	and w16, w15, #0xff
	ldrb w16, [x6, x16]
	lsr w17, w15, #8
	ldrb w17, [x6, x17]
	add w16, w16, w17
	ldrb w17, [_cpu, #UML_GADGET_CPU_MTE_RANDOM_TAGS]
	cbz w17, .Lirg_deterministic\@
	ldr x14, [_cpu, #UML_GADGET_CPU_MTE_RANDOM_STATE]
	eor x14, x14, x14, lsl #13
	eor x14, x14, x14, lsr #7
	eor x14, x14, x14, lsl #17
	str x14, [_cpu, #UML_GADGET_CPU_MTE_RANDOM_STATE]
	udiv x9, x14, x16
	msub x9, x9, x16, x14
	b .Lirg_select\@

.Lirg_deterministic\@:
	ldrb w14, [_cpu, #UML_GADGET_CPU_MTE_TAG]
	add w14, w14, #1
	cmp w14, w16
	csel w14, wzr, w14, hs
	strb w14, [_cpu, #UML_GADGET_CPU_MTE_TAG]
	ldrh w17, [_cpu, #UML_GADGET_CPU_MTE_SEED]
	madd w9, w16, w17, w14
	lsr w9, w9, #16
	udiv w10, w9, w16
	msub w9, w10, w16, w9
	ubfx w10, w17, #5, #1
	eor w10, w10, w17, lsr #3
	eor w10, w10, w17, lsr #2
	eor w10, w10, w17
	and w10, w10, #1
	lsr w17, w17, #1
	orr w17, w17, w10, lsl #15
	strh w17, [_cpu, #UML_GADGET_CPU_MTE_SEED]

.Lirg_select\@:
	mov w14, wzr
	irg_select_stage 0xff, 8, 3
	irg_select_stage 0xf, 4, 2
	irg_select_stage 0x3, 2, 1
	irg_select_stage 0x1, 1, 0
.Lirg_result\@:
	bfi x12, x14, #56, #4
	.if \d_sp
	str x12, [_cpu, #UML_GADGET_CPU_SP]
	.else
	uxth w9, w8
	str x12, [_cpu, x9]
	.endif
	next_gadget
.Lirg_zero\@:
	mov w14, wzr
	b .Lirg_result\@
.endm

mte_irg 0, 0, 0, rrr
mte_irg 0, 0, 1, rrs
mte_irg 0, 1, 0, rzr
mte_irg 0, 1, 1, rzs
mte_irg 1, 0, 0, srr
mte_irg 1, 0, 1, srs
mte_irg 1, 1, 0, szr
mte_irg 1, 1, 1, szs

/* FEAT_CPA ADDPT/SUBPT. Operation, shift, SP/ZR sources, and SP destination
 * are all fixed by translation, producing 128 branch-free hot-path variants.
 * CSEL applies PointerAddCheck only for effective CPTA and a detected carry
 * into the top byte (or a prior poison marker). Stream operand matches IRG.
 */
.macro cpa_pointer opname, subtract, shift, n_sp, m_zero, d_sp, suffix
gadget cpa_\opname\()_\shift\()_\suffix
	ldr x8, [_stream], #8
	.if !\n_sp
	ubfx w10, w8, #8, #5
	add x17, _cpu, #UML_GADGET_CPU_X0
	ldr x12, [x17, x10, lsl #3]
	.else
	ldr x12, [_cpu, #UML_GADGET_CPU_SP]
	.endif
	.if !\m_zero
	ubfx w11, w8, #16, #5
	add x17, _cpu, #UML_GADGET_CPU_X0
	ldr x13, [x17, x11, lsl #3]
	.else
	mov x13, xzr
	.endif
	lsl x13, x13, #\shift
	.if \subtract
	sub x14, x12, x13
	.else
	add x14, x12, x13
	.endif

	eor x15, x14, x12
	lsr x15, x15, #56
	cmp x15, #0
	cset w15, ne
	eor x16, x12, x12, lsr #1
	ubfx x16, x16, #54, #1
	orr w15, w15, w16
	ldrb w16, [_cpu, #UML_GADGET_CPU_CPA_ENABLED]
	tst w15, w16
	mov x17, x14
	lsr x16, x12, #55
	bfi x17, x16, #55, #9
	eor x16, x16, #1
	bfi x17, x16, #54, #1
	csel x14, x17, x14, ne

	.if \d_sp
	str x14, [_cpu, #UML_GADGET_CPU_SP]
	.else
	and w9, w8, #0x1f
	add x17, _cpu, #UML_GADGET_CPU_X0
	str x14, [x17, x9, lsl #3]
	.endif
	next_gadget
.endm

.macro cpa_pointer_modes opname, subtract, shift
cpa_pointer \opname, \subtract, \shift, 0, 0, 0, rrr
cpa_pointer \opname, \subtract, \shift, 0, 0, 1, rrs
cpa_pointer \opname, \subtract, \shift, 0, 1, 0, rzr
cpa_pointer \opname, \subtract, \shift, 0, 1, 1, rzs
cpa_pointer \opname, \subtract, \shift, 1, 0, 0, srr
cpa_pointer \opname, \subtract, \shift, 1, 0, 1, srs
cpa_pointer \opname, \subtract, \shift, 1, 1, 0, szr
cpa_pointer \opname, \subtract, \shift, 1, 1, 1, szs
.endm

.macro cpa_pointer_shifts opname, subtract
cpa_pointer_modes \opname, \subtract, 0
cpa_pointer_modes \opname, \subtract, 1
cpa_pointer_modes \opname, \subtract, 2
cpa_pointer_modes \opname, \subtract, 3
cpa_pointer_modes \opname, \subtract, 4
cpa_pointer_modes \opname, \subtract, 5
cpa_pointer_modes \opname, \subtract, 6
cpa_pointer_modes \opname, \subtract, 7
.endm

cpa_pointer_shifts addpt, 0
cpa_pointer_shifts subpt, 1

/* FEAT_CPA MADDPT/MSUBPT. All XZR source and discard-destination choices are
 * translation-time specializations. MUL+SMULH detect signed product overflow;
 * the independent CPTA/CPTM controls are combined without a hot-path branch.
 * Stream operand packs Rd, Rn, Rm, and Ra into byte lanes 0..3.
 */
.macro cpa_multiply opname, subtract, n_zero, m_zero, a_zero, d_zero, suffix
gadget cpa_\opname\()_\suffix
	.if \d_zero
	ldr x8, [_stream], #8
	next_gadget
	.else
	ldr x8, [_stream], #8
	.if !\a_zero
	ubfx w10, w8, #24, #5
	add x17, _cpu, #UML_GADGET_CPU_X0
	ldr x12, [x17, x10, lsl #3]
	.else
	mov x12, xzr
	.endif

	.if \n_zero || \m_zero
	mov x14, xzr
	mov w6, wzr
	.else
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #5
	add x17, _cpu, #UML_GADGET_CPU_X0
	ldr x13, [x17, x10, lsl #3]
	ldr x11, [x17, x11, lsl #3]
	mul x14, x13, x11
	smulh x6, x13, x11
	asr x7, x14, #63
	cmp x6, x7
	cset w6, ne
	.endif

	.if \subtract
	sub x14, x12, x14
	.else
	add x14, x12, x14
	.endif

	eor x15, x14, x12
	lsr x15, x15, #56
	cmp x15, #0
	cset w15, ne
	eor x16, x12, x12, lsr #1
	ubfx x16, x16, #54, #1
	orr w15, w15, w16
	ldrb w16, [_cpu, #UML_GADGET_CPU_CPA_ENABLED]
	and w15, w15, w16
	ldrb w16, [_cpu, #UML_GADGET_CPU_CPA_MULTIPLY_ENABLED]
	and w6, w6, w16
	orr w15, w15, w6
	tst w15, w15
	mov x17, x14
	lsr x16, x12, #55
	bfi x17, x16, #55, #9
	eor x16, x16, #1
	bfi x17, x16, #54, #1
	csel x14, x17, x14, ne

	and w9, w8, #0x1f
	add x17, _cpu, #UML_GADGET_CPU_X0
	str x14, [x17, x9, lsl #3]
	next_gadget
	.endif
.endm

.macro cpa_multiply_modes opname, subtract
cpa_multiply \opname, \subtract, 0, 0, 0, 0, rrrr
cpa_multiply \opname, \subtract, 0, 0, 0, 1, rrrz
cpa_multiply \opname, \subtract, 0, 0, 1, 0, rrzr
cpa_multiply \opname, \subtract, 0, 0, 1, 1, rrzz
cpa_multiply \opname, \subtract, 0, 1, 0, 0, rzrr
cpa_multiply \opname, \subtract, 0, 1, 0, 1, rzrz
cpa_multiply \opname, \subtract, 0, 1, 1, 0, rzzr
cpa_multiply \opname, \subtract, 0, 1, 1, 1, rzzz
cpa_multiply \opname, \subtract, 1, 0, 0, 0, zrrr
cpa_multiply \opname, \subtract, 1, 0, 0, 1, zrrz
cpa_multiply \opname, \subtract, 1, 0, 1, 0, zrzr
cpa_multiply \opname, \subtract, 1, 0, 1, 1, zrzz
cpa_multiply \opname, \subtract, 1, 1, 0, 0, zzrr
cpa_multiply \opname, \subtract, 1, 1, 0, 1, zzrz
cpa_multiply \opname, \subtract, 1, 1, 1, 0, zzzr
cpa_multiply \opname, \subtract, 1, 1, 1, 1, zzzz
.endm

cpa_multiply_modes maddpt, 0
cpa_multiply_modes msubpt, 1

/* Production multiply family. The validated operation and every XZR choice
 * are translation-selected; discarded results are removed by Builder. Stream:
 * destination, left, right, addend CpuState byte offsets in 16-bit fields. */
.macro multiply_add_fast operation, left_zero, right_zero, addend_zero, suffix, forward=0
	.set .Lsd\@, scalar_offset0
	.set .Lsn\@, scalar_offset16
	.if \left_zero || \right_zero
	.set .Lsn\@, scalar_none
	.endif
	.set .Lsm\@, scalar_offset32
	.if \left_zero || \right_zero
	.set .Lsm\@, scalar_none
	.endif
	.set .Lsa\@, scalar_offset48
	.if \operation >= 8 || \addend_zero
	.set .Lsa\@, scalar_none
	.endif
scalar_entry multiply_add_\suffix, \forward, 5, .Lsd\@, .Lsn\@, .Lsm\@, .Lsa\@, 1
	ldr x8, [_stream], #8
	.if \left_zero || \right_zero
		/* A zero multiplicand makes the product zero. */
		.if \operation >= 8 || \addend_zero
	mov _scalar_x7, xzr
		.else
	scalar_load \forward, 4, _scalar_x7, 12, 8, 48, 16, 0, _cpu
			/* W-form writes zero-extend the copied addend. */
			.if \operation == 0 || \operation == 1
	mov _scalar_w7, _scalar_w7
			.endif
		.endif
	.else
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _cpu
	scalar_load \forward, 2, x16, 11, 8, 32, 16, 0, _cpu
		.if \operation < 8 && !\addend_zero
	scalar_load \forward, 4, x17, 12, 8, 48, 16, 0, _cpu
		.endif
		.if \operation == 0
			.if \addend_zero
	mul _scalar_w7, w15, w16
			.else
	madd _scalar_w7, w15, w16, w17
			.endif
		.elseif \operation == 1
			.if \addend_zero
	mneg _scalar_w7, w15, w16
			.else
	msub _scalar_w7, w15, w16, w17
			.endif
		.elseif \operation == 2
			.if \addend_zero
	mul _scalar_x7, x15, x16
			.else
	madd _scalar_x7, x15, x16, x17
			.endif
		.elseif \operation == 3
			.if \addend_zero
	mneg _scalar_x7, x15, x16
			.else
	msub _scalar_x7, x15, x16, x17
			.endif
		.elseif \operation == 4
			.if \addend_zero
	smull _scalar_x7, w15, w16
			.else
	smaddl _scalar_x7, w15, w16, x17
			.endif
		.elseif \operation == 5
			.if \addend_zero
	smnegl _scalar_x7, w15, w16
			.else
	smsubl _scalar_x7, w15, w16, x17
			.endif
		.elseif \operation == 6
			.if \addend_zero
	umull _scalar_x7, w15, w16
			.else
	umaddl _scalar_x7, w15, w16, x17
			.endif
		.elseif \operation == 7
			.if \addend_zero
	umnegl _scalar_x7, w15, w16
			.else
	umsubl _scalar_x7, w15, w16, x17
			.endif
		.elseif \operation == 8
	smulh _scalar_x7, x15, x16
		.else
	umulh _scalar_x7, x15, x16
		.endif
	.endif
	and x9, x8, #0xffff
	str _scalar_x7, [_cpu, x9]
	next_gadget
	scalar_variants \forward, .Lsn\@, .Lsm\@, .Lsa\@, multiply_add_fast, \operation, \left_zero, \right_zero, \addend_zero, \suffix
.endm
.macro multiply_result operation, left_zero, right_zero, addend_zero, prefix
multiply_add_fast \operation, \left_zero, \right_zero, \addend_zero, \prefix\()_r
.endm
.macro multiply_addends operation, left_zero, right_zero, prefix
multiply_result \operation, \left_zero, \right_zero, 0, \prefix\()_r
multiply_result \operation, \left_zero, \right_zero, 1, \prefix\()_z
.endm
.macro multiply_sources operation, prefix
multiply_addends \operation, 0, 0, \prefix\()_p
multiply_addends \operation, 1, 0, \prefix\()_z
.endm

multiply_sources 0, w_madd
multiply_sources 1, w_msub
multiply_sources 2, x_madd
multiply_sources 3, x_msub
multiply_sources 4, smaddl
multiply_sources 5, smsubl
multiply_sources 6, umaddl
multiply_sources 7, umsubl
.macro multiply_high_sources operation, prefix
multiply_result \operation, 0, 0, 1, \prefix\()_p_z
multiply_result \operation, 1, 0, 1, \prefix\()_z_z
.endm
multiply_high_sources 8, smulh
multiply_high_sources 9, umulh

.purgem multiply_high_sources
.purgem multiply_sources
.purgem multiply_addends
.purgem multiply_result
.purgem multiply_add_fast

/* Translation-selected LSLV/LSRV/ASRV/RORV. Discarded destinations are
 * folded to NOP, so every remaining entry stores a result. */
.macro variable_shift_fast operation, bits64, left_zero, right_zero, same_source, suffix, forward=0
	.set .Lsd\@, scalar_offset0
	.set .Lsn\@, scalar_offset16
	.if \left_zero
	.set .Lsn\@, scalar_none
	.endif
	.set .Lsm\@, scalar_offset32
	.if \right_zero || \same_source
	.set .Lsm\@, scalar_none
	.endif
scalar_entry variable_shift_\suffix, \forward, 6, .Lsd\@, .Lsn\@, .Lsm\@, scalar_none, 1
	ldr x8, [_stream], #8
	.if \left_zero
	mov x15, xzr
	.else
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _cpu
	.endif
	.if \right_zero
	mov x16, xzr
	.elseif \same_source
	mov x16, x15
	.else
	scalar_load \forward, 2, x16, 11, 8, 32, 16, 0, _cpu
	.endif
	.if \bits64
		.if \operation == 0
	lsl _scalar_x7, x15, x16
		.elseif \operation == 1
	lsr _scalar_x7, x15, x16
		.elseif \operation == 2
	asr _scalar_x7, x15, x16
		.else
	ror _scalar_x7, x15, x16
		.endif
	.else
		.if \operation == 0
	lsl _scalar_w7, w15, w16
		.elseif \operation == 1
	lsr _scalar_w7, w15, w16
		.elseif \operation == 2
	asr _scalar_w7, w15, w16
		.else
	ror _scalar_w7, w15, w16
		.endif
	.endif
	uxth w9, w8
	str _scalar_x7, [_cpu, x9]
	next_gadget
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, variable_shift_fast, \operation, \bits64, \left_zero, \right_zero, \same_source, \suffix
.endm

.macro variable_shift_sources operation, bits64, prefix
variable_shift_fast \operation, \bits64, 0, 0, 0, \prefix\()_rr_r
variable_shift_fast \operation, \bits64, 0, 0, 1, \prefix\()_eq_r
variable_shift_fast \operation, \bits64, 0, 1, 0, \prefix\()_rz_r
variable_shift_fast \operation, \bits64, 1, 0, 0, \prefix\()_zr_r
variable_shift_fast \operation, \bits64, 1, 1, 0, \prefix\()_zz_r
.endm

variable_shift_sources 0, 0, w_lsl
variable_shift_sources 1, 0, w_lsr
variable_shift_sources 2, 0, w_asr
variable_shift_sources 3, 0, w_ror
variable_shift_sources 0, 1, x_lsl
variable_shift_sources 1, 1, x_lsr
variable_shift_sources 2, 1, x_asr
variable_shift_sources 3, 1, x_ror

.purgem variable_shift_sources
.purgem variable_shift_fast

/* Translation-selected UDIV/SDIV. Host A64 already implements the guest's
 * zero-divisor and signed-overflow results without a conditional branch.
 * Discarded destinations are folded to NOP. */
.macro divide_fast signed, bits64, left_zero, right_zero, same_source, suffix, forward=0
	.set .Lsd\@, scalar_offset0
	.set .Lsn\@, scalar_offset16
	.if \left_zero
	.set .Lsn\@, scalar_none
	.endif
	.set .Lsm\@, scalar_offset32
	.if \right_zero || \same_source
	.set .Lsm\@, scalar_none
	.endif
scalar_entry divide_\suffix, \forward, 7, .Lsd\@, .Lsn\@, .Lsm\@, scalar_none, 1
	ldr x8, [_stream], #8
	.if \left_zero
	mov x15, xzr
	.else
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _cpu
	.endif
	.if \right_zero
	mov x16, xzr
	.elseif \same_source
	mov x16, x15
	.else
	scalar_load \forward, 2, x16, 11, 8, 32, 16, 0, _cpu
	.endif
	.if \bits64
		.if \signed
	sdiv _scalar_x7, x15, x16
		.else
	udiv _scalar_x7, x15, x16
		.endif
	.else
		.if \signed
	sdiv _scalar_w7, w15, w16
		.else
	udiv _scalar_w7, w15, w16
		.endif
	.endif
	uxth w9, w8
	str _scalar_x7, [_cpu, x9]
	next_gadget
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, divide_fast, \signed, \bits64, \left_zero, \right_zero, \same_source, \suffix
.endm
.macro divide_sources signed, bits64, prefix
divide_fast \signed, \bits64, 0, 0, 0, \prefix\()_rr_r
divide_fast \signed, \bits64, 0, 0, 1, \prefix\()_eq_r
divide_fast \signed, \bits64, 0, 1, 0, \prefix\()_rz_r
divide_fast \signed, \bits64, 1, 0, 0, \prefix\()_zr_r
divide_fast \signed, \bits64, 1, 1, 0, \prefix\()_zz_r
.endm

divide_sources 0, 0, w_u
divide_sources 1, 0, w_s
divide_sources 0, 1, x_u
divide_sources 1, 1, x_s

.purgem divide_sources
.purgem divide_fast

/* Translation-selected checksum operations. Operation, source mode, and
 * direct CpuState byte offsets are all fixed before execution. Native
 * extension bodies remain the shortest supported-host path; software entries
 * share one fully unrolled body per operation for extension-less hosts. */
.macro crc32_load_sources source_mode
	ldr x11, [_stream], #8
	and x10, x11, #0xffff
	.ifc \source_mode, rr
	ubfx x8, x11, #16, #16
	lsr x9, x11, #32
	ldr x14, [_cpu, x8]
	ldr x15, [_cpu, x9]
	.else
	.ifc \source_mode, same
	ubfx x8, x11, #16, #16
	ldr x14, [_cpu, x8]
	mov x15, x14
	.else
	.ifc \source_mode, rz
	ubfx x8, x11, #16, #16
	ldr x14, [_cpu, x8]
	mov x15, xzr
	.else
	.ifc \source_mode, zr
	lsr x9, x11, #32
	mov w14, wzr
	ldr x15, [_cpu, x9]
	.else
	mov w14, wzr
	mov x15, xzr
	.endif
	.endif
	.endif
	.endif
.endm

.macro crc32_native operation, source_mode, suffix
gadget crc32_\suffix
	crc32_load_sources \source_mode
	.if \operation == 0
	crc32b w16, w14, w15
	.elseif \operation == 1
	crc32h w16, w14, w15
	.elseif \operation == 2
	crc32w w16, w14, w15
	.elseif \operation == 3
	crc32x w16, w14, x15
	.elseif \operation == 4
	crc32cb w16, w14, w15
	.elseif \operation == 5
	crc32ch w16, w14, w15
	.elseif \operation == 6
	crc32cw w16, w14, w15
	.else
	crc32cx w16, w14, x15
	.endif
	str x16, [_cpu, x10]
	next_gadget
.endm

.macro crc32_software_step
	sbfx w17, w14, #0, #1
	and w17, w17, w13
	eor w14, w17, w14, lsr #1
.endm

.macro crc32_software_byte
	uxtb w17, w15
	eor w14, w14, w17
	.rept 8
	crc32_software_step
	.endr
.endm

.macro crc32_software_entry source_mode, suffix
gadget crc32_software_\suffix
	crc32_load_sources \source_mode
	b 91f
.endm

.macro crc32_sources operation, prefix, bytes, polynomial_low, polynomial_high
crc32_native \operation, rr, \prefix\()_rr
crc32_native \operation, same, \prefix\()_same
crc32_native \operation, rz, \prefix\()_rz
crc32_native \operation, zr, \prefix\()_zr
crc32_native \operation, zz, \prefix\()_zz
crc32_software_entry rr, \prefix\()_rr
crc32_software_entry same, \prefix\()_same
crc32_software_entry rz, \prefix\()_rz
crc32_software_entry zr, \prefix\()_zr
crc32_software_entry zz, \prefix\()_zz
91:
	movz w13, #\polynomial_low
	movk w13, #\polynomial_high, lsl #16
	crc32_software_byte
	.if \bytes > 1
	lsr x15, x15, #8
	crc32_software_byte
	.endif
	.if \bytes > 2
	lsr x15, x15, #8
	crc32_software_byte
	lsr x15, x15, #8
	crc32_software_byte
	.endif
	.if \bytes > 4
	lsr x15, x15, #8
	crc32_software_byte
	lsr x15, x15, #8
	crc32_software_byte
	lsr x15, x15, #8
	crc32_software_byte
	lsr x15, x15, #8
	crc32_software_byte
	.endif
	str x14, [_cpu, x10]
	next_gadget
.endm

crc32_sources 0, b, 1, 0x8320, 0xedb8
crc32_sources 1, h, 2, 0x8320, 0xedb8
crc32_sources 2, w, 4, 0x8320, 0xedb8
crc32_sources 3, x, 8, 0x8320, 0xedb8
crc32_sources 4, cb, 1, 0x3b78, 0x82f6
crc32_sources 5, ch, 2, 0x3b78, 0x82f6
crc32_sources 6, cw, 4, 0x3b78, 0x82f6
crc32_sources 7, cx, 8, 0x3b78, 0x82f6

.purgem crc32_sources
.purgem crc32_software_entry
.purgem crc32_software_byte
.purgem crc32_software_step
.purgem crc32_native
.purgem crc32_load_sources

/* Translation-selected EXTR/ROR. Width, zero shift, and all XZR choices are
 * fixed. Stream operand: Rd, Rn, Rm, shift in successive bytes. */
.macro extract_register_fast bits64, shift_nonzero, left_zero, right_zero, destination_zero, suffix, forward=0
	.set .Lsd\@, scalar_register0
	.if \destination_zero
	.set .Lsd\@, scalar_none
	.endif
	.set .Lsn\@, scalar_register8
	.if \left_zero
	.set .Lsn\@, scalar_none
	.endif
	.set .Lsm\@, scalar_register16
	.if \right_zero
	.set .Lsm\@, scalar_none
	.endif
scalar_entry extract_register_\suffix, \forward, 8, .Lsd\@, .Lsn\@, .Lsm\@, scalar_none, 1
	ldr x8, [_stream], #8
	add x13, _cpu, #UML_GADGET_CPU_X0
	.if \left_zero
	mov x14, xzr
	.else
	scalar_load \forward, 1, x14, 10, 8, 8, 5, 3, x13
	.endif
	.if \right_zero
	mov x15, xzr
	.else
	scalar_load \forward, 2, x15, 11, 8, 16, 5, 3, x13
	.endif
	.if \shift_nonzero
	ubfx w12, w8, #24, #6
	/* LSL takes its amount modulo the register width and this variant
	 * guarantees a shift of 1..width-1, so the complement is just the
	 * negation: (2^32 - shift) mod width == width - shift for both widths,
	 * since 2^32 is a multiple of 64. One instruction instead of two, and
	 * the chain no longer waits on a constant materialisation. */
		.if \bits64
	lsr _scalar_x16, x15, x12
	neg w17, w12
	lsl x17, x14, x17
	orr _scalar_x16, _scalar_x16, x17
		.else
	lsr _scalar_w16, w15, w12
	neg w17, w12
	lsl w17, w14, w17
	orr _scalar_w16, _scalar_w16, w17
		.endif
	.else
		.if \bits64
	mov _scalar_x16, x15
		.else
	mov _scalar_w16, w15
		.endif
	.endif
	.if !\destination_zero
	and w9, w8, #0x1f
	str _scalar_x16, [x13, x9, lsl #3]
	.endif
	next_gadget
	/* Hold this entry's length, and so every later entry's address and
	 * 64-byte phase, at what it was before the neg. See move_wide_keep. */
	.if \shift_nonzero
	.space 4
	.endif
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, extract_register_fast, \bits64, \shift_nonzero, \left_zero, \right_zero, \destination_zero, \suffix
.endm
.macro extract_destinations bits64, shift_nonzero, left_zero, right_zero, prefix
extract_register_fast \bits64, \shift_nonzero, \left_zero, \right_zero, 0, \prefix\()_r
extract_register_fast \bits64, \shift_nonzero, \left_zero, \right_zero, 1, \prefix\()_z
.endm
.macro extract_sources bits64, shift_nonzero, prefix
extract_destinations \bits64, \shift_nonzero, 0, 0, \prefix\()_rr
extract_destinations \bits64, \shift_nonzero, 0, 1, \prefix\()_rz
extract_destinations \bits64, \shift_nonzero, 1, 0, \prefix\()_zr
extract_destinations \bits64, \shift_nonzero, 1, 1, \prefix\()_zz
.endm

extract_sources 0, 0, w_n
extract_sources 0, 1, w_s
extract_sources 1, 0, x_n
extract_sources 1, 1, x_s

.purgem extract_sources
.purgem extract_destinations
.purgem extract_register_fast

/* Production CSEL/CSINC/CSINV/CSNEG use the native conditional-select family
 * directly. Builder maps XZR sources to CpuState::gadget_zero and discards an
 * XZR destination as NOP, so no opcode/register-mode branches remain. */
.macro conditional_select_fast name, condition, operation, bits64, suffix, forward=0
	.set .Lsd\@, scalar_offset0
	.set .Lsn\@, scalar_offset16
	.set .Lsm\@, scalar_offset32
scalar_entry conditional_select_fast_\name\()_\suffix, \forward, 9, .Lsd\@, .Lsn\@, .Lsm\@, scalar_none, 1
	ldr x8, [_stream], #8
	ubfx x9, x8, #0, #16
	scalar_load \forward, 1, x15, 10, 8, 16
	scalar_load \forward, 2, x16, 11, 8, 32
	msr nzcv, x27
	.if \bits64
		.if \operation == 0
	csel _scalar_x7, x15, x16, \condition
		.elseif \operation == 1
	csinc _scalar_x7, x15, x16, \condition
		.elseif \operation == 2
	csinv _scalar_x7, x15, x16, \condition
		.else
	csneg _scalar_x7, x15, x16, \condition
		.endif
	.else
		.if \operation == 0
	csel _scalar_w7, w15, w16, \condition
		.elseif \operation == 1
	csinc _scalar_w7, w15, w16, \condition
		.elseif \operation == 2
	csinv _scalar_w7, w15, w16, \condition
		.else
	csneg _scalar_w7, w15, w16, \condition
		.endif
	.endif
	str _scalar_x7, [_cpu, x9]
	next_gadget
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, conditional_select_fast, \name, \condition, \operation, \bits64, \suffix
.endm
.macro conditional_select_fast_modes name, condition
conditional_select_fast \name, \condition, 0, 0, w_sel
conditional_select_fast \name, \condition, 0, 1, x_sel
conditional_select_fast \name, \condition, 1, 0, w_inc
conditional_select_fast \name, \condition, 1, 1, x_inc
conditional_select_fast \name, \condition, 2, 0, w_inv
conditional_select_fast \name, \condition, 2, 1, x_inv
conditional_select_fast \name, \condition, 3, 0, w_neg
conditional_select_fast \name, \condition, 3, 1, x_neg
.endm

conditional_select_fast_modes eq, eq
conditional_select_fast_modes ne, ne
conditional_select_fast_modes hs, hs
conditional_select_fast_modes lo, lo
conditional_select_fast_modes mi, mi
conditional_select_fast_modes pl, pl
conditional_select_fast_modes vs, vs
conditional_select_fast_modes vc, vc
conditional_select_fast_modes hi, hi
conditional_select_fast_modes ls, ls
conditional_select_fast_modes ge, ge
conditional_select_fast_modes lt, lt
conditional_select_fast_modes gt, gt
conditional_select_fast_modes le, le

/* AL and NV both select the first operand unconditionally.  Translation
 * shares these two compact copy gadgets across all four opcodes, avoiding an
 * NZCV restore, a second source load, and a conditional-select instruction. */
.macro conditional_select_always bits64, suffix, forward=0
	.set .Lsd\@, scalar_offset0
	.set .Lsn\@, scalar_offset16
scalar_entry conditional_select_fast_always_\suffix, \forward, 9, .Lsd\@, .Lsn\@, scalar_none, scalar_none, 1
	ldr x8, [_stream], #8
	ubfx x9, x8, #0, #16
	scalar_load \forward, 1, _scalar_x15, 10, 8, 16, 16, 0, _cpu
	.if \bits64
	str _scalar_x15, [_cpu, x9]
	.else
	mov _scalar_w7, _scalar_w15
	str _scalar_x7, [_cpu, x9]
	.endif
	next_gadget
	scalar_variants \forward, .Lsn\@, scalar_none, scalar_none, conditional_select_always, \bits64, \suffix
.endm
conditional_select_always 0, w
conditional_select_always 1, x

.purgem conditional_select_always
.purgem conditional_select_fast_modes
.purgem conditional_select_fast

/* Production CCMN/CCMP. The sole conditional branch is the architectural
 * condition itself; width, add/subtract, immediate/register and XZR selection
 * are fixed at translation. Stream: left offset, right offset/immediate, and
 * the already-positioned alternative NZCV value. */
.macro conditional_compare_fast name, condition, subtract, bits64, immediate, always_true, suffix, forward=0
	.set .Lsn\@, scalar_offset0
	.set .Lsm\@, scalar_offset16
	.if \immediate
	.set .Lsm\@, scalar_none
	.endif
scalar_entry conditional_compare_fast_\name\()_\suffix, \forward, 10, scalar_none, .Lsn\@, .Lsm\@, scalar_none, 1
	ldr x8, [_stream], #8
	.if \always_true == 0
	msr nzcv, x27
	b.\condition .Lconditional_compare_true\@
	lsr x27, x8, #32
	next_gadget
.Lconditional_compare_true\@:
	.endif
	scalar_load \forward, 1, x12, 10, 8, 0, 16, 0, _cpu
	.if \immediate
	ubfx x13, x8, #16, #5
	.else
	scalar_load \forward, 2, x13, 11, 8, 16, 16, 0, _cpu
	.endif
	.if \bits64
		.if \subtract
	cmp x12, x13
		.else
	cmn x12, x13
		.endif
	.else
		.if \subtract
	cmp w12, w13
		.else
	cmn w12, w13
		.endif
	.endif
	mrs x27, nzcv
	next_gadget
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, conditional_compare_fast, \name, \condition, \subtract, \bits64, \immediate, \always_true, \suffix
.endm
.macro conditional_compare_fast_modes name, condition, always_true
conditional_compare_fast \name, \condition, 0, 0, 0, \always_true, w_cmn_r
conditional_compare_fast \name, \condition, 0, 0, 1, \always_true, w_cmn_i
conditional_compare_fast \name, \condition, 0, 1, 0, \always_true, x_cmn_r
conditional_compare_fast \name, \condition, 0, 1, 1, \always_true, x_cmn_i
conditional_compare_fast \name, \condition, 1, 0, 0, \always_true, w_cmp_r
conditional_compare_fast \name, \condition, 1, 0, 1, \always_true, w_cmp_i
conditional_compare_fast \name, \condition, 1, 1, 0, \always_true, x_cmp_r
conditional_compare_fast \name, \condition, 1, 1, 1, \always_true, x_cmp_i
.endm

conditional_compare_fast_modes eq, eq, 0
conditional_compare_fast_modes ne, ne, 0
conditional_compare_fast_modes hs, hs, 0
conditional_compare_fast_modes lo, lo, 0
conditional_compare_fast_modes mi, mi, 0
conditional_compare_fast_modes pl, pl, 0
conditional_compare_fast_modes vs, vs, 0
conditional_compare_fast_modes vc, vc, 0
conditional_compare_fast_modes hi, hi, 0
conditional_compare_fast_modes ls, ls, 0
conditional_compare_fast_modes ge, ge, 0
conditional_compare_fast_modes lt, lt, 0
conditional_compare_fast_modes gt, gt, 0
conditional_compare_fast_modes le, le, 0
conditional_compare_fast_modes always, al, 1

.purgem conditional_compare_fast_modes
.purgem conditional_compare_fast

/* Production logical shifted-register entries.  Builder specializes width,
 * operation, inversion, shift kind (including the common no-shift form), and
 * every XZR choice. Stream: Rd, Rn, Rm, shift in successive bytes. */
.macro logical_shifted_fast operation, bits64, invert, shift_kind, left_zero, right_zero, destination_zero, suffix, forward=0
	.set .Lsd\@, scalar_register0
	.if \destination_zero
	.set .Lsd\@, scalar_none
	.endif
	.set .Lsn\@, scalar_register8
	.if \left_zero
	.set .Lsn\@, scalar_none
	.endif
	.set .Lsm\@, scalar_register16
	.if \right_zero
	.set .Lsm\@, scalar_none
	.endif
scalar_entry logical_shifted_\suffix, \forward, 11, .Lsd\@, .Lsn\@, .Lsm\@, scalar_none, 1
	ldr x8, [_stream], #8
	.if \left_zero
		.if \operation != 1 || \invert || \shift_kind != 0 || \destination_zero
	mov x15, xzr
		.endif
	.else
	scalar_load \forward, 1, x15, 10, 8, 8, 5, 3, _cpu
	.endif
	.if \right_zero
	.if \operation == 1 && \bits64 && \left_zero && !\invert && \shift_kind == 0 && !\destination_zero
	mov _scalar_x16, xzr
	.else
	mov x16, xzr
	.endif
	.else
	.if \operation == 1 && \bits64 && \left_zero && !\invert && \shift_kind == 0 && !\destination_zero
	scalar_load \forward, 2, _scalar_x16, 11, 8, 16, 5, 3, _cpu
	.else
	scalar_load \forward, 2, x16, 11, 8, 16, 5, 3, _cpu
	.endif
	.endif
	.if \shift_kind != 0
	ubfx w6, w8, #24, #6
		.if \bits64
			.if \shift_kind == 1
	lsl x16, x16, x6
			.elseif \shift_kind == 2
	lsr x16, x16, x6
			.elseif \shift_kind == 3
	asr x16, x16, x6
			.else
	ror x16, x16, x6
			.endif
		.else
			.if \shift_kind == 1
	lsl w16, w16, w6
			.elseif \shift_kind == 2
	lsr w16, w16, w6
			.elseif \shift_kind == 3
	asr w16, w16, w6
			.else
	ror w16, w16, w6
			.endif
		.endif
	.endif
	.if \invert
	mvn x16, x16
	.endif
	.if \operation == 1 && \left_zero && !\invert && \shift_kind == 0 && !\destination_zero
		.if \bits64
	and w9, w8, #0x1f
	str _scalar_x16, [_cpu, x9, lsl #3]
		.else
	mov _scalar_w7, w16
	and w9, w8, #0x1f
	str _scalar_x7, [_cpu, x9, lsl #3]
		.endif
	.else
		.if \bits64
		.if \operation == 0
	and _scalar_x7, x15, x16
		.elseif \operation == 1
	orr _scalar_x7, x15, x16
		.elseif \operation == 2
	eor _scalar_x7, x15, x16
		.else
	ands _scalar_x7, x15, x16
		.endif
	.else
		.if \operation == 0
	and _scalar_w7, w15, w16
		.elseif \operation == 1
	orr _scalar_w7, w15, w16
		.elseif \operation == 2
	eor _scalar_w7, w15, w16
		.else
	ands _scalar_w7, w15, w16
		.endif
		.endif
		.if \operation == 3
	mrs x27, nzcv
		.endif
		.if !\destination_zero
	and w9, w8, #0x1f
	str _scalar_x7, [_cpu, x9, lsl #3]
		.endif
	.endif
	next_gadget
	/* Preserve the established bank layout after removing the base add. */
	nop
	/* Keep later logical entries at their established cache-line phase. The
	 * native 64-bit MOV alias removed two additional runtime instructions. */
	.if \operation == 1 && \bits64 && \left_zero && !\invert && \shift_kind == 0 && !\destination_zero
	nop
	nop
	nop
	.endif
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, logical_shifted_fast, \operation, \bits64, \invert, \shift_kind, \left_zero, \right_zero, \destination_zero, \suffix
.endm
.macro logical_shifted_destinations operation, bits64, invert, shift_kind, left_zero, right_zero, prefix
logical_shifted_fast \operation, \bits64, \invert, \shift_kind, \left_zero, \right_zero, 0, \prefix\()_r
	.if \operation == 3
logical_shifted_fast \operation, \bits64, \invert, \shift_kind, \left_zero, \right_zero, 1, \prefix\()_z
	.endif
.endm
.macro logical_shifted_sources operation, bits64, invert, shift_kind, prefix
logical_shifted_destinations \operation, \bits64, \invert, \shift_kind, 0, 0, \prefix\()_rr
logical_shifted_destinations \operation, \bits64, \invert, \shift_kind, 0, 1, \prefix\()_rz
logical_shifted_destinations \operation, \bits64, \invert, \shift_kind, 1, 0, \prefix\()_zr
logical_shifted_destinations \operation, \bits64, \invert, \shift_kind, 1, 1, \prefix\()_zz
.endm
.macro logical_shifted_shifts operation, bits64, invert, prefix
logical_shifted_sources \operation, \bits64, \invert, 0, \prefix\()_n
logical_shifted_sources \operation, \bits64, \invert, 1, \prefix\()_l
logical_shifted_sources \operation, \bits64, \invert, 2, \prefix\()_r
logical_shifted_sources \operation, \bits64, \invert, 3, \prefix\()_a
logical_shifted_sources \operation, \bits64, \invert, 4, \prefix\()_o
.endm

logical_shifted_shifts 0, 0, 0, w_and_p
logical_shifted_shifts 0, 0, 1, w_and_n
logical_shifted_shifts 1, 0, 0, w_orr_p
logical_shifted_shifts 1, 0, 1, w_orr_n
logical_shifted_shifts 2, 0, 0, w_eor_p
logical_shifted_shifts 2, 0, 1, w_eor_n
logical_shifted_shifts 3, 0, 0, w_ands_p
logical_shifted_shifts 3, 0, 1, w_ands_n
logical_shifted_shifts 0, 1, 0, x_and_p
logical_shifted_shifts 0, 1, 1, x_and_n
logical_shifted_shifts 1, 1, 0, x_orr_p
logical_shifted_shifts 1, 1, 1, x_orr_n
logical_shifted_shifts 2, 1, 0, x_eor_p
logical_shifted_shifts 2, 1, 1, x_eor_n
logical_shifted_shifts 3, 1, 0, x_ands_p
logical_shifted_shifts 3, 1, 1, x_ands_n

.purgem logical_shifted_shifts
.purgem logical_shifted_sources
.purgem logical_shifted_destinations
.purgem logical_shifted_fast

/* Production logical-immediate entries.  The immediate is decoded once by
 * Builder; width, operation, XZR/SP meanings and flag update are entry-point
 * constants. Stream: [Rd[4:0], Rn[12:8]][decoded immediate]. */
.macro logical_immediate_fast operation, bits64, source_zero, destination_special, suffix, forward=0
	.set .Lsd\@, scalar_register0
	.if \destination_special && \operation != 3
	.set .Lsd\@, scalar_sp
	.endif
	.if \destination_special && \operation == 3
	.set .Lsd\@, scalar_none
	.endif
	.set .Lsn\@, scalar_register8
	.if \source_zero
	.set .Lsn\@, scalar_none
	.endif
scalar_entry logical_immediate_\suffix, \forward, 12, .Lsd\@, .Lsn\@, scalar_none, scalar_none, 2
	ldr x8, [_stream], #8
	ldr x14, [_stream], #8
	.if \source_zero
		.if \operation == 0
	mov _scalar_x16, xzr
		.elseif \operation == 1 || \operation == 2
	mov _scalar_x16, x14
		.else
	mov _scalar_x16, xzr
	mov x27, #0x40000000
		.endif
	.else
	scalar_load \forward, 1, x15, 10, 8, 8, 5, 3, _cpu
		.if \bits64
			.if \operation == 0
	and _scalar_x16, x15, x14
			.elseif \operation == 1
	orr _scalar_x16, x15, x14
			.elseif \operation == 2
	eor _scalar_x16, x15, x14
			.else
	ands _scalar_x16, x15, x14
			.endif
		.else
			.if \operation == 0
	and _scalar_w16, w15, w14
			.elseif \operation == 1
	orr _scalar_w16, w15, w14
			.elseif \operation == 2
	eor _scalar_w16, w15, w14
			.else
	ands _scalar_w16, w15, w14
			.endif
		.endif
		.if \operation == 3
	mrs x27, nzcv
		.endif
	.endif
	.if \destination_special
		.if \operation != 3
	str _scalar_x16, [_cpu, #UML_GADGET_CPU_SP]
		.endif
	.else
	and w9, w8, #0x1f
	str _scalar_x16, [_cpu, x9, lsl #3]
	.endif
	next_gadget
	scalar_variants \forward, .Lsn\@, scalar_none, scalar_none, logical_immediate_fast, \operation, \bits64, \source_zero, \destination_special, \suffix
.endm
.macro logical_immediate_modes operation, bits64, prefix
logical_immediate_fast \operation, \bits64, 0, 0, \prefix\()_rr
logical_immediate_fast \operation, \bits64, 0, 1, \prefix\()_rs
logical_immediate_fast \operation, \bits64, 1, 0, \prefix\()_zr
logical_immediate_fast \operation, \bits64, 1, 1, \prefix\()_zs
.endm

logical_immediate_modes 0, 0, w_and
logical_immediate_modes 1, 0, w_orr
logical_immediate_modes 2, 0, w_eor
logical_immediate_modes 3, 0, w_ands
logical_immediate_modes 0, 1, x_and
logical_immediate_modes 1, 1, x_orr
logical_immediate_modes 2, 1, x_eor
logical_immediate_modes 3, 1, x_ands

.purgem logical_immediate_modes
.purgem logical_immediate_fast

/* Production SBFM/BFM/UBFM. Builder has already decoded both masks, folds a
 * discarded Rd to NOP, and selects width/operation/XZR/zero-rotate. Stream:
 * [destination/source CpuState offsets, rotate, imms][write mask][test mask]. */
.macro bitfield_fast operation, bits64, rotate_nonzero, source_zero, suffix, forward=0
	.set .Lsd\@, scalar_offset0
	.set .Lsn\@, scalar_offset16
	.if \source_zero
	.set .Lsn\@, scalar_none
	.endif
	.set .Lsm\@, scalar_offset0
	.if \operation != 1
	.set .Lsm\@, scalar_none
	.endif
scalar_entry bitfield_\suffix, \forward, 13, .Lsd\@, .Lsn\@, .Lsm\@, scalar_none, 3
	ldr x8, [_stream], #8
	ldr x14, [_stream], #8
	ldr x17, [_stream], #8
	.if \source_zero
	mov x15, xzr
	.else
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _cpu
	.endif
	.if \operation == 1
	/* Capture the old destination before the result overwrites x0. */
	scalar_load \forward, 2, x6, 9, 8, 0
	.endif
	.if !\bits64
	mov w15, w15
	.endif
	.if \rotate_nonzero
	ubfx x12, x8, #32, #6
		.if \bits64
	ror _scalar_x16, x15, x12
		.else
	ror _scalar_w16, w15, w12
		.endif
	.else
	mov _scalar_x16, x15
	.endif
	and _scalar_x16, _scalar_x16, x14
	.if \operation == 0
	ubfx x11, x8, #40, #6
	lsr x6, x15, x11
	tst x6, #1
	csetm x6, ne
	.elseif \operation == 1
	bic x7, x6, x14
	orr _scalar_x16, _scalar_x16, x7
	.else
	mov x6, xzr
	.endif
	bic x6, x6, x17
	and _scalar_x16, _scalar_x16, x17
	orr _scalar_x16, _scalar_x16, x6
	.if !\bits64
	mov _scalar_w16, _scalar_w16
	.endif
	and x9, x8, #0xffff
	str _scalar_x16, [_cpu, x9]
	next_gadget
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, bitfield_fast, \operation, \bits64, \rotate_nonzero, \source_zero, \suffix
.endm
.macro bitfield_sources operation, bits64, prefix
bitfield_fast \operation, \bits64, 0, 0, \prefix\()_n_r
bitfield_fast \operation, \bits64, 0, 1, \prefix\()_n_z
bitfield_fast \operation, \bits64, 1, 0, \prefix\()_r_r
bitfield_fast \operation, \bits64, 1, 1, \prefix\()_r_z
.endm

bitfield_sources 0, 0, w_s
bitfield_sources 1, 0, w_b
bitfield_sources 2, 0, w_u
bitfield_sources 0, 1, x_s
bitfield_sources 1, 1, x_b
bitfield_sources 2, 1, x_u

.purgem bitfield_sources
.purgem bitfield_fast

/* Translation-selected data-processing (1 source) operations. Source and
 * destination are direct CpuState byte offsets. The optional native forms are
 * selected only when the running host advertises the matching extension. */
.macro bit_reverse_count_fast operation, bits64, native, suffix, forward=0
	.set .Lsd\@, scalar_offset16
	.set .Lsn\@, scalar_offset0
scalar_entry bit_reverse_count_\suffix, \forward, 14, .Lsd\@, .Lsn\@, scalar_none, scalar_none, 1
	ldr x9, [_stream], #8
	lsr x11, x9, #16
	scalar_load \forward, 1, x12, 10, 9, 0
	.if \native
		.if \bits64
			.if \operation == 6
	.inst 0xdac01980 /* ctz x0, x12 */
			.elseif \operation == 7
	.inst 0xdac01d80 /* cnt x0, x12 */
			.else
	.inst 0xdac02180 /* abs x0, x12 */
			.endif
		.else
			.if \operation == 6
	.inst 0x5ac01980 /* ctz w0, w12 */
			.elseif \operation == 7
	.inst 0x5ac01d80 /* cnt w0, w12 */
			.else
	.inst 0x5ac02180 /* abs w0, w12 */
			.endif
		.endif
	.elseif \bits64
		.if \operation == 0
	rbit _scalar_x13, x12
		.elseif \operation == 1
	rev16 _scalar_x13, x12
		.elseif \operation == 2
	rev32 _scalar_x13, x12
		.elseif \operation == 3
	rev _scalar_x13, x12
		.elseif \operation == 4
	clz _scalar_x13, x12
		.elseif \operation == 5
	cls _scalar_x13, x12
		.elseif \operation == 6
	rbit _scalar_x13, x12
	clz _scalar_x13, _scalar_x13
		.elseif \operation == 7
	fmov d0, x12
	cnt v0.8b, v0.8b
	addv b0, v0.8b
	umov _scalar_w13, v0.b[0]
		.else
	cmp x12, #0
	cneg _scalar_x13, x12, mi
		.endif
	.else
		.if \operation == 0
	rbit _scalar_w13, w12
		.elseif \operation == 1
	rev16 _scalar_w13, w12
		.elseif \operation == 2
	rev _scalar_w13, w12
		.elseif \operation == 4
	clz _scalar_w13, w12
		.elseif \operation == 5
	cls _scalar_w13, w12
		.elseif \operation == 6
	rbit _scalar_w13, w12
	clz _scalar_w13, _scalar_w13
		.elseif \operation == 7
	mov w12, w12
	fmov d0, x12
	cnt v0.8b, v0.8b
	addv b0, v0.8b
	umov _scalar_w13, v0.b[0]
		.else
	cmp w12, #0
	cneg _scalar_w13, w12, mi
		.endif
	.endif
	str _scalar_x13, [_cpu, x11]
	next_gadget
	scalar_variants \forward, .Lsn\@, scalar_none, scalar_none, bit_reverse_count_fast, \operation, \bits64, \native, \suffix
.endm

bit_reverse_count_fast 0, 0, 0, w_rbit
bit_reverse_count_fast 1, 0, 0, w_rev16
bit_reverse_count_fast 2, 0, 0, w_rev
bit_reverse_count_fast 4, 0, 0, w_clz
bit_reverse_count_fast 5, 0, 0, w_cls
bit_reverse_count_fast 6, 0, 0, w_ctz
bit_reverse_count_fast 7, 0, 0, w_cnt
bit_reverse_count_fast 8, 0, 0, w_abs
bit_reverse_count_fast 0, 1, 0, x_rbit
bit_reverse_count_fast 1, 1, 0, x_rev16
bit_reverse_count_fast 2, 1, 0, x_rev32
bit_reverse_count_fast 3, 1, 0, x_rev
bit_reverse_count_fast 4, 1, 0, x_clz
bit_reverse_count_fast 5, 1, 0, x_cls
bit_reverse_count_fast 6, 1, 0, x_ctz
bit_reverse_count_fast 7, 1, 0, x_cnt
bit_reverse_count_fast 8, 1, 0, x_abs
bit_reverse_count_fast 6, 0, 1, w_ctz_native
bit_reverse_count_fast 7, 0, 1, w_cnt_native
bit_reverse_count_fast 8, 0, 1, w_abs_native
bit_reverse_count_fast 6, 1, 1, x_ctz_native
bit_reverse_count_fast 7, 1, 1, x_cnt_native
bit_reverse_count_fast 8, 1, 1, x_abs_native

.purgem bit_reverse_count_fast

/* Translation resolves instruction/data and XZR cases. */
.macro xpac_register name, operation
gadget \name
	ldr x19, [_stream], #8
	ldr x9, [_cpu, x19]
	software_pointer_auth x9, xzr, \operation
	str x9, [_cpu, x19]
	next_gadget
.endm

xpac_register xpaci, 9
xpac_register xpacd, 10

.purgem xpac_register

gadget xpaclri
	ldr x30, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	software_pointer_auth x30, xzr, 9
	str x30, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	next_gadget

/* Classic FEAT_PAuth operations. Packed operands are direct CpuState byte
 * offsets: destination in bits[15:0], modifier in bits[31:16]. The builder
 * selects modifier-zero forms separately, avoiding an execution-time test. */
.macro pauth_operation name, operation
gadget pauth_\name\()_r
	ldr x19, [_stream], #8
	and w10, w19, #0xffff
	ubfx w11, w19, #16, #16
	ldr x9, [_cpu, x10]
	ldr x12, [_cpu, x11]
	software_pointer_auth x9, x12, \operation
	and w10, w19, #0xffff
	str x9, [_cpu, x10]
	next_gadget
gadget pauth_\name\()_z
	ldr x19, [_stream], #8
	ldr x9, [_cpu, x19]
	software_pointer_auth x9, xzr, \operation
	str x9, [_cpu, x19]
	next_gadget
.endm

pauth_operation pacia, 0
pauth_operation pacib, 1
pauth_operation pacda, 2
pauth_operation pacdb, 3
pauth_operation autia, 4
pauth_operation autib, 5
pauth_operation autda, 6
pauth_operation autdb, 7

.purgem pauth_operation

/* PACGA writes a fresh 64-bit result, so the destination need not be loaded.
 * The builder separately selects the XZR first-source form and packs the
 * remaining direct CpuState offsets into one stream word. */
gadget pauth_pacga_r
	ldr x19, [_stream], #8
	ubfx w11, w19, #16, #16
	ubfx x12, x19, #32, #16
	ldr x9, [_cpu, x11]
	ldr x13, [_cpu, x12]
	software_pointer_auth x9, x13, 8
	and w10, w19, #0xffff
	str x9, [_cpu, x10]
	next_gadget

gadget pauth_pacga_z
	ldr x19, [_stream], #8
	ubfx x12, x19, #32, #16
	ldr x13, [_cpu, x12]
	mov x9, xzr
	software_pointer_auth x9, x13, 8
	and w10, w19, #0xffff
	str x9, [_cpu, x10]
	next_gadget

/* Production FEAT_CSSC min/max. Width, operation and operand form are fixed;
 * source fields are translation-resolved CpuState offsets. For immediates the
 * upper stream word already contains the correctly extended comparison value. */
.macro integer_min_max_fast name, condition, is_signed, bits64, immediate, suffix, forward=0
	.set .Lsd\@, scalar_offset0
	.set .Lsn\@, scalar_offset16
	.set .Lsm\@, scalar_offset32
	.if \immediate
	.set .Lsm\@, scalar_none
	.endif
scalar_entry integer_\name\()_fast_\suffix, \forward, 15, .Lsd\@, .Lsn\@, .Lsm\@, scalar_none, 1
	ldr x8, [_stream], #8
	ubfx x9, x8, #0, #16
	scalar_load \forward, 1, x12, 10, 8, 16, 16, 0, _cpu
	.if \immediate
		.if \is_signed
	asr x13, x8, #32
		.else
	lsr x13, x8, #32
		.endif
	.else
	scalar_load \forward, 2, x13, 11, 8, 32, 16, 0, _cpu
	.endif
	.if \bits64
	cmp x12, x13
	csel _scalar_x14, x12, x13, \condition
	.else
	cmp w12, w13
	csel _scalar_w14, w12, w13, \condition
	.endif
	str _scalar_x14, [_cpu, x9]
	next_gadget
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, integer_min_max_fast, \name, \condition, \is_signed, \bits64, \immediate, \suffix
.endm
.macro integer_min_max_fast_modes name, condition, is_signed
integer_min_max_fast \name, \condition, \is_signed, 0, 0, w_r
integer_min_max_fast \name, \condition, \is_signed, 0, 1, w_i
integer_min_max_fast \name, \condition, \is_signed, 1, 0, x_r
integer_min_max_fast \name, \condition, \is_signed, 1, 1, x_i
.endm

integer_min_max_fast_modes smax, ge, 1
integer_min_max_fast_modes umax, hs, 0
integer_min_max_fast_modes smin, le, 1
integer_min_max_fast_modes umin, ls, 0

.purgem integer_min_max_fast_modes
.purgem integer_min_max_fast

/* Equal inputs and unsigned zero identities are recognized by the builder.
 * These shared copy entries remove the second load, compare and select. */
.macro integer_min_max_copy bits64, suffix, forward=0
	.set .Lsd\@, scalar_offset0
	.set .Lsn\@, scalar_offset16
scalar_entry integer_min_max_copy_\suffix, \forward, 15, .Lsd\@, .Lsn\@, scalar_none, scalar_none, 1
	ldr x8, [_stream], #8
	ubfx x9, x8, #0, #16
	scalar_load \forward, 1, _scalar_x12, 10, 8, 16, 16, 0, _cpu
	.if \bits64 == 0
	mov _scalar_w12, _scalar_w12
	.endif
	str _scalar_x12, [_cpu, x9]
	next_gadget
	scalar_variants \forward, .Lsn\@, scalar_none, scalar_none, integer_min_max_copy, \bits64, \suffix
.endm

integer_min_max_copy 0, w
integer_min_max_copy 1, x

.purgem integer_min_max_copy

gadget cfinv
	eor w27, w27, #0x20000000
	next_gadget

gadget axflag
	mov w8, w27
	ubfx w9, w8, #30, #1
	ubfx w10, w8, #29, #1
	ubfx w11, w8, #28, #1
	orr w9, w9, w11
	bic w10, w10, w11
	lsl w9, w9, #30
	orr w9, w9, w10, lsl #29
	mov w27, w9
	next_gadget

/* The builder selects these one-instruction host forms only after detecting
 * the exact extension.  The software bodies above remain the portable gadget
 * implementation and keep gadget conformance independent of the JIT
 * eligibility decision. */
gadget axflag_native
	msr nzcv, x27
	.inst 0xd500405f
	mrs x27, nzcv
	next_gadget

gadget xaflag
	mov w8, w27
	ubfx w9, w8, #30, #1
	ubfx w10, w8, #29, #1
	orr w11, w9, w10
	eor w12, w11, #1
	and w13, w9, w10
	bic w14, w9, w10
	lsl w12, w12, #31
	orr w12, w12, w13, lsl #30
	orr w12, w12, w11, lsl #29
	orr w12, w12, w14, lsl #28
	mov w27, w12
	next_gadget

gadget xaflag_native
	msr nzcv, x27
	.inst 0xd500403f
	mrs x27, nzcv
	next_gadget

gadget rmif_fast
	ldr x8, [_stream], #8
	and x12, x8, #0xffff
	ubfx x11, x8, #16, #6
	lsr x14, x8, #32
	ldr x12, [_cpu, x12]
	ror x13, x12, x11
	lsl w13, w13, #28
	bic w27, w27, w14
	and w13, w13, w14
	orr w27, w27, w13
	next_gadget

gadget rmif_zero
	ldr w8, [_stream], #8
	bic w27, w27, w8
	next_gadget

.macro setf_fast width, suffix
gadget setf_fast_\suffix
	ldr x8, [_stream], #8
	ldr w12, [_cpu, x8]
	tst w12, #((1 << \width) - 1)
	cset w17, eq
	ubfx w15, w12, #(\width - 1), #1
	eor w16, w12, w12, lsr #1
	ubfx w16, w16, #(\width - 1), #1
	and w18, w27, #0x20000000
	orr w18, w18, w15, lsl #31
	orr w18, w18, w17, lsl #30
	orr w18, w18, w16, lsl #28
	mov w27, w18
	next_gadget
.endm

setf_fast 8, 8
setf_fast 16, 16

.purgem setf_fast

.macro setf_native encoding, suffix
gadget setf_native_\suffix
	ldr x8, [_stream], #8
	ldr w8, [_cpu, x8]
	msr nzcv, x27
	.inst \encoding
	mrs x27, nzcv
	next_gadget
.endm

/* Both encodings name w8; the guest source field is resolved to its CpuState
 * offset in the immutable stream. */
setf_native 0x3a00090d, 8
setf_native 0x3a00490d, 16

.purgem setf_native
