/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Prove that a task-local Mach exception port can intercept the breakpoint
 * used by the Darwin backend in place of an AArch64 Linux svc #0 without
 * creating a second Darwin process.
 */
#include <mach/arm/thread_status.h>
#include <mach/exception_types.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mig.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern boolean_t mach_exc_server(mach_msg_header_t *, mach_msg_header_t *);

static mach_port_t exception_port;
static volatile uint64_t observed_syscall;

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

	(void)port;
	(void)thread;
	(void)task;
	(void)code;
	(void)code_count;
	if (exception != EXC_BREAKPOINT || *flavor != ARM_THREAD_STATE64 ||
	    old_state_count != ARM_THREAD_STATE64_COUNT)
		return KERN_FAILURE;

	memcpy(new_arm, old_arm, sizeof(*new_arm));
	*new_state_count = ARM_THREAD_STATE64_COUNT;
	observed_syscall = old_arm->__x[8];
	new_arm->__pc += sizeof(uint32_t);
	new_arm->__x[0] = UINT64_C(0x554d4c444152574e); /* "UMLDARWN" */
	return KERN_SUCCESS;
}

static void *exception_server(void *unused)
{
	kern_return_t kr;

	(void)unused;
	kr = mach_msg_server(mach_exc_server, 16384, exception_port, 0);
	fprintf(stderr, "mach_msg_server: %s\n", mach_error_string(kr));
	return NULL;
}

static void check_mach(kern_return_t kr, const char *operation)
{
	if (kr == KERN_SUCCESS)
		return;
	fprintf(stderr, "%s: %s\n", operation, mach_error_string(kr));
	exit(2);
}

int main(void)
{
	register uint64_t x0 __asm("x0") = UINT64_C(0x1111111111111111);
	register uint64_t x8 __asm("x8") = 172; /* Linux getpid */
	pthread_t server;

	check_mach(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				       &exception_port), "mach_port_allocate");
	check_mach(mach_port_insert_right(mach_task_self(), exception_port,
			exception_port, MACH_MSG_TYPE_MAKE_SEND),
		   "mach_port_insert_right");
	if (pthread_create(&server, NULL, exception_server, NULL) != 0) {
		perror("pthread_create");
		return 2;
	}
	check_mach(task_set_exception_ports(mach_task_self(), EXC_MASK_BREAKPOINT,
			exception_port,
			EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES,
			ARM_THREAD_STATE64), "task_set_exception_ports");

	__asm__ volatile("brk #0x4c4" : "+r"(x0), "+r"(x8) : : "memory");
	printf("exception=%s syscall=%llu return=%#llx\n",
	       observed_syscall == 172 ? "EXC_BREAKPOINT" : "missing",
	       observed_syscall, x0);
	return observed_syscall != 172 || x0 != UINT64_C(0x554d4c444152574e);
}
