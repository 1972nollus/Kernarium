/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "wine_cpu_protocol.h"

#define REQUEST_CONTEXT UML_WINE_CPU_REQUEST_CONTEXT_OFFSET
#define CONTEXT_GPR (REQUEST_CONTEXT + UML_WINE_X86_CONTEXT_GPR_OFFSET)
#define CONTEXT_RIP (REQUEST_CONTEXT + UML_WINE_X86_CONTEXT_RIP_OFFSET)
#define CONTEXT_RFLAGS (REQUEST_CONTEXT + UML_WINE_X86_CONTEXT_RFLAGS_OFFSET)
#define CONTEXT_VECTORS (REQUEST_CONTEXT + UML_WINE_X86_CONTEXT_VECTORS_OFFSET)

.text
.p2align 4

/* __os_arm64x_x64_jump: target x9, x86 return address is native LR. */
.globl DispatchJump
DispatchJump:
    str x30, [sp, #-8]!
    b uml_arm64ec_capture

/* __os_arm64x_dispatch_ret: target is the native LR. */
.globl RetToEntryThunk
RetToEntryThunk:
    mov x9, x30
    b uml_arm64ec_capture

/* __os_arm64x_dispatch_call_no_redirect: target x9. */
.globl ExitToX64
ExitToX64:
    str x30, [sp, #-8]!

uml_arm64ec_capture:
    ldr x16, [x18, #0x1788]
    mov w17, #1
    strb w17, [x16]
    ldr x16, [x16, #0x30]
    stp x8, x0, [x16, #(CONTEXT_GPR + 0)]
    stp x1, x27, [x16, #(CONTEXT_GPR + 16)]
    mov x17, sp
    stp x17, x29, [x16, #(CONTEXT_GPR + 32)]
    stp x25, x26, [x16, #(CONTEXT_GPR + 48)]
    stp x2, x3, [x16, #(CONTEXT_GPR + 64)]
    stp x4, x5, [x16, #(CONTEXT_GPR + 80)]
    stp x19, x20, [x16, #(CONTEXT_GPR + 96)]
    stp x21, x22, [x16, #(CONTEXT_GPR + 112)]
    str x9, [x16, #CONTEXT_RIP]

    ldr x15, [x16, #CONTEXT_RFLAGS]
    mov x7, #0x8c1
    bic x15, x15, x7
    mrs x7, nzcv
    ubfx x6, x7, #29, #1
    orr x15, x15, x6
    ubfx x6, x7, #30, #1
    orr x15, x15, x6, lsl #6
    ubfx x6, x7, #31, #1
    orr x15, x15, x6, lsl #7
    ubfx x6, x7, #28, #1
    orr x15, x15, x6, lsl #11
    str x15, [x16, #CONTEXT_RFLAGS]

    add x17, x16, #CONTEXT_VECTORS
    str q0, [x17, #0]
    str q1, [x17, #32]
    str q2, [x17, #64]
    str q3, [x17, #96]
    str q4, [x17, #128]
    str q5, [x17, #160]
    str q6, [x17, #192]
    str q7, [x17, #224]
    str q8, [x17, #256]
    str q9, [x17, #288]
    str q10, [x17, #320]
    str q11, [x17, #352]
    str q12, [x17, #384]
    str q13, [x17, #416]
    str q14, [x17, #448]
    str q15, [x17, #480]
    b uml_arm64ec_run

.globl uml_arm64ec_enter_loaded
uml_arm64ec_enter_loaded:
    mov x16, x0
    ldr x17, [x16, #(CONTEXT_GPR + 32)]
    mov sp, x17

uml_arm64ec_run:
    mov w17, #UML_WINE_CPU_OPERATION_RUN
    str w17, [x16, #UML_WINE_CPU_REQUEST_OPERATION_OFFSET]
    str wzr, [x16, #UML_WINE_CPU_REQUEST_STATUS_OFFSET]
    mov x0, x16
    brk #UML_WINE_CPU_BRK_IMMEDIATE
    mov x16, x0
    ldr w17, [x16, #UML_WINE_CPU_REQUEST_STATUS_OFFSET]
    cmp w17, #UML_WINE_CPU_STATUS_OK
    b.ne uml_arm64ec_raise
    ldr w17, [x16, #UML_WINE_CPU_REQUEST_STOP_REASON_OFFSET]
    cmp w17, #UML_WINE_CPU_STOP_NATIVE_TARGET
    b.eq uml_arm64ec_leave_native
    cmp w17, #UML_WINE_CPU_STOP_BUDGET
    b.eq uml_arm64ec_run
    cmp w17, #UML_WINE_CPU_STOP_INTERRUPT
    b.eq uml_arm64ec_run

uml_arm64ec_raise:
    mov x0, x16
    ldr x17, [x18, #0x1788]
    ldr x17, [x17, #8]
    mov sp, x17
    bl uml_arm64ec_raise_stop
    brk #1

uml_arm64ec_leave_native:
    ldr x15, [x18, #0x1788]
    strb wzr, [x15]

    add x17, x16, #CONTEXT_VECTORS
    ldr q0, [x17, #0]
    ldr q1, [x17, #32]
    ldr q2, [x17, #64]
    ldr q3, [x17, #96]
    ldr q4, [x17, #128]
    ldr q5, [x17, #160]
    ldr q6, [x17, #192]
    ldr q7, [x17, #224]
    ldr q8, [x17, #256]
    ldr q9, [x17, #288]
    ldr q10, [x17, #320]
    ldr q11, [x17, #352]
    ldr q12, [x17, #384]
    ldr q13, [x17, #416]
    ldr q14, [x17, #448]
    ldr q15, [x17, #480]

    ldr x15, [x16, #CONTEXT_RFLAGS]
    mov x7, xzr
    tbz x15, #0, 1f
    orr x7, x7, #(1 << 29)
1:  tbz x15, #6, 2f
    orr x7, x7, #(1 << 30)
2:  tbz x15, #7, 3f
    orr x7, x7, #(1 << 31)
3:  tbz x15, #11, 4f
    orr x7, x7, #(1 << 28)
4:  msr nzcv, x7

    ldr x17, [x16, #CONTEXT_RIP]
    ldp x8, x0, [x16, #(CONTEXT_GPR + 0)]
    ldp x1, x27, [x16, #(CONTEXT_GPR + 16)]
    ldp x25, x26, [x16, #(CONTEXT_GPR + 48)]
    ldp x2, x3, [x16, #(CONTEXT_GPR + 64)]
    ldp x4, x5, [x16, #(CONTEXT_GPR + 80)]
    ldp x19, x20, [x16, #(CONTEXT_GPR + 96)]
    ldp x21, x22, [x16, #(CONTEXT_GPR + 112)]
    ldr x29, [x16, #(CONTEXT_GPR + 40)]
    ldr x6, [x16, #(CONTEXT_GPR + 32)]
    mov sp, x6
    mov x9, x17

    /* A target immediately following `blr x16` is the native continuation
     * of an exit thunk.  Its LR, SP, and x4 are already live ARM64EC state;
     * resume it without applying entry-thunk stack conversion.  Otherwise
     * resolve the entry-thunk displacement stored before the target. */
    mov w7, #0x0200
    movk w7, #0xd63f, lsl #16
    ldursw x6, [x17, #-4]
    cmp w6, w7
    b.eq 7f
    and x6, x6, #-4
    add x17, x17, x6
    mov x6, sp
    tbz x6, #3, 6f
    ldr x30, [x6], #8
    /* ARM64EC entry thunks receive the post-pop x64 stack pointer in x4.
     * It is not the ordinary ARM64EC mapping of the volatile x86 r10. */
    mov x4, x6
    mov sp, x6
    br x17
6:
    mov x4, x6
    adrp x30, uml_arm64ec_x86_return
    ldr x30, [x30, #:lo12:uml_arm64ec_x86_return]
7:
    br x17

.globl uml_arm64ec_hostcall
uml_arm64ec_hostcall:
    brk #UML_WINE_CPU_BRK_IMMEDIATE
    ret

.globl uml_arm64ec_jump_stack
uml_arm64ec_jump_stack:
    mov x16, x0
    mov sp, x1
    br x16
