// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "vhost_user_server.hpp"
#include "../input/vnc/vnc_input.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace uml::gpu {

/* The Virtio-GPU backend: a vhost-user server whose device renders through
 * virglrenderer and presents on a display of `displayMode`. `socketPath` is
 * the listening socket for run(); runConnected() needs none. */
[[nodiscard]] std::unique_ptr<VhostUserServer> makeGpuVhostUserServer(
    std::filesystem::path socketPath,
    std::string vncBindAddress,
    std::uint16_t vncPort,
    std::string vncPassword,
    std::uint32_t width,
    std::uint32_t height,
    bool selfTestPattern,
    uml::input::vnc::VncInputSink inputSink = {},
    DisplayMode displayMode = DisplayMode::vnc,
    void* nativeView = nullptr,
    std::uint32_t dotsPerInch = 0,
    uml::input::vnc::PointerMode pointerMode =
        uml::input::vnc::PointerMode::mouse,
    bool autoResizeGuest = true);

} // namespace uml::gpu
