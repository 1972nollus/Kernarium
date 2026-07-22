gadget pc_relative
	ldr x8, [_stream], #8
	ldr x9, [_stream], #8
	cmp x8, #31
	b.eq 1f
	add x10, _cpu, #UML_GADGET_CPU_X0
	str x9, [x10, x8, lsl #3]
1:	next_gadget

gadget tls_read
	ldr x8, [_stream], #8
	cmp x8, #31
	b.eq 1f
	ldr x9, [_cpu, #UML_GADGET_CPU_TLS]
	add x10, _cpu, #UML_GADGET_CPU_X0
	str x9, [x10, x8, lsl #3]
1:	next_gadget

gadget tls_write
	ldr x8, [_stream], #8
	cmp x8, #31
	b.eq 1f
	add x10, _cpu, #UML_GADGET_CPU_X0
	ldr x9, [x10, x8, lsl #3]
	str x9, [_cpu, #UML_GADGET_CPU_TLS]
	b 2f
1:	str xzr, [_cpu, #UML_GADGET_CPU_TLS]
2:	next_gadget

/* Userspace-visible system registers. The less common stateful forms share a
 * compact dispatcher; all ordinary execution remains in the gadget engine. */
gadget system_register
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	and w10, w8, #0xffffffe0
	mov w11, #0xd040
	movk w11, #0xd53b, lsl #16
	cmp w10, w11
	b.eq system_tls_read
	movk w11, #0xd51b, lsl #16
	cmp w10, w11
	b.eq system_tls_write
	mov w11, #0x00e0
	movk w11, #0xd53b, lsl #16
	cmp w10, w11
	b.eq system_dczid_read
	mov w11, #0x0020
	movk w11, #0xd53b, lsl #16
	cmp w10, w11
	b.eq system_ctr_read
	mov w11, #0x4400
	movk w11, #0xd53b, lsl #16
	cmp w10, w11
	b.eq system_fpcr_read
	movk w11, #0xd51b, lsl #16
	cmp w10, w11
	b.eq system_fpcr_write
	mov w11, #0x4420
	movk w11, #0xd53b, lsl #16
	cmp w10, w11
	b.eq system_fpsr_read
	movk w11, #0xd51b, lsl #16
	cmp w10, w11
	b.eq system_fpsr_write
	mov w11, #0x4200
	movk w11, #0xd53b, lsl #16
	cmp w10, w11
	b.eq system_nzcv_read
	movk w11, #0xd51b, lsl #16
	cmp w10, w11
	b.eq system_nzcv_write
	mov w11, #0xe000
	movk w11, #0xd53b, lsl #16
	cmp w10, w11
	b.eq system_counter_frequency
	mov w11, #0xe040
	movk w11, #0xd53b, lsl #16
	cmp w10, w11
	b.eq system_counter_read
	b NAME(uml_arm64_gadget_illegal)
system_tls_read:
	ldr x12, [_cpu, #UML_GADGET_CPU_TLS]
	b system_register_store
system_tls_write:
	bl system_register_load
	str x12, [_cpu, #UML_GADGET_CPU_TLS]
	next_gadget
system_dczid_read:
	mov x12, #0x10
	b system_register_store
system_ctr_read:
	mov w12, #0x8004
	movk w12, #0x8444, lsl #16
	b system_register_store
system_fpcr_read:
	ldr w12, [_cpu, #UML_GADGET_CPU_FPCR]
	b system_register_store
system_fpsr_read:
	ldr x11, [sp, #0x78]
	cbz x11, 3f
	mrs x12, fpsr
	str w12, [_cpu, #UML_GADGET_CPU_FPSR]
	b system_register_store
3:	ldr w12, [_cpu, #UML_GADGET_CPU_FPSR]
	b system_register_store
system_nzcv_read:
	ldr w12, [_cpu, #UML_GADGET_CPU_NZCV]
	b system_register_store
system_counter_frequency:
	mov w12, #0xca00
	movk w12, #0x3b9a, lsl #16
	b system_register_store
system_counter_read:
	stp x1, x2, [sp, #-16]!
	bl NAME(uml_arm64_gadget_counter_read)
	mov x12, x0
	ldp x1, x2, [sp], #16
	b system_register_store
system_fpcr_write:
	bl system_register_load
	str w12, [_cpu, #UML_GADGET_CPU_FPCR]
	ldr x11, [sp, #0x78]
	cbz x11, 4f
	msr fpcr, x12
	str w12, [sp, #0x80]
4:	next_gadget
system_fpsr_write:
	bl system_register_load
	str w12, [_cpu, #UML_GADGET_CPU_FPSR]
	ldr x11, [sp, #0x78]
	cbz x11, 5f
	msr fpsr, x12
5:	next_gadget
system_nzcv_write:
	bl system_register_load
	and w12, w12, #0xf0000000
	str w12, [_cpu, #UML_GADGET_CPU_NZCV]
	next_gadget
system_register_load:
	add x11, _cpu, #UML_GADGET_CPU_X0
	cmp w9, #31
	b.eq 6f
	ldr x12, [x11, x9, lsl #3]
	ret
6:	mov x12, xzr
	ret
system_register_store:
	cmp w9, #31
	b.eq 7f
	add x11, _cpu, #UML_GADGET_CPU_X0
	str x12, [x11, x9, lsl #3]
7:	next_gadget

gadget memory_barrier
	ldr x8, [_stream], #8
	ubfx w8, w8, #5, #2
	cmp w8, #1
	b.eq 1f
	cmp w8, #2
	b.eq 2f
	dsb ish
	b 3f
1:	dmb ish
	b 3f
2:	isb
3:	next_gadget

/* AdvSIMD DUP (general), ported from iSH's ARM64 gadget implementation. */
gadget dup_gpr_to_vector
	ldr x8, [_stream], #8
	and x9, x8, #0x1f
	ubfx x10, x8, #8, #5
	ubfx x11, x8, #16, #2
	ubfx x17, x8, #24, #1
	cmp x10, #31
	b.eq 1f
	add x12, _cpu, #UML_GADGET_CPU_X0
	ldr x13, [x12, x10, lsl #3]
	b 2f
1:	mov x13, xzr
2:	add x15, _cpu, #UML_GADGET_CPU_V0
	add x15, x15, x9, lsl #4
	cmp x11, #0
	b.eq 3f
	cmp x11, #1
	b.eq 4f
	cmp x11, #2
	b.eq 5f
	b 6f
3:	and x13, x13, #0xff
	mov x14, #0x0101010101010101
	mul x14, x13, x14
	b 7f
4:	and x13, x13, #0xffff
	mov x14, #0x0001000100010001
	mul x14, x13, x14
	b 7f
5:	and x13, x13, #0xffffffff
	orr x14, x13, x13, lsl #32
	b 7f
6:	mov x14, x13
7:	str x14, [x15]
	cbz x17, 8f
	str x14, [x15, #8]
	b 9f
8:	str xzr, [x15, #8]
9:	next_gadget

/* LD1/ST1 of one to four consecutive SIMD registers. These structure forms
 * are the hot vector-copy primitives emitted by libc. Stream: instruction,
 * guest PC. Cross-page transfers use the validated TLB copy helper. */
gadget load_store_simd_structure
	ldr x19, [_stream], #8
	mov w25, wzr
	ubfx w22, w19, #12, #4
	mov w21, #1
	cmp w22, #7
	b.eq 1f
	mov w21, #2
	cmp w22, #10
	b.eq 1f
	mov w21, #3
	cmp w22, #6
	b.eq 1f
	mov w21, #4
1:	ubfx w23, w19, #30, #1
	lsl x22, x21, #3
	cbz w23, 2f
	lsl x22, x22, #1
2:	ubfx w24, w19, #5, #5
	cmp w24, #31
	b.eq 3f
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x20, [x8, x24, lsl #3]
	b 4f
3:	ldr x20, [_cpu, #UML_GADGET_CPU_SP]
4:	and x8, x20, #0xfff
	add x8, x8, x22
	cmp x8, #4096
	b.hi simd_structure_slow
	mov x7, x20
	tbnz x19, #22, simd_structure_translate_read
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	b simd_structure_transfer
simd_structure_translate_read:
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	b simd_structure_transfer

simd_structure_slow:
	sub sp, sp, #80
	stp x1, x2, [sp, #64]
	tbz x19, #22, simd_structure_slow_store_prepare
	mov x0, x2
	mov x1, x20
	mov x2, sp
	mov x3, x22
	mov w4, #0
	bl NAME(uml_arm64_gadget_memory_transfer)
	ldp x1, x2, [sp, #64]
	cbz w0, simd_structure_slow_fault
	mov x7, sp
	mov w25, #1
	b simd_structure_transfer
simd_structure_slow_store_prepare:
	mov x7, sp
	mov w25, #2

simd_structure_transfer:
	and w26, w19, #0x1f
	tbz x19, #22, simd_structure_store
	cbz w23, simd_structure_load_d
	ldr q0, [x7]
	cmp w21, #1
	b.eq simd_structure_load_registers
	ldr q1, [x7, #16]
	cmp w21, #2
	b.eq simd_structure_load_registers
	ldr q2, [x7, #32]
	cmp w21, #3
	b.eq simd_structure_load_registers
	ldr q3, [x7, #48]
	b simd_structure_load_registers
simd_structure_load_d:
	ldr d0, [x7]
	cmp w21, #1
	b.eq simd_structure_load_registers
	ldr d1, [x7, #8]
	cmp w21, #2
	b.eq simd_structure_load_registers
	ldr d2, [x7, #16]
	cmp w21, #3
	b.eq simd_structure_load_registers
	ldr d3, [x7, #24]
simd_structure_load_registers:
	add x8, _cpu, #UML_GADGET_CPU_V0
	and w9, w26, #31
	add x10, x8, x9, lsl #4
	cbz w23, 5f
	str q0, [x10]
	b 6f
5:	str d0, [x10]
	str xzr, [x10, #8]
6:	cmp w21, #1
	b.eq simd_structure_transfer_done
	add w26, w26, #1
	and w9, w26, #31
	add x10, x8, x9, lsl #4
	cbz w23, 7f
	str q1, [x10]
	b 8f
7:	str d1, [x10]
	str xzr, [x10, #8]
8:	cmp w21, #2
	b.eq simd_structure_transfer_done
	add w26, w26, #1
	and w9, w26, #31
	add x10, x8, x9, lsl #4
	cbz w23, 9f
	str q2, [x10]
	b 10f
9:	str d2, [x10]
	str xzr, [x10, #8]
10:	cmp w21, #3
	b.eq simd_structure_transfer_done
	add w26, w26, #1
	and w9, w26, #31
	add x10, x8, x9, lsl #4
	cbz w23, 11f
	str q3, [x10]
	b simd_structure_transfer_done
11:	str d3, [x10]
	str xzr, [x10, #8]
	b simd_structure_transfer_done

simd_structure_store:
	add x8, _cpu, #UML_GADGET_CPU_V0
	and w9, w26, #31
	add x10, x8, x9, lsl #4
	ldr q0, [x10]
	cmp w21, #1
	b.eq simd_structure_store_memory
	add w26, w26, #1
	and w9, w26, #31
	add x10, x8, x9, lsl #4
	ldr q1, [x10]
	cmp w21, #2
	b.eq simd_structure_store_memory
	add w26, w26, #1
	and w9, w26, #31
	add x10, x8, x9, lsl #4
	ldr q2, [x10]
	cmp w21, #3
	b.eq simd_structure_store_memory
	add w26, w26, #1
	and w9, w26, #31
	add x10, x8, x9, lsl #4
	ldr q3, [x10]
simd_structure_store_memory:
	cbz w23, simd_structure_store_d
	str q0, [x7]
	cmp w21, #1
	b.eq simd_structure_store_complete
	str q1, [x7, #16]
	cmp w21, #2
	b.eq simd_structure_store_complete
	str q2, [x7, #32]
	cmp w21, #3
	b.eq simd_structure_store_complete
	str q3, [x7, #48]
	b simd_structure_store_complete
simd_structure_store_d:
	str d0, [x7]
	cmp w21, #1
	b.eq simd_structure_store_complete
	str d1, [x7, #8]
	cmp w21, #2
	b.eq simd_structure_store_complete
	str d2, [x7, #16]
	cmp w21, #3
	b.eq simd_structure_store_complete
	str d3, [x7, #24]
simd_structure_store_complete:
	cmp w25, #2
	b.ne simd_structure_transfer_done
	mov x0, x2
	mov x1, x20
	mov x2, sp
	mov x3, x22
	mov w4, #1
	bl NAME(uml_arm64_gadget_memory_transfer)
	ldp x1, x2, [sp, #64]
	cbz w0, simd_structure_slow_fault

simd_structure_transfer_done:
	cbz w25, simd_structure_writeback
	add sp, sp, #80
simd_structure_writeback:
	tbz x19, #23, simd_structure_done
	ubfx w8, w19, #16, #5
	cmp w8, #31
	b.eq 12f
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x8, [x9, x8, lsl #3]
	b 13f
12:	mov x8, x22
13:	add x20, x20, x8
	cmp w24, #31
	b.eq 14f
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x20, [x9, x24, lsl #3]
	b simd_structure_done
14:	str x20, [_cpu, #UML_GADGET_CPU_SP]
simd_structure_done:
	add _stream, _stream, #8
	next_gadget
simd_structure_slow_fault:
	add sp, sp, #80
	b uml_arm64_gadget_memory_fault

/* LD1R/LD2R/LD3R/LD4R loads one element per destination and uses the host
 * DUP instruction for replication. Stream: instruction, guest PC. */
gadget load_simd_replicate
	ldr x19, [_stream], #8
	ubfx w20, w19, #5, #5
	cmp w20, #31
	b.eq 1f
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x20, [x8, x20, lsl #3]
	b 2f
1:	ldr x20, [_cpu, #UML_GADGET_CPU_SP]
2:	ubfx w21, w19, #13, #3
	ubfx w22, w19, #21, #1
	cmp w21, #6
	add w22, w22, #1
	b.eq 12f
	add w22, w22, #2
12:
	ubfx w23, w19, #10, #2
	mov x24, #1
	lsl x24, x24, x23
	mul x25, x24, x22
	and x8, x20, #0xfff
	add x8, x8, x25
	cmp x8, #4096
	b.hi simd_replicate_slow
	mov x7, x20
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	mov w26, wzr
	b simd_replicate_transfer

simd_replicate_slow:
	sub sp, sp, #48
	stp x1, x2, [sp, #32]
	mov x0, x2
	mov x1, x20
	mov x2, sp
	mov x3, x25
	mov w4, #0
	bl NAME(uml_arm64_gadget_memory_transfer)
	ldp x1, x2, [sp, #32]
	cbz w0, simd_replicate_slow_fault
	mov x7, sp
	mov w26, #1

simd_replicate_transfer:
	and w27, w19, #0x1f
	add x8, _cpu, #UML_GADGET_CPU_V0
simd_replicate_loop:
	cmp w23, #0
	b.eq 3f
	cmp w23, #1
	b.eq 4f
	cmp w23, #2
	b.eq 5f
	ldr x9, [x7]
	dup v0.2d, x9
	b 6f
3:	ldrb w9, [x7]
	dup v0.16b, w9
	b 6f
4:	ldrh w9, [x7]
	dup v0.8h, w9
	b 6f
5:	ldr w9, [x7]
	dup v0.4s, w9
6:	and w9, w27, #31
	add x10, x8, x9, lsl #4
	tbnz x19, #30, 7f
	str d0, [x10]
	str xzr, [x10, #8]
	b 8f
7:	str q0, [x10]
8:	add w27, w27, #1
	add x7, x7, x24
	subs w22, w22, #1
	b.ne simd_replicate_loop
	cbz w26, simd_replicate_writeback
	add sp, sp, #48
simd_replicate_writeback:
	tbz x19, #23, simd_replicate_done
	ubfx w8, w19, #16, #5
	cmp w8, #31
	b.eq 9f
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x8, [x9, x8, lsl #3]
	b 10f
9:	mov x8, x25
10:	add x20, x20, x8
	ubfx w8, w19, #5, #5
	cmp w8, #31
	b.eq 11f
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x20, [x9, x8, lsl #3]
	b simd_replicate_done
11:	str x20, [_cpu, #UML_GADGET_CPU_SP]
simd_replicate_done:
	add _stream, _stream, #8
	next_gadget
simd_replicate_slow_fault:
	add sp, sp, #48
	b uml_arm64_gadget_memory_fault

/* LD/ST single structure lane forms. Dynamic lane selection is handled in
 * CpuState memory while the actual guest-memory transfer stays on the TLB
 * fast path. Stream: instruction, guest PC. */
gadget load_store_simd_lane
	ldr x19, [_stream], #8
	ubfx w8, w19, #5, #5
	cmp w8, #31
	b.eq 1f
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x20, [x9, x8, lsl #3]
	b 2f
1:	ldr x20, [_cpu, #UML_GADGET_CPU_SP]
2:	ubfx w8, w19, #13, #3
	ubfx w9, w19, #21, #1
	tbnz w8, #0, 3f
	add w21, w9, #1
	b 4f
3:	add w21, w9, #3
4:	and w10, w8, #6
	ubfx w11, w19, #10, #2
	ubfx w12, w19, #12, #1
	ubfx w13, w19, #30, #1
	cmp w10, #0
	b.eq 5f
	cmp w10, #2
	b.eq 6f
	mov x22, #4
	lsl x22, x22, x11
	mov w14, #1
	sub w14, w14, w11
	lsl w13, w13, w14
	orr w13, w13, w12
	b 7f
5:	mov x22, #1
	lsl w13, w13, #3
	orr w13, w13, w12, lsl #2
	orr w13, w13, w11
	b 7f
6:	mov x22, #2
	lsl w13, w13, #2
	orr w13, w13, w12, lsl #1
	and w11, w11, #1
	orr w13, w13, w11
7:	mul x23, x13, x22
	mul x24, x21, x22
	and x8, x20, #0xfff
	add x8, x8, x24
	cmp x8, #4096
	b.hi simd_lane_slow
	mov x7, x20
	tbnz x19, #22, 8f
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	mov w26, wzr
	b simd_lane_transfer
8:	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	mov w26, wzr
	b simd_lane_transfer

simd_lane_slow:
	sub sp, sp, #48
	stp x1, x2, [sp, #32]
	tbz x19, #22, 9f
	mov x0, x2
	mov x1, x20
	mov x2, sp
	mov x3, x24
	mov w4, #0
	bl NAME(uml_arm64_gadget_memory_transfer)
	ldp x1, x2, [sp, #32]
	cbz w0, simd_lane_slow_fault
	mov x7, sp
	mov w26, #1
	b simd_lane_transfer
9:	mov x7, sp
	mov w26, #2

simd_lane_transfer:
	and w27, w19, #0x1f
	add x8, _cpu, #UML_GADGET_CPU_V0
simd_lane_loop:
	and w9, w27, #31
	add x10, x8, x9, lsl #4
	add x10, x10, x23
	tbz x19, #22, simd_lane_store
	cmp x22, #1
	b.eq 10f
	cmp x22, #2
	b.eq 11f
	cmp x22, #4
	b.eq 12f
	ldr x9, [x7]
	str x9, [x10]
	b simd_lane_next
10:	ldrb w9, [x7]
	strb w9, [x10]
	b simd_lane_next
11:	ldrh w9, [x7]
	strh w9, [x10]
	b simd_lane_next
12:	ldr w9, [x7]
	str w9, [x10]
	b simd_lane_next
simd_lane_store:
	cmp x22, #1
	b.eq 13f
	cmp x22, #2
	b.eq 14f
	cmp x22, #4
	b.eq 15f
	ldr x9, [x10]
	str x9, [x7]
	b simd_lane_next
13:	ldrb w9, [x10]
	strb w9, [x7]
	b simd_lane_next
14:	ldrh w9, [x10]
	strh w9, [x7]
	b simd_lane_next
15:	ldr w9, [x10]
	str w9, [x7]
simd_lane_next:
	add w27, w27, #1
	add x7, x7, x22
	subs w21, w21, #1
	b.ne simd_lane_loop
	cmp w26, #2
	b.ne 16f
	mov x0, x2
	mov x1, x20
	mov x2, sp
	mov x3, x24
	mov w4, #1
	bl NAME(uml_arm64_gadget_memory_transfer)
	ldp x1, x2, [sp, #32]
	cbz w0, simd_lane_slow_fault
16:	cbz w26, simd_lane_writeback
	add sp, sp, #48
simd_lane_writeback:
	tbz x19, #23, simd_lane_done
	ubfx w8, w19, #16, #5
	cmp w8, #31
	b.eq 17f
	add x9, _cpu, #UML_GADGET_CPU_X0
	ldr x8, [x9, x8, lsl #3]
	b 18f
17:	mov x8, x24
18:	add x20, x20, x8
	ubfx w8, w19, #5, #5
	cmp w8, #31
	b.eq 19f
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x20, [x9, x8, lsl #3]
	b simd_lane_done
19:	str x20, [_cpu, #UML_GADGET_CPU_SP]
simd_lane_done:
	add _stream, _stream, #8
	next_gadget
simd_lane_slow_fault:
	add sp, sp, #48
	b uml_arm64_gadget_memory_fault

/* Scalar LDAR/STLR (and their byte/halfword forms) execute as host atomics on
 * the translated shared mapping. Stream: instruction, guest PC. */
gadget load_store_ordered
	ldr x19, [_stream], #8
	ubfx w20, w19, #5, #5
	and w21, w19, #0x1f
	cmp w20, #31
	b.eq 1f
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x8, x20, lsl #3]
	b 2f
1:	ldr x7, [_cpu, #UML_GADGET_CPU_SP]
2:	tbnz x19, #22, load_store_ordered_read
	/* Translate first: a TLB miss calls into C and may clobber caller-saved
	 * registers, including the value register used by the native store. */
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	cmp w21, #31
	b.eq 3f
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x15, [x8, x21, lsl #3]
	b 4f
3:	mov x15, xzr
4:
	ubfx w8, w19, #30, #2
	tbz x19, #15, load_store_relaxed_write
	cmp w8, #0
	b.eq 5f
	cmp w8, #1
	b.eq 6f
	cmp w8, #2
	b.eq 7f
	stlr x15, [x7]
	b load_store_ordered_done
5:	stlrb w15, [x7]
	b load_store_ordered_done
6:	stlrh w15, [x7]
	b load_store_ordered_done
7:	stlr w15, [x7]
	b load_store_ordered_done
load_store_relaxed_write:
	cmp w8, #0
	b.eq 8f
	cmp w8, #1
	b.eq 9f
	cmp w8, #2
	b.eq 10f
	str x15, [x7]
	b load_store_ordered_done
8:	strb w15, [x7]
	b load_store_ordered_done
9:	strh w15, [x7]
	b load_store_ordered_done
10:	str w15, [x7]
	b load_store_ordered_done
load_store_ordered_read:
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	ubfx w8, w19, #30, #2
	tbz x19, #15, load_store_relaxed_read
	cmp w8, #0
	b.eq 11f
	cmp w8, #1
	b.eq 12f
	cmp w8, #2
	b.eq 13f
	ldar x15, [x7]
	b load_store_ordered_store_result
11:	ldarb w15, [x7]
	b load_store_ordered_store_result
12:	ldarh w15, [x7]
	b load_store_ordered_store_result
13:	ldar w15, [x7]
	b load_store_ordered_store_result
load_store_relaxed_read:
	cmp w8, #0
	b.eq 14f
	cmp w8, #1
	b.eq 15f
	cmp w8, #2
	b.eq 16f
	ldr x15, [x7]
	b load_store_ordered_store_result
14:	ldrb w15, [x7]
	b load_store_ordered_store_result
15:	ldrh w15, [x7]
	b load_store_ordered_store_result
16:	ldr w15, [x7]
load_store_ordered_store_result:
	cmp w21, #31
	b.eq load_store_ordered_done
	add x8, _cpu, #UML_GADGET_CPU_X0
	str x15, [x8, x21, lsl #3]
load_store_ordered_done:
	add _stream, _stream, #8
	next_gadget

/* Scalar LDXR/STXR and acquire/release variants use the host monitor on the
 * translated mapping. Stream: instruction, guest PC. */
gadget load_store_exclusive
	ldr x19, [_stream], #8
	ubfx w20, w19, #5, #5
	and w21, w19, #0x1f
	cmp w20, #31
	b.eq 1f
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x7, [x8, x20, lsl #3]
	b 2f
1:	ldr x7, [_cpu, #UML_GADGET_CPU_SP]
2:	tbnz x19, #22, load_store_exclusive_read
	/* Preserve the guest store operand across a cold translation by loading it
	 * only after the miss helper has returned. */
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	cmp w21, #31
	b.eq 3f
	add x8, _cpu, #UML_GADGET_CPU_X0
	ldr x15, [x8, x21, lsl #3]
	b 4f
3:	mov x15, xzr
4:
	ubfx w8, w19, #30, #2
	tbz x19, #15, load_store_exclusive_relaxed_write
	cmp w8, #0
	b.eq 5f
	cmp w8, #1
	b.eq 6f
	cmp w8, #2
	b.eq 7f
	stlxr w14, x15, [x7]
	b load_store_exclusive_status
5:	stlxrb w14, w15, [x7]
	b load_store_exclusive_status
6:	stlxrh w14, w15, [x7]
	b load_store_exclusive_status
7:	stlxr w14, w15, [x7]
	b load_store_exclusive_status
load_store_exclusive_relaxed_write:
	cmp w8, #0
	b.eq 8f
	cmp w8, #1
	b.eq 9f
	cmp w8, #2
	b.eq 10f
	stxr w14, x15, [x7]
	b load_store_exclusive_status
8:	stxrb w14, w15, [x7]
	b load_store_exclusive_status
9:	stxrh w14, w15, [x7]
	b load_store_exclusive_status
10:	stxr w14, w15, [x7]
load_store_exclusive_status:
	ubfx w8, w19, #16, #5
	cmp w8, #31
	b.eq load_store_exclusive_done
	add x9, _cpu, #UML_GADGET_CPU_X0
	str x14, [x9, x8, lsl #3]
	b load_store_exclusive_done
load_store_exclusive_read:
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	ubfx w8, w19, #30, #2
	tbz x19, #15, load_store_exclusive_relaxed_read
	cmp w8, #0
	b.eq 11f
	cmp w8, #1
	b.eq 12f
	cmp w8, #2
	b.eq 13f
	ldaxr x15, [x7]
	b load_store_exclusive_result
11:	ldaxrb w15, [x7]
	b load_store_exclusive_result
12:	ldaxrh w15, [x7]
	b load_store_exclusive_result
13:	ldaxr w15, [x7]
	b load_store_exclusive_result
load_store_exclusive_relaxed_read:
	cmp w8, #0
	b.eq 14f
	cmp w8, #1
	b.eq 15f
	cmp w8, #2
	b.eq 16f
	ldxr x15, [x7]
	b load_store_exclusive_result
14:	ldxrb w15, [x7]
	b load_store_exclusive_result
15:	ldxrh w15, [x7]
	b load_store_exclusive_result
16:	ldxr w15, [x7]
load_store_exclusive_result:
	cmp w21, #31
	b.eq load_store_exclusive_done
	add x8, _cpu, #UML_GADGET_CPU_X0
	str x15, [x8, x21, lsl #3]
load_store_exclusive_done:
	add _stream, _stream, #8
	next_gadget
