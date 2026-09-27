// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "virtio_balloon_device.hpp"

#include <memory>
#include <string>

namespace uml::gpu {
class InProcessVhostUserService;
}

namespace uml::balloon {

class InProcessBalloonService final {
public:
    explicit InProcessBalloonService(BalloonOptions options);
    ~InProcessBalloonService();

    InProcessBalloonService(const InProcessBalloonService&) = delete;
    InProcessBalloonService& operator=(const InProcessBalloonService&) = delete;

    [[nodiscard]] std::string takeConnectionName();
    void stop() noexcept;

private:
    std::unique_ptr<uml::gpu::InProcessVhostUserService> service_;
};

} // namespace uml::balloon
