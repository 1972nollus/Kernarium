/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Derived from iSH Asbestos' ARM64 guest gadgets. Runtime blocks contain only
 * pointers to these precompiled routines and inline integer operands.
 */
#include "../offsets.inc"

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

.macro next_gadget
	ldr x8, [_stream], #8
	br x8
.endm

/* Native scalar FP gadgets share a host floating-point environment for the
 * whole threaded block. The first saves the caller's state and selects the
 * guest FPCR; later gadgets notice guest FPCR writes. Leave restores the host
 * state without leaking guest exception flags into the caller. */
.macro ensure_fp_environment
	ldr x6, [sp, #0x78]
	cbz x6, 97f
	ldr w6, [_cpu, #UML_GADGET_CPU_FPCR]
	ldr w5, [sp, #0x80]
	cmp w5, w6
	b.eq 99f
	str w6, [sp, #0x80]
	msr fpcr, x6
	b 99f
97:
	mrs x6, fpcr
	mrs x5, fpsr
	stp x6, x5, [sp, #0x68]
	mov x6, #1
	str x6, [sp, #0x78]
	ldr w6, [_cpu, #UML_GADGET_CPU_FPCR]
	str w6, [sp, #0x80]
	msr fpcr, x6
	/* FPSR exception and saturation flags are cumulative architectural
	 * state. Seed the native environment from the guest. */
	ldr w6, [_cpu, #UML_GADGET_CPU_FPSR]
	msr fpsr, x6
99:
.endm

.macro restore_fp_environment
	ldr x6, [sp, #0x78]
	cbz x6, 98f
	/* Commit native FP/AdvSIMD exception state before restoring the host. */
	mrs x5, fpsr
	str w5, [_cpu, #UML_GADGET_CPU_FPSR]
	ldp x6, x5, [sp, #0x68]
	msr fpcr, x6
	msr fpsr, x5
	str xzr, [sp, #0x78]
98:
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

/* Translate guest address x7 through the persistent 8,192-entry iSH-style
 * TLB. On success x7 is the host address. The miss helper updates the entry.
 */
.macro translate access, page_offset
	and x8, x7, #0xfffffffffffff000
	ubfx x9, x7, #12, #13
	eor x9, x9, x7, lsr #25
	and w9, w9, #0x1fff
	add x10, _tlb, #UML_GADGET_TLB_ENTRIES
	add x10, x10, x9, lsl #5
	ldr x11, [x10, #\page_offset]
	cmp x8, x11
	b.eq 91f
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x7
	mov w2, #\access
	bl NAME(uml_arm64_gadget_tlb_miss)
	ldp x1, x2, [sp], #16
	cbz x0, uml_arm64_gadget_memory_fault
	mov x7, x0
	b 92f
91:
	.if \access
	ldr x12, [x10, #UML_GADGET_TLB_ENTRY_WRITE_DATA_MINUS_ADDRESS]
	.else
	ldr x12, [x10, #UML_GADGET_TLB_ENTRY_READ_DATA_MINUS_ADDRESS]
	.endif
	add x7, x12, x7
92:
.endm

/* Rare cross-page accesses stay on the gadget path. The helper validates all
 * pages before stores, so a fault cannot leave a partially modified range.
 * q0/q1 carry up to 32 bytes to or from the helper.
 */
.macro transfer_slow access, address, size
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
	bl NAME(uml_arm64_gadget_memory_transfer)
	ldp x1, x2, [sp, #32]
	cbz w0, 97f
	.if !\access
	ldp q0, q1, [sp]
	.endif
	add sp, sp, #48
	b 98f
97:
	add sp, sp, #48
	b uml_arm64_gadget_memory_fault
98:
.endm

.text
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
	str xzr, [sp, #0x78]
	str wzr, [sp, #0x84]
	mov _stream, x0
	mov x29, x3
	next_gadget

.p2align 4
uml_arm64_gadget_leave:
	restore_fpmr_environment
	restore_fp_environment
	mov x1, x29
	ldr lr, [sp, #0x60]
	ldp x28, x29, [sp, #0x50]
	ldp x26, x27, [sp, #0x40]
	ldp x24, x25, [sp, #0x30]
	ldp x22, x23, [sp, #0x20]
	ldp x20, x21, [sp, #0x10]
	ldp x18, x19, [sp], #0x90
	ret

/* MOVZ/MOVN are folded to a full constant by the translator.  Rd==XZR is
 * folded to NOP, so the live entry is a three-load straight-line store. */
gadget move_wide_constant
	ldr x8, [_stream], #8
	ldr x11, [_stream], #8
	add x13, _cpu, #UML_GADGET_CPU_X0
	str x11, [x13, x8]
	next_gadget

/* MOVK keeps only Rd and imm16 in the stream. Width and halfword position are
 * separate entries; invalid 32-bit halfwords and XZR were rejected/folded by
 * the translator. */
.macro move_wide_keep bits64, shift, suffix
gadget move_wide_keep_\suffix
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx x11, x8, #8, #16
	add x13, _cpu, #UML_GADGET_CPU_X0
	ldr x14, [x13, x9, lsl #3]
	.if !\bits64
	mov w14, w14
	.endif
	mov x15, #0xffff
	.if \shift
	lsl x15, x15, #\shift
	lsl x11, x11, #\shift
	.endif
	bic x14, x14, x15
	orr x14, x14, x11
	str x14, [x13, x9, lsl #3]
	next_gadget
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
.macro add_sub_immediate_fast bits64, subtract, flags, source_sp, destination_special, suffix
gadget add_sub_immediate_\suffix
	ldr x8, [_stream], #8
	.if \source_sp
	ldr x15, [_cpu, #UML_GADGET_CPU_SP]
	.else
	ubfx w10, w8, #8, #5
	add x12, _cpu, #UML_GADGET_CPU_X0
	ldr x15, [x12, x10, lsl #3]
	.endif
	lsr x11, x8, #16
	.if \bits64
		.if \subtract
			.if \flags
	subs x16, x15, x11
			.else
	sub x16, x15, x11
			.endif
		.else
			.if \flags
	adds x16, x15, x11
			.else
	add x16, x15, x11
			.endif
		.endif
	.else
		.if \subtract
			.if \flags
	subs w16, w15, w11
			.else
	sub w16, w15, w11
			.endif
		.else
			.if \flags
	adds w16, w15, w11
			.else
	add w16, w15, w11
			.endif
		.endif
	.endif
	.if \flags
	mrs x17, nzcv
	str w17, [_cpu, #UML_GADGET_CPU_NZCV]
	.endif
	.if \destination_special
		.if !\flags
	str x16, [_cpu, #UML_GADGET_CPU_SP]
		.endif
	.else
	and w9, w8, #0x1f
	add x12, _cpu, #UML_GADGET_CPU_X0
	str x16, [x12, x9, lsl #3]
	.endif
	next_gadget
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
	subs x7, x15, x16
			.else
	sub x7, x15, x16
			.endif
		.else
			.if \flags
	adds x7, x15, x16
			.else
	add x7, x15, x16
			.endif
		.endif
	.else
		.if \subtract
			.if \flags
	subs w7, w15, w16
			.else
	sub w7, w15, w16
			.endif
		.else
			.if \flags
	adds w7, w15, w16
			.else
	add w7, w15, w16
			.endif
		.endif
	.endif
	.if \flags
	mrs x14, nzcv
	str w14, [_cpu, #UML_GADGET_CPU_NZCV]
	.endif
	.if !\destination_discard
	and x9, x8, #0xffff
	str x7, [_cpu, x9]
	.endif
	next_gadget
.endm

.macro add_sub_register_shifted_fast bits64, subtract, flags, shift_kind, left_zero, right_zero, destination_discard, suffix
gadget add_sub_register_\suffix
	ldr x8, [_stream], #8
	.if \left_zero
	mov x15, xzr
	.else
	ubfx x10, x8, #16, #16
	ldr x15, [_cpu, x10]
	.endif
	.if \right_zero
	mov x16, xzr
	.else
	ubfx x11, x8, #32, #16
	ldr x16, [_cpu, x11]
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
.endm

.macro add_sub_register_extended_fast bits64, subtract, flags, extension, shift_nonzero, right_zero, destination_discard, suffix
gadget add_sub_register_\suffix
	ldr x8, [_stream], #8
	ubfx x10, x8, #16, #16
	ldr x15, [_cpu, x10]
	.if \right_zero
	mov x16, xzr
	.else
	ubfx x11, x8, #32, #16
	ldr x16, [_cpu, x11]
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

/* Production ADC/SBC entries.  All instruction-invariant choices, including
 * XZR operands and a discarded XZR destination, are translation-selected.
 * Stream operand: Rd[4:0], Rn[12:8], Rm[20:16]. */
.macro add_sub_carry_fast bits64, subtract, flags, left_zero, right_zero, destination_zero, suffix
gadget add_sub_carry_\suffix
	ldr x8, [_stream], #8
	.if \left_zero
	mov x15, xzr
	.else
	ubfx w10, w8, #8, #5
	add x12, _cpu, #UML_GADGET_CPU_X0
	ldr x15, [x12, x10, lsl #3]
	.endif
	.if \right_zero
	mov x16, xzr
	.else
	ubfx w11, w8, #16, #5
	add x12, _cpu, #UML_GADGET_CPU_X0
	ldr x16, [x12, x11, lsl #3]
	.endif
	ldr w12, [_cpu, #UML_GADGET_CPU_NZCV]
	msr nzcv, x12
	.if \bits64
		.if \subtract
			.if \flags
	sbcs x7, x15, x16
			.else
	sbc x7, x15, x16
			.endif
		.else
			.if \flags
	adcs x7, x15, x16
			.else
	adc x7, x15, x16
			.endif
		.endif
	.else
		.if \subtract
			.if \flags
	sbcs w7, w15, w16
			.else
	sbc w7, w15, w16
			.endif
		.else
			.if \flags
	adcs w7, w15, w16
			.else
	adc w7, w15, w16
			.endif
		.endif
	.endif
	.if \flags
	mrs x12, nzcv
	str w12, [_cpu, #UML_GADGET_CPU_NZCV]
	.endif
	.if !\destination_zero
	and w9, w8, #0x1f
	add x12, _cpu, #UML_GADGET_CPU_X0
	str x7, [x12, x9, lsl #3]
	.endif
	next_gadget
.endm

.macro add_sub_carry_zero_modes bits64, subtract, flags, left_zero, right_zero, prefix
add_sub_carry_fast \bits64, \subtract, \flags, \left_zero, \right_zero, 0, \prefix\()_r
add_sub_carry_fast \bits64, \subtract, \flags, \left_zero, \right_zero, 1, \prefix\()_z
.endm

.macro add_sub_carry_source_modes bits64, subtract, flags, prefix
add_sub_carry_zero_modes \bits64, \subtract, \flags, 0, 0, \prefix\()_rr
add_sub_carry_zero_modes \bits64, \subtract, \flags, 0, 1, \prefix\()_rz
add_sub_carry_zero_modes \bits64, \subtract, \flags, 1, 0, \prefix\()_zr
add_sub_carry_zero_modes \bits64, \subtract, \flags, 1, 1, \prefix\()_zz
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
.purgem add_sub_carry_zero_modes
.purgem add_sub_carry_fast

/* FEAT_MTE GMI/SUBP/SUBPS. Encoding-constant SP/ZR choices select one of
 * eight straight-line variants for each operation. No decoder checks or
 * conditional branches remain in the valid production hot path.
 * Stream operand: Rd[4:0], Rn[12:8], Rm[20:16].
 */
.macro mte_pointer opname, operation, n_special, m_special, d_zero, suffix
gadget mte_\opname\()_\suffix
	ldr x8, [_stream], #8
	.if !\n_special
	ubfx w10, w8, #8, #5
	add x17, _cpu, #UML_GADGET_CPU_X0
	ldr x12, [x17, x10, lsl #3]
	.else
	ldr x12, [_cpu, #UML_GADGET_CPU_SP]
	.endif
	.if !\m_special
	ubfx w11, w8, #16, #5
	add x17, _cpu, #UML_GADGET_CPU_X0
	ldr x13, [x17, x11, lsl #3]
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
	mrs x15, nzcv
	str w15, [_cpu, #UML_GADGET_CPU_NZCV]
	.else
	sub x14, x12, x13
	.endif
	.endif

	.if !\d_zero
	and w9, w8, #0x1f
	add x17, _cpu, #UML_GADGET_CPU_X0
	str x14, [x17, x9, lsl #3]
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
 * Stream operand: Rd[4:0], Rn[12:8], tag offset[19:16], imm6[29:24].
 */
.macro mte_tag_add_sub opname, subtract, source_sp, destination_sp, suffix
gadget mte_\opname\()_\suffix
	ldr x8, [_stream], #8
	.if !\source_sp
	ubfx w10, w8, #8, #5
	add x17, _cpu, #UML_GADGET_CPU_X0
	ldr x12, [x17, x10, lsl #3]
	.else
	ldr x12, [_cpu, #UML_GADGET_CPU_SP]
	.endif
	ubfx w13, w8, #16, #4
	ubfx x14, x12, #56, #4
	orr w13, w13, w14, lsl #4
	add x15, _cpu, #UML_GADGET_CPU_MTE_TAG_CHOICES
	ldrb w13, [x15, x13]
	ubfx x14, x8, #24, #6
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
	and w9, w8, #0x1f
	add x17, _cpu, #UML_GADGET_CPU_X0
	str x14, [x17, x9, lsl #3]
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
 * selection itself is branch-free. Stream: [register operand][popcount table].
 */
.macro mte_irg n_sp, m_zero, d_sp, suffix
gadget mte_irg_\suffix
	ldr x8, [_stream], #8
	ldr x6, [_stream], #8
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
	and w9, w8, #0x1f
	add x17, _cpu, #UML_GADGET_CPU_X0
	str x12, [x17, x9, lsl #3]
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
 * are translation-selected; the old unused PC stream word is gone. Stream:
 * destination, left, right, addend CpuState byte offsets in 16-bit fields. */
.macro multiply_add_fast operation, left_zero, right_zero, addend_zero, destination_zero, suffix
gadget multiply_add_\suffix
	ldr x8, [_stream], #8
	.if \left_zero
	mov x15, xzr
	.else
	ubfx x10, x8, #16, #16
	ldr x15, [_cpu, x10]
	.endif
	.if \right_zero
	mov x16, xzr
	.else
	ubfx x11, x8, #32, #16
	ldr x16, [_cpu, x11]
	.endif
	.if \operation < 8
		.if \addend_zero
	mov x17, xzr
		.else
	ubfx x12, x8, #48, #16
	ldr x17, [_cpu, x12]
		.endif
	.endif
	.if \operation == 0
	madd w7, w15, w16, w17
	.elseif \operation == 1
	msub w7, w15, w16, w17
	.elseif \operation == 2
	madd x7, x15, x16, x17
	.elseif \operation == 3
	msub x7, x15, x16, x17
	.elseif \operation == 4
	smaddl x7, w15, w16, x17
	.elseif \operation == 5
	smsubl x7, w15, w16, x17
	.elseif \operation == 6
	umaddl x7, w15, w16, x17
	.elseif \operation == 7
	umsubl x7, w15, w16, x17
	.elseif \operation == 8
	smulh x7, x15, x16
	.else
	umulh x7, x15, x16
	.endif
	.if !\destination_zero
	and x9, x8, #0xffff
	str x7, [_cpu, x9]
	.endif
	next_gadget
.endm
.macro multiply_destinations operation, left_zero, right_zero, addend_zero, prefix
multiply_add_fast \operation, \left_zero, \right_zero, \addend_zero, 0, \prefix\()_r
multiply_add_fast \operation, \left_zero, \right_zero, \addend_zero, 1, \prefix\()_z
.endm
.macro multiply_addends operation, left_zero, right_zero, prefix
multiply_destinations \operation, \left_zero, \right_zero, 0, \prefix\()_r
multiply_destinations \operation, \left_zero, \right_zero, 1, \prefix\()_z
.endm
.macro multiply_sources operation, prefix
multiply_addends \operation, 0, 0, \prefix\()_rr
multiply_addends \operation, 0, 1, \prefix\()_rz
multiply_addends \operation, 1, 0, \prefix\()_zr
multiply_addends \operation, 1, 1, \prefix\()_zz
.endm

multiply_sources 0, w_madd
multiply_sources 1, w_msub
multiply_sources 2, x_madd
multiply_sources 3, x_msub
multiply_sources 4, smaddl
multiply_sources 5, smsubl
multiply_sources 6, umaddl
multiply_sources 7, umsubl
multiply_sources 8, smulh
multiply_sources 9, umulh

.purgem multiply_sources
.purgem multiply_addends
.purgem multiply_destinations
.purgem multiply_add_fast

/* Translation-selected LSLV/LSRV/ASRV/RORV. */
.macro variable_shift_fast operation, bits64, left_zero, right_zero, destination_zero, suffix
gadget variable_shift_\suffix
	ldr x8, [_stream], #8
	add x13, _cpu, #UML_GADGET_CPU_X0
	.if \left_zero
	mov x15, xzr
	.else
	ubfx w10, w8, #8, #5
	ldr x15, [x13, x10, lsl #3]
	.endif
	.if \right_zero
	mov x16, xzr
	.else
	ubfx w11, w8, #16, #5
	ldr x16, [x13, x11, lsl #3]
	.endif
	.if \bits64
		.if \operation == 0
	lsl x7, x15, x16
		.elseif \operation == 1
	lsr x7, x15, x16
		.elseif \operation == 2
	asr x7, x15, x16
		.else
	ror x7, x15, x16
		.endif
	.else
		.if \operation == 0
	lsl w7, w15, w16
		.elseif \operation == 1
	lsr w7, w15, w16
		.elseif \operation == 2
	asr w7, w15, w16
		.else
	ror w7, w15, w16
		.endif
	.endif
	.if !\destination_zero
	and w9, w8, #0x1f
	str x7, [x13, x9, lsl #3]
	.endif
	next_gadget
.endm

.macro variable_shift_destinations operation, bits64, left_zero, right_zero, prefix
variable_shift_fast \operation, \bits64, \left_zero, \right_zero, 0, \prefix\()_r
variable_shift_fast \operation, \bits64, \left_zero, \right_zero, 1, \prefix\()_z
.endm
.macro variable_shift_sources operation, bits64, prefix
variable_shift_destinations \operation, \bits64, 0, 0, \prefix\()_rr
variable_shift_destinations \operation, \bits64, 0, 1, \prefix\()_rz
variable_shift_destinations \operation, \bits64, 1, 0, \prefix\()_zr
variable_shift_destinations \operation, \bits64, 1, 1, \prefix\()_zz
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
.purgem variable_shift_destinations
.purgem variable_shift_fast

/* Translation-selected UDIV/SDIV. Host A64 already implements the guest's
 * zero-divisor and signed-overflow results without a conditional branch. */
.macro divide_fast signed, bits64, left_zero, right_zero, destination_zero, suffix
gadget divide_\suffix
	ldr x8, [_stream], #8
	add x12, _cpu, #UML_GADGET_CPU_X0
	.if \left_zero
	mov x15, xzr
	.else
	ubfx w10, w8, #8, #5
	ldr x15, [x12, x10, lsl #3]
	.endif
	.if \right_zero
	mov x16, xzr
	.else
	ubfx w11, w8, #16, #5
	ldr x16, [x12, x11, lsl #3]
	.endif
	.if \bits64
		.if \signed
	sdiv x7, x15, x16
		.else
	udiv x7, x15, x16
		.endif
	.else
		.if \signed
	sdiv w7, w15, w16
		.else
	udiv w7, w15, w16
		.endif
	.endif
	.if !\destination_zero
	and w9, w8, #0x1f
	str x7, [x12, x9, lsl #3]
	.endif
	next_gadget
.endm
.macro divide_destinations signed, bits64, left_zero, right_zero, prefix
divide_fast \signed, \bits64, \left_zero, \right_zero, 0, \prefix\()_r
divide_fast \signed, \bits64, \left_zero, \right_zero, 1, \prefix\()_z
.endm
.macro divide_sources signed, bits64, prefix
divide_destinations \signed, \bits64, 0, 0, \prefix\()_rr
divide_destinations \signed, \bits64, 0, 1, \prefix\()_rz
divide_destinations \signed, \bits64, 1, 0, \prefix\()_zr
divide_destinations \signed, \bits64, 1, 1, \prefix\()_zz
.endm

divide_sources 0, 0, w_u
divide_sources 1, 0, w_s
divide_sources 0, 1, x_u
divide_sources 1, 1, x_s

.purgem divide_sources
.purgem divide_destinations
.purgem divide_fast

/* Translation-selected CRC32/CRC32C. */
.macro crc32_fast operation, left_zero, right_zero, destination_zero, suffix
gadget crc32_\suffix
	ldr x8, [_stream], #8
	add x13, _cpu, #UML_GADGET_CPU_X0
	.if \left_zero
	mov w14, wzr
	.else
	ubfx w10, w8, #8, #5
	ldr x14, [x13, x10, lsl #3]
	.endif
	.if \right_zero
	mov x15, xzr
	.else
	ubfx w11, w8, #16, #5
	ldr x15, [x13, x11, lsl #3]
	.endif
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
	.if !\destination_zero
	and w9, w8, #0x1f
	str x16, [x13, x9, lsl #3]
	.endif
	next_gadget
.endm
.macro crc32_destinations operation, left_zero, right_zero, prefix
crc32_fast \operation, \left_zero, \right_zero, 0, \prefix\()_r
crc32_fast \operation, \left_zero, \right_zero, 1, \prefix\()_z
.endm
.macro crc32_sources operation, prefix
crc32_destinations \operation, 0, 0, \prefix\()_rr
crc32_destinations \operation, 0, 1, \prefix\()_rz
crc32_destinations \operation, 1, 0, \prefix\()_zr
crc32_destinations \operation, 1, 1, \prefix\()_zz
.endm

crc32_sources 0, b
crc32_sources 1, h
crc32_sources 2, w
crc32_sources 3, x
crc32_sources 4, cb
crc32_sources 5, ch
crc32_sources 6, cw
crc32_sources 7, cx

.purgem crc32_sources
.purgem crc32_destinations
.purgem crc32_fast

/* Translation-selected EXTR/ROR. Width, zero shift, and all XZR choices are
 * fixed. Stream operand: Rd, Rn, Rm, shift in successive bytes. */
.macro extract_register_fast bits64, shift_nonzero, left_zero, right_zero, destination_zero, suffix
gadget extract_register_\suffix
	ldr x8, [_stream], #8
	add x13, _cpu, #UML_GADGET_CPU_X0
	.if \left_zero
	mov x14, xzr
	.else
	ubfx w10, w8, #8, #5
	ldr x14, [x13, x10, lsl #3]
	.endif
	.if \right_zero
	mov x15, xzr
	.else
	ubfx w11, w8, #16, #5
	ldr x15, [x13, x11, lsl #3]
	.endif
	.if \shift_nonzero
	ubfx w12, w8, #24, #6
		.if \bits64
	lsr x16, x15, x12
	mov w17, #64
	sub w17, w17, w12
	lsl x17, x14, x17
	orr x16, x16, x17
		.else
	lsr w16, w15, w12
	mov w17, #32
	sub w17, w17, w12
	lsl w17, w14, w17
	orr w16, w16, w17
		.endif
	.else
		.if \bits64
	mov x16, x15
		.else
	mov w16, w15
		.endif
	.endif
	.if !\destination_zero
	and w9, w8, #0x1f
	str x16, [x13, x9, lsl #3]
	.endif
	next_gadget
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
.macro conditional_select_fast name, condition, operation, bits64, suffix
gadget conditional_select_fast_\name\()_\suffix
	ldr x8, [_stream], #8
	ubfx x9, x8, #0, #16
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	ldr x15, [_cpu, x10]
	ldr x16, [_cpu, x11]
	ldr w12, [_cpu, #UML_GADGET_CPU_NZCV]
	msr nzcv, x12
	.if \bits64
		.if \operation == 0
	csel x7, x15, x16, \condition
		.elseif \operation == 1
	csinc x7, x15, x16, \condition
		.elseif \operation == 2
	csinv x7, x15, x16, \condition
		.else
	csneg x7, x15, x16, \condition
		.endif
	.else
		.if \operation == 0
	csel w7, w15, w16, \condition
		.elseif \operation == 1
	csinc w7, w15, w16, \condition
		.elseif \operation == 2
	csinv w7, w15, w16, \condition
		.else
	csneg w7, w15, w16, \condition
		.endif
	.endif
	str x7, [_cpu, x9]
	next_gadget
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
conditional_select_fast_modes always, al

.purgem conditional_select_fast_modes
.purgem conditional_select_fast

/* Production CCMN/CCMP. The sole conditional branch is the architectural
 * condition itself; width, add/subtract, immediate/register and XZR selection
 * are fixed at translation. Stream: left offset, right offset/immediate, NZCV. */
.macro conditional_compare_fast name, condition, subtract, bits64, immediate, suffix
gadget conditional_compare_fast_\name\()_\suffix
	ldr x8, [_stream], #8
	ldr w9, [_cpu, #UML_GADGET_CPU_NZCV]
	msr nzcv, x9
	b.\condition .Lconditional_compare_true\@
	ubfx x9, x8, #32, #4
	lsl w9, w9, #28
	str w9, [_cpu, #UML_GADGET_CPU_NZCV]
	next_gadget
.Lconditional_compare_true\@:
	ubfx x10, x8, #0, #16
	ldr x12, [_cpu, x10]
	.if \immediate
	ubfx x13, x8, #16, #5
	.else
	ubfx x11, x8, #16, #16
	ldr x13, [_cpu, x11]
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
	mrs x9, nzcv
	str w9, [_cpu, #UML_GADGET_CPU_NZCV]
	next_gadget
.endm
.macro conditional_compare_fast_modes name, condition
conditional_compare_fast \name, \condition, 0, 0, 0, w_cmn_r
conditional_compare_fast \name, \condition, 0, 0, 1, w_cmn_i
conditional_compare_fast \name, \condition, 0, 1, 0, x_cmn_r
conditional_compare_fast \name, \condition, 0, 1, 1, x_cmn_i
conditional_compare_fast \name, \condition, 1, 0, 0, w_cmp_r
conditional_compare_fast \name, \condition, 1, 0, 1, w_cmp_i
conditional_compare_fast \name, \condition, 1, 1, 0, x_cmp_r
conditional_compare_fast \name, \condition, 1, 1, 1, x_cmp_i
.endm

conditional_compare_fast_modes eq, eq
conditional_compare_fast_modes ne, ne
conditional_compare_fast_modes hs, hs
conditional_compare_fast_modes lo, lo
conditional_compare_fast_modes mi, mi
conditional_compare_fast_modes pl, pl
conditional_compare_fast_modes vs, vs
conditional_compare_fast_modes vc, vc
conditional_compare_fast_modes hi, hi
conditional_compare_fast_modes ls, ls
conditional_compare_fast_modes ge, ge
conditional_compare_fast_modes lt, lt
conditional_compare_fast_modes gt, gt
conditional_compare_fast_modes le, le
conditional_compare_fast_modes always, al

.purgem conditional_compare_fast_modes
.purgem conditional_compare_fast

/* Production logical shifted-register entries.  Builder specializes width,
 * operation, inversion, shift kind (including the common no-shift form), and
 * every XZR choice. Stream: Rd, Rn, Rm, shift in successive bytes. */
.macro logical_shifted_fast operation, bits64, invert, shift_kind, left_zero, right_zero, destination_zero, suffix
gadget logical_shifted_\suffix
	ldr x8, [_stream], #8
	add x14, _cpu, #UML_GADGET_CPU_X0
	.if \left_zero
	mov x15, xzr
	.else
	ubfx w10, w8, #8, #5
	ldr x15, [x14, x10, lsl #3]
	.endif
	.if \right_zero
	mov x16, xzr
	.else
	ubfx w11, w8, #16, #5
	ldr x16, [x14, x11, lsl #3]
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
	.if \bits64
		.if \operation == 0
	and x7, x15, x16
		.elseif \operation == 1
	orr x7, x15, x16
		.elseif \operation == 2
	eor x7, x15, x16
		.else
	ands x7, x15, x16
		.endif
	.else
		.if \operation == 0
	and w7, w15, w16
		.elseif \operation == 1
	orr w7, w15, w16
		.elseif \operation == 2
	eor w7, w15, w16
		.else
	ands w7, w15, w16
		.endif
	.endif
	.if \operation == 3
	mrs x14, nzcv
	str w14, [_cpu, #UML_GADGET_CPU_NZCV]
	.endif
	.if !\destination_zero
	and w9, w8, #0x1f
	add x14, _cpu, #UML_GADGET_CPU_X0
	str x7, [x14, x9, lsl #3]
	.endif
	next_gadget
.endm
.macro logical_shifted_destinations operation, bits64, invert, shift_kind, left_zero, right_zero, prefix
logical_shifted_fast \operation, \bits64, \invert, \shift_kind, \left_zero, \right_zero, 0, \prefix\()_r
logical_shifted_fast \operation, \bits64, \invert, \shift_kind, \left_zero, \right_zero, 1, \prefix\()_z
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
.macro logical_immediate_fast operation, bits64, source_zero, destination_special, suffix
gadget logical_immediate_\suffix
	ldr x8, [_stream], #8
	ldr x14, [_stream], #8
	.if \source_zero
	mov x15, xzr
	.else
	ubfx w10, w8, #8, #5
	add x12, _cpu, #UML_GADGET_CPU_X0
	ldr x15, [x12, x10, lsl #3]
	.endif
	.if \bits64
		.if \operation == 0
	and x16, x15, x14
		.elseif \operation == 1
	orr x16, x15, x14
		.elseif \operation == 2
	eor x16, x15, x14
		.else
	ands x16, x15, x14
		.endif
	.else
		.if \operation == 0
	and w16, w15, w14
		.elseif \operation == 1
	orr w16, w15, w14
		.elseif \operation == 2
	eor w16, w15, w14
		.else
	ands w16, w15, w14
		.endif
	.endif
	.if \operation == 3
	mrs x17, nzcv
	str w17, [_cpu, #UML_GADGET_CPU_NZCV]
	.endif
	.if \destination_special
		.if \operation != 3
	str x16, [_cpu, #UML_GADGET_CPU_SP]
		.endif
	.else
	and w9, w8, #0x1f
	add x12, _cpu, #UML_GADGET_CPU_X0
	str x16, [x12, x9, lsl #3]
	.endif
	next_gadget
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
.macro bitfield_fast operation, bits64, rotate_nonzero, source_zero, suffix
gadget bitfield_\suffix
	ldr x8, [_stream], #8
	ldr x14, [_stream], #8
	ldr x17, [_stream], #8
	.if \source_zero
	mov x15, xzr
	.else
	ubfx x10, x8, #16, #16
	ldr x15, [_cpu, x10]
	.endif
	.if !\bits64
	mov w15, w15
	.endif
	.if \rotate_nonzero
	ubfx x12, x8, #32, #6
		.if \bits64
	ror x16, x15, x12
		.else
	ror w16, w15, w12
		.endif
	.else
	mov x16, x15
	.endif
	and x16, x16, x14
	.if \operation == 0
	ubfx x11, x8, #40, #6
	lsr x6, x15, x11
	tst x6, #1
	csetm x6, ne
	.elseif \operation == 1
	and x9, x8, #0xffff
	ldr x6, [_cpu, x9]
	bic x7, x6, x14
	orr x16, x16, x7
	.else
	mov x6, xzr
	.endif
	bic x6, x6, x17
	and x16, x16, x17
	orr x16, x16, x6
	.if !\bits64
	mov w16, w16
	.endif
	and x9, x8, #0xffff
	str x16, [_cpu, x9]
	next_gadget
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

/* Translation-selected data-processing (1 source) operations. */
.macro bit_reverse_count_fast operation, bits64, source_zero, destination_zero, suffix
gadget bit_reverse_count_\suffix
	ldr x8, [_stream], #8
	.if \source_zero
	mov x12, xzr
	.else
	ubfx w10, w8, #8, #5
	add x14, _cpu, #UML_GADGET_CPU_X0
	ldr x12, [x14, x10, lsl #3]
	.endif
	.if \bits64
		.if \operation == 0
	rbit x13, x12
		.elseif \operation == 1
	rev16 x13, x12
		.elseif \operation == 2
	rev32 x13, x12
		.elseif \operation == 3
	rev x13, x12
		.elseif \operation == 4
	clz x13, x12
		.elseif \operation == 5
	cls x13, x12
		.elseif \operation == 6
	rbit x13, x12
	clz x13, x13
		.elseif \operation == 7
	fmov d0, x12
	cnt v0.8b, v0.8b
	addv b0, v0.8b
	umov w13, v0.b[0]
		.else
	cmp x12, #0
	cneg x13, x12, mi
		.endif
	.else
		.if \operation == 0
	rbit w13, w12
		.elseif \operation == 1
	rev16 w13, w12
		.elseif \operation == 2
	rev w13, w12
		.elseif \operation == 4
	clz w13, w12
		.elseif \operation == 5
	cls w13, w12
		.elseif \operation == 6
	rbit w13, w12
	clz w13, w13
		.elseif \operation == 7
	mov w12, w12
	fmov d0, x12
	cnt v0.8b, v0.8b
	addv b0, v0.8b
	umov w13, v0.b[0]
		.else
	cmp w12, #0
	cneg w13, w12, mi
		.endif
	.endif
	.if !\destination_zero
	and w9, w8, #0x1f
	add x14, _cpu, #UML_GADGET_CPU_X0
	str x13, [x14, x9, lsl #3]
	.endif
	next_gadget
.endm
.macro bit_reverse_count_modes operation, bits64, prefix
bit_reverse_count_fast \operation, \bits64, 0, 0, \prefix\()_rr
bit_reverse_count_fast \operation, \bits64, 0, 1, \prefix\()_rz
bit_reverse_count_fast \operation, \bits64, 1, 0, \prefix\()_zr
bit_reverse_count_fast \operation, \bits64, 1, 1, \prefix\()_zz
.endm

bit_reverse_count_modes 0, 0, w_rbit
bit_reverse_count_modes 1, 0, w_rev16
bit_reverse_count_modes 2, 0, w_rev
bit_reverse_count_modes 4, 0, w_clz
bit_reverse_count_modes 5, 0, w_cls
bit_reverse_count_modes 6, 0, w_ctz
bit_reverse_count_modes 7, 0, w_cnt
bit_reverse_count_modes 8, 0, w_abs
bit_reverse_count_modes 0, 1, x_rbit
bit_reverse_count_modes 1, 1, x_rev16
bit_reverse_count_modes 2, 1, x_rev32
bit_reverse_count_modes 3, 1, x_rev
bit_reverse_count_modes 4, 1, x_clz
bit_reverse_count_modes 5, 1, x_cls
bit_reverse_count_modes 6, 1, x_ctz
bit_reverse_count_modes 7, 1, x_cnt
bit_reverse_count_modes 8, 1, x_abs

.purgem bit_reverse_count_modes
.purgem bit_reverse_count_fast

/* FEAT_PAuth strip operations are exact native host instructions. Translation
 * resolves instruction/data and XZR cases, so ordinary execution is one
 * register load, XPAC, and one register store. */
.macro xpac_register name, instruction
gadget \name
	ldr x8, [_stream], #8
	ldr x9, [_cpu, x8]
	\instruction x9
	str x9, [_cpu, x8]
	next_gadget
.endm

xpac_register xpaci, xpaci
xpac_register xpacd, xpacd

.purgem xpac_register

gadget xpaclri
	ldr x30, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	xpaclri
	str x30, [_cpu, #(UML_GADGET_CPU_X0 + 30 * 8)]
	next_gadget

/* Classic FEAT_PAuth operations. Packed operands are direct CpuState byte
 * offsets: destination in bits[15:0], modifier in bits[31:16]. The builder
 * selects modifier-zero forms separately, avoiding an execution-time test. */
.macro pauth_operation name, instruction, zero_instruction
gadget pauth_\name\()_r
	ldr x8, [_stream], #8
	and w10, w8, #0xffff
	ubfx w11, w8, #16, #16
	ldr x9, [_cpu, x10]
	ldr x12, [_cpu, x11]
	\instruction x9, x12
	str x9, [_cpu, x10]
	next_gadget
gadget pauth_\name\()_z
	ldr x8, [_stream], #8
	and w10, w8, #0xffff
	ldr x9, [_cpu, x10]
	\zero_instruction x9
	str x9, [_cpu, x10]
	next_gadget
.endm

pauth_operation pacia, pacia, paciza
pauth_operation pacib, pacib, pacizb
pauth_operation pacda, pacda, pacdza
pauth_operation pacdb, pacdb, pacdzb
pauth_operation autia, autia, autiza
pauth_operation autib, autib, autizb
pauth_operation autda, autda, autdza
pauth_operation autdb, autdb, autdzb

.purgem pauth_operation

/* PACGA writes a fresh 64-bit result, so the destination need not be loaded.
 * The builder separately selects the XZR first-source form and packs the
 * remaining direct CpuState offsets into one stream word. */
gadget pauth_pacga_r
	ldr x8, [_stream], #8
	and w10, w8, #0xffff
	ubfx w11, w8, #16, #16
	ubfx x12, x8, #32, #16
	ldr x9, [_cpu, x11]
	ldr x13, [_cpu, x12]
	pacga x14, x9, x13
	str x14, [_cpu, x10]
	next_gadget

gadget pauth_pacga_z
	ldr x8, [_stream], #8
	and w10, w8, #0xffff
	ubfx x12, x8, #32, #16
	ldr x13, [_cpu, x12]
	pacga x14, xzr, x13
	str x14, [_cpu, x10]
	next_gadget

/* Production FEAT_CSSC min/max. Width and immediate/register form are fixed;
 * XZR sources use CpuState::gadget_zero and XZR destination is folded to NOP. */
.macro integer_min_max_fast name, condition, is_signed, bits64, immediate, suffix
gadget integer_\name\()_fast_\suffix
	ldr x8, [_stream], #8
	ubfx x9, x8, #0, #16
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	ldr x12, [_cpu, x10]
	.if \immediate
	mov x13, x11
		.if \is_signed
	sxtb x13, w13
		.endif
	.else
	ldr x13, [_cpu, x11]
	.endif
	.if \bits64
	cmp x12, x13
	csel x14, x12, x13, \condition
	.else
	cmp w12, w13
	csel w14, w12, w13, \condition
	.endif
	str x14, [_cpu, x9]
	next_gadget
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

gadget cfinv
	ldr w8, [_cpu, #UML_GADGET_CPU_NZCV]
	eor w8, w8, #0x20000000
	str w8, [_cpu, #UML_GADGET_CPU_NZCV]
	next_gadget

gadget axflag
	ldr w8, [_cpu, #UML_GADGET_CPU_NZCV]
	ubfx w9, w8, #30, #1
	ubfx w10, w8, #29, #1
	ubfx w11, w8, #28, #1
	orr w9, w9, w11
	bic w10, w10, w11
	lsl w9, w9, #30
	orr w9, w9, w10, lsl #29
	str w9, [_cpu, #UML_GADGET_CPU_NZCV]
	next_gadget

gadget xaflag
	ldr w8, [_cpu, #UML_GADGET_CPU_NZCV]
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
	str w12, [_cpu, #UML_GADGET_CPU_NZCV]
	next_gadget

gadget rmif_fast
	ldr x8, [_stream], #8
	ubfx x9, x8, #0, #16
	ldr x12, [_cpu, x9]
	ubfx x11, x8, #16, #6
	ror x13, x12, x11
	and w13, w13, #0xf
	ubfx w14, w8, #24, #4
	lsl w13, w13, #28
	lsl w14, w14, #28
	ldr w15, [_cpu, #UML_GADGET_CPU_NZCV]
	bic w15, w15, w14
	and w13, w13, w14
	orr w15, w15, w13
	str w15, [_cpu, #UML_GADGET_CPU_NZCV]
	next_gadget

.macro setf_fast width, suffix
gadget setf_fast_\suffix
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ldr w12, [_cpu, x9]
	mov w13, #\width
	sub w14, w13, #1
	lsrv w15, w12, w14
	and w15, w15, #1
	lsrv w16, w12, w13
	eor w16, w16, w15
	and w16, w16, #1
	mov w17, #1
	lslv w17, w17, w13
	sub w17, w17, #1
	tst w12, w17
	cset w17, eq
	ldr w18, [_cpu, #UML_GADGET_CPU_NZCV]
	and w18, w18, #0x20000000
	orr w18, w18, w15, lsl #31
	orr w18, w18, w17, lsl #30
	orr w18, w18, w16, lsl #28
	str w18, [_cpu, #UML_GADGET_CPU_NZCV]
	next_gadget
.endm

setf_fast 8, 8
setf_fast 16, 16

.purgem setf_fast
