gadget nop
	next_gadget

/* Literal form invariants are resolved while translating. Operand bits 15:0
 * are the CpuState destination byte offset; bits 63:16 are the signed PC
 * displacement. The following word is the guest PC used for address formation
 * and precise memory faults. */
.macro literal_integer_fast name, instruction, host_register, bytes, extension, discard
gadget \name
	ldr x8, [_stream]
	ldr x9, [_stream, #8]
	and x19, x8, #0xffff
	asr x10, x8, #16
	add x7, x9, x10
	and x11, x7, #0xfff
	cmp x11, #(4096 - \bytes)
	b.hi .Lliteral_integer_fast_cross\@
	add _stream, _stream, #8
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()0, [x7]
	.if !\discard
	str x0, [_cpu, x19]
	.endif
	add _stream, _stream, #8
	next_gadget

.Lliteral_integer_fast_cross\@:
	add _stream, _stream, #8
	transfer_slow 0, x7, \bytes
	.if \bytes == 8
	fmov x0, d0
	.else
	fmov w0, s0
	.endif
	.if \extension
	sxtw x0, w0
	.endif
	.if !\discard
	str x0, [_cpu, x19]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro literal_integer_modes name, instruction, host_register, bytes, extension
	literal_integer_fast \name\()_r, \instruction, \host_register, \bytes, \extension, 0
	literal_integer_fast \name\()_z, \instruction, \host_register, \bytes, \extension, 1
.endm

literal_integer_modes literal_load32_fast, ldr, w, 4, 0
literal_integer_modes literal_load64_fast, ldr, x, 8, 0
literal_integer_modes literal_load32_signed_fast, ldrsw, x, 4, 1

.purgem literal_integer_modes
.purgem literal_integer_fast

.macro literal_vector_fast name, instruction, host_register, bytes
gadget \name
	ldr x8, [_stream]
	ldr x9, [_stream, #8]
	and x19, x8, #0xffff
	asr x10, x8, #16
	add x7, x9, x10
	and x11, x7, #0xfff
	cmp x11, #(4096 - \bytes)
	b.hi .Lliteral_vector_fast_cross\@
	add _stream, _stream, #8
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()0, [x7]
	str q0, [_cpu, x19]
	add _stream, _stream, #8
	next_gadget

.Lliteral_vector_fast_cross\@:
	add _stream, _stream, #8
	transfer_slow 0, x7, \bytes
	str q0, [_cpu, x19]
	add _stream, _stream, #8
	next_gadget
.endm

literal_vector_fast literal_load_vector32_fast, ldr, s, 4
literal_vector_fast literal_load_vector64_fast, ldr, d, 8
literal_vector_fast literal_load_vector128_fast, ldr, q, 16

.purgem literal_vector_fast

/* Production word/doubleword unsigned-immediate transfers.  The translator
 * converts both register numbers to CpuState byte offsets and folds XZR into
 * a distinct entry. Stream: [Rt offset, Rn offset, byte displacement][PC].
 * The only conditional branches left are the runtime cross-page and TLB/fault
 * decisions; valid same-page hits execute no opcode or register-mode tests. */
.macro scalar_unsigned_fast load, bits64, transfer_zero, suffix
gadget \suffix
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	lsr x21, x8, #32
	ldr x7, [_cpu, x20]
	add x7, x7, x21
	and x9, x7, #0xfff
	.if \bits64
	cmp x9, #4088
	.else
	cmp x9, #4092
	.endif
	b.hi .Lscalar_unsigned_cross\@
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
		.if \bits64
	ldr x0, [x7]
		.else
	ldr w0, [x7]
		.endif
		.if !\transfer_zero
	str x0, [_cpu, x19]
		.endif
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
		.if \transfer_zero
	mov x0, xzr
		.else
	ldr x0, [_cpu, x19]
		.endif
		.if \bits64
	str x0, [x7]
		.else
	str w0, [x7]
		.endif
	.endif
	add _stream, _stream, #8
	next_gadget

.Lscalar_unsigned_cross\@:
	.if \load
		.if \bits64
	transfer_slow 0, x7, 8
	fmov x0, d0
		.else
	transfer_slow 0, x7, 4
	fmov w0, s0
		.endif
		.if !\transfer_zero
	str x0, [_cpu, x19]
		.endif
	.else
		.if \transfer_zero
	mov x0, xzr
		.else
	ldr x0, [_cpu, x19]
		.endif
		.if \bits64
	fmov d0, x0
	transfer_slow 1, x7, 8
		.else
	fmov s0, w0
	transfer_slow 1, x7, 4
		.endif
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

scalar_unsigned_fast 1, 0, 0, load32_unsigned_fast_r
scalar_unsigned_fast 1, 0, 1, load32_unsigned_fast_z
scalar_unsigned_fast 1, 1, 0, load64_unsigned_fast_r
scalar_unsigned_fast 1, 1, 1, load64_unsigned_fast_z
scalar_unsigned_fast 0, 0, 0, store32_unsigned_fast_r
scalar_unsigned_fast 0, 0, 1, store32_unsigned_fast_z
scalar_unsigned_fast 0, 1, 0, store64_unsigned_fast_r
scalar_unsigned_fast 0, 1, 1, store64_unsigned_fast_z

.purgem scalar_unsigned_fast

/* Translation-specialized byte/halfword and sign-extending unsigned-offset
 * scalar accesses. The stream operand carries CpuState byte offsets for Rt/Rn
 * and the already-scaled displacement, so the runtime does not inspect an
 * opcode or branch on register 31. */
.macro scalar_unsigned_load_narrow_fast name, instruction, host_register, bytes, extension, transfer_zero
gadget \name
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	lsr x21, x8, #32
	ldr x7, [_cpu, x20]
	add x7, x7, x21
	.if \bytes > 1
	and x9, x7, #0xfff
	cmp x9, #(4096 - \bytes)
	b.hi .Lscalar_unsigned_narrow_load_cross\@
	.endif
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()0, [x7]
	.if !\transfer_zero
	str x0, [_cpu, x19]
	.endif
	add _stream, _stream, #8
	next_gadget

	.if \bytes > 1
.Lscalar_unsigned_narrow_load_cross\@:
	transfer_slow 0, x7, \bytes
	.if \bytes == 2
	umov w0, v0.h[0]
	.else
	fmov w0, s0
	.endif
	.if \extension == 1
	sxtb w0, w0
	.elseif \extension == 2
	sxtb x0, w0
	.elseif \extension == 3
	sxth w0, w0
	.elseif \extension == 4
	sxth x0, w0
	.elseif \extension == 5
	sxtw x0, w0
	.endif
	.if !\transfer_zero
	str x0, [_cpu, x19]
	.endif
	add _stream, _stream, #8
	next_gadget
	.endif
.endm

.macro scalar_unsigned_load_narrow_modes name, instruction, host_register, bytes, extension
	scalar_unsigned_load_narrow_fast \name\()_r, \instruction, \host_register, \bytes, \extension, 0
	scalar_unsigned_load_narrow_fast \name\()_z, \instruction, \host_register, \bytes, \extension, 1
.endm

scalar_unsigned_load_narrow_modes load8_unsigned_fast, ldrb, w, 1, 0
scalar_unsigned_load_narrow_modes load16_unsigned_fast, ldrh, w, 2, 0
scalar_unsigned_load_narrow_modes load8_signed_w_fast, ldrsb, w, 1, 1
scalar_unsigned_load_narrow_modes load8_signed_x_fast, ldrsb, x, 1, 2
scalar_unsigned_load_narrow_modes load16_signed_w_fast, ldrsh, w, 2, 3
scalar_unsigned_load_narrow_modes load16_signed_x_fast, ldrsh, x, 2, 4
scalar_unsigned_load_narrow_modes load32_signed_x_fast, ldrsw, x, 4, 5

.purgem scalar_unsigned_load_narrow_modes
.purgem scalar_unsigned_load_narrow_fast

.macro scalar_unsigned_store_narrow_fast name, instruction, bytes, transfer_zero
gadget \name
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	lsr x21, x8, #32
	ldr x7, [_cpu, x20]
	add x7, x7, x21
	.if \bytes > 1
	and x9, x7, #0xfff
	cmp x9, #(4096 - \bytes)
	b.hi .Lscalar_unsigned_narrow_store_cross\@
	.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	.if \transfer_zero
	mov x0, xzr
	.else
	ldr x0, [_cpu, x19]
	.endif
	\instruction w0, [x7]
	add _stream, _stream, #8
	next_gadget

	.if \bytes > 1
.Lscalar_unsigned_narrow_store_cross\@:
	.if \transfer_zero
	mov x0, xzr
	.else
	ldr x0, [_cpu, x19]
	.endif
	fmov d0, x0
	transfer_slow 1, x7, \bytes
	add _stream, _stream, #8
	next_gadget
	.endif
.endm

.macro scalar_unsigned_store_narrow_modes name, instruction, bytes
	scalar_unsigned_store_narrow_fast \name\()_r, \instruction, \bytes, 0
	scalar_unsigned_store_narrow_fast \name\()_z, \instruction, \bytes, 1
.endm

scalar_unsigned_store_narrow_modes store8_unsigned_fast, strb, 1
scalar_unsigned_store_narrow_modes store16_unsigned_fast, strh, 2

.purgem scalar_unsigned_store_narrow_modes
.purgem scalar_unsigned_store_narrow_fast

/* All SIMD&FP unsigned-offset widths use a translated CpuState vector offset,
 * translated base offset, and scaled displacement. Width and direction are
 * fixed by the selected symbol. */
.macro vector_unsigned_fast name, load, instruction, host_register, bytes
gadget \name
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	lsr x21, x8, #32
	ldr x7, [_cpu, x20]
	add x7, x7, x21
	and x9, x7, #0xfff
	cmp x9, #(4096 - \bytes)
	b.hi .Lvector_unsigned_cross\@
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()0, [x7]
	str q0, [_cpu, x19]
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	ldr q0, [_cpu, x19]
	\instruction \host_register\()0, [x7]
	.endif
	add _stream, _stream, #8
	next_gadget

.Lvector_unsigned_cross\@:
	.if \load
	transfer_slow 0, x7, \bytes
	str q0, [_cpu, x19]
	.else
	ldr q0, [_cpu, x19]
	transfer_slow 1, x7, \bytes
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

vector_unsigned_fast vector_load8_unsigned_fast, 1, ldr, b, 1
vector_unsigned_fast vector_load16_unsigned_fast, 1, ldr, h, 2
vector_unsigned_fast vector_load32_unsigned_fast, 1, ldr, s, 4
vector_unsigned_fast vector_load64_unsigned_fast, 1, ldr, d, 8
vector_unsigned_fast vector_load128_unsigned_fast, 1, ldr, q, 16
vector_unsigned_fast vector_store8_unsigned_fast, 0, str, b, 1
vector_unsigned_fast vector_store16_unsigned_fast, 0, str, h, 2
vector_unsigned_fast vector_store32_unsigned_fast, 0, str, s, 4
vector_unsigned_fast vector_store64_unsigned_fast, 0, str, d, 8
vector_unsigned_fast vector_store128_unsigned_fast, 0, str, q, 16

.purgem vector_unsigned_fast

/* Scalar unscaled/pre/post-indexed accesses. Operand: Rt CpuState offset in
 * bits 15:0, Rn offset in 31:16, signed imm9 in 63:32. Mode 0 is adjusted
 * address without writeback, mode 1 pre-index, mode 2 post-index, and mode 3
 * post-index with architecturally suppressed overlapping-load writeback. */
.macro scalar_other_immediate_fast name, load, instruction, host_register, bytes, extension, mode, transfer_zero
gadget \name
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	asr x21, x8, #32
	ldr x24, [_cpu, x20]
	add x25, x24, x21
	.if \mode >= 2
	mov x26, x24
	.else
	mov x26, x25
	.endif
	.if \bytes > 1
	and x9, x26, #0xfff
	cmp x9, #(4096 - \bytes)
	b.hi .Lscalar_other_immediate_cross\@
	.endif
	mov x7, x26
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()0, [x7]
		.if !\transfer_zero
	str x0, [_cpu, x19]
		.endif
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
		.if \transfer_zero
	mov x0, xzr
		.else
	ldr x0, [_cpu, x19]
		.endif
	\instruction \host_register\()0, [x7]
	.endif
	.if \mode == 1 || \mode == 2
	str x25, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget

	.if \bytes > 1
.Lscalar_other_immediate_cross\@:
	.if \load
	transfer_slow 0, x26, \bytes
		.if \bytes == 2
	umov w0, v0.h[0]
		.elseif \bytes == 4
	fmov w0, s0
		.else
	fmov x0, d0
		.endif
		.if \extension == 1
	sxtb w0, w0
		.elseif \extension == 2
	sxtb x0, w0
		.elseif \extension == 3
	sxth w0, w0
		.elseif \extension == 4
	sxth x0, w0
		.elseif \extension == 5
	sxtw x0, w0
		.endif
		.if !\transfer_zero
	str x0, [_cpu, x19]
		.endif
	.else
		.if \transfer_zero
	mov x0, xzr
		.else
	ldr x0, [_cpu, x19]
		.endif
	fmov d0, x0
	transfer_slow 1, x26, \bytes
	.endif
	.if \mode == 1 || \mode == 2
	str x25, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget
	.endif
.endm

.macro scalar_other_immediate_operation name, load, instruction, host_register, bytes, extension
	scalar_other_immediate_fast scalar_other_\name\()_offset_r, \load, \instruction, \host_register, \bytes, \extension, 0, 0
	scalar_other_immediate_fast scalar_other_\name\()_offset_z, \load, \instruction, \host_register, \bytes, \extension, 0, 1
	scalar_other_immediate_fast scalar_other_\name\()_pre_r, \load, \instruction, \host_register, \bytes, \extension, 1, 0
	scalar_other_immediate_fast scalar_other_\name\()_pre_z, \load, \instruction, \host_register, \bytes, \extension, 1, 1
	scalar_other_immediate_fast scalar_other_\name\()_post_r, \load, \instruction, \host_register, \bytes, \extension, 2, 0
	scalar_other_immediate_fast scalar_other_\name\()_post_z, \load, \instruction, \host_register, \bytes, \extension, 2, 1
	scalar_other_immediate_fast scalar_other_\name\()_post_suppress_r, \load, \instruction, \host_register, \bytes, \extension, 3, 0
	scalar_other_immediate_fast scalar_other_\name\()_post_suppress_z, \load, \instruction, \host_register, \bytes, \extension, 3, 1
.endm

scalar_other_immediate_operation store8, 0, strb, w, 1, 0
scalar_other_immediate_operation load8u, 1, ldrb, w, 1, 0
scalar_other_immediate_operation load8sx, 1, ldrsb, x, 1, 2
scalar_other_immediate_operation load8sw, 1, ldrsb, w, 1, 1
scalar_other_immediate_operation store16, 0, strh, w, 2, 0
scalar_other_immediate_operation load16u, 1, ldrh, w, 2, 0
scalar_other_immediate_operation load16sx, 1, ldrsh, x, 2, 4
scalar_other_immediate_operation load16sw, 1, ldrsh, w, 2, 3
scalar_other_immediate_operation store32, 0, str, w, 4, 0
scalar_other_immediate_operation load32u, 1, ldr, w, 4, 0
scalar_other_immediate_operation load32sx, 1, ldrsw, x, 4, 5
scalar_other_immediate_operation store64, 0, str, x, 8, 0
scalar_other_immediate_operation load64u, 1, ldr, x, 8, 0

.purgem scalar_other_immediate_operation
.purgem scalar_other_immediate_fast

/* Register-offset form. The upper operand half stores the translated Rm
 * CpuState byte offset. Extension (UXTW/X/SXTW) and optional scaling are fixed
 * in the gadget symbol. */
.macro scalar_other_register_fast name, load, instruction, host_register, bytes, address_extension, shifted, result_extension, transfer_zero
gadget \name
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	ubfx x21, x8, #32, #16
	ldr x24, [_cpu, x20]
	ldr x26, [_cpu, x21]
	.if \address_extension == 0
	mov w26, w26
	.elseif \address_extension == 2
	sxtw x26, w26
	.endif
	.if \shifted
		.if \bytes == 2
	lsl x26, x26, #1
		.elseif \bytes == 4
	lsl x26, x26, #2
		.elseif \bytes == 8
	lsl x26, x26, #3
		.endif
	.endif
	add x26, x24, x26
	.if \bytes > 1
	and x9, x26, #0xfff
	cmp x9, #(4096 - \bytes)
	b.hi .Lscalar_other_register_cross\@
	.endif
	mov x7, x26
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()0, [x7]
		.if !\transfer_zero
	str x0, [_cpu, x19]
		.endif
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
		.if \transfer_zero
	mov x0, xzr
		.else
	ldr x0, [_cpu, x19]
		.endif
	\instruction \host_register\()0, [x7]
	.endif
	add _stream, _stream, #8
	next_gadget

	.if \bytes > 1
.Lscalar_other_register_cross\@:
	.if \load
	transfer_slow 0, x26, \bytes
		.if \bytes == 2
	umov w0, v0.h[0]
		.elseif \bytes == 4
	fmov w0, s0
		.else
	fmov x0, d0
		.endif
		.if \result_extension == 1
	sxtb w0, w0
		.elseif \result_extension == 2
	sxtb x0, w0
		.elseif \result_extension == 3
	sxth w0, w0
		.elseif \result_extension == 4
	sxth x0, w0
		.elseif \result_extension == 5
	sxtw x0, w0
		.endif
		.if !\transfer_zero
	str x0, [_cpu, x19]
		.endif
	.else
		.if \transfer_zero
	mov x0, xzr
		.else
	ldr x0, [_cpu, x19]
		.endif
	fmov d0, x0
	transfer_slow 1, x26, \bytes
	.endif
	add _stream, _stream, #8
	next_gadget
	.endif
.endm

.macro scalar_other_register_extension name, load, instruction, host_register, bytes, result_extension, address_extension, extension_name, shifted
	scalar_other_register_fast scalar_other_\name\()_\extension_name\()_\shifted\()_r, \load, \instruction, \host_register, \bytes, \address_extension, \shifted, \result_extension, 0
	scalar_other_register_fast scalar_other_\name\()_\extension_name\()_\shifted\()_z, \load, \instruction, \host_register, \bytes, \address_extension, \shifted, \result_extension, 1
.endm

/* The final parameter controls result sign extension; address extension is
 * supplied explicitly by scalar_other_register_extension below. */
.macro scalar_other_register_all name, load, instruction, host_register, bytes, result_extension
	scalar_other_register_extension \name, \load, \instruction, \host_register, \bytes, \result_extension, 0, uxtw, 0
	scalar_other_register_extension \name, \load, \instruction, \host_register, \bytes, \result_extension, 1, x, 0
	scalar_other_register_extension \name, \load, \instruction, \host_register, \bytes, \result_extension, 2, sxtw, 0
	scalar_other_register_extension \name, \load, \instruction, \host_register, \bytes, \result_extension, 0, uxtw, 1
	scalar_other_register_extension \name, \load, \instruction, \host_register, \bytes, \result_extension, 1, x, 1
	scalar_other_register_extension \name, \load, \instruction, \host_register, \bytes, \result_extension, 2, sxtw, 1
.endm

scalar_other_register_all store8, 0, strb, w, 1, 0
scalar_other_register_all load8u, 1, ldrb, w, 1, 0
scalar_other_register_all load8sx, 1, ldrsb, x, 1, 2
scalar_other_register_all load8sw, 1, ldrsb, w, 1, 1
scalar_other_register_all store16, 0, strh, w, 2, 0
scalar_other_register_all load16u, 1, ldrh, w, 2, 0
scalar_other_register_all load16sx, 1, ldrsh, x, 2, 4
scalar_other_register_all load16sw, 1, ldrsh, w, 2, 3
scalar_other_register_all store32, 0, str, w, 4, 0
scalar_other_register_all load32u, 1, ldr, w, 4, 0
scalar_other_register_all load32sx, 1, ldrsw, x, 4, 5
scalar_other_register_all store64, 0, str, x, 8, 0
scalar_other_register_all load64u, 1, ldr, x, 8, 0

.purgem scalar_other_register_all
.purgem scalar_other_register_extension
.purgem scalar_other_register_fast

/* SIMD&FP unscaled/pre/post-indexed and register-offset forms use the same
 * translated address descriptors as scalar memory, with the V-register byte
 * offset in bits 15:0. */
.macro vector_other_immediate_fast name, load, instruction, host_register, bytes, mode
gadget \name
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	asr x21, x8, #32
	ldr x24, [_cpu, x20]
	add x25, x24, x21
	.if \mode == 2
	mov x26, x24
	.else
	mov x26, x25
	.endif
	and x9, x26, #0xfff
	cmp x9, #(4096 - \bytes)
	b.hi .Lvector_other_immediate_cross\@
	mov x7, x26
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()0, [x7]
	str q0, [_cpu, x19]
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	ldr q0, [_cpu, x19]
	\instruction \host_register\()0, [x7]
	.endif
	.if \mode == 1 || \mode == 2
	str x25, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget

.Lvector_other_immediate_cross\@:
	.if \load
	transfer_slow 0, x26, \bytes
	str q0, [_cpu, x19]
	.else
	ldr q0, [_cpu, x19]
	transfer_slow 1, x26, \bytes
	.endif
	.if \mode == 1 || \mode == 2
	str x25, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro vector_other_immediate_operation name, load, instruction, host_register, bytes
	vector_other_immediate_fast vector_other_\name\()_offset, \load, \instruction, \host_register, \bytes, 0
	vector_other_immediate_fast vector_other_\name\()_pre, \load, \instruction, \host_register, \bytes, 1
	vector_other_immediate_fast vector_other_\name\()_post, \load, \instruction, \host_register, \bytes, 2
.endm

.macro vector_other_register_fast name, load, instruction, host_register, bytes, extension, shifted
gadget \name
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	ubfx x21, x8, #32, #16
	ldr x24, [_cpu, x20]
	ldr x26, [_cpu, x21]
	.if \extension == 0
	mov w26, w26
	.elseif \extension == 2
	sxtw x26, w26
	.endif
	.if \shifted
		.if \bytes == 2
	lsl x26, x26, #1
		.elseif \bytes == 4
	lsl x26, x26, #2
		.elseif \bytes == 8
	lsl x26, x26, #3
		.elseif \bytes == 16
	lsl x26, x26, #4
		.endif
	.endif
	add x26, x24, x26
	and x9, x26, #0xfff
	cmp x9, #(4096 - \bytes)
	b.hi .Lvector_other_register_cross\@
	mov x7, x26
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()0, [x7]
	str q0, [_cpu, x19]
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	ldr q0, [_cpu, x19]
	\instruction \host_register\()0, [x7]
	.endif
	add _stream, _stream, #8
	next_gadget

.Lvector_other_register_cross\@:
	.if \load
	transfer_slow 0, x26, \bytes
	str q0, [_cpu, x19]
	.else
	ldr q0, [_cpu, x19]
	transfer_slow 1, x26, \bytes
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro vector_other_register_extension name, load, instruction, host_register, bytes, extension, extension_name, shifted
	vector_other_register_fast vector_other_\name\()_\extension_name\()_\shifted, \load, \instruction, \host_register, \bytes, \extension, \shifted
.endm

.macro vector_other_operation name, load, instruction, host_register, bytes
	vector_other_immediate_operation \name, \load, \instruction, \host_register, \bytes
	vector_other_register_extension \name, \load, \instruction, \host_register, \bytes, 0, uxtw, 0
	vector_other_register_extension \name, \load, \instruction, \host_register, \bytes, 1, x, 0
	vector_other_register_extension \name, \load, \instruction, \host_register, \bytes, 2, sxtw, 0
	vector_other_register_extension \name, \load, \instruction, \host_register, \bytes, 0, uxtw, 1
	vector_other_register_extension \name, \load, \instruction, \host_register, \bytes, 1, x, 1
	vector_other_register_extension \name, \load, \instruction, \host_register, \bytes, 2, sxtw, 1
.endm

vector_other_operation store8, 0, str, b, 1
vector_other_operation load8, 1, ldr, b, 1
vector_other_operation store16, 0, str, h, 2
vector_other_operation load16, 1, ldr, h, 2
vector_other_operation store32, 0, str, s, 4
vector_other_operation load32, 1, ldr, s, 4
vector_other_operation store64, 0, str, d, 8
vector_other_operation load64, 1, ldr, d, 8
vector_other_operation store128, 0, str, q, 16
vector_other_operation load128, 1, ldr, q, 16

.purgem vector_other_operation
.purgem vector_other_register_extension
.purgem vector_other_register_fast
.purgem vector_other_immediate_operation
.purgem vector_other_immediate_fast

/* Translation-specialized integer pair transfers. The packed operand holds
 * Rt/Rt2/Rn CpuState byte offsets in successive 16-bit fields and the signed,
 * already-scaled displacement in the upper 16 bits. Width, direction,
 * writeback mode, sign extension, and both XZR cases are fixed by the symbol.
 * Mode 0 is adjusted address without writeback, mode 1 pre-index, mode 2
 * post-index, and mode 3 post-index with suppressed load writeback. */
.macro integer_pair_fast name, load, instruction, host_register, bytes, sign_extend, mode, first_zero, second_zero
gadget \name
	ldr x8, [_stream], #8
	and x22, x8, #0xffff
	ubfx x23, x8, #16, #16
	ubfx x24, x8, #32, #16
	asr x6, x8, #48
	ldr x16, [_cpu, x24]
	add x21, x16, x6
	.if \mode >= 2
	mov x19, x16
	.else
	mov x19, x21
	.endif
	/* Comparing the page bits of the first and final transfer bytes catches
	 * both page crossing and address wraparound with one hot-path branch. */
	add x9, x19, #((2 * \bytes) - 1)
	eor x9, x9, x19
	tst x9, #0xfffffffffffff000
	b.ne .Linteger_pair_cross\@
	.if \load
	mov x7, x19
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	mov x19, x7
	\instruction \host_register\()6, \host_register\()7, [x19]
		.if !\first_zero
	str x6, [_cpu, x22]
		.endif
		.if !\second_zero
	str x7, [_cpu, x23]
		.endif
	.else
	mov x7, x19
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	mov x19, x7
		.if \first_zero
	mov x6, xzr
		.else
	ldr x6, [_cpu, x22]
		.endif
		.if \second_zero
	mov x7, xzr
		.else
	ldr x7, [_cpu, x23]
		.endif
	\instruction \host_register\()6, \host_register\()7, [x19]
	.endif
	.if \mode == 1 || \mode == 2
	str x21, [_cpu, x24]
	.endif
	add _stream, _stream, #8
	next_gadget

.Linteger_pair_cross\@:
	.if \load
	transfer_slow 0, x19, (\bytes * 2)
		.if \bytes == 4
	fmov w6, s0
	umov w7, v0.s[1]
			.if \sign_extend
	sxtw x6, w6
	sxtw x7, w7
			.endif
		.else
	fmov x6, d0
	umov x7, v0.d[1]
		.endif
		.if !\first_zero
	str x6, [_cpu, x22]
		.endif
		.if !\second_zero
	str x7, [_cpu, x23]
		.endif
	.else
		.if \first_zero
	mov x6, xzr
		.else
	ldr x6, [_cpu, x22]
		.endif
		.if \second_zero
	mov x7, xzr
		.else
	ldr x7, [_cpu, x23]
		.endif
		.if \bytes == 4
	fmov s0, w6
	mov v0.s[1], w7
		.else
	fmov d0, x6
	mov v0.d[1], x7
		.endif
	transfer_slow 1, x19, (\bytes * 2)
	.endif
	.if \mode == 1 || \mode == 2
	str x21, [_cpu, x24]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro integer_pair_mode name, load, instruction, host_register, bytes, sign_extend, mode
	integer_pair_fast integer_pair_\name\()_\mode\()_rr, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 0, 0
	integer_pair_fast integer_pair_\name\()_\mode\()_rz, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 0, 1
	integer_pair_fast integer_pair_\name\()_\mode\()_zr, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 1, 0
	integer_pair_fast integer_pair_\name\()_\mode\()_zz, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 1, 1
.endm

.macro integer_pair_operation name, load, instruction, host_register, bytes, sign_extend
	integer_pair_mode \name, \load, \instruction, \host_register, \bytes, \sign_extend, 0
	integer_pair_mode \name, \load, \instruction, \host_register, \bytes, \sign_extend, 1
	integer_pair_mode \name, \load, \instruction, \host_register, \bytes, \sign_extend, 2
	integer_pair_mode \name, \load, \instruction, \host_register, \bytes, \sign_extend, 3
.endm

integer_pair_operation store32, 0, stp, w, 4, 0
integer_pair_operation load32, 1, ldp, w, 4, 0
integer_pair_operation load32s, 1, ldpsw, x, 4, 1
integer_pair_operation store64, 0, stp, x, 8, 0
integer_pair_operation load64, 1, ldp, x, 8, 0

.purgem integer_pair_operation
.purgem integer_pair_mode
.purgem integer_pair_fast

/* Translation-specialized SIMD&FP pairs. Operand fields match the integer
 * pair descriptor, but the first two offsets address VectorRegister slots.
 * Width, direction, and address mode are fixed by the selected symbol. */
.macro vector_pair_fast name, load, instruction, host_register, bytes, mode
gadget \name
	ldr x8, [_stream], #8
	and x22, x8, #0xffff
	ubfx x23, x8, #16, #16
	ubfx x24, x8, #32, #16
	asr x6, x8, #48
	ldr x16, [_cpu, x24]
	add x21, x16, x6
	.if \mode == 2
	mov x19, x16
	.else
	mov x19, x21
	.endif
	add x9, x19, #((2 * \bytes) - 1)
	eor x9, x9, x19
	tst x9, #0xfffffffffffff000
	b.ne .Lvector_pair_cross\@
	.if \load
	mov x7, x19
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	mov x19, x7
	\instruction \host_register\()0, \host_register\()1, [x19]
	str q0, [_cpu, x22]
	str q1, [_cpu, x23]
	.else
	mov x7, x19
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	mov x19, x7
	ldr q0, [_cpu, x22]
	ldr q1, [_cpu, x23]
	\instruction \host_register\()0, \host_register\()1, [x19]
	.endif
	.if \mode == 1 || \mode == 2
	str x21, [_cpu, x24]
	.endif
	add _stream, _stream, #8
	next_gadget

.Lvector_pair_cross\@:
	.if \load
	transfer_slow 0, x19, (\bytes * 2)
		.if \bytes == 4
	movi v1.16b, #0
	mov v1.s[0], v0.s[1]
	movi v2.16b, #0
	mov v2.s[0], v0.s[0]
	mov v0.16b, v2.16b
		.elseif \bytes == 8
	movi v1.16b, #0
	mov v1.d[0], v0.d[1]
	movi v2.16b, #0
	mov v2.d[0], v0.d[0]
	mov v0.16b, v2.16b
		.endif
	str q0, [_cpu, x22]
	str q1, [_cpu, x23]
	.else
	ldr q0, [_cpu, x22]
	ldr q1, [_cpu, x23]
		.if \bytes == 4
	mov v0.s[1], v1.s[0]
		.elseif \bytes == 8
	mov v0.d[1], v1.d[0]
		.endif
	transfer_slow 1, x19, (\bytes * 2)
	.endif
	.if \mode == 1 || \mode == 2
	str x21, [_cpu, x24]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro vector_pair_operation name, load, instruction, host_register, bytes
	vector_pair_fast vector_pair_\name\()_0, \load, \instruction, \host_register, \bytes, 0
	vector_pair_fast vector_pair_\name\()_1, \load, \instruction, \host_register, \bytes, 1
	vector_pair_fast vector_pair_\name\()_2, \load, \instruction, \host_register, \bytes, 2
.endm

vector_pair_operation store32, 0, stp, s, 4
vector_pair_operation load32, 1, ldp, s, 4
vector_pair_operation store64, 0, stp, d, 8
vector_pair_operation load64, 1, ldp, d, 8
vector_pair_operation store128, 0, stp, q, 16
vector_pair_operation load128, 1, ldp, q, 16

.purgem vector_pair_operation
.purgem vector_pair_fast

/* LDRAA/LDRAB authenticate the base with a zero modifier before applying the
 * signed scaled offset. Key, writeback, SP alignment, and XZR destination are
 * fixed by the builder. Stream: [packed direct offsets/immediate][guest PC]. */
.macro pauth_load name, authentication, writeback, base_sp, discard
gadget pauth_load_\name
	ldr x8, [_stream], #8
	.if !\discard
	and w19, w8, #0xffff
	.endif
	ubfx w20, w8, #16, #16
	asr x21, x8, #32
	ldr x7, [_cpu, x20]
	.if \base_sp
	tst x7, #0xf
	b.ne uml_arm64_gadget_memory_fault
	.endif
	\authentication x7
	add x25, x7, x21
	and x9, x25, #0xfff
	cmp x9, #4088
	b.hi pauth_load_\name\()_cross
	mov x7, x25
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	ldr x0, [x7]
	b pauth_load_\name\()_finish
pauth_load_\name\()_cross:
	transfer_slow 0, x25, 8
	fmov x0, d0
pauth_load_\name\()_finish:
	.if !\discard
	str x0, [_cpu, x19]
	.endif
	.if \writeback
	str x25, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro pauth_load_modes key, authentication
pauth_load \key\()_o_rr, \authentication, 0, 0, 0
pauth_load \key\()_o_rz, \authentication, 0, 0, 1
pauth_load \key\()_o_sr, \authentication, 0, 1, 0
pauth_load \key\()_o_sz, \authentication, 0, 1, 1
pauth_load \key\()_w_rr, \authentication, 1, 0, 0
pauth_load \key\()_w_rz, \authentication, 1, 0, 1
pauth_load \key\()_w_sr, \authentication, 1, 1, 0
pauth_load \key\()_w_sz, \authentication, 1, 1, 1
.endm

pauth_load_modes a, autdza
pauth_load_modes b, autdzb

.purgem pauth_load_modes
.purgem pauth_load

uml_arm64_gadget_memory_fault:
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_MEMORY_FAULT
	b uml_arm64_gadget_leave
