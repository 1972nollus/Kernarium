// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>

namespace uml::video {

class InProcessVideoService final {
public:
    InProcessVideoService();
    ~InProcessVideoService();

    InProcessVideoService(const InProcessVideoService&) = delete;
    InProcessVideoService& operator=(const InProcessVideoService&) = delete;

    [[nodiscard]] std::string takeConnectionName();
    void stop() noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::video
