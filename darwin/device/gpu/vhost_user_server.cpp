// SPDX-License-Identifier: GPL-2.0-or-later

#include "vhost_user_server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <type_traits>
#include <unistd.h>

namespace uml::gpu {
namespace {

constexpr std::size_t maximumMessagePayload = 1024U * 1024U;
constexpr std::size_t maximumReceivedDescriptors = 8;
constexpr std::uint32_t maximumGuestMemoryRegions = 64;
constexpr int socketPollTimeoutMilliseconds = 100;
constexpr std::uint64_t offeredFeatures =
    protocol::vhostFeatureProtocol |
    protocol::virtioFeatureVersion1 |
    protocol::virtioGpuFeatureVirgl |
    protocol::virtioGpuFeatureContextInit;
constexpr std::uint64_t offeredProtocolFeatures =
    protocol::protocolFeatureReplyAck |
    protocol::protocolFeatureSlaveRequest |
    protocol::protocolFeatureConfig |
    protocol::protocolFeatureInbandNotifications;

void closeDescriptor(int& descriptor)
{
    if (descriptor >= 0) {
        ::close(descriptor);
        descriptor = -1;
    }
}

bool readExact(int descriptor, void* destination, std::size_t length)
{
    auto* bytes = static_cast<std::uint8_t*>(destination);
    while (length != 0) {
        const auto count = ::read(descriptor, bytes, length);
        if (count == 0)
            return false;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(
                "vhost-user read failed: " + std::string(std::strerror(errno)));
        }
        bytes += count;
        length -= static_cast<std::size_t>(count);
    }
    return true;
}

void writeExact(int descriptor, const void* source, std::size_t length)
{
    const auto* bytes = static_cast<const std::uint8_t*>(source);
    while (length != 0) {
        const auto count = ::write(descriptor, bytes, length);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(
                "vhost-user write failed: " + std::string(std::strerror(errno)));
        }
        bytes += count;
        length -= static_cast<std::size_t>(count);
    }
}

template<typename Payload>
void writeEmulatorMessage(
    int socket,
    const protocol::VhostHeader& header,
    const Payload& payload)
{
    static_assert(std::is_trivially_copyable_v<Payload>);
    std::array<std::uint8_t, sizeof(header) + sizeof(payload)> message {};
    std::memcpy(message.data(), &header, sizeof(header));
    std::memcpy(
        message.data() + sizeof(header), &payload, sizeof(payload));
    /*
     * Keep the complete frame in one small write.  In the same-process
     * transport, writing only the header can synchronously deliver SIGIO and
     * enter the guest receiver on this thread.  That receiver cannot wait for
     * a payload the interrupted thread has not written yet.
     */
    writeExact(socket, message.data(), message.size());
}

template<typename Value>
Value loadUnaligned(const void* address)
{
    Value value {};
    std::memcpy(&value, address, sizeof(value));
    return value;
}

template<typename Value>
void storeUnaligned(void* address, Value value)
{
    std::memcpy(address, &value, sizeof(value));
}

template<typename Value>
Value payloadAs(const std::vector<std::uint8_t>& payload)
{
    if (payload.size() < sizeof(Value))
        throw std::runtime_error("short vhost-user payload");
    return loadUnaligned<Value>(payload.data());
}

} // namespace

VhostUserServer::VhostUserServer(
    std::filesystem::path socketPath,
    std::string vncBindAddress,
    std::uint16_t vncPort,
    std::string vncPassword,
    std::uint32_t width,
    std::uint32_t height,
    bool selfTestPattern,
    VncInputSink inputSink)
    : socketPath_(std::move(socketPath))
    , vncBindAddress_(std::move(vncBindAddress))
    , vncPort_(vncPort)
    , display_(
          vncBindAddress_,
          vncPort,
          std::move(vncPassword),
          width,
          height,
          std::move(inputSink))
    , gpu_(
          display_,
          [this](std::uint64_t address, std::span<std::uint8_t> destination) {
              return readGuest(address, destination);
          },
          [this](std::uint64_t address, std::size_t length) {
              auto* pointer = translate(address, length, false);
              return pointer == nullptr ?
                  std::span<std::uint8_t> {} :
                  std::span(pointer, length);
          })
    , traceGpuCommands_(std::getenv("UML_GPU_TRACE") != nullptr)
{
    vncPort_ = display_.port();
    if (selfTestPattern)
        display_.fillSelfTestPattern();
}

