/* SPDX-License-Identifier: GPL-2.0 */
#include "../include/uml_darwin_runtime.h"
#include "direct_gateway.h"

#include <mach/arm/thread_status.h>
#include <mach/exception_types.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mig.h>
#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define UML_SYSCALL_BRK 0xd4209880U
#define UML_TLS_WRITE_BRK_BASE 0xd4209a00U
#define UML_TLS_READ_BRK_BASE 0xd4209e00U
#define UML_TLS_BRK_SPAN (31U << 5)
#define UML_ENTER_BRK 0xd420aaa0U
#define UML_DIRECT_RESUME_BRK 0xd420aae0U
#define UML_X18_BRK 0xd420ac00U
#define UML_INDIRECT_BRK 0xd420ae00U
#define ARM64_USER_INSN_BREAKPOINT_CONTROL 0x1e5ULL
#define UML_SIGRETURN_TRAMPOLINE 0x3fffffffff00ULL
#define UML_NR_RT_SIGRETURN 139U
#define UML_EVENT_SYSCALL 1
#define UML_EVENT_FAULT 2
#define UML_EVENT_INDIRECT 3
#define UML_EVENT_FATAL (-1)
#define UML_FAULT_WRITE 2
#define UML_FAULT_PAGE 14
#define BRIDGE_STACK_SIZE (64 * 1024)

extern void darwin_uml_arch_enter(const arm_thread_state64_t *state,
				  unsigned long tls,
				  const arm_neon_state64_t *guest_fp,
				  arm_neon_state64_t *host_fp)
	__attribute__((noreturn));
extern void darwin_uml_return_trampoline(void) __attribute__((noreturn));
extern void darwin_uml_context_switch(void *old_context, void *new_context);
extern void darwin_uml_arch_enter_saved(void *kernel_context,
					const arm_thread_state64_t *state,
					unsigned long tls,
					const arm_neon_state64_t *guest_fp,
					arm_neon_state64_t *host_fp);
extern void darwin_uml_save_fp(arm_neon_state64_t *state);
extern void darwin_uml_restore_fp(const arm_neon_state64_t *state);

struct uml_faultinfo_bridge {
	int error_code;
	int padding;
	uint64_t address;
	int trap_number;
	int trailing_padding;
};

struct uml_registers_bridge {
	arm_thread_state64_t state;
	uint64_t original_x0;
	struct uml_faultinfo_bridge fault;
	int64_t syscall;
	int is_user;
};

#define X18_TRACE_LENGTH 64
#define X18_OPCODE_PROFILE_SIZE 4096

struct x18_trace_entry {
	uint64_t pc;
	uint64_t x18_before;
	uint64_t x18_after;
	uint64_t rd_before;
	uint64_t rd_after;
	uint64_t rn_before;
	uint32_t instruction;
};

struct x18_opcode_profile {
	uint32_t instruction;
	unsigned long count;
};

struct native_switch_context {
	uint64_t x19_x30[12];
	uint64_t sp;
	uint64_t argument;
};

struct guest_context {
	struct direct_gateway_state direct;
	thread_t thread;
	jmp_buf return_point;
	struct uml_registers_bridge *registers;
	arm_neon_state64_t *fp_registers;
	arm_neon_state64_t host_fp_registers;
	void *bridge_stack;
	int event;
	int entering;
	int guest_started;
	int signal_suspended;
	struct native_switch_context kernel_switch;
	struct native_switch_context signal_switch;
	struct native_switch_context direct_switch;
	uint64_t host_x18;
	unsigned long host_tls;
	unsigned long guest_tls;
	uint64_t guest_x18;
	int guest_x18_initialized;
	int write_step_active;
	unsigned long write_step_address;
	unsigned int x18_trace_position;
	struct x18_trace_entry x18_trace[X18_TRACE_LENGTH];
	struct guest_context *next;
};

_Static_assert(offsetof(struct direct_gateway_state, registers) == DG_REGISTERS,
	"direct gateway register pointer offset");
_Static_assert(offsetof(struct direct_gateway_state, fp_registers) ==
	DG_FP_REGISTERS, "direct gateway FP pointer offset");
_Static_assert(offsetof(struct direct_gateway_state, host_fp_registers) ==
	DG_HOST_FP_REGISTERS, "direct gateway host FP pointer offset");
_Static_assert(offsetof(struct direct_gateway_state, kernel_switch) ==
	DG_KERNEL_SWITCH, "direct gateway kernel switch offset");
_Static_assert(offsetof(struct direct_gateway_state, direct_switch) ==
	DG_DIRECT_SWITCH, "direct gateway coroutine offset");
_Static_assert(offsetof(struct direct_gateway_state, host_tls) == DG_HOST_TLS,
	"direct gateway host TLS offset");
_Static_assert(offsetof(struct direct_gateway_state, host_x18) == DG_HOST_X18,
	"direct gateway host x18 offset");
_Static_assert(offsetof(struct direct_gateway_state, guest_tls) == DG_GUEST_TLS,
	"direct gateway guest TLS offset");
_Static_assert(offsetof(struct direct_gateway_state, guest_x18) == DG_GUEST_X18,
	"direct gateway guest x18 offset");
_Static_assert(offsetof(struct direct_gateway_state, scratch_x16) ==
	DG_SCRATCH_X16, "direct gateway scratch offset");
_Static_assert(offsetof(struct direct_gateway_state, resume_stub) ==
	DG_RESUME_STUB, "direct gateway resume offset");
_Static_assert(offsetof(struct direct_gateway_state, guest_pc) == DG_GUEST_PC,
	"direct gateway guest PC offset");
_Static_assert(offsetof(struct direct_gateway_state, event) == DG_EVENT,
	"direct gateway event offset");
_Static_assert(offsetof(struct direct_gateway_state, suspended) == DG_SUSPENDED,
	"direct gateway suspended offset");
_Static_assert(sizeof(struct direct_gateway_state) == DG_SIZE,
	"direct gateway state size");
_Static_assert(offsetof(arm_thread_state64_t, __x[16]) == ARM64_STATE_X16,
	"Darwin x16 state offset");
_Static_assert(offsetof(arm_thread_state64_t, __x[17]) == ARM64_STATE_X17,
	"Darwin x17 state offset");
_Static_assert(offsetof(arm_thread_state64_t, __x[18]) == ARM64_STATE_X18,
	"Darwin x18 state offset");
_Static_assert(offsetof(arm_thread_state64_t, __fp) == ARM64_STATE_FP,
	"Darwin FP state offset");
_Static_assert(offsetof(arm_thread_state64_t, __lr) == ARM64_STATE_LR,
	"Darwin LR state offset");
_Static_assert(offsetof(arm_thread_state64_t, __sp) == ARM64_STATE_SP,
	"Darwin SP state offset");
_Static_assert(offsetof(arm_thread_state64_t, __pc) == ARM64_STATE_PC,
	"Darwin PC state offset");
_Static_assert(offsetof(arm_thread_state64_t, __cpsr) == ARM64_STATE_CPSR,
	"Darwin CPSR state offset");

static pthread_once_t exception_once = PTHREAD_ONCE_INIT;
static pthread_once_t guest_signals_once = PTHREAD_ONCE_INIT;
static pthread_key_t context_key;
static pthread_mutex_t contexts_lock = PTHREAD_MUTEX_INITIALIZER;
static struct guest_context *contexts;
static unsigned int fatal_exception_diagnostics;
static struct sigaction previous_guest_actions[NSIG];
static sigset_t guest_signal_mask;
static _Thread_local struct guest_context *signal_context;
static _Thread_local const arm_exception_state64_t *signal_exception_state;
static unsigned long profiled_direct_syscalls;
static unsigned long profiled_direct_indirects;
static unsigned long profiled_gateway_x18_repairs;
static unsigned long profiled_traps;
static unsigned long profiled_trap_syscalls;
static unsigned long profiled_trap_indirect;
static unsigned long profiled_trap_x18;
static unsigned long profiled_trap_tls;
static unsigned long profiled_trap_other;
static unsigned long profiled_bad_instructions;
static unsigned long profiled_bad_accesses;
static unsigned long profiled_resolved_execute;
static unsigned long profiled_resolved_write;
static unsigned long profiled_resolved_read;
static unsigned long profiled_guest_execute_faults;
static unsigned long profiled_guest_write_faults;
static unsigned long profiled_guest_read_faults;
static unsigned long profiled_contexts_created;
static unsigned long profiled_contexts_destroyed;
static unsigned long profiled_contexts_live;
static unsigned long profiled_syscalls[512];
static struct x18_opcode_profile x18_opcode_profile[X18_OPCODE_PROFILE_SIZE];
static int exception_profile_enabled;

