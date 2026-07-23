/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UML_DARWIN_GUEST_H
#define UML_DARWIN_GUEST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	DARWIN_UML_GUEST_PAGE_SIZE = 4096,
	DARWIN_UML_GUEST_REGISTER_COUNT = 31,
	DARWIN_UML_VECTOR_REGISTER_COUNT = 32,
};

enum darwin_uml_guest_event {
	DARWIN_UML_EVENT_SYSCALL = 1,
	DARWIN_UML_EVENT_FAULT = 2,
	DARWIN_UML_EVENT_INTERRUPT = 3,
	DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION = 4,
	DARWIN_UML_EVENT_BREAKPOINT = 5,
	DARWIN_UML_EVENT_FATAL = -1,
};

enum {
	DARWIN_UML_FAULT_WRITE = 2,
	DARWIN_UML_FAULT_PAGE = 14,
};

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

static_assert(offsetof(darwin_uml_guest_registers, fp) == 29 * sizeof(uint64_t));
static_assert(offsetof(darwin_uml_guest_registers, pc) == 32 * sizeof(uint64_t));
static_assert(offsetof(darwin_uml_guest_registers, pstate) == 33 * sizeof(uint64_t));
static_assert(offsetof(darwin_uml_guest_registers, original_x0) ==
	      34 * sizeof(uint64_t));
static_assert(sizeof(darwin_uml_faultinfo) == 24);
static_assert(sizeof(darwin_uml_guest_fp_registers) == 528);
#endif

#endif
