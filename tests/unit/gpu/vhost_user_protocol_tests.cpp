// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sys/mman.h>
#include <thread>

namespace uml::gpu::testing {
namespace {

void waitForConnectionClose(int socket)
{
    pollfd descriptor {.fd = socket, .events = POLLIN | POLLHUP};
    require(::poll(&descriptor, 1, 2000) == 1);
    std::uint8_t byte {};
    require(::read(socket, &byte, sizeof(byte)) == 0);
}

void negotiateProtocolFeatures(int socket, std::uint64_t features)
{
    constexpr std::uint64_t deviceFeatures =
        protocol::vhostFeatureProtocol;
    sendRequest(
        socket,
        protocol::VhostRequest::setFeatures,
        bytesOf(deviceFeatures),
        {},
        false);
    sendRequestAndReadAcknowledgement(
        socket,
        protocol::VhostRequest::setProtocolFeatures,
        bytesOf(features));
}

template<typename Value>
Value readValueReply(int socket, protocol::VhostRequest request)
{
    protocol::VhostHeader header {};
    Value value {};
    readExact(socket, &header, sizeof(header));
    readExact(socket, &value, sizeof(value));
    require(header.request == static_cast<std::uint32_t>(request));
    require((header.flags & protocol::vhostUserReply) != 0);
    require(header.size == sizeof(value));
    return value;
}

std::vector<std::uint8_t> configPayload(
    protocol::ConfigHeader header,
    std::span<const std::uint8_t> contents)
{
    std::vector<std::uint8_t> payload(sizeof(header) + contents.size());
    std::memcpy(payload.data(), &header, sizeof(header));
    if (!contents.empty()) {
        std::memcpy(
            payload.data() + sizeof(header),
            contents.data(),
            contents.size());
    }
    return payload;
}

template<typename Operation>
void expectRejected(VhostUserServer& server, Operation operation)
{
    withServerConnection(server, [&](int socket) {
        operation(socket);
        waitForConnectionClose(socket);
    });
}

class DeferredQueueDevice final : public uml::virtio::VhostUserDevice {
public:
    DeferredQueueDevice()
    {
        std::array<int, 2> descriptors {-1, -1};
        require(::pipe(descriptors.data()) == 0);
        readinessRead_ = UniqueDescriptor {descriptors[0]};
        readinessWrite_ = UniqueDescriptor {descriptors[1]};
    }

    [[nodiscard]] const char* name() const noexcept override
    {
        return "deferred queue fixture";
    }
    [[nodiscard]] std::uint64_t features() const noexcept override { return 0; }
    [[nodiscard]] std::size_t queueCount() const noexcept override { return 4; }
    [[nodiscard]] std::vector<std::uint8_t> configuration() const override
    {
        return {};
    }
    void writeConfiguration(
        std::uint32_t,
        std::uint32_t,
        std::span<const std::uint8_t>) override
    {
    }
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override
    {
        require(index == 3);
        require(std::ranges::equal(
            request, std::array<std::uint8_t, 4> {1, 2, 3, 4}));
        require(responseCapacity == 4);
        requestId_.store(requestId, std::memory_order_release);
        return {.action = uml::virtio::VhostUserQueueAction::defer};
    }
    void reset() noexcept override
    {
        requestId_.store(0, std::memory_order_release);
        std::lock_guard lock(completionMutex_);
        completion_.reset();
    }
    [[nodiscard]] int eventDescriptor() const noexcept override
    {
        return readinessRead_.get();
    }
    void drainEventSource() override
    {
        std::uint8_t ready {};
        require(::read(readinessRead_.get(), &ready, sizeof(ready)) == 1);
    }
    [[nodiscard]] bool takeQueueCompletion(
        uml::virtio::VhostUserQueueCompletion& completion) override
    {
        std::lock_guard lock(completionMutex_);
        if (!completion_)
            return false;
        completion = std::move(*completion_);
        completion_.reset();
        return true;
    }