/* CONFIG_SMP is disabled for this UML port, so the Linux scheduler enters at
 * most one guest context at a time even though guest tasks live on distinct
 * Darwin pthreads.  Nearby direct-call stubs use this pointer to find that
 * context without reserving a guest-visible general register.
 */
struct direct_gateway_state *darwin_uml_active_direct_gateway;

static unsigned long read_hardware_tls(void)
{
	unsigned long value;

	__asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
	return value;
}

static void report_exception_profile(void)
{
	struct x18_opcode_profile top[16] = { };
	unsigned int index;
	unsigned int position;

	fprintf(stderr,
		"Darwin guest traps: direct_syscall=%lu direct_indirect=%lu "
		"trap=%lu "
		"syscall=%lu indirect=%lu x18=%lu tls=%lu other=%lu "
		"bad_instruction=%lu bad_access=%lu\n",
		profiled_direct_syscalls, profiled_direct_indirects,
		profiled_traps,
		profiled_trap_syscalls, profiled_trap_indirect,
		profiled_trap_x18, profiled_trap_tls, profiled_trap_other,
		profiled_bad_instructions, profiled_bad_accesses);
	fprintf(stderr,
		"Darwin access faults: resolved_execute=%lu resolved_write=%lu "
		"resolved_read=%lu guest_execute=%lu guest_write=%lu "
		"guest_read=%lu gateway_x18_repair=%lu\n",
		profiled_resolved_execute, profiled_resolved_write,
		profiled_resolved_read, profiled_guest_execute_faults,
		profiled_guest_write_faults, profiled_guest_read_faults,
		profiled_gateway_x18_repairs);
	fprintf(stderr,
		"Darwin guest contexts: created=%lu destroyed=%lu live=%lu\n",
		profiled_contexts_created, profiled_contexts_destroyed,
		profiled_contexts_live);
	fprintf(stderr, "Darwin syscall numbers:");
	for (index = 0; index <
	     sizeof(profiled_syscalls) / sizeof(profiled_syscalls[0]); index++)
		if (profiled_syscalls[index] != 0)
			fprintf(stderr, " %u=%lu", index, profiled_syscalls[index]);
	fputc('\n', stderr);
	for (index = 0; index < X18_OPCODE_PROFILE_SIZE; index++) {
		if (x18_opcode_profile[index].count == 0)
			continue;
		for (position = 0; position < 16; position++)
			if (x18_opcode_profile[index].count > top[position].count) {
				unsigned int move;

				for (move = 15; move > position; move--)
					top[move] = top[move - 1];
				top[position] = x18_opcode_profile[index];
				break;
			}
	}
	fprintf(stderr, "Darwin x18 opcodes:");
	for (position = 0; position < 16 && top[position].count != 0;
	     position++)
		fprintf(stderr, " %#x=%lu", top[position].instruction,
			top[position].count);
	fputc('\n', stderr);
}

static void profile_x18_opcode(uint32_t instruction)
{
	unsigned int index;
	unsigned int probes;

	if (!exception_profile_enabled)
		return;
	index = (instruction * 2654435761U) & (X18_OPCODE_PROFILE_SIZE - 1);
	for (probes = 0; probes < X18_OPCODE_PROFILE_SIZE; probes++) {
		struct x18_opcode_profile *entry = &x18_opcode_profile[index];

		if (entry->count == 0) {
			entry->instruction = instruction;
			entry->count = 1;
			return;
		}
		if (entry->instruction == instruction) {
			entry->count++;
			return;
		}
		index = (index + 1) & (X18_OPCODE_PROFILE_SIZE - 1);
	}
}

static struct guest_context *find_context(thread_t thread)
{
	struct guest_context *context;

	for (context = contexts; context != NULL; context = context->next)
		if (context->thread == thread)
			return context;
	return NULL;
}

static void record_x18_trace(struct guest_context *context,
			     const arm_thread_state64_t *before,
			     const arm_thread_state64_t *after,
			     uint64_t x18_before, uint32_t instruction,
			     uint64_t pc)
{
	struct x18_trace_entry *entry = &context->x18_trace[
		context->x18_trace_position++ % X18_TRACE_LENGTH];
	unsigned int rd = instruction & 31U;
	unsigned int rn = (instruction >> 5) & 31U;

	entry->pc = pc;
	entry->instruction = instruction;
	entry->x18_before = x18_before;
	entry->x18_after = context->guest_x18;
	entry->rd_before = rd == 18 ? x18_before :
		(rd < 31 ? before->__x[rd] : 0);
	entry->rd_after = rd == 18 ? context->guest_x18 :
		(rd < 31 ? after->__x[rd] : 0);
	entry->rn_before = rn == 18 ? x18_before :
		(rn < 31 ? before->__x[rn] : 0);
}

void darwin_uml_guest_return_c(struct guest_context *context)
	__attribute__((noreturn));

void darwin_uml_guest_return_c(struct guest_context *context)
{
	longjmp(context->return_point, 1);
}

static uint64_t guest_register(const arm_thread_state64_t *state,
			       unsigned int reg, uint64_t guest_x18,
			       int reg31_is_sp)
{
	if (reg < 18)
		return state->__x[reg];
	if (reg == 18)
		return guest_x18;
	if (reg < 29)
		return state->__x[reg];
	if (reg == 29)
		return __darwin_arm_thread_state64_get_fp(*state);
	if (reg == 30)
		return __darwin_arm_thread_state64_get_lr(*state);
	return reg31_is_sp ? __darwin_arm_thread_state64_get_sp(*state) : 0;
}

static void set_guest_register(arm_thread_state64_t *state,
			       unsigned int reg, uint64_t value,
			       uint64_t *guest_x18, int reg31_is_sp)
{
	if (reg < 18)
		state->__x[reg] = value;
	else if (reg == 18)
		*guest_x18 = value;
	else if (reg < 29)
		state->__x[reg] = value;
	else if (reg == 29)
		__darwin_arm_thread_state64_set_fp(*state, value);
	else if (reg == 30)
		state->__lr = value;
	else if (reg31_is_sp)
		__darwin_arm_thread_state64_set_sp(*state, value);
}

static int emulate_gateway_x18_access(struct guest_context *context,
				       arm_thread_state64_t *state,
				       uintptr_t pc, uint32_t instruction,
				       int x18_stub)
{
	uint32_t operation = instruction & 0xffc00000U;
	unsigned int reg = instruction & 31U;
	unsigned int offset = ((instruction >> 10) & 0xfffU) * 8U;
	uint64_t value;

	if (instruction == 0xf9400252U) {
		/* The second instruction of each gateway dereferences the literal
		 * address of the active-state global.  Complete that load directly.
		 */
		state->__x[18] = (uintptr_t)&context->direct;
	} else {
		if (offset > sizeof(context->direct) - sizeof(value))
			return 0;
		if (operation == 0xf9400000U) {
			memcpy(&value, (const unsigned char *)&context->direct + offset,
			       sizeof(value));
			if (reg < 29)
				state->__x[reg] = value;
			else if (reg == 29)
				__darwin_arm_thread_state64_set_fp(*state, value);
			else if (reg == 30)
				state->__lr = value;
		} else if (operation == 0xf9000000U) {
			if (reg < 29)
				value = state->__x[reg];
			else if (reg == 29)
				value = __darwin_arm_thread_state64_get_fp(*state);
			else if (reg == 30)
				value = __darwin_arm_thread_state64_get_lr(*state);
			else
				value = 0;
			memcpy((unsigned char *)&context->direct + offset, &value,
			       sizeof(value));
			if (x18_stub && reg == 18 && offset == DG_GUEST_X18)
				context->guest_x18 = value;
		} else {
			return 0;
		}
	}
	__darwin_arm_thread_state64_set_pc_fptr(*state,
		(void (*)(void))(pc + sizeof(uint32_t)));
	profiled_gateway_x18_repairs++;
	return 1;
}

