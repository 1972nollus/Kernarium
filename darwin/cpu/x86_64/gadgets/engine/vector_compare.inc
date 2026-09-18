/* SPDX-License-Identifier: GPL-2.0-or-later */
/* COMISS/UCOMISS and COMISD/UCOMISD architectural flags. */

.macro vector_compare_set_flags
	ldr x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	mov x10, #0x8d5
	bic x9, x9, x10
	orr x9, x9, #2
	b.vs .Lx86_vector_compare_unordered\@
	b.eq .Lx86_vector_compare_equal\@
	b.mi .Lx86_vector_compare_less\@
	b .Lx86_vector_compare_done\@
.Lx86_vector_compare_unordered\@:
	orr x9, x9, #1
	orr x9, x9, #4
	orr x9, x9, #0x40
	b .Lx86_vector_compare_done\@
.Lx86_vector_compare_equal\@:
	orr x9, x9, #0x40
	b .Lx86_vector_compare_done\@
.Lx86_vector_compare_less\@:
	orr x9, x9, #1
.Lx86_vector_compare_done\@:
	str x9, [_cpu, #UML_X86_64_CPU_RFLAGS]
	retire_and_next
.endm

.macro vector_compare_register width, fp_register
gadget vector_compare_\width\()_rr
	/* Every arithmetic flag is defined below, and the only overrides a gadget
	 * records lie inside that same set, so a pending record has nothing left
	 * to contribute: discard it instead of materializing a value this
	 * gadget immediately overwrites. */
	str xzr, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ldr \fp_register\()0, [_cpu, x9]
	ldr \fp_register\()1, [_cpu, x10]
	fcmp \fp_register\()0, \fp_register\()1
	vector_compare_set_flags
.endm

.macro vector_compare_memory width, fp_register, load, host_register
gadget vector_compare_\width\()_rm
	/* Every arithmetic flag is defined below, and the only overrides a gadget
	 * records lie inside that same set, so a pending record has nothing left
	 * to contribute: discard it instead of materializing a value this
	 * gadget immediately overwrites. */
	str xzr, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	effective_address
	mov x25, x7
	page_cross_check (\width / 8), .Lx86_vector_compare_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	ldr \fp_register\()1, [x7]
	b .Lx86_vector_compare_loaded\@
.Lx86_vector_compare_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	fmov \fp_register\()1, \host_register\()0
.Lx86_vector_compare_loaded\@:
	and x8, x23, #0xffff
	ldr \fp_register\()0, [_cpu, x8]
	fcmp \fp_register\()0, \fp_register\()1
	vector_compare_set_flags
.endm

vector_compare_register 32, s
vector_compare_register 64, d
vector_compare_memory 32, s, ldr, w
vector_compare_memory 64, d, ldr, x

.purgem vector_compare_memory
.purgem vector_compare_register
.purgem vector_compare_set_flags
