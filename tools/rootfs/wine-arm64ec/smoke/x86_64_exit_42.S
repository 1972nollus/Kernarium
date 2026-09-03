/* SPDX-License-Identifier: CC0-1.0 */

    .text
    .globl main
    .def main; .scl 2; .type 32; .endef
    .seh_proc main
main:
    movl $42, %eax
    ret
    .seh_endproc
