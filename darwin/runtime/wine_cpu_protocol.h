// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * Private ARM64EC guest-to-host CPU service. This is deliberately not part
 * of either the installed emulator API or the Linux host ABI. A Wine PE
 * module issues the reserved BRK from an arm64 Linux process and passes the
 * guest virtual address of this request in x0. The Darwin runtime handles it
 * without involving Linux, then resumes at the following arm64 instruction.
 */
#define UML_WINE_CPU_PROTOCOL_MAGIC 0x554d4c5838365631
#define UML_WINE_CPU_PROTOCOL_VERSION 2
#define UML_WINE_CPU_BRK_IMMEDIATE 0x554d
#define UML_WINE_CPU_BRK_INSTRUCTION \
	(0xd4200000 | (UML_WINE_CPU_BRK_IMMEDIATE << 5))

#define UML_WINE_CPU_OPERATION_QUERY 0
#define UML_WINE_CPU_OPERATION_RUN 1
#define UML_WINE_CPU_OPERATION_INVALIDATE 2
#define UML_WINE_CPU_OPERATION_RESET 3

#define UML_WINE_CPU_STATUS_PENDING 0
#define UML_WINE_CPU_STATUS_OK 1
#define UML_WINE_CPU_STATUS_BAD_REQUEST 2
#define UML_WINE_CPU_STATUS_UNSUPPORTED 3
#define UML_WINE_CPU_STATUS_INTERNAL_ERROR 4

#define UML_WINE_CPU_STOP_NONE 0
#define UML_WINE_CPU_STOP_NATIVE_TARGET 1
#define UML_WINE_CPU_STOP_SYSTEM_CALL 2
#define UML_WINE_CPU_STOP_MEMORY_FAULT 3
#define UML_WINE_CPU_STOP_INTERRUPT 4
#define UML_WINE_CPU_STOP_ILLEGAL_INSTRUCTION 5
#define UML_WINE_CPU_STOP_BREAKPOINT 6
#define UML_WINE_CPU_STOP_FLOATING_POINT 7
#define UML_WINE_CPU_STOP_BUDGET 8
#define UML_WINE_CPU_STOP_FATAL 9

#define UML_WINE_CPU_ACCESS_READ 0
#define UML_WINE_CPU_ACCESS_WRITE 1
#define UML_WINE_CPU_ACCESS_EXECUTE 2

#define UML_WINE_CPU_CAP_X86_64 (1ULL << 0)
#define UML_WINE_CPU_CAP_COMPAT32 (1ULL << 1)
#define UML_WINE_CPU_CAP_AVX (1ULL << 2)
#define UML_WINE_CPU_CAP_AVX2 (1ULL << 3)
#define UML_WINE_CPU_CAP_X87 (1ULL << 4)
#define UML_WINE_CPU_CAP_ARM64EC_BOUNDARY (1ULL << 5)
#define UML_WINE_CPU_CAP_STRICT_TSO (1ULL << 6)

#define UML_WINE_CPU_FLAG_EC_BITMAP_VALID (1U << 0)
/* Host-owned while Linux resolves a page fault raised by nested x86 code. */
#define UML_WINE_CPU_FLAG_LINUX_FAULT_RETRY (1U << 1)

/* Assembly-visible layout. Keep these checked against the C declarations
 * below; the ARM64EC module is built independently from the Apple runtime. */
#define UML_WINE_CPU_REQUEST_OPERATION_OFFSET 16
#define UML_WINE_CPU_REQUEST_FLAGS_OFFSET 20
#define UML_WINE_CPU_REQUEST_STATUS_OFFSET 24
#define UML_WINE_CPU_REQUEST_STOP_REASON_OFFSET 28
#define UML_WINE_CPU_REQUEST_EC_BITMAP_OFFSET 40
#define UML_WINE_CPU_REQUEST_CONTEXT_OFFSET 88

#define UML_WINE_X86_CONTEXT_GPR_OFFSET 0
#define UML_WINE_X86_CONTEXT_RIP_OFFSET 128
#define UML_WINE_X86_CONTEXT_RFLAGS_OFFSET 136
#define UML_WINE_X86_CONTEXT_FS_BASE_OFFSET 144
#define UML_WINE_X86_CONTEXT_GS_BASE_OFFSET 152
#define UML_WINE_X86_CONTEXT_VECTORS_OFFSET 160
#define UML_WINE_X86_CONTEXT_MMX_OFFSET 672
#define UML_WINE_X86_CONTEXT_MXCSR_OFFSET 848
#define UML_WINE_X86_CONTEXT_EXECUTION_MODE_OFFSET 852

