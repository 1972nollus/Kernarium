// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_gpu_memory.hpp"

#import <Metal/Metal.h>

namespace uml::platform {

std::uint64_t hostGpuAllocatedBytes() noexcept
{
    /* Apple silicon has one GPU and Metal hands every caller the same device
     * object, so its counter covers the renderer, the host driver and the
     * presentation path alike. */
    static id<MTLDevice> const device = MTLCreateSystemDefaultDevice();
    return device == nil ? 0 : static_cast<std::uint64_t>(device.currentAllocatedSize);
}

} // namespace uml::platform
