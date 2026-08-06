// SPDX-License-Identifier: GPL-2.0-or-later
#include "provider.hpp"

#include <cerrno>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace uml::network {

SlirpProvider *SlirpProvider::from_opaque(void *opaque) noexcept
{
	return static_cast<SlirpProvider *>(opaque);
}

void SlirpProvider::wake() noexcept
{
	const std::byte value{1};
	if (wake_pipe_[1].get() >= 0)
		(void)::write(wake_pipe_[1].get(), &value, sizeof(value));
}

void SlirpProvider::notify_callback(void *opaque) noexcept
{
	if (auto *emulator = from_opaque(opaque); emulator != nullptr)
		emulator->wake();
}

slirp_ssize_t SlirpProvider::send_packet_callback(
	const void *data, std::size_t length, void *opaque) noexcept
{
	auto *emulator = from_opaque(opaque);
	if (emulator == nullptr || data == nullptr)
		return -1;
	try {
		return emulator->enqueue_received(
			{static_cast<const std::byte *>(data), length});
	} catch (...) {
		return -1;
	}
}

slirp_ssize_t SlirpProvider::enqueue_received(
	std::span<const std::byte> frame)
{
	if (frame.empty() || frame.size() > frame_size)
		return -1;
	std::lock_guard lock{receive_mutex_};
	const auto next = (receive_head_ + 1) % ring_size;
	if (next == receive_tail_)
		return 0;
	auto &destination = receive_ring_[receive_head_];
	destination.length = frame.size();
	std::ranges::copy(frame, destination.data.begin());
	receive_head_ = next;
	if (receive_pipe_[1].get() >= 0) {
		const std::byte value{1};
		(void)::write(receive_pipe_[1].get(), &value, sizeof(value));
	}
	return static_cast<slirp_ssize_t>(frame.size());
}

void SlirpProvider::process_transmit_queue() noexcept
{
	std::array<std::byte, frame_size> frame{};
	for (;;) {
		if (!transmit_permitted())
			return;
		std::size_t length;
		{
			std::lock_guard lock{transmit_mutex_};
			if (transmit_tail_ == transmit_head_)
				return;
			const auto &source = transmit_ring_[transmit_tail_];
			length = source.length;
			std::ranges::copy_n(source.data.begin(), length,
					   frame.begin());
			transmit_tail_ = (transmit_tail_ + 1) % ring_size;
		}
#if UML_SLIRP_TESTING
		processed_transmits_for_testing_.fetch_add(
			1, std::memory_order_release);
#endif
		slirp_input(slirp_.get(),
			reinterpret_cast<const std::uint8_t *>(frame.data()),
			static_cast<int>(length));
	}
}

bool SlirpProvider::transmit_permitted() noexcept
{
	if (stopping_.load(std::memory_order_acquire))
		return false;
#if UML_SLIRP_TESTING
	if (!transmit_pause_requested_for_testing_.load(
		    std::memory_order_acquire))
		return true;
	std::unique_lock lock{transmit_pause_mutex_};
	transmit_pause_reached_for_testing_ = true;
	transmit_pause_condition_.notify_all();
	transmit_pause_condition_.wait(lock, [this] {
		return stopping_.load(std::memory_order_acquire) ||
			!transmit_pause_requested_for_testing_.load(
				std::memory_order_acquire);
	});
	return !stopping_.load(std::memory_order_acquire);
#else
	return true;
#endif
}

int SlirpProvider::read(std::span<std::byte> buffer)
{
	std::size_t length;
	{
		std::lock_guard lock{receive_mutex_};
		if (!started_.load(std::memory_order_acquire))
			return 0;
		if (receive_tail_ == receive_head_)
			return 0;
		const auto &source = receive_ring_[receive_tail_];
		length = source.length;
		if (length > buffer.size())
			return -EMSGSIZE;
		std::ranges::copy_n(source.data.begin(), length, buffer.begin());
		receive_tail_ = (receive_tail_ + 1) % ring_size;
		/* stop() drains this notification channel under receive_mutex_. Keep
		 * dequeue and notification consumption in one ownership interval so
		 * cleanup cannot split them across sessions. */
#if UML_SLIRP_TESTING
		pause_read_before_notification_for_testing();
#endif
		if (receive_pipe_[0].get() >= 0) {
			std::byte value{};
			(void)::read(receive_pipe_[0].get(), &value, sizeof(value));
		}
	}
	return static_cast<int>(length);
}

int SlirpProvider::write(std::span<const std::byte> frame)
{
	if (frame.empty() || frame.size() > frame_size)
		return -EINVAL;
#if UML_SLIRP_TESTING
	pause_write_before_commit_for_testing();
#endif
	{
		std::lock_guard lock{transmit_mutex_};
		/* stop() closes admission first and takes this same mutex before it
		 * clears the ring or drains the wake pipe. The single existing started
		 * load is deliberately inside that ownership interval: delayed writers
		 * fail instead of committing into the next open() session. */
		if (!started_.load(std::memory_order_acquire))
			return -EINVAL;
		const auto next = (transmit_head_ + 1) % ring_size;
		if (next == transmit_tail_)
			return -EAGAIN;
		auto &destination = transmit_ring_[transmit_head_];
		destination.length = frame.size();
		std::ranges::copy(frame, destination.data.begin());
		transmit_head_ = next;
		wake();
	}
	return 0;
}

} // namespace uml::network