VhostUserServer::~VhostUserServer()
{
    closeDescriptor(listenSocket_);
    resetConnection();
    if (!socketPath_.empty())
        ::unlink(socketPath_.c_str());
}

void VhostUserServer::resetConnection()
{
    closeDescriptor(slaveRequestDescriptor_);
    for (auto& mapping : mappings_) {
        if (mapping.address != nullptr && mapping.size != 0)
            ::munmap(mapping.address, static_cast<std::size_t>(mapping.size));
        closeDescriptor(mapping.descriptor);
    }
    mappings_.clear();
    for (auto& queue : queues_)
        closeDescriptor(queue.callDescriptor);
    queues_ = {};
    negotiatedFeatures_ = 0;
    negotiatedProtocolFeatures_ = 0;
}

bool VhostUserServer::receiveMessage(int socket, ReceivedMessage& message)
{
    message = {};
    std::array<
        std::uint8_t,
        CMSG_SPACE(sizeof(int) * maximumReceivedDescriptors)> control {};
    iovec vector {
        .iov_base = &message.header,
        .iov_len = sizeof(message.header),
    };
    msghdr header {
        .msg_name = nullptr,
        .msg_namelen = 0,
        .msg_iov = &vector,
        .msg_iovlen = 1,
        .msg_control = control.data(),
        .msg_controllen = control.size(),
        .msg_flags = 0,
    };

    ssize_t count;
    do {
        count = ::recvmsg(socket, &header, MSG_WAITALL);
    } while (count < 0 && errno == EINTR);
    if (count == 0)
        return false;
    if (count < 0)
        throw std::runtime_error(
            "vhost-user recvmsg failed: " + std::string(std::strerror(errno)));
    if (static_cast<std::size_t>(count) < sizeof(message.header) &&
        !readExact(
            socket,
            reinterpret_cast<std::uint8_t*>(&message.header) + count,
            sizeof(message.header) - static_cast<std::size_t>(count))) {
        return false;
    }
    if ((message.header.flags & protocol::vhostUserVersionMask) !=
        protocol::vhostUserVersion)
        throw std::runtime_error("unsupported vhost-user protocol version");
    if (message.header.size > maximumMessagePayload)
        throw std::runtime_error("oversized vhost-user message");

    for (cmsghdr* controlMessage = CMSG_FIRSTHDR(&header);
         controlMessage != nullptr;
         controlMessage = CMSG_NXTHDR(&header, controlMessage)) {
        if (controlMessage->cmsg_level != SOL_SOCKET ||
            controlMessage->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        const auto descriptorCount =
            (controlMessage->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const auto* descriptors =
            reinterpret_cast<const int*>(CMSG_DATA(controlMessage));
        for (std::size_t index = 0; index < descriptorCount; ++index) {
            ::fcntl(descriptors[index], F_SETFD, FD_CLOEXEC);
            message.descriptors.push_back(descriptors[index]);
        }
    }

    message.payload.resize(message.header.size);
    if (!readExact(socket, message.payload.data(), message.payload.size()))
        return false;
    return true;
}

void VhostUserServer::sendReply(
    int socket,
    const protocol::VhostHeader& request,
    std::span<const std::uint8_t> payload)
{
    protocol::VhostHeader header {
        .request = request.request,
        .flags = protocol::vhostUserVersion | protocol::vhostUserReply,
        .size = static_cast<std::uint32_t>(payload.size()),
    };
    writeExact(socket, &header, sizeof(header));
    writeExact(socket, payload.data(), payload.size());
}

void VhostUserServer::sendIntegerReply(
    int socket,
    const protocol::VhostHeader& request,
    std::uint64_t value)
{
    sendReply(
        socket,
        request,
        std::span(
            reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)));
}

void VhostUserServer::sendAcknowledgement(
    int socket,
    const protocol::VhostHeader& request,
    std::uint64_t status)
{
    sendIntegerReply(socket, request, status);
}

void VhostUserServer::configureMemory(ReceivedMessage& message)
{
    if (message.payload.size() < sizeof(protocol::MemoryTableHeader))
        throw std::runtime_error("short SET_MEM_TABLE payload");
    const auto table = loadUnaligned<protocol::MemoryTableHeader>(
        message.payload.data());
    if (table.regionCount == 0 ||
        table.regionCount > maximumGuestMemoryRegions ||
        message.payload.size() <
            sizeof(table) + static_cast<std::size_t>(table.regionCount) *
                sizeof(protocol::MemoryRegion) ||
        message.descriptors.size() != table.regionCount) {
        throw std::runtime_error("invalid SET_MEM_TABLE region list");
    }

    for (auto& mapping : mappings_) {
        if (mapping.address != nullptr && mapping.size != 0)
            ::munmap(mapping.address, static_cast<std::size_t>(mapping.size));
        closeDescriptor(mapping.descriptor);
    }
    mappings_.clear();
    queues_ = {};
    mappings_.reserve(table.regionCount);
    for (std::uint32_t index = 0; index < table.regionCount; ++index) {
        const auto region = loadUnaligned<protocol::MemoryRegion>(
            message.payload.data() + sizeof(table) +
            static_cast<std::size_t>(index) *
                sizeof(protocol::MemoryRegion));
        if (region.size == 0 ||
            region.size > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("invalid guest-memory region size");
        }
        const int descriptor = message.descriptors[index];
        void* mapped = ::mmap(
            nullptr,
            static_cast<std::size_t>(region.size),
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            descriptor,
            static_cast<off_t>(region.mmapOffset));
        if (mapped == MAP_FAILED)
            throw std::runtime_error(
                "guest-memory mmap failed: " +
                std::string(std::strerror(errno)));
        message.descriptors[index] = -1;
        mappings_.push_back({
            .guestAddress = region.guestAddress,
            .userAddress = region.userAddress,
            .size = region.size,
            .mmapOffset = region.mmapOffset,
            .descriptor = descriptor,
            .address = static_cast<std::uint8_t*>(mapped),
        });
    }
}

std::uint8_t* VhostUserServer::translate(
    std::uint64_t address,
    std::size_t length,
    bool userAddress)
{
    for (auto& mapping : mappings_) {
        const auto base =
            userAddress ? mapping.userAddress : mapping.guestAddress;
        if (address < base)
            continue;
        const std::uint64_t offset = address - base;
        if (offset <= mapping.size &&
            length <= mapping.size - offset) {
            return mapping.address + offset;
        }
    }
    return nullptr;
}

bool VhostUserServer::readGuest(
    std::uint64_t address,
    std::span<std::uint8_t> destination)
{
    auto* source = translate(address, destination.size(), false);
    if (source == nullptr)
        return false;
    std::memcpy(destination.data(), source, destination.size());
    return true;
}

std::vector<VhostUserServer::DescriptorSegment>
VhostUserServer::descriptorChain(const Queue& queue, std::uint16_t head)
{
    if (head >= queue.size)
        throw std::runtime_error("virtqueue head is outside the ring");
    auto* descriptors = translate(
        queue.descriptorAddress,
        static_cast<std::size_t>(queue.size) *
            sizeof(protocol::VirtqueueDescriptor),
        true);
    if (descriptors == nullptr)
        throw std::runtime_error("virtqueue descriptor table is unmapped");

    std::vector<DescriptorSegment> segments;
    std::uint16_t index = head;
    for (std::uint32_t visited = 0; visited < queue.size; ++visited) {
        const auto descriptor =
            loadUnaligned<protocol::VirtqueueDescriptor>(
                descriptors +
                static_cast<std::size_t>(index) *
                    sizeof(protocol::VirtqueueDescriptor));
        if ((descriptor.flags & protocol::descriptorIndirect) != 0)
            throw std::runtime_error("indirect descriptors were not negotiated");
        if (translate(descriptor.address, descriptor.length, false) == nullptr)
            throw std::runtime_error("virtqueue buffer is outside guest memory");
        segments.push_back({
            .address = descriptor.address,
            .length = descriptor.length,
            .writable =
                (descriptor.flags & protocol::descriptorWrite) != 0,
        });
        if ((descriptor.flags & protocol::descriptorNext) == 0)
            return segments;
        index = descriptor.next;
        if (index >= queue.size)
            throw std::runtime_error("virtqueue next index is outside the ring");
    }
    throw std::runtime_error("virtqueue descriptor cycle");
}

void VhostUserServer::signalQueue(std::uint32_t index)
{
    if (index >= queues_.size())
        throw std::runtime_error("invalid Virtio-GPU completion queue");
    if (queues_[index].callDescriptor >= 0) {
        constexpr std::uint64_t completionCount = 1;
        writeExact(
            queues_[index].callDescriptor,
            &completionCount,
            sizeof(completionCount));
        return;
    }
    if (slaveRequestDescriptor_ < 0)
        throw std::runtime_error("no vhost-user slave request descriptor");
    protocol::VhostHeader header {
        .request =
            static_cast<std::uint32_t>(protocol::VhostSlaveRequest::vringCall),
        .flags = protocol::vhostUserVersion,
        .size = sizeof(protocol::VringState),
    };
    protocol::VringState state {
        .index = index,
        .value = 0,
    };
    writeEmulatorMessage(slaveRequestDescriptor_, header, state);
}

void VhostUserServer::processQueue(std::uint32_t index)
{
    if (index >= queues_.size())
        throw std::runtime_error("invalid virtqueue index");
    auto& queue = queues_[index];
    if (!queue.enabled || queue.size == 0)
        return;

    auto* available = translate(
        queue.availableAddress,
        protocol::splitRingHeaderSize +
            static_cast<std::size_t>(queue.size) * sizeof(std::uint16_t),
        true);
    auto* used = translate(
        queue.usedAddress,
        protocol::splitRingHeaderSize +
            static_cast<std::size_t>(queue.size) *
            sizeof(protocol::VirtqueueUsedElement),
        true);
    if (available == nullptr || used == nullptr)
        throw std::runtime_error("virtqueue rings are outside guest memory");

    std::atomic_thread_fence(std::memory_order_acquire);
    const auto availableIndex =
        loadUnaligned<std::uint16_t>(available + sizeof(std::uint16_t));
    bool processed = false;
    while (queue.lastAvailable != availableIndex) {
        const auto ringOffset =
            protocol::splitRingHeaderSize +
            static_cast<std::size_t>(queue.lastAvailable % queue.size) *
                sizeof(std::uint16_t);
        const auto head =
            loadUnaligned<std::uint16_t>(available + ringOffset);
        const auto segments = descriptorChain(queue, head);

        std::size_t requestSize = 0;
        for (const auto& segment : segments) {
            if (!segment.writable)
                requestSize += segment.length;
        }
        if (requestSize > maximumMessagePayload)
            throw std::runtime_error("oversized Virtio-GPU request");
        std::vector<std::uint8_t> request(requestSize);
        std::size_t requestOffset = 0;
        for (const auto& segment : segments) {
            if (segment.writable)
                continue;
            auto* source = translate(segment.address, segment.length, false);
            std::memcpy(
                request.data() + requestOffset, source, segment.length);
            requestOffset += segment.length;
        }

        const auto response = gpu_.process(
            request, index == protocol::gpuCursorQueue);
        if (traceGpuCommands_ &&
            request.size() >= sizeof(protocol::GpuControlHeader) &&
            response.size() >= sizeof(protocol::GpuControlHeader)) {
            const auto requestHeader =
                loadUnaligned<protocol::GpuControlHeader>(request.data());
            const auto responseHeader =
                loadUnaligned<protocol::GpuControlHeader>(response.data());
            std::cerr << "Virtio-GPU queue " << index
                      << " command=0x" << std::hex << requestHeader.type
                      << " response=0x" << responseHeader.type
                      << std::dec << " bytes=" << response.size()
                      << std::endl;
        }
        std::size_t responseOffset = 0;
        for (const auto& segment : segments) {
            if (!segment.writable || responseOffset == response.size())
                continue;
            const auto count = std::min<std::size_t>(
                segment.length, response.size() - responseOffset);
            auto* destination = translate(segment.address, count, false);
            std::memcpy(
                destination, response.data() + responseOffset, count);
            responseOffset += count;
        }
        if (responseOffset != response.size())
            throw std::runtime_error("short Virtio-GPU response buffer");

        const auto usedIndex =
            loadUnaligned<std::uint16_t>(used + sizeof(std::uint16_t));
        protocol::VirtqueueUsedElement element {
            .id = head,
            .length = static_cast<std::uint32_t>(responseOffset),
        };
        const auto usedOffset =
            protocol::splitRingHeaderSize +
            static_cast<std::size_t>(usedIndex % queue.size) *
                sizeof(element);
        storeUnaligned(used + usedOffset, element);
        std::atomic_thread_fence(std::memory_order_release);
        storeUnaligned<std::uint16_t>(
            used + sizeof(std::uint16_t),
            static_cast<std::uint16_t>(usedIndex + 1U));
        ++queue.lastAvailable;
        processed = true;
    }
    if (processed)
        signalQueue(index);
}

void VhostUserServer::handleMessage(
    int socket,
    ReceivedMessage& message)
{
    const auto request =
        static_cast<protocol::VhostRequest>(message.header.request);
    bool sentReply = false;
    switch (request) {
    case protocol::VhostRequest::getFeatures:
        sendIntegerReply(socket, message.header, offeredFeatures);
        sentReply = true;
        break;
    case protocol::VhostRequest::setFeatures:
        negotiatedFeatures_ = payloadAs<std::uint64_t>(message.payload);
        if ((negotiatedFeatures_ & ~offeredFeatures) != 0)
            throw std::runtime_error("guest negotiated unknown features");
        break;
    case protocol::VhostRequest::getProtocolFeatures:
        sendIntegerReply(socket, message.header, offeredProtocolFeatures);
        sentReply = true;
        break;
    case protocol::VhostRequest::setProtocolFeatures:
        negotiatedProtocolFeatures_ =
            payloadAs<std::uint64_t>(message.payload);
        if ((negotiatedProtocolFeatures_ & ~offeredProtocolFeatures) != 0)
            throw std::runtime_error("guest negotiated unknown protocol features");
        break;
    case protocol::VhostRequest::setOwner:
    case protocol::VhostRequest::resetOwner:
        break;
    case protocol::VhostRequest::setSlaveRequestFd:
        if (message.descriptors.size() != 1)
            throw std::runtime_error("SET_SLAVE_REQ_FD has no descriptor");
        closeDescriptor(slaveRequestDescriptor_);
        slaveRequestDescriptor_ = message.descriptors[0];
        message.descriptors[0] = -1;
        break;
    case protocol::VhostRequest::setMemTable:
        configureMemory(message);
        break;
    case protocol::VhostRequest::setVringNum: {
        const auto state =
            payloadAs<protocol::VringState>(message.payload);
        if (state.index >= queues_.size() || state.value == 0 ||
            state.value > protocol::maximumVirtqueueSize)
            throw std::runtime_error("invalid SET_VRING_NUM");
        queues_[state.index].size = state.value;
        break;
    }
    case protocol::VhostRequest::setVringBase: {
        const auto state =
            payloadAs<protocol::VringState>(message.payload);
        if (state.index >= queues_.size() ||
            state.value > std::numeric_limits<std::uint16_t>::max())
            throw std::runtime_error("invalid SET_VRING_BASE");
        queues_[state.index].lastAvailable =
            static_cast<std::uint16_t>(state.value);
        break;
    }
    case protocol::VhostRequest::getVringBase: {
        const auto state =
            payloadAs<protocol::VringState>(message.payload);
        if (state.index >= queues_.size())
            throw std::runtime_error("invalid GET_VRING_BASE");
        const protocol::VringState reply {
            .index = state.index,
            .value = queues_[state.index].lastAvailable,
        };
        sendReply(
            socket,
            message.header,
            std::span(
                reinterpret_cast<const std::uint8_t*>(&reply),
                sizeof(reply)));
        sentReply = true;
        break;
    }
    case protocol::VhostRequest::setVringAddr: {
        const auto address =
            payloadAs<protocol::VringAddress>(message.payload);
        if (address.index >= queues_.size())
            throw std::runtime_error("invalid SET_VRING_ADDR");
        auto& queue = queues_[address.index];
        queue.descriptorAddress = address.descriptor;
        queue.availableAddress = address.available;
        queue.usedAddress = address.used;
        break;
    }
    case protocol::VhostRequest::setVringEnable: {
        const auto state =
            payloadAs<protocol::VringState>(message.payload);
        if (state.index >= queues_.size())
            throw std::runtime_error("invalid SET_VRING_ENABLE");
        queues_[state.index].enabled = state.value != 0;
        break;
    }
    case protocol::VhostRequest::setVringKick:
    case protocol::VhostRequest::vringKick: {
        const auto state =
            payloadAs<protocol::VringState>(message.payload);
        processQueue(state.index);
        break;
    }
    case protocol::VhostRequest::setVringCall: {
        const auto encodedQueue =
            payloadAs<std::uint64_t>(message.payload);
        const auto index = static_cast<std::size_t>(
            encodedQueue & protocol::vhostVringIndexMask);
        const bool noDescriptor =
            (encodedQueue & protocol::vhostVringNoDescriptor) != 0;
        if (index >= queues_.size() ||
            (noDescriptor && !message.descriptors.empty()) ||
            (!noDescriptor && message.descriptors.size() != 1)) {
            throw std::runtime_error("invalid SET_VRING_CALL");
        }
        closeDescriptor(queues_[index].callDescriptor);
        if (!noDescriptor) {
            queues_[index].callDescriptor = message.descriptors[0];
            message.descriptors[0] = -1;
        }
        break;
    }
    case protocol::VhostRequest::getConfig: {
        const auto configRequest =
            payloadAs<protocol::ConfigHeader>(message.payload);
        const auto configuration = gpu_.config();
        if (configRequest.offset > sizeof(configuration) ||
            configRequest.size > sizeof(configuration) - configRequest.offset) {
            throw std::runtime_error("invalid Virtio-GPU config range");
        }
        std::vector<std::uint8_t> payload(
            sizeof(protocol::ConfigHeader) + configRequest.size);
        auto responseHeader = configRequest;
        responseHeader.flags = 0;
        std::memcpy(payload.data(), &responseHeader, sizeof(responseHeader));
        std::memcpy(
            payload.data() + sizeof(responseHeader),
            reinterpret_cast<const std::uint8_t*>(&configuration) +
                configRequest.offset,
            configRequest.size);
        sendReply(socket, message.header, payload);
        sentReply = true;
        break;
    }
    case protocol::VhostRequest::setConfig:
        // events_clear is write-only. No asynchronous display events exist yet.
        break;
    default:
        throw std::runtime_error(
            "unsupported vhost-user request " +
            std::to_string(message.header.request));
    }

    if (!sentReply &&
        (message.header.flags & protocol::vhostUserNeedReply) != 0) {
        sendAcknowledgement(socket, message.header, 0);
    }
    for (auto& descriptor : message.descriptors)
        closeDescriptor(descriptor);
}

void VhostUserServer::serveConnection(
    int connection,
    const std::atomic_bool& stopRequested)
{
    try {
        while (!stopRequested.load(std::memory_order_acquire)) {
            pollfd peer {
                .fd = connection,
                .events = POLLIN,
                .revents = 0,
            };
            const int peerPoll =
                ::poll(&peer, 1, socketPollTimeoutMilliseconds);
            if (peerPoll < 0) {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error("vhost-user peer poll failed");
            }
            if (peerPoll == 0)
                continue;
            ReceivedMessage message;
            if (!receiveMessage(connection, message))
                break;
            handleMessage(connection, message);
        }
    } catch (const std::exception& error) {
        std::cerr << "vhost-user connection failed: " << error.what()
                  << std::endl;
    }
    ::close(connection);
    resetConnection();
}

void VhostUserServer::runConnected(
    int connection,
    const std::atomic_bool& stopRequested,
    const std::function<void()>& ready)
{
    if (connection < 0)
        throw std::invalid_argument("invalid in-process vhost-user socket");
    int noSigpipe = 1;
    ::fcntl(connection, F_SETFD, FD_CLOEXEC);
    ::setsockopt(
        connection, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe));
    std::cout << "Virtio-GPU vhost-user transport: in-process socketpair\n"
              << "VNC display: " << vncBindAddress_ << ':' << vncPort_ << " ("
              << display_.width() << 'x' << display_.height() << ')'
              << std::endl;
    if (ready)
        ready();
    serveConnection(connection, stopRequested);
}