static uint64_t width_mask(unsigned int bits)
{
	return bits == 64 ? UINT64_MAX : UINT32_MAX;
}

static uint64_t low_mask(unsigned int bits)
{
	return bits >= 64 ? UINT64_MAX : ((1ULL << bits) - 1);
}

static uint64_t shifted_value(uint64_t value, unsigned int shift,
			      unsigned int amount, unsigned int bits)
{
	uint64_t mask = width_mask(bits);

	value &= mask;
	if (amount == 0)
		return value;
	switch (shift) {
	case 0:
		return (value << amount) & mask;
	case 1:
		return value >> amount;
	case 2:
		if (bits == 64)
			return (uint64_t)((int64_t)value >> amount);
		return (uint32_t)((int32_t)value >> amount);
	case 3:
		return ((value >> amount) | (value << (bits - amount))) & mask;
	default:
		return value;
	}
}

static int decode_logical_immediate(uint32_t instruction, unsigned int bits,
				    uint64_t *immediate)
{
	unsigned int selector = ((instruction >> 22) & 1U) << 6 |
		(~(instruction >> 10) & 0x3fU);
	unsigned int length;
	unsigned int levels;
	unsigned int size;
	unsigned int s;
	unsigned int r;
	uint64_t element;
	uint64_t mask = 0;
	unsigned int position;

	if (selector == 0)
		return 0;
	for (length = 6; length > 0; length--)
		if (selector & (1U << length))
			break;
	if (length == 0)
		return 0;
	levels = (1U << length) - 1U;
	s = (instruction >> 10) & levels;
	r = (instruction >> 16) & levels;
	if (s == levels)
		return 0;
	size = 1U << length;
	if (size > bits)
		return 0;
	element = s + 1U >= 64 ? UINT64_MAX :
		((1ULL << (s + 1U)) - 1ULL);
	if (r != 0)
		element = ((element >> r) | (element << (size - r))) &
			low_mask(size);
	for (position = 0; position < bits; position += size)
		mask |= element << position;
	*immediate = mask & width_mask(bits);
	return 1;
}

static void set_guest_flags(arm_thread_state64_t *state, uint64_t result,
			    unsigned int bits, int carry, int overflow)
{
	uint32_t flags = 0;
	uint64_t mask = width_mask(bits);

	result &= mask;
	if (result & (1ULL << (bits - 1)))
		flags |= 1U << 31;
	if (result == 0)
		flags |= 1U << 30;
	if (carry)
		flags |= 1U << 29;
	if (overflow)
		flags |= 1U << 28;
	state->__cpsr = (state->__cpsr & 0x0fffffffU) | flags;
}

static void set_add_sub_flags(arm_thread_state64_t *state, uint64_t left,
			      uint64_t right, uint64_t result,
			      unsigned int bits, int subtract)
{
	uint64_t mask = width_mask(bits);
	uint64_t sign = 1ULL << (bits - 1);
	int carry;
	int overflow;

	left &= mask;
	right &= mask;
	result &= mask;
	if (subtract) {
		carry = left >= right;
		overflow = !!(((left ^ right) & (left ^ result) & sign));
	} else {
		carry = bits == 64 ? result < left : left + right > mask;
		overflow = !!((~(left ^ right) & (left ^ result) & sign));
	}
	set_guest_flags(state, result, bits, carry, overflow);
}

static int emulate_x18_instruction(arm_thread_state64_t *state,
				   uint64_t *guest_x18,
				   uint32_t instruction, uint64_t pc)
{
	unsigned int sf = instruction >> 31;
	unsigned int rd = instruction & 31U;
	unsigned int rn = (instruction >> 5) & 31U;
	unsigned int bits = sf ? 64 : 32;
	uint64_t mask = width_mask(bits);

	/* ADR and ADRP. */
	if ((instruction & 0x1f000000U) == 0x10000000U) {
		int64_t immediate = (int64_t)(((instruction >> 5) & 0x7ffffU) << 2 |
					      ((instruction >> 29) & 3U));
		uint64_t value;

		immediate = (immediate << 43) >> 43;
		if (instruction & 0x80000000U)
			value = (pc & ~0xfffULL) + (uint64_t)(immediate << 12);
		else
			value = pc + (uint64_t)immediate;
		set_guest_register(state, rd, value, guest_x18, 0);
		goto done;
	}

	/* MOVN, MOVZ and MOVK. */
	if ((instruction & 0x1f800000U) == 0x12800000U) {
		unsigned int opc = (instruction >> 29) & 3U;
		unsigned int shift = ((instruction >> 21) & 3U) * 16U;
		uint64_t immediate = (uint64_t)((instruction >> 5) & 0xffffU)
			<< shift;
		uint64_t value;

		if (opc == 0)
			value = (~immediate) & mask;
		else if (opc == 2)
			value = immediate;
		else if (opc == 3)
			value = (guest_register(state, rd, *guest_x18, 0) &
				 ~(0xffffULL << shift)) | immediate;
		else
			return 0;
		set_guest_register(state, rd, value & mask, guest_x18, 0);
		goto done;
	}

	/* ADD/SUB immediate. */
	if ((instruction & 0x1f000000U) == 0x11000000U) {
		int subtract = !!(instruction & (1U << 30));
		int update_flags = !!(instruction & (1U << 29));
		uint64_t left = guest_register(state, rn, *guest_x18, 1) & mask;
		uint64_t right = (instruction >> 10) & 0xfffU;
		uint64_t result;

		if (instruction & (1U << 22))
			right <<= 12;
		result = subtract ? left - right : left + right;
		if (update_flags)
			set_add_sub_flags(state, left, right, result, bits,
					  subtract);
		set_guest_register(state, rd, result & mask, guest_x18,
				   !update_flags);
		goto done;
	}

	/* AND/ORR/EOR/ANDS immediate. */
	if ((instruction & 0x1f800000U) == 0x12000000U) {
		unsigned int opc = (instruction >> 29) & 3U;
		uint64_t immediate;
		uint64_t left;
		uint64_t value;

		if (!decode_logical_immediate(instruction, bits, &immediate))
			return 0;
		left = guest_register(state, rn, *guest_x18, 0) & mask;
		if (opc == 0 || opc == 3)
			value = left & immediate;
		else if (opc == 1)
			value = left | immediate;
		else
			value = left ^ immediate;
		if (opc == 3)
			set_guest_flags(state, value, bits, 0, 0);
		set_guest_register(state, rd, value & mask, guest_x18, 0);
		goto done;
	}

	/* SBFM/BFM/UBFM and their ASR/LSR/LSL/extend aliases. */
	if ((instruction & 0x1f800000U) == 0x13000000U) {
		unsigned int opc = (instruction >> 29) & 3U;
		unsigned int immr = (instruction >> 16) & 63U;
		unsigned int imms = (instruction >> 10) & 63U;
		uint64_t source = guest_register(state, rn, *guest_x18, 0) & mask;
		uint64_t value;
		unsigned int field_bits;

		if (immr >= bits || imms >= bits)
			return 0;
		if (imms >= immr) {
			field_bits = imms - immr + 1;
			value = (source >> immr) & low_mask(field_bits);
			if (opc == 0 && field_bits < bits &&
			    (value & (1ULL << (field_bits - 1))))
				value |= ~low_mask(field_bits);
		} else {
			field_bits = imms + 1;
			value = (source & low_mask(field_bits)) << (bits - immr);
			if (opc == 0 && field_bits < bits &&
			    (source & (1ULL << (field_bits - 1))))
				value |= ~(low_mask(field_bits) << (bits - immr));
		}
		if (opc == 1) {
			uint64_t destination = guest_register(state, rd,
							      *guest_x18, 0);
			uint64_t field_mask;

			if (imms >= immr)
				field_mask = low_mask(field_bits);
			else
				field_mask = low_mask(field_bits) << (bits - immr);
			value = (destination & ~field_mask) | (value & field_mask);
		} else if (opc != 0 && opc != 2) {
			return 0;
		}
		set_guest_register(state, rd, value & mask, guest_x18, 0);
		goto done;
	}

	/* ADD/SUB shifted register. */
	if ((instruction & 0x1f200000U) == 0x0b000000U) {
		unsigned int rm = (instruction >> 16) & 31U;
		unsigned int shift = (instruction >> 22) & 3U;
		unsigned int amount = (instruction >> 10) & 63U;
		int subtract = !!(instruction & (1U << 30));
		int update_flags = !!(instruction & (1U << 29));
		uint64_t left = guest_register(state, rn, *guest_x18, 0) & mask;
		uint64_t right = shifted_value(guest_register(state, rm,
			*guest_x18, 0), shift, amount, bits);
		uint64_t result = subtract ? left - right : left + right;

		if (update_flags)
			set_add_sub_flags(state, left, right, result, bits,
					  subtract);
		set_guest_register(state, rd, result & mask, guest_x18, 0);
		goto done;
	}

	/* AND/ORR/EOR/ANDS shifted register. */
	if ((instruction & 0x1f000000U) == 0x0a000000U) {
		unsigned int opc = (instruction >> 29) & 3U;
		unsigned int rm = (instruction >> 16) & 31U;
		unsigned int shift = (instruction >> 22) & 3U;
		unsigned int amount = (instruction >> 10) & 63U;
		uint64_t left = guest_register(state, rn, *guest_x18, 0) & mask;
		uint64_t right = shifted_value(guest_register(state, rm,
			*guest_x18, 0), shift, amount, bits);
		uint64_t result;

		if (instruction & (1U << 21))
			right = ~right & mask;
		if (opc == 0 || opc == 3)
			result = left & right;
		else if (opc == 1)
			result = left | right;
		else
			result = left ^ right;
		if (opc == 3)
			set_guest_flags(state, result, bits, 0, 0);
		set_guest_register(state, rd, result & mask, guest_x18, 0);
		goto done;
	}

	/* MADD/MSUB and their MUL/MNEG aliases. */
	if ((instruction & 0x7fe00000U) == 0x1b000000U) {
		unsigned int rm = (instruction >> 16) & 31U;
		unsigned int ra = (instruction >> 10) & 31U;
		uint64_t left = guest_register(state, rn, *guest_x18, 0) & mask;
		uint64_t right = guest_register(state, rm, *guest_x18, 0) & mask;
		uint64_t addend = guest_register(state, ra, *guest_x18, 0) & mask;
		uint64_t product = left * right;
		uint64_t value = instruction & (1U << 15) ?
			addend - product : addend + product;

		set_guest_register(state, rd, value & mask, guest_x18, 0);
		goto done;
	}

	/* Integer load/store with an unsigned scaled immediate. */
	if ((instruction & 0x3b000000U) == 0x39000000U) {
		unsigned int size = 1U << ((instruction >> 30) & 3U);
		unsigned int opc = (instruction >> 22) & 3U;
		uint64_t address = guest_register(state, rn, *guest_x18, 1) +
			(uint64_t)((instruction >> 10) & 0xfffU) * size;
		uint64_t value = 0;

		if (opc == 0) {
			value = guest_register(state, rd, *guest_x18, 0);
			memcpy((void *)(uintptr_t)address, &value, size);
		} else if (opc == 1) {
			memcpy(&value, (const void *)(uintptr_t)address, size);
			set_guest_register(state, rd, value, guest_x18, 0);
		} else {
			return 0;
		}
		goto done;
	}

	/* Integer load/store pair, including pre/post-index forms. */
	if ((instruction & 0x3a000000U) == 0x28000000U &&
	    !(instruction & (1U << 26))) {
		unsigned int opc = (instruction >> 30) & 3U;
		unsigned int rt2 = (instruction >> 10) & 31U;
		unsigned int mode = (instruction >> 23) & 3U;
		unsigned int size = opc == 2 ? 8U : 4U;
		int64_t offset = (int64_t)((instruction >> 15) & 0x7fU);
		uint64_t base = guest_register(state, rn, *guest_x18, 1);
		uint64_t address;
		uint64_t first = 0;
		uint64_t second = 0;

		offset = (offset << 57) >> 57;
		offset *= size;
		address = mode == 1 ? base : base + offset;
		if (instruction & (1U << 22)) {
			memcpy(&first, (const void *)(uintptr_t)address, size);
			memcpy(&second, (const void *)(uintptr_t)(address + size),
			       size);
			set_guest_register(state, rd, first, guest_x18, 0);
			set_guest_register(state, rt2, second, guest_x18, 0);
		} else {
			first = guest_register(state, rd, *guest_x18, 0);
			second = guest_register(state, rt2, *guest_x18, 0);
			memcpy((void *)(uintptr_t)address, &first, size);
			memcpy((void *)(uintptr_t)(address + size), &second, size);
		}
		if (mode == 1 || mode == 3)
			set_guest_register(state, rn, base + offset, guest_x18, 1);
		goto done;
	}

	return 0;

done:
	__darwin_arm_thread_state64_set_pc_fptr(*state,
		(void (*)(void))(uintptr_t)(pc + sizeof(uint32_t)));
	/* x18 is a Darwin platform register and must remain physical zero. */
	state->__x[18] = 0;
	return 1;
}

