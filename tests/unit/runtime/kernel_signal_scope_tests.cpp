// SPDX-License-Identifier: GPL-2.0-or-later
#include "kernel_signal_scope.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <pthread.h>
#include <stdexcept>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

std::atomic_int kernel_deliveries;
std::atomic_int host_deliveries;

void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error{message};
}

/* Stands in for a handler inside the embedded kernel image. The scopes below
 * are given an image range that covers exactly this function. */
void kernel_handler(int, siginfo_t *, void *)
{
	kernel_deliveries.fetch_add(1, std::memory_order_relaxed);
}

/* A handler the kernel installs from outside its image. */
void foreign_handler(int)
{
}

void host_handler(int)
{
	host_deliveries.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] std::uintptr_t image_begin() noexcept
{
	return reinterpret_cast<std::uintptr_t>(&kernel_handler);
}

[[nodiscard]] std::uintptr_t image_end() noexcept
{
	return image_begin() + 1U;
}

[[nodiscard]] std::uintptr_t handler_of(int signal)
{
	struct sigaction action {};
	require(sigaction(signal, nullptr, &action) == 0,
		"cannot read a signal disposition");
	if ((action.sa_flags & SA_SIGINFO) != 0)
		return reinterpret_cast<std::uintptr_t>(action.sa_sigaction);
	return reinterpret_cast<std::uintptr_t>(action.sa_handler);
}

void install(int signal, void (*handler)(int))
{
	struct sigaction action {};
	action.sa_handler = handler;
	sigemptyset(&action.sa_mask);
	require(sigaction(signal, &action, nullptr) == 0,
		"cannot install a host disposition");
}

/* The flags the Darwin kernel gives its timer handler. */
void install_kernel_handler(int signal)
{
	struct sigaction action {};
	action.sa_sigaction = kernel_handler;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, SIGALRM);
	require(sigaction(signal, &action, nullptr) == 0,
		"cannot install a kernel handler");
}

void change_mask(int how, int signal)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, signal);
	require(pthread_sigmask(how, &set, nullptr) == 0,
		"cannot change the thread signal mask");
}

[[nodiscard]] bool pending_here(int signal)
{
	sigset_t pending;
	sigemptyset(&pending);
	require(sigpending(&pending) == 0, "cannot read pending signals");
	return sigismember(&pending, signal) == 1;
}

[[nodiscard]] sigset_t current_mask()
{
	sigset_t mask;
	require(pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0,
		"cannot read the thread signal mask");
	return mask;
}

void arm_real_timer()
{
	itimerval timer{};
	timer.it_value.tv_sec = 3600;
	timer.it_interval.tv_sec = 3600;
	require(setitimer(ITIMER_REAL, &timer, nullptr) == 0,
		"cannot arm ITIMER_REAL");
}

[[nodiscard]] bool real_timer_disarmed()
{
	itimerval timer{};
	require(getitimer(ITIMER_REAL, &timer) == 0,
		"cannot read ITIMER_REAL");
	return timer.it_value.tv_sec == 0 && timer.it_value.tv_usec == 0 &&
		timer.it_interval.tv_sec == 0 && timer.it_interval.tv_usec == 0;
}

void wait_for(const std::atomic_int &phase, int value)
{
	const auto deadline = std::chrono::steady_clock::now() + 10s;
	while (phase.load(std::memory_order_acquire) != value) {
		require(std::chrono::steady_clock::now() < deadline,
			"timed out waiting for the other thread");
		std::this_thread::sleep_for(1ms);
	}
}

/* Runs on a thread whose SIGALRM mask it inherits from the main thread, and
 * reports the first failure back instead of terminating the process. */
void run_on_thread(const std::function<void()> &body)
{
	std::exception_ptr failure;
	std::thread thread([&] {
		try {
			body();
		} catch (...) {
			failure = std::current_exception();
		}
	});
	thread.join();
	if (failure)
		std::rethrow_exception(failure);
}

void reset_process_state()
{
	for (const int signal : {SIGALRM, SIGUSR1, SIGUSR2, SIGWINCH, SIGPIPE}) {
		install(signal, SIG_IGN);
		install(signal, SIG_DFL);
	}
	const itimerval disarmed{};
	(void)setitimer(ITIMER_REAL, &disarmed, nullptr);
	sigset_t none;
	sigemptyset(&none);
	(void)pthread_sigmask(SIG_SETMASK, &none, nullptr);
	kernel_deliveries.store(0);
	host_deliveries.store(0);
}

/* The hazard the scope exists for: a process-directed signal raised while
 * every thread blocks it stays with the main thread, even after another
 * thread unblocks it, and runs its handler when the main thread unblocks it.
 */
void test_blocked_process_signal_waits_on_main_thread()
{
	reset_process_state();
	install_kernel_handler(SIGALRM);
	change_mask(SIG_BLOCK, SIGALRM);
	run_on_thread([] {
		require(kill(getpid(), SIGALRM) == 0, "cannot raise SIGALRM");
		change_mask(SIG_UNBLOCK, SIGALRM);
		std::this_thread::sleep_for(20ms);
		require(kernel_deliveries.load() == 0,
			"the parked tick moved to a thread which unblocked it");
	});
	require(pending_here(SIGALRM),
		"the blocked tick did not wait on the main thread");
	change_mask(SIG_UNBLOCK, SIGALRM);
	require(kernel_deliveries.load() == 1,
		"unblocking the main thread did not deliver the parked tick");
	reset_process_state();
}

