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
    /* The caller's tag for the request this frame answers, and whether the
     * frame covers the scanout's whole view rather than only its damage. */
    std::uint32_t generation {};
    bool complete {};
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

    /* Read back the scanout's damage. The generation travels to the
     * published frame. A request that replaces one still waiting for the
     * in-flight readback also covers the damage of the one it replaces. */
    [[nodiscard]] PresentOutcome present(
        const MetalScanout& scanout,
        std::uint32_t generation) noexcept;
    /* Keep the scanout's whole view, without reading it, for a later
     * refresh(). Returns false, and keeps nothing, for a scanout this
     * readback cannot convert. */
    bool remember(const MetalScanout& scanout) noexcept;
    /* Read back the whole view of the remembered scanout, if any, and
     * release it. The request is not a guest frame. */
    [[nodiscard]] bool refresh(std::uint32_t generation) noexcept;
    void forget() noexcept;
    [[nodiscard]] DisplayStatistics statistics() const noexcept;
    void recordCpuFallbackFrame() noexcept;
    void shutdown() noexcept;

private:
    std::shared_ptr<MetalReadbackState> state_;
};

} // namespace uml::gpu
