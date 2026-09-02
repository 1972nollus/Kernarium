/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bit test/update and scalar bit count operations. */


.macro bit_update_carry
	bl uml_x86_64_gadget_materialize_flags
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	bic x8, x8, #1
	orr x8, x8, x19
	orr x8, x8, #0x2
	str x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
.endm

/* x24 is the input value and w26 the already-masked bit number. */
.macro bit_compute operation, width, host_register
	mov \host_register\()8, #1
	lslv \host_register\()8, \host_register\()8, \host_register\()26
	lsrv \host_register\()9, \host_register\()24, \host_register\()26
	and w19, w9, #1
	.ifc \operation,bt
	mov \host_register\()27, \host_register\()24
	.endif
	.ifc \operation,bts
	orr \host_register\()27, \host_register\()24, \host_register\()8
	.endif
	.ifc \operation,btr
	bic \host_register\()27, \host_register\()24, \host_register\()8
	.endif
	.ifc \operation,btc
	eor \host_register\()27, \host_register\()24, \host_register\()8
	.endif
	normalize_result \width, \host_register\()27
.endm

.macro bit_register operation, width, load, store, host_register, store_register, form
gadget \operation\()_\width\()_r\form
	.ifc \form,i
	ldp x25, x26, [_stream], #16
	.else
	ldr x8, [_stream], #8
	and x25, x8, #0xffff
	ubfx x9, x8, #16, #16
	\load \host_register\()26, [_cpu, x9]
	.endif
	\load \host_register\()24, [_cpu, x25]
	and w26, w26, #(\width - 1)
	bit_compute \operation, \width, \host_register
	.ifnc \operation,bt
	\store \store_register\()27, [_cpu, x25]
	.endif
	bit_update_carry
	retire_and_next
.endm

.macro bit_memory operation, width, load, store, host_register, form, locked
	.if \locked
gadget \operation\()_\width\()_m\form\()_locked
	.else
gadget \operation\()_\width\()_m\form
	.endif
	effective_address
	.ifc \form,i
	ldr x26, [_stream], #8
	and w26, w26, #(\width - 1)
	.else
	and x8, x23, #0xffff
	\load \host_register\()26, [_cpu, x8]
	.if \width == 16
	sxth x9, w26
	.elseif \width == 32
	sxtw x9, w26
	.else
	mov x9, x26
	.endif
	and w26, w26, #(\width - 1)
	.if \width == 16
	asr x9, x9, #4
	add x7, x7, x9, lsl #1
	.elseif \width == 32
	asr x9, x9, #5
	add x7, x7, x9, lsl #2
	.else
	asr x9, x9, #6
	add x7, x7, x9, lsl #3
	.endif
	.endif
	mov x25, x7
	.if \locked
	locked_aligned \width, .Lx86_bit_split\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
1:
	load_exclusive \width, 24, x7
	bit_compute \operation, \width, \host_register
	store_exclusive \width, 9, 27, x7
	cbnz w9, 1b
	bit_update_carry
	finish_store (\width / 8)
.Lx86_bit_split\@:
	.endif
	page_cross_check (\width / 8), .Lx86_bit_read_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
	b .Lx86_bit_loaded\@
.Lx86_bit_read_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
.Lx86_bit_loaded\@:
	bit_compute \operation, \width, \host_register
	.ifc \operation,bt
	bit_update_carry
	retire_and_next
	.else
	mov x7, x25
	page_cross_check (\width / 8), .Lx86_bit_write_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()27, [x7]
	b .Lx86_bit_written\@
.Lx86_bit_write_cross\@:
	mov x0, _context
	mov x1, x25
	mov x2, x27
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_store)
	cbz w0, .Lx86_memory_fault
.Lx86_bit_written\@:
	bit_update_carry
	finish_store (\width / 8)
	.endif
.endm

.macro bit_width operation, width, load, store, host_register, store_register
	bit_register \operation, \width, \load, \store, \host_register, \store_register, r
	bit_register \operation, \width, \load, \store, \host_register, \store_register, i
	bit_memory \operation, \width, \load, \store, \host_register, r, 0
	bit_memory \operation, \width, \load, \store, \host_register, i, 0
	.ifnc \operation,bt
	bit_memory \operation, \width, \load, \store, \host_register, r, 1
	bit_memory \operation, \width, \load, \store, \host_register, i, 1
	.endif
.endm

.macro bit_family operation
	bit_width \operation, 16, ldrh, strh, w, w
	bit_width \operation, 32, ldr, str, w, x
	bit_width \operation, 64, ldr, str, x, x
.endm

bit_family bt
bit_family bts
bit_family btr
bit_family btc

.purgem bit_family
.purgem bit_width
.purgem bit_memory
.purgem bit_register
.purgem bit_compute
.purgem bit_update_carry

/* Scalar scans/counts use x24 for the source, x25 for the destination offset,
 * and x27 for the result.
 */
.macro bit_count_compute operation, width, host_register
	.ifc \operation,bsf
	rbit \host_register\()27, \host_register\()24
	clz \host_register\()27, \host_register\()27
	.endif
	.ifc \operation,bsr
	clz \host_register\()27, \host_register\()24
	mov \host_register\()8, #(\width - 1)
	.if \width == 16
	sub \host_register\()27, \host_register\()8, \host_register\()27
	add \host_register\()27, \host_register\()27, #16
	.else
	sub \host_register\()27, \host_register\()8, \host_register\()27
	.endif
	.endif
	.ifc \operation,tzcnt
	cbz \host_register\()24, .Lx86_bit_count_zero\@
	rbit \host_register\()27, \host_register\()24
	clz \host_register\()27, \host_register\()27
	b .Lx86_bit_count_ready\@
