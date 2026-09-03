/* SPDX-License-Identifier: CC0-1.0 */

    .text
    .globl _main
    .def _main; .scl 2; .type 32; .endef
_main:
    movl $43, %eax
    ret
