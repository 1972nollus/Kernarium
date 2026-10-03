// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"
#include "gpu_vhost_user_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <deque>
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

class MultiDeferredQueueDevice final : public uml::virtio::VhostUserDevice {
public:
    MultiDeferredQueueDevice()
    {
        std::array<int, 2> descriptors {-1, -1};
        require(::pipe(descriptors.data()) == 0);
        readinessRead_ = UniqueDescriptor {descriptors[0]};
        readinessWrite_ = UniqueDescriptor {descriptors[1]};
    }

    [[nodiscard]] const char* name() const noexcept override
    {
        return "multi-deferred queue fixture";
    }
    [[nodiscard]] std::uint64_t features() const noexcept override { return 0; }
    [[nodiscard]] std::size_t queueCount() const noexcept override { return 1; }
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
        require(index == 0);
        require(request.size() == 1);
        require(responseCapacity == 1);
        std::lock_guard lock(mutex_);
        requestIds_.push_back(requestId);
        requestValues_.push_back(request.front());
        return {.action = uml::virtio::VhostUserQueueAction::defer};
    }
    void reset() noexcept override
    {
        std::lock_guard lock(mutex_);
        requestIds_.clear();
        requestValues_.clear();
        completions_.clear();
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
        std::lock_guard lock(mutex_);
        if (completions_.empty())
            return false;
        completion = std::move(completions_.front());
        completions_.pop_front();
        return true;
    }

    void complete(std::span<const std::size_t> order)
    {
        {
            std::lock_guard lock(mutex_);
            require(requestIds_.size() == 3);
            require(requestValues_.size() == 3);
            require(std::ranges::equal(
                requestValues_, std::array<std::uint8_t, 3> {1, 2, 3}));
            for (const auto index : order) {
                require(index < requestIds_.size());
                completions_.push_back(
                    uml::virtio::VhostUserQueueCompletion {
                        .requestId = requestIds_[index],
                        .response = {
                            static_cast<std::uint8_t>(0xa0U + index)},
                    });
            }
        }
        constexpr std::uint8_t ready = 1;
        require(::write(readinessWrite_.get(), &ready, sizeof(ready)) == 1);
    }

private:
    UniqueDescriptor readinessRead_;
    UniqueDescriptor readinessWrite_;
    std::mutex mutex_;
    std::vector<std::uint64_t> requestIds_;
    std::vector<std::uint8_t> requestValues_;
    std::deque<uml::virtio::VhostUserQueueCompletion> completions_;
};

class CompleteThenRetryDevice final : public uml::virtio::VhostUserDevice {
public:
    [[nodiscard]] const char* name() const noexcept override
    {
        return "complete-then-retry fixture";
    }
    [[nodiscard]] std::uint64_t features() const noexcept override { return 0; }
    [[nodiscard]] std::size_t queueCount() const noexcept override { return 1; }
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
        std::uint64_t,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override
    {
        require(index == 0);
        require(request.empty());
        require(responseCapacity == 4);
        const auto attempt = attempts_.fetch_add(1, std::memory_order_relaxed);
        if (attempt == 0) {
            return {
                .action = uml::virtio::VhostUserQueueAction::complete,
                .response = {9, 8, 7, 6},
            };
        }
        return {.action = uml::virtio::VhostUserQueueAction::retry};
    }
    void reset() noexcept override
    {
        attempts_.store(0, std::memory_order_relaxed);
    }
    [[nodiscard]] unsigned attempts() const noexcept
    {
        return attempts_.load(std::memory_order_relaxed);
    }

private:
    std::atomic_uint attempts_ {};
};

class ConfigChangeDevice final : public uml::virtio::VhostUserDevice {
public:
    ConfigChangeDevice()
    {
        std::array<int, 2> descriptors {-1, -1};
        require(::pipe(descriptors.data()) == 0);
        readinessRead_ = UniqueDescriptor {descriptors[0]};
        readinessWrite_ = UniqueDescriptor {descriptors[1]};
    }