.Lx86_bit_count_zero\@:
	mov \host_register\()27, #\width
.Lx86_bit_count_ready\@:
	.endif
	.ifc \operation,lzcnt
	cbz \host_register\()24, .Lx86_bit_count_zero\@
	clz \host_register\()27, \host_register\()24
	.if \width == 16
	sub \host_register\()27, \host_register\()27, #16
	.endif
	b .Lx86_bit_count_ready\@
.Lx86_bit_count_zero\@:
	mov \host_register\()27, #\width
.Lx86_bit_count_ready\@:
	.endif
	.ifc \operation,popcnt
	fmov d0, x24
	cnt v0.8b, v0.8b
	addv b0, v0.8b
	umov w27, v0.b[0]
	.endif
.endm

.macro bit_count_flags operation, host_register
	.ifc \operation,bsf
	bl uml_x86_64_gadget_materialize_flags
	.endif
	.ifc \operation,bsr
	bl uml_x86_64_gadget_materialize_flags
	.endif
	.ifnc \operation,bsf
	.ifnc \operation,bsr
	/* TZCNT, LZCNT and POPCNT define every arithmetic flag below, and the
	 * only overrides any gadget records lie inside that same set, so a
	 * pending record has nothing left to contribute. Discard it rather than
	 * materializing a value this macro immediately clears.
	 */
	strb wzr, [_cpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	str wzr, [_cpu, #UML_X86_64_CPU_DEFERRED_OVERRIDE_MASK]
	.endif
	.endif
	ldr x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
	.ifc \operation,bsf
	bic x8, x8, #0x40
	cbnz \host_register\()24, .Lx86_bit_count_flags_done\@
	orr x8, x8, #0x40
	.endif
	.ifc \operation,bsr
	bic x8, x8, #0x40
	cbnz \host_register\()24, .Lx86_bit_count_flags_done\@
	orr x8, x8, #0x40
	.endif
	.ifnc \operation,bsf
	.ifnc \operation,bsr
	mov x9, #0x8d5
	bic x8, x8, x9
	.ifc \operation,popcnt
	cbnz \host_register\()24, .Lx86_bit_count_flags_done\@
	orr x8, x8, #0x40
	.else
	cbnz \host_register\()24, .Lx86_bit_count_source_nonzero\@
	orr x8, x8, #1
.Lx86_bit_count_source_nonzero\@:
	cbnz \host_register\()27, .Lx86_bit_count_flags_done\@
	orr x8, x8, #0x40
	.endif
	.endif
	.endif
.Lx86_bit_count_flags_done\@:
	orr x8, x8, #0x2
	str x8, [_cpu, #UML_X86_64_CPU_RFLAGS]
.endm

.macro bit_count_finish operation, width, store, host_register, store_register
	.ifc \operation,bsf
	cbz \host_register\()24, .Lx86_bit_count_no_write\@
	.endif
	.ifc \operation,bsr
	cbz \host_register\()24, .Lx86_bit_count_no_write\@
	.endif
	\store \store_register\()27, [_cpu, x25]
.Lx86_bit_count_no_write\@:
	bit_count_flags \operation, \host_register
	retire_and_next
.endm

.macro bit_count_register operation, width, load, store, host_register, store_register
gadget \operation\()_\width\()_rr
	ldr x8, [_stream], #8
	and x25, x8, #0xffff
	ubfx x9, x8, #16, #16
	\load \host_register\()24, [_cpu, x9]
	bit_count_compute \operation, \width, \host_register
	bit_count_finish \operation, \width, \store, \host_register, \store_register
.endm

.macro bit_count_memory operation, width, load, store, host_register, store_register
gadget \operation\()_\width\()_rm
	effective_address
	mov x25, x7
	page_cross_check (\width / 8), .Lx86_bit_count_read_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	\load \host_register\()24, [x7]
	b .Lx86_bit_count_loaded\@
.Lx86_bit_count_read_cross\@:
	mov x0, _context
	mov x1, x25
	mov w2, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_load)
	cbz x1, .Lx86_memory_fault
	mov x24, x0
.Lx86_bit_count_loaded\@:
	and x25, x23, #0xffff
	bit_count_compute \operation, \width, \host_register
	bit_count_finish \operation, \width, \store, \host_register, \store_register
.endm

.macro bit_count_width operation, width, load, store, host_register, store_register
	bit_count_register \operation, \width, \load, \store, \host_register, \store_register
	bit_count_memory \operation, \width, \load, \store, \host_register, \store_register
.endm

.macro bit_count_family operation
	bit_count_width \operation, 16, ldrh, strh, w, w
	bit_count_width \operation, 32, ldr, str, w, x
	bit_count_width \operation, 64, ldr, str, x, x
.endm

bit_count_family bsf
bit_count_family bsr
bit_count_family tzcnt
bit_count_family lzcnt
bit_count_family popcnt

.purgem bit_count_family
.purgem bit_count_width
.purgem bit_count_memory
.purgem bit_count_register
.purgem bit_count_finish
.purgem bit_count_flags
.purgem bit_count_compute
