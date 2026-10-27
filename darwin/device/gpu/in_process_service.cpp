// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "gpu_vhost_user_server.hpp"
#include "platform/descriptor.hpp"
#include "request_gate.hpp"
#include "unique_descriptor.hpp"
#include "vhost_user_server.hpp"
#include "virgl_renderer.hpp"

#ifndef UML_GPU_SERVICE_TRANSPORT_TESTING
#define UML_GPU_SERVICE_TRANSPORT_TESTING 0
#endif

#if UML_GPU_STARTUP_TESTING || UML_GPU_SERVICE_TRANSPORT_TESTING
#include "in_process_service_testing.hpp"
#endif

#include <atomic>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

namespace uml::gpu {
namespace {

using DescriptorPair = std::array<UniqueDescriptor, 2>;

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
    for (const auto& descriptor : result) {
        uml::platform::configureDescriptorOrThrow(
            descriptor.get(),
            {.closeOnExec = true},
            "in-process GPU descriptor");
    }
    uml::platform::configureDescriptorOrThrow(
        result[0].get(), {.nonblocking = true}, "display input socket");
    uml::platform::configureDescriptorOrThrow(
        result[0].get(),
        {.noSigpipe = true},
        "display input no-signal writes");
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
    {
        /* The input pair first, as it has always been: a start-up that fails
         * later gives its descriptors back with the members. */
        input_ = makeInputChannel();
        service_ = makeService();
    }

    bool holdRequests(std::chrono::milliseconds budget) noexcept
    {
        return requestGate_->hold(budget);
    }

    void releaseRequests() noexcept
    {
        requestGate_->release();
    }

    bool requestsHeld() const noexcept
    {
        return requestGate_->held();
    }

    std::string takeConnectionName()
    {
        std::lock_guard lock(connectionMutex_);
        if (!service_)
            throw std::logic_error("GPU has no connection");
        return service_->takeConnectionName();
    }

    std::string takeInputConnectionName()
    {
        std::lock_guard lock(inputMutex_);
        if (input_.descriptors[1].get() < 0)
            throw std::logic_error("GPU input endpoint already transferred");
        auto name = input_.connectionName;
        (void)input_.descriptors[1].release();
        return name;
    }

    void startNextConnection()
    {
        /* Everything that can fail without touching the connection that is
         * there comes first. */
        auto input = makeInputChannel();
        std::lock_guard lock(connectionMutex_);
        if (stopped_)
            throw std::logic_error("GPU service is stopped");
        if (service_) {
            service_->stop();
            service_.reset();
        }
        {
            /* Input from the display has nowhere to go until the new guest
             * has its endpoint, and a guest that is not there yet must not
             * find the clicks of the one before it. */
            std::lock_guard inputLock(inputMutex_);
            input_ = {};
        }
        service_ = makeService();
        std::lock_guard inputLock(inputMutex_);
        input_ = std::move(input);
        reportedInputFailure_.store(false, std::memory_order_relaxed);
    }

    void stop() noexcept
    {
        {
            std::lock_guard lock(connectionMutex_);
            stopped_ = true;
            if (service_)
                service_->stop();
        }
        /* The display goes with the service; the last reference to it is the
         * connection's, which has just ended. */
        std::shared_ptr<Display> display;
        {
            std::lock_guard lock(displayMutex_);
            display = std::move(display_);
        }
    }

    std::optional<std::uint16_t> vncPort() const noexcept
    {
        std::lock_guard lock(displayMutex_);
        return display_ ? display_->vncPort() : std::nullopt;
    }

private:
    struct InputChannel {
        DescriptorPair descriptors {};
        std::string connectionName;
    };

    [[nodiscard]] static InputChannel makeInputChannel()
    {
        InputChannel channel;
        channel.descriptors = makeInputSocketPair();
        channel.connectionName =
            "fd=" + std::to_string(channel.descriptors[1].get());
        return channel;
    }

