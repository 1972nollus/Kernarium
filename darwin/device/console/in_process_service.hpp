// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace uml::console {

class InProcessConsoleService final {
public:
    explicit InProcessConsoleService(std::filesystem::path socketPath);
    ~InProcessConsoleService();

    InProcessConsoleService(const InProcessConsoleService&) = delete;
    InProcessConsoleService& operator=(const InProcessConsoleService&) = delete;

    [[nodiscard]] std::string takeConnectionName();
    void stop() noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::console