    void complete()
    {
        const auto requestId = requestId_.load(std::memory_order_acquire);
        require(requestId != 0);
        {
            std::lock_guard lock(completionMutex_);
            completion_ = uml::virtio::VhostUserQueueCompletion {
                .requestId = requestId,
                .response = {9, 8, 7, 6},
            };
        }
        constexpr std::uint8_t ready = 1;
        require(::write(readinessWrite_.get(), &ready, sizeof(ready)) == 1);
    }

private:
    UniqueDescriptor readinessRead_;
    UniqueDescriptor readinessWrite_;
    std::atomic<std::uint64_t> requestId_ {};
    std::mutex completionMutex_;
    std::optional<uml::virtio::VhostUserQueueCompletion> completion_;
};

void deferredCompletionSupportsFourQueues()
{
    auto device = std::make_unique<DeferredQueueDevice>();
    auto* completionSource = device.get();
    VhostUserServer server {std::move(device)};
    auto memoryDescriptor = openMemoryDescriptor();
    void* mapped = ::mmap(
        nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
        memoryDescriptor.get(), 0);
    require(mapped != MAP_FAILED);
    auto memory = std::span {
        static_cast<std::uint8_t*>(mapped), static_cast<std::size_t>(4096)};

    constexpr std::uint32_t queueIndex = 3;
    constexpr std::uint64_t guestBase = 0x1000;
    constexpr std::uint64_t userBase = 0x1000'0000;
    constexpr std::size_t descriptorOffset = 0x000;
    constexpr std::size_t availableOffset = 0x100;
    constexpr std::size_t usedOffset = 0x200;
    constexpr std::size_t requestOffset = 0x300;
    constexpr std::size_t responseOffset = 0x400;
    const std::array descriptors {
        protocol::VirtqueueDescriptor {
            .address = guestBase + requestOffset,
            .length = 4,
            .flags = protocol::descriptorNext,
            .next = 1,
        },
        protocol::VirtqueueDescriptor {
            .address = guestBase + responseOffset,
            .length = 4,
            .flags = protocol::descriptorWrite,
            .next = 0,
        },
    };
    std::memcpy(memory.data() + descriptorOffset,
                descriptors.data(), sizeof(descriptors));
    const std::array<std::uint8_t, 4> request {1, 2, 3, 4};
    std::memcpy(memory.data() + requestOffset, request.data(), request.size());
    std::memset(memory.data() + responseOffset, 0xa5, 4);
    const std::uint16_t availableIndex = 1;
    const std::uint16_t head = 0;
    std::memcpy(memory.data() + availableOffset + sizeof(std::uint16_t),
                &availableIndex, sizeof(availableIndex));
    std::memcpy(memory.data() + availableOffset + protocol::splitRingHeaderSize,
                &head, sizeof(head));

    withServerConnection(server, [&](int socket) {
        sendRequest(socket, protocol::VhostRequest::getQueueNum, {}, {}, false);
        require(readValueReply<std::uint64_t>(
                    socket, protocol::VhostRequest::getQueueNum) == 4);
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
        configureMemory(socket, memoryDescriptor.get());
        const protocol::VringState queueSize {
            .index = queueIndex,
            .value = 8,
        };
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringNum, bytesOf(queueSize));
        const protocol::VringAddress addresses {
            .index = queueIndex,
            .flags = 0,
            .descriptor = userBase + descriptorOffset,
            .used = userBase + usedOffset,
            .available = userBase + availableOffset,
            .log = 0,
        };
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringAddr, bytesOf(addresses));
        const protocol::VringState enable {
            .index = queueIndex,
            .value = 1,
        };
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringEnable, bytesOf(enable));

        std::array<int, 2> callPipe {-1, -1};
        require(::pipe(callPipe.data()) == 0);
        UniqueDescriptor callRead {callPipe[0]};
        UniqueDescriptor callWrite {callPipe[1]};
        const std::uint64_t encodedQueue = queueIndex;
        const std::array callDescriptors {callWrite.get()};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringCall,
            bytesOf(encodedQueue), callDescriptors);

        std::array<int, 2> kickPipe {-1, -1};
        require(::pipe(kickPipe.data()) == 0);
        UniqueDescriptor kickRead {kickPipe[0]};
        UniqueDescriptor kickWrite {kickPipe[1]};
        const std::array kickDescriptors {kickRead.get()};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringKick,
            bytesOf(encodedQueue), kickDescriptors);

        auto& usedIndex = *reinterpret_cast<std::uint16_t*>(
            memory.data() + usedOffset + sizeof(std::uint16_t));
        require(std::atomic_ref<std::uint16_t>(usedIndex)
                    .load(std::memory_order_acquire) == 0);
        completionSource->complete();
        pollfd completion {.fd = callRead.get(), .events = POLLIN};
        require(::poll(&completion, 1, 2000) == 1);
        std::uint64_t completionCount {};
        readExact(callRead.get(), &completionCount, sizeof(completionCount));
        require(completionCount == 1);
        require(std::atomic_ref<std::uint16_t>(usedIndex)
                    .load(std::memory_order_acquire) == 1);
        require(std::ranges::equal(
            memory.subspan(responseOffset, 4),
            std::array<std::uint8_t, 4> {9, 8, 7, 6}));
    });
    require(::munmap(mapped, 4096) == 0);
}