    [[nodiscard]] std::unique_ptr<InProcessVhostUserService> makeService()
    {
        return std::make_unique<InProcessVhostUserService>(
            "GPU",
            "uml-gpu",
            InProcessVhostUserService::ServerFactory(
                [this] { return makeServer(); }),
            options_.sharedMemoryRegions,
            requestGate_);
    }

    /*
     * Runs on the service thread. EGL current contexts are thread-local, so
     * the single virglrenderer owner is constructed, used, reset and destroyed
     * on that thread; provider context creation is serialized, while current
     * context and guest submission stay on this one thread. The display is
     * made once, by the first connection on its thread with the guest timer
     * blocked as every worker of it needs, and every later connection borrows
     * it.
     */
    std::unique_ptr<VhostUserServer> makeServer()
    {
#if UML_GPU_STARTUP_TESTING
        testing::startupCheckpoint(testing::StartupFailure::workerEntry);
#endif
#if UML_GPU_SERVICE_TRANSPORT_TESTING
        return testing::makeTransportTestServer();
#else
        std::shared_ptr<Display> display;
        {
            std::lock_guard lock(displayMutex_);
            display = display_;
        }
        if (!display) {
            display = makeGpuDisplay(
                options_.vncBindAddress,
                options_.vncPort,
                options_.vncPassword,
                options_.width,
                options_.height,
                [this](
                    std::span<const uml::input::vnc::VncInputEvent> events) {
                    sendInput(events);
                },
                options_.displayMode,
                options_.nativeView,
                options_.dotsPerInch,
                options_.pointerMode,
                options_.autoResizeGuest);
            std::lock_guard lock(displayMutex_);
            display_ = display;
        }
        return makeGpuVhostUserServer(
            std::move(display), options_.vncBindAddress, options_.dotsPerInch);
#endif
    }

    void sendInput(
        std::span<const uml::input::vnc::VncInputEvent> events) noexcept
    {
        const auto bytes = events.size_bytes();
        std::lock_guard lock(inputMutex_);
        if (input_.descriptors[0].get() < 0)
            return;
        const auto sent = ::send(
            input_.descriptors[0].get(), events.data(), bytes, MSG_DONTWAIT);
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
    /* The channel display input travels on, to the guest that has the other
     * end. Taken by the display's input thread, so it has a lock of its own. */
    std::mutex inputMutex_;
    InputChannel input_;
    std::atomic_bool reportedInputFailure_ {};
    std::shared_ptr<RequestGate> requestGate_ {
        std::make_shared<RequestGate>()};
    mutable std::mutex displayMutex_;
    std::shared_ptr<Display> display_;
    std::mutex connectionMutex_;
    bool stopped_ {};
    /* Last, so the service thread, which forwards input through the members
     * above, is stopped before any of them goes. */
    std::unique_ptr<InProcessVhostUserService> service_;
};

InProcessGpuService::InProcessGpuService(InProcessGpuOptions options)
{
    /* The service starts its thread with the guest timer blocked, and every
     * VNC, virglrenderer and Metal worker started from that thread inherits
     * the mask. */
    VirglRenderer::setDeviceMemoryLimit(options.deviceMemoryLimit);
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

void InProcessGpuService::startNextConnection()
{
    implementation_->startNextConnection();
}

void InProcessGpuService::stop() noexcept
{
    implementation_->stop();
}

bool InProcessGpuService::holdRequests(
    std::chrono::milliseconds budget) noexcept
{
    return implementation_->holdRequests(budget);
}

void InProcessGpuService::releaseRequests() noexcept
{
    implementation_->releaseRequests();
}

bool InProcessGpuService::requestsHeld() const noexcept
{
    return implementation_->requestsHeld();
}

std::optional<std::uint16_t> InProcessGpuService::vncPort() const noexcept
{
    return implementation_->vncPort();
}

} // namespace uml::gpu
