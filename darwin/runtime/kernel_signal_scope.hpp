// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>

namespace uml::runtime {

/*
 * An embedded kernel installs process-wide host signal handlers and owns
 * ITIMER_REAL while it runs, and all of it is still in place when the
 * kernel's entry returns. The loader releases the image right after that, so
 * a handler left pointing into it branches into unmapped memory the next time
 * its signal is delivered. That signal need not be a new one: Darwin assigns a
 * process-directed signal to a thread when it is generated and never moves it,
 * so a timer tick raised while every thread blocked SIGALRM waits on the main
 * thread for as long as that thread keeps it blocked, which on the AppKit
 * thread is until after the kernel has returned.
 *
 * The scope records the process's signal dispositions and the calling
 * thread's signal mask before the kernel runs. Closing it disarms ITIMER_REAL,
 * passes every signal whose handler lies inside the image through SIG_IGN,
 * which discards an instance pending on any thread, reinstalls the recorded
 * disposition, and restores the calling thread's mask. SIG_DFL and SIG_IGN
 * dispositions reference no kernel code and are left as the kernel set them.
 * The scope must close before the image is released.
 */
class KernelSignalScope final {
public:
	KernelSignalScope(std::uintptr_t image_begin,
		std::uintptr_t image_end) noexcept;
	~KernelSignalScope();

	KernelSignalScope(const KernelSignalScope &) = delete;
	KernelSignalScope &operator=(const KernelSignalScope &) = delete;

private:
	[[nodiscard]] bool inside_image(
		const struct sigaction &action) const noexcept;

	std::uintptr_t image_begin_;
	std::uintptr_t image_end_;
	std::array<struct sigaction, NSIG> dispositions_{};
	std::array<bool, NSIG> recorded_{};
	sigset_t mask_{};
	bool mask_recorded_{};
};

/*
 * Releases the kernel image the loader mapped, once the kernel has returned.
 * The image's last pages are the first guest RAM, and a host-service view of
 * guest RAM can still be read after the run: in-process backends stop only
 * after it returns. While such a view borrows the RAM root, the range stays
 * mapped and goes back to the window's reservation with the last borrowed
 * view, as retired guest RAM does. Otherwise a range inside the window goes
 * back to its reservation at once and any other range is unmapped.
 */
void release_kernel_image(void *address, std::size_t length,
	bool in_window) noexcept;

} // namespace uml::runtime
