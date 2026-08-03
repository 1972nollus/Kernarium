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
	return emulator->enqueue_received(
		{static_cast<const std::byte *>(data), length});
}

slirp_ssize_t SlirpProvider::enqueue_received(
	std::span<const std::byte> frame) noexcept
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
		slirp_input(slirp_.get(),
			reinterpret_cast<const std::uint8_t *>(frame.data()),
			static_cast<int>(length));
	}
}

int SlirpProvider::read(std::span<std::byte> buffer) noexcept
{
	std::size_t length;
	{
		std::lock_guard lock{receive_mutex_};
		if (receive_tail_ == receive_head_)
			return 0;
		const auto &source = receive_ring_[receive_tail_];
		length = source.length;
		receive_tail_ = (receive_tail_ + 1) % ring_size;
		if (length > buffer.size())
			return -EMSGSIZE;
		std::ranges::copy_n(source.data.begin(), length, buffer.begin());
	}
	if (receive_pipe_[0].get() >= 0) {
		std::byte value{};
		(void)::read(receive_pipe_[0].get(), &value, sizeof(value));
	}
	return static_cast<int>(length);
}

int SlirpProvider::write(std::span<const std::byte> frame) noexcept
{
	if (!started_.load(std::memory_order_acquire) || frame.empty() ||
	    frame.size() > frame_size)
		return -EINVAL;
	{
		std::lock_guard lock{transmit_mutex_};
		const auto next = (transmit_head_ + 1) % ring_size;
		if (next == transmit_tail_)
			return -EAGAIN;
		auto &destination = transmit_ring_[transmit_head_];
		destination.length = frame.size();
		std::ranges::copy(frame, destination.data.begin());
		transmit_head_ = next;
	}
	wake();
	return 0;
}

} // namespace uml::network
