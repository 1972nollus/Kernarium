// SPDX-License-Identifier: GPL-2.0-or-later
#include "provider.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <system_error>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>

namespace uml::network {
namespace {

class ScopedGuestTimerBlock final {
public:
	ScopedGuestTimerBlock()
	{
		sigset_t blocked;

		sigemptyset(&blocked);
		sigaddset(&blocked, SIGALRM);
		error_ = pthread_sigmask(SIG_BLOCK, &blocked, &previous_);
	}

	~ScopedGuestTimerBlock()
	{
		if (error_ == 0)
			(void)pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
	}

	[[nodiscard]] int error() const noexcept
	{
		return error_;
	}

private:
	sigset_t previous_ {};
	int error_ {};
};

constexpr std::array<unsigned char, 6> default_mac{
	0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

bool configure_nonblocking(int descriptor) noexcept
{
	const int flags = fcntl(descriptor, F_GETFL);
	return flags >= 0 &&
		fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) >= 0;
}

bool create_pipe(std::array<UniqueFd, 2> &result) noexcept
{
	std::array<int, 2> descriptors{};
	if (pipe(descriptors.data()) != 0)
		return false;
	result[0].reset(descriptors[0]);
	result[1].reset(descriptors[1]);
	if (configure_nonblocking(result[0].get()) &&
	    configure_nonblocking(result[1].get()))
		return true;
	result[0].reset();
	result[1].reset();
	return false;
}

bool ensure_pipe(std::array<UniqueFd, 2> &result) noexcept
{
	if (result[0].get() >= 0 && result[1].get() >= 0)
		return true;
	result[0].reset();
	result[1].reset();
	return create_pipe(result);
}

void drain_pipe(UniqueFd &descriptor) noexcept
{
	std::array<std::byte, 64> buffer{};
	while (descriptor.get() >= 0 &&
	       ::read(descriptor.get(), buffer.data(), buffer.size()) > 0)
		;
}

} // namespace

UniqueFd::~UniqueFd()
{
	reset();
}

UniqueFd::UniqueFd(UniqueFd &&other) noexcept
	: descriptor_(other.descriptor_)
{
	other.descriptor_ = -1;
}

UniqueFd &UniqueFd::operator=(UniqueFd &&other) noexcept
{
	if (this != &other) {
		reset();
		descriptor_ = other.descriptor_;
		other.descriptor_ = -1;
	}
	return *this;
}

void UniqueFd::reset(int descriptor) noexcept
{
	if (descriptor_ >= 0)
		(void)close(descriptor_);
	descriptor_ = descriptor;
}

void SlirpDeleter::operator()(Slirp *slirp) const noexcept
{
	if (slirp != nullptr)
		slirp_cleanup(slirp);
}

SlirpProvider::~SlirpProvider()
{
	stop();
}

int SlirpProvider::open(std::span<unsigned char, 6> mac,
                       unsigned int &mtu)
{
	std::lock_guard lock{start_mutex_};
	if (!started_.load(std::memory_order_acquire)) {
		stopping_.store(false, std::memory_order_release);
		/* Both pipes are provider-owned capabilities. Keep their numeric
		 * descriptors stable across stop/reopen so an IRQ registration that
		 * borrowed receive_pipe_[0] can never observe descriptor reuse between
		 * sessions. Failed first-open attempts may retain the same owned pipes
		 * for a later retry. */
		if (!ensure_pipe(wake_pipe_) || !ensure_pipe(receive_pipe_))
			return errno == 0 ? -EIO : -errno;

		SlirpConfig config{};
		config.version = 6;
		config.in_enabled = true;
		config.restricted = 0;
		(void)inet_pton(AF_INET, "10.0.2.0", &config.vnetwork);
		(void)inet_pton(AF_INET, "255.255.255.0", &config.vnetmask);
		(void)inet_pton(AF_INET, "10.0.2.2", &config.vhost);
		(void)inet_pton(AF_INET, "10.0.2.15", &config.vdhcp_start);
		(void)inet_pton(AF_INET, "10.0.2.3", &config.vnameserver);
		config.vhostname = "uml-darwin";
		config.if_mtu = 1500;
		config.if_mru = 1500;
		slirp_.reset(slirp_new(&config, &callbacks(), this));
		if (!slirp_) {
			return -EIO;
		}
		try {
			/*
			 * ITIMER_REAL is UML's guest clock and must run only on its
			 * designated CPU pthread. Darwin otherwise may select this
			 * SLiRP worker for SIGALRM and enter Linux scheduler code on a
			 * foreign host stack.
			 */
			ScopedGuestTimerBlock timer_block;

			if (timer_block.error() != 0)
				throw std::system_error{
					timer_block.error(), std::generic_category()};
			worker_ = std::jthread{[this](std::stop_token stop_token) {
				event_loop(stop_token);
			}};
		} catch (const std::system_error &) {
			slirp_.reset();
			return -EIO;
		}
		started_.store(true, std::memory_order_release);
	}
	std::ranges::copy(default_mac, mac.begin());
	mtu = 1500;
	return 0;
}

int SlirpProvider::irq_fd() const
{
	std::lock_guard lock{start_mutex_};
	return started_.load(std::memory_order_acquire) ?
		receive_pipe_[0].get() : -1;
}

void SlirpProvider::stop() noexcept
{
	std::lock_guard lock{start_mutex_};
	if (!started_.load(std::memory_order_acquire))
		return;
	/* Close new guest I/O admission before waking the worker. Operations that
	 * already committed under a queue mutex retain descriptor ownership until
	 * that mutex is released; cleanup takes the same mutexes below. */
	started_.store(false, std::memory_order_release);
	stopping_.store(true, std::memory_order_release);
	worker_.request_stop();
#if UML_SLIRP_TESTING
	{
		std::lock_guard pause_lock{transmit_pause_mutex_};
		transmit_pause_requested_for_testing_.store(
			false, std::memory_order_release);
	}
	transmit_pause_condition_.notify_all();
#endif
	wake();
	if (worker_.joinable())
		worker_.join();
#if UML_SLIRP_TESTING
	notify_receive_cleanup_for_testing();
#endif
	{
		std::lock_guard receive_lock{receive_mutex_};
		receive_head_ = 0;
		receive_tail_ = 0;
		drain_pipe(receive_pipe_[0]);
	}
	{
		std::lock_guard transmit_lock{transmit_mutex_};
		transmit_head_ = 0;
		transmit_tail_ = 0;
		drain_pipe(wake_pipe_[0]);
	}
	slirp_.reset();
	timers_.clear();
}

#if UML_SLIRP_TESTING
void SlirpProvider::pause_transmit_for_testing() noexcept
{
	{
		std::lock_guard lock{transmit_pause_mutex_};
		transmit_pause_reached_for_testing_ = false;
		transmit_pause_requested_for_testing_.store(
			true, std::memory_order_release);
	}
	wake();
}

bool SlirpProvider::wait_for_transmit_pause_for_testing() noexcept
{
	std::unique_lock lock{transmit_pause_mutex_};
	return transmit_pause_condition_.wait_for(
		lock, std::chrono::seconds{2}, [this] {
			return transmit_pause_reached_for_testing_;
		});
}

void SlirpProvider::pause_next_write_before_commit_for_testing() noexcept
{
	std::lock_guard lock{io_pause_mutex_};
	write_pause_requested_for_testing_ = true;
	write_pause_reached_for_testing_ = false;
	release_write_for_testing_ = false;
}

bool SlirpProvider::wait_for_write_pause_for_testing() noexcept
{
	std::unique_lock lock{io_pause_mutex_};
	return io_pause_condition_.wait_for(
		lock, std::chrono::seconds{2}, [this] {
			return write_pause_reached_for_testing_;
		});
}

void SlirpProvider::release_write_for_testing() noexcept
{
	{
		std::lock_guard lock{io_pause_mutex_};
		release_write_for_testing_ = true;
	}
	io_pause_condition_.notify_all();
}

void SlirpProvider::pause_next_read_before_notification_for_testing() noexcept
{
	std::lock_guard lock{io_pause_mutex_};
	read_pause_requested_for_testing_ = true;
	read_pause_reached_for_testing_ = false;
	release_read_for_testing_ = false;
	receive_cleanup_reached_for_testing_ = false;
}

bool SlirpProvider::wait_for_read_pause_for_testing() noexcept
{
	std::unique_lock lock{io_pause_mutex_};
	return io_pause_condition_.wait_for(
		lock, std::chrono::seconds{2}, [this] {
			return read_pause_reached_for_testing_;
		});
}

void SlirpProvider::release_read_for_testing() noexcept
{
	{
		std::lock_guard lock{io_pause_mutex_};
		release_read_for_testing_ = true;
	}
	io_pause_condition_.notify_all();
}

bool SlirpProvider::wait_for_receive_cleanup_for_testing() noexcept
{
	std::unique_lock lock{io_pause_mutex_};
	return io_pause_condition_.wait_for(
		lock, std::chrono::seconds{2}, [this] {
			return receive_cleanup_reached_for_testing_;
		});
}

slirp_ssize_t SlirpProvider::enqueue_received_for_testing(
	std::span<const std::byte> frame) noexcept
{
	try {
		return enqueue_received(frame);
	} catch (...) {
		return -1;
	}
}

bool SlirpProvider::readiness_channels_drained_for_testing() const noexcept
{
	std::array<pollfd, 2> descriptors{{
		{.fd = wake_pipe_[0].get(), .events = POLLIN, .revents = 0},
		{.fd = receive_pipe_[0].get(), .events = POLLIN, .revents = 0},
	}};
	return descriptors[0].fd >= 0 && descriptors[1].fd >= 0 &&
		::poll(descriptors.data(), descriptors.size(), 0) == 0;
}

void SlirpProvider::pause_write_before_commit_for_testing() noexcept
{
	std::unique_lock lock{io_pause_mutex_};
	if (!write_pause_requested_for_testing_)
		return;
	write_pause_requested_for_testing_ = false;
	write_pause_reached_for_testing_ = true;
	io_pause_condition_.notify_all();
	io_pause_condition_.wait(lock, [this] {
		return release_write_for_testing_;
	});
}

void SlirpProvider::pause_read_before_notification_for_testing() noexcept
{
	std::unique_lock lock{io_pause_mutex_};
	if (!read_pause_requested_for_testing_)
		return;
	read_pause_requested_for_testing_ = false;
	read_pause_reached_for_testing_ = true;
	io_pause_condition_.notify_all();
	io_pause_condition_.wait(lock, [this] {
		return release_read_for_testing_;
	});
}

void SlirpProvider::notify_receive_cleanup_for_testing() noexcept
{
	{
		std::lock_guard lock{io_pause_mutex_};
		receive_cleanup_reached_for_testing_ = true;
	}
	io_pause_condition_.notify_all();
}
#endif

} // namespace uml::network

