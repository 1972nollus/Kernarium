// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "scanout_framebuffer.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace uml::gpu {

struct MetalPresentationState;

// Common newest-frame-wins Metal timeline used by the AppKit and UIKit shells.
// Objective-C and Metal types remain inside the implementation.
class MetalPresentation final {
public:
    MetalPresentation(std::uint32_t width, std::uint32_t height);
    ~MetalPresentation();

    MetalPresentation(const MetalPresentation&) = delete;
    MetalPresentation& operator=(const MetalPresentation&) = delete;

    void setRedrawCallback(std::function<void()> callback);
    [[nodiscard]] void* device() const noexcept;
    [[nodiscard]] ScanoutFramebuffer& framebuffer() noexcept;
    [[nodiscard]] const ScanoutFramebuffer& framebuffer() const noexcept;
    void publishCpuFrame() noexcept;
    [[nodiscard]] PresentOutcome present(
        const MetalScanout& scanout) noexcept;
    void draw(void* nativeView) noexcept;
    [[nodiscard]] DisplayStatistics statistics() const noexcept;
    void recordCpuFallbackFrame() noexcept;
    void shutdown() noexcept;

private:
    std::shared_ptr<MetalPresentationState> state_;
};

} // namespace uml::gpu
