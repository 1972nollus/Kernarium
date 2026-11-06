// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"

#include <array>
#include <span>

namespace uml::input::virtio {

struct TouchscreenReport final {
    // A lift at a new point first reports the final active position, then
    // ends the slot. Type-B readers otherwise lose that last movement.
    std::array<protocol::InputEvent, 16> events {};
    std::size_t count {};

    [[nodiscard]] std::span<const protocol::InputEvent> span() const noexcept
    {
        return {events.data(), count};
    }
};

/* Linux type-B contacts. One host contact keeps its slot until it ends;
 * leaving and re-entering the visible display starts a new tracking ID.
 * ABS_X/Y follow the first contact until it lifts, for single-touch readers. */
class Touchscreen final {
public:
    [[nodiscard]] TouchscreenReport touch(
        std::uint32_t slot,
        std::uint32_t x,
        std::uint32_t y,
        bool pressed) noexcept;

private:
    struct Contact final {
        std::int32_t trackingId {-1};
        std::uint32_t x {};
        std::uint32_t y {};
    };

    [[nodiscard]] std::int32_t nextTrackingId() noexcept;

    std::array<Contact, protocol::touchscreenContactCount> contacts_ {};
    std::uint32_t primary_ {protocol::touchscreenContactCount};
    std::int32_t nextId_ {};
};

} // namespace uml::input::virtio
