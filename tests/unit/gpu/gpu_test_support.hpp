// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "unique_descriptor.hpp"
#include "vhost_user_server.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace uml::gpu::testing {

using ::uml::testing::require;
using ::uml::testing::ScratchBacking;

void runVhostUserServerTests(const char* executable);
[[noreturn]] void runVhostUserServerChild(std::string_view mode);
void runVhostUserProtocolTests();
void runInProcessServiceTests();

[[nodiscard]] inline std::size_t descriptorCount()
{
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator {"/dev/fd"}) {
        (void)entry;
        ++count;
    }
    return count;
}

struct SocketPair {
    std::array<UniqueDescriptor, 2> descriptors;
};

[[nodiscard]] inline SocketPair makeSocketPair()
{
    std::array<int, 2> raw {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, raw.data()) == 0);
    return {{UniqueDescriptor {raw[0]}, UniqueDescriptor {raw[1]}}};
}

[[nodiscard]] inline UniqueDescriptor openTestDescriptor()
{
    const int descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    require(descriptor >= 0);
    return UniqueDescriptor {descriptor};
}

struct Pipe {
    UniqueDescriptor readEnd;
    UniqueDescriptor writeEnd;
};

[[nodiscard]] inline Pipe makePipe()
{
    std::array<int, 2> raw {-1, -1};
    require(::pipe(raw.data()) == 0);
    return {UniqueDescriptor {raw[0]}, UniqueDescriptor {raw[1]}};
}

// The name of the scratch file behind a test's guest memory.
inline constexpr std::string_view memoryBackingName = "uml-gpu-memory.XXXXXX";

inline void sendBytesWithDescriptors(
    int socket,
    const void* bytes,
    std::size_t size,
    std::span<const int> descriptors)
{
    constexpr std::size_t maximumTestDescriptors =
        maximumVhostUserDescriptors + 1U;
    require(descriptors.size() <= maximumTestDescriptors);
    if (descriptors.empty()) {
        require(::write(socket, bytes, size) == static_cast<ssize_t>(size));
        return;
    }

    alignas(cmsghdr) std::array<
        std::uint8_t,
        CMSG_SPACE(sizeof(int) * maximumTestDescriptors)> control {};
    iovec vector {
        .iov_base = const_cast<void*>(bytes),
        .iov_len = size,
    };
    msghdr message {
        .msg_name = nullptr,
        .msg_namelen = 0,
        .msg_iov = &vector,
        .msg_iovlen = 1,
        .msg_control = control.data(),
        .msg_controllen = static_cast<socklen_t>(
            CMSG_SPACE(sizeof(int) * descriptors.size())),
        .msg_flags = 0,
    };
    auto* header = CMSG_FIRSTHDR(&message);
    require(header != nullptr);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int) * descriptors.size());
    std::memcpy(
        CMSG_DATA(header),
        descriptors.data(),
        sizeof(int) * descriptors.size());
    require(::sendmsg(socket, &message, 0) == static_cast<ssize_t>(size));
}

