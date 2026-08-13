// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"
#include "virtio_input_device.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace uml::input::virtio {

class InProcessInputService final {
public:
    InProcessInputService(
        DeviceKind kind,
        std::uint32_t width,
        std::uint32_t height);
    ~InProcessInputService();

    InProcessInputService(const InProcessInputService&) = delete;
    InProcessInputService& operator=(const InProcessInputService&) = delete;

    [[nodiscard]] std::string takeConnectionName();
    void send(std::span<const protocol::InputEvent> events) noexcept;
    void stop() noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::input::virtio
