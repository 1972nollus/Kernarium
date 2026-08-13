// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uml::virtio {

struct VhostUserQueueResult final {
    bool consumed {};
    std::vector<std::uint8_t> response;
};

class VhostUserDevice {
public:
    virtual ~VhostUserDevice() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t features() const noexcept = 0;
    [[nodiscard]] virtual std::size_t queueCount() const noexcept = 0;
    [[nodiscard]] virtual std::vector<std::uint8_t> configuration() const = 0;
    virtual void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) = 0;
    [[nodiscard]] virtual VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) = 0;
    virtual void reset() noexcept = 0;

    // A device with asynchronous device-to-driver data can expose one
    // readiness descriptor. The transport drains it on its worker and then
    // retries queues which may previously have had no device data available.
    [[nodiscard]] virtual int eventDescriptor() const noexcept { return -1; }
    virtual void drainEventSource() {}
    virtual void setStopRequest(const std::atomic_bool*) noexcept {}
    virtual void traceQueue(
        std::uint32_t,
        std::span<const std::uint8_t>,
        std::span<const std::uint8_t>) const noexcept
    {
    }
};

} // namespace uml::virtio
