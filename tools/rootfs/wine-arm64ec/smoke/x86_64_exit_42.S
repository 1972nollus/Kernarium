/* SPDX-License-Identifier: CC0-1.0 */

    .text
    .globl main
    .def main; .scl 2; .type 32; .endef
    .seh_proc main
main:
    /* The first four arguments fit in registers, while CreateFileA's final
     * three arguments force its ARM64EC entry thunk to read the x64 stack
     * through x4.  This catches a bridge that can run trivial AMD64 code but
     * does not establish the entry-thunk transition ABI. */
    subq $56, %rsp
    .seh_stackalloc 56
    .seh_endprologue
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

    .section .rdata,"dr"
uml_stack_argument_path:
    .asciz "Z:\\\\tmp\\\\uml-arm64ec-x64-stack-args.tmp"
