// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "virtio_balloon_device.hpp"

#include <memory>
#include <string>

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
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::balloon