static int emulate_indirect_branch(arm_thread_state64_t *state,
				   uint64_t guest_x18,
				   uint32_t instruction, uint64_t pc,
				   uint64_t *target_value)
{
	unsigned int reg = (instruction >> 5) & 31U;
	uint64_t target;

	if ((instruction & 0xfffffc1fU) == 0xd61f0000U) {
		/* BR Xn */
		target = guest_register(state, reg, guest_x18, 0);
	} else if ((instruction & 0xfffffc1fU) == 0xd63f0000U) {
		/* BLR Xn */
		target = guest_register(state, reg, guest_x18, 0);
		state->__lr = pc + sizeof(uint32_t);
	} else if ((instruction & 0xfffffc1fU) == 0xd65f0000U) {
		/* RET Xn (x30 when omitted). */
		target = guest_register(state, reg, guest_x18, 0);
	} else {
		return 0;
	}
	if (darwin_uml_vm_prepare_execute(target) < 0)
		return 0;
	__darwin_arm_thread_state64_set_pc_fptr(*state,
		(void (*)(void))(uintptr_t)target);
	state->__x[18] = 0;
	*target_value = target;
	return 1;
}

static int set_thread_write_step_breakpoint(thread_act_t thread,
					    unsigned long guest_pc,
					    int enabled)
{
	arm_debug_state64_t debug_state;
	mach_msg_type_number_t count = ARM_DEBUG_STATE64_COUNT;
	kern_return_t result;
	unsigned int index = 0;

	result = thread_get_state(thread, ARM_DEBUG_STATE64,
		(thread_state_t)&debug_state, &count);
	if (result != KERN_SUCCESS)
		return -1;
	if (enabled) {
		debug_state.__bvr[index] =
			guest_pc + sizeof(uint32_t);
		debug_state.__bcr[index] =
			ARM64_USER_INSN_BREAKPOINT_CONTROL;
	} else {
		debug_state.__bcr[index] = 0;
		debug_state.__bvr[index] = 0;
	}
	result = thread_set_state(thread, ARM_DEBUG_STATE64,
		(thread_state_t)&debug_state, ARM_DEBUG_STATE64_COUNT);
	return result == KERN_SUCCESS ? 0 : -1;
}

int darwin_uml_begin_write_single_step(unsigned long address,
				       unsigned long guest_pc)
{
	struct guest_context *context = signal_context;
	int result;

	if (context == NULL || context->write_step_active)
		return -1;
	result = darwin_uml_vm_begin_write_step(address);
	if (result <= 0)
		return result;
	if (set_thread_write_step_breakpoint(context->thread, guest_pc, 1) < 0) {
		(void)darwin_uml_vm_finish_write_step(address);
		return -1;
	}
	context->write_step_address = address;
	context->write_step_active = 1;
	return 1;
}

static int finish_write_single_step(struct guest_context *context)
{
	int debug_result;
	int vm_result;

	debug_result = set_thread_write_step_breakpoint(context->thread, 0, 0);
	vm_result = darwin_uml_vm_finish_write_step(
		context->write_step_address);
	context->write_step_active = 0;
	context->write_step_address = 0;
	return debug_result < 0 || vm_result < 0 ? -1 : 0;
}

