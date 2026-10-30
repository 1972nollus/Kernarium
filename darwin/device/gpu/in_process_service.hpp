// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "shared_memory_regions.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
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
    /* Shared with the other backends of the same guest. This service
     * publishes every host-visible blob range it maps there, so a device
     * whose guest names a buffer inside the region can resolve it. */
    std::shared_ptr<uml::virtio::SharedMemoryRegions> sharedMemoryRegions;
    /* The most device memory a Venus guest may allocate, counted against
     * everything the host GPU holds for this process, or zero for no bound. */
    std::uint64_t deviceMemoryLimit {};
};

/* The GPU of one process's guest, and the display it presents on.
 *
 * The display (the VNC listener and its clients, or the native window or
 * view) and the frame tap belong to the service and last until stop(). The
 * guest's connection does not: it is the Virtio-GPU device, the renderer and
 * every context and resource the guest made, and it lasts as long as that
 * guest. A guest that restarts itself is a new guest on the same machine, so
 * startNextConnection() ends the old connection and opens a fresh one while
 * the display, and what is attached to it, carries on. */
class InProcessGpuService {
public:
    explicit InProcessGpuService(InProcessGpuOptions options);
    ~InProcessGpuService();

    InProcessGpuService(const InProcessGpuService&) = delete;
    InProcessGpuService& operator=(const InProcessGpuService&) = delete;

    /* Transfer each guest endpoint of the current connection exactly once.
     * The service retains only the opposite endpoint used by its worker, so
     * guest closure and numeric descriptor reuse cannot make service teardown
     * close an unrelated file. */
    [[nodiscard]] std::string takeConnectionName();
    [[nodiscard]] std::string takeInputConnectionName();

    /* The guest the connection served has gone. Ends that connection, which
     * resets the device and destroys the renderer with everything the old
     * guest created in it, including its share of the device memory pool, and
     * opens a fresh one for the next guest; take*ConnectionName() then hand
     * out the new endpoints. The display carries on, cleared until the next
     * guest scans out, and so do the clients on it. Display input that
     * arrives while no connection exists is dropped.
     *
     * The request gate is not touched: requests held back when the guest
     * restarted stay held for the new device, which opens it with
     * releaseRequests() like the old one.
     *
     * The old guest's endpoints are the guest's to close: the service shuts
     * its own ends down whatever the guest has done, and an endpoint left open
     * only holds a descriptor until it is closed. Throws, with the display as
     * it was, when the new connection cannot be opened; the service may then
     * have no connection, and a later call tries again. Throws
     * std::logic_error after stop(). */
    void startNextConnection();

    /* Ends the connection and the display. */
    void stop() noexcept;

    /* Holds back the guest's GPU requests so none reaches the renderer, and
     * with it the host GPU, until releaseRequests(). A request that arrives
     * meanwhile waits in its virtqueue, through any suspend of the guest that
     * disables and re-enables the ring, and runs in order after the release.
     * Returns whether no request was still running after `budget`; the hold
     * is in place either way. Work the renderer already has, fences included,
     * carries on. The application holds before it enters the background, where
     * the host GPU is revoked within seconds. */
    [[nodiscard]] bool holdRequests(
        std::chrono::milliseconds budget) noexcept;
    void releaseRequests() noexcept;
    [[nodiscard]] bool requestsHeld() const noexcept;

    /* The port a VNC display listens on, or nothing for any other display. */
    [[nodiscard]] std::optional<std::uint16_t> vncPort() const noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::gpu