void validConfigurationProtocol(VhostUserServer& server)
{
    withServerConnection(server, [](int socket) {
        // A back-end that advertises VHOST_USER_F_PROTOCOL_FEATURES must
        // support protocol-feature discovery before SET_FEATURES.
        sendRequest(
            socket,
            protocol::VhostRequest::getProtocolFeatures,
            {},
            {},
            false);
        const auto offered = readValueReply<std::uint64_t>(
            socket, protocol::VhostRequest::getProtocolFeatures);
        require(
            (offered & protocol::protocolFeatureMultipleQueues) != 0);

        // MQ queue-count discovery is permitted after the feature was queried
        // and before the selected protocol-feature mask is sent.
        sendRequest(
            socket, protocol::VhostRequest::getQueueNum, {}, {}, false);
        require(readValueReply<std::uint64_t>(
                    socket, protocol::VhostRequest::getQueueNum) ==
                protocol::gpuQueueCount);

        constexpr std::uint64_t deviceFeatures =
            protocol::vhostFeatureProtocol;
        sendRequest(
            socket,
            protocol::VhostRequest::setFeatures,
            bytesOf(deviceFeatures),
            {},
            false);
        constexpr std::uint64_t features =
            protocol::protocolFeatureMultipleQueues |
            protocol::protocolFeatureConfig;
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setProtocolFeatures,
            bytesOf(features));

        const protocol::ConfigHeader getHeader {
            .offset = 0,
            .size = sizeof(protocol::GpuConfig),
            .flags = protocol::configFlagWritable,
        };
        const std::array<std::uint8_t, sizeof(protocol::GpuConfig)> zeros {};
        const auto getPayload = configPayload(getHeader, zeros);
        sendRequest(
            socket,
            protocol::VhostRequest::getConfig,
            getPayload,
            {},
            false);
        protocol::VhostHeader response {};
        std::vector<std::uint8_t> responsePayload(getPayload.size());
        readExact(socket, &response, sizeof(response));
        readExact(
            socket, responsePayload.data(), responsePayload.size());
        require(
            response.request ==
            static_cast<std::uint32_t>(protocol::VhostRequest::getConfig));
        require((response.flags & protocol::vhostUserReply) != 0);
        require(response.size == getPayload.size());
        protocol::ConfigHeader returnedHeader {};
        protocol::GpuConfig configuration {};
        std::memcpy(
            &returnedHeader, responsePayload.data(), sizeof(returnedHeader));
        std::memcpy(
            &configuration,
            responsePayload.data() + sizeof(returnedHeader),
            sizeof(configuration));
        require(returnedHeader.offset == 0);
        require(returnedHeader.size == sizeof(configuration));
        require(returnedHeader.flags == protocol::configFlagWritable);
        require(configuration.eventsRead == 0);
        require(configuration.scanoutCount == 1);
        require(configuration.capsetCount == 2);

        const std::uint32_t clearEvents = UINT32_MAX;
        const protocol::ConfigHeader clearHeader {
            .offset = offsetof(protocol::GpuConfig, eventsClear),
            .size = sizeof(clearEvents),
            .flags = protocol::configFlagWritable,
        };
        const auto clearPayload = configPayload(clearHeader, bytesOf(clearEvents));
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setConfig, clearPayload);

        const std::uint32_t restoredScanoutCount = 1;
        const protocol::ConfigHeader migrationHeader {
            .offset = offsetof(protocol::GpuConfig, scanoutCount),
            .size = sizeof(restoredScanoutCount),
            .flags = protocol::configFlagMigration,
        };
        const auto migrationPayload =
            configPayload(migrationHeader, bytesOf(restoredScanoutCount));
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setConfig, migrationPayload);
    });
}

