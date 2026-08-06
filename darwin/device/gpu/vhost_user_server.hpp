// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"
#include "unique_descriptor.hpp"
#include "vhost_user_message.hpp"
#include "virtio_gpu_device.hpp"
#include "vnc_display.hpp"
#include "../input/vnc/vnc_input.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
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
        VncInputSink inputSink = {});
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

private:
    struct Mapping {
        std::uint64_t guestAddress {};
        std::uint64_t userAddress {};
        std::uint64_t size {};
        std::uint64_t mmapOffset {};
        UniqueDescriptor descriptor;
        std::uint8_t* address {};
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

    void resetConnection() noexcept;
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
    void signalQueue(std::uint32_t index);
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

    std::filesystem::path socketPath_;
    std::string vncBindAddress_;
    std::uint16_t vncPort_ {};
    VncDisplay display_;
    VirtioGpuDevice gpu_;
    UniqueDescriptor slaveRequestDescriptor_;
    std::vector<Mapping> mappings_;
    std::array<Queue, protocol::gpuQueueCount> queues_ {};
    std::uint64_t negotiatedFeatures_ {};
    std::uint64_t negotiatedProtocolFeatures_ {};
    bool featuresConfigured_ {};
    const std::atomic_bool* connectionStopRequested_ {};
};

} // namespace uml::gpu
