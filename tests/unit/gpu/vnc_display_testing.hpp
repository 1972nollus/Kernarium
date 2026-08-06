// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
namespace uml::gpu {
class VncDisplay;
}

namespace uml::gpu::testing {

void failNextEventThreadStart() noexcept;
void eventThreadStartCheckpoint(std::uint16_t port);
[[nodiscard]] std::uint16_t failedEventThreadPort() noexcept;

} // namespace uml::gpu::testing
