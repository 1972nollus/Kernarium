// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"
#include "unique_descriptor.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace uml::gpu {

inline constexpr std::size_t maximumVhostUserMessagePayload =
    1024U * 1024U;
// Linux vhost-user memory tables may contain up to 64 regions. Keep the wire
// receiver's descriptor capacity aligned with that protocol limit so a valid
// table cannot be rejected solely by the ancillary-data buffer size.
inline constexpr std::size_t maximumVhostUserDescriptors = 64;

class ReceivedDescriptors final {
public:
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    [[nodiscard]] UniqueDescriptor& operator[](std::size_t index) noexcept
    {
        return descriptors_[index];
    }

    [[nodiscard]] const UniqueDescriptor& operator[](
        std::size_t index) const noexcept
    {
        return descriptors_[index];
    }

    [[nodiscard]] bool adopt(UniqueDescriptor descriptor) noexcept
    {
        if (count_ == descriptors_.size())
            return false;
        descriptors_[count_++] = std::move(descriptor);
        return true;
    }

private:
    std::array<UniqueDescriptor, maximumVhostUserDescriptors> descriptors_;
    std::size_t count_ {};
};

struct VhostUserMessage final {
    protocol::VhostHeader header {};
    std::vector<std::uint8_t> payload;
    ReceivedDescriptors descriptors;
};

// Returns false only for an orderly peer close or a truncated stream.
[[nodiscard]] bool receiveVhostUserMessage(
    int socket,
    VhostUserMessage& message,
    const std::atomic_bool* stopRequested = nullptr);

} // namespace uml::gpu