void invalidControlMessagesAreRejected(VhostUserServer& server)
{
    expectRejected(server, [](int socket) {
        auto descriptor = openTestDescriptor();
        const std::array descriptors {descriptor.get()};
        sendRequest(
            socket,
            protocol::VhostRequest::getFeatures,
            {},
            descriptors);
    });
    expectRejected(server, [](int socket) {
        constexpr std::uint64_t deviceFeatures =
            protocol::vhostFeatureProtocol;
        sendRequest(
            socket,
            protocol::VhostRequest::setFeatures,
            bytesOf(deviceFeatures),
            {},
            false);
        const protocol::VringState enable {.index = 0, .value = 2};
        sendRequest(
            socket,
            protocol::VhostRequest::setVringEnable,
            bytesOf(enable));
    });
    expectRejected(server, [](int socket) {
        const protocol::VringState enable {.index = 0, .value = 1};
        sendRequest(
            socket,
            protocol::VhostRequest::setVringEnable,
            bytesOf(enable));
    });
    expectRejected(server, [](int socket) {
        const protocol::VringState queueSize {.index = 0, .value = 3};
        sendRequest(
            socket,
            protocol::VhostRequest::setVringNum,
            bytesOf(queueSize));
    });
    expectRejected(server, [](int socket) {
        protocol::VringAddress address {};
        address.flags = 1;
        sendRequest(
            socket,
            protocol::VhostRequest::setVringAddr,
            bytesOf(address));
    });
    expectRejected(server, [](int socket) {
        constexpr std::uint64_t encoded =
            protocol::vhostVringNoDescriptor | (1ULL << 9U);
        sendRequest(
            socket,
            protocol::VhostRequest::setVringCall,
            bytesOf(encoded));
    });
    expectRejected(server, [](int socket) {
        constexpr std::uint64_t queue = 0;
        sendRequest(
            socket,
            protocol::VhostRequest::setVringKick,
            bytesOf(queue));
    });
    expectRejected(server, [](int socket) {
        auto descriptor = openTestDescriptor();
        constexpr std::uint64_t queue = protocol::vhostVringNoDescriptor;
        const std::array descriptors {descriptor.get()};
        sendRequest(
            socket,
            protocol::VhostRequest::setVringKick,
            bytesOf(queue),
            descriptors);
    });
    expectRejected(server, [](int socket) {
        const protocol::VringState kick {.index = 0, .value = 0};
        sendRequest(
            socket,
            protocol::VhostRequest::vringKick,
            bytesOf(kick));
    });
    expectRejected(server, [](int socket) {
        constexpr std::uint64_t deviceFeatures =
            protocol::vhostFeatureProtocol;
        sendRequest(
            socket,
            protocol::VhostRequest::setFeatures,
            bytesOf(deviceFeatures),
            {},
            false);
        constexpr std::uint64_t invalidFeatures =
            protocol::protocolFeatureInbandNotifications;
        sendRequest(
            socket,
            protocol::VhostRequest::setProtocolFeatures,
            bytesOf(invalidFeatures));
    });
    expectRejected(server, [](int socket) {
        constexpr std::uint64_t features =
            protocol::protocolFeatureInbandNotifications |
            protocol::protocolFeatureSlaveRequest |
            protocol::protocolFeatureReplyAck;
        negotiateProtocolFeatures(socket, features);
        const protocol::VringState kick {.index = 0, .value = 1};
        sendRequest(
            socket,
            protocol::VhostRequest::vringKick,
            bytesOf(kick));
    });
    expectRejected(server, [](int socket) {
        const protocol::VringState state {.index = 0, .value = 1};
        sendRequest(
            socket,
            protocol::VhostRequest::getVringBase,
            bytesOf(state));
    });
    expectRejected(server, [](int socket) {
        auto descriptor = openMemoryDescriptor();
        const protocol::MemoryTableHeader table {
            .regionCount = 1,
            .padding = 1,
        };
        const protocol::MemoryRegion region {
            .guestAddress = 0x1000,
            .size = 4096,
            .userAddress = 0x1000'0000,
            .mmapOffset = 0,
        };
        std::array<std::uint8_t, sizeof(table) + sizeof(region)> payload {};
        std::memcpy(payload.data(), &table, sizeof(table));
        std::memcpy(
            payload.data() + sizeof(table), &region, sizeof(region));
        const std::array descriptors {descriptor.get()};
        sendRequest(
            socket,
            protocol::VhostRequest::setMemTable,
            payload,
            descriptors);
    });
}

void invalidConfigurationMessagesAreRejected(VhostUserServer& server)
{
    const auto rejectSet = [&server](protocol::ConfigHeader header,
                                     std::span<const std::uint8_t> contents) {
        expectRejected(server, [&](int socket) {
            negotiateProtocolFeatures(
                socket, protocol::protocolFeatureConfig);
            const auto payload = configPayload(header, contents);
            sendRequest(
                socket, protocol::VhostRequest::setConfig, payload);
        });
    };

    const std::uint32_t value = 1;
    rejectSet(
        {
            .offset = offsetof(protocol::GpuConfig, eventsRead),
            .size = sizeof(value),
            .flags = protocol::configFlagWritable,
        },
        bytesOf(value));
    rejectSet(
        {
            .offset = offsetof(protocol::GpuConfig, eventsClear),
            .size = sizeof(value),
            .flags = 2,
        },
        bytesOf(value));
    rejectSet(
        {
            .offset = sizeof(protocol::GpuConfig) + 1U,
            .size = sizeof(value),
            .flags = protocol::configFlagMigration,
        },
        bytesOf(value));
    rejectSet(
        {
            .offset = offsetof(protocol::GpuConfig, eventsClear),
            .size = sizeof(value) + 1U,
            .flags = protocol::configFlagWritable,
        },
        bytesOf(value));

    withServerConnection(server, [](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureConfig);
        const protocol::ConfigHeader header {
            .offset = 0,
            .size = sizeof(protocol::GpuConfig),
            .flags = protocol::configFlagMigration,
        };
        const std::array<std::uint8_t, sizeof(protocol::GpuConfig)> zeros {};
        const auto payload = configPayload(header, zeros);
        sendRequest(
            socket,
            protocol::VhostRequest::getConfig,
            payload,
            {},
            false);
        protocol::VhostHeader response {};
        readExact(socket, &response, sizeof(response));
        require(
            response.request ==
            static_cast<std::uint32_t>(protocol::VhostRequest::getConfig));
        require((response.flags & protocol::vhostUserReply) != 0);
        require(response.size == 0);
    });
    expectRejected(server, [](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureConfig);
        const protocol::ConfigHeader header {
            .offset = 0,
            .size = sizeof(protocol::GpuConfig),
            .flags = protocol::configFlagWritable,
        };
        sendRequest(
            socket,
            protocol::VhostRequest::getConfig,
            bytesOf(header));
    });
}

