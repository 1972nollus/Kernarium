// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "vhost_user_server.hpp"

#include <atomic>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

namespace uml::gpu {
namespace {

class ScopedGuestTimerBlock final {
public:
    ScopedGuestTimerBlock()
    {
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGALRM);
        const int error = ::pthread_sigmask(
            SIG_BLOCK, &blocked, &previous_);
        if (error != 0) {
            throw std::system_error(
                error,
                std::generic_category(),
                "blocking the UML guest timer around GPU thread creation");
        }
        active_ = true;
    }

    ScopedGuestTimerBlock(const ScopedGuestTimerBlock&) = delete;
    ScopedGuestTimerBlock& operator=(const ScopedGuestTimerBlock&) = delete;

    ~ScopedGuestTimerBlock()
    {
        if (active_)
            (void)::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

private:
    sigset_t previous_ {};
    bool active_ {};
};

std::array<int, 2> makeSocketPair()
{
    std::array<int, 2> descriptors {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) != 0) {
        throw std::runtime_error(
            "in-process vhost-user socketpair failed: " +
            std::string(std::strerror(errno)));
    }
    return descriptors;
}

std::array<int, 2> makeInputSocketPair()
{
    std::array<int, 2> descriptors {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors.data()) != 0) {
        throw std::runtime_error(
            "in-process VNC input socketpair failed: " +
            std::string(std::strerror(errno)));
    }
    const int flags = ::fcntl(descriptors[0], F_GETFL);
    if (flags < 0 ||
        ::fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        const int error = errno;
        ::close(descriptors[0]);
        ::close(descriptors[1]);
        throw std::runtime_error(
            "configuring VNC input socket failed: " +
            std::string(std::strerror(error)));
    }
    int noSigpipe = 1;
    (void)::setsockopt(
        descriptors[0],
        SOL_SOCKET,
        SO_NOSIGPIPE,
        &noSigpipe,
        sizeof(noSigpipe));
    return descriptors;
}

} // namespace

class InProcessGpuService::Implementation {
public:
    explicit Implementation(InProcessGpuOptions options)
        : options_(std::move(options))
        , descriptors_(makeSocketPair())
        , inputDescriptors_(makeInputSocketPair())
        , connectionName_("fd=" + std::to_string(descriptors_[1]))
        , inputConnectionName_(
              "fd=" + std::to_string(inputDescriptors_[1]))
        , thread_([this] { run(); })
    {
        std::unique_lock lock(stateMutex_);
        stateChanged_.wait(lock, [this] { return ready_ || failure_; });
        if (failure_) {
            lock.unlock();
            thread_.join();
            std::rethrow_exception(failure_);
        }
    }

    ~Implementation()
    {
        stop();
        if (inputDescriptors_[0] >= 0)
            ::close(std::exchange(inputDescriptors_[0], -1));
    }

    const std::string& connectionName() const noexcept
    {
        return connectionName_;
    }

    const std::string& inputConnectionName() const noexcept
    {
        return inputConnectionName_;
    }

    void stop() noexcept
    {
        stopRequested_.store(true, std::memory_order_release);
        if (thread_.joinable())
            thread_.join();
    }

private:
    void run() noexcept
    {
        (void)::pthread_setname_np("uml-gpu");
        try {
            /*
             * CGL current contexts are pthread-local. Construct and destroy
             * virglrenderer on the transport thread so its initialization
             * context remains current when the guest later requests capsets
             * and submits commands on this same thread.
             */
            VhostUserServer server(
                {},
                options_.vncBindAddress,
                options_.vncPort,
                options_.vncPassword,
                options_.width,
                options_.height,
                false,
                [this](std::span<const VncInputEvent> events) {
                    sendInput(events);
                });
            server.runConnected(descriptors_[0], stopRequested_, [this] {
                {
                    std::lock_guard lock(stateMutex_);
                    ready_ = true;
                }
                stateChanged_.notify_one();
            });
        } catch (...) {
            {
                std::lock_guard lock(stateMutex_);
                failure_ = std::current_exception();
            }
            stateChanged_.notify_one();
        }
    }

    void sendInput(std::span<const VncInputEvent> events) noexcept
    {
        const auto bytes = events.size_bytes();
        const auto sent = ::send(
            inputDescriptors_[0], events.data(), bytes, MSG_DONTWAIT);
        if (sent == static_cast<ssize_t>(bytes)) {
            return;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        if (!reportedInputFailure_.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(
                stderr,
                "Unable to forward VNC input to UML: %s\n",
                sent < 0 ? std::strerror(errno) : "short datagram");
        }
    }

    InProcessGpuOptions options_;
    std::array<int, 2> descriptors_;
    std::array<int, 2> inputDescriptors_;
    std::string connectionName_;
    std::string inputConnectionName_;
    std::atomic_bool stopRequested_ {};
    std::atomic_bool reportedInputFailure_ {};
    std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    bool ready_ {};
    std::exception_ptr failure_;
    std::thread thread_;
};

InProcessGpuService::InProcessGpuService(InProcessGpuOptions options)
{
    /*
     * Darwin delivers process-wide ITIMER_REAL ticks to any pthread which
     * leaves SIGALRM unblocked. UML's handler enters the Linux timer and
     * scheduler, so running it on a VNC, virglrenderer, Metal, or transport
     * worker corrupts that host thread's context. Block the signal while all
     * initial GPU workers are constructed; later renderer workers inherit the
     * transport thread's blocked mask as well. The calling UML CPU regains its
     * original mask when this scope ends.
     */
    ScopedGuestTimerBlock timerBlock;
    implementation_ =
        std::make_unique<Implementation>(std::move(options));
}

InProcessGpuService::~InProcessGpuService() = default;

const std::string& InProcessGpuService::connectionName() const noexcept
{
    return implementation_->connectionName();
}

const std::string& InProcessGpuService::inputConnectionName() const noexcept
{
    return implementation_->inputConnectionName();
}

void InProcessGpuService::stop() noexcept
{
    implementation_->stop();
}

} // namespace uml::gpu