    [[nodiscard]] const char* name() const noexcept override
    {
        return "config change fixture";
    }
    [[nodiscard]] std::uint64_t features() const noexcept override { return 0; }
    [[nodiscard]] std::size_t queueCount() const noexcept override { return 1; }
    [[nodiscard]] std::vector<std::uint8_t> configuration() const override
    {
        protocol::GpuConfig config {
            .eventsRead = 0,
            .eventsClear = 0,
            .scanoutCount = 1,
            .capsetCount = 0,
        };
        const auto generation =
            generation_.load(std::memory_order_acquire);
        if (generation !=
            clearedGeneration_.load(std::memory_order_acquire)) {
            config.eventsRead = protocol::gpuEventDisplay;
        }
        const auto* first = reinterpret_cast<const std::uint8_t*>(&config);
        return {first, first + sizeof(config)};
    }
    void configurationRead(
        std::uint32_t offset,
        std::uint32_t size) noexcept override
    {
        constexpr std::uint32_t eventsReadOffset =
            offsetof(protocol::GpuConfig, eventsRead);
        constexpr std::uint32_t eventsReadEnd =
            eventsReadOffset + sizeof(protocol::GpuConfig::eventsRead);
        const auto readEnd =
            static_cast<std::uint64_t>(offset) + size;
        if (offset <= eventsReadOffset && readEnd >= eventsReadEnd) {
            reportedGeneration_.store(
                generation_.load(std::memory_order_acquire),
                std::memory_order_release);
        }
    }
    void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) override
    {
        require(offset == offsetof(protocol::GpuConfig, eventsClear));
        require(flags == protocol::configFlagWritable);
        require(bytes.size() == sizeof(std::uint32_t));
        std::uint32_t events {};
        std::memcpy(&events, bytes.data(), sizeof(events));
        if ((events & protocol::gpuEventDisplay) == 0)
            return;
        clearedGeneration_.store(
            reportedGeneration_.load(std::memory_order_acquire),
            std::memory_order_release);
        if (generation_.load(std::memory_order_acquire) !=
            clearedGeneration_.load(std::memory_order_acquire)) {
            pending_.store(true, std::memory_order_release);
        }
    }
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t,
        std::uint64_t,
        std::span<const std::uint8_t>,
        std::size_t) override
    {
        return {};
    }
    void reset() noexcept override
    {
        pending_.store(false, std::memory_order_release);
        generation_.store(0, std::memory_order_release);
        reportedGeneration_.store(0, std::memory_order_release);
        clearedGeneration_.store(0, std::memory_order_release);
    }
    [[nodiscard]] int eventDescriptor() const noexcept override
    {
        return readinessRead_.get();
    }
    void drainEventSource() override
    {
        std::uint8_t ready {};
        require(::read(readinessRead_.get(), &ready, sizeof(ready)) == 1);
        pending_.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool configChangePending() const noexcept override
    {
        return pending_.load(std::memory_order_acquire);
    }
    void clearConfigChange() noexcept override
    {
        pending_.store(false, std::memory_order_release);
    }

    void trigger()
    {
        generation_.fetch_add(1, std::memory_order_acq_rel);
        constexpr std::uint8_t ready = 1;
        require(::write(readinessWrite_.get(), &ready, sizeof(ready)) == 1);
    }

    [[nodiscard]] bool pending() const noexcept
    {
        return pending_.load(std::memory_order_acquire);
    }

private:
    UniqueDescriptor readinessRead_;
    UniqueDescriptor readinessWrite_;
    std::atomic_bool pending_ {};
    std::atomic_uint64_t generation_ {};
    mutable std::atomic_uint64_t reportedGeneration_ {};
    std::atomic_uint64_t clearedGeneration_ {};
};

