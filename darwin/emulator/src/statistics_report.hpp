/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace uml::emulator {

/*
 * The line a macOS launcher prints for each interval of --stats=<ms>: the
 * share of the guest's vCPUs that computed, the frames that reached the
 * display and the frames the guest offered, per second, the memory the guest
 * and the host report and, with a display, how many graphics allocations the
 * host has refused the guest. It is measurement tooling with no UI, built from
 * the same calls the iOS applications make, so a Mac validates their figures.
 * The report keeps no clock and reads no source: it turns the samples it is
 * given into text.
 */

/* One reading of the host's figures for a running guest. */
struct StatisticsSample final {
	std::chrono::steady_clock::time_point at;
	/* The guest's vCPUs that have come up, and the user and system time the
	 * threads that run them have used in this boot. */
	std::uint32_t processors {};
	std::uint64_t cpu_time_ns {};
	/* The display's frame counters; both stay 0 without a display. */
	std::uint64_t presented_frames {};
	std::uint64_t offered_frames {};
	/* The guest's RAM and the part of it the host holds. */
	std::uint64_t ram_bytes {};
	std::uint64_t ram_held_bytes {};
	/* How many times the host told the guest its GPU had no room for an
	 * allocation, since the display was made; none without a display. */
	std::optional<std::uint64_t> gpu_memory_refusals;
};

/* The guest kernel's own memory figures. */
struct StatisticsMemory final {
	std::uint64_t total_bytes {};
	std::uint64_t available_bytes {};
};

class StatisticsReport final {
public:
	/* Whether the guest's memory is read with the next sample: every second
	 * one, since it changes slowly and each read costs the guest an
	 * interrupt. Asking advances the cadence. */
	[[nodiscard]] bool memory_due() noexcept
	{
		return (++asked_ & 1U) == 0U;
	}

	/* No guest was running when the last sample was due: whatever comes next
	 * starts a new baseline, since a rate must not span two guests. */
	void guest_ended() noexcept { previous_.reset(); }

	/* Takes `sample` and returns the line for the interval since the one
	 * before it, or nothing when there is no interval to speak of: the first
	 * sample, the first after guest_ended(), or one whose time did not
	 * advance or whose counters went backwards (a guest that restarted
	 * between two samples starts them again from zero). `memory` is the
	 * guest kernel's figures when this interval read them. */
	[[nodiscard]] std::optional<std::string> add(
		const StatisticsSample &sample,
		const std::optional<StatisticsMemory> &memory)
	{
		if (!origin_)
			origin_ = sample.at;
		const auto previous = std::exchange(previous_, sample);
		if (!previous || sample.at <= previous->at ||
		    sample.cpu_time_ns < previous->cpu_time_ns ||
		    sample.presented_frames < previous->presented_frames ||
		    sample.offered_frames < previous->offered_frames)
			return std::nullopt;
		const double interval = std::chrono::duration<double>(
			sample.at - previous->at).count();
		const double elapsed = std::chrono::duration<double>(
			sample.at - *origin_).count();
		const double share = sample.processors == 0U ? 0.0 :
			100.0 * static_cast<double>(
				sample.cpu_time_ns - previous->cpu_time_ns) /
			(interval * 1e9 * sample.processors);
		const double presented = static_cast<double>(
			sample.presented_frames - previous->presented_frames) /
			interval;
		const double offered = static_cast<double>(
			sample.offered_frames - previous->offered_frames) /
			interval;
		char line[320];
		int length = std::snprintf(line, sizeof(line),
			"uml-darwin: stats %.1f s cpu %.1f%% of %u vCPUs "
			"frames %.1f/s presented %.1f/s offered",
			elapsed, share > 100.0 ? 100.0 : share, sample.processors,
			presented, offered);
		if (memory && length > 0 && length < static_cast<int>(sizeof(line)))
			length += std::snprintf(line + length, sizeof(line) - length,
				" memory %llu MiB in use of %llu MiB",
				mebibytes(memory->total_bytes >
						  memory->available_bytes ?
					  memory->total_bytes -
						  memory->available_bytes :
					  0U),
				mebibytes(memory->total_bytes));
		if (sample.ram_bytes != 0U && length > 0 &&
		    length < static_cast<int>(sizeof(line)))
			(void)std::snprintf(line + length, sizeof(line) - length,
				" held %llu of %llu MiB",
				mebibytes(sample.ram_held_bytes),
				mebibytes(sample.ram_bytes));
		std::string text {line};
		if (sample.gpu_memory_refusals)
			text += " gpu-refusals " +
				std::to_string(*sample.gpu_memory_refusals);
		return text;
	}

private:
	[[nodiscard]] static unsigned long long mebibytes(
		std::uint64_t bytes) noexcept
	{
		return static_cast<unsigned long long>(bytes >> 20);
	}

	std::optional<StatisticsSample> previous_;
	std::optional<std::chrono::steady_clock::time_point> origin_;
	unsigned asked_ {};
};

} // namespace uml::emulator