kern_return_t catch_mach_exception_raise(mach_port_t port,
		mach_port_t thread, mach_port_t task, exception_type_t exception,
		mach_exception_data_t code, mach_msg_type_number_t code_count)
{
	(void)port;
	(void)thread;
	(void)task;
	(void)exception;
	(void)code;
	(void)code_count;
	return KERN_FAILURE;
}

kern_return_t catch_mach_exception_raise_state(mach_port_t port,
		exception_type_t exception, const mach_exception_data_t code,
		mach_msg_type_number_t code_count, int *flavor,
		const thread_state_t old_state,
		mach_msg_type_number_t old_state_count, thread_state_t new_state,
		mach_msg_type_number_t *new_state_count)
{
	(void)port;
	(void)exception;
	(void)code;
	(void)code_count;
	(void)flavor;
	(void)old_state;
	(void)old_state_count;
	(void)new_state;
	(void)new_state_count;
	return KERN_FAILURE;
}

kern_return_t catch_mach_exception_raise_state_identity(mach_port_t port,
		mach_port_t thread, mach_port_t task, exception_type_t exception,
		mach_exception_data_t code, mach_msg_type_number_t code_count,
		int *flavor, thread_state_t old_state,
		mach_msg_type_number_t old_state_count, thread_state_t new_state,
		mach_msg_type_number_t *new_state_count)
{
	arm_thread_state64_t *old_arm = (arm_thread_state64_t *)old_state;
	arm_thread_state64_t *new_arm = (arm_thread_state64_t *)new_state;
	struct guest_context *context;
	uintptr_t pc;

	(void)port;
	(void)task;
	if (*flavor != ARM_THREAD_STATE64 ||
	    old_state_count != ARM_THREAD_STATE64_COUNT)
		return KERN_FAILURE;

	pthread_mutex_lock(&contexts_lock);
	context = find_context(thread);
	if (context == NULL || context->registers == NULL) {
		/* Kernel-side UML faults are intentionally handled through its
		 * POSIX SIGSEGV path.  Decline them here so Mach/BSD continues the
		 * normal exception-to-signal chain.  The launcher signal handler
		 * remains the final guard for a genuine host crash.
		 */
		pthread_mutex_unlock(&contexts_lock);
		return KERN_FAILURE;
	}
	context->guest_x18 = context->direct.guest_x18;
	pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(*old_arm);
	if (exception == EXC_BREAKPOINT && context->write_step_active) {
		if (finish_write_single_step(context) < 0) {
			pthread_mutex_unlock(&contexts_lock);
			return KERN_FAILURE;
		}
		memcpy(new_arm, old_arm, sizeof(*new_arm));
		*new_state_count = ARM_THREAD_STATE64_COUNT;
		pthread_mutex_unlock(&contexts_lock);
		return KERN_SUCCESS;
	}
	if (exception == EXC_BAD_ACCESS) {
		unsigned long guest_site;
		unsigned int scratch;
		unsigned int gateway_instruction;
		int gateway_kind;

		gateway_kind = darwin_uml_vm_gateway_x18_fault(
			pc, &gateway_instruction);
		if (gateway_kind > 0) {
			if (!emulate_gateway_x18_access(context, old_arm, pc,
						 gateway_instruction,
						 gateway_kind == 2)) {
				pthread_mutex_unlock(&contexts_lock);
				return KERN_FAILURE;
			}
			memcpy(new_arm, old_arm, sizeof(*new_arm));
			*new_state_count = ARM_THREAD_STATE64_COUNT;
			pthread_mutex_unlock(&contexts_lock);
			return KERN_SUCCESS;
		}

		/* The native instruction in an x18 gateway is a relocated guest
		 * instruction.  Attribute its data fault to the original guest PC,
		 * and restore the guest scratch register which the gateway borrowed,
		 * before Linux observes or retries the fault.
		 */
		if (darwin_uml_vm_x18_stub_fault(pc, &guest_site, &scratch) > 0) {
			if (scratch != 0)
				old_arm->__x[scratch] =
					context->direct.scratch_x16;
			pc = (uintptr_t)guest_site;
			__darwin_arm_thread_state64_set_pc_fptr(*old_arm,
				(void (*)(void))pc);
		}
	}
	if (exception == EXC_BREAKPOINT && context->entering &&
	    *(const uint32_t *)pc == UML_ENTER_BRK) {
		/* Returning a complete Mach state is the only AArch64 userspace
		 * transfer which preserves x0-x30 and the destination PC.  It also
		 * avoids borrowing Linux's x16 or x18 as an indirect-branch scratch
		 * register.
		 */
		memcpy(new_arm, &context->registers->state, sizeof(*new_arm));
		new_arm->__x[18] = (uintptr_t)&context->direct;
		*new_state_count = ARM_THREAD_STATE64_COUNT;
		context->entering = 0;
		pthread_mutex_unlock(&contexts_lock);
		return KERN_SUCCESS;
	}
	if (exception == EXC_BREAKPOINT &&
	    *(const uint32_t *)pc == UML_DIRECT_RESUME_BRK) {
		/* A syscall changed PC (exec, signal delivery, or restart).  The
		 * direct coroutine has already restored guest FP/TLS state; install
		 * the kernel's complete integer state atomically and abandon its
		 * static per-site return thunk.
		 */
		memcpy(new_arm, &context->registers->state, sizeof(*new_arm));
		new_arm->__x[18] = (uintptr_t)&context->direct;
		*new_state_count = ARM_THREAD_STATE64_COUNT;
		pthread_mutex_unlock(&contexts_lock);
		return KERN_SUCCESS;
	}
	memcpy(&context->registers->state, old_arm, sizeof(*old_arm));
	context->event = UML_EVENT_FATAL;

	if (exception == EXC_BREAKPOINT) {
		uint32_t instruction = *(const uint32_t *)pc;
		uint32_t original_instruction;
		unsigned long indirect_site;
		uint64_t indirect_target;

		if (instruction == UML_INDIRECT_BRK &&
		    darwin_uml_vm_indirect_instruction(
			pc, &original_instruction, &indirect_site) > 0 &&
		    emulate_indirect_branch(old_arm, context->guest_x18,
					    original_instruction, indirect_site,
					    &indirect_target)) {
			(void)darwin_uml_vm_cache_indirect(indirect_site,
				original_instruction, indirect_target);
			old_arm->__x[18] = (uintptr_t)&context->direct;
			memcpy(new_arm, old_arm, sizeof(*new_arm));
			*new_state_count = ARM_THREAD_STATE64_COUNT;
			pthread_mutex_unlock(&contexts_lock);
			return KERN_SUCCESS;
		} else if (instruction == UML_X18_BRK &&
		    darwin_uml_vm_x18_instruction(
			pc, &original_instruction) > 0) {
			arm_thread_state64_t before = *old_arm;
			uint64_t x18_before = context->guest_x18;

			profile_x18_opcode(original_instruction);
			if (emulate_x18_instruction(old_arm, &context->guest_x18,
						    original_instruction, pc)) {
				record_x18_trace(context, &before, old_arm,
						 x18_before, original_instruction, pc);
				context->direct.guest_x18 = context->guest_x18;
				old_arm->__x[18] = (uintptr_t)&context->direct;
				memcpy(new_arm, old_arm, sizeof(*new_arm));
				*new_state_count = ARM_THREAD_STATE64_COUNT;
				pthread_mutex_unlock(&contexts_lock);
				return KERN_SUCCESS;
			}
			fprintf(stderr,
				"Darwin: unsupported guest x18 instruction pc=%p original=%#x\n",
				(void *)pc, original_instruction);
		} else if (instruction == UML_SYSCALL_BRK) {
			int syscall_status = darwin_uml_vm_validate_syscall(pc);

			if (syscall_status == 0) {
				old_arm->__x[18] = 0;
				memcpy(new_arm, old_arm, sizeof(*new_arm));
				*new_state_count = ARM_THREAD_STATE64_COUNT;
				pthread_mutex_unlock(&contexts_lock);
				return KERN_SUCCESS;
			}
			if (syscall_status < 0)
				goto return_to_kernel;
			__darwin_arm_thread_state64_set_pc_fptr(*old_arm,
				(void (*)(void))(pc + sizeof(uint32_t)));
			memcpy(&context->registers->state, old_arm,
			       sizeof(*old_arm));
			context->registers->state.__x[18] = context->guest_x18;
			context->event = UML_EVENT_SYSCALL;
		} else if (instruction >= UML_TLS_WRITE_BRK_BASE &&
			   instruction <= UML_TLS_WRITE_BRK_BASE +
				UML_TLS_BRK_SPAN &&
			   ((instruction - UML_TLS_WRITE_BRK_BASE) & 31U) == 0) {
			unsigned int reg =
				(instruction - UML_TLS_WRITE_BRK_BASE) >> 5;

			context->guest_tls = reg == 31 ? 0 :
				guest_register(old_arm, reg, context->guest_x18, 0);
			context->direct.guest_tls = context->guest_tls;
			__darwin_arm_thread_state64_set_pc_fptr(*old_arm,
				(void (*)(void))(pc + sizeof(uint32_t)));
			old_arm->__x[18] = (uintptr_t)&context->direct;
			memcpy(new_arm, old_arm, sizeof(*new_arm));
			*new_state_count = ARM_THREAD_STATE64_COUNT;
			pthread_mutex_unlock(&contexts_lock);
			return KERN_SUCCESS;
		} else if (instruction >= UML_TLS_READ_BRK_BASE &&
			   instruction <= UML_TLS_READ_BRK_BASE +
				UML_TLS_BRK_SPAN &&
			   ((instruction - UML_TLS_READ_BRK_BASE) & 31U) == 0) {
			unsigned int reg =
				(instruction - UML_TLS_READ_BRK_BASE) >> 5;

			if (reg != 31)
				set_guest_register(old_arm, reg, context->guest_tls,
						   &context->guest_x18, 0);
			__darwin_arm_thread_state64_set_pc_fptr(*old_arm,
				(void (*)(void))(pc + sizeof(uint32_t)));
			old_arm->__x[18] = (uintptr_t)&context->direct;
			memcpy(new_arm, old_arm, sizeof(*new_arm));
			*new_state_count = ARM_THREAD_STATE64_COUNT;
			pthread_mutex_unlock(&contexts_lock);
			return KERN_SUCCESS;
		}
	} else if (exception == EXC_BAD_INSTRUCTION) {
		uint32_t instruction;

		memcpy(&instruction, (const void *)pc, sizeof(instruction));
		/*
		 * An executable sibling can expose an absent Linux 4 KiB page as
		 * the zero-filled quarter of a 16 KiB Darwin slot.  ARM reports
		 * the first zero word as an undefined instruction instead of a VM
		 * fault.  Turn only that untracked-quarter case back into the Linux
		 * instruction page fault which the host granule suppressed.
		 */
		if (instruction == 0 && !darwin_uml_vm_has_mapping(pc)) {
			context->registers->fault.error_code = 0;
			context->registers->fault.address = pc;
			context->registers->fault.trap_number = UML_FAULT_PAGE;
			context->event = UML_EVENT_FAULT;
		}
	} else if (exception == EXC_BAD_ACCESS) {
		arm_exception_state64_t exception_state;
		mach_msg_type_number_t count = ARM_EXCEPTION_STATE64_COUNT;
		kern_return_t result;
		uint64_t fault_address;
		uint32_t syndrome;
		unsigned int exception_class;
		int is_instruction;
		int is_write;
		int resolved;

		if (pc == UML_SIGRETURN_TRAMPOLINE) {
			old_arm->__x[8] = UML_NR_RT_SIGRETURN;
			__darwin_arm_thread_state64_set_pc_fptr(*old_arm,
				(void (*)(void))(pc + sizeof(uint32_t)));
			memcpy(&context->registers->state, old_arm,
			       sizeof(*old_arm));
			context->registers->state.__x[18] = context->guest_x18;
			context->event = UML_EVENT_SYSCALL;
			goto return_to_kernel;
		}

		memset(&exception_state, 0, sizeof(exception_state));
		if (signal_exception_state != NULL) {
			exception_state = *signal_exception_state;
			result = KERN_SUCCESS;
		} else {
			result = thread_get_state(thread, ARM_EXCEPTION_STATE64,
					  (thread_state_t)&exception_state, &count);
		}
		syndrome = result == KERN_SUCCESS ? exception_state.__esr : 0;
		fault_address = result == KERN_SUCCESS ? exception_state.__far :
			(code_count > 1 ? (uint64_t)code[1] : 0);
		exception_class = syndrome >> 26;
		is_instruction = exception_class == 0x20 ||
			exception_class == 0x21;
		is_write = !is_instruction && (syndrome & (1U << 6));
		resolved = !is_instruction ?
			darwin_uml_vm_patch_x18_fault(pc) : 0;
		if (resolved == 0)
			resolved = darwin_uml_vm_resolve_fault(fault_address,
							 is_instruction,
							 is_write);
		if (resolved > 0) {
			if (is_instruction)
				profiled_resolved_execute++;
			else if (is_write)
				profiled_resolved_write++;
			else
				profiled_resolved_read++;
			memcpy(new_arm, old_arm, sizeof(*new_arm));
			*new_state_count = ARM_THREAD_STATE64_COUNT;
			pthread_mutex_unlock(&contexts_lock);
			return KERN_SUCCESS;
		}
		if (is_instruction)
			profiled_guest_execute_faults++;
		else if (is_write)
			profiled_guest_write_faults++;
		else
			profiled_guest_read_faults++;
		context->registers->fault.error_code =
			is_write ? UML_FAULT_WRITE : 0;
		context->registers->fault.address = fault_address;
		context->registers->fault.trap_number = UML_FAULT_PAGE;
		context->event = UML_EVENT_FAULT;
	}
	context->registers->state.__x[18] = context->guest_x18;
	if (context->event == UML_EVENT_FATAL &&
	    fatal_exception_diagnostics++ < 16) {
		uint32_t instruction = 0;

		if (exception != EXC_BAD_ACCESS)
			memcpy(&instruction, (const void *)pc, sizeof(instruction));
		fprintf(stderr,
			"Darwin: unhandled guest exception type=%d pc=%p "
			"instruction=%#x code_count=%u code0=%#llx code1=%#llx\n",
			exception, (void *)pc, instruction, code_count,
			code_count > 0 ? (unsigned long long)code[0] : 0ULL,
			code_count > 1 ? (unsigned long long)code[1] : 0ULL);
	}
return_to_kernel:
	memcpy(new_arm, old_arm, sizeof(*new_arm));
	new_arm->__x[0] = (uintptr_t)context;
	new_arm->__x[1] = context->host_tls;
	new_arm->__x[2] = (uintptr_t)context->fp_registers;
	new_arm->__x[3] = (uintptr_t)&context->host_fp_registers;
	new_arm->__x[18] = context->host_x18;
	__darwin_arm_thread_state64_set_sp(*new_arm,
		(uintptr_t)context->bridge_stack + BRIDGE_STACK_SIZE - 16);
	__darwin_arm_thread_state64_set_pc_fptr(*new_arm,
		darwin_uml_return_trampoline);
	*new_state_count = ARM_THREAD_STATE64_COUNT;
	pthread_mutex_unlock(&contexts_lock);
	return KERN_SUCCESS;
}

