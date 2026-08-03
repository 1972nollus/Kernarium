// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace uml::gpu {

struct [[gnu::packed]] VncInputEvent {
    std::uint16_t type;
    std::uint16_t code;
    std::int32_t value;
};

using VncInputSink =
    std::function<void(std::span<const VncInputEvent> events)>;

namespace input {

constexpr std::uint16_t eventSync = 0;
constexpr std::uint16_t eventKey = 1;
constexpr std::uint16_t eventRelative = 2;
constexpr std::uint16_t eventAbsolute = 3;

constexpr std::uint16_t syncReport = 0;
constexpr std::uint16_t relativeHorizontalWheel = 6;
constexpr std::uint16_t relativeWheel = 8;
constexpr std::uint16_t absoluteX = 0;
constexpr std::uint16_t absoluteY = 1;

constexpr std::uint16_t buttonLeft = 0x110;
constexpr std::uint16_t buttonRight = 0x111;
constexpr std::uint16_t buttonMiddle = 0x112;

constexpr std::int32_t absoluteMaximum = 32767;

[[nodiscard]] std::optional<std::uint16_t> keyCode(
    std::uint32_t keySymbol) noexcept;

} // namespace input

static_assert(sizeof(VncInputEvent) == 8);

} // namespace uml::gpu
