// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <libslirp.h>
#include <poll.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>

#ifndef UML_SLIRP_TESTING
#define UML_SLIRP_TESTING 0
#endif

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

    int open(std::span<unsigned char, 6> mac, unsigned int &mtu);
	/* The active-session IRQ descriptor is borrowed from the provider and keeps
	 * the same numeric identity across stop/reopen. It closes only when the
	 * provider itself is destroyed, after users have released registrations. */
    [[nodiscard]] int irq_fd() const;
    int read(std::span<std::byte> buffer);
    int write(std::span<const std::byte> frame);
	void stop() noexcept;

#if UML_SLIRP_TESTING
	void pause_transmit_for_testing() noexcept;
	[[nodiscard]] bool wait_for_transmit_pause_for_testing() noexcept;
	void pause_next_write_before_commit_for_testing() noexcept;
	[[nodiscard]] bool wait_for_write_pause_for_testing() noexcept;
	void release_write_for_testing() noexcept;
	void pause_next_read_before_notification_for_testing() noexcept;
	[[nodiscard]] bool wait_for_read_pause_for_testing() noexcept;
	void release_read_for_testing() noexcept;
	[[nodiscard]] bool wait_for_receive_cleanup_for_testing() noexcept;
	[[nodiscard]] slirp_ssize_t enqueue_received_for_testing(
		std::span<const std::byte> frame) noexcept;
	[[nodiscard]] bool readiness_channels_drained_for_testing() const noexcept;
	[[nodiscard]] std::uint64_t processed_transmits_for_testing() const noexcept
	{
		return processed_transmits_for_testing_.load(
			std::memory_order_acquire);
	}
#endif

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

    slirp_ssize_t enqueue_received(std::span<const std::byte> frame);
	void process_transmit_queue() noexcept;
	[[nodiscard]] bool transmit_permitted() noexcept;
	void wake() noexcept;
#if UML_SLIRP_TESTING
	void pause_write_before_commit_for_testing() noexcept;
	void pause_read_before_notification_for_testing() noexcept;
	void notify_receive_cleanup_for_testing() noexcept;
#endif
	void event_loop(std::stop_token stop_token) noexcept;
	[[nodiscard]] std::uint32_t timer_timeout(
		std::uint32_t timeout) const noexcept;
	void handle_expired_timers() noexcept;

	mutable std::mutex start_mutex_;
	std::mutex receive_mutex_;
	std::mutex transmit_mutex_;
#if UML_SLIRP_TESTING
	std::mutex transmit_pause_mutex_;
	std::condition_variable transmit_pause_condition_;
	std::mutex io_pause_mutex_;
	std::condition_variable io_pause_condition_;
#endif
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
	std::atomic_bool stopping_{true};
#if UML_SLIRP_TESTING
	std::atomic_bool transmit_pause_requested_for_testing_{};
	bool transmit_pause_reached_for_testing_{};
	std::atomic_uint64_t processed_transmits_for_testing_{};
	bool write_pause_requested_for_testing_{};
	bool write_pause_reached_for_testing_{};
	bool release_write_for_testing_{};
	bool read_pause_requested_for_testing_{};
	bool read_pause_reached_for_testing_{};
	bool release_read_for_testing_{};
	bool receive_cleanup_reached_for_testing_{};
#endif
};

} // namespace uml::network
