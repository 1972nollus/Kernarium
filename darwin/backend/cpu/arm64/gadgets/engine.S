/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Derived from iSH Asbestos' ARM64 guest gadgets. Runtime blocks contain only
 * pointers to these precompiled routines and inline integer operands.
 */
#include "offsets.inc"

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

.text
.globl NAME(uml_arm64_gadget_enter)
.p2align 4
NAME(uml_arm64_gadget_enter):
	/* x0 = data-only program stream, x1 = CpuState. */
	stp x18, x19, [sp, #-0x70]!
	stp x20, x21, [sp, #0x10]
	stp x22, x23, [sp, #0x20]
	stp x24, x25, [sp, #0x30]
	stp x26, x27, [sp, #0x40]
	stp x28, x29, [sp, #0x50]
	str lr, [sp, #0x60]
	mov _stream, x0
	next_gadget

.p2align 4
uml_arm64_gadget_leave:
	ldr lr, [sp, #0x60]
	ldp x28, x29, [sp, #0x50]
	ldp x26, x27, [sp, #0x40]
	ldp x24, x25, [sp, #0x30]
	ldp x22, x23, [sp, #0x20]
	ldp x20, x21, [sp, #0x10]
	ldp x18, x19, [sp], #0x70
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
6:	ands x16, x15, x14
	mrs x17, nzcv
	str w17, [_cpu, #UML_GADGET_CPU_NZCV]
7:	tbnz x8, #31, 8f
	mov w16, w16
8:	cmp x9, #31
	b.eq 9f
	add x12, _cpu, #UML_GADGET_CPU_X0
	str x16, [x12, x9, lsl #3]
9:	next_gadget

gadget pc_relative
	ldr x8, [_stream], #8
	ldr x9, [_stream], #8
	cmp x8, #31
	b.eq 1f
	add x10, _cpu, #UML_GADGET_CPU_X0
	str x9, [x10, x8, lsl #3]
1:	next_gadget

gadget tls_read
	ldr x8, [_stream], #8
	cmp x8, #31
	b.eq 1f
	ldr x9, [_cpu, #UML_GADGET_CPU_TLS]
	add x10, _cpu, #UML_GADGET_CPU_X0
	str x9, [x10, x8, lsl #3]
1:	next_gadget

gadget tls_write
	ldr x8, [_stream], #8
	cmp x8, #31
	b.eq 1f
	add x10, _cpu, #UML_GADGET_CPU_X0
	ldr x9, [x10, x8, lsl #3]
	str x9, [_cpu, #UML_GADGET_CPU_TLS]
	b 2f
1:	str xzr, [_cpu, #UML_GADGET_CPU_TLS]
2:	next_gadget

/* AdvSIMD DUP (general), ported from iSH's ARM64 gadget implementation. */
gadget dup_gpr_to_vector
	ldr x8, [_stream], #8
	and x9, x8, #0x1f
	ubfx x10, x8, #8, #5
	ubfx x11, x8, #16, #2
	ubfx x17, x8, #24, #1
	cmp x10, #31
	b.eq 1f
	add x12, _cpu, #UML_GADGET_CPU_X0
	ldr x13, [x12, x10, lsl #3]
	b 2f
1:	mov x13, xzr
2:	add x15, _cpu, #UML_GADGET_CPU_V0
	add x15, x15, x9, lsl #4
	cmp x11, #0
	b.eq 3f
	cmp x11, #1
	b.eq 4f
	cmp x11, #2
	b.eq 5f
	b 6f
3:	and x13, x13, #0xff
	mov x14, #0x0101010101010101
	mul x14, x13, x14
	b 7f
4:	and x13, x13, #0xffff
	mov x14, #0x0001000100010001
	mul x14, x13, x14
	b 7f
5:	and x13, x13, #0xffffffff
	orr x14, x13, x13, lsl #32
	b 7f
6:	mov x14, x13
7:	str x14, [x15]
	cbz x17, 8f
	str x14, [x15, #8]
	b 9f
8:	str xzr, [x15, #8]
9:	next_gadget

gadget nop
	next_gadget

.macro load_base_and_address
	ldr x8, [_stream], #8
	and x19, x8, #0x1f
	ubfx x20, x8, #8, #5
	ubfx x21, x8, #16, #32
	cmp x20, #31
	b.eq 90f
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x9, x20, lsl #3]
	b 91f
90:	ldr x7, [_cpu, #UML_GADGET_CPU_SP]
91:	add x7, x7, x21
.endm

/* The direct host load/store gadgets may be used only when the complete
 * access stays within one translated 4 KiB guest page.  The stream following
 * the address operand is [instruction][guest PC], which is exactly the input
 * expected by the generic decoded gadget on the cross-page slow path.
 */
.macro select_memory_path last_offset
	and x9, x7, #0xfff
	cmp x9, #\last_offset
	b.hi NAME(uml_arm64_gadget_decoded)
	/* Skip the instruction while leaving the guest PC at _stream for the
	 * shared memory-fault handler.
	 */
	add _stream, _stream, #8
.endm

gadget load32_unsigned
	load_base_and_address
	select_memory_path 4092
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	ldr w0, [x7]
	cmp x19, #31
	b.eq 1f
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x0, [x9, x19, lsl #3]
1:	add _stream, _stream, #8
	next_gadget

gadget load64_unsigned
	load_base_and_address
	select_memory_path 4088
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	ldr x0, [x7]
	cmp x19, #31
	b.eq 1f
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x0, [x9, x19, lsl #3]
1:	add _stream, _stream, #8
	next_gadget

gadget store32_unsigned
	load_base_and_address
	select_memory_path 4092
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	cmp x19, #31
	b.eq 1f
	add x9, _cpu, #UML_GADGET_CPU_X0
	add x9, x9, x19, lsl #3
	ldr w0, [x9]
	b 2f
1:	mov w0, wzr
2:	str w0, [x7]
	add _stream, _stream, #8
	next_gadget

gadget store64_unsigned
	load_base_and_address
	select_memory_path 4088
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	cmp x19, #31
	b.eq 1f
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x0, [x9, x19, lsl #3]
	b 2f
1:	mov x0, xzr
2:	str x0, [x7]
	add _stream, _stream, #8
	next_gadget

/* Integer load/store instructions using an unscaled immediate, pre/post
 * indexing, or a register offset. Stream: [instruction][guest PC]. Keeping
 * the original instruction at _stream until the complete access has passed
 * the single-page check lets the generic decoder handle rare cross-page
 * accesses without reconstructing its stream operands.
 */
gadget load_store_integer_other
	ldr x8, [_stream]
	and w19, w8, #0x1f
	ubfx w20, w8, #5, #5
	ubfx w21, w8, #30, #2
	ubfx w22, w8, #22, #2

	cmp w20, #31
	b.eq integer_other_load_sp
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x24, [x9, x20, lsl #3]
	b integer_other_base_ready
integer_other_load_sp:
	ldr x24, [_cpu, #UML_GADGET_CPU_SP]
integer_other_base_ready:
	tbz x8, #21, integer_other_immediate

	/* Register offset: UXTW/LSL/SXTW/SXTX, optionally scaled by the
	 * transfer size. Builder validation keeps other option values on the
	 * decoded path.
	 */
	mov w23, wzr
	ubfx w6, w8, #16, #5
	cmp w6, #31
	b.eq integer_other_zero_offset
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x26, [x9, x6, lsl #3]
	b integer_other_offset_loaded
integer_other_zero_offset:
	mov x26, xzr
integer_other_offset_loaded:
	ubfx w6, w8, #13, #3
	cmp w6, #2
	b.eq integer_other_uxtw
	cmp w6, #6
	b.eq integer_other_sxtw
	b integer_other_offset_extended
integer_other_uxtw:
	and x26, x26, #0xffffffff
	b integer_other_offset_extended
integer_other_sxtw:
	sxtw x26, w26
integer_other_offset_extended:
	tbz x8, #12, integer_other_offset_ready
	lsl x26, x26, x21
integer_other_offset_ready:
	add x26, x24, x26
	b integer_other_address_ready

integer_other_immediate:
	sbfx x6, x8, #12, #9
	add x25, x24, x6
	ubfx w23, w8, #10, #2
	cmp w23, #1
	csel x26, x24, x25, eq

integer_other_address_ready:
	/* Host pages need not be adjacent even when guest pages are. */
	mov x27, #1
	lsl x27, x27, x21
	and x9, x26, #0xfff
	add x9, x9, x27
	cmp x9, #4096
	b.hi NAME(uml_arm64_gadget_decoded)
	/* Consume the instruction, leaving the guest PC for a fault. */
	add _stream, _stream, #8
	cbz w22, integer_other_store

	mov x7, x26
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	cmp w21, #0
	b.eq integer_other_load8
	cmp w21, #1
	b.eq integer_other_load16
	cmp w21, #2
	b.eq integer_other_load32
	ldr x0, [x7]
	b integer_other_load_result
integer_other_load8:
	ldrb w0, [x7]
	cmp w22, #2
	b.eq integer_other_sign8_x
	cmp w22, #3
	b.ne integer_other_load_result
	sxtb w0, w0
	b integer_other_load_result
integer_other_sign8_x:
	sxtb x0, w0
	b integer_other_load_result
integer_other_load16:
	ldrh w0, [x7]
	cmp w22, #2
	b.eq integer_other_sign16_x
	cmp w22, #3
	b.ne integer_other_load_result
	sxth w0, w0
	b integer_other_load_result
integer_other_sign16_x:
	sxth x0, w0
	b integer_other_load_result
integer_other_load32:
	ldr w0, [x7]
	cmp w22, #2
	b.ne integer_other_load_result
	sxtw x0, w0
integer_other_load_result:
	cmp w19, #31
	b.eq integer_other_writeback
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x0, [x9, x19, lsl #3]
	b integer_other_writeback

integer_other_store:
	mov x7, x26
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	cmp w19, #31
	b.eq integer_other_zero_store
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x0, [x9, x19, lsl #3]
	b integer_other_store_value_ready
integer_other_zero_store:
	mov x0, xzr
integer_other_store_value_ready:
	cmp w21, #0
	b.eq integer_other_store8
	cmp w21, #1
	b.eq integer_other_store16
	cmp w21, #2
	b.eq integer_other_store32
	str x0, [x7]
	b integer_other_writeback
integer_other_store8:
	strb w0, [x7]
	b integer_other_writeback
integer_other_store16:
	strh w0, [x7]
	b integer_other_writeback
integer_other_store32:
	str w0, [x7]

integer_other_writeback:
	cmp w23, #1
	b.eq integer_other_do_writeback
	cmp w23, #3
	b.ne integer_other_done
integer_other_do_writeback:
	cmp w20, #31
	b.eq integer_other_write_sp
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x25, [x9, x20, lsl #3]
	b integer_other_done
integer_other_write_sp:
	str x25, [_cpu, #UML_GADGET_CPU_SP]
integer_other_done:
	add _stream, _stream, #8
	next_gadget

/* Integer LDP/STP, including signed-offset, pre-index, and post-index forms.
 * Stream: [instruction][guest PC]. Both addresses are translated before an
 * STP changes memory, preserving restartability across a 4 KiB boundary.
 */
gadget load_store_pair
	ldr x8, [_stream]
	and w22, w8, #0x1f
	ubfx w23, w8, #10, #5
	ubfx w24, w8, #5, #5
	ubfx w25, w8, #30, #2
	ubfx w26, w8, #22, #1
	ubfx w27, w8, #23, #2
	sbfx x6, x8, #15, #7
	cmp w25, #2
	cset w5, eq
	add w5, w5, #2
	lsl x6, x6, x5
	cmp w24, #31
	b.eq pair_load_sp
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x16, [x9, x24, lsl #3]
	b pair_base_ready
pair_load_sp:
	ldr x16, [_cpu, #UML_GADGET_CPU_SP]
pair_base_ready:
	add x21, x16, x6
	cmp w27, #1
	csel x19, x16, x21, eq
	mov x5, #4
	mov x9, #8
	cmp w25, #2
	csel x5, x5, x9, ne
	/* Each element must fit within one guest page because adjacent guest
	 * pages are not required to have adjacent host mappings. Keep the
	 * instruction unconsumed so the generic decoder can handle either
	 * uncommon cross-page element.
	 */
	and x9, x19, #0xfff
	add x9, x9, x5
	cmp x9, #4096
	b.hi NAME(uml_arm64_gadget_decoded)
	adds x20, x19, x5
	b.cs NAME(uml_arm64_gadget_decoded)
	and x9, x20, #0xfff
	add x9, x9, x5
	cmp x9, #4096
	b.hi NAME(uml_arm64_gadget_decoded)
	/* Consume the instruction, leaving the guest PC for a fault. */
	add _stream, _stream, #8
	cbz w26, pair_translate_store

	mov x7, x19
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	mov x19, x7
	mov x7, x20
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	mov x20, x7
	cmp w25, #2
	b.eq pair_load64
	ldr w6, [x19]
	ldr w7, [x20]
	b pair_store_loaded
pair_load64:
	ldr x6, [x19]
	ldr x7, [x20]
pair_store_loaded:
	add x8, _cpu, #UML_GADGET_CPU_X0
	cmp w22, #31
	b.eq pair_skip_first_load
	str x6, [x8, x22, lsl #3]
pair_skip_first_load:
	cmp w23, #31
	b.eq pair_writeback
	str x7, [x8, x23, lsl #3]
	b pair_writeback

pair_translate_store:
	mov x7, x19
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	mov x19, x7
	mov x7, x20
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	mov x20, x7
	add x8, _cpu, #UML_GADGET_CPU_X0
	cmp w22, #31
	b.eq pair_zero_first_store
	ldr x6, [x8, x22, lsl #3]
	b pair_first_store_ready
pair_zero_first_store:
	mov x6, xzr
pair_first_store_ready:
	cmp w23, #31
	b.eq pair_zero_second_store
	ldr x7, [x8, x23, lsl #3]
	b pair_second_store_ready
pair_zero_second_store:
	mov x7, xzr
pair_second_store_ready:
	cmp w25, #2
	b.eq pair_store64
	str w6, [x19]
	str w7, [x20]
	b pair_writeback
pair_store64:
	str x6, [x19]
	str x7, [x20]

pair_writeback:
	cmp w27, #1
	b.eq pair_do_writeback
	cmp w27, #3
	b.ne pair_done
pair_do_writeback:
	cmp w24, #31
	b.eq pair_write_sp
	add x8, _cpu, #UML_GADGET_CPU_X0
	str x21, [x8, x24, lsl #3]
	b pair_done
pair_write_sp:
	str x21, [_cpu, #UML_GADGET_CPU_SP]
pair_done:
	add _stream, _stream, #8
	next_gadget

uml_arm64_gadget_memory_fault:
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_MEMORY_FAULT
	b uml_arm64_gadget_leave

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

/* Execute an instruction through the precompiled C++ decoder. This remains a
 * static gadget: the threaded program contains only its address and the guest
 * instruction word, never generated host instructions.
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