void configChangeWaitsForTheBackendChannelAndRequiresAcknowledgement()
{
    auto device = std::make_unique<ConfigChangeDevice>();
    auto* source = device.get();
    VhostUserServer server {std::move(device)};
    withServerConnection(server, [&](int socket) {
        constexpr std::uint64_t features =
            protocol::protocolFeatureReplyAck |
            protocol::protocolFeatureSlaveRequest |
            protocol::protocolFeatureConfig;
        negotiateProtocolFeatures(socket, features);

        /* Publish before SET_SLAVE_REQ_FD to prove the transport retains the
         * notification until it has a channel on which to deliver it. */
        source->trigger();
        auto backend = makeSocketPair();
        const std::array descriptors {backend.descriptors[1].get()};
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setSlaveRequestFd,
            {},
            descriptors);

        pollfd ready {
            .fd = backend.descriptors[0].get(),
            .events = POLLIN,
        };
        require(::poll(&ready, 1, 2000) == 1);
        protocol::VhostHeader notification {};
        readExact(
            backend.descriptors[0].get(),
            &notification,
            sizeof(notification));
        require(notification.request == static_cast<std::uint32_t>(
            protocol::VhostSlaveRequest::configChange));
        require(notification.flags ==
            (protocol::vhostUserVersion | protocol::vhostUserNeedReply));
        require(notification.size == 0);
        require(source->pending());

        const auto acknowledge = [&](const protocol::VhostHeader& request) {
            const protocol::VhostHeader acknowledgement {
                .request = request.request,
                .flags =
                    protocol::vhostUserVersion | protocol::vhostUserReply,
                .size = sizeof(std::uint64_t),
            };
            constexpr std::uint64_t status = 0;
            sendMessageWithDescriptors(
                backend.descriptors[0].get(),
                acknowledgement,
                {},
                bytesOf(status));
        };
        const auto waitUntilDelivered = [&] {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds {2};
            while (source->pending() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            require(!source->pending());
        };
        const auto readNotification = [&] {
            pollfd descriptor {
                .fd = backend.descriptors[0].get(),
                .events = POLLIN,
            };
            require(::poll(&descriptor, 1, 2000) == 1);
            protocol::VhostHeader next {};
            readExact(backend.descriptors[0].get(), &next, sizeof(next));
            require(next.request == static_cast<std::uint32_t>(
                protocol::VhostSlaveRequest::configChange));
            require(next.flags ==
                (protocol::vhostUserVersion | protocol::vhostUserNeedReply));
            require(next.size == 0);
            return next;
        };
        const auto readConfig = [&] {
            const protocol::ConfigHeader header {
                .offset = 0,
                .size = sizeof(protocol::GpuConfig),
                .flags = protocol::configFlagWritable,
            };
            const std::array<std::uint8_t, sizeof(protocol::GpuConfig)> zeros {};
            sendRequest(
                socket,
                protocol::VhostRequest::getConfig,
                configPayload(header, zeros),
                {},
                false);
            protocol::VhostHeader response {};
            std::array<
                std::uint8_t,
                sizeof(protocol::ConfigHeader) + sizeof(protocol::GpuConfig)>
                payload {};
            readExact(socket, &response, sizeof(response));
            readExact(socket, payload.data(), payload.size());
            require(response.request == static_cast<std::uint32_t>(
                protocol::VhostRequest::getConfig));
            require(response.size == payload.size());
            protocol::GpuConfig config {};
            std::memcpy(
                &config,
                payload.data() + sizeof(protocol::ConfigHeader),
                sizeof(config));
            return config;
        };
        const auto clearDisplayEvent = [&] {
            constexpr std::uint32_t events = protocol::gpuEventDisplay;
            const protocol::ConfigHeader header {
                .offset = offsetof(protocol::GpuConfig, eventsClear),
                .size = sizeof(events),
                .flags = protocol::configFlagWritable,
            };
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setConfig,
                configPayload(header, bytesOf(events)));
        };
        const auto readScanoutCount = [&] {
            const protocol::ConfigHeader header {
                .offset = offsetof(protocol::GpuConfig, scanoutCount),
                .size = sizeof(protocol::GpuConfig::scanoutCount),
                .flags = protocol::configFlagWritable,
            };
            const std::array<
                std::uint8_t,
                sizeof(protocol::GpuConfig::scanoutCount)> zeros {};
            sendRequest(
                socket,
                protocol::VhostRequest::getConfig,
                configPayload(header, zeros),
                {},
                false);
            protocol::VhostHeader response {};
            std::array<
                std::uint8_t,
                sizeof(protocol::ConfigHeader) +
                    sizeof(protocol::GpuConfig::scanoutCount)> payload {};
            readExact(socket, &response, sizeof(response));
            readExact(socket, payload.data(), payload.size());
            require(response.request == static_cast<std::uint32_t>(
                protocol::VhostRequest::getConfig));
            require(response.size == payload.size());
            std::uint32_t scanoutCount {};
            std::memcpy(
                &scanoutCount,
                payload.data() + sizeof(protocol::ConfigHeader),
                sizeof(scanoutCount));
            return scanoutCount;
        };

        acknowledge(notification);
        waitUntilDelivered();
        require(readConfig().eventsRead == protocol::gpuEventDisplay);

        /* A second resize reaches the IRQ path while the first config work is
         * still represented by its old events_read snapshot. Its interrupt
         * may coalesce, so clearing that snapshot must enqueue another
         * backend notification after SET_CONFIG has been acknowledged. */
        source->trigger();
        const auto coalesced = readNotification();
        acknowledge(coalesced);
        waitUntilDelivered();
        require(readScanoutCount() == 1);
        clearDisplayEvent();
        const auto rearmed = readNotification();
        acknowledge(rearmed);
        waitUntilDelivered();

        require(readConfig().eventsRead == protocol::gpuEventDisplay);
        clearDisplayEvent();
        pollfd unexpected {
            .fd = backend.descriptors[0].get(),
            .events = POLLIN,
        };
        require(::poll(&unexpected, 1, 100) == 0);
    });
}

/* A UML front-end reads a GET_CONFIG reply from the main socket without
 * draining the backend channel, and the guest CPU that drains it can be the
 * one waiting. A config change still waiting for its acknowledgement must
 * therefore not stop the server from answering; a request that is not a
 * query waits for the acknowledgement and keeps its order. */
