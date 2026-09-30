/* SPDX-License-Identifier: GPL-2.0-or-later */
/* x87 scalar conversion, packed state, and control-word memory forms. */

.macro x87_load_converted name, size, helper, signed16=0, has_cpu=1
gadget x87_load_\name
	effective_address
	x87_scalar_load \size, x9
	sub sp, sp, #16
	.if \has_cpu
	mov x0, _cpu
		.if \size == 4
	mov w1, w9
		.else
	mov x1, x9
		.endif
	mov x2, sp
	.else
		.if \signed16
	sxth w0, w9
		.elseif \size == 4
	mov w0, w9
		.else
	mov x0, x9
		.endif
	mov x1, sp
	.endif
	bl NAME(uml_x86_64_x87_\helper)
	x87_load_value sp, x9, w10
	add sp, sp, #16
	x87_push x9, w10
	retire_and_next
.endm

x87_load_converted f32, 4, from_f32, 0, 1
x87_load_converted f64, 8, from_f64, 0, 1
x87_load_converted i16, 2, from_i32, 1, 0
x87_load_converted i32, 4, from_i32, 0, 0
x87_load_converted i64, 8, from_i64, 0, 0

gadget x87_load_f80
	effective_address
	mov x25, x7
	sub sp, sp, #16
	x87_bulk_load 10, sp, .Lx86_x87_load_f80_fault
	x87_load_value sp, x9, w10
	add sp, sp, #16
	x87_push x9, w10
	retire_and_next
.Lx86_x87_load_f80_fault:
	add sp, sp, #16
	b .Lx86_memory_fault

gadget x87_load_bcd
	effective_address
	mov x25, x7
	sub sp, sp, #32
	x87_bulk_load 10, sp, .Lx86_x87_load_bcd_fault
	mov x0, _cpu
	mov x1, sp
	add x2, sp, #16
	bl NAME(uml_x86_64_x87_from_bcd)
	add x11, sp, #16
	x87_load_value x11, x9, w10
	add sp, sp, #32
	x87_push x9, w10
	retire_and_next
.Lx86_x87_load_bcd_fault:
	add sp, sp, #32
	b .Lx86_memory_fault

.macro x87_store_float name, size, helper, pop
gadget x87_store_\name
	effective_address
	mov x25, x7
	sub sp, sp, #16
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	mov x0, _cpu
	mov x1, sp
	bl NAME(uml_x86_64_x87_to_\helper)
	mov x9, x0
	add sp, sp, #16
	x87_scalar_store \size, x9
	.if \pop
	x87_pop
	.endif
	finish_store \size
.endm

x87_store_float f32_keep, 4, f32, 0
x87_store_float f32_pop, 4, f32, 1
x87_store_float f64_keep, 8, f64, 0
x87_store_float f64_pop, 8, f64, 1

.macro x87_store_integer name, size, helper, truncate, pop
gadget x87_store_\name
	effective_address
	mov x25, x7
	sub sp, sp, #16
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	mov x0, _cpu
	mov x1, sp
	mov w2, #\truncate
	bl NAME(uml_x86_64_x87_to_\helper)
	mov x9, x0
	add sp, sp, #16
	x87_scalar_store \size, x9
	.if \pop
	x87_pop
	.endif
	finish_store \size
.endm

x87_store_integer i16_truncate_pop, 2, i16, 1, 1
x87_store_integer i16_keep, 2, i16, 0, 0
x87_store_integer i16_pop, 2, i16, 0, 1
x87_store_integer i32_truncate_pop, 4, i32, 1, 1
x87_store_integer i32_keep, 4, i32, 0, 0
x87_store_integer i32_pop, 4, i32, 0, 1
x87_store_integer i64_truncate_pop, 8, i64, 1, 1
x87_store_integer i64_pop, 8, i64, 0, 1

gadget x87_store_f80_pop
	effective_address
	mov x25, x7
	sub sp, sp, #16
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	x87_bulk_store 10, sp, .Lx86_x87_store_f80_fault
	add sp, sp, #16
	x87_pop
	finish_store 10
.Lx86_x87_store_f80_fault:
	add sp, sp, #16
	b .Lx86_memory_fault

gadget x87_store_bcd_pop
	effective_address
	mov x25, x7
	sub sp, sp, #32
	mov w13, #0
	x87_load_stack w13, x9, w10
	x87_store_value sp, x9, w10
	mov x0, _cpu
	mov x1, sp
	add x2, sp, #16
	bl NAME(uml_x86_64_x87_to_bcd)
	add x11, sp, #16
	x87_bulk_store 10, x11, .Lx86_x87_store_bcd_fault
	add sp, sp, #32
	x87_pop
	finish_store 10
.Lx86_x87_store_bcd_fault:
	add sp, sp, #32
	b .Lx86_memory_fault

