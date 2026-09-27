// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace uml::gpu {
class InProcessVhostUserService;
}

namespace uml::console {

class InProcessConsoleService final {
public:
    explicit InProcessConsoleService(
        std::filesystem::path socketPath,
        std::string portName = {});
    // Duplicates an already-connected SOCK_STREAM descriptor.
    InProcessConsoleService(int connectedDescriptor, std::string portName);
    ~InProcessConsoleService();

    InProcessConsoleService(const InProcessConsoleService&) = delete;
    InProcessConsoleService& operator=(const InProcessConsoleService&) = delete;

    [[nodiscard]] std::string takeConnectionName();
    void stop() noexcept;

private:
    std::unique_ptr<uml::gpu::InProcessVhostUserService> service_;
};

} // namespace uml::console
