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

/* Bytes of this process's footprint the kernel charges as graphics memory:
 * Metal resources and surfaces, and also the pages the GPU driver has written
 * in its own storage, such as Metal 4 command allocators, which the device
 * counter above omits. Zero when the kernel does not report it. One task_info
 * call. */
[[nodiscard]] std::uint64_t hostGraphicsFootprintBytes() noexcept;

} // namespace uml::platform
