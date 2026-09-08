/* SPDX-License-Identifier: CC0-1.0 */

    .text
    .globl main
    .def main; .scl 2; .type 32; .endef
    .seh_proc main
main:
    /* InitOnceExecuteOnce enters this AMD64 callback from native ARM64EC and
     * returns to the native continuation following its `blr x16` exit thunk.
     * This catches a bridge that can call native APIs but corrupts a native
     * caller while returning from emulated code. */
    subq $56, %rsp
    .seh_stackalloc 56
    .seh_endprologue
    /* INT 2e must reach Wine's Windows syscall dispatcher. An out-of-range
     * number returns STATUS_INVALID_SYSTEM_SERVICE, never SIGILL. */
    movl $0xffffffff, %eax
    int $0x2e
    cmpl $0xc000001c, %eax
    jne 1f
    leaq uml_once(%rip), %rcx
    leaq uml_once_callback(%rip), %rdx
    leaq uml_callback_parameter(%rip), %r8
    leaq uml_callback_context(%rip), %r9
    callq *__imp_InitOnceExecuteOnce(%rip)
    testl %eax, %eax
    je 1f
    cmpl $1, uml_callback_seen(%rip)
    jne 1f
    leaq uml_callback_parameter(%rip), %rax
    cmpq %rax, uml_callback_context(%rip)
    jne 1f

    /* The first four arguments fit in registers, while CreateFileA's final
     * three arguments force its ARM64EC entry thunk to read the x64 stack
     * through x4.  This catches a bridge that can run trivial AMD64 code but
     * does not establish the entry-thunk transition ABI. */
    leaq uml_stack_argument_path(%rip), %rcx
    movl $0x40000000, %edx
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    movq $2, 32(%rsp)
    movq $0x80, 40(%rsp)
    movq $0, 48(%rsp)
    callq *__imp_CreateFileA(%rip)
    cmpq $-1, %rax
    je 1f
    movq %rax, %rcx
    callq *__imp_CloseHandle(%rip)
    testl %eax, %eax
    je 1f
    leaq uml_stack_argument_path(%rip), %rcx
    callq *__imp_DeleteFileA(%rip)
    testl %eax, %eax
    je 1f
    movl $42, %eax
    jmp 2f
1:
    movl $1, %eax
2:
    addq $56, %rsp
    ret
    .seh_endproc

    .globl uml_once_callback
    .def uml_once_callback; .scl 2; .type 32; .endef
    .seh_proc uml_once_callback
uml_once_callback:
    .seh_endprologue
    movq %rdx, (%r8)
    movl $1, uml_callback_seen(%rip)
    movl $1, %eax
    ret
    .seh_endproc

    .section .rdata,"dr"
uml_stack_argument_path:
    .asciz "Z:\\\\tmp\\\\uml-arm64ec-x64-stack-args.tmp"
uml_callback_parameter:
    .quad 0x554d4c43414c4c42

    .data
    .p2align 3
uml_once:
    .quad 0
uml_callback_context:
    .quad 0
uml_callback_seen:
    .long 0
