/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <chrono>

namespace uml::emulator {

/*
 * The loop guard of an application that boots a guest again after the guest
 * restarted itself.
 *
 * A restart is early when it comes less than `early_restart_window` after its
 * boot started. A guest that restarts early `early_restart_limit` times in a
 * row is not going to boot: the application ends the loop instead of
 * rebooting it for ever. A boot that lasts `early_restart_window` or more
 * resets the count. These constants are the one place the policy lives; every
 * application that runs the loop uses them.
 */
inline constexpr std::chrono::seconds early_restart_window{15};
inline constexpr unsigned early_restart_limit = 5;

/* The macOS launchers' exit status when the guard ended the loop. 0 is a
 * power-off and 1 a launch error; a guest never ends with 3. */
inline constexpr int early_restart_exit_status = 3;

class RestartLoopGuard final {
public:
	using Clock = std::chrono::steady_clock;

	/* A boot starts at `now`. */
	void boot_started(Clock::time_point now) noexcept { started_ = now; }

	/* The guest asked to restart at `now`. Whether the application may boot
	 * it again: false once `early_restart_limit` early restarts followed
	 * each other. */
	[[nodiscard]] bool may_restart(Clock::time_point now) noexcept
	{
		if (now - started_ < early_restart_window)
			++early_in_a_row_;
		else
			early_in_a_row_ = 0;
		return early_in_a_row_ < early_restart_limit;
	}

	/* Restarts in a row that came early. */
	[[nodiscard]] unsigned early_restarts() const noexcept
	{
		return early_in_a_row_;
	}

private:
	Clock::time_point started_{};
	unsigned early_in_a_row_{};
};

} // namespace uml::emulator
