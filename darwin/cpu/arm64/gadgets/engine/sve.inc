/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The SVE part of the gadget region: the non-streaming SVE forms, which no
 * supported host executes, lowered onto AdvSIMD and general registers at
 * the virtual CPU's fixed vector length of 16 bytes
 * (cpu/arm64/sve_profile.hpp). Only a program translated for SVCR.SM = 0
 * selects these entries; streaming mode runs the SVE forms natively from
 * the SME part, and its streaming-safety walk ends at the SME part's end.
 * Nothing here is pushed into another part, so no entry of the parts before
 * it moves or changes for it, and the assembler accepts no SVE or SME
 * instruction here: sme.inc has turned those extensions off.
 *
 * State: Z_n is CpuState::vectors[n], so it is V_n; P0-P15 and FFR are the
 * mask images CpuState keeps, 0xff in each active byte and 0x00 elsewhere,
 * and every gadget here writes them in that form. A program holding an SVE
 * word accesses the FP state, which carries P and FFR, before it runs: the
 * builder marks it so, and its link entry is sve_link_entry below, which
 * also records that the task has executed SVE (CpuState::sve_live). */
#include "../sve_layout.h"

.pushsection GADGET_SECTION_SVE

/* sme.inc named its gadgets for the SME part; these are ordinary entries. */
.purgem gadget
.macro gadget name
	.globl NAME(uml_arm64_gadget_\name)
	.p2align 4
NAME(uml_arm64_gadget_\name):
.endm

/* The link entry of a block holding an SVE word: fp_link_entry's FP state
 * access, which brings P and FFR with the vectors, and the live marker. */
gadget sve_link_entry
	ldrb w8, [_cpu, #UML_GADGET_CPU_FP_LOADED]
	mov w9, #1
	strb w9, [_cpu, #UML_ARM64_CPU_SVE_LIVE]
	cbz w8, 97f
	strb w9, [_cpu, #UML_GADGET_CPU_FP_DIRTY]
98:
	next_gadget
97:
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	helper_call uml_arm64_gadget_access_fp_state
	ldp x1, x2, [sp], #16
	b 98b

/* An SVE word no generated gadget runs, or that a generated gadget leaves
 * off its fast path: the semantic library runs it on the register file
 * (uml_arm64_gadget_sve_execute). Stream: [word | row << 32][PC]. The
 * guest's NZCV, kept in x27, and its cumulative FPSR flags, kept in the
 * host register while the native FP environment is active, go to CpuState
 * before the call and come back after it. A fault leaves through the
 * memory-fault exit with the fault in the TLB, an UNDEFINED word (a
 * reserved bitmask immediate) through the illegal-instruction exit, each
 * with _stream at the PC word; an internal error is the fatal event. */
gadget sve_library
	ldr x8, [_stream], #8
	ldrb w6, [_cpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbz w6, 1f
	mrs x16, fpsr
	str w16, [_cpu, #UML_GADGET_CPU_FPSR]
1:
	str w27, [_cpu, #UML_GADGET_CPU_NZCV]
	mov x19, x1
	mov x20, x2
	mov x0, x1
	mov x1, x2
	mov x2, x8
	helper_call uml_arm64_gadget_sve_execute
	mov x1, x19
	mov x2, x20
	ldr w27, [_cpu, #UML_GADGET_CPU_NZCV]
	ldrb w6, [_cpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbz w6, 2f
	ldr w16, [_cpu, #UML_GADGET_CPU_FPSR]
	msr fpsr, x16
2:
	cbnz w0, 3f
	add _stream, _stream, #8
	next_gadget
3:
	cmp w0, #UML_GADGET_SVE_LIBRARY_FAULT
	b.ne 4f
	b uml_arm64_gadget_memory_fault
4:
	ldr x8, [_stream]
	str _stream, [_cpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	cmp w0, #UML_GADGET_SVE_LIBRARY_UNDEFINED
	mov w0, #UML_GADGET_EVENT_ILLEGAL
	/* Otherwise EngineEvent::fatal. */
	mov w9, #-1
	csel w0, w0, w9, eq
	b uml_arm64_gadget_leave

/* v\reg := the element pattern of \esize bytes, 0xff in each element's
 * lowest byte: the bits of a predicate a consumer of that size reads. */
.macro sve_element_pattern esize, reg
	.if \esize == 2
	movi v\reg\().8h, #0xff
	.elseif \esize == 4
	movi v\reg\().4s, #0xff
	.elseif \esize == 8
	movi v\reg\().2d, #0xff
	.else
	.error "sve_element_pattern: element size \esize"
	.endif
.endm

/* v\mask, a predicate, becomes the lane mask of its active elements of
 * \esize bytes (2, 4 or 8): all ones in each element whose lowest byte is
 * active. Clobbers v\scratch. A byte-element consumer uses the predicate
 * itself. */
.macro sve_lane_mask esize, mask, scratch
	sve_element_pattern \esize, \scratch
	.if \esize == 2
	cmtst v\mask\().8h, v\mask\().8h, v\scratch\().8h
	.elseif \esize == 4
	cmtst v\mask\().4s, v\mask\().4s, v\scratch\().4s
	.else
	cmtst v\mask\().2d, v\mask\().2d, v\scratch\().2d
	.endif
.endm

/* x27 := NZCV as PredTest(mask, result, 8) leaves it, for the mask in
 * x9:x10 and the result in x11:x12 (low:high): N, the result's byte at the
 * mask's first active byte is set; Z, no active byte of the result is set;
 * C, the result's byte at the mask's last active byte is clear, which it
 * also is without active bytes; V clear. Every byte is 0x00 or 0xff, so
 * the lowest set bit of the mask is bit 0 of its first active byte and the
 * highest is bit 7 of its last. Clobbers x11-x17. */
.macro sve_predicate_test
	and x11, x11, x9
	and x12, x12, x10
	neg x13, x9
	and x13, x13, x9
	neg x14, x10
	and x14, x14, x10
	and x13, x13, x11
	and x14, x14, x12
	cmp x9, #0
	csel x13, x13, x14, ne
	clz x15, x10
	lsl x16, x12, x15
	clz x15, x9
	lsl x17, x11, x15
	cmp x10, #0
	csel x16, x16, x17, ne
	orr x15, x11, x12
	cmp x13, #0
	cset w13, ne
	cmp x15, #0
	cset w15, eq
	mvn x16, x16
	lsr x16, x16, #63
	lsl w27, w13, #31
	orr w27, w27, w15, lsl #30
	orr w27, w27, w16, lsl #29
.endm

/* control.inc's follow_block_link, for a branch fused into an SVE gadget
 * whose fall-through is translated inline behind it. */
.macro sve_follow_block_link link
	ldp _stream, x11, [\link]
	add x29, x29, x11
	tbnz x29, #63, 98f
	ldr x9, [\link, #16]
	str x9, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
98:
	next_gadget
.endm

#include "sve_generated.inc"

.purgem sve_follow_block_link
.purgem sve_predicate_test
.purgem sve_lane_mask
.purgem sve_element_pattern

.popsection
