/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../layout/assembly_offsets.h"

.arch armv8.2-a

#ifndef UML_X86_64_STRICT_TSO
#define UML_X86_64_STRICT_TSO 0
#endif

#if __APPLE__
#define NAME(value) _##value
#else
#define NAME(value) value
#endif

_cpu .req x20
_context .req x21
_stream .req x28
_executed .req x29

.macro gadget name
	.globl NAME(uml_x86_64_gadget_\name)
	.p2align 4
NAME(uml_x86_64_gadget_\name):
.endm

.macro next_gadget
	ldr x8, [_stream], #8
	br x8
.endm

.macro x86_memory_order_boundary
#if UML_X86_64_STRICT_TSO
	/* A full boundary is intentionally conservative. It is stronger than
	 * x86 TSO and covers every ordinary, vector, string and cross-page memory
	 * ordering path without classifying each gadget as a load or store. */
	dmb ish
#endif
.endm

.macro retire_and_next
	add _executed, _executed, #1
	x86_memory_order_boundary
	next_gadget
.endm

/* Bytes 24..31 of the record hold operation, width, carry, one padding byte,
 * and the two override halves. Both varying fields are macro parameters, so
 * that whole tail is a single link-time constant and the record costs three
 * stores instead of six. Consumers that need a carry or an override write it
 * after this macro.
 */
