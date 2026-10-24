// SPDX-License-Identifier: GPL-2.0-or-later
//
// A static aarch64 program that leaves known values in x15-x30 on its
// virtual CPU, for aarch32_guest_probe.py, which assembles and links it in
// the guest with `as` and `ld -static`. The runtime enters an AArch32 task
// with R0-R14 only, so these registers are what a 64-bit task leaves behind
// for the 32-bit task that runs next on the same virtual CPU.
//
//   spin                  sets them before every sched_yield, forever
//   exec PROGRAM ARGS...  sets them and executes PROGRAM; exits 65 if
//                         execve fails
//
// Any other use exits 64. Every value has both halves set.

	.text
	.globl	_start

.macro dirty
	ldr	x15, =0xa64d000f000f0bad
	ldr	x16, =0xa64d0010001020ad
	ldr	x17, =0xa64d0011001130ad
	ldr	x18, =0xa64d0012001240ad
	ldr	x19, =0xa64d0013001350ad
	ldr	x20, =0xa64d0014001460ad
	ldr	x21, =0xa64d0015001570ad
	ldr	x22, =0xa64d0016001680ad
	ldr	x23, =0xa64d0017001790ad
	ldr	x24, =0xa64d00180018a0ad
	ldr	x25, =0xa64d00190019b0ad
	ldr	x26, =0xa64d001a001ac0ad
	ldr	x27, =0xa64d001b001bd0ad
	ldr	x28, =0xa64d001c001ce0ad
	ldr	x29, =0xa64d001d001df0ad
	ldr	x30, =0xa64d001e001e00ad
.endm

_start:
	ldr	x9, [sp]		// argc
	add	x10, sp, #8		// argv
	cmp	x9, #2
	b.lt	usage
	ldr	x11, [x10, #8]		// argv[1]
	ldrb	w12, [x11]
	cmp	w12, #'s'
	b.eq	spin
	cmp	w12, #'e'
	b.eq	run
usage:
	mov	x0, #64
	mov	x8, #94			// exit_group
	svc	#0

spin:
	dirty
	mov	x8, #124		// sched_yield
	svc	#0
	b	spin

run:
	cmp	x9, #3
	b.lt	usage
	add	x1, x10, #16		// &argv[2]
	ldr	x0, [x1]		// the program
	add	x2, x10, x9, lsl #3
	add	x2, x2, #8		// envp, after argv's NULL
	dirty
	mov	x8, #221		// execve
	svc	#0
	mov	x0, #65
	mov	x8, #94
	svc	#0

	.ltorg

	.section .note.GNU-stack,"",%progbits
