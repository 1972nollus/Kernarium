/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../layout/assembly_offsets.h"

.arch armv8-a

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

.macro retire_and_next
	add _executed, _executed, #1
	next_gadget
.endm

/* Stream descriptor:
 *   15:0   value-register byte offset
 *   31:16  base-register byte offset or 0xffff
 *   47:32  index-register byte offset or 0xffff
 *   49:48  index scale shift
 *   51:50  segment (0 none, 1 FS, 2 GS)
 *   52     32-bit effective-address arithmetic
 *   59:56  instruction length
 * Followed by the folded displacement/absolute address and current RIP.
 */
.macro effective_address
	ldp x23, x14, [_stream], #16
	ldr x22, [_stream], #8
	mov x7, x14
	mov w15, #0xffff
	ubfx w9, w23, #16, #16
	cmp w9, w15
	b.eq .Lx86_no_base\@
	ldr x10, [_cpu, x9]
	tbz x23, #52, .Lx86_base_ready\@
	mov w10, w10
.Lx86_base_ready\@:
	add x7, x7, x10
.Lx86_no_base\@:
	ubfx x9, x23, #32, #16
	cmp w9, w15
	b.eq .Lx86_no_index\@
	ldr x10, [_cpu, x9]
	tbz x23, #52, .Lx86_index_ready\@
	mov w10, w10
.Lx86_index_ready\@:
	ubfx x11, x23, #48, #2
	lsl x10, x10, x11
	add x7, x7, x10
.Lx86_no_index\@:
	tbz x23, #52, .Lx86_address_ready\@
	mov w7, w7
.Lx86_address_ready\@:
	ubfx x9, x23, #50, #2
	cmp w9, #1
	b.ne .Lx86_not_fs\@
	ldr x10, [_cpu, #UML_X86_64_CPU_FS_BASE]
	add x7, x7, x10
	b .Lx86_segment_ready\@
.Lx86_not_fs\@:
	cmp w9, #2
	b.ne .Lx86_segment_ready\@
	ldr x10, [_cpu, #UML_X86_64_CPU_GS_BASE]
	add x7, x7, x10
.Lx86_segment_ready\@:
.endm

.macro translate access, entry_offset, size
	and x9, x7, #0xfffffffffffff000
	lsr x10, x7, #12
	eor x10, x10, x10, lsr #13
	and w10, w10, #0x1fff
	ldr x11, [_context, #UML_X86_64_CONTEXT_TLB]
	add x11, x11, x10, lsl #5
	ldp x12, x13, [x11, #\entry_offset]
	cmp x9, x12
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
	add x7, x13, x7
.Lx86_tlb_done\@:
.endm

.macro finish_store size
	ldr x8, [_context, #UML_X86_64_CONTEXT_PROGRAM_END]
	cmp x25, x8
	b.hs .Lx86_store_done\@
	add x9, x25, #\size
	ldr x10, [_context, #UML_X86_64_CONTEXT_PROGRAM_BEGIN]
	cmp x9, x10
	b.ls .Lx86_store_done\@
	mov x0, _context
	mov x1, x25
	bl NAME(uml_x86_64_gadget_invalidate_code)
	ubfx x8, x23, #56, #4
	add x8, x22, x8
	str x8, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.Lx86_store_done\@:
	retire_and_next
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
	next_gadget

.p2align 4
uml_x86_64_gadget_leave:
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
 * pay this branchy work only when they actually inspect flags.
 */
.p2align 4
uml_x86_64_gadget_materialize_flags:
	ldrb w5, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	cbz w5, 9f
	add x6, _cpu, #UML_X86_64_CPU_DEFERRED_LEFT
	ldp x7, x8, [x6]
	ldr x10, [x6, #16]
	ldrb w11, [x6, #25]
	ldrb w16, [x6, #26]
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
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	cmp w5, #4
	b.lo 41f
	cmp w5, #5
	b.hi 41f
	mov x13, #0x8d4
	b 42f
41:
	mov x13, #0x8d5
42:
	bic x9, x9, x13
	cbnz x10, 5f
	orr x9, x9, #0x40
5:
	tst x10, x12
	b.eq 6f
	orr x9, x9, #0x80
6:
	and w13, w10, #0xff
	eor w13, w13, w13, lsr #4
	eor w13, w13, w13, lsr #2
	eor w13, w13, w13, lsr #1
	tbnz w13, #0, 7f
	orr x9, x9, #0x4
7:
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
	b .Lx86_flags_done

.Lx86_flags_add:
	cmp x10, x7
	b.hs 71f
	orr x9, x9, #0x1
71:
.Lx86_flags_add_preserve_carry:
	eor x13, x7, x8
	mvn x13, x13
	eor x14, x7, x10
	and x13, x13, x14
	tst x13, x12
	b.eq 72f
	orr x9, x9, #0x800
72:
	b .Lx86_flags_auxiliary

.Lx86_flags_add_carry:
	lsl x13, x12, #1
	sub x13, x13, #1
	add x14, x8, x16
	and x14, x14, x13
	cmp x14, x8
	cset w15, lo
	cmp x10, x7
	cset w17, lo
	orr w15, w15, w17
	cbz w15, 75f
	orr x9, x9, #0x1
75:
	b .Lx86_flags_add_preserve_carry

.Lx86_flags_sub:
	cmp x7, x8
	b.hs 73f
	orr x9, x9, #0x1
73:
.Lx86_flags_sub_preserve_carry:
	eor x13, x7, x8
	eor x14, x7, x10
	and x13, x13, x14
	tst x13, x12
	b.eq 74f
	orr x9, x9, #0x800
74:
	b .Lx86_flags_auxiliary

.Lx86_flags_sub_borrow:
	lsl x13, x12, #1
	sub x13, x13, #1
	add x14, x8, x16
	and x14, x14, x13
	cmp x14, x8
	cset w15, lo
	cmp x7, x14
	cset w17, lo
	orr w15, w15, w17
	cbz w15, 76f
	orr x9, x9, #0x1
76:
	b .Lx86_flags_sub_preserve_carry

.Lx86_flags_auxiliary:
	eor x13, x7, x8
	eor x13, x13, x10
	tbz x13, #4, .Lx86_flags_done
	orr x9, x9, #0x10

.Lx86_flags_done:
	orr x9, x9, #0x2
	str x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	strb wzr, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
9:
	ret

gadget block_exit
	ldr x8, [_stream]
	str x8, [_cpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave

gadget nop
	retire_and_next
