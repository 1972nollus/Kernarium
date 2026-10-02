// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "vhost_user_server.hpp"

#include <utility>

namespace uml::balloon {

std::unique_ptr<uml::gpu::InProcessVhostUserService> makeInProcessService(
    BalloonOptions options)
{
    return std::make_unique<uml::gpu::InProcessVhostUserService>(
        "Virtio balloon", "uml-balloon",
        [options = std::move(options)]() mutable {
            return std::make_unique<VirtioBalloonDevice>(
                std::move(options), makeGuestRamBalloonMemory());
        });
}

} // namespace uml::balloon
