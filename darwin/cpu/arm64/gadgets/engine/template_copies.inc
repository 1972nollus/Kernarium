/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Byte-identical copies of the ordinary ADD/SUB-immediate entry, selected by
 * the guest destination register.
 *
 * A template addresses its operand through a register index, so every guest
 * register that flows through one entry shares that entry's load and store
 * program counters. The host's memory-dependence predictor is keyed on those
 * program counters, not on the address, so it learns one relationship for all
 * of them and serializes chains that are independent. Measured on this host
 * with a threaded-code model of this entry: two independent chains cost 5.3
 * cycles per gadget through one body and 3.4 through two byte-identical
 * copies, four chains 4.1 against 3.0, converging once about six chains are in
 * flight.
 *
 * The copies are identical instruction for instruction; only their addresses
 * differ, which is the whole mechanism. Selecting on the destination register
 * modulo the copy count keeps the choice free at translation time and gives
 * neighbouring registers distinct entries.
 *
 * Appended after the established bank so no existing entry moves or changes
 * its 64-byte phase.
 */
#if UML_GADGET_TEMPLATE_COPIES > 1

/* Same body as add_sub_immediate_fast's _rr mode: ordinary destination,
 * ordinary source, register-indexed on both sides. */
.macro add_sub_immediate_copy_entry bits64, subtract, flags, copy, suffix, forward
scalar_entry add_sub_immediate_copy\copy\()_\suffix, \forward, 1, scalar_register0, scalar_register8, scalar_none, scalar_none, 1
#if UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST
	ldp x8, x14, [_stream], #16
#else
	ldr x8, [_stream], #8
#endif
	scalar_load \forward, 1, x15, 10, 8, 8, 5, 3, _cpu
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
	and w9, w8, #0x1f
	str _scalar_x16, [_cpu, x9, lsl #3]
#if UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST
	br x14
#else
	next_gadget
#endif
.endm

.macro add_sub_immediate_copy_pair bits64, subtract, flags, copy, suffix
add_sub_immediate_copy_entry \bits64, \subtract, \flags, \copy, \suffix, 0
#if UML_GADGET_SCALAR_FORWARDING
add_sub_immediate_copy_entry \bits64, \subtract, \flags, \copy, \suffix, 1
#endif
.endm

/* Copy 0 is the established entry, so only copies 1 and up are emitted. An
 * .irp counter cannot be used here: the enclosing macro expansion consumes the
 * \() separator and leaves the loop variable glued to the next token. */
.macro add_sub_immediate_copies bits64, subtract, flags, prefix
add_sub_immediate_copy_pair \bits64, \subtract, \flags, 1, \prefix
#if UML_GADGET_TEMPLATE_COPIES > 2
add_sub_immediate_copy_pair \bits64, \subtract, \flags, 2, \prefix
add_sub_immediate_copy_pair \bits64, \subtract, \flags, 3, \prefix
#endif
.endm

add_sub_immediate_copies 0, 0, 0, w_add_n
add_sub_immediate_copies 0, 0, 1, w_add_s
add_sub_immediate_copies 0, 1, 0, w_sub_n
add_sub_immediate_copies 0, 1, 1, w_sub_s
add_sub_immediate_copies 1, 0, 0, x_add_n
add_sub_immediate_copies 1, 0, 1, x_add_s
add_sub_immediate_copies 1, 1, 0, x_sub_n
add_sub_immediate_copies 1, 1, 1, x_sub_s

/* One flat table so the translator indexes rather than switches:
 * index = (((bits64 * 2 + subtract) * 2 + flags) * UML_GADGET_TEMPLATE_COPIES)
 * + copy, with copy 0 naming the established entry. */
.macro add_sub_immediate_copy_row prefix
	.quad NAME(uml_arm64_gadget_add_sub_immediate_\prefix\()_rr)
	.quad NAME(uml_arm64_gadget_add_sub_immediate_copy1_\prefix)
#if UML_GADGET_TEMPLATE_COPIES > 2
	.quad NAME(uml_arm64_gadget_add_sub_immediate_copy2_\prefix)
	.quad NAME(uml_arm64_gadget_add_sub_immediate_copy3_\prefix)
#endif
.endm

#if __APPLE__
.pushsection __DATA_CONST,__gadget_copies
#else
.pushsection .data.rel.ro.gadget_copies,"aw"
#endif
.p2align 3
.globl NAME(uml_arm64_gadget_add_sub_immediate_copy_table)
NAME(uml_arm64_gadget_add_sub_immediate_copy_table):
add_sub_immediate_copy_row w_add_n
add_sub_immediate_copy_row w_add_s
add_sub_immediate_copy_row w_sub_n
add_sub_immediate_copy_row w_sub_s
add_sub_immediate_copy_row x_add_n
add_sub_immediate_copy_row x_add_s
add_sub_immediate_copy_row x_sub_n
add_sub_immediate_copy_row x_sub_s
.popsection

.purgem add_sub_immediate_copy_row
.purgem add_sub_immediate_copies
.purgem add_sub_immediate_copy_pair
.purgem add_sub_immediate_copy_entry

#endif