inline void sendMessageWithDescriptors(
    int socket,
    const protocol::VhostHeader& header,
    std::span<const int> descriptors,
    std::span<const std::uint8_t> payload = {})
{
    std::vector<std::uint8_t> bytes(sizeof(header) + payload.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    if (!payload.empty()) {
        std::memcpy(
            bytes.data() + sizeof(header), payload.data(), payload.size());
    }
    sendBytesWithDescriptors(socket, bytes.data(), bytes.size(), descriptors);
}

inline void sendRequest(
    int socket,
    protocol::VhostRequest request,
    std::span<const std::uint8_t> payload,
    std::span<const int> descriptors = {},
    bool needReply = true)
{
    const protocol::VhostHeader header {
        .request = static_cast<std::uint32_t>(request),
        .flags = protocol::vhostUserVersion |
            (needReply ? protocol::vhostUserNeedReply : 0U),
        .size = static_cast<std::uint32_t>(payload.size()),
    };
    sendMessageWithDescriptors(socket, header, descriptors, payload);
}

template<typename Value>
[[nodiscard]] inline std::span<const std::uint8_t> bytesOf(
    const Value& value)
{
    return {
        reinterpret_cast<const std::uint8_t*>(&value),
        sizeof(value),
    };
}

inline void readExact(int descriptor, void* destination, std::size_t size)
{
    auto* bytes = static_cast<std::uint8_t*>(destination);
    while (size != 0) {
        const auto count = ::read(descriptor, bytes, size);
        require(count > 0);
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
}

inline void readAcknowledgement(int socket, protocol::VhostRequest request)
{
    protocol::VhostHeader header {};
    std::uint64_t status = 1;
    readExact(socket, &header, sizeof(header));
    readExact(socket, &status, sizeof(status));
    require(header.request == static_cast<std::uint32_t>(request));
    require((header.flags & protocol::vhostUserReply) != 0);
    require(header.size == sizeof(status));
    require(status == 0);
}

inline void sendRequestAndReadAcknowledgement(
    int socket,
    protocol::VhostRequest request,
    std::span<const std::uint8_t> payload,
    std::span<const int> descriptors = {})
{
    sendRequest(socket, request, payload, descriptors);
    readAcknowledgement(socket, request);
}

/* Negotiates the vhost-user protocol features `features` the way a front end
 * does, with the device offering only the protocol-features bit. */
inline void negotiateProtocolFeatures(int socket, std::uint64_t features)
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

[[nodiscard]] inline std::vector<std::uint8_t> memoryTablePayload(
    std::span<const protocol::MemoryRegion> regions)
{
    const protocol::MemoryTableHeader table {
        .regionCount = static_cast<std::uint32_t>(regions.size()),
        .padding = 0,
    };
    std::vector<std::uint8_t> payload(
        sizeof(table) + regions.size_bytes());
    std::memcpy(payload.data(), &table, sizeof(table));
    std::memcpy(
        payload.data() + sizeof(table), regions.data(), regions.size_bytes());
    return payload;
}

inline void configureMemory(
    int socket,
    int descriptor,
    std::uint64_t size = 4096,
    std::uint64_t mmapOffset = 0)
{
    const protocol::MemoryRegion region {
        .guestAddress = 0x1000,
        .size = size,
        .userAddress = 0x1000'0000,
        .mmapOffset = mmapOffset,
    };
    const auto payload = memoryTablePayload(std::span {&region, 1U});
    const std::array descriptors {descriptor};
    sendRequestAndReadAcknowledgement(
        socket,
        protocol::VhostRequest::setMemTable,
        payload,
        descriptors);
}

/* One page of guest memory holding a split virtqueue at fixed offsets, mapped
 * into the test. configureMemory publishes it at guest address 0x1000 and user
 * address 0x1000'0000. Each test lays out its own descriptors and buffers. */
class SplitRing {
public:
    static constexpr std::size_t size = 4096;
    static constexpr std::uint64_t guestBase = 0x1000;
    static constexpr std::uint64_t userBase = 0x1000'0000;
    static constexpr std::size_t descriptorOffset = 0x000;
    static constexpr std::size_t availableOffset = 0x100;
    static constexpr std::size_t usedOffset = 0x200;

    explicit SplitRing(
        const std::source_location location = std::source_location::current())
        : backing_(memoryBackingName, size, location)
    {
        void* mapped = ::mmap(
            nullptr,
            size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            backing_.descriptor(),
            0);
        require(mapped != MAP_FAILED, "mmap", location);
        memory_ = {static_cast<std::uint8_t*>(mapped), size};
    }
    ~SplitRing() { require(::munmap(memory_.data(), size) == 0); }
    SplitRing(const SplitRing&) = delete;
    SplitRing& operator=(const SplitRing&) = delete;

    [[nodiscard]] int descriptor() const noexcept
    {
        return backing_.descriptor();
    }
    [[nodiscard]] std::span<std::uint8_t> memory() const noexcept
    {
        return memory_;
    }

    void writeDescriptors(
        std::span<const protocol::VirtqueueDescriptor> descriptors) const
    {
        std::memcpy(
            memory_.data() + descriptorOffset,
            descriptors.data(),
            descriptors.size_bytes());
    }
    /* Writes available ring entries, the first of them at `first`. */
    void writeHeads(
        std::span<const std::uint16_t> heads,
        std::size_t first = 0) const
    {
        std::memcpy(
            memory_.data() + availableOffset + protocol::splitRingHeaderSize +
                first * sizeof(std::uint16_t),
            heads.data(),
            heads.size_bytes());
    }
    void setAvailableIndex(std::uint16_t index) const
    {
        std::atomic_ref<std::uint16_t>(word(availableOffset))
            .store(index, std::memory_order_release);
    }
    /* Writes the descriptor table and makes `heads` available. */
    void publish(
        std::span<const protocol::VirtqueueDescriptor> descriptors,
        std::span<const std::uint16_t> heads) const
    {
        writeDescriptors(descriptors);
        writeHeads(heads);
        setAvailableIndex(static_cast<std::uint16_t>(heads.size()));
    }

    [[nodiscard]] std::uint16_t usedIndex() const
    {
        return std::atomic_ref<std::uint16_t>(word(usedOffset))
            .load(std::memory_order_acquire);
    }
    [[nodiscard]] protocol::VirtqueueUsedElement usedElement(
        std::size_t index) const
    {
        protocol::VirtqueueUsedElement element {};
        std::memcpy(
            &element,
            memory_.data() + usedOffset + protocol::splitRingHeaderSize +
                index * sizeof(element),
            sizeof(element));
        return element;
    }

    [[nodiscard]] static protocol::VringAddress address(
        std::uint32_t queueIndex) noexcept
    {
        return {
            .index = queueIndex,
            .flags = 0,
            .descriptor = userBase + descriptorOffset,
            .used = userBase + usedOffset,
            .available = userBase + availableOffset,
            .log = 0,
        };
    }

private:
    /* A ring's index word, which follows its flags word. */
    [[nodiscard]] std::uint16_t& word(std::size_t ringOffset) const
    {
        return *reinterpret_cast<std::uint16_t*>(
            memory_.data() + ringOffset + sizeof(std::uint16_t));
    }

    ScratchBacking backing_;
    std::span<std::uint8_t> memory_;
};

/* Sizes queue `index` to eight entries, places it in `ring` and, unless told
 * otherwise, enables it. */
inline void configureQueue(
    int socket,
    const SplitRing& ring,
    std::uint32_t index,
    bool enable = true)
{
    const protocol::VringState queueSize {.index = index, .value = 8};
    sendRequestAndReadAcknowledgement(
        socket, protocol::VhostRequest::setVringNum, bytesOf(queueSize));
    const auto addresses = ring.address(index);
    sendRequestAndReadAcknowledgement(
        socket, protocol::VhostRequest::setVringAddr, bytesOf(addresses));
    if (!enable)
        return;
    const protocol::VringState enabled {.index = index, .value = 1};
    sendRequestAndReadAcknowledgement(
        socket, protocol::VhostRequest::setVringEnable, bytesOf(enabled));
}

/* A started queue's notification pipes: the server writes `call` and reads
 * `kick`. */
struct QueueChannels {
    Pipe call;
    Pipe kick;
};

/* Configures and enables queue `index`, then hands the server its call and
 * kick pipe ends. */
[[nodiscard]] inline QueueChannels startQueue(
    int socket,
    const SplitRing& ring,
    std::uint32_t index = 0)
{
    configureQueue(socket, ring, index);
    const std::uint64_t queue = index;
    QueueChannels channels {.call = makePipe(), .kick = {}};
    const std::array callDescriptors {channels.call.writeEnd.get()};
    sendRequestAndReadAcknowledgement(
        socket,
        protocol::VhostRequest::setVringCall,
        bytesOf(queue),
        callDescriptors);
    channels.kick = makePipe();
    const std::array kickDescriptors {channels.kick.readEnd.get()};
    sendRequestAndReadAcknowledgement(
        socket,
        protocol::VhostRequest::setVringKick,
        bytesOf(queue),
        kickDescriptors);
    return channels;
}

/* Requires one call notification on `callRead` within two seconds. */
inline void awaitCall(int callRead)
{
    pollfd completion {.fd = callRead, .events = POLLIN};
    require(::poll(&completion, 1, 2000) == 1);
    std::uint64_t completionCount {};
    readExact(callRead, &completionCount, sizeof(completionCount));
    require(completionCount == 1);
}

template<typename Operation>
void withServerConnection(VhostUserServer& server, Operation operation)
{
    auto sockets = makeSocketPair();
    std::atomic_bool stopRequested {};
    std::thread worker {
        [&server,
         &stopRequested,
         connection = sockets.descriptors[1].release()] {
            server.runConnected(connection, stopRequested);
        }};
    try {
        operation(sockets.descriptors[0].get());
    } catch (...) {
        (void)::shutdown(sockets.descriptors[0].get(), SHUT_RDWR);
        sockets.descriptors[0].reset();
        worker.join();
        throw;
    }
    (void)::shutdown(sockets.descriptors[0].get(), SHUT_RDWR);
    sockets.descriptors[0].reset();
    worker.join();
}

/* Requires the server to close `socket` within two seconds. */
inline void waitForConnectionClose(int socket)
{
    pollfd descriptor {.fd = socket, .events = POLLIN | POLLHUP};
    require(::poll(&descriptor, 1, 2000) == 1);
    std::uint8_t byte {};
    require(::read(socket, &byte, sizeof(byte)) == 0);
}

[[nodiscard]] inline int descriptorFromName(const std::string& name)
{
    require(name.starts_with("fd="));
    return std::stoi(name.substr(3));
}

/* Sends a SET_FEATURES frame cut short, carrying `descriptors`: half of its
 * header, or with `partialPayload` its header and half of its payload. */
inline void writePartialFrame(
    int socket,
    bool partialPayload,
    std::span<const int> descriptors = {})
{
    const protocol::VhostHeader header {
        .request = static_cast<std::uint32_t>(
            protocol::VhostRequest::setFeatures),
        .flags = protocol::vhostUserVersion,
        .size = partialPayload ?
            static_cast<std::uint32_t>(sizeof(std::uint64_t)) : 0U,
    };
    if (!partialPayload) {
        sendBytesWithDescriptors(
            socket, &header, sizeof(header) / 2U, descriptors);
        return;
    }
    std::array<std::uint8_t, sizeof(header) + sizeof(std::uint32_t)>
        fragment {};
    std::memcpy(fragment.data(), &header, sizeof(header));
    sendBytesWithDescriptors(
        socket, fragment.data(), fragment.size(), descriptors);
}

/* A vhost-user device for transport tests: no features, no configuration and
 * a no-op reset. With `signalsReadiness`, eventDescriptor() is a pipe that
 * signalReadiness() makes readable and drainEventSource() empties. Each
 * fixture supplies processQueue and overrides what it observes. */
class FixtureDevice : public uml::virtio::VhostUserDevice {
public:
    FixtureDevice(
        const char* name,
        std::size_t queueCount,
        bool signalsReadiness = false)
        : name_(name), queueCount_(queueCount)
    {
        if (signalsReadiness)
            readiness_ = makePipe();
    }

    [[nodiscard]] const char* name() const noexcept override { return name_; }
    [[nodiscard]] std::uint64_t features() const noexcept override { return 0; }
    [[nodiscard]] std::size_t queueCount() const noexcept override
    {
        return queueCount_;
    }
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
    void reset() noexcept override {}
    [[nodiscard]] int eventDescriptor() const noexcept override
    {
        return readiness_.readEnd.get();
    }
    void drainEventSource() override
    {
        std::uint8_t ready {};
        require(
            ::read(readiness_.readEnd.get(), &ready, sizeof(ready)) == 1);
    }

protected:
    void signalReadiness() const
    {
        constexpr std::uint8_t ready = 1;
        require(
            ::write(readiness_.writeEnd.get(), &ready, sizeof(ready)) == 1);
    }

private:
    const char* name_;
    std::size_t queueCount_;
    Pipe readiness_;
};

/* Runs `executable` again with `mode` as its only argument. */
[[nodiscard]] inline pid_t launchChild(const char* executable, const char* mode)
{
    const pid_t child = ::fork();
    require(child >= 0);
    if (child == 0) {
        ::execl(executable, executable, mode, nullptr);
        std::_Exit(127);
    }
    return child;
}

/* Returns the child's wait status. A child still running after `timeout` is
 * killed and fails the test. */
[[nodiscard]] inline int waitForChild(
    pid_t child,
    std::chrono::steady_clock::duration timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    for (;;) {
        const auto result = ::waitpid(child, &status, WNOHANG);
        if (result < 0 && errno == EINTR)
            continue;
        require(result >= 0);
        if (result == child)
            return status;
        if (std::chrono::steady_clock::now() >= deadline) {
            require(::kill(child, SIGKILL) == 0);
            require(::waitpid(child, &status, 0) == child);
            ::uml::testing::fail("the child did not exit in time");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
    }
}

} // namespace uml::gpu::testing