void configChangeWaitStillAnswersConfigReads()
{
    auto device = std::make_unique<ConfigChangeDevice>();
    auto* source = device.get();
    VhostUserServer server {std::move(device)};
    withServerConnection(server, [&](int socket) {
        constexpr std::uint64_t features =
            protocol::protocolFeatureReplyAck |
            protocol::protocolFeatureSlaveRequest |
            protocol::protocolFeatureConfig;
        negotiateProtocolFeatures(socket, features);
        auto backend = makeSocketPair();
        const std::array descriptors {backend.descriptors[1].get()};
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setSlaveRequestFd,
            {},
            descriptors);

        source->trigger();
        pollfd notified {
            .fd = backend.descriptors[0].get(),
            .events = POLLIN,
        };
        require(::poll(&notified, 1, 2000) == 1);
        protocol::VhostHeader notification {};
        readExact(
            backend.descriptors[0].get(),
            &notification,
            sizeof(notification));
        require(notification.request == static_cast<std::uint32_t>(
            protocol::VhostSlaveRequest::configChange));
        require((notification.flags & protocol::vhostUserNeedReply) != 0);

        // The acknowledgement is withheld: the server is now waiting.
        const protocol::ConfigHeader configHeader {
            .offset = 0,
            .size = sizeof(protocol::GpuConfig),
            .flags = protocol::configFlagWritable,
        };
        const std::array<std::uint8_t, sizeof(protocol::GpuConfig)> zeros {};
        sendRequest(
            socket,
            protocol::VhostRequest::getConfig,
            configPayload(configHeader, zeros),
            {},
            false);
        pollfd answered {.fd = socket, .events = POLLIN};
        require(::poll(&answered, 1, 2000) == 1);
        protocol::VhostHeader response {};
        std::array<
            std::uint8_t,
            sizeof(protocol::ConfigHeader) + sizeof(protocol::GpuConfig)>
            payload {};
        readExact(socket, &response, sizeof(response));
        readExact(socket, payload.data(), payload.size());
        require(response.request == static_cast<std::uint32_t>(
            protocol::VhostRequest::getConfig));
        require(response.size == payload.size());
        protocol::GpuConfig config {};
        std::memcpy(
            &config,
            payload.data() + sizeof(protocol::ConfigHeader),
            sizeof(config));
        require(config.eventsRead == protocol::gpuEventDisplay);

        // GET_VRING_BASE changes queue state, so it waits for the reply.
        const protocol::VringState ring {.index = 0, .value = 0};
        sendRequest(
            socket,
            protocol::VhostRequest::getVringBase,
            bytesOf(ring),
            {},
            false);
        pollfd early {.fd = socket, .events = POLLIN};
        require(::poll(&early, 1, 200) == 0);
        require(source->pending());

        const protocol::VhostHeader acknowledgement {
            .request = notification.request,
            .flags = protocol::vhostUserVersion | protocol::vhostUserReply,
            .size = sizeof(std::uint64_t),
        };
        constexpr std::uint64_t status = 0;
        sendMessageWithDescriptors(
            backend.descriptors[0].get(),
            acknowledgement,
            {},
            bytesOf(status));
        const auto base = readValueReply<protocol::VringState>(
            socket, protocol::VhostRequest::getVringBase);
        require(base.index == 0 && base.value == 0);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds {2};
        while (source->pending() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        require(!source->pending());
    });
}

class SharedMemoryDevice final : public uml::virtio::VhostUserDevice {
public:
    explicit SharedMemoryDevice(std::size_t actionCount = 1)
        : actionCount_(actionCount)
    {
    }

