/* SPDX-License-Identifier: GPL-2.0 */

	.text
	.p2align 12
	.globl _start
_start:
	mov x1, #1
	mov x2, #2
	mov x3, #3
	mov x4, #4
	mov x5, #5
	mov x6, #6
	mov x7, #7
	mov x9, #9
	mov x10, #10
	mov x11, #11
	mov x12, #12
	mov x13, #13
	mov x14, #14
	mov x15, #15
	mov x16, #16
	mov x17, #17
	mov x18, #18
	mov x19, #19
	mov x20, #20
	mov x21, #21
	mov x22, #22
	mov x23, #23
	mov x24, #24
	mov x25, #25
	mov x26, #26
	mov x27, #27
	mov x28, #28
	mov x29, #29
	mov x30, #30
	movi v0.16b, #0x5a
	mov v1.16b, v0.16b
	mov v2.16b, v0.16b
	mov v3.16b, v0.16b
	mov v4.16b, v0.16b
	mov v5.16b, v0.16b
	mov v6.16b, v0.16b
	mov v7.16b, v0.16b
	mov v8.16b, v0.16b
	mov v9.16b, v0.16b
	mov v10.16b, v0.16b
	mov v11.16b, v0.16b
	mov v12.16b, v0.16b
	mov v13.16b, v0.16b
	mov v14.16b, v0.16b
	mov v15.16b, v0.16b
	mov v16.16b, v0.16b
	mov v17.16b, v0.16b
	mov v18.16b, v0.16b
	mov v19.16b, v0.16b
	mov v20.16b, v0.16b
	mov v21.16b, v0.16b
	mov v22.16b, v0.16b
	mov v23.16b, v0.16b
	mov v24.16b, v0.16b
	mov v25.16b, v0.16b
	mov v26.16b, v0.16b
	mov v27.16b, v0.16b
	mov v28.16b, v0.16b
	mov v29.16b, v0.16b
	mov v30.16b, v0.16b
	mov v31.16b, v0.16b

	/* getpid() has no arguments; x0 is its only architectural result. */
	mov x8, #172
	svc #0

	adrp x0, saved
	add x0, x0, :lo12:saved
	stp x1, x2, [x0, #0]
	stp x3, x4, [x0, #16]
	stp x5, x6, [x0, #32]
	stp x7, x8, [x0, #48]
	stp x9, x10, [x0, #64]
	stp x11, x12, [x0, #80]
	stp x13, x14, [x0, #96]
	stp x15, x16, [x0, #112]
	stp x17, x18, [x0, #128]
	stp x19, x20, [x0, #144]
	stp x21, x22, [x0, #160]
	stp x23, x24, [x0, #176]
	stp x25, x26, [x0, #192]
	stp x27, x28, [x0, #208]
	stp x29, x30, [x0, #224]
	add x0, x0, #240
	stp q0, q1, [x0, #0]
	stp q2, q3, [x0, #32]
	stp q4, q5, [x0, #64]
	stp q6, q7, [x0, #96]
	stp q8, q9, [x0, #128]
	stp q10, q11, [x0, #160]
	stp q12, q13, [x0, #192]
	stp q14, q15, [x0, #224]
	stp q16, q17, [x0, #256]
	stp q18, q19, [x0, #288]
	stp q20, q21, [x0, #320]
	stp q22, q23, [x0, #352]
	stp q24, q25, [x0, #384]
	stp q26, q27, [x0, #416]
	stp q28, q29, [x0, #448]
	stp q30, q31, [x0, #480]

	adrp x0, saved
	add x0, x0, :lo12:saved
	mov x1, #1
1:
	ldr x2, [x0], #8
	cmp x1, #8
	b.eq 2f
	cmp x2, x1
	b.ne fail_gpr
	b 3f
2:
	cmp x2, #172
	b.ne fail_gpr
3:
	add x1, x1, #1
	cmp x1, #31
	b.ne 1b

	/* Every byte in all 32 vector registers must survive the syscall. */
	mov x1, #512
4:
	ldrb w2, [x0], #1
	cmp w2, #0x5a
	b.ne fail_neon
	subs x1, x1, #1
	b.ne 4b

	/* These system-register instructions are breakpoint-emulated on Darwin. */
	mov x0, #0x1234
	msr tpidr_el0, x0
	mrs x1, tpidr_el0
	cmp x0, x1
	b.ne fail_tls

	adrp x1, pass_message
	add x1, x1, :lo12:pass_message
	mov x2, #26
	b print_and_idle
fail_gpr:
	cmp x1, #18
	b.ne 6f
	adrp x1, x18_message
	add x1, x1, :lo12:x18_message
	mov x2, #30
	b print_and_idle
6:
	adrp x1, gpr_message
	add x1, x1, :lo12:gpr_message
	mov x2, #30
	b print_and_idle
fail_neon:
	adrp x1, neon_message
	add x1, x1, :lo12:neon_message
	mov x2, #31
	b print_and_idle
fail_tls:
	adrp x1, tls_message
	add x1, x1, :lo12:tls_message
	mov x2, #30

print_and_idle:
	mov x0, #1
	mov x8, #64
	svc #0
5:
	mov x8, #172
	svc #0
	b 5b

	.section .rodata
pass_message:
	.ascii "guest register test: PASS\n"
 x18_message:
	.ascii "guest register test: X18 FAIL\n"
gpr_message:
	.ascii "guest register test: GPR FAIL\n"
neon_message:
	.ascii "guest register test: NEON FAIL\n"
tls_message:
	.ascii "guest register test: TLS FAIL\n"

	.bss
	.p2align 12
saved:
	.skip 752
