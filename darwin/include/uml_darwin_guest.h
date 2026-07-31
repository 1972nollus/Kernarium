/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_GUEST_H
#define UML_DARWIN_GUEST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DARWIN_UML_GUEST_PAGE_SIZE
#define DARWIN_UML_GUEST_PAGE_SIZE 16384
#endif

#ifndef DARWIN_UML_GUEST_PAGE_SHIFT
#define DARWIN_UML_GUEST_PAGE_SHIFT 14
#endif

enum {
	DARWIN_UML_GUEST_REGISTER_COUNT = 31,
	DARWIN_UML_VECTOR_REGISTER_COUNT = 32,
	DARWIN_UML_POINTER_AUTH_KEY_COUNT = 5,
};

enum darwin_uml_guest_event {
	DARWIN_UML_EVENT_SYSCALL = 1,
	DARWIN_UML_EVENT_FAULT = 2,
	DARWIN_UML_EVENT_INTERRUPT = 3,
	DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION = 4,
	DARWIN_UML_EVENT_BREAKPOINT = 5,
	DARWIN_UML_EVENT_GCS_FAULT = 6,
	DARWIN_UML_EVENT_FLOATING_POINT_TRAP = 7,
	DARWIN_UML_EVENT_FATAL = -1,
};

enum {
	DARWIN_UML_FAULT_WRITE = 2,
	DARWIN_UML_FAULT_PAGE = 14,
};

/* ESR-shaped metadata carried in fault.error_code for a floating-point trap. */
#define DARWIN_UML_FLOATING_POINT_EC (0x2cU << 26)
#define DARWIN_UML_FLOATING_POINT_IL (1U << 25)
#define DARWIN_UML_FLOATING_POINT_TFV (1U << 23)
#define DARWIN_UML_FLOATING_POINT_INVALID (1U << 0)
#define DARWIN_UML_FLOATING_POINT_DIVIDE_BY_ZERO (1U << 1)
#define DARWIN_UML_FLOATING_POINT_OVERFLOW (1U << 2)
#define DARWIN_UML_FLOATING_POINT_UNDERFLOW (1U << 3)
#define DARWIN_UML_FLOATING_POINT_INEXACT (1U << 4)
#define DARWIN_UML_FLOATING_POINT_INPUT_DENORMAL (1U << 7)
#define DARWIN_UML_FLOATING_POINT_SYNDROME_BASE \
	(DARWIN_UML_FLOATING_POINT_EC | DARWIN_UML_FLOATING_POINT_IL | \
	 DARWIN_UML_FLOATING_POINT_TFV)

/* This is an ABI address installed by arch/arm64/um/signal.c, not generated
 * code.  Reaching it asks the native UML kernel to execute rt_sigreturn.
 */
#define DARWIN_UML_SIGRETURN_TRAMPOLINE UINT64_C(0x3fffffffff00)
#define DARWIN_UML_NR_RT_SIGRETURN UINT64_C(139)

struct darwin_uml_faultinfo {
	int32_t error_code;
	int32_t padding;
	uint64_t address;
	int32_t trap_number;
	int32_t trailing_padding;
};

/* Matches struct uml_pt_regs through (but not including) its flexible FP
 * state.  The general-purpose prefix deliberately matches Darwin's
 * arm_thread_state64_t, which is also the layout used to generate UML's
 * host-register offsets.
 */
struct darwin_uml_guest_registers {
	uint64_t x[29];
	uint64_t fp;
	uint64_t lr;
	uint64_t sp;
	uint64_t pc;
	uint32_t pstate;
	uint32_t state_flags;
	uint64_t original_x0;
	struct darwin_uml_faultinfo fault;
	int64_t syscall;
	int32_t is_user;
	int32_t padding;
	/* Architectural state without a slot in user_pt_regs.  Keeping it in
	 * uml_pt_regs makes syscall/fault boundaries and vCPU migration lossless
	 * without putting backend pointers in the kernel register frame.
	 */
	uint64_t fpmr;
	uint64_t svcr;
	/*
	 * Guest-owned 128-bit keys.  The low and high halves are stored at
	 * indexes 0 and 1 respectively.  Keeping these in uml_pt_regs makes
	 * signatures independent of the Darwin process and vCPU that runs a
	 * task.
	 */
	uint64_t pointer_auth_keys[DARWIN_UML_POINTER_AUTH_KEY_COUNT][2];
};

/* Linux's user_fpsimd_state and Darwin's arm_neon_state64_t share this
 * prefix.  The explicit tail also accounts for the 16-byte struct alignment.
 */
struct darwin_uml_guest_fp_registers {
	uint8_t vectors[DARWIN_UML_VECTOR_REGISTER_COUNT][16];
	uint32_t fpsr;
	uint32_t fpcr;
	uint32_t reserved[2];
};

#ifdef __cplusplus
}

static_assert(DARWIN_UML_EVENT_FLOATING_POINT_TRAP == 7);
static_assert(DARWIN_UML_FLOATING_POINT_SYNDROME_BASE == 0xb2800000U);
static_assert(DARWIN_UML_GUEST_PAGE_SIZE ==
	      (UINT64_C(1) << DARWIN_UML_GUEST_PAGE_SHIFT));
static_assert(DARWIN_UML_GUEST_PAGE_SIZE == 4096 ||
	      DARWIN_UML_GUEST_PAGE_SIZE == 16384);
static_assert(offsetof(darwin_uml_guest_registers, fp) == 29 * sizeof(uint64_t));
static_assert(offsetof(darwin_uml_guest_registers, pc) == 32 * sizeof(uint64_t));
static_assert(offsetof(darwin_uml_guest_registers, pstate) == 33 * sizeof(uint64_t));
static_assert(offsetof(darwin_uml_guest_registers, original_x0) ==
	      34 * sizeof(uint64_t));
static_assert(offsetof(darwin_uml_guest_registers, fpmr) ==
	      40 * sizeof(uint64_t));
static_assert(offsetof(darwin_uml_guest_registers, pointer_auth_keys) ==
	      42 * sizeof(uint64_t));
static_assert(sizeof(darwin_uml_guest_registers) == 52 * sizeof(uint64_t));
static_assert(sizeof(darwin_uml_faultinfo) == 24);
static_assert(sizeof(darwin_uml_guest_fp_registers) == 528);
#endif

#endif
