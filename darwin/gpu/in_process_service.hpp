// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace uml::gpu {

struct InProcessGpuOptions {
    std::string vncBindAddress {"127.0.0.1"};
    std::uint16_t vncPort {5900};
    std::uint32_t width {1280};
    std::uint32_t height {720};
};

class InProcessGpuService {
public:
    explicit InProcessGpuService(InProcessGpuOptions options);
    ~InProcessGpuService();

    InProcessGpuService(const InProcessGpuService&) = delete;
    InProcessGpuService& operator=(const InProcessGpuService&) = delete;

    [[nodiscard]] const std::string& connectionName() const noexcept;
    [[nodiscard]] const std::string& inputConnectionName() const noexcept;
    void stop() noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::gpu