static void chain_guest_signal(int signum, siginfo_t *info, void *pointer)
{
	struct sigaction *previous = &previous_guest_actions[signum];

	if (previous->sa_flags & SA_SIGINFO) {
		previous->sa_sigaction(signum, info, pointer);
		return;
	}
	if (previous->sa_handler == SIG_IGN)
		return;
	if (previous->sa_handler != SIG_DFL && previous->sa_handler != NULL) {
		previous->sa_handler(signum);
		return;
	}
	signal(signum, SIG_DFL);
	raise(signum);
}

/* Return 1 when the host shadow fault was resolved in place, and 0 when a
 * Linux fault/syscall event has been prepared for the UML kernel.  The POSIX
 * signal handler already owns the exact guest state, so access faults do not
 * need the generic Mach-shaped dispatcher or its global context lookup.
 */
static int handle_guest_access_signal(struct guest_context *context,
		ucontext_t *ucontext, siginfo_t *info)
{
	arm_thread_state64_t *state = &ucontext->uc_mcontext->__ss;
	const arm_exception_state64_t *exception_state =
		&ucontext->uc_mcontext->__es;
	uintptr_t pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(*state);
	uint64_t fault_address = exception_state->__far;
	uint32_t syndrome = exception_state->__esr;
	unsigned int exception_class = syndrome >> 26;
	int is_instruction = exception_class == 0x20 || exception_class == 0x21;
	int is_write = !is_instruction && (syndrome & (1U << 6));
	int resolved;
	unsigned long guest_site;
	unsigned int scratch;
	unsigned int gateway_instruction;
	int gateway_kind;

	context->guest_x18 = context->direct.guest_x18;
	if (fault_address == 0 && info != NULL && info->si_addr != NULL)
		fault_address = (uint64_t)(uintptr_t)info->si_addr;
	gateway_kind = darwin_uml_vm_gateway_x18_fault(pc,
		&gateway_instruction);
	if (gateway_kind > 0) {
		return emulate_gateway_x18_access(context, state, pc,
						   gateway_instruction,
						   gateway_kind == 2);
	}
	if (darwin_uml_vm_x18_stub_fault(pc, &guest_site, &scratch) > 0) {
		if (scratch != 0)
			state->__x[scratch] = context->direct.scratch_x16;
		pc = (uintptr_t)guest_site;
		__darwin_arm_thread_state64_set_pc_fptr(*state,
			(void (*)(void))pc);
	}
	if (pc == UML_SIGRETURN_TRAMPOLINE) {
		state->__x[8] = UML_NR_RT_SIGRETURN;
		__darwin_arm_thread_state64_set_pc_fptr(*state,
			(void (*)(void))(pc + sizeof(uint32_t)));
		memcpy(&context->registers->state, state, sizeof(*state));
		context->registers->state.__x[18] = context->guest_x18;
		context->event = UML_EVENT_SYSCALL;
		return 0;
	}
	resolved = !is_instruction ? darwin_uml_vm_patch_x18_fault(pc) : 0;
	if (resolved == 0)
		resolved = darwin_uml_vm_resolve_fault(fault_address,
			is_instruction, is_write);
	if (resolved > 0) {
		if (exception_profile_enabled) {
			if (is_instruction)
				profiled_resolved_execute++;
			else if (is_write)
				profiled_resolved_write++;
			else
				profiled_resolved_read++;
		}
		return 1;
	}
	if (exception_profile_enabled) {
		if (is_instruction)
			profiled_guest_execute_faults++;
		else if (is_write)
			profiled_guest_write_faults++;
		else
			profiled_guest_read_faults++;
	}
	memcpy(&context->registers->state, state, sizeof(*state));
	context->registers->state.__x[18] = context->guest_x18;
	context->registers->fault.error_code = is_write ? UML_FAULT_WRITE : 0;
	context->registers->fault.address = fault_address;
	context->registers->fault.trap_number = UML_FAULT_PAGE;
	context->event = resolved < 0 ? UML_EVENT_FATAL : UML_EVENT_FAULT;
	return 0;
}

