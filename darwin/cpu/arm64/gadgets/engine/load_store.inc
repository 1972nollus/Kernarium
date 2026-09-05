gadget nop
	next_gadget

/* Literal form invariants are resolved while translating. Operand bits 15:0
 * are the CpuState destination byte offset; bits 63:16 are the signed PC
 * displacement. The following word is the guest PC used for address formation
 * and precise memory faults. */
.macro literal_integer_fast name, instruction, host_register, bytes, extension, discard, forward=0
	.set .Lsd\@, scalar_offset0
	.if \discard
	.set .Lsd\@, scalar_none
	.endif
scalar_entry \name, \forward, (17 - 1), .Lsd\@, scalar_none, scalar_none, scalar_none, 2
	ldr x8, [_stream]
	ldr x9, [_stream, #8]
	and x19, x8, #0xffff
	asr x10, x8, #16
	add x7, x9, x10
	branch_if_cross_page x7, \bytes, x11, .Lliteral_integer_fast_cross\@
	add _stream, _stream, #8
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \forward
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
	scalar_variants \forward, scalar_none, scalar_none, scalar_none, literal_integer_fast, \name, \instruction, \host_register, \bytes, \extension, \discard
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
	branch_if_cross_page x7, \bytes, x11, .Lliteral_vector_fast_cross\@
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
.macro scalar_unsigned_fast load, bits64, transfer_zero, base_sp, suffix, forward=0
	.set .Lsd\@, scalar_offset0
	.if !\load || \transfer_zero
	.set .Lsd\@, scalar_none
	.endif
	.set .Lsn\@, scalar_offset16
	.set .Lsm\@, scalar_offset0
	.if \load || \transfer_zero
	.set .Lsm\@, scalar_none
	.endif
scalar_entry \suffix, \forward, (17 - \load), .Lsd\@, .Lsn\@, .Lsm\@, scalar_none, 2
	.if !\load && (\forward & 2)
	mov x22, x0
	.endif
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	lsr x21, x8, #32
	scalar_read \forward, 1, x7, x20
	.if \base_sp
	tst x7, #0xf
		.if \load
	.if \forward
	b.ne .Lscalar_forward_alignment_read\@
	.else
	b.ne .Lscalar_alignment_fault_read
	.endif
		.else
	.if \forward
	b.ne .Lscalar_forward_alignment_write\@
	.else
	b.ne .Lscalar_alignment_fault_write
	.endif
		.endif
	.endif
	add x7, x7, x21
	.if !\base_sp
	ldrb w9, [_cpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, .Lscalar_unsigned_untagged\@
	ubfx x7, x7, #0, #56
.Lscalar_unsigned_untagged\@:
	.endif
	.if \bits64
	branch_if_cross_page x7, 8, x9, .Lscalar_unsigned_cross\@
	.else
	branch_if_cross_page x7, 4, x9, .Lscalar_unsigned_cross\@
	.endif
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \forward
		.if \bits64
	ldr x0, [x7]
		.else
	ldr w0, [x7]
		.endif
		.if !\transfer_zero
	str x0, [_cpu, x19]
		.endif
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \forward
		.if \transfer_zero
	mov x0, xzr
		.else
	.if !(\forward & 2)
	ldr x0, [_cpu, x19]
	.endif
		.endif
		.if \bits64
	scalar_memory_transfer 0, \forward, 2, str, x
		.else
	scalar_memory_transfer 0, \forward, 2, str, w
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
	.if !(\forward & 2)
	ldr x0, [_cpu, x19]
	.endif
		.endif
		.if \bits64
	scalar_store_vector \forward, 2, 1
	transfer_slow 1, x7, 8
		.else
	scalar_store_vector \forward, 2, 0
	transfer_slow 1, x7, 4
		.endif
	.endif
	add _stream, _stream, #8
	next_gadget
	.if \forward
.Lscalar_forward_alignment_read\@:
	b .Lscalar_alignment_fault_read
	.endif
	.if \forward
.Lscalar_forward_alignment_write\@:
	b .Lscalar_alignment_fault_write
	.endif
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, scalar_unsigned_fast, \load, \bits64, \transfer_zero, \base_sp, \suffix
.endm

.macro scalar_unsigned_modes load, bits64, name
scalar_unsigned_fast \load, \bits64, 0, 0, \name\()_r
scalar_unsigned_fast \load, \bits64, 1, 0, \name\()_z
scalar_unsigned_fast \load, \bits64, 0, 1, \name\()_r_sp
scalar_unsigned_fast \load, \bits64, 1, 1, \name\()_z_sp
.endm

scalar_unsigned_modes 1, 0, load32_unsigned_fast
scalar_unsigned_modes 1, 1, load64_unsigned_fast
scalar_unsigned_modes 0, 0, store32_unsigned_fast
scalar_unsigned_modes 0, 1, store64_unsigned_fast

.purgem scalar_unsigned_modes
.purgem scalar_unsigned_fast

/* Translation-specialized byte/halfword and sign-extending unsigned-offset
 * scalar accesses. The stream operand carries CpuState byte offsets for Rt/Rn
 * and the already-scaled displacement, so the runtime does not inspect an
 * opcode or branch on register 31. */
.macro scalar_unsigned_load_narrow_fast name, instruction, host_register, bytes, extension, transfer_zero, base_sp, forward=0
	.set .Lsd\@, scalar_offset0
	.if \transfer_zero
	.set .Lsd\@, scalar_none
	.endif
	.set .Lsn\@, scalar_offset16
scalar_entry \name, \forward, (17 - 1), .Lsd\@, .Lsn\@, scalar_none, scalar_none, 2
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	lsr x21, x8, #32
	scalar_read \forward, 1, x7, x20
	.if \base_sp
	tst x7, #0xf
	.if \forward
	b.ne .Lscalar_forward_alignment_read\@
	.else
	b.ne .Lscalar_alignment_fault_read
	.endif
	.endif
	add x7, x7, x21
	.if !\base_sp
	ldrb w9, [_cpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, .Lscalar_unsigned_narrow_load_untagged\@
	ubfx x7, x7, #0, #56
.Lscalar_unsigned_narrow_load_untagged\@:
	.endif
	.if \bytes > 1
	branch_if_cross_page x7, \bytes, x9, .Lscalar_unsigned_narrow_load_cross\@
	.endif
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \forward
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
	.if \forward
.Lscalar_forward_alignment_read\@:
	b .Lscalar_alignment_fault_read
	.endif
	scalar_variants \forward, .Lsn\@, scalar_none, scalar_none, scalar_unsigned_load_narrow_fast, \name, \instruction, \host_register, \bytes, \extension, \transfer_zero, \base_sp
.endm

.macro scalar_unsigned_load_narrow_modes name, instruction, host_register, bytes, extension
	scalar_unsigned_load_narrow_fast \name\()_r, \instruction, \host_register, \bytes, \extension, 0, 0
	scalar_unsigned_load_narrow_fast \name\()_z, \instruction, \host_register, \bytes, \extension, 1, 0
	scalar_unsigned_load_narrow_fast \name\()_r_sp, \instruction, \host_register, \bytes, \extension, 0, 1
	scalar_unsigned_load_narrow_fast \name\()_z_sp, \instruction, \host_register, \bytes, \extension, 1, 1
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

.macro scalar_unsigned_store_narrow_fast name, instruction, bytes, transfer_zero, base_sp, forward=0
	.set .Lsn\@, scalar_offset16
	.set .Lsm\@, scalar_offset0
	.if \transfer_zero
	.set .Lsm\@, scalar_none
	.endif
scalar_entry \name, \forward, (17 - 0), scalar_none, .Lsn\@, .Lsm\@, scalar_none, 2
	.if !0 && (\forward & 2)
	mov x22, x0
	.endif
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	lsr x21, x8, #32
	scalar_read \forward, 1, x7, x20
	.if \base_sp
	tst x7, #0xf
	.if \forward
	b.ne .Lscalar_forward_alignment_write\@
	.else
	b.ne .Lscalar_alignment_fault_write
	.endif
	.endif
	add x7, x7, x21
	.if !\base_sp
	ldrb w9, [_cpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, .Lscalar_unsigned_narrow_store_untagged\@
	ubfx x7, x7, #0, #56
.Lscalar_unsigned_narrow_store_untagged\@:
	.endif
	.if \bytes > 1
	branch_if_cross_page x7, \bytes, x9, .Lscalar_unsigned_narrow_store_cross\@
	.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \forward
	.if \transfer_zero
	mov x0, xzr
	.else
	.if !(\forward & 2)
	ldr x0, [_cpu, x19]
	.endif
	.endif
	scalar_memory_transfer 0, \forward, 2, \instruction, w
	add _stream, _stream, #8
	next_gadget

	.if \bytes > 1
.Lscalar_unsigned_narrow_store_cross\@:
	.if \transfer_zero
	mov x0, xzr
	.else
	.if !(\forward & 2)
	ldr x0, [_cpu, x19]
	.endif
	.endif
	scalar_store_vector \forward, 2, 1
	transfer_slow 1, x7, \bytes
	add _stream, _stream, #8
	next_gadget
	.endif
	.if \forward
.Lscalar_forward_alignment_write\@:
	b .Lscalar_alignment_fault_write
	.endif
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, scalar_unsigned_store_narrow_fast, \name, \instruction, \bytes, \transfer_zero, \base_sp
.endm

.macro scalar_unsigned_store_narrow_modes name, instruction, bytes
	scalar_unsigned_store_narrow_fast \name\()_r, \instruction, \bytes, 0, 0
	scalar_unsigned_store_narrow_fast \name\()_z, \instruction, \bytes, 1, 0
	scalar_unsigned_store_narrow_fast \name\()_r_sp, \instruction, \bytes, 0, 1
	scalar_unsigned_store_narrow_fast \name\()_z_sp, \instruction, \bytes, 1, 1
.endm

scalar_unsigned_store_narrow_modes store8_unsigned_fast, strb, 1
scalar_unsigned_store_narrow_modes store16_unsigned_fast, strh, 2

.purgem scalar_unsigned_store_narrow_modes
.purgem scalar_unsigned_store_narrow_fast

/* All SIMD&FP unsigned-offset widths use a translated CpuState vector offset,
 * translated base offset, and scaled displacement. Width and direction are
 * fixed by the selected symbol. */
.macro vector_unsigned_fast name, load, instruction, host_register, bytes, base_sp, forward=0
	.set .Lsn\@, scalar_offset16
scalar_entry \name, \forward, (17 - \load), scalar_none, .Lsn\@, scalar_none, scalar_none, 2
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	lsr x21, x8, #32
	scalar_read \forward, 1, x7, x20
	.if \base_sp
	tst x7, #0xf
		.if \load
	.if \forward
	b.ne .Lscalar_forward_alignment_read\@
	.else
	b.ne .Lscalar_alignment_fault_read
	.endif
		.else
	.if \forward
	b.ne .Lscalar_forward_alignment_write\@
	.else
	b.ne .Lscalar_alignment_fault_write
	.endif
		.endif
	.endif
	add x7, x7, x21
	.if !\base_sp
	ldrh w9, [_cpu, #UML_GADGET_CPU_TAGGED_ADDRESS_STATE]
	cbnz w9, .Lvector_unsigned_tagged\@
	.endif
	branch_if_cross_page x7, \bytes, x9, .Lvector_unsigned_cross\@
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \forward
	\instruction \host_register\()0, [x7]
	str q0, [_cpu, x19]
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \forward
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

	.if !\base_sp
.Lvector_unsigned_tagged\@:
	sub sp, sp, #32
	stp x1, x2, [sp, #16]
	.if \load
	movi v0.16b, #0
	.else
	ldr q0, [_cpu, x19]
	.endif
	str q0, [sp]
	mov x0, _cpu
	mov x1, _tlb
	mov x2, x7
	mov x3, sp
	mov x4, #\bytes
	mov w5, #(!\load)
	mov w6, #\base_sp
	mov w7, #((\bytes << 8) | 1)
	bl NAME(uml_arm64_simd_structure_memory_transfer)
	ldp x1, x2, [sp, #16]
	cbz w0, 97f
	.if \load
	ldr q0, [sp]
	str q0, [_cpu, x19]
	.endif
	add sp, sp, #32
	add _stream, _stream, #8
	next_gadget
97:
	add sp, sp, #32
	b uml_arm64_gadget_memory_fault
	.endif
	.if \forward
.Lscalar_forward_alignment_read\@:
	b .Lscalar_alignment_fault_read
	.endif
	.if \forward
.Lscalar_forward_alignment_write\@:
	b .Lscalar_alignment_fault_write
	.endif
	scalar_variants \forward, .Lsn\@, scalar_none, scalar_none, vector_unsigned_fast, \name, \load, \instruction, \host_register, \bytes, \base_sp
.endm

.macro vector_unsigned_modes name, load, instruction, host_register, bytes
	vector_unsigned_fast vector_\name\()_unsigned_fast, \load, \instruction, \host_register, \bytes, 0
	vector_unsigned_fast vector_\name\()_unsigned_fast_sp, \load, \instruction, \host_register, \bytes, 1
.endm

vector_unsigned_modes load8, 1, ldr, b, 1
vector_unsigned_modes load16, 1, ldr, h, 2
vector_unsigned_modes load32, 1, ldr, s, 4
vector_unsigned_modes load64, 1, ldr, d, 8
vector_unsigned_modes load128, 1, ldr, q, 16
vector_unsigned_modes store8, 0, str, b, 1
vector_unsigned_modes store16, 0, str, h, 2
vector_unsigned_modes store32, 0, str, s, 4
vector_unsigned_modes store64, 0, str, d, 8
vector_unsigned_modes store128, 0, str, q, 16

.purgem vector_unsigned_modes
.purgem vector_unsigned_fast

/* Scalar unscaled/pre/post-indexed accesses. Operand: Rt CpuState offset in
 * bits 15:0, Rn offset in 31:16, signed imm9 in 63:32. Mode 0 is adjusted
 * address without writeback, mode 1 pre-index, mode 2 post-index, and mode 3
 * post-index with architecturally suppressed overlapping-load writeback. */
.macro scalar_other_immediate_fast name, load, instruction, host_register, bytes, extension, mode, transfer_zero, base_sp, forward=0
	.set .Lsd\@, scalar_offset0
	.if !\load || \transfer_zero || \mode == 1 || \mode == 2
	.set .Lsd\@, scalar_none
	.endif
	.set .Lsn\@, scalar_offset16
	.set .Lsm\@, scalar_offset0
	.if \load || \transfer_zero
	.set .Lsm\@, scalar_none
	.endif
scalar_entry \name, \forward, (17 - \load), .Lsd\@, .Lsn\@, .Lsm\@, scalar_none, 2
	.if !\load && (\forward & 2)
	mov x22, x0
	.endif
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	asr x21, x8, #32
	scalar_read \forward, 1, x24, x20
	.if \base_sp
	tst x24, #0xf
		.if \load
	.if \forward
	b.ne .Lscalar_forward_alignment_read\@
	.else
	b.ne .Lscalar_alignment_fault_read
	.endif
		.else
	.if \forward
	b.ne .Lscalar_forward_alignment_write\@
	.else
	b.ne .Lscalar_alignment_fault_write
	.endif
		.endif
	.endif
	add x25, x24, x21
	.if \mode >= 2
	mov x26, x24
	.else
	mov x26, x25
	.endif
	.if !\base_sp
	ldrb w9, [_cpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, .Lscalar_other_immediate_untagged\@
	ubfx x26, x26, #0, #56
.Lscalar_other_immediate_untagged\@:
	.endif
	.if \bytes > 1
	branch_if_cross_page x26, \bytes, x9, .Lscalar_other_immediate_cross\@
	.endif
	mov x7, x26
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \forward
	scalar_memory_transfer \load, \forward, 2, \instruction, \host_register
		.if !\transfer_zero
	str x0, [_cpu, x19]
		.endif
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \forward
		.if \transfer_zero
	mov x0, xzr
		.else
	.if !(\forward & 2)
	ldr x0, [_cpu, x19]
	.endif
		.endif
	scalar_memory_transfer \load, \forward, 2, \instruction, \host_register
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
	.if !(\forward & 2)
	ldr x0, [_cpu, x19]
	.endif
		.endif
	scalar_store_vector \forward, 2, 1
	transfer_slow 1, x26, \bytes
	.endif
	.if \mode == 1 || \mode == 2
	str x25, [_cpu, x20]
	.endif
	add _stream, _stream, #8
	next_gadget
	.endif
	.if \forward
.Lscalar_forward_alignment_read\@:
	b .Lscalar_alignment_fault_read
	.endif
	.if \forward
.Lscalar_forward_alignment_write\@:
	b .Lscalar_alignment_fault_write
	.endif
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, scalar_other_immediate_fast, \name, \load, \instruction, \host_register, \bytes, \extension, \mode, \transfer_zero, \base_sp
.endm

.macro scalar_other_immediate_operation name, load, instruction, host_register, bytes, extension
	scalar_other_immediate_fast scalar_other_\name\()_offset_r, \load, \instruction, \host_register, \bytes, \extension, 0, 0, 0
	scalar_other_immediate_fast scalar_other_\name\()_offset_z, \load, \instruction, \host_register, \bytes, \extension, 0, 1, 0
	scalar_other_immediate_fast scalar_other_\name\()_offset_r_sp, \load, \instruction, \host_register, \bytes, \extension, 0, 0, 1
	scalar_other_immediate_fast scalar_other_\name\()_offset_z_sp, \load, \instruction, \host_register, \bytes, \extension, 0, 1, 1
	scalar_other_immediate_fast scalar_other_\name\()_pre_r, \load, \instruction, \host_register, \bytes, \extension, 1, 0, 0
	scalar_other_immediate_fast scalar_other_\name\()_pre_z, \load, \instruction, \host_register, \bytes, \extension, 1, 1, 0
	scalar_other_immediate_fast scalar_other_\name\()_pre_r_sp, \load, \instruction, \host_register, \bytes, \extension, 1, 0, 1
	scalar_other_immediate_fast scalar_other_\name\()_pre_z_sp, \load, \instruction, \host_register, \bytes, \extension, 1, 1, 1
	scalar_other_immediate_fast scalar_other_\name\()_post_r, \load, \instruction, \host_register, \bytes, \extension, 2, 0, 0
	scalar_other_immediate_fast scalar_other_\name\()_post_z, \load, \instruction, \host_register, \bytes, \extension, 2, 1, 0
	scalar_other_immediate_fast scalar_other_\name\()_post_r_sp, \load, \instruction, \host_register, \bytes, \extension, 2, 0, 1
	scalar_other_immediate_fast scalar_other_\name\()_post_z_sp, \load, \instruction, \host_register, \bytes, \extension, 2, 1, 1
	.if \load
	/* Suppressed writeback requires Rn == Rt != 31, so only the ordinary
	 * transfer-register entry is reachable. */
	scalar_other_immediate_fast scalar_other_\name\()_post_suppress_r, \load, \instruction, \host_register, \bytes, \extension, 3, 0, 0
	.endif
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

/* Only translation-selected SP forms can reach these cold paths. All GPR-base
 * scalar-memory gadgets retain their prior branch shape. The operand has
 * already been consumed, so _stream points directly at the precise guest PC.
 */
.p2align 4
.Lscalar_alignment_fault_read:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	ldr x1, [_cpu, #UML_GADGET_CPU_SP]
	mov w2, #0
	bl NAME(uml_arm64_alignment_fault)
	ldp x1, x2, [sp], #16
	b uml_arm64_gadget_memory_fault

.p2align 4
.Lscalar_alignment_fault_write:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	ldr x1, [_cpu, #UML_GADGET_CPU_SP]
	mov w2, #1
	bl NAME(uml_arm64_alignment_fault)
	ldp x1, x2, [sp], #16
	b uml_arm64_gadget_memory_fault

/* Register-offset form. The upper operand half stores the translated Rm
 * CpuState byte offset. Extension (UXTW/X/SXTW) and optional scaling are fixed
 * in the gadget symbol. */
.macro scalar_other_register_fast name, load, instruction, host_register, bytes, address_extension, shifted, result_extension, transfer_zero, base_sp, forward=0
	.set .Lsd\@, scalar_offset0
	.if !\load || \transfer_zero
	.set .Lsd\@, scalar_none
	.endif
	.set .Lsn\@, scalar_offset16
	.set .Lsm\@, scalar_offset32
	.set .Lsa\@, scalar_offset0
	.if \load || \transfer_zero
	.set .Lsa\@, scalar_none
	.endif
scalar_entry \name, \forward, (17 - \load), .Lsd\@, .Lsn\@, .Lsm\@, .Lsa\@, 2
	.if !\load && (\forward & 4)
	mov x22, x0
	.endif
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	ubfx x21, x8, #32, #16
	scalar_read \forward, 1, x24, x20
	.if \base_sp
	tst x24, #0xf
		.if \load
	.if \forward
	b.ne .Lscalar_forward_alignment_read\@
	.else
	b.ne .Lscalar_alignment_fault_read
	.endif
		.else
	.if \forward
	b.ne .Lscalar_forward_alignment_write\@
	.else
	b.ne .Lscalar_alignment_fault_write
	.endif
		.endif
	.endif
	scalar_read \forward, 2, x26, x21
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
	.if !\base_sp
	ldrb w9, [_cpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, .Lscalar_other_register_untagged\@
	ubfx x26, x26, #0, #56
.Lscalar_other_register_untagged\@:
	.endif
	.if \bytes > 1
	branch_if_cross_page x26, \bytes, x9, .Lscalar_other_register_cross\@
	.endif
	mov x7, x26
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \forward
	scalar_memory_transfer \load, \forward, 4, \instruction, \host_register
		.if !\transfer_zero
	str x0, [_cpu, x19]
		.endif
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \forward
		.if \transfer_zero
	mov x0, xzr
		.else
	.if !(\forward & 4)
	ldr x0, [_cpu, x19]
	.endif
		.endif
	scalar_memory_transfer \load, \forward, 4, \instruction, \host_register
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
	.if !(\forward & 4)
	ldr x0, [_cpu, x19]
	.endif
		.endif
	scalar_store_vector \forward, 4, 1
	transfer_slow 1, x26, \bytes
	.endif
	add _stream, _stream, #8
	next_gadget
	.endif
	.if \forward
.Lscalar_forward_alignment_read\@:
	b .Lscalar_alignment_fault_read
	.endif
	.if \forward
.Lscalar_forward_alignment_write\@:
	b .Lscalar_alignment_fault_write
	.endif
	scalar_variants \forward, .Lsn\@, .Lsm\@, .Lsa\@, scalar_other_register_fast, \name, \load, \instruction, \host_register, \bytes, \address_extension, \shifted, \result_extension, \transfer_zero, \base_sp
.endm

.macro scalar_other_register_extension name, load, instruction, host_register, bytes, result_extension, address_extension, extension_name, shifted
	scalar_other_register_fast scalar_other_\name\()_\extension_name\()_\shifted\()_r, \load, \instruction, \host_register, \bytes, \address_extension, \shifted, \result_extension, 0, 0
	scalar_other_register_fast scalar_other_\name\()_\extension_name\()_\shifted\()_z, \load, \instruction, \host_register, \bytes, \address_extension, \shifted, \result_extension, 1, 0
	scalar_other_register_fast scalar_other_\name\()_\extension_name\()_\shifted\()_r_sp, \load, \instruction, \host_register, \bytes, \address_extension, \shifted, \result_extension, 0, 1
	scalar_other_register_fast scalar_other_\name\()_\extension_name\()_\shifted\()_z_sp, \load, \instruction, \host_register, \bytes, \address_extension, \shifted, \result_extension, 1, 1
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
.macro vector_other_immediate_fast name, load, instruction, host_register, bytes, mode, base_sp, forward=0
	.set .Lsn\@, scalar_offset16
scalar_entry \name, \forward, (17 - \load), scalar_none, .Lsn\@, scalar_none, scalar_none, 2
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	asr x21, x8, #32
	scalar_read \forward, 1, x24, x20
	.if \base_sp
	tst x24, #0xf
		.if \load
	.if \forward
	b.ne .Lscalar_forward_alignment_read\@
	.else
	b.ne .Lscalar_alignment_fault_read
	.endif
		.else
	.if \forward
	b.ne .Lscalar_forward_alignment_write\@
	.else
	b.ne .Lscalar_alignment_fault_write
	.endif
		.endif
	.endif
	add x25, x24, x21
	.if \mode == 2
	mov x26, x24
	.else
	mov x26, x25
	.endif
	.if !\base_sp || \mode != 0
	ldrh w9, [_cpu, #UML_GADGET_CPU_TAGGED_ADDRESS_STATE]
	cbnz w9, .Lvector_other_immediate_tagged\@
	.endif
	branch_if_cross_page x26, \bytes, x9, .Lvector_other_immediate_cross\@
	mov x7, x26
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \forward
	\instruction \host_register\()0, [x7]
	str q0, [_cpu, x19]
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \forward
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

	.if !\base_sp || \mode != 0
.Lvector_other_immediate_tagged\@:
	sub sp, sp, #32
	stp x1, x2, [sp, #16]
	.if \load
	movi v0.16b, #0
	.else
	ldr q0, [_cpu, x19]
	.endif
	str q0, [sp]
	mov x0, _cpu
	mov x1, _tlb
	mov x2, x26
	mov x3, sp
	mov x4, #\bytes
	mov w5, #(!\load)
	mov w6, #\base_sp
	mov w7, #((\bytes << 8) | 1)
	bl NAME(uml_arm64_simd_structure_memory_transfer)
	ldp x1, x2, [sp, #16]
	cbz w0, 97f
	.if \load
	ldr q0, [sp]
	str q0, [_cpu, x19]
	.endif
	.if \mode == 1 || \mode == 2
	str x25, [_cpu, x20]
	.endif
	add sp, sp, #32
	add _stream, _stream, #8
	next_gadget
97:
	add sp, sp, #32
	b uml_arm64_gadget_memory_fault
	.endif
	.if \forward
.Lscalar_forward_alignment_read\@:
	b .Lscalar_alignment_fault_read
	.endif
	.if \forward
.Lscalar_forward_alignment_write\@:
	b .Lscalar_alignment_fault_write
	.endif
	scalar_variants \forward, .Lsn\@, scalar_none, scalar_none, vector_other_immediate_fast, \name, \load, \instruction, \host_register, \bytes, \mode, \base_sp
.endm

.macro vector_other_immediate_operation name, load, instruction, host_register, bytes
	vector_other_immediate_fast vector_other_\name\()_offset, \load, \instruction, \host_register, \bytes, 0, 0
	vector_other_immediate_fast vector_other_\name\()_pre, \load, \instruction, \host_register, \bytes, 1, 0
	vector_other_immediate_fast vector_other_\name\()_post, \load, \instruction, \host_register, \bytes, 2, 0
	vector_other_immediate_fast vector_other_\name\()_offset_sp, \load, \instruction, \host_register, \bytes, 0, 1
	vector_other_immediate_fast vector_other_\name\()_pre_sp, \load, \instruction, \host_register, \bytes, 1, 1
	vector_other_immediate_fast vector_other_\name\()_post_sp, \load, \instruction, \host_register, \bytes, 2, 1
.endm

.macro vector_other_register_fast name, load, instruction, host_register, bytes, extension, shifted, base_sp, forward=0
	.set .Lsn\@, scalar_offset16
	.set .Lsm\@, scalar_offset32
scalar_entry \name, \forward, (17 - \load), scalar_none, .Lsn\@, .Lsm\@, scalar_none, 2
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	ubfx x21, x8, #32, #16
	scalar_read \forward, 1, x24, x20
	.if \base_sp
	tst x24, #0xf
		.if \load
	.if \forward
	b.ne .Lscalar_forward_alignment_read\@
	.else
	b.ne .Lscalar_alignment_fault_read
	.endif
		.else
	.if \forward
	b.ne .Lscalar_forward_alignment_write\@
	.else
	b.ne .Lscalar_alignment_fault_write
	.endif
		.endif
	.endif
	scalar_read \forward, 2, x26, x21
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
	ldrh w9, [_cpu, #UML_GADGET_CPU_TAGGED_ADDRESS_STATE]
	cbnz w9, .Lvector_other_register_tagged\@
	branch_if_cross_page x26, \bytes, x9, .Lvector_other_register_cross\@
	mov x7, x26
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \forward
	\instruction \host_register\()0, [x7]
	str q0, [_cpu, x19]
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \forward
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

.Lvector_other_register_tagged\@:
	sub sp, sp, #32
	stp x1, x2, [sp, #16]
	.if \load
	movi v0.16b, #0
	.else
	ldr q0, [_cpu, x19]
	.endif
	str q0, [sp]
	mov x0, _cpu
	mov x1, _tlb
	mov x2, x26
	mov x3, sp
	mov x4, #\bytes
	mov w5, #(!\load)
	mov w6, #\base_sp
	mov w7, #((\bytes << 8) | 1)
	bl NAME(uml_arm64_simd_structure_memory_transfer)
	ldp x1, x2, [sp, #16]
	cbz w0, 97f
	.if \load
	ldr q0, [sp]
	str q0, [_cpu, x19]
	.endif
	add sp, sp, #32
	add _stream, _stream, #8
	next_gadget
97:
	add sp, sp, #32
	b uml_arm64_gadget_memory_fault
	.if \forward
.Lscalar_forward_alignment_read\@:
	b .Lscalar_alignment_fault_read
	.endif
	.if \forward
.Lscalar_forward_alignment_write\@:
	b .Lscalar_alignment_fault_write
	.endif
	scalar_variants \forward, .Lsn\@, .Lsm\@, scalar_none, vector_other_register_fast, \name, \load, \instruction, \host_register, \bytes, \extension, \shifted, \base_sp
.endm

.macro vector_other_register_extension name, load, instruction, host_register, bytes, extension, extension_name, shifted
	vector_other_register_fast vector_other_\name\()_\extension_name\()_\shifted, \load, \instruction, \host_register, \bytes, \extension, \shifted, 0
	vector_other_register_fast vector_other_\name\()_\extension_name\()_\shifted\()_sp, \load, \instruction, \host_register, \bytes, \extension, \shifted, 1
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
 * writeback mode, sign extension, SP versus GPR base, and both XZR cases are
 * fixed by the symbol. Mode 0 is adjusted address without writeback, mode 1
 * pre-index, mode 2 post-index, and mode 3 post-index with suppressed load
 * writeback. */
.macro integer_pair_fast name, load, instruction, host_register, bytes, sign_extend, mode, first_zero, second_zero, sp_base, forward=0
	.set .Lsn\@, scalar_offset32
scalar_entry \name, \forward, (17 - \load), scalar_none, .Lsn\@, scalar_none, scalar_none, 2
	ldr x8, [_stream], #8
	and x22, x8, #0xffff
	ubfx x23, x8, #16, #16
	ubfx x24, x8, #32, #16
	asr x6, x8, #48
	scalar_read \forward, 1, x16, x24
	.if \sp_base
	tst x16, #0xf
		.if \load
	.if \forward
	b.ne .Lscalar_forward_alignment_pair_read\@
	.else
	b.ne uml_arm64_gadget_pair_alignment_fault_read
	.endif
		.else
	.if \forward
	b.ne .Lscalar_forward_alignment_pair_write\@
	.else
	b.ne uml_arm64_gadget_pair_alignment_fault_write
	.endif
		.endif
	.endif
	add x21, x16, x6
	.if \mode >= 2
	mov x19, x16
	.else
	mov x19, x21
	.endif
	.if !\sp_base
	/* CpuState keeps the MTE and TBI flags in the low and high bytes of
	 * this halfword. MTE requires the precise allocation-tag helper, while
	 * TBI alone can retain the ordinary pair fast path after stripping the
	 * logical tag from the effective address. */
	ldrh w9, [_cpu, #UML_GADGET_CPU_TAGGED_ADDRESS_STATE]
	cbz w9, .Linteger_pair_untagged\@
	tbnz w9, #0, .Linteger_pair_tagged\@
	ubfx x19, x19, #0, #56
.Linteger_pair_untagged\@:
	.elseif \mode != 0
	/* SP is never top-byte ignored, but writeback pair forms remain
	 * allocation-tag checked when MTE is active. */
	ldrb w9, [_cpu, #UML_GADGET_CPU_MTE_ENABLED]
	cbnz w9, .Linteger_pair_tagged\@
	.endif
	/* Comparing the page bits of the first and final transfer bytes catches
	 * both page crossing and address wraparound with one hot-path branch. */
	branch_if_cross_page x19, (2 * \bytes), x9, .Linteger_pair_cross\@
	.if \load
	mov x7, x19
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \forward
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
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \forward
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

	.if !\sp_base || \mode != 0
.Linteger_pair_tagged\@:
	sub sp, sp, #32
	stp x1, x2, [sp, #16]
	.if \load
	stp xzr, xzr, [sp]
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
	stp w6, w7, [sp]
		.else
	stp x6, x7, [sp]
		.endif
	.endif
	mov x0, _cpu
	mov x1, _tlb
	mov x2, x19
	mov x3, sp
	mov x4, #(2 * \bytes)
	mov w5, #(!\load)
	mov w6, #\sp_base
	mov w7, #((\bytes << 8) | 1)
	bl NAME(uml_arm64_simd_structure_memory_transfer)
	ldp x1, x2, [sp, #16]
	cbz w0, 97f
	.if \load
		.if \bytes == 4
	ldp w6, w7, [sp]
			.if \sign_extend
	sxtw x6, w6
	sxtw x7, w7
			.endif
		.else
	ldp x6, x7, [sp]
		.endif
		.if !\first_zero
	str x6, [_cpu, x22]
		.endif
		.if !\second_zero
	str x7, [_cpu, x23]
		.endif
	.endif
	.if \mode == 1 || \mode == 2
	str x21, [_cpu, x24]
	.endif
	add sp, sp, #32
	add _stream, _stream, #8
	next_gadget
97:
	add sp, sp, #32
	b uml_arm64_gadget_memory_fault
	.endif
	.if \forward
.Lscalar_forward_alignment_pair_read\@:
	b uml_arm64_gadget_pair_alignment_fault_read
	.endif
	.if \forward
.Lscalar_forward_alignment_pair_write\@:
	b uml_arm64_gadget_pair_alignment_fault_write
	.endif
	scalar_variants \forward, .Lsn\@, scalar_none, scalar_none, integer_pair_fast, \name, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, \first_zero, \second_zero, \sp_base
.endm

.macro integer_pair_mode name, load, instruction, host_register, bytes, sign_extend, mode
	integer_pair_fast integer_pair_\name\()_\mode\()_rr, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 0, 0, 0
	integer_pair_fast integer_pair_\name\()_\mode\()_rz, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 0, 1, 0
	integer_pair_fast integer_pair_\name\()_\mode\()_zr, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 1, 0, 0
	integer_pair_fast integer_pair_\name\()_\mode\()_zz, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 1, 1, 0
	integer_pair_fast integer_pair_\name\()_\mode\()_rr_sp, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 0, 0, 1
	integer_pair_fast integer_pair_\name\()_\mode\()_rz_sp, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 0, 1, 1
	integer_pair_fast integer_pair_\name\()_\mode\()_zr_sp, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 1, 0, 1
	integer_pair_fast integer_pair_\name\()_\mode\()_zz_sp, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 1, 1, 1
.endm

.macro integer_pair_store_operation name, instruction, host_register, bytes
	integer_pair_mode \name, 0, \instruction, \host_register, \bytes, 0, 0
	integer_pair_mode \name, 0, \instruction, \host_register, \bytes, 0, 1
	integer_pair_mode \name, 0, \instruction, \host_register, \bytes, 0, 2
.endm

.macro integer_pair_load_operation name, instruction, host_register, bytes, sign_extend
	integer_pair_mode \name, 1, \instruction, \host_register, \bytes, \sign_extend, 0
	integer_pair_mode \name, 1, \instruction, \host_register, \bytes, \sign_extend, 1
	integer_pair_mode \name, 1, \instruction, \host_register, \bytes, \sign_extend, 2
	/* Mode 3 is post-index with WBSUPPRESS and is load-only. */
	integer_pair_mode \name, 1, \instruction, \host_register, \bytes, \sign_extend, 3
.endm

integer_pair_store_operation store32, stp, w, 4
integer_pair_load_operation load32, ldp, w, 4, 0
integer_pair_load_operation load32s, ldpsw, x, 4, 1
integer_pair_store_operation store64, stp, x, 8
integer_pair_load_operation load64, ldp, x, 8, 0

.purgem integer_pair_load_operation
.purgem integer_pair_store_operation
.purgem integer_pair_mode
.purgem integer_pair_fast

/* Translation-specialized SIMD&FP pairs. Operand fields match the integer
 * pair descriptor, but the first two offsets address VectorRegister slots.
 * Width, direction, address mode, and SP versus GPR base are fixed by the
 * selected symbol. */
.macro vector_pair_fast name, load, instruction, host_register, bytes, mode, sp_base, forward=0
	.set .Lsn\@, scalar_offset32
scalar_entry \name, \forward, (17 - \load), scalar_none, .Lsn\@, scalar_none, scalar_none, 2
	ldr x8, [_stream], #8
	and x22, x8, #0xffff
	ubfx x23, x8, #16, #16
	ubfx x24, x8, #32, #16
	asr x6, x8, #48
	scalar_read \forward, 1, x16, x24
	.if \sp_base
	tst x16, #0xf
		.if \load
	.if \forward
	b.ne .Lscalar_forward_alignment_pair_read\@
	.else
	b.ne uml_arm64_gadget_pair_alignment_fault_read
	.endif
		.else
	.if \forward
	b.ne .Lscalar_forward_alignment_pair_write\@
	.else
	b.ne uml_arm64_gadget_pair_alignment_fault_write
	.endif
		.endif
	.endif
	add x21, x16, x6
	.if \mode == 2
	mov x19, x16
	.else
	mov x19, x21
	.endif
	.if !\sp_base || \mode != 0
	ldrh w9, [_cpu, #UML_GADGET_CPU_TAGGED_ADDRESS_STATE]
	cbnz w9, .Lvector_pair_tagged\@
	.endif
	branch_if_cross_page x19, (2 * \bytes), x9, .Lvector_pair_cross\@
	.if \load
	mov x7, x19
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \forward
	mov x19, x7
	\instruction \host_register\()0, \host_register\()1, [x19]
	str q0, [_cpu, x22]
	str q1, [_cpu, x23]
	.else
	mov x7, x19
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \forward
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

	.if !\sp_base || \mode != 0
.Lvector_pair_tagged\@:
	sub sp, sp, #48
	stp x1, x2, [sp, #32]
	.if \load
	movi v0.16b, #0
	movi v1.16b, #0
	.else
	ldr q0, [_cpu, x22]
	ldr q1, [_cpu, x23]
	/* The helper moves one contiguous range, so a narrower pair has to be
	 * packed into it exactly as the cross-page path does. */
		.if \bytes == 4
	mov v0.s[1], v1.s[0]
		.elseif \bytes == 8
	mov v0.d[1], v1.d[0]
		.endif
	.endif
	stp q0, q1, [sp]
	mov x0, _cpu
	mov x1, _tlb
	mov x2, x19
	mov x3, sp
	mov x4, #(2 * \bytes)
	mov w5, #(!\load)
	mov w6, #\sp_base
	mov w7, #(((2 * \bytes) << 8) | 1)
	bl NAME(uml_arm64_simd_structure_memory_transfer)
	ldp x1, x2, [sp, #32]
	cbz w0, 97f
	.if \load
	ldp q0, q1, [sp]
	/* The contiguous range holds both narrow elements in q0, so split them
	 * back into two zero-extended destinations. */
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
	.endif
	.if \mode == 1 || \mode == 2
	str x21, [_cpu, x24]
	.endif
	add sp, sp, #48
	add _stream, _stream, #8
	next_gadget
97:
	add sp, sp, #48
	b uml_arm64_gadget_memory_fault
	.endif
	.if \forward
.Lscalar_forward_alignment_pair_read\@:
	b uml_arm64_gadget_pair_alignment_fault_read
	.endif
	.if \forward
.Lscalar_forward_alignment_pair_write\@:
	b uml_arm64_gadget_pair_alignment_fault_write
	.endif
	scalar_variants \forward, .Lsn\@, scalar_none, scalar_none, vector_pair_fast, \name, \load, \instruction, \host_register, \bytes, \mode, \sp_base
.endm

.macro vector_pair_operation name, load, instruction, host_register, bytes
	vector_pair_fast vector_pair_\name\()_0, \load, \instruction, \host_register, \bytes, 0, 0
	vector_pair_fast vector_pair_\name\()_1, \load, \instruction, \host_register, \bytes, 1, 0
	vector_pair_fast vector_pair_\name\()_2, \load, \instruction, \host_register, \bytes, 2, 0
	vector_pair_fast vector_pair_\name\()_0_sp, \load, \instruction, \host_register, \bytes, 0, 1
	vector_pair_fast vector_pair_\name\()_1_sp, \load, \instruction, \host_register, \bytes, 1, 1
	vector_pair_fast vector_pair_\name\()_2_sp, \load, \instruction, \host_register, \bytes, 2, 1
.endm

vector_pair_operation store32, 0, stp, s, 4
vector_pair_operation load32, 1, ldp, s, 4
vector_pair_operation store64, 0, stp, d, 8
vector_pair_operation load64, 1, ldp, d, 8
vector_pair_operation store128, 0, stp, q, 16
vector_pair_operation load128, 1, ldp, q, 16

.purgem vector_pair_operation
.purgem vector_pair_fast

/* Only SP-specialized pair gadgets can reach these cold paths. GPR-base
 * gadgets contain no alignment test or branch. */
.p2align 4
uml_arm64_gadget_pair_alignment_fault_read:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x16
	mov w2, #0
	bl NAME(uml_arm64_alignment_fault)
	ldp x1, x2, [sp], #16
	b uml_arm64_gadget_memory_fault

.p2align 4
uml_arm64_gadget_pair_alignment_fault_write:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x16
	mov w2, #1
	bl NAME(uml_arm64_alignment_fault)
	ldp x1, x2, [sp], #16
	b uml_arm64_gadget_memory_fault

/* LDRAA/LDRAB authenticate the base with a zero modifier before applying the
 * signed scaled offset. Key, writeback, SP alignment, and XZR destination are
 * fixed by the builder. Stream: [packed direct offsets/immediate][guest PC]. */
.macro pauth_load name, operation, writeback, base_sp, discard
gadget pauth_load_\name
	ldr x8, [_stream], #8
	.if !\discard
	and w19, w8, #0xffff
	.endif
	asr x21, x8, #32
	.if \base_sp
	ldr x7, [_cpu, #UML_GADGET_CPU_SP]
	.else
	ubfx w20, w8, #16, #16
	ldr x7, [_cpu, x20]
	.endif
	.if \base_sp
	tst x7, #0xf
	b.ne .Lscalar_alignment_fault_read
	.endif
	software_pointer_auth x7, xzr, \operation
	add x25, x7, x21
	branch_if_cross_page x25, 8, x9, pauth_load_\name\()_cross
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
	.if \base_sp
	str x25, [_cpu, #UML_GADGET_CPU_SP]
	.else
	str x25, [_cpu, x20]
	.endif
	.endif
	add _stream, _stream, #8
	next_gadget
.endm

.macro pauth_load_modes key, operation
pauth_load \key\()_o_rr, \operation, 0, 0, 0
pauth_load \key\()_o_rz, \operation, 0, 0, 1
pauth_load \key\()_o_sr, \operation, 0, 1, 0
pauth_load \key\()_o_sz, \operation, 0, 1, 1
pauth_load \key\()_w_rr, \operation, 1, 0, 0
pauth_load \key\()_w_rz, \operation, 1, 0, 1
pauth_load \key\()_w_sr, \operation, 1, 1, 0
pauth_load \key\()_w_sz, \operation, 1, 1, 1
.endm

pauth_load_modes a, 6
pauth_load_modes b, 7

.purgem pauth_load_modes
.purgem pauth_load

uml_arm64_gadget_memory_fault:
	ldr x8, [_stream]
	str _stream, [_cpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_MEMORY_FAULT
	b uml_arm64_gadget_leave
