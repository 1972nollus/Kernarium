// SPDX-License-Identifier: GPL-2.0-or-later
#include "provider.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace uml::network {
namespace {

std::int64_t monotonic_nanoseconds() noexcept
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

short poll_events_from_slirp(int events) noexcept
{
	short result = 0;
	if ((events & SLIRP_POLL_IN) != 0)
		result |= POLLIN;
	if ((events & SLIRP_POLL_OUT) != 0)
		result |= POLLOUT;
	if ((events & SLIRP_POLL_PRI) != 0)
		result |= POLLPRI;
	if ((events & SLIRP_POLL_ERR) != 0)
		result |= POLLERR;
	if ((events & SLIRP_POLL_HUP) != 0)
		result |= POLLHUP;
	return result;
}

int slirp_events_from_poll(short events) noexcept
{
	int result = 0;
	if ((events & POLLIN) != 0)
		result |= SLIRP_POLL_IN;
	if ((events & POLLOUT) != 0)
		result |= SLIRP_POLL_OUT;
	if ((events & POLLPRI) != 0)
		result |= SLIRP_POLL_PRI;
	if ((events & POLLERR) != 0)
		result |= SLIRP_POLL_ERR;
	if ((events & POLLHUP) != 0)
		result |= SLIRP_POLL_HUP;
	return result;
}

} // namespace

void SlirpProvider::guest_error_callback(const char *message,
					void *opaque) noexcept
{
	static constexpr char prefix[] = "Darwin user network: ";
	(void)opaque;
	(void)::write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
	if (message != nullptr)
		(void)::write(STDERR_FILENO, message, std::strlen(message));
	(void)::write(STDERR_FILENO, "\n", 1);
}

std::int64_t SlirpProvider::clock_callback(void *opaque) noexcept
{
	(void)opaque;
	return monotonic_nanoseconds();
}

void *SlirpProvider::timer_new_callback(SlirpTimerId id,
				       void *callback_opaque,
				       void *opaque) noexcept
{
	auto *emulator = from_opaque(opaque);
	if (emulator == nullptr)
		return nullptr;
	try {
		auto timer = std::make_unique<Timer>();
		timer->id = id;
		timer->callback_opaque = callback_opaque;
		auto *result = timer.get();
		emulator->timers_.push_front(std::move(timer));
		return result;
	} catch (...) {
		return nullptr;
	}
}

void SlirpProvider::timer_free_callback(void *timer, void *opaque) noexcept
{
	auto *emulator = from_opaque(opaque);
	if (emulator == nullptr)
		return;
	const auto match = std::ranges::find_if(
		emulator->timers_, [timer](const auto &candidate) {
			return candidate.get() == timer;
		});
	if (match != emulator->timers_.end())
		emulator->timers_.erase(match);
}

void SlirpProvider::timer_modify_callback(
	void *timer, std::int64_t expires_ms, void *opaque) noexcept
{
	(void)opaque;
	if (timer != nullptr)
		static_cast<Timer *>(timer)->expires_ms = expires_ms;
}

void SlirpProvider::ignore_poll_socket(
	slirp_os_socket descriptor, void *opaque) noexcept
{
	(void)descriptor;
	(void)opaque;
}

int SlirpProvider::add_poll_descriptor(
	slirp_os_socket descriptor, int events, void *opaque) noexcept
{
	auto &state = *static_cast<PollState *>(opaque);
	if (state.count >= max_poll_descriptors)
		return -1;
	auto &entry = state.descriptors[state.count];
	entry.fd = descriptor;
	entry.events = poll_events_from_slirp(events);
	entry.revents = 0;
	return static_cast<int>(state.count++);
}

int SlirpProvider::get_poll_events(int index, void *opaque) noexcept
{
	const auto &state = *static_cast<PollState *>(opaque);
	if (index < 0 || static_cast<nfds_t>(index) >= state.count)
		return 0;
	return slirp_events_from_poll(state.descriptors[index].revents);
}

std::uint32_t SlirpProvider::timer_timeout(
	std::uint32_t timeout) const noexcept
{
	const auto now_ms = monotonic_nanoseconds() / 1'000'000;
	for (const auto &timer : timers_) {
		if (timer->expires_ms < 0)
			continue;
		const auto remaining = timer->expires_ms - now_ms;
		if (remaining <= 0)
			return 0;
		if (static_cast<std::uint64_t>(remaining) < timeout)
			timeout = static_cast<std::uint32_t>(remaining);
	}
	return timeout;
}

void SlirpProvider::handle_expired_timers() noexcept
{
	for (;;) {
		const auto now_ms = monotonic_nanoseconds() / 1'000'000;
		const auto match = std::ranges::find_if(
			timers_, [now_ms](const auto &timer) {
				return timer->expires_ms >= 0 &&
					timer->expires_ms <= now_ms;
			});
		if (match == timers_.end())
			return;
		auto &timer = **match;
		timer.expires_ms = -1;
		slirp_handle_timer(slirp_.get(), timer.id,
				   timer.callback_opaque);
	}
}

void SlirpProvider::event_loop(std::stop_token stop_token) noexcept
{
	while (!stop_token.stop_requested()) {
		PollState state{};
		std::uint32_t timeout = 1000;
		process_transmit_queue();
		slirp_pollfds_fill_socket(slirp_.get(), &timeout,
					 add_poll_descriptor, &state);
		timeout = timer_timeout(timeout);
		const auto wake_index = state.count;
		auto &wake_entry = state.descriptors[wake_index];
		wake_entry.fd = wake_pipe_[0].get();
		wake_entry.events = POLLIN;
		state.count++;
		const int result = poll(state.descriptors.data(), state.count,
					static_cast<int>(timeout));
		if ((wake_entry.revents & POLLIN) != 0) {
			std::array<std::byte, 64> buffer{};
			while (::read(wake_pipe_[0].get(), buffer.data(),
				      buffer.size()) > 0)
				;
		}
		if (stop_token.stop_requested())
			break;
		slirp_pollfds_poll(slirp_.get(), result < 0,
				  get_poll_events, &state);
		handle_expired_timers();
	}
}

const SlirpCb &SlirpProvider::callbacks() noexcept
{
	static const SlirpCb callbacks{
		.send_packet = send_packet_callback,
		.guest_error = guest_error_callback,
		.clock_get_ns = clock_callback,
		.timer_free = timer_free_callback,
		.timer_mod = timer_modify_callback,
		.notify = notify_callback,
		.timer_new_opaque = timer_new_callback,
		.register_poll_socket = ignore_poll_socket,
		.unregister_poll_socket = ignore_poll_socket,
	};
	return callbacks;
}

} // namespace uml::network
