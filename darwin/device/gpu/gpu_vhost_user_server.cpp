// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_vhost_user_server.hpp"

#include "deferred_fences.hpp"
#include "platform/descriptor.hpp"
#include "protocol.hpp"
#include "unique_descriptor.hpp"
#include "vhost_user_device.hpp"
#include "virtio_gpu_device.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/event.h>
#include <utility>
#include <vector>

namespace uml::gpu {
namespace {

template<typename Value>
Value loadUnaligned(const void* address)
{
    Value value {};
    std::memcpy(&value, address, sizeof(value));
    return value;
}

class GpuVhostUserDevice final : public uml::virtio::VhostUserDevice {
public:
    GpuVhostUserDevice(
        std::unique_ptr<Display> display,
        std::string vncBindAddress,
        std::uint32_t dotsPerInch)
        : ownedDisplay_(std::move(display))
        , display_(*ownedDisplay_)
        , vncBindAddress_(std::move(vncBindAddress))
        , gpu_(
              display_,
              [this](
                  std::uint64_t address,
                  std::span<std::uint8_t> destination) {
                  return readGuest(address, destination);
              },
              [this](std::uint64_t address, std::size_t length) {
                  return mapper_(address, length);
              },
              dotsPerInch)
        , hasEdid_(dotsPerInch != 0)
        , eventQueue_(::kqueue())
    {
        if (eventQueue_.get() < 0)
            throw std::runtime_error("creating GPU event queue failed");
        uml::platform::configureDescriptorOrThrow(
            eventQueue_.get(),
            {.closeOnExec = true},
            "close-on-exec descriptor");
        const int resizeDescriptor = display_.resizeEventDescriptor();
        if (resizeDescriptor >= 0) {
            struct kevent change {};
            EV_SET(
                &change,
                static_cast<std::uintptr_t>(resizeDescriptor),
                EVFILT_READ,
                EV_ADD | EV_ENABLE,
                0,
                0,
                nullptr);
            int result;
            do {
                result = ::kevent(
                    eventQueue_.get(), &change, 1, nullptr, 0, nullptr);
            } while (result < 0 && errno == EINTR);
            if (result < 0) {
                throw std::runtime_error(
                    "registering GPU display-resize event failed");
            }
        }
    }

    [[nodiscard]] const char* name() const noexcept override
    {
        return "Virtio-GPU";
    }

    [[nodiscard]] std::uint64_t features() const noexcept override
    {
        return protocol::virtioGpuFeatureVirgl |
            (hasEdid_ ? protocol::virtioGpuFeatureEdid : 0U) |
            protocol::virtioGpuFeatureResourceBlob |
            protocol::virtioGpuFeatureContextInit;
    }

    [[nodiscard]] std::size_t queueCount() const noexcept override
    {
        return protocol::gpuQueueCount;
    }

    [[nodiscard]] std::vector<std::uint8_t> configuration() const override
    {
        const auto config = gpu_.config();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&config);
        return {bytes, bytes + sizeof(config)};
    }

    void configurationRead(
        std::uint32_t offset,
        std::uint32_t size) noexcept override
    {
        gpu_.recordConfigRead(offset, size);
    }

