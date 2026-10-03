/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Packed floating-point unpack, shuffle, horizontal, and add/sub layouts. */

.macro vector_shuffle_ps_index lane, shift, right
	lsr w16, w13, #\shift
	and w16, w16, #3
	.if \right
	add w16, w16, #4
	.endif
	lsl w16, w16, #2
	mov w17, #0x0101
	movk w17, #0x0101, lsl #16
	mul w16, w16, w17
	mov w17, #0x0100
	movk w17, #0x0302, lsl #16
	add w16, w16, w17
	mov v6.s[\lane], w16
.endm

.macro vector_shuffle_body operation, element, width
	.ifc \operation,unpack_low
	.if \element == 32
	zip1 v4.4s, v0.4s, v1.4s
	.if \width == 256
	zip1 v5.4s, v2.4s, v3.4s
	.endif
	.else
	zip1 v4.2d, v0.2d, v1.2d
	.if \width == 256
	zip1 v5.2d, v2.2d, v3.2d
	.endif
	.endif
	.endif
	.ifc \operation,unpack_high
	.if \element == 32
	zip2 v4.4s, v0.4s, v1.4s
	.if \width == 256
	zip2 v5.4s, v2.4s, v3.4s
	.endif
	.else
	zip2 v4.2d, v0.2d, v1.2d
	.if \width == 256
	zip2 v5.2d, v2.2d, v3.2d
	.endif
	.endif
	.endif
	.ifc \operation,shuffle
	.if \element == 32
	vector_shuffle_ps_index 0, 0, 0
	vector_shuffle_ps_index 1, 2, 0
	vector_shuffle_ps_index 2, 4, 1
	vector_shuffle_ps_index 3, 6, 1
	tbl v4.16b, {v0.16b, v1.16b}, v6.16b
	.if \width == 256
	tbl v5.16b, {v2.16b, v3.16b}, v6.16b
	.endif
	.else
	tbz w13, #0, Lx86_vector_shuffle_pd_left_low\@
	mov v4.d[0], v0.d[1]
	b Lx86_vector_shuffle_pd_left_done\@
Lx86_vector_shuffle_pd_left_low\@:
	mov v4.d[0], v0.d[0]
Lx86_vector_shuffle_pd_left_done\@:
	tbz w13, #1, Lx86_vector_shuffle_pd_right_low\@
	mov v4.d[1], v1.d[1]
	b Lx86_vector_shuffle_pd_right_done\@
Lx86_vector_shuffle_pd_right_low\@:
	mov v4.d[1], v1.d[0]
Lx86_vector_shuffle_pd_right_done\@:
	.if \width == 256
	tbz w13, #2, Lx86_vector_shuffle_pd_upper_left_low\@
	mov v5.d[0], v2.d[1]
	b Lx86_vector_shuffle_pd_upper_left_done\@
Lx86_vector_shuffle_pd_upper_left_low\@:
	mov v5.d[0], v2.d[0]
Lx86_vector_shuffle_pd_upper_left_done\@:
	tbz w13, #3, Lx86_vector_shuffle_pd_upper_right_low\@
	mov v5.d[1], v3.d[1]
	b Lx86_vector_shuffle_pd_upper_right_done\@
Lx86_vector_shuffle_pd_upper_right_low\@:
	mov v5.d[1], v3.d[0]
Lx86_vector_shuffle_pd_upper_right_done\@:
	.endif
	.endif
	.endif
	.ifc \operation,hadd
	.if \element == 32
	faddp v4.4s, v0.4s, v1.4s
	.if \width == 256
	faddp v5.4s, v2.4s, v3.4s
	.endif
	.else
	faddp d4, v0.2d
	faddp d6, v1.2d
	mov v4.d[1], v6.d[0]
	.if \width == 256
	faddp d5, v2.2d
	faddp d6, v3.2d
	mov v5.d[1], v6.d[0]
	.endif
	.endif
	simd_check_result \element, \width
	.endif
	.ifc \operation,hsub
	.if \element == 32
	rev64 v6.4s, v0.4s
	fsub v6.4s, v0.4s, v6.4s
	rev64 v7.4s, v1.4s
	fsub v7.4s, v1.4s, v7.4s
	uzp1 v4.4s, v6.4s, v7.4s
	.if \width == 256
	rev64 v6.4s, v2.4s
	fsub v6.4s, v2.4s, v6.4s
	rev64 v7.4s, v3.4s
	fsub v7.4s, v3.4s, v7.4s
	uzp1 v5.4s, v6.4s, v7.4s
	.endif
	.else
	ext v6.16b, v0.16b, v0.16b, #8
	fsub d4, d0, d6
	ext v6.16b, v1.16b, v1.16b, #8
	fsub d6, d1, d6
	mov v4.d[1], v6.d[0]
	.if \width == 256
	ext v6.16b, v2.16b, v2.16b, #8
	fsub d5, d2, d6
	ext v6.16b, v3.16b, v3.16b, #8
	fsub d6, d3, d6
	mov v5.d[1], v6.d[0]
	.endif
	.endif
	simd_check_result \element, \width
	.endif
	.ifc \operation,addsub
	.if \element == 32
	fsub v4.4s, v0.4s, v1.4s
	fadd v6.4s, v0.4s, v1.4s
	mov v4.s[1], v6.s[1]
	mov v4.s[3], v6.s[3]
	.if \width == 256
	fsub v5.4s, v2.4s, v3.4s
	fadd v6.4s, v2.4s, v3.4s
	mov v5.s[1], v6.s[1]
	mov v5.s[3], v6.s[3]
	.endif
	.else
	fsub v4.2d, v0.2d, v1.2d
	fadd v6.2d, v0.2d, v1.2d
	mov v4.d[1], v6.d[1]
	.if \width == 256
	fsub v5.2d, v2.2d, v3.2d
	fadd v6.2d, v2.2d, v3.2d
	mov v5.d[1], v6.d[1]
	.endif
	.endif
	simd_check_result \element, \width
	.endif
