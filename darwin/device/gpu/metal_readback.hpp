// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace uml::gpu {

struct MetalReadbackFrame {
    std::span<const std::uint8_t> pixels;
    std::size_t rowBytes {};
    protocol::GpuFormat format {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t destinationX {};
    std::uint32_t destinationY {};
};

struct MetalReadbackState;

/* Damage-bounded, newest-frame-wins conversion from private Metal textures to
 * CPU-visible pixels. VNC necessarily consumes host memory, but the guest GPU
 * timeline must not wait for that readback or for the VNC encoder. */
class MetalReadback final {
public:
    using PublishCallback =
        std::function<void(const MetalReadbackFrame&)>;

    explicit MetalReadback(PublishCallback publish);
    ~MetalReadback();

    MetalReadback(const MetalReadback&) = delete;
    MetalReadback& operator=(const MetalReadback&) = delete;

    [[nodiscard]] PresentOutcome present(
        const MetalScanout& scanout) noexcept;
    [[nodiscard]] DisplayStatistics statistics() const noexcept;
    void recordCpuFallbackFrame() noexcept;
    void shutdown() noexcept;

private:
    std::shared_ptr<MetalReadbackState> state_;
};

} // namespace uml::gpu
