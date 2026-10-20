// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "hibernation_mode.hpp"
#include "memory/guest_ram_image.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace uml::runtime {

/*
 * Hibernating a paused guest's RAM.
 *
 * A guest that the host has quiesced stays in the process, and on a physical
 * iOS device its RAM is wired, so the system can neither compress it nor
 * count it as anything but the application's own. Hibernation writes the RAM
 * the guest holds to a file in the application's container and takes it out
 * of the process, so the paused application is small enough for the system to
 * keep. It is the one window in which guest RAM is not wired.
 *
 * The order is the whole contract.
 *
 *   hibernate: every thread that can touch guest RAM is held, then the RAM is
 *   written and evicted.
 *   restore:   the RAM is read back, verified against the image's own hashes
 *   and wired again, and only then are the threads released; the caller wakes
 *   the guest after that.
 *
 * A restore that cannot bring every page back verified must not let anything
 * run on the missing memory. Nothing is released then, and the process ends,
 * so that the next launch boots the guest again (the application's restart
 * notice).
 */

/* What one hibernation and its restore did, for the log and for tests. */
struct HibernationReport final {
	std::size_t threads_held{};
	std::size_t threads_named{};
	std::size_t threads_registered{};
	unsigned hold_passes{};
	unsigned hold_deferred{};
	unsigned hold_unsettled{};
	int hold_failure{};
	std::uint64_t hold_ms{};
	std::uint64_t write_ms{};
	std::uint64_t restore_ms{};
	/* The process's physical footprint, in bytes, before the write, after it
	 * and after the restore. */
	std::uint64_t footprint_before{};
	std::uint64_t footprint_after{};
	std::uint64_t footprint_restored{};
	memory::ImageStats written{};
	memory::ImageStats restored{};
};

/*
 * The work behind a hibernation, as steps the orchestration calls in order.
 * The production steps hold real threads and a real image; a test can
 * replace them with steps that record or fail. Every step after `hold` runs
 * with threads held and must neither allocate nor write to a stream.
 */
class HibernationSteps {
public:
	virtual ~HibernationSteps() = default;
	/* Makes ready what can only be made before a thread is held, and says
	 * whether there is anything to hibernate. */
	[[nodiscard]] virtual bool prepare() noexcept = 0;
	/* Holds every thread that can touch guest RAM and freezes the guest's
	 * memory commitment. False leaves everything running. */
	[[nodiscard]] virtual bool hold(HibernationReport &report) noexcept = 0;
	/* Writes the image and evicts. False when part of the RAM stayed in the
	 * process; whatever left is still brought back by `restore`. */
	[[nodiscard]] virtual bool write(HibernationReport &report) noexcept = 0;
	/* Brings the evicted RAM back, verified and wired. False when the RAM is
	 * not complete. */
	[[nodiscard]] virtual bool restore(HibernationReport &report) noexcept = 0;
	/* Lets the held threads run. */
	virtual void release() noexcept = 0;
	/* Makes a `write` in progress leave the chunks it has not reached in the
	 * process. Callable from any thread. */
	virtual void set_abort(bool abort) noexcept = 0;
};

class GuestHibernation final {
public:
	/* What ends the process when the RAM cannot be restored. */
	using FailureHandler = void (*)() noexcept;

	/* Ends the process without running anything of the guest's. */
	static void end_process() noexcept;

	/* The production steps for the process's guest RAM. `directory` is where
	 * the image goes: the application's own temporary directory. */
	[[nodiscard]] static std::unique_ptr<GuestHibernation> create(
		HibernationMode mode, std::string directory);

	explicit GuestHibernation(std::unique_ptr<HibernationSteps> steps,
		FailureHandler on_failure = &end_process) noexcept;
	~GuestHibernation();
	GuestHibernation(const GuestHibernation &) = delete;
	GuestHibernation &operator=(const GuestHibernation &) = delete;

	/* After the guest is suspended and its host files are flushed. True when
	 * the hibernation took place, in whole or in part; false when nothing was
	 * changed and the guest's RAM is as it was. */
	[[nodiscard]] bool hibernate() noexcept;
	/* Before the guest is woken. True when the RAM is whole, or was never
	 * hibernated, and the threads run again. False when it could not be
	 * restored: the failure handler has been called, and nothing runs. */
	[[nodiscard]] bool restore() noexcept;

	[[nodiscard]] bool hibernated() const noexcept { return hibernated_; }
	/* Makes a hibernation in progress stop evicting. Callable from any
	 * thread; the next `hibernate` starts afresh. */
	void request_abort() noexcept;

	[[nodiscard]] const HibernationReport &report() const noexcept
	{
		return report_;
	}

private:
	std::unique_ptr<HibernationSteps> steps_;
	FailureHandler on_failure_;
	HibernationReport report_{};
	bool hibernated_{false};
};

/* The directory the image of this process goes in: $TMPDIR when it is set,
 * else the user's temporary directory. */
[[nodiscard]] std::string hibernation_directory();

/* Deletes the images a process that ended left in `directory`. */
std::size_t remove_stale_hibernation_images(const std::string &directory) noexcept;

} // namespace uml::runtime
