// SPDX-License-Identifier: GPL-2.0-or-later

#include "display.hpp"

#include "native_display.hpp"
#if !UML_GPU_ANGLE_METAL
#include "vnc_display.hpp"
#endif

#include <stdexcept>
#include <utility>

namespace uml::gpu {

std::unique_ptr<Display> makeDisplay(
    DisplayMode mode,
    const std::string& vncBindAddress,
    std::uint16_t vncPort,
    std::string vncPassword,
    std::uint32_t width,
    std::uint32_t height,
    uml::input::vnc::VncInputSink inputSink,
    void* nativeView)
{
#if UML_GPU_ANGLE_METAL
    (void)vncBindAddress;
    (void)vncPort;
    (void)vncPassword;
#endif
    switch (mode) {
    case DisplayMode::vnc:
#if !UML_GPU_ANGLE_METAL
        return std::make_unique<VncDisplay>(
            vncBindAddress,
            vncPort,
            std::move(vncPassword),
            width,
            height,
            std::move(inputSink));
#else
        throw std::invalid_argument(
            "VNC display is not part of the iOS GPU prototype");
#endif
    case DisplayMode::native:
        return std::make_unique<NativeDisplay>(
            width, height, std::move(inputSink), nativeView);
    }
    throw std::invalid_argument("unsupported Virtio-GPU display mode");
}

} // namespace uml::gpu
