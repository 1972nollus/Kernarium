// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace uml::console::protocol {

inline constexpr std::uint32_t inputQueue = 0;
inline constexpr std::uint32_t outputQueue = 1;
inline constexpr std::size_t queueCount = 2;
inline constexpr std::uint32_t deviceId = 3;

struct Configuration final {
    std::uint16_t columns {};
    std::uint16_t rows {};
    std::uint32_t maximumPorts {};
    std::uint32_t emergencyWrite {};
};

static_assert(sizeof(Configuration) == 12);

} // namespace uml::console::protocol
