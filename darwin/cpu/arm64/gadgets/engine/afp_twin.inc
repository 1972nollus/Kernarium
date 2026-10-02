/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FEAT_AFP twins. Where the virtual CPU implements FEAT_AFP (the
 * implements_afp() predicate), the builder runs a twin in place of each entry
 * whose code would ignore FPCR.FIZ, AH or NEP: a scalar result the host
 * instruction writes over a cleared register instead of the architectural
 * merge source, a sign operation outside the guest FP environment, a software
 * model without the alternative controls, or an FPCR access masked without
 * them. The twins live in their own section and carry no forwarding variants,
 * so no established entry moves and other hosts translate exactly as before.
 * Each row of the table below maps one entry, or one equal-stride
 * fixed-point table, to its twin. */
#if __APPLE__
.pushsection __DATA_CONST,__gadget_afp
#else
.pushsection .data.rel.ro.gadget_afp,"aw"
#endif
.p2align 3
.globl NAME(uml_arm64_afp_twin_begin)
NAME(uml_arm64_afp_twin_begin):
.popsection

.macro afp_twins_begin
#if __APPLE__
	.pushsection __TEXT,__gadget_afp,regular,pure_instructions
#else
	.pushsection .text.gadget_afp,"ax"
#endif
.endm

.macro afp_twins_end
	.popsection
.endm

/* Entries [base_begin, base_end) run as [twin_begin, twin_end). */
.macro afp_twin_row base_begin, base_end, twin_begin, twin_end
#if __APPLE__
	.pushsection __DATA_CONST,__gadget_afp
#else
	.pushsection .data.rel.ro.gadget_afp,"aw"
#endif
	.quad \base_begin, \base_end, \twin_begin, \twin_end
	.popsection
.endm

/* The twin of one entry. Gadgets are 16-byte aligned, so a four-byte range
 * names exactly one. */
.macro afp_gadget name
	gadget \name\()_afp
	afp_twin_row NAME(uml_arm64_gadget_\name), NAME(uml_arm64_gadget_\name) + 4, NAME(uml_arm64_gadget_\name\()_afp), NAME(uml_arm64_gadget_\name\()_afp) + 4
.endm

/* The row of a fixed-point table whose twin is \name\()_afp. */
.macro afp_table_row name
	afp_twin_row NAME(uml_arm64_gadget_\name), NAME(uml_arm64_gadget_\name\()_end), NAME(uml_arm64_gadget_\name\()_afp), NAME(uml_arm64_gadget_\name\()_afp_end)
.endm

.macro afp_twin_table_end
#if __APPLE__
	.pushsection __DATA_CONST,__gadget_afp
#else
	.pushsection .data.rel.ro.gadget_afp,"aw"
#endif
	.globl NAME(uml_arm64_afp_twin_end)
NAME(uml_arm64_afp_twin_end):
	.popsection
.endm
