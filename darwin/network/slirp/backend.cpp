// SPDX-License-Identifier: GPL-2.0-or-later
#include "backend.hpp"

extern "C" {
#include "../../include/uml_darwin_runtime.h"
}

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <system_error>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

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

SlirpBackend &SlirpBackend::instance()
{
	static SlirpBackend backend;
	return backend;
}

SlirpBackend::~SlirpBackend()
{
	stop();
}

int SlirpBackend::open(std::span<unsigned char, 6> mac,
		       unsigned int &mtu) noexcept
{
	std::lock_guard lock{start_mutex_};
	if (!started_.load(std::memory_order_acquire)) {
		if (!create_pipe(wake_pipe_) || !create_pipe(receive_pipe_))
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
			wake_pipe_[0].reset();
			wake_pipe_[1].reset();
			receive_pipe_[0].reset();
			receive_pipe_[1].reset();
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
			wake_pipe_[0].reset();
			wake_pipe_[1].reset();
			receive_pipe_[0].reset();
			receive_pipe_[1].reset();
			return -EIO;
		}
		started_.store(true, std::memory_order_release);
	}
	std::ranges::copy(default_mac, mac.begin());
	mtu = 1500;
	return 0;
}

int SlirpBackend::irq_fd() const noexcept
{
	std::lock_guard lock{start_mutex_};
	return started_.load(std::memory_order_acquire) ?
		receive_pipe_[0].get() : -1;
}

void SlirpBackend::stop() noexcept
{
	std::lock_guard lock{start_mutex_};
	if (!started_.load(std::memory_order_acquire))
		return;
	worker_.request_stop();
	wake();
	if (worker_.joinable())
		worker_.join();
	started_.store(false, std::memory_order_release);
	slirp_.reset();
	timers_.clear();
	wake_pipe_[0].reset();
	wake_pipe_[1].reset();
	receive_pipe_[0].reset();
	receive_pipe_[1].reset();
}

} // namespace uml::network

extern "C" int uml_slirp_net_open(
	void *, unsigned char mac[6], unsigned int *mtu)
{
	if (mac == nullptr || mtu == nullptr)
		return -EINVAL;
	if (const auto *disabled = std::getenv("UML_DISABLE_NETWORK");
	    disabled != nullptr && disabled[0] != '\0' && disabled[0] != '0')
		return -ENETDOWN;
	return uml::network::SlirpBackend::instance().open(
		std::span<unsigned char, 6>{mac, 6}, *mtu);
}

extern "C" int uml_slirp_net_irq_fd(void *)
{
	return uml::network::SlirpBackend::instance().irq_fd();
}

extern "C" int uml_slirp_net_read(
	void *, void *buffer, std::size_t capacity)
{
	if (buffer == nullptr && capacity != 0)
		return -EINVAL;
	return uml::network::SlirpBackend::instance().read(
		{static_cast<std::byte *>(buffer), capacity});
}

extern "C" int uml_slirp_net_write(
	void *, const void *buffer, std::size_t length)
{
	if (buffer == nullptr && length != 0)
		return -EINVAL;
	return uml::network::SlirpBackend::instance().write(
		{static_cast<const std::byte *>(buffer), length});
}

extern "C" void uml_slirp_net_stop(void)
{
	uml::network::SlirpBackend::instance().stop();
}