void misalignedTranslatedRingsAreRejected(VhostUserServer& server)
{
    auto memoryDescriptor = openMemoryDescriptor();
    expectRejected(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);

        constexpr std::uint64_t guestBase = 0x1000;
        constexpr std::uint64_t skewedUserBase = 0x1000'0001;
        const protocol::MemoryRegion region {
            .guestAddress = guestBase,
            .size = 4096,
            .userAddress = skewedUserBase,
            .mmapOffset = 0,
        };
        const auto payload = memoryTablePayload(std::span {&region, 1U});
        const std::array memoryDescriptors {memoryDescriptor.get()};
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setMemTable,
            payload,
            memoryDescriptors);

        const protocol::VringState queueSize {.index = 0, .value = 8};
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setVringNum,
            bytesOf(queueSize));
        /* Each opaque address satisfies its wire alignment, but translation
         * through the odd region base would produce odd host pointers. */
        const protocol::VringAddress addresses {
            .index = 0,
            .flags = 0,
            .descriptor = 0x1000'0010,
            .used = 0x1000'0200,
            .available = 0x1000'0100,
            .log = 0,
        };
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setVringAddr,
            bytesOf(addresses));
        const protocol::VringState enable {.index = 0, .value = 1};
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setVringEnable,
            bytesOf(enable));
        constexpr std::uint64_t pollingQueue =
            protocol::vhostVringNoDescriptor;
        sendRequest(
            socket,
            protocol::VhostRequest::setVringKick,
            bytesOf(pollingQueue));
    });
}

enum class QueueDeviceCommand {
    displayInfo,
    contextCreate,
    resourceCreate2d,
};

struct QueueScenario final {
    bool descriptorNotification {true};
    std::uint16_t availableIndex {1};
    bool reservedDescriptorFlag {};
    bool reservedAvailableFlag {};
    bool pollingCompletion {};
    bool resetOwnerBeforeStart {};
    bool resetOwnerRecreatesContext {};
    bool reversedDescriptorOrder {};
    bool suppressInterrupt {};
    bool enableQueue {true};
    bool inbandNotification {};
    bool legacyFeatureNegotiation {};
    QueueDeviceCommand command {QueueDeviceCommand::displayInfo};
    // Zero selects the complete response for the requested command. Tests set
    // an explicit smaller capacity only when exercising response preflight.
    std::uint32_t responseLength {};
};

