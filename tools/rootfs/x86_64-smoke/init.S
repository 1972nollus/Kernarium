/* SPDX-License-Identifier: GPL-2.0-or-later */
	.intel_syntax noprefix
	.global _start
	.section .text

_start:
	/* Validate the x86-64 arch_prctl path and FS-relative addressing. */
	mov eax, 158
	mov edi, 0x1002
	lea rsi, [rip + tls_word]
	syscall
	test eax, eax
	jne failure
	mov rax, QWORD PTR fs:0
	mov rbx, 0x1122334455667788
	cmp rax, rbx
	jne failure

	/* Install a handler without SA_RESTORER so Darwin's synthetic return
	 * boundary and the x86-64 rt_sigframe are both exercised.
	 */
	mov eax, 13
	mov edi, 10
	lea rsi, [rip + signal_action]
	xor edx, edx
	mov r10d, 8
	syscall
	test eax, eax
	jne failure

	mov eax, 39
	syscall
	mov edi, eax
	mov esi, 10
	mov eax, 62
	syscall
	test eax, eax
	jne failure
	mov rax, QWORD PTR [rip + signal_seen]
	cmp rax, 1
	jne failure

	mov eax, 1
	mov edi, 1
	lea rsi, [rip + success_message]
	mov edx, OFFSET success_message_size
	syscall

	/* LINUX_REBOOT_CMD_POWER_OFF. */
	mov eax, 169
	mov edi, 0xfee1dead
	mov esi, 0x28121969
	mov edx, 0x4321fedc
	xor r10d, r10d
	syscall

	/* A successful reboot does not return. */
	xor edi, edi
	mov eax, 231
	syscall

failure:
	mov eax, 1
	mov edi, 2
	lea rsi, [rip + failure_message]
	mov edx, OFFSET failure_message_size
	syscall
	mov edi, 1
	mov eax, 231
	syscall

signal_handler:
	mov eax, 1
	mov QWORD PTR [rip + signal_seen], rax
	ret

	.section .rodata
success_message:
	.ascii "x86_64 gadgets rootfs: syscall tls signal ok\n"
success_message_end:
	.equ success_message_size, success_message_end - success_message
failure_message:
	.ascii "x86_64 gadgets rootfs: validation failed\n"
failure_message_end:
	.equ failure_message_size, failure_message_end - failure_message

	.section .data
	.align 8
tls_word:
	.quad 0x1122334455667788
signal_seen:
	.quad 0
signal_action:
	.quad signal_handler
	.quad 0
	.quad 0
	.quad 0
