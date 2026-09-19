// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "unique_descriptor.hpp"
#include "vhost_user_server.hpp"

#ifndef UML_GPU_SERVICE_TRANSPORT_TESTING
#define UML_GPU_SERVICE_TRANSPORT_TESTING 0
#endif

#if UML_GPU_STARTUP_TESTING || UML_GPU_SERVICE_TRANSPORT_TESTING
#include "in_process_service_testing.hpp"
#endif

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

using DescriptorPair = std::array<UniqueDescriptor, 2>;

void setCloseOnExec(const DescriptorPair& descriptors)
{
    for (const auto& descriptor : descriptors) {
        const int flags = ::fcntl(descriptor.get(), F_GETFD);
        if (flags < 0 ||
            ::fcntl(
                descriptor.get(), F_SETFD, flags | FD_CLOEXEC) != 0) {
            throw std::runtime_error(
                "configuring in-process GPU descriptor failed: " +
                std::string(std::strerror(errno)));
        }
    }
}

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

DescriptorPair makeSocketPair()
{
    std::array<int, 2> descriptors {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) != 0) {
        throw std::runtime_error(
            "in-process vhost-user socketpair failed: " +
            std::string(std::strerror(errno)));
    }
    DescriptorPair result {
        UniqueDescriptor {descriptors[0]}, UniqueDescriptor {descriptors[1]}};
    setCloseOnExec(result);
#if UML_GPU_STARTUP_TESTING
    testing::startupCheckpoint(
        testing::StartupFailure::afterTransportSocketPair);
#endif
    return result;
}

UniqueDescriptor duplicateDescriptor(int descriptor)
{
    const int duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        throw std::runtime_error(
            "duplicating in-process GPU stop descriptor failed: " +
            std::string(std::strerror(errno)));
    }
    return UniqueDescriptor {duplicate};
}

DescriptorPair makeInputSocketPair()
{
    std::array<int, 2> descriptors {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors.data()) != 0) {
        throw std::runtime_error(
            "in-process display input socketpair failed: " +
            std::string(std::strerror(errno)));
    }
    DescriptorPair result {
        UniqueDescriptor {descriptors[0]}, UniqueDescriptor {descriptors[1]}};
    setCloseOnExec(result);
    const int flags = ::fcntl(result[0].get(), F_GETFL);
    if (flags < 0 ||
        ::fcntl(result[0].get(), F_SETFL, flags | O_NONBLOCK) != 0) {
        const int error = errno;
        throw std::runtime_error(
            "configuring display input socket failed: " +
            std::string(std::strerror(error)));
    }
    if (::fcntl(result[0].get(), F_SETNOSIGPIPE, 1) != 0) {
        const int error = errno;
        throw std::runtime_error(
            "configuring display input no-signal writes failed: " +
            std::string(std::strerror(error)));
    }
#if UML_GPU_STARTUP_TESTING
    testing::startupCheckpoint(
        testing::StartupFailure::afterInputSocketPair);
#endif
    return result;
}

} // namespace

class InProcessGpuService::Implementation {
public:
    explicit Implementation(InProcessGpuOptions options)
        : options_(std::move(options))
        , descriptors_(makeSocketPair())
        , stopDescriptor_(duplicateDescriptor(descriptors_[0].get()))
        , inputDescriptors_(options_.inputSink ? DescriptorPair{} : makeInputSocketPair())
        , connectionName_("fd=" + std::to_string(descriptors_[1].get()))
        , inputConnectionName_(
              options_.inputSink ? std::string{} :
              "fd=" + std::to_string(inputDescriptors_[1].get()))
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
    }

    std::string takeConnectionName()
    {
        if (descriptors_[1].get() < 0)
            throw std::logic_error("GPU transport endpoint already transferred");
        auto name = connectionName_;
        (void)descriptors_[1].release();
        return name;
    }

    std::string takeInputConnectionName()
    {
        if (inputDescriptors_[1].get() < 0)
            throw std::logic_error("GPU input endpoint already transferred");
        auto name = inputConnectionName_;
        (void)inputDescriptors_[1].release();
        return name;
    }

    void stop() noexcept
    {
        stopRequested_.store(true, std::memory_order_release);
        // Keep a separately owned duplicate of the worker endpoint solely as
        // a wake capability.  shutdown(2) interrupts poll plus any partial
        // frame read/write without closing a descriptor number that could
        // already have been transferred to, closed by, and reused in UML.
        if (stopDescriptor_.get() >= 0)
            (void)::shutdown(stopDescriptor_.get(), SHUT_RDWR);
        if (thread_.joinable())
            thread_.join();
    }

private:
    void run() noexcept
    {
        (void)::pthread_setname_np("uml-gpu");
        try {
#if UML_GPU_STARTUP_TESTING
            testing::startupCheckpoint(testing::StartupFailure::workerEntry);
#endif
            /*
             * EGL current contexts are thread-local. Construct, use, reset,
             * and destroy the single virglrenderer owner on the transport
             * thread; provider context creation is serialized, while current
             * context and guest submission stay on this one thread.
             */
            const auto ready = [this] {
                {
                    std::lock_guard lock(stateMutex_);
                    ready_ = true;
                }
                stateChanged_.notify_one();
            };
#if UML_GPU_SERVICE_TRANSPORT_TESTING
            auto server = testing::makeTransportTestServer();
            server->runConnected(
                descriptors_[0].release(), stopRequested_, ready);
#else
            VhostUserServer server(
                {},
                options_.vncBindAddress,
                options_.vncPort,
                options_.vncPassword,
                options_.width,
                options_.height,
                false,
                [this](
                    std::span<const uml::input::vnc::VncInputEvent> events) {
                    sendInput(events);
                },
                options_.displayMode,
                options_.nativeView,
                options_.dotsPerInch,
                options_.pointerMode,
                options_.autoResizeGuest);
            server.runConnected(
                descriptors_[0].release(), stopRequested_, ready);
#endif
        } catch (...) {
            {
                std::lock_guard lock(stateMutex_);
                failure_ = std::current_exception();
            }
            stateChanged_.notify_one();
        }
    }

    void sendInput(
        std::span<const uml::input::vnc::VncInputEvent> events) noexcept
    {
        if (options_.inputSink) {
            if (!uml::input::vnc::tryDeliver(options_.inputSink, events) &&
                !reportedInputFailure_.exchange(true, std::memory_order_relaxed)) {
                std::fputs("Unable to deliver display input to composed devices\n", stderr);
            }
            return;
        }
        const auto bytes = events.size_bytes();
        const auto sent = ::send(
            inputDescriptors_[0].get(), events.data(), bytes, MSG_DONTWAIT);
        if (sent == static_cast<ssize_t>(bytes)) {
            return;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        if (!reportedInputFailure_.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(
                stderr,
                "Unable to forward display input to UML: %s\n",
                sent < 0 ? std::strerror(errno) : "short datagram");
        }
    }

    InProcessGpuOptions options_;
    DescriptorPair descriptors_;
    UniqueDescriptor stopDescriptor_;
    DescriptorPair inputDescriptors_;
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

std::string InProcessGpuService::takeConnectionName()
{
    return implementation_->takeConnectionName();
}

std::string InProcessGpuService::takeInputConnectionName()
{
    return implementation_->takeInputConnectionName();
}

void InProcessGpuService::stop() noexcept
{
    implementation_->stop();
}

} // namespace uml::gpu