void queueNotificationProcessesQueue(
    VhostUserServer& server,
    QueueScenario scenario)
{
    auto memoryDescriptor = openMemoryDescriptor();
    void* mapped = ::mmap(
        nullptr,
        4096,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        memoryDescriptor.get(),
        0);
    require(mapped != MAP_FAILED);
    auto memory = std::span {
        static_cast<std::uint8_t*>(mapped), static_cast<std::size_t>(4096)};

    constexpr std::uint64_t guestBase = 0x1000;
    constexpr std::uint64_t userBase = 0x1000'0000;
    constexpr std::size_t descriptorOffset = 0x000;
    constexpr std::size_t availableOffset = 0x100;
    constexpr std::size_t usedOffset = 0x200;
    constexpr std::size_t requestOffset = 0x300;
    constexpr std::size_t responseOffset = 0x400;
    const bool createContext = scenario.resetOwnerRecreatesContext ||
        scenario.command == QueueDeviceCommand::contextCreate;
    const bool createResource =
        scenario.command == QueueDeviceCommand::resourceCreate2d;
    const auto requestLength = createContext ?
        sizeof(protocol::GpuContextCreate) :
        createResource ? sizeof(protocol::GpuResourceCreate2d) :
                         sizeof(protocol::GpuControlHeader);
    const std::uint32_t completeResponseLength =
        createContext || createResource ?
        sizeof(protocol::GpuControlHeader) :
        sizeof(protocol::GpuDisplayInfo);
    const auto responseLength = scenario.responseLength == 0 ?
        completeResponseLength : scenario.responseLength;
    std::array<protocol::VirtqueueDescriptor, 2> descriptors {
        protocol::VirtqueueDescriptor {
            .address = guestBase + requestOffset,
            .length = static_cast<std::uint32_t>(requestLength),
            .flags = static_cast<std::uint16_t>(
                protocol::descriptorNext |
                (scenario.reservedDescriptorFlag ? 8U : 0U)),
            .next = 1,
        },
        protocol::VirtqueueDescriptor {
            .address = guestBase + responseOffset,
            .length = static_cast<std::uint32_t>(responseLength),
            .flags = protocol::descriptorWrite,
            .next = 0,
        },
    };
    if (scenario.reversedDescriptorOrder) {
        descriptors = {
            protocol::VirtqueueDescriptor {
                .address = guestBase + responseOffset,
                .length = responseLength,
                .flags = static_cast<std::uint16_t>(
                    protocol::descriptorWrite | protocol::descriptorNext),
                .next = 1,
            },
            protocol::VirtqueueDescriptor {
                .address = guestBase + requestOffset,
                .length = static_cast<std::uint32_t>(requestLength),
                .flags = 0,
                .next = 0,
            },
        };
    }
    std::memcpy(
        memory.data() + descriptorOffset,
        descriptors.data(),
        sizeof(descriptors));
    if (createContext) {
        protocol::GpuContextCreate request {};
        request.header.type =
            static_cast<std::uint32_t>(protocol::GpuCommand::contextCreate);
        request.header.contextId = 41;
        request.contextInit = protocol::virglCapsetId;
        std::memcpy(memory.data() + requestOffset, &request, sizeof(request));
    } else if (createResource) {
        protocol::GpuResourceCreate2d request {};
        request.header.type = static_cast<std::uint32_t>(
            protocol::GpuCommand::resourceCreate2d);
        request.resourceId = 73;
        request.format = static_cast<std::uint32_t>(
            protocol::GpuFormat::b8g8r8a8Unorm);
        request.width = 4;
        request.height = 4;
        std::memcpy(memory.data() + requestOffset, &request, sizeof(request));
    } else {
        protocol::GpuControlHeader request {};
        request.type =
            static_cast<std::uint32_t>(protocol::GpuCommand::getDisplayInfo);
        std::memcpy(memory.data() + requestOffset, &request, sizeof(request));
    }
    const std::uint16_t head = 0;
    const std::uint16_t availableFlags =
        static_cast<std::uint16_t>(
            (scenario.suppressInterrupt ?
                 protocol::availableSuppressInterrupt : 0U) |
            (scenario.reservedAvailableFlag ? 2U : 0U));
    const std::uint16_t initialAvailableFlags = 0;
    std::memcpy(
        memory.data() + availableOffset,
        &initialAvailableFlags,
        sizeof(initialAvailableFlags));
    std::memcpy(
        memory.data() + availableOffset + protocol::splitRingHeaderSize,
        &head,
        sizeof(head));
    constexpr std::uint8_t responseSentinel = 0xa5;
    std::memset(
        memory.data() + responseOffset,
        responseSentinel,
        sizeof(protocol::GpuDisplayInfo));

    withServerConnection(server, [&](int socket) {
        if (scenario.legacyFeatureNegotiation) {
            constexpr std::uint64_t deviceFeatures =
                protocol::virtioFeatureVersion1;
            sendRequest(
                socket,
                protocol::VhostRequest::setFeatures,
                bytesOf(deviceFeatures),
                {},
                false);
        } else {
            std::uint64_t protocolFeatures =
                protocol::protocolFeatureReplyAck;
            if (scenario.inbandNotification) {
                protocolFeatures |=
                    protocol::protocolFeatureInbandNotifications |
                    protocol::protocolFeatureSlaveRequest;
            }
            negotiateProtocolFeatures(socket, protocolFeatures);
        }
        configureMemory(socket, memoryDescriptor.get());
        const protocol::VringState queueSize {.index = 0, .value = 8};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringNum, bytesOf(queueSize));
        const protocol::VringAddress addresses {
            .index = 0,
            .flags = 0,
            .descriptor = userBase + descriptorOffset,
            .used = userBase + usedOffset,
            .available = userBase + availableOffset,
            .log = 0,
        };
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setVringAddr,
            bytesOf(addresses));
        const protocol::VringState enable {.index = 0, .value = 1};
        if (scenario.enableQueue && !scenario.legacyFeatureNegotiation) {
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringEnable,
                bytesOf(enable));
        }

        constexpr std::uint64_t queue = 0;
        UniqueDescriptor callRead;
        UniqueDescriptor callWrite;
        SocketPair backendChannel;
        if (scenario.inbandNotification) {
            backendChannel = makeSocketPair();
            const std::array backendDescriptors {
                backendChannel.descriptors[1].get()};
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setSlaveRequestFd,
                {},
                backendDescriptors);
        } else if (scenario.pollingCompletion) {
            constexpr std::uint64_t pollingQueue =
                protocol::vhostVringNoDescriptor;
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringCall,
                bytesOf(pollingQueue));
        } else {
            std::array<int, 2> callPipe {-1, -1};
            require(::pipe(callPipe.data()) == 0);
            callRead = UniqueDescriptor {callPipe[0]};
            callWrite = UniqueDescriptor {callPipe[1]};
            const std::array callDescriptors {callWrite.get()};
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringCall,
                bytesOf(queue),
                callDescriptors);
        }

        if (scenario.resetOwnerBeforeStart) {
            sendRequestAndReadAcknowledgement(
                socket, protocol::VhostRequest::resetOwner, {});
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringEnable,
                bytesOf(enable));
        }

        UniqueDescriptor kickRead;
        UniqueDescriptor kickWrite;
        if (scenario.inbandNotification) {
            // VRING_KICK travels over the main channel below.
        } else if (scenario.descriptorNotification) {
            std::array<int, 2> kickPipe {-1, -1};
            require(::pipe(kickPipe.data()) == 0);
            kickRead = UniqueDescriptor {kickPipe[0]};
            kickWrite = UniqueDescriptor {kickPipe[1]};
            const std::array kickDescriptors {kickRead.get()};
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringKick,
                bytesOf(queue),
                kickDescriptors);
        } else {
            constexpr std::uint64_t pollingQueue =
                protocol::vhostVringNoDescriptor;
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringKick,
                bytesOf(pollingQueue));
        }

        auto& sharedAvailableFlags = *reinterpret_cast<std::uint16_t*>(
            memory.data() + availableOffset);
        auto& sharedAvailableIndex = *reinterpret_cast<std::uint16_t*>(
            memory.data() + availableOffset + sizeof(std::uint16_t));
        std::atomic_ref<std::uint16_t>(sharedAvailableFlags)
            .store(availableFlags, std::memory_order_relaxed);
        std::atomic_ref<std::uint16_t>(sharedAvailableIndex)
            .store(scenario.availableIndex, std::memory_order_release);
        if (scenario.inbandNotification) {
            const protocol::VringState kick {.index = 0, .value = 0};
            sendRequest(
                socket,
                protocol::VhostRequest::vringKick,
                bytesOf(kick));

            protocol::VhostHeader notification {};
            protocol::VringState notifiedQueue {};
            readExact(
                backendChannel.descriptors[0].get(),
                &notification,
                sizeof(notification));
            readExact(
                backendChannel.descriptors[0].get(),
                &notifiedQueue,
                sizeof(notifiedQueue));
            require(
                notification.request == static_cast<std::uint32_t>(
                    protocol::VhostSlaveRequest::vringCall));
            require(
                notification.flags ==
                (protocol::vhostUserVersion |
                 protocol::vhostUserNeedReply));
            require(notification.size == sizeof(notifiedQueue));
            require(notifiedQueue.index == 0);
            require(notifiedQueue.value == 0);

            const std::uint64_t status = 0;
            const protocol::VhostHeader reply {
                .request = notification.request,
                .flags = protocol::vhostUserVersion |
                    protocol::vhostUserReply,
                .size = sizeof(status),
            };
            sendMessageWithDescriptors(
                backendChannel.descriptors[0].get(),
                reply,
                {},
                bytesOf(status));
            readAcknowledgement(
                socket, protocol::VhostRequest::vringKick);
        } else if (scenario.descriptorNotification) {
            constexpr std::uint64_t kick = 1;
            require(::write(kickWrite.get(), &kick, sizeof(kick)) ==
                    static_cast<ssize_t>(sizeof(kick)));
        }
        if (scenario.availableIndex > 8 ||
            scenario.reservedDescriptorFlag ||
            scenario.reservedAvailableFlag ||
            scenario.reversedDescriptorOrder ||
            ((scenario.enableQueue || scenario.legacyFeatureNegotiation) &&
             responseLength < completeResponseLength)) {
            waitForConnectionClose(socket);
            if (responseLength < sizeof(protocol::GpuDisplayInfo)) {
                require(std::ranges::all_of(
                    memory.subspan(
                        responseOffset, sizeof(protocol::GpuDisplayInfo)),
                    [](std::uint8_t byte) {
                        return byte == responseSentinel;
                    }));
            }
            return;
        }
        if (scenario.inbandNotification) {
            // The backend-request acknowledgement above is the completion
            // synchronization point.
        } else if (scenario.pollingCompletion || scenario.suppressInterrupt) {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(2);
            std::uint16_t observedUsedIndex {};
            do {
                auto& sharedUsedIndex = *reinterpret_cast<std::uint16_t*>(
                    memory.data() + usedOffset + sizeof(std::uint16_t));
                observedUsedIndex =
                    std::atomic_ref<std::uint16_t>(sharedUsedIndex)
                        .load(std::memory_order_acquire);
                if (observedUsedIndex == 1)
                    break;
                std::this_thread::yield();
            } while (std::chrono::steady_clock::now() < deadline);
            require(observedUsedIndex == 1);
            if (scenario.suppressInterrupt &&
                !scenario.pollingCompletion) {
                pollfd completion {.fd = callRead.get(), .events = POLLIN};
                require(::poll(&completion, 1, 50) == 0);
            }
        } else {
            pollfd completion {.fd = callRead.get(), .events = POLLIN};
            require(::poll(&completion, 1, 2000) == 1);
            std::uint64_t completionCount {};
            readExact(callRead.get(), &completionCount, sizeof(completionCount));
            require(completionCount == 1);
        }

        auto& sharedUsedIndex = *reinterpret_cast<std::uint16_t*>(
            memory.data() + usedOffset + sizeof(std::uint16_t));
        const auto usedIndex =
            std::atomic_ref<std::uint16_t>(sharedUsedIndex)
                .load(std::memory_order_acquire);
        require(usedIndex == 1);
        protocol::GpuControlHeader response {};
        std::memcpy(
            &response, memory.data() + responseOffset, sizeof(response));
        if (scenario.enableQueue || scenario.legacyFeatureNegotiation) {
            require(
                response.type == static_cast<std::uint32_t>(
                    createContext || createResource ?
                        protocol::GpuResponse::okNoData :
                        protocol::GpuResponse::okDisplayInfo));
        } else {
            require(std::ranges::all_of(
                memory.subspan(
                    responseOffset, sizeof(protocol::GpuDisplayInfo)),
                [](std::uint8_t byte) {
                    return byte == responseSentinel;
                }));
            protocol::VirtqueueUsedElement usedElement {};
            std::memcpy(
                &usedElement,
                memory.data() + usedOffset + protocol::splitRingHeaderSize,
                sizeof(usedElement));
            require(usedElement.length == 0);
        }

        if (scenario.resetOwnerRecreatesContext) {
            /* The second identical context creation succeeds only if
             * RESET_OWNER retired device state, while retaining the reusable
             * memory table and notification endpoints. */
            sendRequestAndReadAcknowledgement(
                socket, protocol::VhostRequest::resetOwner, {});
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringEnable,
                bytesOf(enable));
            const std::array kickDescriptors {kickRead.get()};
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringKick,
                bytesOf(queue),
                kickDescriptors);

            const std::uint16_t secondHead = 0;
            std::memcpy(
                memory.data() + availableOffset +
                    protocol::splitRingHeaderSize + sizeof(secondHead),
                &secondHead,
                sizeof(secondHead));
            std::memset(
                memory.data() + responseOffset,
                responseSentinel,
                sizeof(protocol::GpuControlHeader));
            std::atomic_ref<std::uint16_t>(sharedAvailableIndex)
                .store(2, std::memory_order_release);
            constexpr std::uint64_t secondKick = 1;
            require(::write(
                kickWrite.get(), &secondKick, sizeof(secondKick)) ==
                static_cast<ssize_t>(sizeof(secondKick)));

            pollfd completion {.fd = callRead.get(), .events = POLLIN};
            require(::poll(&completion, 1, 2000) == 1);
            std::uint64_t completionCount {};
            readExact(
                callRead.get(), &completionCount, sizeof(completionCount));
            require(completionCount == 1);
            require(std::atomic_ref<std::uint16_t>(sharedUsedIndex)
                .load(std::memory_order_acquire) == 2);
            std::memcpy(
                &response, memory.data() + responseOffset, sizeof(response));
            require(response.type == static_cast<std::uint32_t>(
                protocol::GpuResponse::okNoData));
        }
    });
    require(::munmap(mapped, 4096) == 0);
}

