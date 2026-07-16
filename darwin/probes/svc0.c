/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Verify how Darwin reports the AArch64 Linux syscall instruction (svc #0).
 * The Darwin UML backend uses the resulting synchronous signal as its
 * userspace-to-kernel transition.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t caught_signal;

static void catch_svc(int signal, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;
	static const char message[] = "caught svc #0\n";

	(void)info;
	caught_signal = signal;
	uc->uc_mcontext->__ss.__pc += sizeof(uint32_t);
	(void)write(STDOUT_FILENO, message, sizeof(message) - 1);
}

int main(void)
{
	static const int signals[] = { SIGILL, SIGSYS, SIGTRAP, SIGSEGV, SIGBUS };
	struct sigaction action;
	register uint64_t syscall_number __asm("x8") = 172; /* Linux getpid */
	size_t i;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = catch_svc;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		if (sigaction(signals[i], &action, NULL) != 0) {
			perror("sigaction");
			return 2;
		}
	}

	__asm__ volatile("svc #0" : "+r"(syscall_number) : : "memory");
	printf("signal=%d (%s); svc #0 was consumed by XNU\n",
	       caught_signal, strsignal(caught_signal));
	return caught_signal != 0;
}
