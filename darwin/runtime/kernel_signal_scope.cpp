// SPDX-License-Identifier: GPL-2.0-or-later
#include "kernel_signal_scope.hpp"
#include "memory/shared_backing.hpp"

#include <pthread.h>
#include <sys/mman.h>
#include <sys/time.h>

namespace uml::runtime {
namespace {

[[nodiscard]] bool revocable(int signal) noexcept
{
	return signal != SIGKILL && signal != SIGSTOP;
}

[[nodiscard]] std::uintptr_t handler_address(
	const struct sigaction &action) noexcept
{
	if ((action.sa_flags & SA_SIGINFO) != 0)
		return reinterpret_cast<std::uintptr_t>(action.sa_sigaction);
	return reinterpret_cast<std::uintptr_t>(action.sa_handler);
}

} // namespace

KernelSignalScope::KernelSignalScope(std::uintptr_t image_begin,
	std::uintptr_t image_end) noexcept
	: image_begin_{image_begin}, image_end_{image_end}
{
	for (int signal = 1; signal < NSIG; ++signal)
		recorded_[signal] = revocable(signal) &&
			sigaction(signal, nullptr, &dispositions_[signal]) == 0;
	mask_recorded_ = pthread_sigmask(SIG_SETMASK, nullptr, &mask_) == 0;
}

KernelSignalScope::~KernelSignalScope()
{
	/* Stop the kernel's only self-directed signal source first, so no tick
	 * can be raised after its pending instances have been discarded. */
	const itimerval disarmed{};
	(void)setitimer(ITIMER_REAL, &disarmed, nullptr);

	for (int signal = 1; signal < NSIG; ++signal) {
		struct sigaction current {};
		if (!revocable(signal) ||
		    sigaction(signal, nullptr, &current) != 0 ||
		    !inside_image(current))
			continue;
		/* Ignoring a signal discards every pending instance of it, including
		 * one Darwin parked on a thread which still blocks it. */
		struct sigaction ignore {};
		ignore.sa_handler = SIG_IGN;
		sigemptyset(&ignore.sa_mask);
		(void)sigaction(signal, &ignore, nullptr);

		struct sigaction restored {};
		restored.sa_handler = SIG_DFL;
		sigemptyset(&restored.sa_mask);
		if (recorded_[signal] && !inside_image(dispositions_[signal]))
			restored = dispositions_[signal];
		(void)sigaction(signal, &restored, nullptr);
	}

	/* The kernel blocks its timer signal and SIGPIPE on the thread which
	 * entered it; the host gets that thread back with the mask it lent. */
	if (mask_recorded_)
		(void)pthread_sigmask(SIG_SETMASK, &mask_, nullptr);
}

bool KernelSignalScope::inside_image(
	const struct sigaction &action) const noexcept
{
	const auto address = handler_address(action);
	return address >= image_begin_ && address < image_end_;
}

void release_kernel_image(void *address, std::size_t length,
	bool in_window) noexcept
{
	if (address == nullptr || address == MAP_FAILED || length == 0)
		return;
	if (::uml::memory::retain_borrowed_guest_ram(address, length))
		return;
	if (in_window)
		(void)::uml::memory::reserve_host_range(address, length);
	else
		(void)munmap(address, length);
}

} // namespace uml::runtime
