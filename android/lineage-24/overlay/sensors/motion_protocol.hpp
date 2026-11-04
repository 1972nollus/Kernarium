// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace andarium::motion {
constexpr uint32_t port = 5556;
constexpr size_t record_size = 20;
struct Sample {
    uint32_t type;
    std::array<float, 3> values;
};
inline std::optional<Sample> decode(std::span<const uint8_t> bytes) {
    if (bytes.size() != record_size || bytes[0] != 'A' || bytes[1] != 'N' ||
        bytes[2] != 'S' || bytes[3] != '1') return std::nullopt;
    const auto word = [&bytes](size_t offset) {
        return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8) |
               (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
    };
    Sample sample{word(4), {}};
    if (sample.type != 1 && sample.type != 4) return std::nullopt;
    for (size_t i = 0; i < sample.values.size(); ++i) {
        sample.values[i] = std::bit_cast<float>(word(8 + i * 4));
        if (!std::isfinite(sample.values[i])) return std::nullopt;
    }
    return sample;
}
} // namespace andarium::motion