#ifndef __ASSEMBLER__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct uml_wine_x86_context {
	uint64_t gpr[16];
	uint64_t rip;
	uint64_t rflags;
	uint64_t fs_base;
	uint64_t gs_base;
	uint8_t vectors[16][32];
	uint64_t mmx[8];
	uint64_t x87_significand[8];
	/* Base addresses for compat-mode user GDT selectors 0x33 through 0x53. */
	uint32_t compat_tls_bases[5];
	uint16_t segment_selectors[6];
	uint16_t x87_sign_exponent[8];
	uint32_t mxcsr;
	uint32_t execution_mode;
	uint16_t x87_control;
	uint16_t x87_status;
	uint16_t x87_tag;
	uint8_t x87_top;
	uint8_t x87_reduced_precision;
	uint8_t system_call_32bit;
	uint8_t compat_tls_present;
	uint8_t reserved[2];
};

struct uml_wine_cpu_request {
	uint64_t magic;
	uint32_t version;
	uint32_t size;
	uint32_t operation;
	uint32_t flags;
	uint32_t status;
	uint32_t stop_reason;
	uint64_t capabilities;
	uint64_t ec_bitmap;
	uint64_t invalidate_address;
	uint64_t invalidate_size;
	uint64_t fault_address;
	uint64_t retired_instructions;
	uint32_t fault_access;
	uint32_t syndrome;
	struct uml_wine_x86_context context;
};

#ifdef __cplusplus
}

#include <cstddef>

static_assert(sizeof(uml_wine_x86_context) == 872);
static_assert(sizeof(uml_wine_cpu_request) == 960);
static_assert(offsetof(uml_wine_cpu_request, operation) ==
	UML_WINE_CPU_REQUEST_OPERATION_OFFSET);
static_assert(offsetof(uml_wine_cpu_request, flags) ==
	UML_WINE_CPU_REQUEST_FLAGS_OFFSET);
static_assert(offsetof(uml_wine_cpu_request, status) ==
	UML_WINE_CPU_REQUEST_STATUS_OFFSET);
static_assert(offsetof(uml_wine_cpu_request, stop_reason) ==
	UML_WINE_CPU_REQUEST_STOP_REASON_OFFSET);
static_assert(offsetof(uml_wine_cpu_request, ec_bitmap) ==
	UML_WINE_CPU_REQUEST_EC_BITMAP_OFFSET);
static_assert(offsetof(uml_wine_cpu_request, context) ==
	UML_WINE_CPU_REQUEST_CONTEXT_OFFSET);
static_assert(offsetof(uml_wine_x86_context, gpr) ==
	UML_WINE_X86_CONTEXT_GPR_OFFSET);
static_assert(offsetof(uml_wine_x86_context, rip) ==
	UML_WINE_X86_CONTEXT_RIP_OFFSET);
static_assert(offsetof(uml_wine_x86_context, rflags) ==
	UML_WINE_X86_CONTEXT_RFLAGS_OFFSET);
static_assert(offsetof(uml_wine_x86_context, fs_base) ==
	UML_WINE_X86_CONTEXT_FS_BASE_OFFSET);
static_assert(offsetof(uml_wine_x86_context, gs_base) ==
	UML_WINE_X86_CONTEXT_GS_BASE_OFFSET);
static_assert(offsetof(uml_wine_x86_context, vectors) ==
	UML_WINE_X86_CONTEXT_VECTORS_OFFSET);
static_assert(offsetof(uml_wine_x86_context, mmx) ==
	UML_WINE_X86_CONTEXT_MMX_OFFSET);
static_assert(offsetof(uml_wine_x86_context, mxcsr) ==
	UML_WINE_X86_CONTEXT_MXCSR_OFFSET);
static_assert(offsetof(uml_wine_x86_context, execution_mode) ==
	UML_WINE_X86_CONTEXT_EXECUTION_MODE_OFFSET);
#endif

#endif /* !__ASSEMBLER__ */
