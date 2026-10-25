// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "in_process_vhost_user_service.hpp"

#include <cstdint>
#include <memory>

namespace uml::audio {

enum class OutputMode : std::uint8_t {
    // The Mac's or iPhone's default output.
    host,
    // Nothing is played; the guest still sees a device that plays in real
    // time.
    null,
};

struct ServiceOptions final {
    OutputMode output {OutputMode::host};
};

[[nodiscard]] std::unique_ptr<uml::gpu::InProcessVhostUserService>
makeInProcessService(const ServiceOptions& options = {});

} // namespace uml::audio