.macro record_deferred operation, width
	add x15, _cpu, #UML_X86_64_CPU_DEFERRED_LEFT
	stp x11, x12, [x15]
	mov x14, #((\operation) | ((\width / 8) << 8))
	stp x13, x14, [x15, #16]
.endm

/* The same record from any three registers. */
.macro record_deferred_from operation, width, left, right, result
	add x15, _cpu, #UML_X86_64_CPU_DEFERRED_LEFT
	stp \left, \right, [x15]
	mov x14, #((\operation) | ((\width / 8) << 8))
	stp \result, x14, [x15, #16]
.endm

/* A guest access no wider than one byte lies inside one page by construction,
 * so the boundary test folds away entirely at assembly time. `size` is the
 * access width in bytes and must be an absolute expression.
 */
.macro page_cross_check size, label
	.if (\size) > 1
	add x9, x7, #((\size) - 1)
	eor x9, x9, x7
	tbnz x9, #12, \label
	.endif
.endm

/* A host W-form instruction writes the whole 64-bit register, so a result
 * computed at 32 or 64 bits already is the guest value. Only the narrow widths
 * still need masking. Sites whose producer is a narrow load or the zero-extending
 * cross-page helper need nothing at all and call neither macro.
 */
.macro normalize_result width, register
	.if \width == 8
	and \register, \register, #0xff
	.elseif \width == 16
	uxth \register, \register
	.endif
.endm

/* Stream descriptor:
 *   15:0   value-register byte offset
 *   31:16  base-register byte offset or 0xffff
 *   47:32  index-register byte offset or 0xffff
 *   49:48  index scale shift
 *   51:50  segment (0 none, 1 FS, 2 GS)
 *   52     32-bit effective-address arithmetic
 *   53     16-bit effective-address arithmetic
 *   59:56  instruction length
 *   60     a write translation proved this store publishes nothing:
 *          its page holds no cached code and no publication is pending
 * Followed by the folded displacement/absolute address and current RIP.
 */
.macro effective_address
	ldp x23, x14, [_stream], #16
	ldr x22, [_stream], #8
	ubfx x9, x23, #16, #16
	ubfx x11, x23, #32, #16
	ubfx x12, x23, #48, #2
	ldr x10, [_cpu, x9]
	ldr x13, [_cpu, x11]
	lsl x13, x13, x12
	add x7, x14, x10
	add x7, x7, x13
	/* Truncation distributes over addition and shifting, so masking the sum
	 * once is equivalent to masking every component and the sum.
	 */
	tbnz x23, #53, .Lx86_address_16\@
	tbz x23, #52, .Lx86_address_ready\@
	mov w7, w7
	b .Lx86_address_ready\@
.Lx86_address_16\@:
	uxth w7, w7
.Lx86_address_ready\@:
	/* The selector indexes {segment_zero, fs_base, gs_base}, so no segment is
	 * an addition of zero rather than a pair of branches.
	 */
	ubfx x9, x23, #50, #2
	add x11, _cpu, x9, lsl #3
	ldr x10, [x11, #UML_X86_64_CPU_SEGMENT_ZERO]
	add x7, x7, x10
	/* Legacy protected mode has a 32-bit linear-address space. Linux's i386
	 * TLS sequences rely on a high FS/GS base plus an effective offset
	 * wrapping through zero; long mode keeps its full-width FS/GS base. The
	 * wrap follows the selector, not the base value, so a zero FS base in
	 * compat32 still wraps exactly as before.
	 */
	cbz x9, .Lx86_segment_ready\@
	ldrb w11, [_cpu, #UML_X86_64_CPU_EXECUTION_MODE]
	cmp w11, #1
	b.ne .Lx86_segment_ready\@
	mov w7, w7
.Lx86_segment_ready\@:
.endm

.macro translate access, entry_offset, size
	.if \access == 1
	/* Only a translation that reaches the decision below may skip the store's
	 * publication; the miss and page-crossing paths leave this clear.
	 */
	and x23, x23, #~(1 << 60)
	.endif
	and x9, x7, #0xfffffffffffff000
	lsr x10, x7, #12
	eor x10, x10, x10, lsr #13
	and w10, w10, #0x1fff
	ldr x11, [_context, #UML_X86_64_CONTEXT_TLB]
	add x11, x11, x10, lsl #5
	ldp x12, x13, [x11, #\entry_offset]
	.if \access == 1
	bic x14, x12, #(UML_X86_64_TLB_WRITE_CODE | UML_X86_64_TLB_WRITE_CACHED_CODE)
	cmp x9, x14
	.else
	cmp x9, x12
	.endif
	b.eq .Lx86_tlb_hit\@
	mov x0, _context
	mov x1, x7
	mov w2, #\access
	mov w3, #\size
	bl NAME(uml_x86_64_gadget_tlb_miss)
	cbz x0, .Lx86_memory_fault
	mov x7, x0
	b .Lx86_tlb_done\@
.Lx86_tlb_hit\@:
	.if \access == 1
	tbz x12, #1, .Lx86_tlb_write_code_ready\@
	ldrb w14, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cmp w14, #UML_X86_64_WRITE_CODE_PUBLISHED
	b.eq .Lx86_tlb_write_code_ready\@
	mov w14, #UML_X86_64_WRITE_CODE_PENDING
	strb w14, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
.Lx86_tlb_write_code_ready\@:
	/* Reading the context after the guest store costs a memory-disambiguation
	 * stall, and calling out to publication costs far more. Publication has
	 * work only when a publish is still pending or this page holds cached
	 * code, and neither depends on what the store writes, so settle it here.
	 */
	tbnz x12, #2, .Lx86_store_publishes\@
	ldrb w14, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cmp w14, #UML_X86_64_WRITE_CODE_PENDING
	b.eq .Lx86_store_publishes\@
	orr x23, x23, #(1 << 60)
.Lx86_store_publishes\@:
	.endif
	add x7, x13, x7
.Lx86_tlb_done\@:
.endm

/* x86 LOCK-prefixed read-modify-write.
 *
 * A locked gadget must be atomic against the other vCPU threads that execute
 * guest code concurrently, so it performs its update with an ARM64 exclusive
 * loop instead of a plain load and store.  Exclusive access requires natural
 * alignment; x86 permits an unaligned locked operand, so a locked gadget
 * routes an unaligned address to its ordinary non-atomic sequence.  Natural
 * alignment also keeps the operand inside one page, which is why the exclusive
 * path needs no page-crossing check.
 *
 * Nothing may touch memory between the exclusive load and store or the monitor
 * clears and the loop cannot make progress, so operands are read into
 * registers before the loop and flags are recorded after it.
 */
.macro locked_aligned width, fallback
	.if \width != 8
	tst x7, #(\width / 8 - 1)
	b.ne \fallback
	.endif
.endm

.macro load_exclusive width, number, address
	.if \width == 8
	ldaxrb w\number, [\address]
	.elseif \width == 16
	ldaxrh w\number, [\address]
	.elseif \width == 32
	ldaxr w\number, [\address]
	.else
	ldaxr x\number, [\address]
	.endif
.endm

.macro store_exclusive width, status, number, address
	.if \width == 8
	stlxrb w\status, w\number, [\address]
	.elseif \width == 16
	stlxrh w\status, w\number, [\address]
	.elseif \width == 32
	stlxr w\status, w\number, [\address]
	.else
	stlxr w\status, x\number, [\address]
	.endif
.endm

.macro publish_pending_code address, size
	tbnz x23, #60, .Lx86_code_published\@
	ldrb w8, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cbz w8, .Lx86_code_published\@
	mov x0, _context
	mov x1, \address
	mov w2, #\size
	bl NAME(uml_x86_64_gadget_publish_code_write)
.Lx86_code_published\@:
.endm

.macro finish_store size
	publish_pending_code x25, \size
.Lx86_store_check_overlap\@:
	ldr x8, [_context, #UML_X86_64_CONTEXT_PROGRAM_END]
	cmp x25, x8
	b.hs .Lx86_store_done\@
	add x9, x25, #\size
	ldr x10, [_context, #UML_X86_64_CONTEXT_PROGRAM_BEGIN]
	cmp x9, x10
	b.ls .Lx86_store_done\@
	ldrb w8, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cbnz w8, .Lx86_store_code_exit\@
	mov x0, _context
	mov x1, x25
	bl NAME(uml_x86_64_gadget_invalidate_code)
.Lx86_store_code_exit\@:
	ubfx x8, x23, #56, #4
	add x8, x22, x8
	str x8, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.Lx86_store_done\@:
	retire_and_next
.endm

/* A BlockLink contains {stream, instruction_count, guest RIP, begin, end}.
 * Refuse a target whose complete translated block would cross this native
 * run's exact budget. Every executable store resets cached targets whose
 * saved instruction bytes overlap the write. At the next branch, retain the
 * native chain only when the global code sequence contains exactly this run's
 * own publication, then accept that sequence without cooling the unrelated
 * links. An overwritten target already points at its link_exit fallback.
 * A reset record therefore needs no active-state branch and translated code
 * remains immutable.
 *
 * Keep this common to the load/store and control families: a direct CALL has
 * to finish its architectural stack write before it can enter its callee.
 */
.macro follow_block_link link
	mov x19, \link
	ldrb w9, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cbz w9, .Lx86_link_code_ready\@
	mov x0, _context
	bl NAME(uml_x86_64_gadget_code_link_safe)
	cbz w0, .Lx86_link_code_exit\@
	strb wzr, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
.Lx86_link_code_exit\@:
	cbnz w0, .Lx86_link_code_ready\@
	ldr x9, [x19, #16]
	str x9, [_cpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.Lx86_link_code_ready\@:
	ldp _stream, x11, [x19]
	add x12, _executed, x11
	ldr x13, [_context, #UML_X86_64_CONTEXT_INSTRUCTION_LIMIT]
	cmp x12, x13
	b.ls .Lx86_link_follow\@
	ldr x9, [x19, #16]
	str x9, [_cpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.Lx86_link_follow\@:
	ldp x12, x13, [x19, #24]
	stp x12, x13, [_context, #UML_X86_64_CONTEXT_PROGRAM_BEGIN]
	next_gadget
.endm

/* Probe the address space's shared guest-RIP tier. The entry names the block
 * that last ran at that RIP; enter it only when the recorded RIP is the one
 * just computed, so an evicted or stale entry costs a fallback. `target` must
 * not be x11, x12 or x13.
 */
.macro follow_target_link target, miss
	ldr x11, [_context, #UML_X86_64_CONTEXT_TARGET_LINKS]
	eor x12, \target, \target, lsr #UML_X86_64_TARGET_LINK_FOLD
	and x12, x12, #UML_X86_64_TARGET_LINK_MASK
	add x11, x11, x12, lsl #4
	ldp x12, x13, [x11]
	cmp x12, \target
	b.ne \miss
	follow_block_link x13
.endm

/* An IndirectLink holds two {guest RIP, BlockLink pointer} pairs. A site that
 * stays monomorphic or bimorphic settles here in two compares. Beyond that the
 * shared tier still knows the target whenever its block has run recently, so
 * only a target neither tier holds is worth a C++ round trip; that exit
 * preserves the resolved target and identifies this exact data-only site for
 * cold training. Every hit enters the same budget-checked BlockLink path as a
 * direct edge. */
.macro follow_indirect_link target, site
	ldp x11, x12, [\site]
	cmp \target, x11
	b.eq .Lx86_indirect_follow\@
	ldp x11, x12, [\site, #16]
	cmp \target, x11
	b.eq .Lx86_indirect_follow\@
	follow_target_link \target, .Lx86_indirect_train\@
.Lx86_indirect_train\@:
	str \site, [_context, #UML_X86_64_CONTEXT_INDIRECT_MISS]
	str \target, [_cpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.Lx86_indirect_follow\@:
	mov x8, x12
	follow_block_link x8
.endm

.text
.p2align 4
.globl NAME(uml_x86_64_gadget_enter)
NAME(uml_x86_64_gadget_enter):
	/* x0 = immutable threaded stream, x1 = CpuState, x2 = execution
	 * context. x18 is reserved by Apple and is preserved untouched.
	 */
	stp x18, x19, [sp, #-0x70]!
	stp x20, x21, [sp, #0x10]
	stp x22, x23, [sp, #0x20]
	stp x24, x25, [sp, #0x30]
	stp x26, x27, [sp, #0x40]
	stp x28, x29, [sp, #0x50]
	str lr, [sp, #0x60]
	mov _stream, x0
	mov _cpu, x1
	mov _context, x2
	mov _executed, xzr
	x86_memory_order_boundary
	next_gadget

.p2align 4
uml_x86_64_gadget_leave:
	x86_memory_order_boundary
	mov x1, _executed
	ldr lr, [sp, #0x60]
	ldp x28, x29, [sp, #0x50]
	ldp x26, x27, [sp, #0x40]
	ldp x24, x25, [sp, #0x30]
	ldp x22, x23, [sp, #0x20]
	ldp x20, x21, [sp, #0x10]
	ldp x18, x19, [sp], #0x70
	ret

/* Materialize the lazy x86 arithmetic flags without entering C++. Common
 * arithmetic gadgets record byte width plus masked inputs/result; consumers
 * pay this work only when they actually inspect flags.
 *
 * Operation and width are both constants at the recording site, so the two
 * run-time dispatch chains they used to drive are replaced by one bounded
 * table branch into a body that has them folded in. `flags_body` is the single
 * definition of the semantics: passing zero for either parameter selects the
 * run-time form, which is what the generic table slots use for a width the
 * table does not enumerate.
 */

/* Carry-flag input for the two operations that fold in an incoming carry. */
.macro flags_adjusted_carry width, compare_left
	.if (\width) == 0
	lsl x13, x12, #1
	sub x13, x13, #1
	add x14, x8, x16
	and x14, x14, x13
	.elseif (\width) == 8
	add x14, x8, x16
	.else
	add x14, x8, x16
	and x14, x14, #((1 << ((\width) * 8)) - 1)
	.endif
	cmp x14, x8
	cset w15, lo
	.ifc \compare_left,result
	cmp x10, x7
	.else
	cmp x7, x14
	.endif
	cset w17, lo
	orr w15, w15, w17
.endm

/* Test the width's sign bit in \reg and set OF when it is set. The caller
 * places \label, so the same macro serves named and numeric local labels.
 */
.macro flags_overflow_from width, reg, label
	.if (\width) == 0
	tst \reg, x12
	b.eq \label
	.else
	tbz \reg, #((\width) * 8 - 1), \label
	.endif
	orr x9, x9, #0x800
.endm

/* The last bit a shift moved out, in bit 0 of x14: x7 is the original
 * value, x8 the masked nonzero count. A count beyond the width shifts out
 * zeros for SHL (8) and SHR (9) and the sign for SAR (10). */
.macro shift_carry_bit operation, width
	.if (\operation) == 8
	mov x14, #((\width) * 8)
	cmp x8, x14
	b.hi 91f
	sub x14, x14, x8
	lsr x14, x7, x14
	b 92f
91:
	mov x14, xzr
92:
	.elseif (\operation) == 9
	cmp x8, #((\width) * 8)
	b.hi 91f
	sub x14, x8, #1
	lsr x14, x7, x14
	b 92f
91:
	mov x14, xzr
92:
	.else
	cmp x8, #((\width) * 8)
	b.ls 91f
	mov x14, #((\width) * 8 - 1)
	b 92f
91:
	sub x14, x8, #1
92:
	lsr x14, x7, x14
	.endif
.endm

/* SHL, SHR and SAR from their record: the result defines zero, sign and
 * parity, carry is the last bit shifted out, overflow is defined for a count
 * of one and otherwise left as it was, and the auxiliary flag is untouched --
 * exactly what the eager override used to leave. */
.macro shift_flags_body operation, width
	add x6, _cpu, #UML_X86_64_CPU_DEFERRED_LEFT
	ldp x7, x8, [x6]
	ldr x10, [x6, #16]
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	mov x13, #0xc5
	bic x9, x9, x13
	cbnz x10, 1f
	orr x9, x9, #0x40
1:
	tbz x10, #((\width) * 8 - 1), 2f
	orr x9, x9, #0x80
2:
	and w13, w10, #0xff
	eor w13, w13, w13, lsr #4
	eor w13, w13, w13, lsr #2
	eor w13, w13, w13, lsr #1
	tbnz w13, #0, 3f
	orr x9, x9, #0x4
3:
	shift_carry_bit \operation, \width
	tbz w14, #0, 4f
	orr x9, x9, #0x1
4:
	cmp x8, #1
	b.ne 5f
	bic x9, x9, #0x800
	.if (\operation) == 8
	lsr x14, x10, #((\width) * 8 - 1)
	eor x14, x14, x9
	tbz x14, #0, 5f
	orr x9, x9, #0x800
	.elseif (\operation) == 9
	tbz x7, #((\width) * 8 - 1), 5f
	orr x9, x9, #0x800
	.endif
5:
	b .Lx86_flags_apply_overrides
.endm

.macro shift_carry_body operation, width
	add x6, _cpu, #UML_X86_64_CPU_DEFERRED_LEFT
	ldp x7, x8, [x6]
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	bic x9, x9, #0x1
	shift_carry_bit \operation, \width
	tbz w14, #0, 4f
	orr x9, x9, #0x1
4:
	b .Lx86_carry_apply_overrides
.endm

.macro flags_body operation, width
	add x6, _cpu, #UML_X86_64_CPU_DEFERRED_LEFT
	ldp x7, x8, [x6]
	ldr x10, [x6, #16]
	.if (\operation) == 0
	ldrb w16, [x6, #26]
	.elseif ((\operation) == 6) || ((\operation) == 7)
	ldrb w16, [x6, #26]
	.endif
	/* Narrow the recorded operands to the width that produced them. */
	.if (\width) == 1
	and x7, x7, #0xff
	and x8, x8, #0xff
	and x10, x10, #0xff
	.elseif (\width) == 2
	uxth w7, w7
	uxth w8, w8
	uxth w10, w10
	.elseif (\width) == 4
	mov w7, w7
	mov w8, w8
	mov w10, w10
	.elseif (\width) == 0
	ldrb w11, [x6, #25]
	cmp w11, #1
	b.ne 1f
	and x7, x7, #0xff
	and x8, x8, #0xff
	and x10, x10, #0xff
	mov x12, #0x80
	b 4f
1:
	cmp w11, #2
	b.ne 2f
	uxth w7, w7
	uxth w8, w8
	uxth w10, w10
	mov x12, #0x8000
	b 4f
2:
	cmp w11, #4
	b.ne 3f
	mov w7, w7
	mov w8, w8
	mov w10, w10
	mov x12, #0x80000000
	b 4f
3:
	mov x12, #1
	lsl x12, x12, #63
4:
	.endif
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	/* INC and DEC preserve CF; every other recorded operation defines it. */
	.if (\operation) == 0
	cmp w5, #4
	b.lo 41f
	cmp w5, #5
	b.hi 41f
	mov x13, #0x8d4
	b 42f
41:
	mov x13, #0x8d5
42:
	.elseif ((\operation) == 4) || ((\operation) == 5)
	mov x13, #0x8d4
	.else
	mov x13, #0x8d5
	.endif
	bic x9, x9, x13
	cbnz x10, 5f
	orr x9, x9, #0x40
5:
	.if (\width) == 0
	tst x10, x12
	b.eq 6f
	.else
	tbz x10, #((\width) * 8 - 1), 6f
	.endif
	orr x9, x9, #0x80
6:
	and w13, w10, #0xff
	eor w13, w13, w13, lsr #4
	eor w13, w13, w13, lsr #2
	eor w13, w13, w13, lsr #1
	tbnz w13, #0, 7f
	orr x9, x9, #0x4
7:
	.if (\operation) == 0
	cmp w5, #1
	b.eq .Lx86_flags_add
	cmp w5, #4
	b.eq .Lx86_flags_add_preserve_carry
	cmp w5, #6
	b.eq .Lx86_flags_add_carry
	cmp w5, #2
	b.eq .Lx86_flags_sub
	cmp w5, #5
	b.eq .Lx86_flags_sub_preserve_carry
	cmp w5, #7
	b.eq .Lx86_flags_sub_borrow
	b .Lx86_flags_apply_overrides

.Lx86_flags_add:
	cmp x10, x7
	b.hs .Lx86_flags_add_preserve_carry
	orr x9, x9, #0x1
.Lx86_flags_add_preserve_carry:
	eor x13, x7, x8
	mvn x13, x13
	eor x14, x7, x10
	and x13, x13, x14
	flags_overflow_from 0, x13, .Lx86_flags_add_aux
.Lx86_flags_add_aux:
	b .Lx86_flags_auxiliary

.Lx86_flags_add_carry:
	flags_adjusted_carry 0, result
	cbz w15, .Lx86_flags_add_preserve_carry
	orr x9, x9, #0x1
	b .Lx86_flags_add_preserve_carry

.Lx86_flags_sub:
	cmp x7, x8
	b.hs .Lx86_flags_sub_preserve_carry
	orr x9, x9, #0x1
.Lx86_flags_sub_preserve_carry:
	eor x13, x7, x8
	eor x14, x7, x10
	and x13, x13, x14
	flags_overflow_from 0, x13, .Lx86_flags_sub_aux
.Lx86_flags_sub_aux:
	b .Lx86_flags_auxiliary

.Lx86_flags_sub_borrow:
	flags_adjusted_carry 0, right
	cbz w15, .Lx86_flags_sub_preserve_carry
	orr x9, x9, #0x1
	b .Lx86_flags_sub_preserve_carry

.Lx86_flags_auxiliary:
	eor x13, x7, x8
	eor x13, x13, x10
	tbz x13, #4, .Lx86_flags_apply_overrides
	orr x9, x9, #0x10
	b .Lx86_flags_apply_overrides
	.else
	/* Carry, overflow and auxiliary for one known operation. */
	.if (\operation) == 1
	cmp x10, x7
	b.hs 81f
	orr x9, x9, #0x1
81:
	.elseif (\operation) == 2
	cmp x7, x8
	b.hs 81f
	orr x9, x9, #0x1
81:
	.elseif (\operation) == 6
	flags_adjusted_carry \width, result
	cbz w15, 81f
	orr x9, x9, #0x1
81:
	.elseif (\operation) == 7
	flags_adjusted_carry \width, right
	cbz w15, 81f
	orr x9, x9, #0x1
81:
	.endif
	.if (\operation) != 3
	.if ((\operation) == 1) || ((\operation) == 4) || ((\operation) == 6)
	eor x13, x7, x8
	mvn x13, x13
	.else
	eor x13, x7, x8
	.endif
	eor x14, x7, x10
	and x13, x13, x14
	flags_overflow_from \width, x13, 82f
82:
	eor x13, x7, x8
	eor x13, x13, x10
	tbz x13, #4, 83f
	orr x9, x9, #0x10
83:
	.endif
	b .Lx86_flags_apply_overrides
	.endif
.endm

.p2align 4
uml_x86_64_gadget_materialize_flags:
	ldrb w5, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	cbnz w5, .Lx86_flags_dispatch
	ldrh w13, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	cbz w13, 9f
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	b .Lx86_flags_apply_overrides
.Lx86_flags_dispatch:
	/* Masking both keys keeps the branch inside the table whatever the
	 * recorded bytes hold; the slots those values reach are the generic body.
	 */
	ldrb w11, [_cpu, #UML_X86_64_CPU_DEFERRED_WIDTH]
	adr x14, .Lx86_flags_table
	and w12, w5, #15
	and w13, w11, #0xf
	add x14, x14, x12, lsl #6
	add x14, x14, x13, lsl #2
	br x14

.p2align 2
.Lx86_flags_table:
.irp op,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
.irp w,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	.if ((\op) >= 1) && ((\op) <= 10) && (((\w) == 1) || ((\w) == 2) || ((\w) == 4) || ((\w) == 8))
	b .Lx86_flags_o\op\()_w\w
	.else
	b .Lx86_flags_generic
	.endif
.endr
.endr

.irp op,1,2,3,4,5,6,7
.irp w,1,2,4,8
.Lx86_flags_o\op\()_w\w:
	flags_body \op, \w
.endr
.endr

.irp op,8,9,10
.irp w,1,2,4,8
.Lx86_flags_o\op\()_w\w:
	shift_flags_body \op, \w
.endr
.endr

.Lx86_flags_generic:
	flags_body 0, 0

.Lx86_flags_apply_overrides:
	ldrh w13, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	cbz w13, .Lx86_flags_store
	ldrh w14, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_VALUE]
	bic x9, x9, x13
	and x14, x14, x13
	orr x9, x9, x14
.Lx86_flags_store:
	orr x9, x9, #0x2
	str x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	strb wzr, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	str wzr, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
9:
	ret

/* Carry-only materialization. INC and DEC preserve CF and define the other
 * five arithmetic flags from their own record; ADC and SBB read CF as an
 * operand and then define all six from theirs. Every one of them therefore
 * needs exactly one correct bit out of RFLAGS, and the full routine's zero,
 * sign, parity, auxiliary and overflow work is overwritten the moment the
 * caller records. This entry point computes carry alone from the same
 * pending record, through the same operation-and-width table, and leaves the
 * five dead bits as they are.
 *
 * It is only for callers that record before any guest memory access can
 * fault. Between this routine and the caller's own record the five stale bits
 * are architecturally visible, so a gadget that can leave in that window --
 * every read-modify-write form -- keeps the full routine.
 */
.macro carry_body operation, width
	.if ((\operation) == 4) || ((\operation) == 5)
	/* INC and DEC preserve carry. */
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	b .Lx86_carry_apply_overrides
	.elseif (\operation) == 3
	/* Logical operations clear carry. */
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	bic x9, x9, #0x1
	b .Lx86_carry_apply_overrides
	.else
	add x6, _cpu, #UML_X86_64_CPU_DEFERRED_LEFT
	ldp x7, x8, [x6]
	ldr x10, [x6, #16]
	.if ((\operation) == 0) || ((\operation) == 6) || ((\operation) == 7)
	ldrb w16, [x6, #26]
	.endif
	.if (\width) == 1
	and x7, x7, #0xff
	and x8, x8, #0xff
	and x10, x10, #0xff
	.elseif (\width) == 2
	uxth w7, w7
	uxth w8, w8
	uxth w10, w10
	.elseif (\width) == 4
	mov w7, w7
	mov w8, w8
	mov w10, w10
	.elseif (\width) == 0
	ldrb w11, [x6, #25]
	cmp w11, #1
	b.ne 11f
	and x7, x7, #0xff
	and x8, x8, #0xff
	and x10, x10, #0xff
	mov x12, #0x80
	b 14f
11:
	cmp w11, #2
	b.ne 12f
	uxth w7, w7
	uxth w8, w8
	uxth w10, w10
	mov x12, #0x8000
	b 14f
12:
	cmp w11, #4
	b.ne 13f
	mov w7, w7
	mov w8, w8
	mov w10, w10
	mov x12, #0x80000000
	b 14f
13:
	mov x12, #1
	lsl x12, x12, #63
14:
	.endif
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	.if (\operation) == 0
	/* INC, DEC and an unrecognized operation reach this body with a
	 * run-time key; only the four defining operations clear carry. */
	cmp w5, #4
	b.lo 15f
	cmp w5, #5
	b.ls .Lx86_carry_apply_overrides
15:
	bic x9, x9, #0x1
	cmp w5, #1
	b.eq 16f
	cmp w5, #2
	b.eq 17f
	cmp w5, #6
	b.eq 18f
	cmp w5, #7
	b.eq 19f
	b .Lx86_carry_apply_overrides
16:
	cmp x10, x7
	b.hs .Lx86_carry_apply_overrides
	orr x9, x9, #0x1
	b .Lx86_carry_apply_overrides
17:
	cmp x7, x8
	b.hs .Lx86_carry_apply_overrides
	orr x9, x9, #0x1
	b .Lx86_carry_apply_overrides
18:
	flags_adjusted_carry 0, result
	cbz w15, .Lx86_carry_apply_overrides
	orr x9, x9, #0x1
	b .Lx86_carry_apply_overrides
19:
	flags_adjusted_carry 0, right
	cbz w15, .Lx86_carry_apply_overrides
	orr x9, x9, #0x1
	b .Lx86_carry_apply_overrides
	.else
	bic x9, x9, #0x1
	.if (\operation) == 1
	cmp x10, x7
	b.hs .Lx86_carry_apply_overrides
	orr x9, x9, #0x1
	.elseif (\operation) == 2
	cmp x7, x8
	b.hs .Lx86_carry_apply_overrides
	orr x9, x9, #0x1
	.elseif (\operation) == 6
	flags_adjusted_carry \width, result
	cbz w15, .Lx86_carry_apply_overrides
	orr x9, x9, #0x1
	.elseif (\operation) == 7
	flags_adjusted_carry \width, right
	cbz w15, .Lx86_carry_apply_overrides
	orr x9, x9, #0x1
	.endif
	b .Lx86_carry_apply_overrides
	.endif
	.endif
.endm

.p2align 4
uml_x86_64_gadget_materialize_carry:
	ldrb w5, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	cbnz w5, .Lx86_carry_dispatch
	ldrh w13, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	cbz w13, 9f
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	b .Lx86_carry_apply_overrides
.Lx86_carry_dispatch:
	ldrb w11, [_cpu, #UML_X86_64_CPU_DEFERRED_WIDTH]
	adr x14, .Lx86_carry_table
	and w12, w5, #15
	and w13, w11, #0xf
	add x14, x14, x12, lsl #6
	add x14, x14, x13, lsl #2
	br x14

.p2align 2
.Lx86_carry_table:
.irp op,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
.irp w,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	.if ((\op) >= 1) && ((\op) <= 10) && (((\w) == 1) || ((\w) == 2) || ((\w) == 4) || ((\w) == 8))
	b .Lx86_carry_o\op\()_w\w
	.else
	b .Lx86_carry_generic
	.endif
.endr
.endr

.irp op,1,2,3,4,5,6,7
.irp w,1,2,4,8
.Lx86_carry_o\op\()_w\w:
	carry_body \op, \w
.endr
.endr

.irp op,8,9,10
.irp w,1,2,4,8
.Lx86_carry_o\op\()_w\w:
	shift_carry_body \op, \w
.endr
.endr

.Lx86_carry_generic:
	carry_body 0, 0

.Lx86_carry_apply_overrides:
	ldrh w13, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	cbz w13, .Lx86_carry_store
	ldrh w14, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_VALUE]
	bic x9, x9, x13
	and x14, x14, x13
	orr x9, x9, x14
.Lx86_carry_store:
	orr x9, x9, #0x2
	str x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	strb wzr, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	str wzr, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
9:
	ret


gadget block_exit
	ldr x8, [_stream]
	str x8, [_cpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave

/* The same block end, for a block the fetch window cut short rather than a
 * branch. Its successor is the next address, which is as statically known as a
 * direct jump's target, so it is followed the same way. Falling off the end of
 * a block retires no instruction, which is why this does not advance the
 * retirement count the way `jump_linked` does for the jump it executes.
 */
gadget block_exit_linked
	ldr x8, [_stream]
	follow_block_link x8

gadget nop
	retire_and_next