static void guest_exception_signal(int signal, siginfo_t *info, void *pointer)
{
	struct guest_context *context = signal_context;
	ucontext_t *ucontext = pointer;
	arm_thread_state64_t new_state;
	arm_thread_state64_t *signal_state;
	mach_exception_data_type_t code[2] = { 0, 0 };
	mach_msg_type_number_t new_count = ARM_THREAD_STATE64_COUNT;
	exception_type_t exception;
	int flavor = ARM_THREAD_STATE64;
	kern_return_t result;

	if (context == NULL || context->registers == NULL || ucontext == NULL ||
	    ucontext->uc_mcontext == NULL) {
		chain_guest_signal(signal, info, pointer);
		return;
	}
	context->guest_tls = context->direct.guest_tls;
	context->guest_x18 = context->direct.guest_x18;
	signal_state = &ucontext->uc_mcontext->__ss;
	if (signal == SIGTRAP && !context->write_step_active) {
		uintptr_t pc = (uintptr_t)
			__darwin_arm_thread_state64_get_pc(*signal_state);
		uint32_t instruction = *(const uint32_t *)pc;

		if (exception_profile_enabled) {
			profiled_traps++;
			if (instruction == UML_SYSCALL_BRK)
				profiled_trap_syscalls++;
			else if (instruction == UML_INDIRECT_BRK)
				profiled_trap_indirect++;
			else if (instruction == UML_X18_BRK)
				profiled_trap_x18++;
			else if ((instruction >= UML_TLS_WRITE_BRK_BASE &&
				  instruction <= UML_TLS_WRITE_BRK_BASE +
					UML_TLS_BRK_SPAN) ||
				 (instruction >= UML_TLS_READ_BRK_BASE &&
				  instruction <= UML_TLS_READ_BRK_BASE +
					UML_TLS_BRK_SPAN))
				profiled_trap_tls++;
			else
				profiled_trap_other++;
		}

		if (instruction == UML_INDIRECT_BRK) {
			uint32_t original_instruction;
			unsigned long indirect_site;
			uint64_t indirect_target;

			if (darwin_uml_vm_indirect_instruction(pc,
				    &original_instruction, &indirect_site) > 0 &&
			    emulate_indirect_branch(signal_state,
				    context->guest_x18, original_instruction,
				    indirect_site, &indirect_target)) {
				(void)darwin_uml_vm_cache_indirect(indirect_site,
					original_instruction, indirect_target);
				signal_state->__x[18] =
					(uintptr_t)&context->direct;
				return;
			}
		}

		/* Syscalls are by far the hottest exception class.  Handle their
		 * already-validated BRK directly on the owning Darwin thread instead
		 * of routing through the generic Mach-shaped dispatcher and its global
		 * context lock.  Faults and emulated instructions retain the common
		 * path below.
		 */
		if (instruction == UML_SYSCALL_BRK) {
			int syscall_status = darwin_uml_vm_validate_syscall(pc);

			if (syscall_status == 0) {
				signal_state->__x[18] = 0;
				return;
			}
			if (syscall_status < 0) {
				context->event = UML_EVENT_FATAL;
				goto suspend_for_kernel;
			}
			if (darwin_uml_vm_promote_syscall(pc) < 0) {
				context->event = UML_EVENT_FATAL;
				goto suspend_for_kernel;
			}
			__darwin_arm_thread_state64_set_pc_fptr(*signal_state,
				(void (*)(void))(pc + sizeof(uint32_t)));
			memcpy(&context->registers->state, signal_state,
			       sizeof(*signal_state));
			context->registers->state.__x[18] = context->guest_x18;
			context->event = UML_EVENT_SYSCALL;
			goto suspend_for_kernel;
		}
	}
	if (signal == SIGBUS || signal == SIGSEGV) {
		if (exception_profile_enabled)
			profiled_bad_accesses++;
		if (handle_guest_access_signal(context, ucontext, info) > 0)
			return;
		goto suspend_for_kernel;
	}
	if (signal == SIGILL)
		profiled_bad_instructions++;
	switch (signal) {
	case SIGTRAP:
		exception = EXC_BREAKPOINT;
		break;
	case SIGILL:
		exception = EXC_BAD_INSTRUCTION;
		break;
	case SIGBUS:
	case SIGSEGV:
		exception = EXC_BAD_ACCESS;
		code[1] = (mach_exception_data_type_t)(uintptr_t)info->si_addr;
		break;
	default:
		chain_guest_signal(signal, info, pointer);
		return;
	}
	signal_exception_state = &ucontext->uc_mcontext->__es;
	result = catch_mach_exception_raise_state_identity(0, context->thread,
		mach_task_self(), exception, code, 2, &flavor,
		(thread_state_t)&ucontext->uc_mcontext->__ss,
		ARM_THREAD_STATE64_COUNT, (thread_state_t)&new_state,
		&new_count);
	signal_exception_state = NULL;
	if (result != KERN_SUCCESS || new_count != ARM_THREAD_STATE64_COUNT) {
		chain_guest_signal(signal, info, pointer);
		return;
	}
	ucontext->uc_mcontext->__ss = new_state;
	if ((uintptr_t)__darwin_arm_thread_state64_get_pc(new_state) !=
	    (uintptr_t)darwin_uml_return_trampoline)
		return;
suspend_for_kernel:
	/* Keep the native signal frame suspended while the UML kernel services
	 * this event.  The next entry resumes this exact frame, whose sigreturn
	 * restores the complete guest state without taking a second entry trap.
	 */
	memcpy(context->fp_registers, &ucontext->uc_mcontext->__ns,
	       sizeof(*context->fp_registers));
	darwin_uml_restore_fp(&context->host_fp_registers);
	/* A handler's signal is blocked until sigreturn.  The UML kernel may
	 * create a Darwin thread while this frame is suspended; expose the normal
	 * unblocked mask first so that child can receive its initial guest trap.
	 */
	(void)sigprocmask(SIG_UNBLOCK, &guest_signal_mask, NULL);
	context->signal_suspended = 1;
	darwin_uml_context_switch(&context->signal_switch,
				  &context->kernel_switch);
	context->signal_suspended = 0;
	ucontext->uc_mcontext->__ss = context->registers->state;
	ucontext->uc_mcontext->__ss.__x[18] = (uintptr_t)&context->direct;
	memcpy(&ucontext->uc_mcontext->__ns, context->fp_registers,
	       sizeof(*context->fp_registers));
}