gadget x87_load_control
	effective_address
	x87_scalar_load 2, x9
	strh w9, [_cpu, #UML_X86_64_CPU_X87_CONTROL]
	retire_unless_unmasked w9, 0x3f

gadget x87_store_control
	effective_address
	mov x25, x7
	ldrh w9, [_cpu, #UML_X86_64_CPU_X87_CONTROL]
	x87_scalar_store 2, x9
	finish_store 2

gadget x87_store_status
	effective_address
	mov x25, x7
	ldrh w9, [_cpu, #UML_X86_64_CPU_X87_STATUS]
	bic w9, w9, #0x3800
	ldrb w10, [_cpu, #UML_X86_64_CPU_X87_TOP]
	orr w9, w9, w10, lsl #11
	x87_scalar_store 2, x9
	finish_store 2

.macro x87_environment_restore name, environment_size, full
gadget x87_environment_restore_\name
	effective_address
	mov x25, x7
	.set .Lx86_x87_environment_total\@, (\environment_size + (\full * 80))
	.if .Lx86_x87_environment_total\@ <= 96
	.set .Lx86_x87_environment_stack\@, 96
	.else
	.set .Lx86_x87_environment_stack\@, 112
	.endif
	sub sp, sp, #.Lx86_x87_environment_stack\@
	x87_bulk_load .Lx86_x87_environment_total\@, sp, \
		.Lx86_x87_environment_restore_fault\@
	ldrh w9, [sp]
	strh w9, [_cpu, #UML_X86_64_CPU_X87_CONTROL]
	.if \environment_size == 28
	ldrh w9, [sp, #4]
	ldrh w10, [sp, #8]
	.else
	ldrh w9, [sp, #2]
	ldrh w10, [sp, #4]
	.endif
	ubfx w11, w9, #11, #3
	strb w11, [_cpu, #UML_X86_64_CPU_X87_TOP]
	bic w9, w9, #0x3800
	strh w9, [_cpu, #UML_X86_64_CPU_X87_STATUS]
	strh w10, [_cpu, #UML_X86_64_CPU_X87_TAG]
	.if \full
	add x9, sp, #\environment_size
	add x10, _cpu, #UML_X86_64_CPU_X87
	add x11, _cpu, #UML_X86_64_CPU_MMX
	mov w12, #8
.Lx86_x87_environment_restore_loop\@:
	ldr x13, [x9]
	ldrh w14, [x9, #8]
	str x13, [x10]
	strh w14, [x10, #8]
	str x13, [x11], #8
	add x9, x9, #10
	add x10, x10, #16
	subs w12, w12, #1
	b.ne .Lx86_x87_environment_restore_loop\@
	.endif
	add sp, sp, #.Lx86_x87_environment_stack\@
	ldrh w9, [_cpu, #UML_X86_64_CPU_X87_CONTROL]
	retire_unless_unmasked w9, 0x3f
.Lx86_x87_environment_restore_fault\@:
	add sp, sp, #.Lx86_x87_environment_stack\@
	b .Lx86_memory_fault
.endm

.macro x87_environment_save name, environment_size, full
gadget x87_environment_save_\name
	effective_address
	mov x25, x7
	.set .Lx86_x87_environment_total\@, (\environment_size + (\full * 80))
	.if .Lx86_x87_environment_total\@ <= 96
	.set .Lx86_x87_environment_stack\@, 96
	.else
	.set .Lx86_x87_environment_stack\@, 112
	.endif
	sub sp, sp, #.Lx86_x87_environment_stack\@
	movi v0.16b, #0
	mov x9, sp
	.set .Lx86_x87_environment_clear_count\@, \
		(.Lx86_x87_environment_stack\@ / 16)
	mov w10, #.Lx86_x87_environment_clear_count\@
.Lx86_x87_environment_clear_loop\@:
	str q0, [x9], #16
	subs w10, w10, #1
	b.ne .Lx86_x87_environment_clear_loop\@
	ldrh w9, [_cpu, #UML_X86_64_CPU_X87_CONTROL]
	strh w9, [sp]
	ldrh w9, [_cpu, #UML_X86_64_CPU_X87_STATUS]
	bic w9, w9, #0x3800
	ldrb w10, [_cpu, #UML_X86_64_CPU_X87_TOP]
	orr w9, w9, w10, lsl #11
	ldrh w10, [_cpu, #UML_X86_64_CPU_X87_TAG]
	.if \environment_size == 28
	strh w9, [sp, #4]
	strh w10, [sp, #8]
	.else
	strh w9, [sp, #2]
	strh w10, [sp, #4]
	.endif
	.if \full
	add x9, sp, #\environment_size
	add x10, _cpu, #UML_X86_64_CPU_X87
	mov w11, #8
.Lx86_x87_environment_save_loop\@:
	ldr x12, [x10]
	ldrh w13, [x10, #8]
	str x12, [x9]
	strh w13, [x9, #8]
	add x9, x9, #10
	add x10, x10, #16
	subs w11, w11, #1
	b.ne .Lx86_x87_environment_save_loop\@
	.endif
	x87_bulk_store .Lx86_x87_environment_total\@, sp, \
		.Lx86_x87_environment_save_fault\@
	add sp, sp, #.Lx86_x87_environment_stack\@
	.if \full
	x87_initialize_state
	.else
	ldrh w9, [_cpu, #UML_X86_64_CPU_X87_CONTROL]
	orr w9, w9, #0x3f
	strh w9, [_cpu, #UML_X86_64_CPU_X87_CONTROL]
	.endif
	finish_store .Lx86_x87_environment_total\@
.Lx86_x87_environment_save_fault\@:
	add sp, sp, #.Lx86_x87_environment_stack\@
	b .Lx86_memory_fault
.endm

x87_environment_restore env14, 14, 0
x87_environment_restore env28, 28, 0
x87_environment_restore state94, 14, 1
x87_environment_restore state108, 28, 1
x87_environment_save env14, 14, 0
x87_environment_save env28, 28, 0
x87_environment_save state94, 14, 1
x87_environment_save state108, 28, 1

.purgem x87_environment_save
.purgem x87_environment_restore
.purgem x87_store_integer
.purgem x87_store_float
.purgem x87_load_converted
