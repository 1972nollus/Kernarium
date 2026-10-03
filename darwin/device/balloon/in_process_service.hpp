// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "in_process_vhost_user_service.hpp"
#include "virtio_balloon_device.hpp"

#include <memory>

namespace uml::balloon {

[[nodiscard]] std::unique_ptr<uml::gpu::InProcessVhostUserService>
makeInProcessService(BalloonOptions options);

} // namespace uml::balloon