static void install_guest_signals(void)
{
	static const int signals[] = { SIGTRAP, SIGILL, SIGBUS, SIGSEGV };
	struct sigaction action = { };
	size_t index;

	action.sa_sigaction = guest_exception_signal;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	sigemptyset(&guest_signal_mask);
	for (index = 0; index < sizeof(signals) / sizeof(signals[0]); index++)
		sigaddset(&guest_signal_mask, signals[index]);
	for (index = 0; index < sizeof(signals) / sizeof(signals[0]); index++)
		if (sigaction(signals[index], &action,
			      &previous_guest_actions[signals[index]]) < 0)
			abort();
}

static void destroy_guest_context(void *pointer)
{
	struct guest_context *context = pointer;
	struct guest_context **link;

	if (context == NULL)
		return;
	pthread_mutex_lock(&contexts_lock);
	for (link = &contexts; *link != NULL; link = &(*link)->next) {
		if (*link != context)
			continue;
		*link = context->next;
		break;
	}
	if (darwin_uml_active_direct_gateway == &context->direct)
		darwin_uml_active_direct_gateway = NULL;
	profiled_contexts_destroyed++;
	if (profiled_contexts_live != 0)
		profiled_contexts_live--;
	pthread_mutex_unlock(&contexts_lock);
	if (signal_context == context)
		signal_context = NULL;
	if (context->thread != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), context->thread);
	if (context->bridge_stack != NULL &&
	    context->bridge_stack != MAP_FAILED)
		(void)munmap(context->bridge_stack, BRIDGE_STACK_SIZE);
	free(context);
}

static void initialize_exceptions(void)
{
	if (pthread_key_create(&context_key, destroy_guest_context) != 0)
		abort();
	if (getenv("UML_PROFILE_TRAPS") != NULL) {
		exception_profile_enabled = 1;
		atexit(report_exception_profile);
	}
}

void darwin_uml_runtime_init(void)
{
	pthread_once(&exception_once, initialize_exceptions);
}

static struct guest_context *current_context(void)
{
	struct guest_context *context = pthread_getspecific(context_key);

	if (context != NULL)
		return context;
	context = calloc(1, sizeof(*context));
	if (context == NULL)
		return NULL;
	context->bridge_stack = mmap(NULL, BRIDGE_STACK_SIZE,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (context->bridge_stack == MAP_FAILED) {
		free(context);
		return NULL;
	}
	{
		stack_t signal_stack = { };

		signal_stack.ss_sp = context->bridge_stack;
		signal_stack.ss_size = BRIDGE_STACK_SIZE;
		if (sigaltstack(&signal_stack, NULL) < 0) {
			munmap(context->bridge_stack, BRIDGE_STACK_SIZE);
			free(context);
			return NULL;
		}
	}
	context->host_tls = read_hardware_tls();
	__asm__ volatile("mov %0, x18" : "=r"(context->host_x18));
	context->direct.host_fp_registers = &context->host_fp_registers;
	context->direct.kernel_switch = &context->kernel_switch;
	context->direct.direct_switch = &context->direct_switch;
	context->direct.host_tls = context->host_tls;
	context->direct.host_x18 = context->host_x18;
	context->thread = mach_thread_self();
	pthread_mutex_lock(&contexts_lock);
	context->next = contexts;
	contexts = context;
	profiled_contexts_created++;
	profiled_contexts_live++;
	pthread_mutex_unlock(&contexts_lock);
	pthread_setspecific(context_key, context);
	signal_context = context;
	pthread_once(&guest_signals_once, install_guest_signals);
	return context;
}

int darwin_uml_enter_guest(void *registers, void *fp_registers,
			   unsigned long fp_size, unsigned long generation,
			   unsigned long *tls)
{
	struct guest_context *context;

	(void)generation;
	darwin_uml_runtime_init();
	context = current_context();
	if (context == NULL || fp_size != sizeof(arm_neon_state64_t))
		return UML_EVENT_FATAL;
	context->registers = registers;
	context->fp_registers = fp_registers;
	context->direct.registers = registers;
	context->direct.fp_registers = fp_registers;
	if (!context->guest_x18_initialized) {
		context->guest_x18 = context->registers->state.__x[18];
		context->guest_x18_initialized = 1;
	} else if (context->signal_suspended || context->direct.suspended) {
		/* The UML kernel may replace the saved state while servicing an
		 * event (execve and signal delivery are the important examples).
		 */
		context->guest_x18 = context->registers->state.__x[18];
	}
	context->registers->state.__x[18] = context->guest_x18;
	context->event = UML_EVENT_FATAL;
	context->guest_tls = *tls;
	context->direct.event = UML_EVENT_FATAL;
	context->direct.guest_tls = context->guest_tls;
	context->direct.guest_x18 = context->guest_x18;
	darwin_uml_active_direct_gateway = &context->direct;
	if (darwin_uml_vm_prepare_execute(
			__darwin_arm_thread_state64_get_pc(context->registers->state)) < 0) {
		fprintf(stderr, "Darwin: failed to prepare guest code at pc=%p\n",
			(void *)(uintptr_t)__darwin_arm_thread_state64_get_pc(
				context->registers->state));
		context->registers = NULL;
		return UML_EVENT_FATAL;
	}
	if (!context->guest_started) {
		context->guest_started = 1;
		context->entering = 1;
		darwin_uml_arch_enter_saved(&context->kernel_switch,
			&context->registers->state, context->guest_tls,
			context->fp_registers, &context->host_fp_registers);
	} else if (context->signal_suspended) {
		darwin_uml_save_fp(&context->host_fp_registers);
		darwin_uml_context_switch(&context->kernel_switch,
					  &context->signal_switch);
	} else if (context->direct.suspended) {
		darwin_uml_save_fp(&context->host_fp_registers);
		darwin_uml_context_switch(&context->kernel_switch,
					  &context->direct_switch);
	} else {
		context->event = UML_EVENT_FATAL;
	}
	/* An indirect-cache miss already captured the architectural target and,
	 * for BLR, the link register in the direct gateway snapshot.  Grow the
	 * generic per-site cache while the guest is suspended, then resume the
	 * coroutine immediately instead of taking a Mach exception or entering
	 * the Linux trap path.
	 */
	while (context->direct.suspended &&
	       context->direct.event == UML_EVENT_INDIRECT) {
		int cache_result = darwin_uml_vm_cache_direct_indirect(
			context->direct.resume_stub, context->direct.guest_pc);

		profiled_direct_indirects++;
		if (cache_result < 0) {
			context->direct.event = UML_EVENT_FATAL;
			break;
		}
		darwin_uml_save_fp(&context->host_fp_registers);
		darwin_uml_context_switch(&context->kernel_switch,
					  &context->direct_switch);
	}
	if (context->direct.suspended) {
		if (context->direct.event == UML_EVENT_SYSCALL) {
			unsigned long syscall =
				context->registers->state.__x[8];

			profiled_direct_syscalls++;
			if (exception_profile_enabled &&
			    syscall < sizeof(profiled_syscalls) /
				     sizeof(profiled_syscalls[0]))
				profiled_syscalls[syscall]++;
		}
		context->event = context->direct.event;
		context->guest_tls = context->direct.guest_tls;
		context->guest_x18 = context->direct.guest_x18;
	}
	*tls = context->guest_tls;
	context->entering = 0;
	context->registers = NULL;
	context->fp_registers = NULL;
	return context->event;
}
