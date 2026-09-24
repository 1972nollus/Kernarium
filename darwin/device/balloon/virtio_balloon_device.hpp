// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "unique_descriptor.hpp"
#include "vhost_user_device.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace uml::balloon {

/* The host memory the balloon hands back and takes again: guest-physical
 * byte ranges, aligned to the host page. */
class BalloonMemory {
public:
    virtual ~BalloonMemory() = default;

    [[nodiscard]] virtual int release(
        std::uint64_t offset, std::uint64_t length) noexcept = 0;
    [[nodiscard]] virtual int commit(
        std::uint64_t offset, std::uint64_t length) noexcept = 0;
};

struct BalloonOptions final {
    /* The guest RAM the balloon may take pages from. */
    std::uint64_t guestMemoryBytes {};
    /* How much memory the balloon should hold, in bytes. The device rounds
     * it down to `granularity` and follows it only once it has moved by a
     * whole granule, so that a target hovering around a boundary does not
     * make the guest inflate and deflate in turn. */
    std::function<std::uint64_t()> target;
    std::uint64_t granularity {16ULL << 20};
    std::chrono::milliseconds interval {500};
    /* The host page the memory is released in. */
    std::size_t hostPageSize {};
};

/* A Virtio balloon whose size follows a host-side target. Every host page
 * whose 4 KiB balloon frames the guest has all handed over is released; the
 * first frame of it the guest takes back commits the host page again before
 * the request completes, which VIRTIO_BALLOON_F_MUST_TELL_HOST makes the
 * guest wait for. */
class VirtioBalloonDevice final : public uml::virtio::VhostUserDevice {
public:
    VirtioBalloonDevice(BalloonOptions options,
        std::unique_ptr<BalloonMemory> memory);

    [[nodiscard]] const char* name() const noexcept override;
    [[nodiscard]] std::uint64_t features() const noexcept override;
    [[nodiscard]] std::size_t queueCount() const noexcept override;
    [[nodiscard]] std::vector<std::uint8_t> configuration() const override;
    void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) override;
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override;
    void reset() noexcept override;

    [[nodiscard]] int eventDescriptor() const noexcept override;
    void drainEventSource() override;
    [[nodiscard]] bool configChangePending() const noexcept override;
    void clearConfigChange() noexcept override;

    /* Re-evaluates the target now; the timer calls this every interval. */
    void updateTarget();

    [[nodiscard]] std::uint64_t targetBytes() const noexcept;
    [[nodiscard]] std::uint64_t actualBytes() const noexcept;
    [[nodiscard]] std::uint64_t releasedBytes() const noexcept;

private:
    void inflate(std::span<const std::uint8_t> frames);
    void deflate(std::span<const std::uint8_t> frames);
    void apply(bool release, std::vector<std::uint64_t>& hostPages);

    BalloonOptions options_;
    std::unique_ptr<BalloonMemory> memory_;
    std::uint64_t framesPerHostPage_ {};
    std::uint64_t frameCount_ {};
    /* One bit per 4 KiB frame the guest has handed over. */
    std::vector<std::uint64_t> frames_;
    std::uint64_t releasedHostPages_ {};
    std::uint32_t targetPages_ {};
    std::uint32_t actualPages_ {};
    bool configChangePending_ {};
    uml::gpu::UniqueDescriptor eventQueue_;
};

/* The process's guest RAM, as published by the physical-memory backing. */
[[nodiscard]] std::unique_ptr<BalloonMemory> makeGuestRamBalloonMemory();

/* A target that follows the memory the host GPU holds for this process, up
 * to `limit` bytes. */
[[nodiscard]] std::function<std::uint64_t()> hostGpuMemoryTarget(
    std::uint64_t limit);

} // namespace uml::balloon
