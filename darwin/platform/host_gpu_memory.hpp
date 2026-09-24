// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::platform {

/* Bytes the host GPU holds for this process right now: every buffer, texture
 * and heap on the system Metal device, whoever allocated it, including
 * no-copy buffers over shared memory. Zero without a Metal device. The query
 * reads a counter the driver maintains and is cheap enough to make per
 * allocation. */
[[nodiscard]] std::uint64_t hostGpuAllocatedBytes() noexcept;

} // namespace uml::platform
