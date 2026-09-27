// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "video_decoder.hpp"
#include "unique_descriptor.hpp"
#include "vhost_user_server.hpp"
#include "virtio_video_device.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <sys/socket.h>

namespace uml::video {
namespace {

using DescriptorPair = std::array<uml::gpu::UniqueDescriptor, 2>;

DescriptorPair makeSocketPair()
{
    std::array<int, 2> descriptors {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) != 0) {
        throw std::runtime_error(
            "in-process Virtio video socketpair failed: " +
            std::string(std::strerror(errno)));
    }
    DescriptorPair result {
        uml::gpu::UniqueDescriptor {descriptors[0]},
        uml::gpu::UniqueDescriptor {descriptors[1]},
    };
    for (const auto& descriptor : result) {
        const int flags = ::fcntl(descriptor.get(), F_GETFD);
        if (flags < 0 ||
            ::fcntl(descriptor.get(), F_SETFD, flags | FD_CLOEXEC) != 0) {
            throw std::runtime_error(
                "configuring in-process video descriptor failed: " +
                std::string(std::strerror(errno)));
        }
    }
    return result;
}

uml::gpu::UniqueDescriptor duplicateDescriptor(int descriptor)
{
    const int duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        throw std::runtime_error(
            "duplicating in-process video stop descriptor failed: " +
            std::string(std::strerror(errno)));
    }
    return uml::gpu::UniqueDescriptor {duplicate};
}

class ScopedGuestTimerBlock final {
public:
    ScopedGuestTimerBlock()
    {
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGALRM);
        const int error = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous_);
        if (error != 0) {
            throw std::system_error(
                error,
                std::generic_category(),
                "blocking UML timer around Virtio video thread creation");
        }
        active_ = true;
    }

    ~ScopedGuestTimerBlock()
    {
        if (active_)
            (void)::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

private:
    sigset_t previous_ {};
    bool active_ {};
};

} // namespace

class InProcessVideoService::Implementation final {
public:
    explicit Implementation(
        std::shared_ptr<uml::virtio::SharedMemoryRegions> regions)
        : regions_(std::move(regions))
        , descriptors_(makeSocketPair())
        , stopDescriptor_(duplicateDescriptor(descriptors_[0].get()))
        , connectionName_("fd=" + std::to_string(descriptors_[1].get()))
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

    ~Implementation() { stop(); }

    std::string takeConnectionName()
    {
        if (descriptors_[1].get() < 0)
            throw std::logic_error("Virtio video endpoint already transferred");
        auto name = connectionName_;
        (void)descriptors_[1].release();
        return name;
    }

    void stop() noexcept
    {
        stopRequested_.store(true, std::memory_order_release);
        if (stopDescriptor_.get() >= 0)
            (void)::shutdown(stopDescriptor_.get(), SHUT_RDWR);
        if (thread_.joinable())
            thread_.join();
    }

private:
    void run() noexcept
    {
        (void)::pthread_setname_np("uml-video");
        try {
            auto device = std::make_unique<VirtioVideoDevice>(
                uml::platform::videoDecodeCapabilities(), uml::platform::makeVideoDecoder);
            uml::gpu::VhostUserServer server(std::move(device));
            if (regions_)
                server.setSharedMemoryRegions(regions_);
            server.runConnected(
                descriptors_[0].release(), stopRequested_, [this] {
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

    std::shared_ptr<uml::virtio::SharedMemoryRegions> regions_;
    DescriptorPair descriptors_;
    uml::gpu::UniqueDescriptor stopDescriptor_;
    std::string connectionName_;
    std::atomic_bool stopRequested_ {};
    std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    bool ready_ {};
    std::exception_ptr failure_;
    std::thread thread_;
};

InProcessVideoService::InProcessVideoService(
    std::shared_ptr<uml::virtio::SharedMemoryRegions> regions)
{
    ScopedGuestTimerBlock timerBlock;
    implementation_ = std::make_unique<Implementation>(std::move(regions));
}

InProcessVideoService::~InProcessVideoService() = default;

std::string InProcessVideoService::takeConnectionName()
{
    return implementation_->takeConnectionName();
}

void InProcessVideoService::stop() noexcept
{
    implementation_->stop();
}

} // namespace uml::video