/* The native display case: the kernel runs on a worker while the AppKit
 * thread blocks SIGALRM, and the tick parked on the AppKit thread must not
 * reach the kernel's handler once the image is gone. */
void test_scope_revokes_kernel_signals_from_worker()
{
	reset_process_state();
	install(SIGALRM, host_handler);
	install(SIGUSR1, host_handler);
	change_mask(SIG_BLOCK, SIGALRM);
	std::atomic_int phase{};
	std::exception_ptr worker_failure;
	std::exception_ptr main_failure;
	std::thread worker([&] {
		try {
			change_mask(SIG_UNBLOCK, SIGALRM);
			const sigset_t lent = current_mask();
			{
				const uml::runtime::KernelSignalScope scope{
					image_begin(), image_end()};
				install_kernel_handler(SIGALRM);
				install_kernel_handler(SIGUSR1);
				install_kernel_handler(SIGUSR2);
				install(SIGWINCH, foreign_handler);
				install(SIGPIPE, SIG_IGN);
				change_mask(SIG_BLOCK, SIGALRM);
				change_mask(SIG_BLOCK, SIGPIPE);
				arm_real_timer();
				require(kill(getpid(), SIGALRM) == 0,
					"cannot raise SIGALRM");
				phase.store(1, std::memory_order_release);
				wait_for(phase, 2);
			}
			require(handler_of(SIGALRM) ==
				reinterpret_cast<std::uintptr_t>(&host_handler),
				"SIGALRM did not return to the host handler");
			require(handler_of(SIGUSR1) ==
				reinterpret_cast<std::uintptr_t>(&host_handler),
				"SIGUSR1 did not return to the host handler");
			require(handler_of(SIGUSR2) ==
				reinterpret_cast<std::uintptr_t>(SIG_DFL),
				"SIGUSR2 did not return to its default action");
			require(handler_of(SIGWINCH) ==
				reinterpret_cast<std::uintptr_t>(&foreign_handler),
				"a handler outside the image was revoked");
			require(handler_of(SIGPIPE) ==
				reinterpret_cast<std::uintptr_t>(SIG_IGN),
				"an ignored signal was revoked");
			require(real_timer_disarmed(),
				"ITIMER_REAL survived the kernel");
			const sigset_t returned = current_mask();
			require(returned == lent,
				"the lent thread came back with another mask");
		} catch (...) {
			worker_failure = std::current_exception();
		}
		phase.store(3, std::memory_order_release);
	});
	bool parked = false;
	try {
		wait_for(phase, 1);
		parked = pending_here(SIGALRM);
	} catch (...) {
		main_failure = std::current_exception();
	}
	phase.store(2, std::memory_order_release);
	worker.join();
	if (worker_failure)
		std::rethrow_exception(worker_failure);
	if (main_failure)
		std::rethrow_exception(main_failure);
	require(parked, "the test did not park a tick on the main thread");
	require(!pending_here(SIGALRM),
		"the parked tick survived the revocation");
	change_mask(SIG_UNBLOCK, SIGALRM);
	require(kernel_deliveries.load() == 0,
		"a revoked kernel handler ran");
	require(host_deliveries.load() == 0,
		"a tick raised for the kernel reached the host handler");
	reset_process_state();
}

/* The headless case: the kernel runs on the main thread itself. */
void test_scope_revokes_kernel_signals_on_calling_thread()
{
	reset_process_state();
	const sigset_t lent = current_mask();
	{
		const uml::runtime::KernelSignalScope scope{
			image_begin(), image_end()};
		install_kernel_handler(SIGALRM);
		change_mask(SIG_BLOCK, SIGALRM);
		arm_real_timer();
		require(kill(getpid(), SIGALRM) == 0, "cannot raise SIGALRM");
		require(pending_here(SIGALRM),
			"the test did not park a tick on the calling thread");
	}
	const sigset_t returned = current_mask();
	require(returned == lent,
		"the calling thread came back with another mask");
	require(handler_of(SIGALRM) ==
		reinterpret_cast<std::uintptr_t>(SIG_DFL),
		"SIGALRM did not return to its default action");
	require(real_timer_disarmed(), "ITIMER_REAL survived the kernel");
	require(!pending_here(SIGALRM),
		"the parked tick survived the revocation");
	require(kernel_deliveries.load() == 0, "a revoked kernel handler ran");
	reset_process_state();
}

} // namespace

int main()
{
	try {
		test_blocked_process_signal_waits_on_main_thread();
		test_scope_revokes_kernel_signals_from_worker();
		test_scope_revokes_kernel_signals_on_calling_thread();
	} catch (const std::exception &error) {
		std::fprintf(stderr, "kernel signal scope test failed: %s\n",
			error.what());
		return EXIT_FAILURE;
	}
	std::puts("kernel signal scope tests passed");
	return EXIT_SUCCESS;
}
