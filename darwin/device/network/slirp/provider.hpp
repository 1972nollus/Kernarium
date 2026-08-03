// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <libslirp.h>
#include <poll.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>

namespace uml::network {

inline constexpr std::size_t frame_size = 2048;
inline constexpr std::size_t ring_size = 256;
inline constexpr std::size_t max_poll_descriptors = 512;

class UniqueFd final {
public:
	UniqueFd() = default;
	explicit UniqueFd(int descriptor) noexcept : descriptor_(descriptor) {}
	~UniqueFd();

	UniqueFd(const UniqueFd &) = delete;
	UniqueFd &operator=(const UniqueFd &) = delete;
	UniqueFd(UniqueFd &&other) noexcept;
	UniqueFd &operator=(UniqueFd &&other) noexcept;

	void reset(int descriptor = -1) noexcept;
	[[nodiscard]] int get() const noexcept { return descriptor_; }

private:
	int descriptor_{-1};
};

struct Frame final {
	std::size_t length{};
	std::array<std::byte, frame_size> data{};
};

struct Timer final {
	SlirpTimerId id{};
	void *callback_opaque{};
	std::int64_t expires_ms{-1};
};

struct PollState final {
	std::array<pollfd, max_poll_descriptors + 1> descriptors{};
	nfds_t count{};
};

struct SlirpDeleter final {
	void operator()(Slirp *slirp) const noexcept;
};

class SlirpProvider final {
public:
	SlirpProvider() = default;

	SlirpProvider(const SlirpProvider &) = delete;
	SlirpProvider &operator=(const SlirpProvider &) = delete;
	~SlirpProvider();

	int open(std::span<unsigned char, 6> mac, unsigned int &mtu) noexcept;
	[[nodiscard]] int irq_fd() const noexcept;
	int read(std::span<std::byte> buffer) noexcept;
	int write(std::span<const std::byte> frame) noexcept;
	void stop() noexcept;

private:
	static const SlirpCb &callbacks() noexcept;
	static SlirpProvider *from_opaque(void *opaque) noexcept;
	static slirp_ssize_t send_packet_callback(
		const void *data, std::size_t length, void *opaque) noexcept;
	static void guest_error_callback(const char *message,
					 void *opaque) noexcept;
	static std::int64_t clock_callback(void *opaque) noexcept;
	static void *timer_new_callback(SlirpTimerId id,
					void *callback_opaque,
					void *opaque) noexcept;
	static void timer_free_callback(void *timer, void *opaque) noexcept;
	static void timer_modify_callback(void *timer, std::int64_t expires_ms,
					  void *opaque) noexcept;
	static void notify_callback(void *opaque) noexcept;
	static void ignore_poll_socket(slirp_os_socket descriptor,
				       void *opaque) noexcept;
	static int add_poll_descriptor(slirp_os_socket descriptor, int events,
				       void *opaque) noexcept;
	static int get_poll_events(int index, void *opaque) noexcept;

	slirp_ssize_t enqueue_received(std::span<const std::byte> frame) noexcept;
	void process_transmit_queue() noexcept;
	void wake() noexcept;
	void event_loop(std::stop_token stop_token) noexcept;
	[[nodiscard]] std::uint32_t timer_timeout(
		std::uint32_t timeout) const noexcept;
	void handle_expired_timers() noexcept;

	mutable std::mutex start_mutex_;
	std::mutex receive_mutex_;
	std::mutex transmit_mutex_;
	std::array<Frame, ring_size> receive_ring_{};
	std::array<Frame, ring_size> transmit_ring_{};
	std::size_t receive_head_{};
	std::size_t receive_tail_{};
	std::size_t transmit_head_{};
	std::size_t transmit_tail_{};
	std::list<std::unique_ptr<Timer>> timers_;
	std::unique_ptr<Slirp, SlirpDeleter> slirp_;
	std::array<UniqueFd, 2> wake_pipe_;
	std::array<UniqueFd, 2> receive_pipe_;
	std::jthread worker_;
	std::atomic_bool started_{false};
};

} // namespace uml::network
