// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>

namespace uml::battery {

class InProcessBatteryService final {
public:
    InProcessBatteryService();
    ~InProcessBatteryService();

    InProcessBatteryService(const InProcessBatteryService&) = delete;
    InProcessBatteryService& operator=(const InProcessBatteryService&) = delete;

    [[nodiscard]] std::string takeConnectionName();
    void stop() noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::battery
