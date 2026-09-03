/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wine_cpu_protocol.h"

.text
.p2align 4

.globl uml_wow64_hostcall
uml_wow64_hostcall:
    brk #UML_WINE_CPU_BRK_IMMEDIATE
    ret
