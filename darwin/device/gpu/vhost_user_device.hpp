// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "unique_descriptor.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace uml::virtio {

enum class VhostUserQueueAction : std::uint8_t {
    retry,
    complete,
    defer,
};

struct VhostUserQueueResult final {
    VhostUserQueueAction action {VhostUserQueueAction::retry};
    std::vector<std::uint8_t> response;
};

struct VhostUserQueueCompletion final {
    std::uint64_t requestId {};
    std::vector<std::uint8_t> response;
};

struct VhostUserSharedMemoryRegion final {
    std::uint8_t id {};
    std::uint64_t size {};
};

struct VhostUserSharedMemoryAction final {
    enum class Type : std::uint8_t {
        map,
        unmap,
    };

    Type type {};
    std::uint64_t actionId {};
    std::uint8_t regionId {};
    std::uint64_t descriptorOffset {};
    std::uint64_t regionOffset {};
    std::uint64_t length {};
    bool writable {};
    uml::gpu::UniqueDescriptor descriptor;
};

class VhostUserDevice {
public:
    using GuestMapper = std::function<std::span<std::uint8_t>(std::uint64_t, std::size_t)>;
    virtual ~VhostUserDevice() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t features() const noexcept = 0;
    [[nodiscard]] virtual std::size_t queueCount() const noexcept = 0;
    [[nodiscard]] virtual std::vector<std::uint8_t> configuration() const = 0;
    virtual void configurationRead(
        std::uint32_t,
        std::uint32_t) noexcept
    {
    }
    virtual void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) = 0;
    [[nodiscard]] virtual VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) = 0;
    virtual void reset() noexcept = 0;
    virtual void prepareReset() noexcept {}
    // Called only on the transport worker. Returned spans are borrowed until
    // reset; devices must release all retained guest state in reset().
    virtual void setGuestMapper(GuestMapper) {}

    [[nodiscard]] virtual std::optional<VhostUserSharedMemoryRegion>
        sharedMemoryRegion() const noexcept
    {
        return std::nullopt;
    }
    [[nodiscard]] virtual bool takeSharedMemoryAction(
        VhostUserSharedMemoryAction&)
    {
        return false;
    }
    virtual void completeSharedMemoryAction(
        std::uint64_t,
        bool) noexcept
    {
    }

    // A device with asynchronous device-to-driver data can expose one
    // readiness descriptor. The transport drains it on its worker and then
    // retries queues which may previously have had no device data available.
    [[nodiscard]] virtual int eventDescriptor() const noexcept { return -1; }
    virtual void drainEventSource() {}
    [[nodiscard]] virtual bool takeQueueCompletion(
        VhostUserQueueCompletion&)
    {
        return false;
    }
    /* A device event may also require a transport-level Virtio config
     * interrupt rather than a virtqueue retry or completion. */
    [[nodiscard]] virtual bool configChangePending() const noexcept
    {
        return false;
    }
    [[nodiscard]] virtual bool claimConfigChange() noexcept
    {
        return configChangePending();
    }
    virtual void releaseConfigChange() noexcept {}
    virtual void clearConfigChange() noexcept {}
    virtual void setStopRequest(const std::atomic_bool*) noexcept {}
    virtual void traceQueue(
        std::uint32_t,
        std::span<const std::uint8_t>,
        std::span<const std::uint8_t>) const noexcept
    {
    }
};

} // namespace uml::virtio