    [[nodiscard]] const char* name() const noexcept override
    {
        return "shared-memory fixture";
    }
    [[nodiscard]] std::uint64_t features() const noexcept override { return 0; }
    [[nodiscard]] std::size_t queueCount() const noexcept override { return 1; }
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
        std::uint32_t,
        std::uint64_t,
        std::span<const std::uint8_t>,
        std::size_t) override
    {
        return {.action = uml::virtio::VhostUserQueueAction::retry};
    }
    void reset() noexcept override {}
    void prepareReset() noexcept override
    {
        if (actionIssued_)
            return;
        actionIssued_ = true;
        try {
            actions_.reserve(actionCount_);
            for (std::size_t index = 0; index < actionCount_; ++index) {
                FILE* file = std::tmpfile();
                if (file == nullptr)
                    return;
                const int descriptor = ::dup(::fileno(file));
                const bool sized = descriptor >= 0 &&
                    ::ftruncate(descriptor, 0x8000) == 0;
                std::fclose(file);
                if (!sized) {
                    if (descriptor >= 0)
                        ::close(descriptor);
                    return;
                }
                actions_.push_back(
                    uml::virtio::VhostUserSharedMemoryAction {
                        .type = uml::virtio::VhostUserSharedMemoryAction::Type::map,
                        .actionId = 77 + index,
                        .regionId = protocol::hostVisibleSharedMemoryRegionId,
                        .descriptorOffset = 0x4000,
                        .regionOffset = 0x8000 + index * 0x4000,
                        .length = 0x4000,
                        .writable = true,
                        .descriptor = UniqueDescriptor {descriptor},
                    });
            }
        } catch (...) {
            actions_.clear();
        }
    }
    [[nodiscard]] std::optional<uml::virtio::VhostUserSharedMemoryRegion>
        sharedMemoryRegion() const noexcept override
    {
        return uml::virtio::VhostUserSharedMemoryRegion {
            .id = protocol::hostVisibleSharedMemoryRegionId,
            .size = protocol::hostVisibleSharedMemoryRegionSize,
        };
    }
    [[nodiscard]] bool takeSharedMemoryAction(
        uml::virtio::VhostUserSharedMemoryAction& action) override
    {
        if (actions_.empty())
            return false;
        action = std::move(actions_.front());
        actions_.erase(actions_.begin());
        return true;
    }
    void completeSharedMemoryAction(
        std::uint64_t actionId,
        bool success) noexcept override
    {
        completedAction_ = actionId;
        ++completedActionCount_;
        actionSucceeded_ = actionSucceeded_ && success;
    }
    [[nodiscard]] std::uint64_t completedAction() const noexcept
    {
        return completedAction_;
    }
    [[nodiscard]] bool actionSucceeded() const noexcept
    {
        return actionSucceeded_;
    }
    [[nodiscard]] std::size_t completedActionCount() const noexcept
    {
        return completedActionCount_;
    }

private:
    std::size_t actionCount_;
    std::vector<uml::virtio::VhostUserSharedMemoryAction> actions_;
    bool actionIssued_ {};
    std::uint64_t completedAction_ {};
    std::size_t completedActionCount_ {};
    bool actionSucceeded_ {true};
};

void sharedMemoryRequiresBackendAcknowledgements()
{
    auto device = std::make_unique<SharedMemoryDevice>();
    VhostUserServer server {std::move(device)};
    expectRejected(server, [](int socket) {
        constexpr std::uint64_t deviceFeatures =
            protocol::vhostFeatureProtocol;
        sendRequest(
            socket,
            protocol::VhostRequest::setFeatures,
            bytesOf(deviceFeatures),
            {},
            false);
        constexpr std::uint64_t incompleteFeatures =
            protocol::protocolFeatureSharedMemoryMap |
            protocol::protocolFeatureSlaveRequest;
        sendRequest(
            socket,
            protocol::VhostRequest::setProtocolFeatures,
            bytesOf(incompleteFeatures));
    });
}

void sharedMemoryConfigurationRequiresNegotiation()
{
    auto device = std::make_unique<SharedMemoryDevice>();
    VhostUserServer server {std::move(device)};
    expectRejected(server, [](int socket) {
        sendRequest(
            socket,
            protocol::VhostRequest::getSharedMemoryConfig,
            {},
            {},
            false);
    });
}