void disconnectReconnectIsPristine(VhostUserServer& server)
{
    // Each helper invocation opens and closes a distinct transport connection.
    // The second identical command can succeed only if disconnect retired both
    // host bookkeeping and the corresponding renderer object from the first.
    for (unsigned connection = 0; connection < 2; ++connection) {
        queueNotificationProcessesQueue(
            server, {.command = QueueDeviceCommand::contextCreate});
    }
    for (unsigned connection = 0; connection < 2; ++connection) {
        queueNotificationProcessesQueue(
            server, {.command = QueueDeviceCommand::resourceCreate2d});
    }
}

} // namespace

void runVhostUserProtocolTests()
{
    const auto before = descriptorCount();
    deferredCompletionSupportsFourQueues();
    {
        VhostUserServer server {
            {}, "127.0.0.1", 0, {}, 64, 64, false};
        validConfigurationProtocol(server);
        invalidControlMessagesAreRejected(server);
        invalidConfigurationMessagesAreRejected(server);
        misalignedTranslatedRingsAreRejected(server);
        queueNotificationProcessesQueue(server, {});
        queueNotificationProcessesQueue(
            server, {.descriptorNotification = false});
        queueNotificationProcessesQueue(
            server, {.availableIndex = 9});
        queueNotificationProcessesQueue(
            server, {.reservedDescriptorFlag = true});
        queueNotificationProcessesQueue(
            server, {.reservedAvailableFlag = true});
        queueNotificationProcessesQueue(
            server, {.pollingCompletion = true});
        queueNotificationProcessesQueue(
            server, {.resetOwnerBeforeStart = true});
        queueNotificationProcessesQueue(
            server, {.resetOwnerRecreatesContext = true});
        disconnectReconnectIsPristine(server);
        queueNotificationProcessesQueue(
            server, {.reversedDescriptorOrder = true});
        queueNotificationProcessesQueue(
            server, {.suppressInterrupt = true});
        queueNotificationProcessesQueue(
            server, {.enableQueue = false});
        queueNotificationProcessesQueue(
            server, {.inbandNotification = true});
        queueNotificationProcessesQueue(
            server, {.legacyFeatureNegotiation = true});
        queueNotificationProcessesQueue(
            server,
            {.responseLength = sizeof(protocol::GpuControlHeader)});
    }
    require(descriptorCount() == before);
}

} // namespace uml::gpu::testing