    void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) override
    {
        constexpr std::uint32_t configSize = sizeof(protocol::GpuConfig);
        constexpr std::uint32_t eventsClearOffset =
            offsetof(protocol::GpuConfig, eventsClear);
        constexpr std::uint32_t eventsClearEnd =
            eventsClearOffset + sizeof(protocol::GpuConfig::eventsClear);
        const bool validFlag =
            flags == protocol::configFlagWritable ||
            flags == protocol::configFlagMigration;
        const bool validRange =
            !bytes.empty() && offset <= configSize &&
            bytes.size() <= configSize - offset;
        const bool writableRange =
            offset >= eventsClearOffset && offset <= eventsClearEnd &&
            bytes.size() <= eventsClearEnd - offset;
        if (!validFlag || !validRange ||
            (flags == protocol::configFlagWritable && !writableRange)) {
            throw std::runtime_error("invalid Virtio-GPU config write");
        }
        if (flags == protocol::configFlagWritable) {
            std::uint32_t clearEvents = 0;
            const auto relativeOffset =
                static_cast<std::size_t>(offset - eventsClearOffset);
            std::memcpy(
                reinterpret_cast<std::uint8_t*>(&clearEvents) + relativeOffset,
                bytes.data(),
                bytes.size());
            gpu_.clearEvents(clearEvents);
            if ((clearEvents & protocol::gpuEventDisplay) != 0 &&
                gpu_.displayEventPending()) {
                configChangePending_ = true;
            }
        }
    }

    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override
    {
        const bool cursorQueue = index == protocol::gpuCursorQueue;
        if (!cursorQueue &&
            responseCapacity < sizeof(protocol::GpuControlHeader)) {
            throw std::runtime_error("short Virtio-GPU response buffer");
        }
        if (!fences_.empty() && !fences_.orderingRelaxed() &&
            !gpu_.canProcessWhileFencesPending(request, cursorQueue)) {
            return {.action = uml::virtio::VhostUserQueueAction::retry};
        }

        const bool mayDefer = canDeferFence(cursorQueue, request);
        if (mayDefer && fences_.full())
            return {.action = uml::virtio::VhostUserQueueAction::retry};
        if (mayDefer && fences_.empty())
            armFenceTimer();

        auto result = gpu_.processWithDeferredFence(
            request, cursorQueue, requestId);
        if (result.response.size() > responseCapacity)
            throw std::runtime_error("short Virtio-GPU response buffer");
        if (result.destroyedContextId) {
            /* The context's held commands complete on the next tick. */
            const bool held = !fences_.empty();
            fences_.forgetContext(*result.destroyedContextId);
            if (held)
                armFenceTimer();
        }
        if (result.fence) {
            if (!mayDefer)
                throw std::runtime_error("invalid deferred GPU fence");
            fences_.defer(
                requestId,
                loadUnaligned<protocol::GpuControlHeader>(request.data()),
                std::move(result.response),
                *result.fence,
                DeferredFences::Clock::now());
            return {.action = uml::virtio::VhostUserQueueAction::defer};
        }
        if (fences_.empty() && gpu_.rendererHasPendingWork())
            armFenceTimer();
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = std::move(result.response),
        };
    }

    void reset() noexcept override
    {
        cancelFenceTimer();
        fences_.clear();
        gpu_.reset();
        configChangePending_ = false;
    }

    [[nodiscard]] int eventDescriptor() const noexcept override
    {
        return eventQueue_.get();
    }

    void drainEventSource() override
    {
        std::array<struct kevent, 2> events {};
        const timespec timeout {};
        int eventCount;
        do {
            eventCount = ::kevent(
                eventQueue_.get(),
                nullptr,
                0,
                events.data(),
                static_cast<int>(events.size()),
                &timeout);
        } while (eventCount < 0 && errno == EINTR);
        if (eventCount < 0)
            throw std::runtime_error("draining GPU events failed");

        bool resizeReady = false;
        bool fenceTimerReady = false;
        const int resizeDescriptor = display_.resizeEventDescriptor();
        for (int index = 0; index < eventCount; ++index) {
            const auto& event = events[static_cast<std::size_t>(index)];
            if ((event.flags & EV_ERROR) != 0)
                throw std::runtime_error("GPU event source failed");
            if (event.filter == EVFILT_TIMER &&
                event.ident == fenceTimerIdentifier) {
                fenceTimerReady = true;
                continue;
            }
            if (resizeDescriptor >= 0 && event.filter == EVFILT_READ &&
                event.ident ==
                    static_cast<std::uintptr_t>(resizeDescriptor)) {
                resizeReady = true;
                continue;
            }
            throw std::runtime_error("unexpected GPU event source");
        }

        if (resizeReady && display_.drainResizeEvent()) {
            gpu_.notifyDisplayChanged();
            configChangePending_ = true;
        }
        if (!fenceTimerReady)
            return;
        if (fences_.empty()) {
            /* A guest thread blocked on a query result spins on the buffer
             * the renderer fills and issues no further commands, so no fence
             * of its own is left to drive the scan that completes the query.
             * Keep polling for as long as the renderer owns such work. */
            if (!gpu_.rendererHasPendingWork())
                return;
            if (gpu_.pollRenderer() != 0)
                throw std::runtime_error("polling GPU fences failed");
            if (gpu_.rendererHasPendingWork())
                armFenceTimer();
            return;
        }

        /* A Virgl context retires through the renderer-wide fence scan, so
         * one poll drains every Virgl context. A Venus context owns its own
         * render-server timeline and retires only when it is polled itself,
         * so each distinct pending context needs its own poll. */
        fences_.forEachContext([this](std::uint32_t contextId) {
            if (gpu_.pollContextFences(contextId) != 0)
                throw std::runtime_error("polling GPU fences failed");
        });
        fences_.retire(
            DeferredFences::Clock::now(),
            [this](const DeferredFence& fence) {
                return gpu_.contextFenceStatus(fence);
            });
        if (!fences_.empty() || gpu_.rendererHasPendingWork())
            armFenceTimer();
    }

    [[nodiscard]] bool takeQueueCompletion(
        uml::virtio::VhostUserQueueCompletion& completion) override
    {
        DeferredFences::Completion completed;
        if (!fences_.takeCompletion(completed))
            return false;
        completion = {
            .requestId = completed.requestId,
            .response = std::move(completed.response),
        };
        return true;
    }

    [[nodiscard]] bool configChangePending() const noexcept override
    {
        return configChangePending_;
    }

    [[nodiscard]] bool claimConfigChange() noexcept override
    {
        return configChangePending_ && display_.claimResizeNotification();
    }

    void releaseConfigChange() noexcept override
    {
        display_.releaseResizeNotification();
    }

    void clearConfigChange() noexcept override
    {
        configChangePending_ = false;
    }

    void prepareReset() noexcept override
    {
        gpu_.prepareReset();
    }

    [[nodiscard]] std::optional<uml::virtio::VhostUserSharedMemoryRegion>
        sharedMemoryRegion() const noexcept override
    {
        return uml::virtio::VhostUserSharedMemoryRegion {
            .id = protocol::hostVisibleSharedMemoryRegionId,
            .size = protocol::hostVisibleSharedMemoryRegionSize,
        };
    }

    [[nodiscard]] bool takeSharedMemoryAction(
        uml::virtio::VhostUserSharedMemoryAction& destination) override
    {
        auto action = gpu_.takeSharedMemoryAction();
        if (!action)
            return false;
        destination = uml::virtio::VhostUserSharedMemoryAction {
            .type = action->type ==
                    VirtioGpuDevice::SharedMemoryAction::Type::map
                ? uml::virtio::VhostUserSharedMemoryAction::Type::map
                : uml::virtio::VhostUserSharedMemoryAction::Type::unmap,
            .actionId = action->actionId,
            .regionId = action->regionId,
            .descriptorOffset = action->descriptorOffset,
            .regionOffset = action->regionOffset,
            .length = action->length,
            .writable = action->writable,
            .descriptor = std::move(action->descriptor),
        };
        return true;
    }

    void completeSharedMemoryAction(
        std::uint64_t actionId,
        bool success) noexcept override
    {
        gpu_.completeSharedMemoryAction(actionId, success);
    }

    void setStopRequest(
        const std::atomic_bool* stopRequested) noexcept override
    {
        gpu_.setStopRequest(stopRequested);
    }

    void setGuestMapper(GuestMapper mapper) override
    {
        mapper_ = std::move(mapper);
    }

    [[nodiscard]] std::string readinessDescription() const override
    {
        const auto size = std::to_string(display_.width()) + "x" +
            std::to_string(display_.height());
        if (const auto port = display_.vncPort()) {
            return "VNC display: " + vncBindAddress_ + ":" +
                std::to_string(static_cast<unsigned int>(*port)) + " (" +
                size + ")\n";
        }
        return "Native Metal display: " + size + "\n";
    }