void standardizedSharedMemoryNegotiationBatchesBackendRequestsBeforeReply()
{
    auto device = std::make_unique<SharedMemoryDevice>(2);
    auto* observed = device.get();
    VhostUserServer server {std::move(device)};
    withServerConnection(server, [&](int socket) {
        sendRequest(
            socket,
            protocol::VhostRequest::getProtocolFeatures,
            {},
            {},
            false);
        const auto offered = readValueReply<std::uint64_t>(
            socket, protocol::VhostRequest::getProtocolFeatures);
        require((offered & protocol::protocolFeatureSharedMemoryMap) != 0);

        constexpr std::uint64_t deviceFeatures =
            protocol::vhostFeatureProtocol;
        sendRequest(
            socket,
            protocol::VhostRequest::setFeatures,
            bytesOf(deviceFeatures),
            {},
            false);
        constexpr std::uint64_t protocolFeatures =
            protocol::protocolFeatureReplyAck |
            protocol::protocolFeatureSlaveRequest |
            protocol::protocolFeatureSharedMemoryMap;
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setProtocolFeatures,
            bytesOf(protocolFeatures));

        std::array<int, 2> backendSockets {-1, -1};
        require(::socketpair(
            AF_UNIX, SOCK_STREAM, 0, backendSockets.data()) == 0);
        UniqueDescriptor backendClient {backendSockets[0]};
        UniqueDescriptor backendServer {backendSockets[1]};
        const std::array backendDescriptors {backendServer.get()};
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setSlaveRequestFd,
            {},
            backendDescriptors);

        sendRequest(
            socket,
            protocol::VhostRequest::getSharedMemoryConfig,
            {},
            {},
            false);
        const auto config = readValueReply<protocol::SharedMemoryConfig>(
            socket, protocol::VhostRequest::getSharedMemoryConfig);
        require(config.regionCount == 1);
        require(config.regionSizes[0] == 0);
        require(config.regionSizes[protocol::hostVisibleSharedMemoryRegionId] ==
            protocol::hostVisibleSharedMemoryRegionSize);

        sendRequest(
            socket, protocol::VhostRequest::resetOwner, {});
        for (std::size_t index = 0; index < 2; ++index) {
            pollfd prematureReply {.fd = socket, .events = POLLIN};
            require(::poll(&prematureReply, 1, 50) == 0);

            VhostUserMessage backendMessage;
            require(receiveVhostUserMessage(
                backendClient.get(), backendMessage));
            require(backendMessage.header.request == static_cast<std::uint32_t>(
                protocol::VhostSlaveRequest::sharedMemoryMap));
            require(backendMessage.header.flags ==
                (protocol::vhostUserVersion | protocol::vhostUserNeedReply));
            require(backendMessage.descriptors.size() == 1);
            require(backendMessage.payload.size() ==
                sizeof(protocol::SharedMemoryMap));
            protocol::SharedMemoryMap map {};
            std::memcpy(&map, backendMessage.payload.data(), sizeof(map));
            require(map.regionId == protocol::hostVisibleSharedMemoryRegionId);
            require(map.descriptorOffset == 0x4000);
            require(map.regionOffset == 0x8000 + index * 0x4000);
            require(map.length == 0x4000);
            require(map.flags == protocol::sharedMemoryMapReadWrite);

            const std::uint64_t status = 0;
            const protocol::VhostHeader reply {
                .request = backendMessage.header.request,
                .flags = protocol::vhostUserVersion | protocol::vhostUserReply,
                .size = sizeof(status),
            };
            sendMessageWithDescriptors(
                backendClient.get(), reply, {}, bytesOf(status));
        }
        readAcknowledgement(socket, protocol::VhostRequest::resetOwner);
        sendRequest(
            socket, protocol::VhostRequest::getFeatures, {}, {}, false);
        (void)readValueReply<std::uint64_t>(
            socket, protocol::VhostRequest::getFeatures);
        require(observed->completedAction() == 78);
        require(observed->completedActionCount() == 2);
        require(observed->actionSucceeded());
    });
}

void rejectedSharedMemoryActionClosesBackendChannel()
{
    auto device = std::make_unique<SharedMemoryDevice>();
    auto* observed = device.get();
    VhostUserServer server {std::move(device)};
    withServerConnection(server, [&](int socket) {
        constexpr std::uint64_t protocolFeatures =
            protocol::protocolFeatureReplyAck |
            protocol::protocolFeatureSlaveRequest |
            protocol::protocolFeatureSharedMemoryMap;
        negotiateProtocolFeatures(socket, protocolFeatures);

        std::array<int, 2> backendSockets {-1, -1};
        require(::socketpair(
            AF_UNIX, SOCK_STREAM, 0, backendSockets.data()) == 0);
        UniqueDescriptor backendClient {backendSockets[0]};
        UniqueDescriptor backendServer {backendSockets[1]};
        const std::array backendDescriptors {backendServer.get()};
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setSlaveRequestFd,
            {},
            backendDescriptors);
        backendServer.reset();

        sendRequest(socket, protocol::VhostRequest::resetOwner, {});
        VhostUserMessage backendMessage;
        require(receiveVhostUserMessage(
            backendClient.get(), backendMessage));
        require(backendMessage.descriptors.size() == 1);

        const std::uint64_t status = 1;
        const protocol::VhostHeader reply {
            .request = backendMessage.header.request,
            .flags = protocol::vhostUserVersion | protocol::vhostUserReply,
            .size = sizeof(status),
        };
        sendMessageWithDescriptors(
            backendClient.get(), reply, {}, bytesOf(status));
        waitForConnectionClose(backendClient.get());
        require(observed->completedAction() == 77);
        require(!observed->actionSucceeded());
    });
}

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
        sendRequest(
            socket, protocol::VhostRequest::getFeatures, {}, {}, false);
        (void)readValueReply<std::uint64_t>(
            socket, protocol::VhostRequest::getFeatures);

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

