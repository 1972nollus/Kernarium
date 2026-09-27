// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"
#include "unique_descriptor.hpp"
#include "vhost_user_device.hpp"
#include "vhost_user_message.hpp"
#include "display.hpp"
#include "../input/vnc/vnc_input.hpp"
#include "../../memory/shared_backing.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace uml::gpu {

class VhostUserServer {
public:
    VhostUserServer(
        std::filesystem::path socketPath,
        std::string vncBindAddress,
        std::uint16_t vncPort,
        std::string vncPassword,
        std::uint32_t width,
        std::uint32_t height,
        bool selfTestPattern,
        uml::input::vnc::VncInputSink inputSink = {},
        DisplayMode displayMode = DisplayMode::vnc,
        void* nativeView = nullptr,
        std::uint32_t dotsPerInch = 0,
        uml::input::vnc::PointerMode pointerMode =
            uml::input::vnc::PointerMode::mouse,
        bool autoResizeGuest = true);
    explicit VhostUserServer(
        std::unique_ptr<uml::virtio::VhostUserDevice> device);
    ~VhostUserServer();

    VhostUserServer(const VhostUserServer&) = delete;
    VhostUserServer& operator=(const VhostUserServer&) = delete;

    void run(
        const std::atomic_bool& stopRequested,
        const std::function<void()>& ready = {});
    void runConnected(
        int connection,
        const std::atomic_bool& stopRequested,
        const std::function<void()>& ready = {});

    /* Shares one shared-memory index with the other backends of the same
     * guest. The backend that owns a region publishes its ranges here; a
     * device whose guest names buffers inside a region resolves them here.
     * Call before run(). */
    void setSharedMemoryRegions(
        std::shared_ptr<uml::virtio::SharedMemoryRegions> regions);

private:
    struct Mapping {
        std::uint64_t guestAddress {};
        std::uint64_t userAddress {};
        std::uint64_t size {};
        std::uint64_t mmapOffset {};
        UniqueDescriptor descriptor;
        uml::memory::SharedBackingView backing;
    };

    struct Queue {
        std::uint32_t size {};
        std::uint16_t lastAvailable {};
        std::uint64_t descriptorAddress {};
        std::uint64_t availableAddress {};
        std::uint64_t usedAddress {};
        UniqueDescriptor kickDescriptor;
        UniqueDescriptor callDescriptor;
        bool enabled {};
        bool started {};
        bool polling {};
        bool callPolling {};
    };

    struct DescriptorSegment {
        std::uint64_t address {};
        std::uint32_t length {};
        bool writable {};
    };

    struct PendingRequest {
        std::uint64_t requestId {};
        std::uint32_t queueIndex {};
        std::uint16_t head {};
        std::size_t responseCapacity {};
        std::vector<DescriptorSegment> segments;
    };

    void resetConnection() noexcept;
    void resetDeviceState() noexcept;
    void resetQueues() noexcept;
    void clearMappings() noexcept;
    void handleMessage(int socket, VhostUserMessage& message);
    void sendReply(
        int socket,
        const protocol::VhostHeader& request,
        std::span<const std::uint8_t> payload);
    void sendIntegerReply(
        int socket,
        const protocol::VhostHeader& request,
        std::uint64_t value);
    void sendAcknowledgement(
        int socket,
        const protocol::VhostHeader& request,
        std::uint64_t status);
    void configureMemory(VhostUserMessage& message);
    void configureKick(VhostUserMessage& message);
    void drainKick(std::uint32_t index);
    void processQueue(std::uint32_t index);
    void dispatchSharedMemoryActions();
    void drainQueueCompletions();
    void signalPendingConfigChange();
    void completeRequest(
        const PendingRequest& request,
        std::span<const std::uint8_t> response);
    void notifyQueue(std::uint32_t index);
    void signalQueue(std::uint32_t index);
    /* Waits for the front-end's answer to a backend request while still
     * answering every front-end request whose reply it awaits without
     * reading the backend channel; other requests are deferred. */
    [[nodiscard]] VhostUserMessage receiveBackendReply(const char* channel);
    void handleDeferredFrontendMessages(int socket);
    void serveConnection(
        int connection,
        const std::atomic_bool& stopRequested);

    [[nodiscard]] std::uint8_t* translate(
        std::uint64_t address,
        std::size_t length,
        bool userAddress);
    [[nodiscard]] bool readGuest(
        std::uint64_t address,
        std::span<std::uint8_t> destination);
    [[nodiscard]] std::vector<DescriptorSegment> descriptorChain(
        const Queue& queue,
        std::uint16_t head);
    [[nodiscard]] std::uint64_t allocateRequestId();

    std::filesystem::path socketPath_;
    std::string vncBindAddress_;
    std::unique_ptr<Display> display_;
    std::unique_ptr<uml::virtio::VhostUserDevice> device_;
    UniqueDescriptor slaveRequestDescriptor_;
    std::shared_ptr<uml::virtio::SharedMemoryRegions> sharedMemoryRegions_;
    std::vector<Mapping> mappings_;
    std::vector<Queue> queues_;
    std::unordered_map<std::uint64_t, PendingRequest> pendingRequests_;
    std::uint64_t nextRequestId_ {1};
    std::uint64_t negotiatedFeatures_ {};
    std::uint64_t negotiatedProtocolFeatures_ {};
    bool featuresConfigured_ {};
    const std::atomic_bool* connectionStopRequested_ {};
    /* The connection's main socket, while one is being served. */
    int frontendDescriptor_ {-1};
    /* Front-end requests read during a backend wait, in arrival order. */
    std::deque<VhostUserMessage> deferredFrontendMessages_;
};

/* Serves one vhost-user device, on a thread of its own, to the guest of this
 * process. The guest's virtio_uml transport takes one end of a socketpair by
 * its connection name; the service answers on the other end until stop().
 * Every in-process device service is one of these around its own device. */
class InProcessVhostUserService final {
public:
    using DeviceFactory =
        std::function<std::unique_ptr<uml::virtio::VhostUserDevice>()>;

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
    DeviceFactory makeDevice_;
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
