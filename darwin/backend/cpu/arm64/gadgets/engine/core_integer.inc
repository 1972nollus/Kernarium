/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Derived from iSH Asbestos' ARM64 guest gadgets. Runtime blocks contain only
 * pointers to these precompiled routines and inline integer operands.
 */
#include "../offsets.inc"

/* Apple Silicon exposes the Armv8 CRC and cryptographic extensions. Keep the
 * assembler aware of them so extension-backed gadgets can use native ops. */
.arch armv8-a+crc+crypto

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
	msr fpsr, xzr
99:
.endm

.macro restore_fp_environment
	ldr x6, [sp, #0x78]
	cbz x6, 98f
	ldp x6, x5, [sp, #0x68]
	msr fpcr, x6
	msr fpsr, x5
	str xzr, [sp, #0x78]
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
	b.ne 91f
	.if \access
	ldr x12, [x10, #UML_GADGET_TLB_ENTRY_WRITE_DATA_MINUS_ADDRESS]
	.else
	ldr x12, [x10, #UML_GADGET_TLB_ENTRY_READ_DATA_MINUS_ADDRESS]
	.endif
	add x7, x12, x7
	b 92f
91:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x7
	mov w2, #\access
	bl NAME(uml_arm64_gadget_tlb_miss)
	ldp x1, x2, [sp], #16
	cbz x0, uml_arm64_gadget_memory_fault
	mov x7, x0
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
	/* x0 = data-only program stream, x1 = CpuState. */
	stp x18, x19, [sp, #-0x90]!
	stp x20, x21, [sp, #0x10]
	stp x22, x23, [sp, #0x20]
	stp x24, x25, [sp, #0x30]
	stp x26, x27, [sp, #0x40]
	stp x28, x29, [sp, #0x50]
	str lr, [sp, #0x60]
	str xzr, [sp, #0x78]
	mov _stream, x0
	next_gadget

.p2align 4
uml_arm64_gadget_leave:
	restore_fp_environment
	ldr lr, [sp, #0x60]
	ldp x28, x29, [sp, #0x50]
	ldp x26, x27, [sp, #0x40]
	ldp x24, x25, [sp, #0x30]
	ldp x22, x23, [sp, #0x20]
	ldp x20, x21, [sp, #0x10]
	ldp x18, x19, [sp], #0x90
	ret

gadget movz
	ldr x8, [_stream], #8
	and x9, x8, #0x1f
	ubfx x10, x8, #8, #2
	ubfx x11, x8, #16, #16
	ubfx x12, x8, #32, #1
	lsl x10, x10, #4
	lsl x11, x11, x10
	cbnz x12, 1f
	and x11, x11, #0xffffffff
1:	add x13, _cpu, #UML_GADGET_CPU_X0
	cmp x9, #31
	b.eq 2f
	str x11, [x13, x9, lsl #3]
2:	next_gadget

gadget movk
	ldr x8, [_stream], #8
	and x9, x8, #0x1f
	ubfx x10, x8, #8, #2
	ubfx x11, x8, #16, #16
	ubfx x12, x8, #32, #1
	add x13, _cpu, #UML_GADGET_CPU_X0
	cmp x9, #31
	b.eq 3f
	ldr x14, [x13, x9, lsl #3]
	cbnz x12, 1f
	and x14, x14, #0xffffffff
1:	lsl x10, x10, #4
	mov x15, #0xffff
	lsl x15, x15, x10
	lsl x11, x11, x10
	bic x14, x14, x15
	orr x14, x14, x11
	cbnz x12, 2f
	and x14, x14, #0xffffffff
2:	str x14, [x13, x9, lsl #3]
3:	next_gadget

gadget movn
	ldr x8, [_stream], #8
	and x9, x8, #0x1f
	ubfx x10, x8, #8, #2
	ubfx x11, x8, #16, #16
	ubfx x12, x8, #32, #1
	lsl x10, x10, #4
	lsl x11, x11, x10
	mvn x11, x11
	cbnz x12, 1f
	and x11, x11, #0xffffffff
1:	cmp x9, #31
	b.eq 2f
	add x13, _cpu, #UML_GADGET_CPU_X0
	str x11, [x13, x9, lsl #3]
2:	next_gadget

/* ADD/SUB (immediate), including the architectural SP and flag semantics.
 * The inline operand is the original instruction word.
 */
gadget add_sub_immediate
	ldr x8, [_stream], #8
	and x9, x8, #0x1f
	ubfx x10, x8, #5, #5
	ubfx x11, x8, #10, #12
	tbz x8, #22, 1f
	lsl x11, x11, #12
1:	cmp x10, #31
	b.eq 2f
	add x12, _cpu, #UML_GADGET_CPU_X0
	ldr x15, [x12, x10, lsl #3]
	b 3f
2:	ldr x15, [_cpu, #UML_GADGET_CPU_SP]
3:	tbnz x8, #31, 6f
	/* 32-bit form. Native ADDS/SUBS calculate exactly the guest NZCV. */
	tbnz x8, #30, 4f
	adds w16, w15, w11
	b 5f
4:	subs w16, w15, w11
5:	mov w16, w16
	b 9f
6:	tbnz x8, #30, 7f
	adds x16, x15, x11
	b 9f
7:	subs x16, x15, x11
9:	tbz x8, #29, 10f
	mrs x17, nzcv
	str w17, [_cpu, #UML_GADGET_CPU_NZCV]
10:	cmp x9, #31
	b.ne 11f
	/* Rd=31 is SP only when S=0; flag-setting aliases discard the result. */
	tbnz x8, #29, 12f
	str x16, [_cpu, #UML_GADGET_CPU_SP]
	b 12f
11:	add x12, _cpu, #UML_GADGET_CPU_X0
	str x16, [x12, x9, lsl #3]
12:	next_gadget

/* ADD/SUB (shifted or extended register). The inline operand is the original
 * instruction word. This is the same-architecture threaded-code form used by
 * iSH's ARM64 Asbestos backend, adapted to CpuState's compact register ABI.
 */
gadget add_sub_register
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #31, #1
	ubfx w13, w8, #30, #1
	ubfx w14, w8, #29, #1

	/* Shifted-register Rn is XZR; extended-register Rn may be SP. */
	cmp w10, #31
	b.ne addsub_load_rn
	tbnz x8, #21, addsub_load_sp
	mov x15, xzr
	b addsub_rn_ready
addsub_load_sp:
	ldr x15, [_cpu, #UML_GADGET_CPU_SP]
	b addsub_rn_ready
addsub_load_rn:
	add x7, _cpu, #UML_GADGET_CPU_X0
	ldr x15, [x7, x10, lsl #3]
addsub_rn_ready:
	cmp w11, #31
	b.eq addsub_zero_rm
	add x7, _cpu, #UML_GADGET_CPU_X0
	ldr x16, [x7, x11, lsl #3]
	b addsub_rm_ready
addsub_zero_rm:
	mov x16, xzr
addsub_rm_ready:
	tbnz x8, #21, addsub_extend

	ubfx w17, w8, #22, #2
	ubfx w6, w8, #10, #6
	cbz w6, addsub_operand_ready
	cbz w12, addsub_shift32
	cmp w17, #0
	b.eq addsub_lsl64
	cmp w17, #1
	b.eq addsub_lsr64
	asr x16, x16, x6
	b addsub_operand_ready
addsub_lsl64:
	lsl x16, x16, x6
	b addsub_operand_ready
addsub_lsr64:
	lsr x16, x16, x6
	b addsub_operand_ready
addsub_shift32:
	cmp w17, #0
	b.eq addsub_lsl32
	cmp w17, #1
	b.eq addsub_lsr32
	asr w16, w16, w6
	b addsub_operand_ready
addsub_lsl32:
	lsl w16, w16, w6
	b addsub_operand_ready
addsub_lsr32:
	lsr w16, w16, w6
	b addsub_operand_ready

addsub_extend:
	ubfx w17, w8, #13, #3
	ubfx w6, w8, #10, #3
	cmp w17, #0
	b.eq addsub_uxtb
	cmp w17, #1
	b.eq addsub_uxth
	cmp w17, #2
	b.eq addsub_uxtw
	cmp w17, #3
	b.eq addsub_extended
	cmp w17, #4
	b.eq addsub_sxtb
	cmp w17, #5
	b.eq addsub_sxth
	cmp w17, #6
	b.eq addsub_sxtw
	b addsub_extended
addsub_uxtb:
	and x16, x16, #0xff
	b addsub_extended
addsub_uxth:
	and x16, x16, #0xffff
	b addsub_extended
addsub_uxtw:
	mov w16, w16
	b addsub_extended
addsub_sxtb:
	sxtb x16, w16
	b addsub_extended
addsub_sxth:
	sxth x16, w16
	b addsub_extended
addsub_sxtw:
	sxtw x16, w16
addsub_extended:
	lsl x16, x16, x6

addsub_operand_ready:
	cbz w12, addsub_operation32
	cbnz w14, addsub_flags64
	cbnz w13, addsub_sub64
	add x7, x15, x16
	b addsub_store
addsub_sub64:
	sub x7, x15, x16
	b addsub_store
addsub_flags64:
	cbnz w13, addsub_subs64
	adds x7, x15, x16
	b addsub_save_flags
addsub_subs64:
	subs x7, x15, x16
	b addsub_save_flags

addsub_operation32:
	cbnz w14, addsub_flags32
	cbnz w13, addsub_sub32
	add w7, w15, w16
	b addsub_store
addsub_sub32:
	sub w7, w15, w16
	b addsub_store
addsub_flags32:
	cbnz w13, addsub_subs32
	adds w7, w15, w16
	b addsub_save_flags
addsub_subs32:
	subs w7, w15, w16

addsub_save_flags:
	mrs x15, nzcv
	str w15, [_cpu, #UML_GADGET_CPU_NZCV]

addsub_store:
	cmp w9, #31
	b.ne addsub_store_register
	/* Rd=31 is SP only for non-flag-setting extended-register forms. */
	tbz x8, #21, addsub_done
	cbnz w14, addsub_done
	str x7, [_cpu, #UML_GADGET_CPU_SP]
	b addsub_done
addsub_store_register:
	add x15, _cpu, #UML_GADGET_CPU_X0
	str x7, [x15, x9, lsl #3]
addsub_done:
	next_gadget

/* ADC/ADCS/SBC/SBCS, using the saved guest carry and native NZCV result. */
gadget add_sub_carry
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	cmp w10, #31
	b.eq carry_zero_left
	add x12, _cpu, #UML_GADGET_CPU_X0
	ldr x15, [x12, x10, lsl #3]
	b carry_left_ready
carry_zero_left:
	mov x15, xzr
carry_left_ready:
	cmp w11, #31
	b.eq carry_zero_right
	add x12, _cpu, #UML_GADGET_CPU_X0
	ldr x16, [x12, x11, lsl #3]
	b carry_right_ready
carry_zero_right:
	mov x16, xzr
carry_right_ready:
	ldr w12, [_cpu, #UML_GADGET_CPU_NZCV]
	msr nzcv, x12
	tbz x8, #31, carry_operation32
	tbnz x8, #30, carry_subtract64
	tbnz x8, #29, carry_add_flags64
	adc x7, x15, x16
	b carry_store
carry_add_flags64:
	adcs x7, x15, x16
	b carry_save_flags
carry_subtract64:
	tbnz x8, #29, carry_subtract_flags64
	sbc x7, x15, x16
	b carry_store
carry_subtract_flags64:
	sbcs x7, x15, x16
	b carry_save_flags
carry_operation32:
	tbnz x8, #30, carry_subtract32
	tbnz x8, #29, carry_add_flags32
	adc w7, w15, w16
	b carry_store
carry_add_flags32:
	adcs w7, w15, w16
	b carry_save_flags
carry_subtract32:
	tbnz x8, #29, carry_subtract_flags32
	sbc w7, w15, w16
	b carry_store
carry_subtract_flags32:
	sbcs w7, w15, w16
carry_save_flags:
	mrs x12, nzcv
	str w12, [_cpu, #UML_GADGET_CPU_NZCV]
carry_store:
	cmp w9, #31
	b.eq carry_done
	add x12, _cpu, #UML_GADGET_CPU_X0
	str x7, [x12, x9, lsl #3]
carry_done:
	next_gadget

/* MADD/MSUB, signed/unsigned long multiply-add, and high multiply. The
 * validated stream retains its PC operand only for consistency with decoded
 * fallback blocks. Stream: [instruction][guest PC].
 */
gadget multiply_add
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #10, #5
	ubfx w12, w8, #16, #5
	ubfx w13, w8, #21, #3
	add x14, _cpu, #UML_GADGET_CPU_X0
	cmp w10, #31
	csel x15, xzr, x14, eq
	b.eq multiply_left_ready
	ldr x15, [x14, x10, lsl #3]
multiply_left_ready:
	cmp w12, #31
	csel x16, xzr, x14, eq
	b.eq multiply_right_ready
	ldr x16, [x14, x12, lsl #3]
multiply_right_ready:
	cmp w11, #31
	csel x17, xzr, x14, eq
	b.eq multiply_addend_ready
	ldr x17, [x14, x11, lsl #3]
multiply_addend_ready:
	cmp w13, #0
	b.eq multiply_regular
	cmp w13, #1
	b.eq multiply_signed_long
	cmp w13, #5
	b.eq multiply_unsigned_long
	cmp w13, #2
	b.eq multiply_signed_high
	umulh x7, x15, x16
	b multiply_store
multiply_signed_high:
	smulh x7, x15, x16
	b multiply_store
multiply_signed_long:
	tbnz x8, #15, multiply_signed_subtract_long
	smaddl x7, w15, w16, x17
	b multiply_store
multiply_signed_subtract_long:
	smsubl x7, w15, w16, x17
	b multiply_store
multiply_unsigned_long:
	tbnz x8, #15, multiply_unsigned_subtract_long
	umaddl x7, w15, w16, x17
	b multiply_store
multiply_unsigned_subtract_long:
	umsubl x7, w15, w16, x17
	b multiply_store
multiply_regular:
	tbnz x8, #31, multiply_regular64
	tbnz x8, #15, multiply_subtract32
	madd w7, w15, w16, w17
	b multiply_store
multiply_subtract32:
	msub w7, w15, w16, w17
	b multiply_store
multiply_regular64:
	tbnz x8, #15, multiply_subtract64
	madd x7, x15, x16, x17
	b multiply_store
multiply_subtract64:
	msub x7, x15, x16, x17
multiply_store:
	cmp w9, #31
	b.eq multiply_done
	str x7, [x14, x9, lsl #3]
multiply_done:
	add _stream, _stream, #8
	next_gadget

/* LSLV/LSRV/ASRV/RORV. */
gadget variable_shift
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #10, #2
	add x13, _cpu, #UML_GADGET_CPU_X0
	cmp w10, #31
	b.eq variable_shift_zero_value
	ldr x15, [x13, x10, lsl #3]
	b variable_shift_value_ready
variable_shift_zero_value:
	mov x15, xzr
variable_shift_value_ready:
	cmp w11, #31
	b.eq variable_shift_zero_amount
	ldr x16, [x13, x11, lsl #3]
	b variable_shift_amount_ready
variable_shift_zero_amount:
	mov x16, xzr
variable_shift_amount_ready:
	tbz x8, #31, variable_shift_32
	cmp w12, #0
	b.eq variable_shift_left64
	cmp w12, #1
	b.eq variable_shift_right64
	cmp w12, #2
	b.eq variable_shift_arithmetic64
	ror x7, x15, x16
	b variable_shift_store
variable_shift_left64:
	lsl x7, x15, x16
	b variable_shift_store
variable_shift_right64:
	lsr x7, x15, x16
	b variable_shift_store
variable_shift_arithmetic64:
	asr x7, x15, x16
	b variable_shift_store
variable_shift_32:
	cmp w12, #0
	b.eq variable_shift_left32
	cmp w12, #1
	b.eq variable_shift_right32
	cmp w12, #2
	b.eq variable_shift_arithmetic32
	ror w7, w15, w16
	b variable_shift_store
variable_shift_left32:
	lsl w7, w15, w16
	b variable_shift_store
variable_shift_right32:
	lsr w7, w15, w16
	b variable_shift_store
variable_shift_arithmetic32:
	asr w7, w15, w16
variable_shift_store:
	cmp w9, #31
	b.eq variable_shift_done
	str x7, [x13, x9, lsl #3]
variable_shift_done:
	next_gadget

/* UDIV/SDIV have the same defined zero-divisor and signed-overflow behavior
 * on the guest and host A64 architectures. */
gadget divide
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	add x12, _cpu, #UML_GADGET_CPU_X0
	cmp w10, #31
	b.eq divide_zero_left
	ldr x15, [x12, x10, lsl #3]
	b divide_left_ready
divide_zero_left:
	mov x15, xzr
divide_left_ready:
	cmp w11, #31
	b.eq divide_zero_right
	ldr x16, [x12, x11, lsl #3]
	b divide_right_ready
divide_zero_right:
	mov x16, xzr
divide_right_ready:
	tbz x8, #31, divide_32
	tbnz x8, #10, divide_signed64
	udiv x7, x15, x16
	b divide_store
divide_signed64:
	sdiv x7, x15, x16
	b divide_store
divide_32:
	tbnz x8, #10, divide_signed32
	udiv w7, w15, w16
	b divide_store
divide_signed32:
	sdiv w7, w15, w16
divide_store:
	cmp w9, #31
	b.eq divide_done
	str x7, [x12, x9, lsl #3]
divide_done:
	next_gadget

/* CRC32/CRC32C map one-for-one to the host CRC extension. */
gadget crc32
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #10, #3
	add x13, _cpu, #UML_GADGET_CPU_X0
	cmp w10, #31
	b.eq 1f
	ldr x14, [x13, x10, lsl #3]
	b 2f
1:	mov w14, wzr
2:	cmp w11, #31
	b.eq 3f
	ldr x15, [x13, x11, lsl #3]
	b 4f
3:	mov x15, xzr
4:	cmp w12, #0
	b.eq 5f
	cmp w12, #1
	b.eq 6f
	cmp w12, #2
	b.eq 7f
	cmp w12, #3
	b.eq 8f
	cmp w12, #4
	b.eq 9f
	cmp w12, #5
	b.eq 10f
	cmp w12, #6
	b.eq 11f
	crc32cx w16, w14, x15
	b 12f
5:	crc32b w16, w14, w15
	b 12f
6:	crc32h w16, w14, w15
	b 12f
7:	crc32w w16, w14, w15
	b 12f
8:	crc32x w16, w14, x15
	b 12f
9:	crc32cb w16, w14, w15
	b 12f
10:	crc32ch w16, w14, w15
	b 12f
11:	crc32cw w16, w14, w15
12:	cmp w9, #31
	b.eq 13f
	str x16, [x13, x9, lsl #3]
13:	next_gadget

/* EXTR/ROR use variable shifts so one gadget covers every immediate. */
gadget extract_register
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #10, #6
	add x13, _cpu, #UML_GADGET_CPU_X0
	cmp w10, #31
	b.eq 1f
	ldr x14, [x13, x10, lsl #3]
	b 2f
1:	mov x14, xzr
2:	cmp w11, #31
	b.eq 3f
	ldr x15, [x13, x11, lsl #3]
	b 4f
3:	mov x15, xzr
4:	tbz x8, #31, 7f
	cbz w12, 5f
	lsr x16, x15, x12
	mov w17, #64
	sub w17, w17, w12
	lsl x17, x14, x17
	orr x16, x16, x17
	b 8f
5:	mov x16, x15
	b 8f
7:	cbz w12, 6f
	lsr w16, w15, w12
	mov w17, #32
	sub w17, w17, w12
	lsl w17, w14, w17
	orr w16, w16, w17
	b 8f
6:	mov w16, w15
8:	cmp w9, #31
	b.eq 9f
	str x16, [x13, x9, lsl #3]
9:	next_gadget

/* CSEL/CSINC/CSINV/CSNEG. The inline operand is the guest instruction. */
gadget conditional_select
	ldr x19, [_stream], #8
	ubfx w20, w19, #5, #5
	ubfx w21, w19, #16, #5
	and w22, w19, #0x1f
	ubfx w23, w19, #12, #4
	stp x1, x2, [sp, #-16]!
	ldr w0, [_cpu, #UML_GADGET_CPU_NZCV]
	mov w1, w23
	bl NAME(uml_arm64_gadget_condition_holds)
	ldp x1, x2, [sp], #16
	cbz w0, conditional_select_false
	cmp w20, #31
	b.eq conditional_select_zero
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x8, x20, lsl #3]
	b conditional_select_width
conditional_select_false:
	cmp w21, #31
	b.eq conditional_select_zero_false
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x8, x21, lsl #3]
	b conditional_select_transform
conditional_select_zero_false:
	mov x7, xzr
conditional_select_transform:
	tbnz x19, #30, conditional_select_invert
	tbz x19, #10, conditional_select_width
	add x7, x7, #1
	b conditional_select_width
conditional_select_invert:
	tbz x19, #10, conditional_select_complement
	neg x7, x7
	b conditional_select_width
conditional_select_complement:
	mvn x7, x7
	b conditional_select_width
conditional_select_zero:
	mov x7, xzr
conditional_select_width:
	tbnz x19, #31, conditional_select_store
	mov w7, w7
conditional_select_store:
	cmp w22, #31
	b.eq conditional_select_done
	add x8, _cpu, #UML_GADGET_CPU_X0
	str x7, [x8, x22, lsl #3]
conditional_select_done:
	next_gadget

/* CCMN/CCMP. The builder fixes the condition so the host evaluates it from
 * the guest NZCV without calling back into C++. Operand: guest instruction. */
.macro conditional_compare name, condition
gadget conditional_compare_\name
	ldr x8, [_stream], #8
	ldr w9, [_cpu, #UML_GADGET_CPU_NZCV]
	msr nzcv, x9
	b.\condition 1f
	and w9, w8, #0xf
	lsl w9, w9, #28
	str w9, [_cpu, #UML_GADGET_CPU_NZCV]
	b 6f
1:	ubfx w9, w8, #5, #5
	ubfx w10, w8, #16, #5
	add x11, _cpu, #UML_GADGET_CPU_X0
	cmp w9, #31
	b.eq 2f
	ldr x12, [x11, x9, lsl #3]
	b 3f
2:	mov x12, xzr
3:	tbz x8, #11, 4f
	mov x13, x10
	b 5f
4:	cmp w10, #31
	b.eq 7f
	ldr x13, [x11, x10, lsl #3]
	b 5f
7:	mov x13, xzr
5:	tbz x8, #31, 8f
	tbnz x8, #30, 9f
	cmn x12, x13
	b 10f
9:	cmp x12, x13
	b 10f
8:	tbnz x8, #30, 11f
	cmn w12, w13
	b 10f
11:	cmp w12, w13
10:	mrs x9, nzcv
	str w9, [_cpu, #UML_GADGET_CPU_NZCV]
6:	next_gadget
.endm

conditional_compare eq, eq
conditional_compare ne, ne
conditional_compare hs, hs
conditional_compare lo, lo
conditional_compare mi, mi
conditional_compare pl, pl
conditional_compare vs, vs
conditional_compare vc, vc
conditional_compare hi, hi
conditional_compare ls, ls
conditional_compare ge, ge
conditional_compare lt, lt
conditional_compare gt, gt
conditional_compare le, le
conditional_compare always, al

/* AND/BIC/ORR/ORN/EOR/EON/ANDS/BICS (shifted register). */
gadget logical_shifted
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #16, #5
	ubfx w12, w8, #31, #1
	ubfx w13, w8, #29, #2

	cmp w10, #31
	b.eq logical_zero_rn
	add x14, _cpu, #UML_GADGET_CPU_X0
	ldr x15, [x14, x10, lsl #3]
	b logical_rn_ready
logical_zero_rn:
	mov x15, xzr
logical_rn_ready:
	cmp w11, #31
	b.eq logical_zero_rm
	add x14, _cpu, #UML_GADGET_CPU_X0
	ldr x16, [x14, x11, lsl #3]
	b logical_rm_ready
logical_zero_rm:
	mov x16, xzr
logical_rm_ready:
	ubfx w17, w8, #22, #2
	ubfx w6, w8, #10, #6
	cbz w6, logical_shift_ready
	cbz w12, logical_shift32
	cmp w17, #0
	b.eq logical_lsl64
	cmp w17, #1
	b.eq logical_lsr64
	cmp w17, #2
	b.eq logical_asr64
	ror x16, x16, x6
	b logical_shift_ready
logical_lsl64:
	lsl x16, x16, x6
	b logical_shift_ready
logical_lsr64:
	lsr x16, x16, x6
	b logical_shift_ready
logical_asr64:
	asr x16, x16, x6
	b logical_shift_ready
logical_shift32:
	cmp w17, #0
	b.eq logical_lsl32
	cmp w17, #1
	b.eq logical_lsr32
	cmp w17, #2
	b.eq logical_asr32
	ror w16, w16, w6
	b logical_shift_ready
logical_lsl32:
	lsl w16, w16, w6
	b logical_shift_ready
logical_lsr32:
	lsr w16, w16, w6
	b logical_shift_ready
logical_asr32:
	asr w16, w16, w6

logical_shift_ready:
	tbz x8, #21, logical_operand_ready
	mvn x16, x16
logical_operand_ready:
	cbz w12, logical_operation32
	cmp w13, #1
	b.eq logical_orr64
	cmp w13, #2
	b.eq logical_eor64
	cmp w13, #3
	b.eq logical_ands64
	and x7, x15, x16
	b logical_store
logical_orr64:
	orr x7, x15, x16
	b logical_store
logical_eor64:
	eor x7, x15, x16
	b logical_store
logical_ands64:
	ands x7, x15, x16
	b logical_save_flags

logical_operation32:
	cmp w13, #1
	b.eq logical_orr32
	cmp w13, #2
	b.eq logical_eor32
	cmp w13, #3
	b.eq logical_ands32
	and w7, w15, w16
	b logical_store
logical_orr32:
	orr w7, w15, w16
	b logical_store
logical_eor32:
	eor w7, w15, w16
	b logical_store
logical_ands32:
	ands w7, w15, w16

logical_save_flags:
	mrs x14, nzcv
	str w14, [_cpu, #UML_GADGET_CPU_NZCV]
logical_store:
	cmp w9, #31
	b.eq logical_done
	add x14, _cpu, #UML_GADGET_CPU_X0
	str x7, [x14, x9, lsl #3]
logical_done:
	next_gadget

/* Logical (immediate). The second operand was decoded by the C++ builder,
 * avoiding any runtime instruction generation while keeping this hot path
 * entirely in precompiled assembly.
 */
gadget logical_immediate
	ldr x8, [_stream], #8
	ldr x14, [_stream], #8
	and x9, x8, #0x1f
	ubfx x10, x8, #5, #5
	ubfx x11, x8, #29, #2
	cmp x10, #31
	b.eq 1f
	add x12, _cpu, #UML_GADGET_CPU_X0
	ldr x15, [x12, x10, lsl #3]
	b 2f
1:	mov x15, xzr
2:	cmp x11, #1
	b.eq 4f
	cmp x11, #2
	b.eq 5f
	cmp x11, #3
	b.eq 6f
	and x16, x15, x14
	b 7f
4:	orr x16, x15, x14
	b 7f
5:	eor x16, x15, x14
	b 7f
6:	tbz x8, #31, 10f
	ands x16, x15, x14
	b 11f
10:	ands w16, w15, w14
11:	mrs x17, nzcv
	str w17, [_cpu, #UML_GADGET_CPU_NZCV]
7:	tbnz x8, #31, 8f
	mov w16, w16
8:	cmp x9, #31
	b.ne 12f
	cmp x11, #3
	b.eq 9f
	str x16, [_cpu, #UML_GADGET_CPU_SP]
	b 9f
12:	add x12, _cpu, #UML_GADGET_CPU_X0
	str x16, [x12, x9, lsl #3]
9:	next_gadget

/* SBFM/BFM/UBFM. The builder predecodes the architectural write and test
 * masks, leaving only the variable rotate and merge in the hot path.
 * Stream: [instruction][write mask][test mask].
 */
gadget bitfield
	ldr x8, [_stream], #8
	ldr x14, [_stream], #8
	ldr x17, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #10, #6
	ubfx w12, w8, #16, #6
	ubfx w13, w8, #29, #2
	cmp w10, #31
	b.eq bitfield_zero_source
	add x15, _cpu, #UML_GADGET_CPU_X0
	ldr x15, [x15, x10, lsl #3]
	b bitfield_source_ready
bitfield_zero_source:
	mov x15, xzr
bitfield_source_ready:
	tbnz x8, #31, bitfield_rotate64
	ror w16, w15, w12
	mov w16, w16
	mov w15, w15
	b bitfield_rotated
bitfield_rotate64:
	ror x16, x15, x12
bitfield_rotated:
	and x16, x16, x14
	cmp w13, #1
	b.eq bitfield_merge
	cmp w13, #0
	b.ne bitfield_blend
	/* Sign-fill the bits selected from the top value. */
	lsr x6, x15, x11
	tst x6, #1
	csetm x6, ne
	b bitfield_blend_with_top
bitfield_merge:
	cmp w9, #31
	b.eq bitfield_zero_destination
	add x6, _cpu, #UML_GADGET_CPU_X0
	ldr x6, [x6, x9, lsl #3]
	b bitfield_destination_ready
bitfield_zero_destination:
	mov x6, xzr
bitfield_destination_ready:
	bic x7, x6, x14
	orr x16, x16, x7
	b bitfield_blend_with_top
bitfield_blend:
	mov x6, xzr
bitfield_blend_with_top:
	bic x6, x6, x17
	and x16, x16, x17
	orr x16, x16, x6
	tbnz x8, #31, bitfield_store
	mov w16, w16
bitfield_store:
	cmp w9, #31
	b.eq bitfield_done
	add x7, _cpu, #UML_GADGET_CPU_X0
	str x16, [x7, x9, lsl #3]
bitfield_done:
	next_gadget

/* RBIT/REV16/REV32/REV/CLZ/CLS map directly to native A64 operations. */
gadget bit_reverse_count
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #5, #5
	ubfx w11, w8, #10, #5
	cmp w10, #31
	b.eq bit_reverse_zero_source
	add x12, _cpu, #UML_GADGET_CPU_X0
	ldr x12, [x12, x10, lsl #3]
	b bit_reverse_source_ready
bit_reverse_zero_source:
	mov x12, xzr
bit_reverse_source_ready:
	tbz x8, #31, bit_reverse_32
	cmp w11, #0
	b.eq bit_reverse_rbit64
	cmp w11, #1
	b.eq bit_reverse_rev16_64
	cmp w11, #2
	b.eq bit_reverse_rev32_64
	cmp w11, #3
	b.eq bit_reverse_rev64
	cmp w11, #4
	b.eq bit_reverse_clz64
	cls x13, x12
	b bit_reverse_store
bit_reverse_rbit64:
	rbit x13, x12
	b bit_reverse_store
bit_reverse_rev16_64:
	rev16 x13, x12
	b bit_reverse_store
bit_reverse_rev32_64:
	rev32 x13, x12
	b bit_reverse_store
bit_reverse_rev64:
	rev x13, x12
	b bit_reverse_store
bit_reverse_clz64:
	clz x13, x12
	b bit_reverse_store
bit_reverse_32:
	cmp w11, #0
	b.eq bit_reverse_rbit32
	cmp w11, #1
	b.eq bit_reverse_rev16_32
	cmp w11, #2
	b.eq bit_reverse_rev32
	cmp w11, #4
	b.eq bit_reverse_clz32
	cls w13, w12
	b bit_reverse_store
bit_reverse_rbit32:
	rbit w13, w12
	b bit_reverse_store
bit_reverse_rev16_32:
	rev16 w13, w12
	b bit_reverse_store
bit_reverse_rev32:
	rev w13, w12
	b bit_reverse_store
bit_reverse_clz32:
	clz w13, w12
bit_reverse_store:
	cmp w9, #31
	b.eq bit_reverse_done
	add x14, _cpu, #UML_GADGET_CPU_X0
	str x13, [x14, x9, lsl #3]
bit_reverse_done:
	next_gadget