void multipleDeferredCompletionsRetainTheirDescriptorChains()
{
    auto device = std::make_unique<MultiDeferredQueueDevice>();
    auto* completionSource = device.get();
    VhostUserServer server {std::move(device)};
    auto memoryDescriptor = openMemoryDescriptor();
    void* mapped = ::mmap(
        nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
        memoryDescriptor.get(), 0);
    require(mapped != MAP_FAILED);
    auto memory = std::span {
        static_cast<std::uint8_t*>(mapped), static_cast<std::size_t>(4096)};

    constexpr std::uint64_t guestBase = 0x1000;
    constexpr std::uint64_t userBase = 0x1000'0000;
    constexpr std::size_t descriptorOffset = 0x000;
    constexpr std::size_t availableOffset = 0x100;
    constexpr std::size_t usedOffset = 0x200;
    constexpr std::array<std::size_t, 3> requestOffsets {
        0x300, 0x310, 0x320};
    constexpr std::array<std::size_t, 3> responseOffsets {
        0x400, 0x410, 0x420};
    std::array<protocol::VirtqueueDescriptor, 6> descriptors {};
    std::array<std::uint16_t, 3> heads {};
    for (std::size_t index = 0; index < requestOffsets.size(); ++index) {
        const auto requestDescriptor = static_cast<std::uint16_t>(index * 2U);
        const auto responseDescriptor =
            static_cast<std::uint16_t>(requestDescriptor + 1U);
        heads[index] = requestDescriptor;
        descriptors[requestDescriptor] = {
            .address = guestBase + requestOffsets[index],
            .length = 1,
            .flags = protocol::descriptorNext,
            .next = responseDescriptor,
        };
        descriptors[responseDescriptor] = {
            .address = guestBase + responseOffsets[index],
            .length = 1,
            .flags = protocol::descriptorWrite,
            .next = 0,
        };
        memory[requestOffsets[index]] = static_cast<std::uint8_t>(index + 1U);
        memory[responseOffsets[index]] = 0x5a;
    }
    std::memcpy(
        memory.data() + descriptorOffset,
        descriptors.data(),
        sizeof(descriptors));
    const std::uint16_t availableIndex = 3;
    std::memcpy(
        memory.data() + availableOffset + sizeof(std::uint16_t),
        &availableIndex,
        sizeof(availableIndex));
    std::memcpy(
        memory.data() + availableOffset + protocol::splitRingHeaderSize,
        heads.data(),
        sizeof(heads));

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
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
            socket, protocol::VhostRequest::setVringAddr, bytesOf(addresses));
        const protocol::VringState enable {.index = 0, .value = 1};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringEnable, bytesOf(enable));

        std::array<int, 2> callPipe {-1, -1};
        require(::pipe(callPipe.data()) == 0);
        UniqueDescriptor callRead {callPipe[0]};
        UniqueDescriptor callWrite {callPipe[1]};
        constexpr std::uint64_t queue = 0;
        const std::array callDescriptors {callWrite.get()};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringCall,
            bytesOf(queue), callDescriptors);

        std::array<int, 2> kickPipe {-1, -1};
        require(::pipe(kickPipe.data()) == 0);
        UniqueDescriptor kickRead {kickPipe[0]};
        UniqueDescriptor kickWrite {kickPipe[1]};
        const std::array kickDescriptors {kickRead.get()};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringKick,
            bytesOf(queue), kickDescriptors);

        auto& usedIndex = *reinterpret_cast<std::uint16_t*>(
            memory.data() + usedOffset + sizeof(std::uint16_t));
        require(std::atomic_ref<std::uint16_t>(usedIndex)
                    .load(std::memory_order_acquire) == 0);
        constexpr std::array<std::size_t, 3> completionOrder {2, 0, 1};
        completionSource->complete(completionOrder);
        pollfd completion {.fd = callRead.get(), .events = POLLIN};
        require(::poll(&completion, 1, 2000) == 1);
        std::uint64_t completionCount {};
        readExact(callRead.get(), &completionCount, sizeof(completionCount));
        require(completionCount == 1);
        require(std::atomic_ref<std::uint16_t>(usedIndex)
                    .load(std::memory_order_acquire) == 3);

        for (std::size_t used = 0; used < completionOrder.size(); ++used) {
            protocol::VirtqueueUsedElement element {};
            std::memcpy(
                &element,
                memory.data() + usedOffset + protocol::splitRingHeaderSize +
                    used * sizeof(element),
                sizeof(element));
            require(element.id == heads[completionOrder[used]]);
            require(element.length == 1);
        }
        for (std::size_t index = 0; index < responseOffsets.size(); ++index) {
            require(memory[responseOffsets[index]] ==
                static_cast<std::uint8_t>(0xa0U + index));
        }
    });
    require(::munmap(mapped, 4096) == 0);
}

