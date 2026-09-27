// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "shared_memory_regions.hpp"

#include <memory>
#include <string>

namespace uml::gpu {
class InProcessVhostUserService;
}

namespace uml::video {

class InProcessVideoService final {
public:
    /* `regions` is the shared-memory index of the backend that owns the
     * guest's host-visible region, when the guest has one. With it the
     * decoder can fill a capture buffer that lives in that region. */
    explicit InProcessVideoService(
        std::shared_ptr<uml::virtio::SharedMemoryRegions> regions = {});
    ~InProcessVideoService();

    InProcessVideoService(const InProcessVideoService&) = delete;
    InProcessVideoService& operator=(const InProcessVideoService&) = delete;

    [[nodiscard]] std::string takeConnectionName();
    void stop() noexcept;

private:
    std::unique_ptr<uml::gpu::InProcessVhostUserService> service_;
};

} // namespace uml::video
