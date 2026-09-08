// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "channel.hpp"

namespace uml::console {

// Owns one already-connected SOCK_STREAM descriptor. This is used by an
// in-process application relay; UnixSocketChannel remains the external CLI
// transport and its filesystem ownership contract is unchanged.
class ConnectedSocketChannel final : public ConsoleChannel {
public:
    explicit ConnectedSocketChannel(int descriptor);
    ~ConnectedSocketChannel() override;

    ConnectedSocketChannel(const ConnectedSocketChannel&) = delete;
    ConnectedSocketChannel& operator=(const ConnectedSocketChannel&) = delete;

    [[nodiscard]] std::vector<std::uint8_t> read(
        std::size_t maximumBytes) override;
    void write(std::span<const std::uint8_t> bytes) noexcept override;
    [[nodiscard]] int eventDescriptor() const noexcept override;
    void drainEventSource() override;
    void reset() noexcept override;

private:
    int descriptor_ {-1};
};

} // namespace uml::console
