// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gpu_require.hpp"
#include "protocol.hpp"
#include "scratch_backing.hpp"
#include "unique_descriptor.hpp"
#include "vhost_user_server.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace uml::gpu::testing {

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

[[nodiscard]] inline int descriptorFromName(const std::string& name)
{
    require(name.starts_with("fd="));
    return std::stoi(name.substr(3));
}

} // namespace uml::gpu::testing