void VhostUserServer::run(
    const std::atomic_bool& stopRequested,
    const std::function<void()>& ready)
{
    if (socketPath_.string().size() >= sizeof(sockaddr_un::sun_path))
        throw std::invalid_argument("vhost-user socket path is too long");
    ::unlink(socketPath_.c_str());
    listenSocket_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenSocket_ < 0)
        throw std::runtime_error(
            "socket failed: " + std::string(std::strerror(errno)));
    ::fcntl(listenSocket_, F_SETFD, FD_CLOEXEC);
    int noSigpipe = 1;
    ::setsockopt(
        listenSocket_, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe));

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(
        address.sun_path,
        socketPath_.c_str(),
        socketPath_.string().size() + 1U);
    if (::bind(
            listenSocket_,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0 ||
        ::listen(listenSocket_, 1) != 0) {
        throw std::runtime_error(
            "vhost-user listen failed: " +
            std::string(std::strerror(errno)));
    }
    ::chmod(socketPath_.c_str(), 0600);
    std::cout << "Virtio-GPU vhost-user socket: " << socketPath_ << '\n'
              << "VNC display: " << vncBindAddress_ << ':' << vncPort_ << " ("
              << display_.width() << 'x' << display_.height() << ')'
              << std::endl;
    if (ready)
        ready();

    while (!stopRequested.load(std::memory_order_acquire)) {
        pollfd listener {
            .fd = listenSocket_,
            .events = POLLIN,
            .revents = 0,
        };
        const int pollResult =
            ::poll(&listener, 1, socketPollTimeoutMilliseconds);
        if (pollResult < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(
                "poll failed: " + std::string(std::strerror(errno)));
        }
        if (pollResult == 0)
            continue;

        const int connection = ::accept(listenSocket_, nullptr, nullptr);
        if (connection < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(
                "accept failed: " + std::string(std::strerror(errno)));
        }
        ::fcntl(connection, F_SETFD, FD_CLOEXEC);
        ::setsockopt(
            connection, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe));
        serveConnection(connection, stopRequested);
    }
}

} // namespace uml::gpu
