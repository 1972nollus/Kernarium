/* SPDX-License-Identifier: GPL-2.0 */
	.text
	.globl _start
	.type _start, %function
_start:
	/* Force one lazy libc symbol resolution before exercising raw syscalls. */
	bl getpid

	mov x0, #1
	adr x1, message
	mov x2, #(message_end - message)
	mov x8, #64
	svc #0

	/* Stay alive as PID 1 so the harness can observe a stable guest. */
1:
	mov x0, #0
	mov x1, #0
	mov x2, #0
	mov x3, #0
	mov x4, #0
	mov x5, #0
	mov x8, #73
	svc #0
	b 1b

	.section .rodata
message:
	.ascii "dynamic 4K guest: PASS\n"
message_end:

