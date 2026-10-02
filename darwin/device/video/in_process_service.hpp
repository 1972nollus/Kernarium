// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "in_process_vhost_user_service.hpp"
#include "shared_memory_regions.hpp"

#include <memory>

namespace uml::video {

/* `regions` is the shared-memory index of the backend that owns the guest's
 * host-visible region, when the guest has one. With it the decoder can fill
 * a capture buffer that lives in that region. */
[[nodiscard]] std::unique_ptr<uml::gpu::InProcessVhostUserService>
makeInProcessService(
    std::shared_ptr<uml::virtio::SharedMemoryRegions> regions = {});

} // namespace uml::video
