// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"
#include "virgl_renderer.hpp"
#include "display.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <sys/uio.h>

#ifndef UML_GPU_DEVICE_TESTING
#define UML_GPU_DEVICE_TESTING 0
#endif

namespace uml::gpu {

#if UML_GPU_DEVICE_TESTING
namespace testing {
class VirtioGpuDeviceDependencies;
}
#endif

class VirtioGpuDevice {
public:
    using GuestReader =
        std::function<bool(std::uint64_t, std::span<std::uint8_t>)>;
    using GuestMapper =
        std::function<std::span<std::uint8_t>(std::uint64_t, std::size_t)>;
#if UML_GPU_DEVICE_TESTING
    VirtioGpuDevice(
        testing::VirtioGpuDeviceDependencies& dependencies,
        GuestReader guestReader,
        GuestMapper guestMapper,
        std::uint32_t dotsPerInch = 0);
#else
    VirtioGpuDevice(
        Display& display,
        GuestReader guestReader,
        GuestMapper guestMapper,
        std::uint32_t dotsPerInch = 0);
#endif
    ~VirtioGpuDevice();

    [[nodiscard]] protocol::GpuConfig config() const noexcept;
    // The caller owns this flag and must keep it alive until it clears the
    // registration after process() has returned.
    void setStopRequest(const std::atomic_bool* stopRequested) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::vector<std::uint8_t> process(
        std::span<const std::uint8_t> request,
        bool cursorQueue);

private:
    struct BackingEntry {
        std::uint64_t address {};
        std::uint32_t length {};
    };

    struct Resource {
        protocol::GpuFormat format {};
        std::uint32_t width {};
        std::uint32_t height {};
        std::vector<std::uint8_t> pixels;
        std::vector<BackingEntry> backing;
    };

    struct VirglResource {
        protocol::GpuFormat format {};
        std::uint32_t target {};
        std::uint32_t width {};
        std::uint32_t height {};
        std::vector<BackingEntry> backing;
        std::vector<iovec> vectors;
    };

    [[nodiscard]] std::vector<std::uint8_t> response(
        const protocol::GpuControlHeader& request,
        protocol::GpuResponse type) const;
    [[nodiscard]] std::vector<std::uint8_t> displayInfo(
        const protocol::GpuControlHeader& request) const;
    [[nodiscard]] std::vector<std::uint8_t> edid(
        const protocol::GpuGetEdid& request) const;
    [[nodiscard]] bool readBacking(
        const Resource& resource,
        std::uint64_t offset,
        std::span<std::uint8_t> destination) const;
    [[nodiscard]] bool parseBacking(
        std::span<const std::uint8_t> request,
        std::size_t entriesOffset,
        std::uint32_t entryCount,
        std::vector<BackingEntry>& backing,
        std::vector<iovec>& vectors) const;
    [[nodiscard]] std::vector<std::uint8_t> capsetInfo(
        const protocol::GpuGetCapsetInfo& request) const;
    [[nodiscard]] std::vector<std::uint8_t> capset(
        const protocol::GpuGetCapset& request) const;

#if UML_GPU_DEVICE_TESTING
    testing::VirtioGpuDeviceDependencies& dependencies_;
#else
    Display& display_;
#endif
    GuestReader guestReader_;
    GuestMapper guestMapper_;
    std::uint32_t dotsPerInch_;
#if !UML_GPU_DEVICE_TESTING
    VirglRenderer renderer_;
#endif
    std::unordered_map<std::uint32_t, Resource> resources_;
    std::unordered_map<std::uint32_t, VirglResource> virglResources_;
    std::unordered_set<std::uint32_t> contexts_;
    std::vector<std::uint8_t> transferStaging_;
    std::vector<std::uint8_t> readbackStaging_;
    const std::atomic_bool* stopRequested_ {};
    std::uint32_t scanoutResource_ {};
    protocol::GpuRectangle scanoutRectangle_ {};
};

} // namespace uml::gpu
