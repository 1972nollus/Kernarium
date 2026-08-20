// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "protocol.hpp"
#include "virgl_renderer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace uml::gpu::testing {

/* Test-only dependency boundary for VirtioGpuDevice. Ordinary builds use
 * UML_GPU_DEVICE_TESTING=0 and call VirglRenderer and VncDisplay directly, so
 * this virtual surface adds no production field, branch, or indirect call. */
class VirtioGpuDeviceDependencies {
public:
    virtual ~VirtioGpuDeviceDependencies() = default;

    [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
    virtual void clear() noexcept = 0;
    virtual void update(
        std::span<const std::uint8_t> source,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        std::size_t sourceRowBytes,
        protocol::GpuFormat sourceFormat,
        std::uint32_t sourceX,
        std::uint32_t sourceY,
        std::uint32_t destinationX,
        std::uint32_t destinationY,
        std::uint32_t width,
        std::uint32_t height) = 0;

    [[nodiscard]] virtual VirglRenderer::Capset capset(
        std::uint32_t capsetId) const = 0;
    [[nodiscard]] virtual int createContext(
        std::uint32_t contextId,
        std::uint32_t flags,
        std::string_view debugName) noexcept = 0;
    virtual void destroyContext(std::uint32_t contextId) noexcept = 0;
    [[nodiscard]] virtual int submit(
        std::uint32_t contextId,
        std::span<const std::uint8_t> command) noexcept = 0;
    [[nodiscard]] virtual int createResource3d(
        const VirglRenderer::Resource3d& resource) noexcept = 0;
    [[nodiscard]] virtual int transferResource(
        bool toHost,
        std::uint32_t resourceId,
        std::uint32_t contextId,
        std::uint32_t level,
        std::uint32_t stride,
        std::uint32_t layerStride,
        const VirglRenderer::Box& box,
        std::uint64_t offset) noexcept = 0;
    [[nodiscard]] virtual int readResourceRectangle(
        std::uint32_t resourceId,
        std::span<std::uint8_t> destination,
        std::uint32_t offset,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height) = 0;
    [[nodiscard]] virtual int resourceMetalTexture(
        std::uint32_t resourceId,
        void*& texture) noexcept = 0;
    [[nodiscard]] virtual PresentOutcome presentMetal(
        const MetalScanout& scanout) noexcept = 0;
    [[nodiscard]] virtual bool needsFullCpuFallback() const noexcept = 0;
    virtual void recordCpuFallbackFrame() noexcept = 0;
    [[nodiscard]] virtual int createFenceAndWait(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceId,
        const std::atomic_bool* stopRequested) noexcept = 0;
    virtual void unrefResource(std::uint32_t resourceId) noexcept = 0;
    [[nodiscard]] virtual int attachBacking(
        std::uint32_t resourceId,
        std::span<iovec> backing) noexcept = 0;
    virtual void detachBacking(std::uint32_t resourceId) noexcept = 0;
    [[nodiscard]] virtual int attachResource(
        std::uint32_t contextId,
        std::uint32_t resourceId) noexcept = 0;
    [[nodiscard]] virtual int detachResource(
        std::uint32_t contextId,
        std::uint32_t resourceId) noexcept = 0;
    virtual void resetFenceState() noexcept = 0;
};

} // namespace uml::gpu::testing
