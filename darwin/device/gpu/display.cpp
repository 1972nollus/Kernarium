// SPDX-License-Identifier: GPL-2.0-or-later

#include "display.hpp"

#include "native_display.hpp"
#include "vnc_display.hpp"

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
    uml::input::vnc::VncInputSink inputSink)
{
    switch (mode) {
    case DisplayMode::vnc:
        return std::make_unique<VncDisplay>(
            vncBindAddress,
            vncPort,
            std::move(vncPassword),
            width,
            height,
            std::move(inputSink));
    case DisplayMode::native:
        return std::make_unique<NativeDisplay>(
            width, height, std::move(inputSink));
    }
    throw std::invalid_argument("unsupported Virtio-GPU display mode");
}

} // namespace uml::gpu