.endm

.macro vector_shuffle_register operation, element, width, control
gadget vector_\operation\()_\element\()_\width\()_rr
	ldr x8, [_stream], #8
	.if \control
	ldr x13, [_stream], #8
	.endif
	vector_read_pair \width
	vector_shuffle_body \operation, \element, \width
	vector_write_xmm \width, 8
	retire_and_next
.endm

.macro vector_shuffle_memory operation, element, width, control
gadget vector_\operation\()_\element\()_\width\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	page_cross_check (\width / 8), Lx86_vector_shuffle_cross\@
	translate 0, UML_X86_64_TLB_READ_PAGE, (\width / 8)
	ldr q1, [x7]
	.if \width == 256
	ldr q3, [x7, #16]
	.endif
	b Lx86_vector_shuffle_loaded\@
Lx86_vector_shuffle_cross\@:
	sub sp, sp, #32
	mov x0, _context
	mov x1, x25
	mov x2, sp
	mov w3, #(\width / 8)
	bl NAME(uml_x86_64_gadget_cross_page_vector_load)
	cbz w0, Lx86_vector_shuffle_cross_fault\@
	ldr q1, [sp]
	.if \width == 256
	ldr q3, [sp, #16]
	.endif
	add sp, sp, #32
Lx86_vector_shuffle_loaded\@:
	and x10, x26, #0xffff
	add x10, _cpu, x10
	ldr q0, [x10]
	.if \width == 256
	ldr q2, [x10, #16]
	.endif
	.if \control
	ubfx x13, x26, #16, #8
	.endif
	vector_shuffle_body \operation, \element, \width
	vector_write_xmm \width, 23
	retire_and_next
Lx86_vector_shuffle_cross_fault\@:
	add sp, sp, #32
	b Lx86_memory_fault
.endm

.macro vector_shuffle_family operation, element, width, control=0
	vector_shuffle_register \operation, \element, \width, \control
	vector_shuffle_memory \operation, \element, \width, \control
.endm

vector_shuffle_family unpack_low, 32, 128
vector_shuffle_family unpack_low, 32, 256
vector_shuffle_family unpack_low, 64, 128
vector_shuffle_family unpack_low, 64, 256
vector_shuffle_family unpack_high, 32, 128
vector_shuffle_family unpack_high, 32, 256
vector_shuffle_family unpack_high, 64, 128
vector_shuffle_family unpack_high, 64, 256
vector_shuffle_family shuffle, 32, 128, 1
vector_shuffle_family shuffle, 32, 256, 1
vector_shuffle_family shuffle, 64, 128, 1
vector_shuffle_family shuffle, 64, 256, 1
vector_shuffle_family hadd, 32, 128
vector_shuffle_family hadd, 32, 256
vector_shuffle_family hadd, 64, 128
vector_shuffle_family hadd, 64, 256
vector_shuffle_family hsub, 32, 128
vector_shuffle_family hsub, 32, 256
vector_shuffle_family hsub, 64, 128
vector_shuffle_family hsub, 64, 256
vector_shuffle_family addsub, 32, 128
vector_shuffle_family addsub, 32, 256
vector_shuffle_family addsub, 64, 128
vector_shuffle_family addsub, 64, 256

.purgem vector_shuffle_family
.purgem vector_shuffle_memory
.purgem vector_shuffle_register
.purgem vector_shuffle_body
.purgem vector_shuffle_ps_index
