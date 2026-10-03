// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "gpu_vhost_user_server.hpp"
#include "platform/descriptor.hpp"
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
        , inputDescriptors_(makeInputSocketPair())
        , inputConnectionName_(
              "fd=" + std::to_string(inputDescriptors_[1].get()))
        , service_(
              "GPU",
              "uml-gpu",
              InProcessVhostUserService::ServerFactory(
                  [this] { return makeServer(); }),
              options_.sharedMemoryRegions)
    {
    }

    std::string takeConnectionName()
    {
        return service_.takeConnectionName();
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
        service_.stop();
    }

private:
    /*
     * Runs on the service thread. EGL current contexts are thread-local, so
     * the single virglrenderer owner is constructed, used, reset and destroyed
     * on that thread; provider context creation is serialized, while current
     * context and guest submission stay on this one thread.
     */
    std::unique_ptr<VhostUserServer> makeServer()
    {
#if UML_GPU_STARTUP_TESTING
        testing::startupCheckpoint(testing::StartupFailure::workerEntry);
#endif
#if UML_GPU_SERVICE_TRANSPORT_TESTING
        return testing::makeTransportTestServer();
#else
        return makeGpuVhostUserServer(
            std::filesystem::path {},
            options_.vncBindAddress,
            options_.vncPort,
            options_.vncPassword,
            options_.width,
            options_.height,
            false,
            [this](std::span<const uml::input::vnc::VncInputEvent> events) {
                sendInput(events);
            },
            options_.displayMode,
            options_.nativeView,
            options_.dotsPerInch,
            options_.pointerMode,
            options_.autoResizeGuest);
#endif
    }

    void sendInput(
        std::span<const uml::input::vnc::VncInputEvent> events) noexcept
    {
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
    DescriptorPair inputDescriptors_;
    std::string inputConnectionName_;
    std::atomic_bool reportedInputFailure_ {};
    /* Last, so the service thread, which forwards input through the members
     * above, is stopped before any of them goes. */
    InProcessVhostUserService service_;
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

void InProcessGpuService::stop() noexcept
{
    implementation_->stop();
}

} // namespace uml::gpu