void completedPrefixNotifiesBeforeRetry()
{
    auto device = std::make_unique<CompleteThenRetryDevice>();
    auto* observed = device.get();
    VhostUserServer server {std::move(device)};
    auto memoryDescriptor = openMemoryDescriptor();
    void* mapped = ::mmap(
        nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
        memoryDescriptor.get(), 0);
    require(mapped != MAP_FAILED);
    auto memory = std::span {
        static_cast<std::uint8_t*>(mapped), static_cast<std::size_t>(4096)};

    constexpr std::uint64_t guestBase = 0x1000;
    constexpr std::uint64_t userBase = 0x1000'0000;
    constexpr std::size_t descriptorOffset = 0x000;
    constexpr std::size_t availableOffset = 0x100;
    constexpr std::size_t usedOffset = 0x200;
    constexpr std::size_t firstResponseOffset = 0x300;
    constexpr std::size_t secondResponseOffset = 0x400;
    const std::array descriptors {
        protocol::VirtqueueDescriptor {
            .address = guestBase + firstResponseOffset,
            .length = 4,
            .flags = protocol::descriptorWrite,
            .next = 0,
        },
        protocol::VirtqueueDescriptor {
            .address = guestBase + secondResponseOffset,
            .length = 4,
            .flags = protocol::descriptorWrite,
            .next = 0,
        },
    };
    std::memcpy(
        memory.data() + descriptorOffset, descriptors.data(),
        sizeof(descriptors));
    const std::uint16_t availableIndex = 2;
    const std::array<std::uint16_t, 2> heads {0, 1};
    std::memcpy(
        memory.data() + availableOffset + sizeof(std::uint16_t),
        &availableIndex, sizeof(availableIndex));
    std::memcpy(
        memory.data() + availableOffset + protocol::splitRingHeaderSize,
        heads.data(), sizeof(heads));
    constexpr std::uint8_t responseSentinel = 0xa5;
    std::memset(memory.data() + firstResponseOffset, responseSentinel, 4);
    std::memset(memory.data() + secondResponseOffset, responseSentinel, 4);

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
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
            socket, protocol::VhostRequest::setVringAddr, bytesOf(addresses));
        const protocol::VringState enable {.index = 0, .value = 1};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringEnable, bytesOf(enable));

        std::array<int, 2> callPipe {-1, -1};
        require(::pipe(callPipe.data()) == 0);
        UniqueDescriptor callRead {callPipe[0]};
        UniqueDescriptor callWrite {callPipe[1]};
        constexpr std::uint64_t queue = 0;
        const std::array callDescriptors {callWrite.get()};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringCall,
            bytesOf(queue), callDescriptors);

        std::array<int, 2> kickPipe {-1, -1};
        require(::pipe(kickPipe.data()) == 0);
        UniqueDescriptor kickRead {kickPipe[0]};
        UniqueDescriptor kickWrite {kickPipe[1]};
        const std::array kickDescriptors {kickRead.get()};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringKick,
            bytesOf(queue), kickDescriptors);

        pollfd completion {.fd = callRead.get(), .events = POLLIN};
        require(::poll(&completion, 1, 2000) == 1);
        std::uint64_t completionCount {};
        readExact(callRead.get(), &completionCount, sizeof(completionCount));
        require(completionCount == 1);
        auto& usedIndex = *reinterpret_cast<std::uint16_t*>(
            memory.data() + usedOffset + sizeof(std::uint16_t));
        require(std::atomic_ref<std::uint16_t>(usedIndex)
                    .load(std::memory_order_acquire) == 1);
        require(observed->attempts() == 2);
        require(std::ranges::equal(
            memory.subspan(firstResponseOffset, 4),
            std::array<std::uint8_t, 4> {9, 8, 7, 6}));
        require(std::ranges::all_of(
            memory.subspan(secondResponseOffset, 4),
            [](std::uint8_t byte) { return byte == responseSentinel; }));
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
        require(configuration.capsetCount == protocol::venusCapsetIndex + 1U);

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
        readAcknowledgement(
            socket, protocol::VhostRequest::setVringKick);
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
            sendRequest(
                socket, protocol::VhostRequest::getFeatures, {}, {}, false);
            (void)readValueReply<std::uint64_t>(
                socket, protocol::VhostRequest::getFeatures);
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
            // The subsequent main-channel reply above is the completion
            // synchronization point after the backend acknowledgement.
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
    multipleDeferredCompletionsRetainTheirDescriptorChains();
    deferredCompletionSupportsFourQueues();
    completedPrefixNotifiesBeforeRetry();
    configChangeWaitsForTheBackendChannelAndRequiresAcknowledgement();
    configChangeWaitStillAnswersConfigReads();
    sharedMemoryRequiresBackendAcknowledgements();
    sharedMemoryConfigurationRequiresNegotiation();
    standardizedSharedMemoryNegotiationBatchesBackendRequestsBeforeReply();
    rejectedSharedMemoryActionClosesBackendChannel();
    {
        const auto ownedServer = makeGpuVhostUserServer(
            "127.0.0.1", 0, {}, 64, 64, false);
        auto& server = *ownedServer;
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
