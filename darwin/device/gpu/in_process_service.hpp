// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "shared_memory_regions.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace uml::gpu {

struct InProcessGpuOptions {
    DisplayMode displayMode {DisplayMode::vnc};
    std::string vncBindAddress {"127.0.0.1"};
    std::uint16_t vncPort {5900};
    std::string vncPassword;
    std::uint32_t width {1280};
    std::uint32_t height {720};
    std::uint32_t dotsPerInch {};
    uml::input::vnc::PointerMode pointerMode {
        uml::input::vnc::PointerMode::mouse};
    void* nativeView {};
    bool autoResizeGuest {true};
    // A composed input device may consume display events without the legacy
    // kernel input socket. It must outlive this service.
    uml::input::vnc::VncInputSink inputSink {};
    /* Shared with the other backends of the same guest. This service
     * publishes every host-visible blob range it maps there, so a device
     * whose guest names a buffer inside the region can resolve it. */
    std::shared_ptr<uml::virtio::SharedMemoryRegions> sharedMemoryRegions;
};

class InProcessGpuService {
public:
    explicit InProcessGpuService(InProcessGpuOptions options);
    ~InProcessGpuService();

    InProcessGpuService(const InProcessGpuService&) = delete;
    InProcessGpuService& operator=(const InProcessGpuService&) = delete;

    /* Transfer each guest endpoint exactly once. The service retains only
     * the opposite endpoint used by its worker, so guest closure and numeric
     * descriptor reuse cannot make service teardown close an unrelated file. */
    [[nodiscard]] std::string takeConnectionName();
    [[nodiscard]] std::string takeInputConnectionName();
    void stop() noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::gpu
