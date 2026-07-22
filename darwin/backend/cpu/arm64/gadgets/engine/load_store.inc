gadget nop
	next_gadget

.macro load_base_and_address
	ldr x8, [_stream], #8
	and x19, x8, #0x1f
	ubfx x20, x8, #8, #5
	ubfx x21, x8, #16, #32
	cmp x20, #31
	b.eq 90f
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x9, x20, lsl #3]
	b 91f
90:	ldr x7, [_cpu, #UML_GADGET_CPU_SP]
91:	add x7, x7, x21
.endm

/* Direct host accesses are used within one translated 4 KiB page; a dedicated
 * TLB-copy helper handles the rare cross-page case without interpreting the
 * instruction. The stream is [instruction][guest PC].
 */
.macro select_memory_path last_offset, slow_path
	and x9, x7, #0xfff
	cmp x9, #\last_offset
	b.hi \slow_path
	/* Skip the instruction while leaving the guest PC at _stream for the
	 * shared memory-fault handler.
	 */
	add _stream, _stream, #8
.endm

gadget load32_unsigned
	load_base_and_address
	select_memory_path 4092, load32_cross_page
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	ldr w0, [x7]
	b load32_store_result
load32_cross_page:
	add _stream, _stream, #8
	transfer_slow 0, x7, 4
	fmov w0, s0
load32_store_result:
	cmp x19, #31
	b.eq 1f
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x0, [x9, x19, lsl #3]
1:	add _stream, _stream, #8
	next_gadget

gadget load64_unsigned
	load_base_and_address
	select_memory_path 4088, load64_cross_page
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	ldr x0, [x7]
	b load64_store_result
load64_cross_page:
	add _stream, _stream, #8
	transfer_slow 0, x7, 8
	fmov x0, d0
load64_store_result:
	cmp x19, #31
	b.eq 1f
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x0, [x9, x19, lsl #3]
1:	add _stream, _stream, #8
	next_gadget

gadget store32_unsigned
	load_base_and_address
	select_memory_path 4092, store32_cross_page
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	cmp x19, #31
	b.eq 1f
	add x9, _cpu, #UML_GADGET_CPU_X0
	add x9, x9, x19, lsl #3
	ldr w0, [x9]
	b 2f
1:	mov w0, wzr
2:	str w0, [x7]
	add _stream, _stream, #8
	next_gadget
store32_cross_page:
	add _stream, _stream, #8
	cmp x19, #31
	b.eq 3f
	add x9, _cpu, #UML_GADGET_CPU_X0
	add x9, x9, x19, lsl #3
	ldr w0, [x9]
	b 4f
3:	mov w0, wzr
4:	fmov s0, w0
	transfer_slow 1, x7, 4
	add _stream, _stream, #8
	next_gadget

gadget store64_unsigned
	load_base_and_address
	select_memory_path 4088, store64_cross_page
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	cmp x19, #31
	b.eq 1f
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x0, [x9, x19, lsl #3]
	b 2f
1:	mov x0, xzr
2:	str x0, [x7]
	add _stream, _stream, #8
	next_gadget
store64_cross_page:
	add _stream, _stream, #8
	cmp x19, #31
	b.eq 3f
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x0, [x9, x19, lsl #3]
	b 4f
3:	mov x0, xzr
4:	fmov d0, x0
	transfer_slow 1, x7, 8
	add _stream, _stream, #8
	next_gadget

/* Scalar integer unsigned-immediate loads/stores not handled by the compact
 * word/doubleword gadgets above. Stream: [instruction][guest PC].
 */
gadget load_store_integer_unsigned
	ldr x8, [_stream]
	and w19, w8, #0x1f
	ubfx w20, w8, #5, #5
	ubfx w21, w8, #30, #2
	ubfx w22, w8, #22, #2
	ubfx x23, x8, #10, #12
	cmp w20, #31
	b.eq integer_unsigned_load_sp
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x9, x20, lsl #3]
	b integer_unsigned_base_ready
integer_unsigned_load_sp:
	ldr x7, [_cpu, #UML_GADGET_CPU_SP]
integer_unsigned_base_ready:
	lsl x23, x23, x21
	add x7, x7, x23
	mov x24, #1
	lsl x24, x24, x21
	and x9, x7, #0xfff
	add x9, x9, x24
	cmp x9, #4096
	b.hi integer_unsigned_cross_page
	/* Consume the instruction, leaving the PC for a translation fault. */
	add _stream, _stream, #8
	cbz w22, integer_unsigned_store
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	cmp w21, #0
	b.eq integer_unsigned_load8
	cmp w21, #1
	b.eq integer_unsigned_load16
	/* Only LDRSW reaches this path for a word transfer. */
	ldr w0, [x7]
	sxtw x0, w0
	b integer_unsigned_load_result
integer_unsigned_load8:
	ldrb w0, [x7]
integer_unsigned_loaded8:
	cmp w22, #2
	b.eq integer_unsigned_sign8_x
	cmp w22, #3
	b.ne integer_unsigned_load_result
	sxtb w0, w0
	b integer_unsigned_load_result
integer_unsigned_sign8_x:
	sxtb x0, w0
	b integer_unsigned_load_result
integer_unsigned_load16:
	ldrh w0, [x7]
integer_unsigned_loaded16:
	cmp w22, #2
	b.eq integer_unsigned_sign16_x
	cmp w22, #3
	b.ne integer_unsigned_load_result
	sxth w0, w0
	b integer_unsigned_load_result
integer_unsigned_sign16_x:
	sxth x0, w0
integer_unsigned_load_result:
	cmp w19, #31
	b.eq integer_unsigned_done
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x0, [x9, x19, lsl #3]
	b integer_unsigned_done
integer_unsigned_store:
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	cmp w19, #31
	b.eq integer_unsigned_zero_store
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x0, [x9, x19, lsl #3]
	b integer_unsigned_store_ready
integer_unsigned_zero_store:
	mov x0, xzr
integer_unsigned_store_ready:
	cmp w21, #0
	b.eq integer_unsigned_store8
	strh w0, [x7]
	b integer_unsigned_done
integer_unsigned_store8:
	strb w0, [x7]
	b integer_unsigned_done
integer_unsigned_cross_page:
	add _stream, _stream, #8
	cbz w22, integer_unsigned_cross_store
	cmp w21, #0
	b.eq integer_unsigned_cross_load8
	cmp w21, #1
	b.eq integer_unsigned_cross_load16
	transfer_slow 0, x7, 4
	fmov w0, s0
	sxtw x0, w0
	b integer_unsigned_load_result
integer_unsigned_cross_load8:
	transfer_slow 0, x7, 1
	umov w0, v0.b[0]
	b integer_unsigned_loaded8
integer_unsigned_cross_load16:
	transfer_slow 0, x7, 2
	umov w0, v0.h[0]
	b integer_unsigned_loaded16
integer_unsigned_cross_store:
	cmp w19, #31
	b.eq integer_unsigned_cross_zero_store
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x0, [x9, x19, lsl #3]
	b integer_unsigned_cross_store_ready
integer_unsigned_cross_zero_store:
	mov x0, xzr
integer_unsigned_cross_store_ready:
	fmov d0, x0
	cmp w21, #0
	b.eq integer_unsigned_cross_store8
	transfer_slow 1, x7, 2
	b integer_unsigned_done
integer_unsigned_cross_store8:
	transfer_slow 1, x7, 1
integer_unsigned_done:
	add _stream, _stream, #8
	next_gadget

/* SIMD&FP unsigned-immediate loads/stores, including 128-bit Q transfers.
 * Scalar vector loads clear the unused high bytes as required by A64.
 * Stream: [instruction][guest PC].
 */
gadget load_store_vector_unsigned
	ldr x8, [_stream]
	and w19, w8, #0x1f
	ubfx w20, w8, #5, #5
	ubfx w21, w8, #30, #2
	ubfx w22, w8, #22, #2
	ubfx x23, x8, #10, #12
	cmp w22, #2
	b.lo vector_unsigned_size_ready
	mov w21, #4
vector_unsigned_size_ready:
	cmp w20, #31
	b.eq vector_unsigned_load_sp
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x9, x20, lsl #3]
	b vector_unsigned_base_ready
vector_unsigned_load_sp:
	ldr x7, [_cpu, #UML_GADGET_CPU_SP]
vector_unsigned_base_ready:
	lsl x23, x23, x21
	add x7, x7, x23
	mov x24, #1
	lsl x24, x24, x21
	and x9, x7, #0xfff
	add x9, x9, x24
	cmp x9, #4096
	b.hi vector_unsigned_cross_page
	add _stream, _stream, #8
	tbnz x8, #22, vector_unsigned_load
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	add x9, _cpu, #UML_GADGET_CPU_V0
	add x9, x9, x19, lsl #4
	ldr q0, [x9]
	cmp w21, #0
	b.eq vector_unsigned_store8
	cmp w21, #1
	b.eq vector_unsigned_store16
	cmp w21, #2
	b.eq vector_unsigned_store32
	cmp w21, #3
	b.eq vector_unsigned_store64
	str q0, [x7]
	b vector_unsigned_done
vector_unsigned_store8:
	str b0, [x7]
	b vector_unsigned_done
vector_unsigned_store16:
	str h0, [x7]
	b vector_unsigned_done
vector_unsigned_store32:
	str s0, [x7]
	b vector_unsigned_done
vector_unsigned_store64:
	str d0, [x7]
	b vector_unsigned_done
vector_unsigned_load:
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	movi v0.16b, #0
	cmp w21, #0
	b.eq vector_unsigned_load8
	cmp w21, #1
	b.eq vector_unsigned_load16
	cmp w21, #2
	b.eq vector_unsigned_load32
	cmp w21, #3
	b.eq vector_unsigned_load64
	ldr q0, [x7]
	b vector_unsigned_load_store
vector_unsigned_load8:
	ldr b0, [x7]
	b vector_unsigned_load_store
vector_unsigned_load16:
	ldr h0, [x7]
	b vector_unsigned_load_store
vector_unsigned_load32:
	ldr s0, [x7]
	b vector_unsigned_load_store
vector_unsigned_load64:
	ldr d0, [x7]
vector_unsigned_load_store:
	add x9, _cpu, #UML_GADGET_CPU_V0
	add x9, x9, x19, lsl #4
	str q0, [x9]
	b vector_unsigned_done
vector_unsigned_cross_page:
	add _stream, _stream, #8
	tbnz x8, #22, vector_unsigned_cross_load
	add x9, _cpu, #UML_GADGET_CPU_V0
	add x9, x9, x19, lsl #4
	ldr q0, [x9]
	cmp w21, #0
	b.eq vector_unsigned_cross_store8
	cmp w21, #1
	b.eq vector_unsigned_cross_store16
	cmp w21, #2
	b.eq vector_unsigned_cross_store32
	cmp w21, #3
	b.eq vector_unsigned_cross_store64
	transfer_slow 1, x7, 16
	b vector_unsigned_done
vector_unsigned_cross_store8:
	transfer_slow 1, x7, 1
	b vector_unsigned_done
vector_unsigned_cross_store16:
	transfer_slow 1, x7, 2
	b vector_unsigned_done
vector_unsigned_cross_store32:
	transfer_slow 1, x7, 4
	b vector_unsigned_done
vector_unsigned_cross_store64:
	transfer_slow 1, x7, 8
	b vector_unsigned_done
vector_unsigned_cross_load:
	cmp w21, #0
	b.eq vector_unsigned_cross_load8
	cmp w21, #1
	b.eq vector_unsigned_cross_load16
	cmp w21, #2
	b.eq vector_unsigned_cross_load32
	cmp w21, #3
	b.eq vector_unsigned_cross_load64
	transfer_slow 0, x7, 16
	b vector_unsigned_load_store
vector_unsigned_cross_load8:
	transfer_slow 0, x7, 1
	b vector_unsigned_load_store
vector_unsigned_cross_load16:
	transfer_slow 0, x7, 2
	b vector_unsigned_load_store
vector_unsigned_cross_load32:
	transfer_slow 0, x7, 4
	b vector_unsigned_load_store
vector_unsigned_cross_load64:
	transfer_slow 0, x7, 8
	b vector_unsigned_load_store
vector_unsigned_done:
	add _stream, _stream, #8
	next_gadget

/* SIMD&FP unscaled, pre/post-indexed, and register-offset transfers.
 * Stream: [instruction][guest PC].
 */
gadget load_store_vector_other
	ldr x8, [_stream]
	and w19, w8, #0x1f
	ubfx w20, w8, #5, #5
	ubfx w21, w8, #30, #2
	ubfx w22, w8, #22, #2
	cmp w22, #2
	b.lo vector_other_size_ready
	mov w21, #4
vector_other_size_ready:
	cmp w20, #31
	b.eq vector_other_load_sp
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x24, [x9, x20, lsl #3]
	b vector_other_base_ready
vector_other_load_sp:
	ldr x24, [_cpu, #UML_GADGET_CPU_SP]
vector_other_base_ready:
	tbz x8, #21, vector_other_immediate
	mov w23, wzr
	ubfx w6, w8, #16, #5
	cmp w6, #31
	b.eq vector_other_zero_offset
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x26, [x9, x6, lsl #3]
	b vector_other_offset_loaded
vector_other_zero_offset:
	mov x26, xzr
vector_other_offset_loaded:
	ubfx w6, w8, #13, #3
	cmp w6, #2
	b.eq vector_other_uxtw
	cmp w6, #6
	b.eq vector_other_sxtw
	b vector_other_offset_extended
vector_other_uxtw:
	mov w26, w26
	b vector_other_offset_extended
vector_other_sxtw:
	sxtw x26, w26
vector_other_offset_extended:
	tbz x8, #12, vector_other_offset_ready
	lsl x26, x26, x21
vector_other_offset_ready:
	add x26, x24, x26
	b vector_other_address_ready
vector_other_immediate:
	sbfx x6, x8, #12, #9
	add x25, x24, x6
	ubfx w23, w8, #10, #2
	cmp w23, #1
	csel x26, x24, x25, eq
vector_other_address_ready:
	mov x27, #1
	lsl x27, x27, x21
	and x9, x26, #0xfff
	add x9, x9, x27
	cmp x9, #4096
	b.hi vector_other_cross_page
	add _stream, _stream, #8
	tbnz x8, #22, vector_other_load
	mov x7, x26
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	add x9, _cpu, #UML_GADGET_CPU_V0
	add x9, x9, x19, lsl #4
	ldr q0, [x9]
	cmp w21, #0
	b.eq vector_other_store8
	cmp w21, #1
	b.eq vector_other_store16
	cmp w21, #2
	b.eq vector_other_store32
	cmp w21, #3
	b.eq vector_other_store64
	str q0, [x7]
	b vector_other_writeback
vector_other_store8:
	str b0, [x7]
	b vector_other_writeback
vector_other_store16:
	str h0, [x7]
	b vector_other_writeback
vector_other_store32:
	str s0, [x7]
	b vector_other_writeback
vector_other_store64:
	str d0, [x7]
	b vector_other_writeback
vector_other_load:
	mov x7, x26
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	movi v0.16b, #0
	cmp w21, #0
	b.eq vector_other_load8
	cmp w21, #1
	b.eq vector_other_load16
	cmp w21, #2
	b.eq vector_other_load32
	cmp w21, #3
	b.eq vector_other_load64
	ldr q0, [x7]
	b vector_other_load_store
vector_other_load8:
	ldr b0, [x7]
	b vector_other_load_store
vector_other_load16:
	ldr h0, [x7]
	b vector_other_load_store
vector_other_load32:
	ldr s0, [x7]
	b vector_other_load_store
vector_other_load64:
	ldr d0, [x7]
vector_other_load_store:
	add x9, _cpu, #UML_GADGET_CPU_V0
	add x9, x9, x19, lsl #4
	str q0, [x9]
	b vector_other_writeback
vector_other_cross_page:
	add _stream, _stream, #8
	tbnz x8, #22, vector_other_cross_load
	add x9, _cpu, #UML_GADGET_CPU_V0
	add x9, x9, x19, lsl #4
	ldr q0, [x9]
	cmp w21, #0
	b.eq vector_other_cross_store8
	cmp w21, #1
	b.eq vector_other_cross_store16
	cmp w21, #2
	b.eq vector_other_cross_store32
	cmp w21, #3
	b.eq vector_other_cross_store64
	transfer_slow 1, x26, 16
	b vector_other_writeback
vector_other_cross_store8:
	transfer_slow 1, x26, 1
	b vector_other_writeback
vector_other_cross_store16:
	transfer_slow 1, x26, 2
	b vector_other_writeback
vector_other_cross_store32:
	transfer_slow 1, x26, 4
	b vector_other_writeback
vector_other_cross_store64:
	transfer_slow 1, x26, 8
	b vector_other_writeback
vector_other_cross_load:
	cmp w21, #0
	b.eq vector_other_cross_load8
	cmp w21, #1
	b.eq vector_other_cross_load16
	cmp w21, #2
	b.eq vector_other_cross_load32
	cmp w21, #3
	b.eq vector_other_cross_load64
	transfer_slow 0, x26, 16
	b vector_other_load_store
vector_other_cross_load8:
	transfer_slow 0, x26, 1
	b vector_other_load_store
vector_other_cross_load16:
	transfer_slow 0, x26, 2
	b vector_other_load_store
vector_other_cross_load32:
	transfer_slow 0, x26, 4
	b vector_other_load_store
vector_other_cross_load64:
	transfer_slow 0, x26, 8
	b vector_other_load_store
vector_other_writeback:
	cmp w23, #1
	b.eq vector_other_do_writeback
	cmp w23, #3
	b.ne vector_other_done
vector_other_do_writeback:
	cmp w20, #31
	b.eq vector_other_write_sp
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x25, [x9, x20, lsl #3]
	b vector_other_done
vector_other_write_sp:
	str x25, [_cpu, #UML_GADGET_CPU_SP]
vector_other_done:
	add _stream, _stream, #8
	next_gadget

/* Integer load/store instructions using an unscaled immediate, pre/post
 * indexing, or a register offset. Stream: [instruction][guest PC].
 */
gadget load_store_integer_other
	ldr x8, [_stream]
	and w19, w8, #0x1f
	ubfx w20, w8, #5, #5
	ubfx w21, w8, #30, #2
	ubfx w22, w8, #22, #2

	cmp w20, #31
	b.eq integer_other_load_sp
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x24, [x9, x20, lsl #3]
	b integer_other_base_ready
integer_other_load_sp:
	ldr x24, [_cpu, #UML_GADGET_CPU_SP]
integer_other_base_ready:
	tbz x8, #21, integer_other_immediate

	/* Register offset: UXTW/LSL/SXTW/SXTX, optionally scaled by the
	 * transfer size. Builder validation rejects other option values.
	 */
	mov w23, wzr
	ubfx w6, w8, #16, #5
	cmp w6, #31
	b.eq integer_other_zero_offset
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x26, [x9, x6, lsl #3]
	b integer_other_offset_loaded
integer_other_zero_offset:
	mov x26, xzr
integer_other_offset_loaded:
	ubfx w6, w8, #13, #3
	cmp w6, #2
	b.eq integer_other_uxtw
	cmp w6, #6
	b.eq integer_other_sxtw
	b integer_other_offset_extended
integer_other_uxtw:
	and x26, x26, #0xffffffff
	b integer_other_offset_extended
integer_other_sxtw:
	sxtw x26, w26
integer_other_offset_extended:
	tbz x8, #12, integer_other_offset_ready
	lsl x26, x26, x21
integer_other_offset_ready:
	add x26, x24, x26
	b integer_other_address_ready

integer_other_immediate:
	sbfx x6, x8, #12, #9
	add x25, x24, x6
	ubfx w23, w8, #10, #2
	cmp w23, #1
	csel x26, x24, x25, eq

integer_other_address_ready:
	/* Host pages need not be adjacent even when guest pages are. */
	mov x27, #1
	lsl x27, x27, x21
	and x9, x26, #0xfff
	add x9, x9, x27
	cmp x9, #4096
	b.hi integer_other_cross_page
	/* Consume the instruction, leaving the guest PC for a fault. */
	add _stream, _stream, #8
	cbz w22, integer_other_store

	mov x7, x26
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	cmp w21, #0
	b.eq integer_other_load8
	cmp w21, #1
	b.eq integer_other_load16
	cmp w21, #2
	b.eq integer_other_load32
	ldr x0, [x7]
	b integer_other_load_result
integer_other_load8:
	ldrb w0, [x7]
integer_other_loaded8:
	cmp w22, #2
	b.eq integer_other_sign8_x
	cmp w22, #3
	b.ne integer_other_load_result
	sxtb w0, w0
	b integer_other_load_result
integer_other_sign8_x:
	sxtb x0, w0
	b integer_other_load_result
integer_other_load16:
	ldrh w0, [x7]
integer_other_loaded16:
	cmp w22, #2
	b.eq integer_other_sign16_x
	cmp w22, #3
	b.ne integer_other_load_result
	sxth w0, w0
	b integer_other_load_result
integer_other_sign16_x:
	sxth x0, w0
	b integer_other_load_result
integer_other_load32:
	ldr w0, [x7]
integer_other_loaded32:
	cmp w22, #2
	b.ne integer_other_load_result
	sxtw x0, w0
integer_other_load_result:
	cmp w19, #31
	b.eq integer_other_writeback
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x0, [x9, x19, lsl #3]
	b integer_other_writeback

integer_other_store:
	mov x7, x26
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	cmp w19, #31
	b.eq integer_other_zero_store
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x0, [x9, x19, lsl #3]
	b integer_other_store_value_ready
integer_other_zero_store:
	mov x0, xzr
integer_other_store_value_ready:
	cmp w21, #0
	b.eq integer_other_store8
	cmp w21, #1
	b.eq integer_other_store16
	cmp w21, #2
	b.eq integer_other_store32
	str x0, [x7]
	b integer_other_writeback
integer_other_store8:
	strb w0, [x7]
	b integer_other_writeback
integer_other_store16:
	strh w0, [x7]
	b integer_other_writeback
integer_other_store32:
	str w0, [x7]
	b integer_other_writeback

integer_other_cross_page:
	add _stream, _stream, #8
	cbz w22, integer_other_cross_store
	cmp w21, #0
	b.eq integer_other_cross_load8
	cmp w21, #1
	b.eq integer_other_cross_load16
	cmp w21, #2
	b.eq integer_other_cross_load32
	transfer_slow 0, x26, 8
	fmov x0, d0
	b integer_other_load_result
integer_other_cross_load8:
	transfer_slow 0, x26, 1
	umov w0, v0.b[0]
	b integer_other_loaded8
integer_other_cross_load16:
	transfer_slow 0, x26, 2
	umov w0, v0.h[0]
	b integer_other_loaded16
integer_other_cross_load32:
	transfer_slow 0, x26, 4
	fmov w0, s0
	b integer_other_loaded32
integer_other_cross_store:
	cmp w19, #31
	b.eq integer_other_cross_zero_store
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x0, [x9, x19, lsl #3]
	b integer_other_cross_store_ready
integer_other_cross_zero_store:
	mov x0, xzr
integer_other_cross_store_ready:
	fmov d0, x0
	cmp w21, #0
	b.eq integer_other_cross_store8
	cmp w21, #1
	b.eq integer_other_cross_store16
	cmp w21, #2
	b.eq integer_other_cross_store32
	transfer_slow 1, x26, 8
	b integer_other_writeback
integer_other_cross_store8:
	transfer_slow 1, x26, 1
	b integer_other_writeback
integer_other_cross_store16:
	transfer_slow 1, x26, 2
	b integer_other_writeback
integer_other_cross_store32:
	transfer_slow 1, x26, 4

integer_other_writeback:
	cmp w23, #1
	b.eq integer_other_do_writeback
	cmp w23, #3
	b.ne integer_other_done
integer_other_do_writeback:
	cmp w20, #31
	b.eq integer_other_write_sp
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x25, [x9, x20, lsl #3]
	b integer_other_done
integer_other_write_sp:
	str x25, [_cpu, #UML_GADGET_CPU_SP]
integer_other_done:
	add _stream, _stream, #8
	next_gadget

/* Integer LDP/STP, including signed-offset, pre-index, and post-index forms.
 * Stream: [instruction][guest PC]. Both addresses are translated before an
 * STP changes memory, preserving restartability across a 4 KiB boundary.
 */
gadget load_store_pair
	ldr x8, [_stream]
	and w22, w8, #0x1f
	ubfx w23, w8, #10, #5
	ubfx w24, w8, #5, #5
	ubfx w25, w8, #30, #2
	ubfx w26, w8, #22, #1
	ubfx w27, w8, #23, #2
	sbfx x6, x8, #15, #7
	cmp w25, #2
	cset w5, eq
	add w5, w5, #2
	lsl x6, x6, x5
	cmp w24, #31
	b.eq pair_load_sp
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x16, [x9, x24, lsl #3]
	b pair_base_ready
pair_load_sp:
	ldr x16, [_cpu, #UML_GADGET_CPU_SP]
pair_base_ready:
	add x21, x16, x6
	cmp w27, #1
	csel x19, x16, x21, eq
	mov x5, #4
	mov x9, #8
	cmp w25, #2
	csel x5, x5, x9, ne
	/* Adjacent guest pages need not have adjacent host mappings. Route either
	 * uncommon cross-page element through the validated TLB-copy helper.
	 */
	and x9, x19, #0xfff
	add x9, x9, x5
	cmp x9, #4096
	b.hi pair_cross_page
	adds x20, x19, x5
	b.cs pair_cross_page
	and x9, x20, #0xfff
	add x9, x9, x5
	cmp x9, #4096
	b.hi pair_cross_page
	/* Consume the instruction, leaving the guest PC for a fault. */
	add _stream, _stream, #8
	cbz w26, pair_translate_store

	mov x7, x19
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	mov x19, x7
	mov x7, x20
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	mov x20, x7
	cmp w25, #2
	b.eq pair_load64
	ldr w6, [x19]
	ldr w7, [x20]
	b pair_store_loaded
pair_load64:
	ldr x6, [x19]
	ldr x7, [x20]
pair_store_loaded:
	cmp w25, #1
	b.ne 1f
	sxtw x6, w6
	sxtw x7, w7
1:
	add x8, _cpu, #UML_GADGET_CPU_X0
	cmp w22, #31
	b.eq pair_skip_first_load
	str x6, [x8, x22, lsl #3]
pair_skip_first_load:
	cmp w23, #31
	b.eq pair_writeback
	str x7, [x8, x23, lsl #3]
	b pair_writeback

pair_translate_store:
	mov x7, x19
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	mov x19, x7
	mov x7, x20
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	mov x20, x7
	add x8, _cpu, #UML_GADGET_CPU_X0
	cmp w22, #31
	b.eq pair_zero_first_store
	ldr x6, [x8, x22, lsl #3]
	b pair_first_store_ready
pair_zero_first_store:
	mov x6, xzr
pair_first_store_ready:
	cmp w23, #31
	b.eq pair_zero_second_store
	ldr x7, [x8, x23, lsl #3]
	b pair_second_store_ready
pair_zero_second_store:
	mov x7, xzr
pair_second_store_ready:
	cmp w25, #2
	b.eq pair_store64
	str w6, [x19]
	str w7, [x20]
	b pair_writeback
pair_store64:
	str x6, [x19]
	str x7, [x20]
	b pair_writeback

pair_cross_page:
	add _stream, _stream, #8
	cbnz w26, pair_cross_load
	add x8, _cpu, #UML_GADGET_CPU_X0
	cmp w22, #31
	b.eq pair_cross_zero_first
	ldr x6, [x8, x22, lsl #3]
	b pair_cross_first_ready
pair_cross_zero_first:
	mov x6, xzr
pair_cross_first_ready:
	cmp w23, #31
	b.eq pair_cross_zero_second
	ldr x7, [x8, x23, lsl #3]
	b pair_cross_second_ready
pair_cross_zero_second:
	mov x7, xzr
pair_cross_second_ready:
	cmp w25, #2
	b.eq pair_cross_store64
	fmov s0, w6
	mov v0.s[1], w7
	transfer_slow 1, x19, 8
	b pair_writeback
pair_cross_store64:
	fmov d0, x6
	mov v0.d[1], x7
	transfer_slow 1, x19, 16
	b pair_writeback
pair_cross_load:
	cmp w25, #2
	b.eq pair_cross_load64
	transfer_slow 0, x19, 8
	fmov w6, s0
	umov w7, v0.s[1]
	b pair_store_loaded
pair_cross_load64:
	transfer_slow 0, x19, 16
	fmov x6, d0
	umov x7, v0.d[1]
	b pair_store_loaded

pair_writeback:
	cmp w27, #1
	b.eq pair_do_writeback
	cmp w27, #3
	b.ne pair_done
pair_do_writeback:
	cmp w24, #31
	b.eq pair_write_sp
	add x8, _cpu, #UML_GADGET_CPU_X0
	str x21, [x8, x24, lsl #3]
	b pair_done
pair_write_sp:
	str x21, [_cpu, #UML_GADGET_CPU_SP]
pair_done:
	add _stream, _stream, #8
	next_gadget

/* SIMD&FP LDP/STP for S, D, and Q registers. Stream: [instruction][PC]. */
gadget load_store_vector_pair
	ldr x8, [_stream]
	and w22, w8, #0x1f
	ubfx w23, w8, #10, #5
	ubfx w24, w8, #5, #5
	ubfx w25, w8, #30, #2
	ubfx w26, w8, #22, #1
	ubfx w27, w8, #23, #2
	sbfx x6, x8, #15, #7
	add w5, w25, #2
	lsl x6, x6, x5
	cmp w24, #31
	b.eq vector_pair_load_sp
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x16, [x9, x24, lsl #3]
	b vector_pair_base_ready
vector_pair_load_sp:
	ldr x16, [_cpu, #UML_GADGET_CPU_SP]
vector_pair_base_ready:
	add x21, x16, x6
	cmp w27, #1
	csel x19, x16, x21, eq
	mov x5, #4
	lsl x5, x5, x25
	and x9, x19, #0xfff
	add x9, x9, x5
	cmp x9, #4096
	b.hi vector_pair_cross_page
	adds x20, x19, x5
	b.cs vector_pair_cross_page
	and x9, x20, #0xfff
	add x9, x9, x5
	cmp x9, #4096
	b.hi vector_pair_cross_page
	add _stream, _stream, #8
	cbz w26, vector_pair_translate_store
	mov x7, x19
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	mov x19, x7
	mov x7, x20
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	mov x20, x7
	movi v0.16b, #0
	movi v1.16b, #0
	cmp w25, #0
	b.eq vector_pair_load32
	cmp w25, #1
	b.eq vector_pair_load64
	ldr q0, [x19]
	ldr q1, [x20]
	b vector_pair_store_loaded
vector_pair_load32:
	ldr s0, [x19]
	ldr s1, [x20]
	b vector_pair_store_loaded
vector_pair_load64:
	ldr d0, [x19]
	ldr d1, [x20]
vector_pair_store_loaded:
	add x9, _cpu, #UML_GADGET_CPU_V0
	add x10, x9, x22, lsl #4
	add x11, x9, x23, lsl #4
	str q0, [x10]
	str q1, [x11]
	b vector_pair_writeback
vector_pair_translate_store:
	mov x7, x19
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	mov x19, x7
	mov x7, x20
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	mov x20, x7
	add x9, _cpu, #UML_GADGET_CPU_V0
	add x10, x9, x22, lsl #4
	add x11, x9, x23, lsl #4
	ldr q0, [x10]
	ldr q1, [x11]
	cmp w25, #0
	b.eq vector_pair_store32
	cmp w25, #1
	b.eq vector_pair_store64
	str q0, [x19]
	str q1, [x20]
	b vector_pair_writeback
vector_pair_store32:
	str s0, [x19]
	str s1, [x20]
	b vector_pair_writeback
vector_pair_store64:
	str d0, [x19]
	str d1, [x20]
	b vector_pair_writeback
vector_pair_cross_page:
	add _stream, _stream, #8
	cbnz w26, vector_pair_cross_load
	add x9, _cpu, #UML_GADGET_CPU_V0
	add x10, x9, x22, lsl #4
	add x11, x9, x23, lsl #4
	ldr q0, [x10]
	ldr q1, [x11]
	cmp w25, #0
	b.eq vector_pair_cross_store32
	cmp w25, #1
	b.eq vector_pair_cross_store64
	transfer_slow 1, x19, 32
	b vector_pair_writeback
vector_pair_cross_store32:
	mov v0.s[1], v1.s[0]
	transfer_slow 1, x19, 8
	b vector_pair_writeback
vector_pair_cross_store64:
	mov v0.d[1], v1.d[0]
	transfer_slow 1, x19, 16
	b vector_pair_writeback
vector_pair_cross_load:
	cmp w25, #0
	b.eq vector_pair_cross_load32
	cmp w25, #1
	b.eq vector_pair_cross_load64
	transfer_slow 0, x19, 32
	b vector_pair_store_loaded
vector_pair_cross_load32:
	transfer_slow 0, x19, 8
	movi v1.16b, #0
	mov v1.s[0], v0.s[1]
	movi v2.16b, #0
	mov v2.s[0], v0.s[0]
	mov v0.16b, v2.16b
	b vector_pair_store_loaded
vector_pair_cross_load64:
	transfer_slow 0, x19, 16
	movi v1.16b, #0
	mov v1.d[0], v0.d[1]
	movi v2.16b, #0
	mov v2.d[0], v0.d[0]
	mov v0.16b, v2.16b
	b vector_pair_store_loaded
vector_pair_writeback:
	cmp w27, #1
	b.eq vector_pair_do_writeback
	cmp w27, #3
	b.ne vector_pair_done
vector_pair_do_writeback:
	cmp w24, #31
	b.eq vector_pair_write_sp
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x21, [x9, x24, lsl #3]
	b vector_pair_done
vector_pair_write_sp:
	str x21, [_cpu, #UML_GADGET_CPU_SP]
vector_pair_done:
	add _stream, _stream, #8
	next_gadget

uml_arm64_gadget_memory_fault:
	ldr x8, [_stream]
	str x8, [_cpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_MEMORY_FAULT
	b uml_arm64_gadget_leave

