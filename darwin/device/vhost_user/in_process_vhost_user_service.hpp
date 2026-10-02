// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "unique_descriptor.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace uml::virtio {
class SharedMemoryRegions;
class VhostUserDevice;
} // namespace uml::virtio

namespace uml::gpu {

class VhostUserServer;

/* Serves one vhost-user device, on a thread of its own, to the guest of this
 * process. The guest's virtio_uml transport takes one end of a socketpair by
 * its connection name; the service answers on the other end until stop().
 * Every in-process device service is one of these around its own device. */
class InProcessVhostUserService final {
public:
    using DeviceFactory =
        std::function<std::unique_ptr<uml::virtio::VhostUserDevice>()>;
    using ServerFactory = std::function<std::unique_ptr<VhostUserServer>()>;

    /* Starts the service thread with the guest timer blocked, creates the
     * device on it, and returns once the server is ready. An exception the
     * thread raised before then is rethrown here. `label` names the device
     * in error messages, `threadName` names the thread, and a non-null
     * `sharedMemoryRegions` is given to the server before it runs. */
    InProcessVhostUserService(
        std::string label,
        std::string threadName,
        DeviceFactory makeDevice,
        std::shared_ptr<uml::virtio::SharedMemoryRegions>
            sharedMemoryRegions = {});
    /* The same, for a device whose server needs more than the device, such
     * as the GPU's display: `makeServer` builds the whole server on the
     * service thread. */
    InProcessVhostUserService(
        std::string label,
        std::string threadName,
        ServerFactory makeServer,
        std::shared_ptr<uml::virtio::SharedMemoryRegions>
            sharedMemoryRegions = {});
    ~InProcessVhostUserService();

    InProcessVhostUserService(const InProcessVhostUserService&) = delete;
    InProcessVhostUserService& operator=(
        const InProcessVhostUserService&) = delete;

    /* The guest's end, named for a virtio_uml.device= argument. It can be
     * taken once; the guest owns that descriptor from then on. */
    [[nodiscard]] std::string takeConnectionName();
    /* Stops serving and joins the thread; later calls do nothing. */
    void stop() noexcept;

private:
    void run() noexcept;

    std::string label_;
    std::string threadName_;
    ServerFactory makeServer_;
    std::shared_ptr<uml::virtio::SharedMemoryRegions> sharedMemoryRegions_;
    std::array<UniqueDescriptor, 2> descriptors_;
    UniqueDescriptor stopDescriptor_;
    std::string connectionName_;
    std::atomic_bool stopRequested_ {};
    std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    bool ready_ {};
    std::exception_ptr failure_;
    std::thread thread_;
};

} // namespace uml::gpu
