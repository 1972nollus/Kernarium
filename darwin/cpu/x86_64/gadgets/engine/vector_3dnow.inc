/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AMD 3DNow! MMX-register operations. */

.macro vector_3dnow_float_to_i32
	frintz v6.2s, v1.2s
	mov w14, #0x4f000000
	dup v16.2s, w14
	fcmgt v7.2s, v16.2s, v6.2s
	mov w14, #0xcf000000
	dup v17.2s, w14
	fcmge v17.2s, v6.2s, v17.2s
	and v7.8b, v7.8b, v17.8b
	fcvtzs v16.2s, v6.2s
	mov w14, #0x80000000
	dup v17.2s, w14
	bsl v7.8b, v16.8b, v17.8b
	mov v16.8b, v7.8b
.endm

.macro vector_3dnow_body opcode
	.if \opcode == 0x0c
	smov w14, v1.h[0]
	smov w15, v1.h[2]
	scvtf s4, w14
	scvtf s6, w15
	mov v4.s[1], v6.s[0]
	.elseif \opcode == 0x0d
	scvtf v4.2s, v1.2s
	.elseif \opcode == 0x1c
	vector_3dnow_float_to_i32
	smov w14, v16.h[0]
	smov w15, v16.h[2]
	mov v4.s[0], w14
	mov v4.s[1], w15
	.elseif \opcode == 0x1d
	vector_3dnow_float_to_i32
	mov v4.8b, v16.8b
	.elseif \opcode == 0x86 || \opcode == 0x96
	fmov v6.2s, #1.0
	fdiv v4.2s, v6.2s, v1.2s
	.if \opcode == 0x96
	dup v4.2s, v4.s[0]
	.endif
	.elseif \opcode == 0x87 || \opcode == 0x97
	fabs v6.2s, v1.2s
	fsqrt v6.2s, v6.2s
	fmov v7.2s, #1.0
	fdiv v4.2s, v7.2s, v6.2s
	mov w14, #0x80000000
	dup v7.2s, w14
	and v7.8b, v7.8b, v1.8b
	orr v4.8b, v4.8b, v7.8b
	.if \opcode == 0x97
	dup v4.2s, v4.s[0]
	.endif
	.elseif \opcode == 0xa6 || \opcode == 0xa7 || \opcode == 0xb6
	mov v4.8b, v1.8b
	.elseif \opcode == 0xbb
	rev64 v4.2s, v1.2s
	.elseif \opcode == 0xbf
	urhadd v4.8b, v0.8b, v1.8b
	.elseif \opcode == 0xb7
	smull v6.4s, v0.4h, v1.4h
	movi v7.4s, #0x80, lsl #8
	add v6.4s, v6.4s, v7.4s
	sshr v6.4s, v6.4s, #16
	xtn v4.4h, v6.4s
	.elseif \opcode == 0x8a || \opcode == 0x8e || \opcode == 0xae
	dup v6.2s, v0.s[1]
	.if \opcode == 0xae
	fadd s4, s0, s6
	.else
	fsub s4, s0, s6
	.endif
	dup v6.2s, v1.s[1]
	.if \opcode == 0x8a
	fsub s6, s1, s6
	.else
	fadd s6, s1, s6
	.endif
	mov v4.s[1], v6.s[0]
	.elseif \opcode == 0x90
	fcmge v4.2s, v0.2s, v1.2s
	.elseif \opcode == 0xa0
	fcmgt v4.2s, v0.2s, v1.2s
	.elseif \opcode == 0xb0
	fcmeq v4.2s, v0.2s, v1.2s
	.elseif \opcode == 0x94
	fcmgt v4.2s, v1.2s, v0.2s
	bsl v4.8b, v0.8b, v1.8b
	.elseif \opcode == 0xa4
	fcmgt v4.2s, v0.2s, v1.2s
	bsl v4.8b, v0.8b, v1.8b
	.elseif \opcode == 0x9a
	fsub v4.2s, v0.2s, v1.2s
	.elseif \opcode == 0x9e
	fadd v4.2s, v0.2s, v1.2s
	.elseif \opcode == 0xaa
	fsub v4.2s, v1.2s, v0.2s
	.elseif \opcode == 0xb4
	fmul v4.2s, v0.2s, v1.2s
	.endif
.endm

/* 3DNow! has no exception flags, and MXCSR does not govern it: the body's
 * host floating-point instructions keep FPSR, the guest's MXCSR flags, as
 * they found it. Uses x12. */
.macro vector_3dnow_kept_status opcode
	mrs x12, fpsr
	vector_3dnow_body \opcode
	msr fpsr, x12
.endm

.macro vector_3dnow_register opcode
gadget vector_3dnow_\opcode\()_rr
	ldr x8, [_stream], #8
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	ldr d0, [_cpu, x10]
	ldr d1, [_cpu, x11]
	vector_3dnow_kept_status 0x\opcode
	vector_write 64, 8
	retire_and_next
.endm

.macro vector_3dnow_memory opcode
gadget vector_3dnow_\opcode\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	page_cross_check 8, Lx86_vector_3dnow_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, 8
	ldr d1, [x7]
	b Lx86_vector_3dnow_loaded\@
Lx86_vector_3dnow_cross\@:
	cross_page_load 8
	fmov d1, x0
Lx86_vector_3dnow_loaded\@:
	and x10, x26, #0xffff
	ldr d0, [_cpu, x10]
	vector_3dnow_kept_status 0x\opcode
	vector_write 64, 23
	retire_and_next
.endm

.macro vector_3dnow_family opcode
	vector_3dnow_register \opcode
	vector_3dnow_memory \opcode
.endm

vector_3dnow_family 0c
vector_3dnow_family 0d
vector_3dnow_family 1c
vector_3dnow_family 1d
vector_3dnow_family 86
vector_3dnow_family 87
vector_3dnow_family 8a
vector_3dnow_family 8e
vector_3dnow_family 90
vector_3dnow_family 94
vector_3dnow_family 96
vector_3dnow_family 97
vector_3dnow_family 9a
vector_3dnow_family 9e
vector_3dnow_family a0
vector_3dnow_family a4
vector_3dnow_family a6
vector_3dnow_family a7
vector_3dnow_family aa
vector_3dnow_family ae
vector_3dnow_family b0
vector_3dnow_family b4
vector_3dnow_family b6
vector_3dnow_family b7
vector_3dnow_family bb
vector_3dnow_family bf

.purgem vector_3dnow_family
.purgem vector_3dnow_memory
.purgem vector_3dnow_register
.purgem vector_3dnow_kept_status
.purgem vector_3dnow_body
.purgem vector_3dnow_float_to_i32
