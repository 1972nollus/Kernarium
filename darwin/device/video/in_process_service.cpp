// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "video_decoder.hpp"
#include "vhost_user_server.hpp"
#include "virtio_video_device.hpp"

#include <utility>

namespace uml::video {

std::unique_ptr<uml::gpu::InProcessVhostUserService> makeInProcessService(
    std::shared_ptr<uml::virtio::SharedMemoryRegions> regions)
{
    return std::make_unique<uml::gpu::InProcessVhostUserService>(
        "Virtio video", "uml-video",
        [] {
            return std::make_unique<VirtioVideoDevice>(
                uml::platform::videoDecodeCapabilities(),
                uml::platform::makeVideoDecoder);
        },
        std::move(regions));
}

} // namespace uml::video