extern "C" int uml_slirp_net_open(
	void *context, unsigned char mac[6], unsigned int *mtu)
{
	if (context == nullptr || mac == nullptr || mtu == nullptr)
		return -EINVAL;
	if (const auto *disabled = std::getenv("UML_DISABLE_NETWORK");
	    disabled != nullptr && disabled[0] != '\0' && disabled[0] != '0')
		return -ENETDOWN;
	try {
		return static_cast<uml::network::SlirpProvider *>(context)->open(
			std::span<unsigned char, 6>{mac, 6}, *mtu);
	} catch (...) {
		return -EIO;
	}
}

extern "C" int uml_slirp_net_irq_fd(void *context)
{
	if (context == nullptr)
		return -1;
	try {
		return static_cast<uml::network::SlirpProvider *>(context)->irq_fd();
	} catch (...) {
		return -1;
	}
}

extern "C" int uml_slirp_net_read(
	void *context, void *buffer, std::size_t capacity)
{
	if (context == nullptr || (buffer == nullptr && capacity != 0))
		return -EINVAL;
	try {
		return static_cast<uml::network::SlirpProvider *>(context)->read(
			{static_cast<std::byte *>(buffer), capacity});
	} catch (...) {
		return -EIO;
	}
}

extern "C" int uml_slirp_net_write(
	void *context, const void *buffer, std::size_t length)
{
	if (context == nullptr || (buffer == nullptr && length != 0))
		return -EINVAL;
	try {
		return static_cast<uml::network::SlirpProvider *>(context)->write(
			{static_cast<const std::byte *>(buffer), length});
	} catch (...) {
		return -EIO;
	}
}

extern "C" void uml_slirp_net_stop(void *context)
{
	if (context != nullptr)
		static_cast<uml::network::SlirpProvider *>(context)->stop();
}

extern "C" void *uml_slirp_net_create(void) noexcept
{
	return new (std::nothrow) uml::network::SlirpProvider;
}

extern "C" void uml_slirp_net_destroy(void *context) noexcept
{
	delete static_cast<uml::network::SlirpProvider *>(context);
}
