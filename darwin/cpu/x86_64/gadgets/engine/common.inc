/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../layout/assembly_offsets.h"

.arch armv8.2-a

#define NAME(value) _##value

_cpu .req x20
_context .req x21
_stream .req x28
_executed .req x29

	.set x86_guest_memory_gadget, 0

/* Name the start of the cold section, whose own labels are temporary, so a
 * host sample of a cold path names this section rather than whatever precedes
 * it in the image. It emits no bytes. */
	.pushsection __TEXT,__gadget_cold,regular,pure_instructions
NAME(uml_x86_64_gadget_cold_section_begin):
	.popsection

.macro gadget name
	.globl NAME(uml_x86_64_gadget_\name)
	.p2align 4
NAME(uml_x86_64_gadget_\name):
	.set x86_guest_memory_gadget, 0
.endm

/* Publish the guest's rounding mode as the host's.
 *
 * x86 keeps it in MXCSR and the host in FPCR, and the two number the directed
 * modes the other way round, so the two-bit field is reversed. Writing FPCR
 * can cost a pipeline flush, so it is written only when it would change --
 * which, for the guests that never leave round-to-nearest, is never.
 * `scratch*` must be free at the call site.
 */
.macro publish_guest_rounding scratch0, scratch1, scratch2
	ldr w\scratch0, [_cpu, #UML_X86_64_CPU_MXCSR]
	mrs x\scratch1, fpcr
	ubfx w\scratch0, w\scratch0, #13, #2
	rbit w\scratch2, w\scratch0
	lsr w\scratch2, w\scratch2, #30
	ubfx x\scratch0, x\scratch1, #22, #2
	cmp x\scratch0, x\scratch2
	b.eq Lx86_rounding_ready\@
	bfi x\scratch1, x\scratch2, #22, #2
	msr fpcr, x\scratch1
Lx86_rounding_ready\@:
.endm

.macro next_gadget
	ldr x8, [_stream], #8
	br x8
.endm

/* x86 promises total store order: the other CPUs see one CPU's stores in the
 * order it made them, and its loads see memory move forward in that same
 * order. Guest code written for x86 leans on that without saying so, and a
 * LOCK-prefixed instruction or an XCHG against memory is in addition a full
 * fence -- a work-stealing deque stores its tail, runs a locked instruction
 * and only then loads the head. The host orders nothing: it may run two
 * independent loads in either order, and an exclusive loop orders only its own
 * operand. With several vCPUs that is a task taken twice, a wild pointer or a
 * wakeup that never comes.
 *
 * A guest memory access is therefore followed by a barrier before the next
 * guest instruction can touch memory. `translate`, `stack_fast` and the few
 * private translation and bulk-helper macros mark the gadget they expand in,
 * and every retiring way out of a marked gadget carries the barrier, so a
 * gadget that works on registers alone pays nothing and a locked gadget is
 * fenced like any other store. The mark is textual: a memory form that joins
 * another gadget's tail runs the boundary before it branches. Accesses inside
 * one instruction keep the order the gadget gives them.
 */
.macro x86_memory_order_barrier
	dmb ish
.endm

.macro x86_marks_guest_memory
	.set x86_guest_memory_gadget, 1
.endm

.macro x86_memory_order_boundary
	.if x86_guest_memory_gadget
	x86_memory_order_barrier
	.endif
.endm

.macro retire_and_next
	add _executed, _executed, #1
	x86_memory_order_boundary
	next_gadget
.endm

/* The engine leaves #MF for an x87 exception already pending, and #XM while
 * MXCSR unmasks underflow or precision, to the runtime, which runs the
 * instructions that could raise them one at a time, checked; the gadgets check
 * other unmasked x87 and SIMD exceptions themselves while the run's
 * `exception_checks` says so. An instruction that loads MXCSR or the x87
 * control word therefore retires and leaves when it could change what a run
 * checks, so the runtime sees the change before the next instruction runs:
 * when it unmasks an exception in a run that does not check that kind, when it
 * unmasks SIMD underflow or precision, or when it unmasks an x87 exception
 * whose flag is set, which is then pending. For a gadget that computed its
 * effective address: x22 and the descriptor's length name the next
 * instruction. */
.macro retire_and_leave
	ubfx x8, x23, #56, #4
	add x8, x22, x8
	str x8, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
.endm

/* `value` is the MXCSR just loaded. */
.macro retire_unless_simd_unchecked value
	mvn w8, \value
	tst w8, #0x1f80
	b.ne Lx86_simd_unmasked\@
	retire_and_next
Lx86_simd_unmasked\@:
	tst w8, #0x1800
	b.ne Lx86_simd_unchecked\@
	ldrb w8, [_cpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w8, #1, Lx86_simd_unchecked\@
	retire_and_next
Lx86_simd_unchecked\@:
	retire_and_leave
.endm

/* `control` is the x87 control word just loaded, and the status word is
 * already the one the instruction leaves. */
.macro retire_unless_x87_unchecked control
	mvn w8, \control
	tst w8, #0x3f
	b.ne Lx86_x87_unmasked\@
	retire_and_next
Lx86_x87_unmasked\@:
	ldrb w8, [_cpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w8, #0, Lx86_x87_unchecked\@
	ldrh w8, [_cpu, #UML_X86_64_CPU_X87_STATUS]
	bic w8, w8, \control
	tst w8, #0x3f
	b.ne Lx86_x87_unchecked\@
	retire_and_next
Lx86_x87_unchecked\@:
	retire_and_leave
.endm

/* The same for an instruction that loads both. */
.macro retire_unless_any_unchecked
	ldr w8, [_cpu, #UML_X86_64_CPU_MXCSR]
	mvn w8, w8
	tst w8, #0x1f80
	b.eq Lx86_simd_masked\@
	tst w8, #0x1800
	b.ne Lx86_simd_unchecked\@
	ldrb w8, [_cpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w8, #1, Lx86_simd_unchecked\@
Lx86_simd_masked\@:
	ldrh w9, [_cpu, #UML_X86_64_CPU_X87_CONTROL]
	retire_unless_x87_unchecked w9
Lx86_simd_unchecked\@:
	retire_and_leave
.endm

/* While MXCSR unmasks invalid operation, division by zero, overflow or a
 * denormal operand, and masks underflow and precision, the runtime sets
 * `exception_check_simd` in `exception_checks`. The SIMD floating-point
 * gadgets then test their values for anything that could have raised an
 * unmasked exception, before they store anything: a result whose exponent is
 * all ones or one below -- a NaN, an infinity, or the largest finite value an
 * overflow rounds to -- a NaN input to a comparison, minimum or maximum, and a
 * conversion to an integer out of its range. Such an instruction leaves, and
 * the runtime runs it again, checked, which raises #XM or completes it with
 * its exact flags. Where the gadget already takes a separate path for such a
 * value the test costs nothing more; elsewhere it costs a load and a branch
 * while the gadgets do not check.
 *
 * Leaves while the gadgets check SIMD exceptions: on a path taken only for a
 * special value, or for an instruction whose intermediate results no test of
 * its result can see. */
.macro simd_leave_if_checking
	ldrb w14, [_cpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w14, #1, Lx86_simd_unchecked\@
	b uml_x86_64_gadget_exception_check
Lx86_simd_unchecked\@:
.endm

/* w14 = nonzero when a lane of v`register` (`element` bits wide) has an
 * exponent of all ones or one below. Uses v17 and v18. */
.macro simd_special_lanes element, register
	.if \element == 16
	shl v17.8h, v\register\().8h, #1
	movi v18.8h, #0xf0, lsl #8
	cmhs v17.8h, v17.8h, v18.8h
	.elseif \element == 32
	shl v17.4s, v\register\().4s, #1
	movi v18.4s, #0xfe, lsl #24
	cmhs v17.4s, v17.4s, v18.4s
	.else
	shl v17.2d, v\register\().2d, #1
	mov x14, #0xffc0000000000000
	dup v18.2d, x14
	cmhs v17.2d, v17.2d, v18.2d
	.endif
	umaxv s17, v17.4s
	fmov w14, s17
.endm

/* Branches to `label` when lane 0 of v`register` has an exponent of all ones
 * or one below, or with `condition` lo when it does not. Uses x14. */
.macro simd_special_scalar element, register, label, condition=hs
	.if \element == 32
	fmov w14, s\register
	ubfx w14, w14, #23, #8
	cmp w14, #0xfe
	.else
	fmov x14, d\register
	ubfx x14, x14, #52, #11
	cmp x14, #0x7fe
	.endif
	b.\condition \label
.endm

/* w14 = all ones when no lane of v`left` or v`right` is a NaN, else zero.
 * Uses v17 and v18. */
.macro simd_ordered_lanes element, left, right
	.if \element == 32
	fcmeq v17.4s, v\left\().4s, v\left\().4s
	fcmeq v18.4s, v\right\().4s, v\right\().4s
	.else
	fcmeq v17.2d, v\left\().2d, v\left\().2d
	fcmeq v18.2d, v\right\().2d, v\right\().2d
	.endif
	and v17.16b, v17.16b, v18.16b
	uminv s17, v17.4s
	fmov w14, s17
.endm

/* While the gadgets check SIMD exceptions, leave when a lane of a packed
 * result -- v`first`, and v`second` for a 256-bit one -- is special. */
.macro simd_check_packed element, first, second=-1
	ldrb w14, [_cpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w14, #1, Lx86_simd_packed_ok\@
	simd_special_lanes \element, \first
	.if \second >= 0
	cbnz w14, Lx86_simd_packed_special\@
	simd_special_lanes \element, \second
	.endif
	cbz w14, Lx86_simd_packed_ok\@
Lx86_simd_packed_special\@:
	b uml_x86_64_gadget_exception_check
Lx86_simd_packed_ok\@:
.endm

/* The same for a packed result in v4, and v5 for a 256-bit one. */
.macro simd_check_result element, width
	.if \width == 256
	simd_check_packed \element, 4, 5
	.else
	simd_check_packed \element, 4
	.endif
.endm

/* The same for the scalar result in lane 0 of v`register`. */
.macro simd_check_scalar element, register
	ldrb w14, [_cpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w14, #1, Lx86_simd_scalar_ok\@
	simd_special_scalar \element, \register, Lx86_simd_scalar_special\@
	b Lx86_simd_scalar_ok\@
Lx86_simd_scalar_special\@:
	b uml_x86_64_gadget_exception_check
Lx86_simd_scalar_ok\@:
.endm

/* The same for a NaN in a lane of the packed inputs v`left` and v`right`, and
 * v`left2` and v`right2` for 256-bit ones. */
.macro simd_check_packed_nan element, left, right, left2=-1, right2=-1
	ldrb w14, [_cpu, #UML_X86_64_CPU_EXCEPTION_CHECKS]
	tbz w14, #1, Lx86_simd_inputs_ok\@
	simd_ordered_lanes \element, \left, \right
	cbz w14, Lx86_simd_inputs_nan\@
	.if \left2 >= 0
	simd_ordered_lanes \element, \left2, \right2
	cbz w14, Lx86_simd_inputs_nan\@
	.endif
	b Lx86_simd_inputs_ok\@
Lx86_simd_inputs_nan\@:
	b uml_x86_64_gadget_exception_check
Lx86_simd_inputs_ok\@:
.endm

/* The same for a NaN in lane 0 of v`left` or v`right`, tested in any case:
 * the test costs no more than the gate would. */
.macro simd_check_scalar_nan element, left, right
	.if \element == 32
	fcmp s\left, s\right
	.else
	fcmp d\left, d\right
	.endif
	b.vc Lx86_simd_ordered\@
	simd_leave_if_checking
Lx86_simd_ordered\@:
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

/* An INC or DEC record. It carries the pending bit its prologue left in the
 * record it displaces: set when the preserved carry lives in the carry slot.
 */
.macro record_deferred_pending operation, width
	add x15, _cpu, #UML_X86_64_CPU_DEFERRED_LEFT
	ldrb w16, [_cpu, #UML_X86_64_CPU_DEFERRED_PENDING]
	stp x11, x12, [x15]
	mov x14, #((\operation) | ((\width / 8) << 8))
	orr x14, x14, x16, lsl #24
	stp x13, x14, [x15, #16]
.endm

/* INC and DEC preserve the carry flag, which the pending record may still
 * define. Rather than settle it -- a call, a table and the override overlay,
 * on an instruction that is two host instructions of work -- copy that record
 * into the carry slot and mark the pending bit, so the carry is computed only
 * when a consumer reads it through the INC or DEC record. A pending INC or
 * DEC record already names its carry source, so it is left alone; the one
 * with a carry override riding on it is settled the old way, which clears the
 * bit. The prologue writes nothing but the slot and the bit, so a memory form
 * that faults after it leaves the record exactly as it found it. x5, x7, x8,
 * x10 and x14 to x16 are clobbered.
 *
 * An empty record is settled, not pending: RFLAGS already holds the carry,
 * which is the state every gadget that materializes and republishes the flags
 * leaves -- ADCX and ADOX, the flag writers, and any consumer that had to
 * materialize. The slot answers only through the operation-and-width table,
 * where an empty record reads as a carry of zero, so parking one would
 * discard a settled carry. It takes the same way out as a pending INC or DEC
 * record: nothing to do but a carry override, which is settled as before.
 */
.macro lazy_carry_prologue
	ldr x14, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	ands w5, w14, #0xff
	sub w5, w5, #4
	/* `sub` keeps the flags `ands` defined, so an empty record forces the
	 * settled way out without a test of its own. */
	ccmp w5, #1, #4, ne
	b.ls Lx86_lazy_settled\@
	add x15, _cpu, #UML_X86_64_CPU_DEFERRED_LEFT
	ldp x7, x8, [x15]
	ldr x10, [x15, #16]
	add x15, _cpu, #UML_X86_64_CPU_CARRY_SLOT
	stp x7, x8, [x15]
	stp x10, x14, [x15, #16]
	mov w16, #1
	strb w16, [_cpu, #UML_X86_64_CPU_DEFERRED_PENDING]
	b Lx86_lazy_ready\@
Lx86_lazy_settled\@:
	tbz x14, #32, Lx86_lazy_ready\@
	bl uml_x86_64_gadget_materialize_carry
Lx86_lazy_ready\@:
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
 *   31:16  base-register byte offset, or UML_X86_64_CPU_SEGMENT_ZERO when
 *          there is no base register
 *   47:32  index-register byte offset, or UML_X86_64_CPU_SEGMENT_ZERO when
 *          there is no index register
 *   49:48  index scale shift
 *   51:50  segment (0 none, 1 FS, 2 GS)
 *   52     32-bit effective-address arithmetic
 *   53     16-bit effective-address arithmetic
 *   54     none of the three above: the address is complete once the base,
 *          the scaled index and the displacement are summed
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
	/* A long-mode address with no segment override is finished here. The
	 * builder proves that from the operand, because what remains costs a
	 * load of the selected segment base through a computed address -- a form
	 * the host cannot disambiguate against the CpuState stores every gadget
	 * makes, and the most expensive instruction of the generic address path.
	 * Adding a zero segment base and masking nothing are what that work comes
	 * to in the plain case, so skipping it is the same address.
	 */
	tbnz x23, #UML_X86_64_MEMORY_PLAIN_ADDRESS, Lx86_segment_ready\@
	b Lx86_address_general\@
	/* What a segment override or a narrow address space still needs is two
	 * instructions in the entry and its whole body in the cold section, so a
	 * bank this size keeps only the plain form in the hot stream.
	 */
	.pushsection __TEXT,__gadget_cold,regular,pure_instructions
Lx86_address_general\@:
	/* Truncation distributes over addition and shifting, so masking the sum
	 * once is equivalent to masking every component and the sum.
	 */
	tbnz x23, #53, Lx86_address_16\@
	tbz x23, #52, Lx86_address_ready\@
	mov w7, w7
	b Lx86_address_ready\@
Lx86_address_16\@:
	uxth w7, w7
Lx86_address_ready\@:
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
	cbz x9, Lx86_address_done\@
	ldrb w11, [_cpu, #UML_X86_64_CPU_EXECUTION_MODE]
	cmp w11, #1
	b.ne Lx86_address_done\@
	mov w7, w7
Lx86_address_done\@:
	b Lx86_segment_ready\@
	.popsection
Lx86_segment_ready\@:
.endm

/* The stack's own entry, re-checked before any hashing: x25 holds the guest
 * address, and a tag equal to its page -- with no code bit set, which a
 * page holding a cached translation always has -- leaves the host address in
 * x7. Anything else takes \miss, which is the full translation with
 * `record_stack` so the next access finds the new entry here. A write also
 * requires that no earlier store left a publication pending, because the
 * ordinary translation would settle it here; with both conditions the store
 * owes no publication. It may still land in the block that is running, so a
 * store's caller joins its epilogue at the overlap test.
 */
.macro stack_fast access, miss
	x86_marks_guest_memory
	ldr x11, [_context, #UML_X86_64_CONTEXT_STACK_ENTRY]
	and x9, x25, #0xfffffffffffff000
	ldp x12, x13, [x11, #((\access) * 16)]
	eor x14, x9, x12
	.if \access == 1
	ldrb w15, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	orr x14, x14, x15
	.endif
	cbnz x14, \miss
	add x7, x25, x13
.endm

.macro translate access, entry_offset, size, record_stack=0
	x86_marks_guest_memory
	.if \access == 1
	/* Only a translation that reaches the decision below may skip the store's
	 * publication; the miss and page-crossing paths leave this clear.
	 */
	and x23, x23, #~(1 << 60)
	.endif
	and x9, x7, #0xfffffffffffff000
	lsr x10, x7, #12
	eor x10, x10, x10, lsr #15
	and w10, w10, #0x7fff
	ldr x11, [_context, #UML_X86_64_CONTEXT_TLB]
	add x11, x11, x10, lsl #5
	.if \record_stack
	str x11, [_context, #UML_X86_64_CONTEXT_STACK_ENTRY]
	.endif
	ldp x12, x13, [x11, #\entry_offset]
	.if \access == 1
	bic x14, x12, #(UML_X86_64_TLB_WRITE_CODE | UML_X86_64_TLB_WRITE_CACHED_CODE)
	cmp x9, x14
	.else
	cmp x9, x12
	.endif
	b.eq Lx86_tlb_hit\@
	mov x0, _context
	mov x1, x7
	mov w2, #\access
	mov w3, #\size
	bl NAME(uml_x86_64_gadget_tlb_miss)
	cbz x0, Lx86_memory_fault
	mov x7, x0
	b Lx86_tlb_done\@
Lx86_tlb_hit\@:
	.if \access == 1
	/* Reading the context after the guest store costs a memory-disambiguation
	 * stall, and calling out to publication costs far more. Publication has
	 * work only when a publish is still pending or this page holds cached
	 * code, and neither depends on what the store writes, so both are settled
	 * here. `write_code` is none, pending or published and only pending is
	 * work, so its low bit answers for it and joins the entry's cached-code
	 * bit in one test; the tag's own bits are clear on a hit, because the
	 * compare above proved the tag is a page-aligned address once they are
	 * removed.
	 */
	ldrb w14, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	tbz x12, #1, Lx86_tlb_write_code_ready\@
	/* A test-and-branch reaches 32 KiB and the cold section is a bank away,
	 * so the rare arm is entered through an unconditional branch. */
	b Lx86_tlb_write_code_owed\@
Lx86_tlb_write_code_ready\@:
	orr w14, w14, w12, lsr #2
	tbnz w14, #0, Lx86_store_publishes\@
	orr x23, x23, #(1 << 60)
Lx86_store_publishes\@:
	.endif
	add x7, x13, x7
Lx86_tlb_done\@:
	.if \access == 1
	/* A store to a page some executor holds a translation of is the rare one,
	 * and settling it needs a second context field, so it leaves the hot
	 * stream: a bank of thousands of write translations otherwise fetches
	 * this arm on every store that will never take it.
	 *
	 * A publication covers a whole page, so such a store owes a record unless
	 * this program has already published this very page; a store to any other
	 * page owes its own, because the page stamp another vCPU validates
	 * against moves only with a publication. The page this program last
	 * published answers that on its own: it holds an impossible page until
	 * the first publication and again whenever a followed link retires this
	 * program's publications, which is exactly when `write_code` says nothing
	 * is owed.
	 */
	.pushsection __TEXT,__gadget_cold,regular,pure_instructions
Lx86_tlb_write_code_owed\@:
	ldr x14, [_context, #UML_X86_64_CONTEXT_CODE_PAGE]
	eor x14, x9, x14
	cbz x14, Lx86_tlb_write_code_kept\@
	mov w14, #UML_X86_64_WRITE_CODE_PENDING
	strb w14, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
Lx86_tlb_write_code_kept\@:
	ldrb w14, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	b Lx86_tlb_write_code_ready\@
	.popsection
	.endif
.endm

/* A probe that only translates, for the gather and masked-memory loops,
 * which translate element by element: it neither records the stack entry
 * nor settles a store's publication, and a miss the MMU refuses branches to
 * `fault`. A write probe compares the write page with its tag bits intact,
 * so a store to a page holding code always takes the miss path, where the
 * helper publishes it. Its index is translate's. */
.macro translate_plain access, entry_offset, size, fault
	x86_marks_guest_memory
	and x9, x7, #0xfffffffffffff000
	lsr x10, x7, #12
	eor x10, x10, x10, lsr #15
	and w10, w10, #0x7fff
	ldr x11, [_context, #UML_X86_64_CONTEXT_TLB]
	add x11, x11, x10, lsl #5
	ldp x12, x13, [x11, #\entry_offset]
	cmp x9, x12
	b.eq Lx86_plain_tlb_hit\@
	mov x0, _context
	mov x1, x7
	mov w2, #\access
	mov w3, #\size
	bl NAME(uml_x86_64_gadget_tlb_miss)
	cbz x0, \fault
	mov x7, x0
	b Lx86_plain_tlb_done\@
Lx86_plain_tlb_hit\@:
	add x7, x13, x7
Lx86_plain_tlb_done\@:
.endm

/* An access whose `size` bytes at guest address x25 cross a page goes
 * through the helpers, and a fault the MMU raises leaves through the shared
 * exit. A load returns the value zero-extended in x0. */
.macro cross_page_load size
	mov x0, _context
	mov x1, x25
	mov w2, #\size
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, Lx86_memory_fault
.endm

.macro cross_page_store value, size
	mov x0, _context
	mov x1, x25
	mov x2, \value
	mov w3, #\size
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, Lx86_memory_fault
.endm

/* Read a `size`-byte memory operand into register `number`: x7 holds its
 * address and x25 the copy the cross-page helper reads from. `load` and
 * `register` give the in-page load and its register width, and `move` the
 * width at which the helper's result is copied. */
.macro read_operand size, load, register, number, move=x
	page_cross_check \size, Lx86_read_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, \size
	\load \register\()\number, [x7]
	b Lx86_read_loaded\@
Lx86_read_cross\@:
	cross_page_load \size
	mov \move\()\number, \move\()0
Lx86_read_loaded\@:
.endm

/* x86 LOCK-prefixed read-modify-write.
 *
 * A locked gadget must be atomic against the other vCPU threads that execute
 * guest code concurrently, so it performs its update with an ARM64 exclusive
 * loop instead of a plain load and store.  x86 permits a locked operand at any
 * address.  An exclusive access needs natural alignment, which also keeps the
 * operand inside one page, so the exclusive path needs no page-crossing check
 * and a naturally aligned operand goes straight to it.
 *
 * Anything else reaches `split_operand`.  A host with FEAT_LSE2, which the
 * context reports, takes an exclusive access at any alignment inside one
 * 16-byte granule, so an operand that fits in one rejoins the exclusive path.
 * The rest -- an operand across a granule or a page, and every unaligned one
 * on a host without the feature -- takes the gadget's ordinary load and store
 * under the process-wide split lock.  That keeps it atomic against every other
 * operand on that path, which covers the same instruction running on the other
 * vCPUs, but not against an exclusive loop or a plain store that overlaps it.
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

/* The split lock brackets the ordinary load and store of an operand that left
 * the exclusive path.  Both ends are calls, so only callee-saved registers
 * survive them; x7 is restored from the guest address in x25.  A fault in
 * between reaches `Lx86_memory_fault`, which releases the lock.
 */
.macro split_lock_enter
	mov x0, _context
	bl NAME(uml_x86_64_gadget_split_lock_enter)
	mov x7, x25
.endm

.macro split_lock_leave
	mov x0, _context
	bl NAME(uml_x86_64_gadget_split_lock_leave)
.endm

/* The head of a locked gadget's fallback: x7 still holds the guest address,
 * and \exclusive is the gadget's exclusive path.  x9 is clobbered.
 */
.macro split_operand size, exclusive
	ldrb w9, [_context, #UML_X86_64_CONTEXT_UNALIGNED_EXCLUSIVES]
	cbz w9, Lx86_split_operand_locked\@
	add x9, x7, #((\size) - 1)
	eor x9, x9, x7
	tst x9, #0xfffffffffffffff0
	b.eq \exclusive
Lx86_split_operand_locked\@:
	split_lock_enter
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
	tbnz x23, #60, Lx86_code_published\@
	ldrb w8, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cbz w8, Lx86_code_published\@
	mov x0, _context
	mov x1, \address
	mov w2, #\size
	bl NAME(uml_x86_64_gadget_publish_code_write)
Lx86_code_published\@:
.endm

.macro finish_store size
	publish_pending_code x25, \size
Lx86_store_check_overlap\@:
	ldr x8, [_context, #UML_X86_64_CONTEXT_PROGRAM_END]
	cmp x25, x8
	b.hs Lx86_store_done\@
	add x9, x25, #\size
	ldr x10, [_context, #UML_X86_64_CONTEXT_PROGRAM_BEGIN]
	cmp x9, x10
	b.ls Lx86_store_done\@
	ldrb w8, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	cbnz w8, Lx86_store_code_exit\@
	mov x0, _context
	mov x1, x25
	bl NAME(uml_x86_64_gadget_invalidate_code)
Lx86_store_code_exit\@:
	ubfx x8, x23, #56, #4
	add x8, x22, x8
	str x8, [_cpu, #UML_X86_64_CPU_RIP]
	add _executed, _executed, #1
	x86_memory_order_boundary
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
Lx86_store_done\@:
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
	cbz w9, Lx86_link_code_ready\@
	mov x0, _context
	bl NAME(uml_x86_64_gadget_code_link_safe)
	cbz w0, Lx86_link_code_exit\@
	strb wzr, [_context, #UML_X86_64_CONTEXT_WRITE_CODE]
	/* Those publications are settled, so the page they covered is no longer
	 * one this program has published: the next store to it owes a record
	 * again, just as it did before this program made any. */
	mov w9, #UML_X86_64_CODE_PAGE_NONE
	str x9, [_context, #UML_X86_64_CONTEXT_CODE_PAGE]
Lx86_link_code_exit\@:
	cbnz w0, Lx86_link_code_ready\@
	ldr x9, [x19, #16]
	str x9, [_cpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
Lx86_link_code_ready\@:
	ldp _stream, x11, [x19]
	adds x12, _executed, x11
	b.le Lx86_link_follow\@
	ldr x9, [x19, #16]
	str x9, [_cpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
Lx86_link_follow\@:
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
	b.eq Lx86_indirect_follow\@
	ldp x11, x12, [\site, #16]
	cmp \target, x11
	b.eq Lx86_indirect_follow\@
	follow_target_link \target, Lx86_indirect_train\@
Lx86_indirect_train\@:
	str \site, [_context, #UML_X86_64_CONTEXT_INDIRECT_MISS]
	str \target, [_cpu, #UML_X86_64_CPU_RIP]
	mov w0, #UML_X86_64_EVENT_CONTINUED
	b uml_x86_64_gadget_leave
Lx86_indirect_follow\@:
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
	stp x18, x19, [sp, #-0xb0]!
	stp x20, x21, [sp, #0x10]
	stp x22, x23, [sp, #0x20]
	stp x24, x25, [sp, #0x30]
	stp x26, x27, [sp, #0x40]
	stp x28, x29, [sp, #0x50]
	str lr, [sp, #0x60]
	/* Gadgets use the whole vector file, and the procedure call standard
	 * makes the low halves of v8 to v15 the callee's to preserve. */
	stp d8, d9, [sp, #0x70]
	stp d10, d11, [sp, #0x80]
	stp d12, d13, [sp, #0x90]
	stp d14, d15, [sp, #0xa0]
	mov _stream, x0
	mov _cpu, x1
	mov _context, x2
	mov x9, sp
	str x9, [_context, #UML_X86_64_CONTEXT_ENTRY_STACK]
	/* Run the dispatch in the guest's rounding mode. */
	publish_guest_rounding 9, 10, 11
	/* The retirement counter carries this run's budget as a bias, so every
	 * linked edge tests the sign of one addition rather than loading the
	 * limit and comparing against it. `leave` takes the bias back out. */
	ldr x9, [_context, #UML_X86_64_CONTEXT_INSTRUCTION_LIMIT]
	neg _executed, x9
	x86_memory_order_boundary
	next_gadget

/* The instruction running raised an exception the guest unmasked, which the
 * engine does not deliver itself, and left before it committed anything: the
 * runtime runs it again, checked. A gadget can leave from inside a frame of
 * its own, so the exit takes the stack back to the entry frame, and it hands
 * the runtime its stream pointer, which is past a stream word of that
 * instruction and so names it. */
.p2align 4
uml_x86_64_gadget_exception_check:
	ldr x9, [_context, #UML_X86_64_CONTEXT_ENTRY_STACK]
	mov sp, x9
	str _stream, [_context, #UML_X86_64_CONTEXT_EXCEPTION_STREAM]
	mov w0, #UML_X86_64_EVENT_EXCEPTION_CHECK
	b uml_x86_64_gadget_leave

.p2align 4
uml_x86_64_gadget_leave:
	/* Some ways out of a gadget that has touched guest memory come straight
	 * here -- a repeated string that runs out of budget, a fallback after a
	 * link -- so leaving the engine always orders what came before it. */
	x86_memory_order_barrier
	/* Hand the caller back its own rounding mode, which is the host
	 * default, by clearing FPCR's mode field, so entry needs no slot for a
	 * saved copy.  Every path here has sp back at the entry frame: the
	 * callee-saved registers below are restored from it. */
	mrs x10, fpcr
	ubfx x11, x10, #22, #2
	cbz x11, Lx86_leave_rounding_ready
	bic x10, x10, #(3 << 22)
	msr fpcr, x10
Lx86_leave_rounding_ready:
	ldr x9, [_context, #UML_X86_64_CONTEXT_INSTRUCTION_LIMIT]
	add x1, _executed, x9
	ldp d14, d15, [sp, #0xa0]
	ldp d12, d13, [sp, #0x90]
	ldp d10, d11, [sp, #0x80]
	ldp d8, d9, [sp, #0x70]
	ldr lr, [sp, #0x60]
	ldp x28, x29, [sp, #0x50]
	ldp x26, x27, [sp, #0x40]
	ldp x24, x25, [sp, #0x30]
	ldp x22, x23, [sp, #0x20]
	ldp x20, x21, [sp, #0x10]
	ldp x18, x19, [sp], #0xb0
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
	cinc w15, w15, lo
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
	b Lx86_flags_apply_overrides
.endm

.macro shift_carry_body operation, width, base=UML_X86_64_CPU_DEFERRED_LEFT, exit=Lx86_carry_apply_overrides, load_flags=1
	add x6, _cpu, #\base
	ldp x7, x8, [x6]
	.if \load_flags
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	.endif
	bic x9, x9, #0x1
	shift_carry_bit \operation, \width
	tbz w14, #0, 4f
	orr x9, x9, #0x1
4:
	b \exit
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
	b.eq Lx86_flags_add
	cmp w5, #4
	b.eq Lx86_flags_add_preserve_carry
	cmp w5, #6
	b.eq Lx86_flags_add_carry
	cmp w5, #2
	b.eq Lx86_flags_sub
	cmp w5, #5
	b.eq Lx86_flags_sub_preserve_carry
	cmp w5, #7
	b.eq Lx86_flags_sub_borrow
	b Lx86_flags_apply_overrides

Lx86_flags_add:
	cmp x10, x7
	b.hs Lx86_flags_add_preserve_carry
	orr x9, x9, #0x1
Lx86_flags_add_preserve_carry:
	eor x13, x7, x8
	mvn x13, x13
	eor x14, x7, x10
	and x13, x13, x14
	flags_overflow_from 0, x13, Lx86_flags_add_aux
Lx86_flags_add_aux:
	b Lx86_flags_auxiliary

Lx86_flags_add_carry:
	flags_adjusted_carry 0, result
	cbz w15, Lx86_flags_add_preserve_carry
	orr x9, x9, #0x1
	b Lx86_flags_add_preserve_carry

Lx86_flags_sub:
	cmp x7, x8
	b.hs Lx86_flags_sub_preserve_carry
	orr x9, x9, #0x1
Lx86_flags_sub_preserve_carry:
	eor x13, x7, x8
	eor x14, x7, x10
	and x13, x13, x14
	flags_overflow_from 0, x13, Lx86_flags_sub_aux
Lx86_flags_sub_aux:
	b Lx86_flags_auxiliary

Lx86_flags_sub_borrow:
	flags_adjusted_carry 0, right
	cbz w15, Lx86_flags_sub_preserve_carry
	orr x9, x9, #0x1
	b Lx86_flags_sub_preserve_carry

Lx86_flags_auxiliary:
	eor x13, x7, x8
	eor x13, x13, x10
	tbz x13, #4, Lx86_flags_auxiliary_done
	orr x9, x9, #0x10
Lx86_flags_auxiliary_done:
	sub w13, w5, #4
	cmp w13, #1
	b.hi Lx86_flags_apply_overrides
	ldrb w16, [_cpu, #UML_X86_64_CPU_DEFERRED_PENDING]
	cbz w16, Lx86_flags_apply_overrides
	adr x17, Lx86_flags_apply_overrides
	b Lx86_slot_carry
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
	.if ((\operation) == 4) || ((\operation) == 5)
	/* The preserved carry may still be pending in the slot. */
	ldrb w16, [_cpu, #UML_X86_64_CPU_DEFERRED_PENDING]
	cbz w16, Lx86_flags_apply_overrides
	adr x17, Lx86_flags_apply_overrides
	b Lx86_slot_carry
	.endif
	b Lx86_flags_apply_overrides
	.endif
.endm

.p2align 4
uml_x86_64_gadget_materialize_flags:
	ldrb w5, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	cbnz w5, Lx86_flags_dispatch
	ldrh w13, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	cbz w13, 9f
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	b Lx86_flags_apply_overrides
Lx86_flags_dispatch:
	/* Masking both keys keeps the branch inside the table whatever the
	 * recorded bytes hold; the slots those values reach are the generic body.
	 */
	ldrb w11, [_cpu, #UML_X86_64_CPU_DEFERRED_WIDTH]
	adr x14, Lx86_flags_table
	and w12, w5, #15
	and w13, w11, #0xf
	add x14, x14, x12, lsl #6
	add x14, x14, x13, lsl #2
	br x14

.p2align 2
Lx86_flags_table:
.irp op,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
.irp w,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	.if ((\op) >= 1) && ((\op) <= 10) && (((\w) == 1) || ((\w) == 2) || ((\w) == 4) || ((\w) == 8))
	b Lx86_flags_o\op\()_w\w
	.else
	b Lx86_flags_generic
	.endif
.endr
.endr

.irp op,1,2,3,4,5,6,7
.irp w,1,2,4,8
Lx86_flags_o\op\()_w\w:
	flags_body \op, \w
.endr
.endr

.irp op,8,9,10
.irp w,1,2,4,8
Lx86_flags_o\op\()_w\w:
	shift_flags_body \op, \w
.endr
.endr

Lx86_flags_generic:
	flags_body 0, 0

Lx86_flags_apply_overrides:
	ldrh w13, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	cbz w13, Lx86_flags_store
	ldrh w14, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_VALUE]
	bic x9, x9, x13
	and x14, x14, x13
	orr x9, x9, x14
Lx86_flags_store:
	orr x9, x9, #0x2
	str x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	str xzr, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
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
.macro carry_body operation, width, base=UML_X86_64_CPU_DEFERRED_LEFT, exit=Lx86_carry_apply_overrides, load_flags=1
	.if ((\operation) == 4) || ((\operation) == 5)
	/* INC and DEC preserve carry, which may still be pending in the slot.
	 * The slot itself never holds one of their records. */
	.if \load_flags
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	ldrb w16, [_cpu, #UML_X86_64_CPU_DEFERRED_PENDING]
	cbz w16, \exit
	adr x17, \exit
	b Lx86_slot_carry
	.else
	b \exit
	.endif
	.elseif (\operation) == 3
	/* Logical operations clear carry. */
	.if \load_flags
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	.endif
	bic x9, x9, #0x1
	b \exit
	.else
	add x6, _cpu, #\base
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
	.if \load_flags
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	.endif
	.if (\operation) == 0
	/* INC, DEC and an unrecognized operation reach this body with a
	 * run-time key; only the four defining operations clear carry. */
	cmp w5, #4
	b.lo 15f
	cmp w5, #5
	b.hi 15f
	.if \load_flags
	ldrb w16, [_cpu, #UML_X86_64_CPU_DEFERRED_PENDING]
	cbz w16, \exit
	adr x17, \exit
	b Lx86_slot_carry
	.else
	b \exit
	.endif
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
	b \exit
16:
	cmp x10, x7
	b.hs \exit
	orr x9, x9, #0x1
	b \exit
17:
	cmp x7, x8
	b.hs \exit
	orr x9, x9, #0x1
	b \exit
18:
	flags_adjusted_carry 0, result
	cbz w15, \exit
	orr x9, x9, #0x1
	b \exit
19:
	flags_adjusted_carry 0, right
	cbz w15, \exit
	orr x9, x9, #0x1
	b \exit
	.else
	bic x9, x9, #0x1
	.if (\operation) == 1
	cmp x10, x7
	b.hs \exit
	orr x9, x9, #0x1
	.elseif (\operation) == 2
	cmp x7, x8
	b.hs \exit
	orr x9, x9, #0x1
	.elseif (\operation) == 6
	flags_adjusted_carry \width, result
	cbz w15, \exit
	orr x9, x9, #0x1
	.elseif (\operation) == 7
	flags_adjusted_carry \width, right
	cbz w15, \exit
	orr x9, x9, #0x1
	.endif
	b \exit
	.endif
	.endif
.endm

.p2align 4
uml_x86_64_gadget_materialize_carry:
	ldrb w5, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	cbnz w5, Lx86_carry_dispatch
	ldrh w13, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	cbz w13, 9f
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	b Lx86_carry_apply_overrides
Lx86_carry_dispatch:
	ldrb w11, [_cpu, #UML_X86_64_CPU_DEFERRED_WIDTH]
	adr x14, Lx86_carry_table
	and w12, w5, #15
	and w13, w11, #0xf
	add x14, x14, x12, lsl #6
	add x14, x14, x13, lsl #2
	br x14

.p2align 2
Lx86_carry_table:
.irp op,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
.irp w,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	.if ((\op) >= 1) && ((\op) <= 10) && (((\w) == 1) || ((\w) == 2) || ((\w) == 4) || ((\w) == 8))
	b Lx86_carry_o\op\()_w\w
	.else
	b Lx86_carry_generic
	.endif
.endr
.endr

.irp op,1,2,3,4,5,6,7
.irp w,1,2,4,8
Lx86_carry_o\op\()_w\w:
	carry_body \op, \w
.endr
.endr

.irp op,8,9,10
.irp w,1,2,4,8
Lx86_carry_o\op\()_w\w:
	shift_carry_body \op, \w
.endr
.endr

Lx86_carry_generic:
	carry_body 0, 0

Lx86_carry_apply_overrides:
	ldrh w13, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	cbz w13, Lx86_carry_store
	ldrh w14, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_VALUE]
	bic x9, x9, x13
	and x14, x14, x13
	orr x9, x9, x14
Lx86_carry_store:
	orr x9, x9, #0x2
	str x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	str xzr, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
9:
	ret

/* The carry a pending INC or DEC record preserves, from the record in the
 * carry slot: the same operation-and-width table over the slot, computing
 * into the flags the caller is building in x9 and returning through x17. Only
 * the slot record's carry override applies; its other flags died when the
 * INC or DEC recorded. Both materializers reach this with x5 to x16 free.
 */
.p2align 4
Lx86_slot_carry:
	ldrb w5, [_cpu, #(UML_X86_64_CPU_CARRY_SLOT + 24)]
	ldrb w11, [_cpu, #(UML_X86_64_CPU_CARRY_SLOT + 25)]
	adr x14, Lx86_slot_table
	and w12, w5, #15
	and w13, w11, #0xf
	add x14, x14, x12, lsl #6
	add x14, x14, x13, lsl #2
	br x14

.p2align 2
Lx86_slot_table:
.irp op,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
.irp w,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	.if ((\op) >= 1) && ((\op) <= 10) && (((\w) == 1) || ((\w) == 2) || ((\w) == 4) || ((\w) == 8))
	b Lx86_slot_o\op\()_w\w
	.else
	b Lx86_slot_generic
	.endif
.endr
.endr

.irp op,1,2,3,4,5,6,7
.irp w,1,2,4,8
Lx86_slot_o\op\()_w\w:
	carry_body \op, \w, UML_X86_64_CPU_CARRY_SLOT, Lx86_slot_apply_overrides, 0
.endr
.endr

.irp op,8,9,10
.irp w,1,2,4,8
Lx86_slot_o\op\()_w\w:
	shift_carry_body \op, \w, UML_X86_64_CPU_CARRY_SLOT, Lx86_slot_apply_overrides, 0
.endr
.endr

Lx86_slot_generic:
	carry_body 0, 0, UML_X86_64_CPU_CARRY_SLOT, Lx86_slot_apply_overrides, 0

Lx86_slot_apply_overrides:
	ldrh w13, [_cpu, #(UML_X86_64_CPU_CARRY_SLOT + 28)]
	tbz w13, #0, 9f
	ldrh w14, [_cpu, #(UML_X86_64_CPU_CARRY_SLOT + 30)]
	bfi x9, x14, #0, #1
9:
	br x17


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

/* Write the vector result in q4 (and q5 for 256 bits) to the register the
 * descriptor's low half addresses. A 128-bit VEX write, which bit 63 marks,
 * zeroes the upper half; a legacy SSE write leaves it. */
.macro vector_write_xmm width, descriptor
	and x9, x\descriptor, #0xffff
	add x9, _cpu, x9
	str q4, [x9]
	.if \width == 256
	str q5, [x9, #16]
	.else
	tbz x\descriptor, #63, Lx86_vector_write_done\@
	movi v6.16b, #0
	str q6, [x9, #16]
Lx86_vector_write_done\@:
	.endif
.endm

/* An MMX write, as on x86, also stores `value` into the x87 register the MMX
 * register at state offset `offset` aliases, with an all-ones exponent, and
 * clears the x87 tag word and top. `index`, `entry` and `tag` are scratch. */
.macro synchronize_mmx offset, value, index=x10, entry=x11, tag=w12
	sub \index, \offset, #UML_X86_64_CPU_MMX
	lsr \index, \index, #3
	add \entry, _cpu, #UML_X86_64_CPU_X87
	add \entry, \entry, \index, lsl #4
	str \value, [\entry]
	mov \tag, #0xffff
	strh \tag, [\entry, #8]
	strh wzr, [_cpu, #UML_X86_64_CPU_X87_TAG]
	strb wzr, [_cpu, #UML_X86_64_CPU_X87_TOP]
.endm

/* vector_write_xmm, or for a 64-bit width an MMX write of d4. */
.macro vector_write width, descriptor
	.if \width == 64
	and x9, x\descriptor, #0xffff
	str d4, [_cpu, x9]
	synchronize_mmx x9, d4
	.else
	vector_write_xmm \width, \descriptor
	.endif
.endm

/* Lane helpers of the three-operand map gadgets: the byte index sequence
 * 0..15, and a lane mask from bits `first` upward of w13 or x13. */
.macro vector_map3_index_sequence output
	mov x14, #0x0100
	movk x14, #0x0302, lsl #16
	movk x14, #0x0504, lsl #32
	movk x14, #0x0706, lsl #48
	fmov d\output, x14
	mov x14, #0x0908
	movk x14, #0x0b0a, lsl #16
	movk x14, #0x0d0c, lsl #32
	movk x14, #0x0f0e, lsl #48
	mov v\output\().d[1], x14
.endm
.macro vector_map3_mask_s output, first
	movi v\output\().16b, #0
	.irp lane, 0, 1, 2, 3
	ubfx w14, w13, #(\first + \lane), #1
	neg w14, w14
	mov v\output\().s[\lane], w14
	.endr
.endm
.macro vector_map3_mask_d output, first
	movi v\output\().16b, #0
	.irp lane, 0, 1
	ubfx x14, x13, #(\first + \lane), #1
	neg x14, x14
	mov v\output\().d[\lane], x14
	.endr
.endm
