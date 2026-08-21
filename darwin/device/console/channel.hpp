// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uml::console {

class ConsoleChannel {
public:
    virtual ~ConsoleChannel() = default;

    [[nodiscard]] virtual std::vector<std::uint8_t> read(
        std::size_t maximumBytes) = 0;
    virtual void write(std::span<const std::uint8_t> bytes) noexcept = 0;
    [[nodiscard]] virtual int eventDescriptor() const noexcept = 0;
    virtual void drainEventSource() = 0;
    virtual void reset() noexcept = 0;
};

} // namespace uml::console