private:
    static constexpr std::uintptr_t fenceTimerIdentifier = 1;

    /* The transport's own translation: a range that one mapped region holds
     * whole, copied out. */
    [[nodiscard]] bool readGuest(
        std::uint64_t address,
        std::span<std::uint8_t> destination)
    {
        const auto source = mapper_(address, destination.size());
        if (source.data() == nullptr)
            return false;
        std::memcpy(destination.data(), source.data(), destination.size());
        return true;
    }
    static constexpr std::intptr_t fencePollMicroseconds = 100;

    [[nodiscard]] bool canDeferFence(
        bool cursorQueue,
        std::span<const std::uint8_t> request) const noexcept
    {
        if (cursorQueue || request.size() < sizeof(protocol::GpuControlHeader))
            return false;
        const auto header =
            loadUnaligned<protocol::GpuControlHeader>(request.data());
        const auto command = static_cast<protocol::GpuCommand>(header.type);
        const bool fence = (header.flags & protocol::gpuFlagFence) != 0;
        const bool pipelineCommand =
            command == protocol::GpuCommand::submit3d ||
            command == protocol::GpuCommand::transferToHost3d ||
            command == protocol::GpuCommand::transferFromHost3d;
        /* Virgl retires one fence timeline per context and Venus one per
         * guest ring. The device knows each context's capset, so it decides
         * which rings defer and completes the rest synchronously. */
        return pipelineCommand && fence && header.contextId != 0;
    }

    void armFenceTimer()
    {
        struct kevent change {};
        EV_SET(
            &change,
            fenceTimerIdentifier,
            EVFILT_TIMER,
            EV_ADD | EV_ENABLE | EV_ONESHOT,
            NOTE_USECONDS,
            fencePollMicroseconds,
            nullptr);
        int result;
        do {
            result = ::kevent(
                eventQueue_.get(), &change, 1, nullptr, 0, nullptr);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            throw std::runtime_error("arming GPU fence timer failed");
    }

    void cancelFenceTimer() noexcept
    {
        struct kevent change {};
        EV_SET(
            &change,
            fenceTimerIdentifier,
            EVFILT_TIMER,
            EV_DELETE,
            0,
            0,
            nullptr);
        while (::kevent(
                   eventQueue_.get(), &change, 1, nullptr, 0, nullptr) < 0 &&
            errno == EINTR) {
        }
    }

    std::unique_ptr<Display> ownedDisplay_;
    Display& display_;
    std::string vncBindAddress_;
    GuestMapper mapper_;
    VirtioGpuDevice gpu_;
    bool hasEdid_;
    bool configChangePending_ {};
    UniqueDescriptor eventQueue_;
    DeferredFences fences_;
};

} // namespace

std::unique_ptr<VhostUserServer> makeGpuVhostUserServer(
    std::filesystem::path socketPath,
    std::string vncBindAddress,
    std::uint16_t vncPort,
    std::string vncPassword,
    std::uint32_t width,
    std::uint32_t height,
    bool selfTestPattern,
    uml::input::vnc::VncInputSink inputSink,
    DisplayMode displayMode,
    void* nativeView,
    std::uint32_t dotsPerInch,
    uml::input::vnc::PointerMode pointerMode,
    bool autoResizeGuest)
{
    auto display = makeDisplay(
        displayMode,
        vncBindAddress,
        vncPort,
        std::move(vncPassword),
        width,
        height,
        std::move(inputSink),
        nativeView,
        pointerMode,
        dotsPerInch == 0 ?
            protocol::maximumDisplayDimension :
            protocol::maximumEdidDimension,
        autoResizeGuest);
    auto& presented = *display;
    auto server = std::make_unique<VhostUserServer>(
        std::make_unique<GpuVhostUserDevice>(
            std::move(display), std::move(vncBindAddress), dotsPerInch),
        std::move(socketPath));
    if (selfTestPattern)
        presented.fillSelfTestPattern();
    return server;
}

} // namespace uml::gpu
