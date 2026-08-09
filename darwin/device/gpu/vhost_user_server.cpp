// SPDX-License-Identifier: GPL-2.0-or-later

#include "vhost_user_server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <type_traits>
#include <utility>
#include <unistd.h>

#ifndef UML_RUNTIME_PROFILING
#define UML_RUNTIME_PROFILING 0
#endif

namespace uml::gpu {
namespace {

constexpr std::uint32_t maximumGuestMemoryRegions =
    static_cast<std::uint32_t>(maximumVhostUserDescriptors);
#ifndef UML_GPU_STOP_POLL_TIMEOUT_MILLISECONDS
#define UML_GPU_STOP_POLL_TIMEOUT_MILLISECONDS 100
#endif
constexpr int socketPollTimeoutMilliseconds =
    UML_GPU_STOP_POLL_TIMEOUT_MILLISECONDS;
static_assert(socketPollTimeoutMilliseconds > 0);
constexpr std::uint64_t offeredFeatures =
    protocol::vhostFeatureProtocol |
    protocol::virtioFeatureVersion1 |
    protocol::virtioGpuFeatureVirgl |
    protocol::virtioGpuFeatureContextInit;
constexpr std::uint64_t offeredProtocolFeatures =
    protocol::protocolFeatureMultipleQueues |
    protocol::protocolFeatureReplyAck |
    protocol::protocolFeatureSlaveRequest |
    protocol::protocolFeatureConfig |
    protocol::protocolFeatureInbandNotifications;

#if UML_RUNTIME_PROFILING
[[nodiscard]] bool traceGpuCommandsEnabled() noexcept
{
    static const bool enabled = std::getenv("UML_GPU_TRACE") != nullptr;
    return enabled;
}
#endif

class ConnectionStopped final {};

void setCloseOnExec(int descriptor)
{
    const int flags = ::fcntl(descriptor, F_GETFD);
    if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0)
        throw std::runtime_error(
            "configuring close-on-exec descriptor failed: " +
            std::string(std::strerror(errno)));
}

void setNonblocking(int descriptor)
{
    const int flags = ::fcntl(descriptor, F_GETFL);
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::runtime_error(
            "configuring nonblocking vhost-user descriptor failed: " +
            std::string(std::strerror(errno)));
    }
}

void configureOutputDescriptor(int descriptor)
{
    setNonblocking(descriptor);
    if (::fcntl(descriptor, F_SETNOSIGPIPE, 1) != 0) {
        throw std::runtime_error(
            "configuring no-signal vhost-user descriptor failed: " +
            std::string(std::strerror(errno)));
    }
}

void flushReadinessOutput()
{
    if (std::fflush(stdout) == 0)
        return;
    const int flushError = errno;
    throw std::runtime_error(
        "VNC readiness output flush failed: " +
        std::string(std::strerror(flushError)));
}

class SocketPathCleanup final {
public:
    explicit SocketPathCleanup(std::filesystem::path path)
        : path_(std::move(path))
    {
    }

    void adopt()
    {
        struct stat status {};
        if (::lstat(path_.c_str(), &status) != 0 ||
            !S_ISSOCK(status.st_mode)) {
            throw std::runtime_error(
                "bound vhost-user socket path changed unexpectedly");
        }
        device_ = status.st_dev;
        inode_ = status.st_ino;
        owned_ = true;
    }

    ~SocketPathCleanup()
    {
        struct stat status {};
        if (owned_ && ::lstat(path_.c_str(), &status) == 0 &&
            S_ISSOCK(status.st_mode) && status.st_dev == device_ &&
            status.st_ino == inode_)
            (void)::unlink(path_.c_str());
    }

private:
    std::filesystem::path path_;
    dev_t device_ {};
    ino_t inode_ {};
    bool owned_ {};
};

void writeExact(
    int descriptor,
    const void* source,
    std::size_t length,
    const std::atomic_bool* stopRequested = nullptr)
{
    const auto* bytes = static_cast<const std::uint8_t*>(source);
    while (length != 0) {
        if (stopRequested != nullptr &&
            stopRequested->load(std::memory_order_acquire)) {
            throw ConnectionStopped {};
        }
        pollfd output {
            .fd = descriptor,
            .events = POLLOUT,
            .revents = 0,
        };
        const int pollResult = ::poll(
            &output,
            1,
            stopRequested == nullptr ? -1 : socketPollTimeoutMilliseconds);
        if (pollResult < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(
                "vhost-user write poll failed: " +
                std::string(std::strerror(errno)));
        }
        if (pollResult == 0)
            continue;
        if ((output.revents & POLLNVAL) != 0)
            throw std::runtime_error("invalid vhost-user write descriptor");
        const auto count = ::write(descriptor, bytes, length);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            throw std::runtime_error(
                "vhost-user write failed: " + std::string(std::strerror(errno)));
        }
        if (count == 0)
            throw std::runtime_error("short vhost-user write");
        bytes += count;
        length -= static_cast<std::size_t>(count);
    }
}

void writeFrame(
    int descriptor,
    const protocol::VhostHeader& header,
    std::span<const std::uint8_t> payload,
    const std::atomic_bool* stopRequested)
{
    std::array<iovec, 2> vectors {{
        {
            .iov_base = const_cast<protocol::VhostHeader*>(&header),
            .iov_len = sizeof(header),
        },
        {
            .iov_base = const_cast<std::uint8_t*>(payload.data()),
            .iov_len = payload.size(),
        },
    }};
    std::size_t current = 0;
    const std::size_t vectorCount = payload.empty() ? 1U : vectors.size();
    while (current < vectorCount) {
        if (stopRequested != nullptr &&
            stopRequested->load(std::memory_order_acquire)) {
            throw ConnectionStopped {};
        }
        pollfd output {
            .fd = descriptor,
            .events = POLLOUT,
            .revents = 0,
        };
        const int pollResult = ::poll(
            &output, 1, socketPollTimeoutMilliseconds);
        if (pollResult < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(
                "vhost-user frame poll failed: " +
                std::string(std::strerror(errno)));
        }
        if (pollResult == 0)
            continue;
        if ((output.revents & POLLNVAL) != 0)
            throw std::runtime_error("invalid vhost-user frame descriptor");

        const auto count = ::writev(
            descriptor,
            vectors.data() + current,
            static_cast<int>(vectorCount - current));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            throw std::runtime_error(
                "vhost-user frame write failed: " +
                std::string(std::strerror(errno)));
        }
        if (count == 0)
            throw std::runtime_error("short vhost-user frame write");

        auto consumed = static_cast<std::size_t>(count);
        while (current < vectorCount &&
               consumed >= vectors[current].iov_len) {
            consumed -= vectors[current].iov_len;
            ++current;
        }
        if (current < vectorCount && consumed != 0) {
            vectors[current].iov_base =
                static_cast<std::uint8_t*>(vectors[current].iov_base) +
                consumed;
            vectors[current].iov_len -= consumed;
        }
    }
}

template<typename Payload>
void writeEmulatorMessage(
    int socket,
    const protocol::VhostHeader& header,
    const Payload& payload,
    const std::atomic_bool* stopRequested)
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
    writeExact(socket, message.data(), message.size(), stopRequested);
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

std::uint16_t loadSharedRingValue(
    const std::uint8_t* address,
    std::memory_order order)
{
    static_assert(std::atomic_ref<std::uint16_t>::is_always_lock_free);
    auto* value = reinterpret_cast<std::uint16_t*>(
        const_cast<std::uint8_t*>(address));
    return std::atomic_ref<std::uint16_t>(*value).load(order);
}

void storeSharedRingValue(
    std::uint8_t* address,
    std::uint16_t value,
    std::memory_order order)
{
    static_assert(std::atomic_ref<std::uint16_t>::is_always_lock_free);
    auto* destination = reinterpret_cast<std::uint16_t*>(address);
    std::atomic_ref<std::uint16_t>(*destination).store(value, order);
}

template<typename Value>
Value payloadAs(const std::vector<std::uint8_t>& payload)
{
    if (payload.size() < sizeof(Value))
        throw std::runtime_error("short vhost-user payload");
    return loadUnaligned<Value>(payload.data());
}

void requirePayloadSize(
    const VhostUserMessage& message,
    std::size_t expected,
    const char* request)
{
    if (message.payload.size() != expected)
        throw std::runtime_error(std::string("invalid ") + request +
                                 " payload size");
}

void requireDescriptorCount(
    const VhostUserMessage& message,
    std::size_t expected,
    const char* request)
{
    if (message.descriptors.size() != expected)
        throw std::runtime_error(std::string("invalid ") + request +
                                 " descriptor count");
}

void requireNoDescriptors(
    const VhostUserMessage& message,
    const char* request)
{
    requireDescriptorCount(message, 0, request);
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
    uml::input::vnc::VncInputSink inputSink)
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
{
    vncPort_ = display_.port();
    if (selfTestPattern)
        display_.fillSelfTestPattern();
}

VhostUserServer::~VhostUserServer()
{
    resetConnection();
}

void VhostUserServer::resetConnection() noexcept
{
    gpu_.reset();
    slaveRequestDescriptor_.reset();
    clearMappings();
    queues_ = {};
    negotiatedFeatures_ = 0;
    negotiatedProtocolFeatures_ = 0;
    featuresConfigured_ = false;
}

void VhostUserServer::clearMappings() noexcept
{
    for (auto& mapping : mappings_) {
        if (mapping.address != nullptr && mapping.size != 0)
            ::munmap(mapping.address, static_cast<std::size_t>(mapping.size));
    }
    mappings_.clear();
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
    writeFrame(socket, header, payload, connectionStopRequested_);
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

void VhostUserServer::configureMemory(VhostUserMessage& message)
{
    if (message.payload.size() < sizeof(protocol::MemoryTableHeader))
        throw std::runtime_error("short SET_MEM_TABLE payload");
    const auto table = loadUnaligned<protocol::MemoryTableHeader>(
        message.payload.data());
    if (table.padding != 0 || table.regionCount == 0 ||
        table.regionCount > maximumGuestMemoryRegions ||
        message.payload.size() !=
            sizeof(table) + static_cast<std::size_t>(table.regionCount) *
                sizeof(protocol::MemoryRegion) ||
        message.descriptors.size() != table.regionCount) {
        throw std::runtime_error("invalid SET_MEM_TABLE region list");
    }

    std::vector<Mapping> replacements;
    const auto releaseReplacements = [&replacements] {
        for (auto& mapping : replacements) {
            if (mapping.address != nullptr && mapping.size != 0)
                (void)::munmap(
                    mapping.address, static_cast<std::size_t>(mapping.size));
        }
        replacements.clear();
    };
    try {
        replacements.reserve(table.regionCount);
        for (std::uint32_t index = 0; index < table.regionCount; ++index) {
            const auto region = loadUnaligned<protocol::MemoryRegion>(
                message.payload.data() + sizeof(table) +
                static_cast<std::size_t>(index) *
                    sizeof(protocol::MemoryRegion));
            if (region.size == 0 ||
                region.size > std::numeric_limits<std::size_t>::max() ||
                region.mmapOffset >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<off_t>::max()) ||
                region.mmapOffset >
                    std::numeric_limits<std::uint64_t>::max() - region.size ||
                region.guestAddress >
                    std::numeric_limits<std::uint64_t>::max() - region.size ||
                region.userAddress >
                    std::numeric_limits<std::uint64_t>::max() - region.size) {
                throw std::runtime_error("invalid guest-memory region size");
            }
            const auto overlaps = [&region](const Mapping& existing) {
                const bool guestOverlap =
                    region.guestAddress <
                        existing.guestAddress + existing.size &&
                    existing.guestAddress <
                        region.guestAddress + region.size;
                const bool userOverlap =
                    region.userAddress <
                        existing.userAddress + existing.size &&
                    existing.userAddress <
                        region.userAddress + region.size;
                return guestOverlap || userOverlap;
            };
            if (std::ranges::any_of(replacements, overlaps))
                throw std::runtime_error("overlapping guest-memory regions");
            const int descriptor = message.descriptors[index].get();
            struct stat descriptorStatus {};
            if (::fstat(descriptor, &descriptorStatus) != 0) {
                throw std::runtime_error(
                    "guest-memory descriptor inspection failed: " +
                    std::string(std::strerror(errno)));
            }
            if (S_ISREG(descriptorStatus.st_mode) &&
                (descriptorStatus.st_size < 0 ||
                 region.mmapOffset >
                     static_cast<std::uint64_t>(descriptorStatus.st_size) ||
                 region.size >
                     static_cast<std::uint64_t>(descriptorStatus.st_size) -
                         region.mmapOffset)) {
                throw std::runtime_error(
                    "guest-memory region exceeds its backing descriptor");
            }
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
            Mapping mapping {
                .guestAddress = region.guestAddress,
                .userAddress = region.userAddress,
                .size = region.size,
                .mmapOffset = region.mmapOffset,
                .address = static_cast<std::uint8_t*>(mapped),
            };
            try {
                replacements.push_back(std::move(mapping));
            } catch (...) {
                (void)::munmap(mapped, static_cast<std::size_t>(region.size));
                throw;
            }
            replacements.back().descriptor =
                std::move(message.descriptors[index]);
        }
    } catch (...) {
        releaseReplacements();
        throw;
    }

    // Renderer backing can retain pointers into the old mappings. Retire that
    // device state before unmapping, then commit the replacement without any
    // operation that can throw or lose a descriptor.
    gpu_.reset();
    clearMappings();
    queues_ = {};
    if (featuresConfigured_ &&
        (negotiatedFeatures_ & protocol::vhostFeatureProtocol) == 0) {
        for (auto& queue : queues_)
            queue.enabled = true;
    }
    mappings_ = std::move(replacements);
}

void VhostUserServer::configureKick(VhostUserMessage& message)
{
    requirePayloadSize(message, sizeof(std::uint64_t), "SET_VRING_KICK");
    const auto encodedQueue = payloadAs<std::uint64_t>(message.payload);
    const auto index = static_cast<std::size_t>(
        encodedQueue & protocol::vhostVringIndexMask);
    const bool noDescriptor =
        (encodedQueue & protocol::vhostVringNoDescriptor) != 0;
    if (index >= queues_.size() ||
        (encodedQueue & ~protocol::vhostVringDescriptorMask) != 0 ||
        (noDescriptor && !message.descriptors.empty()) ||
        (!noDescriptor && message.descriptors.size() != 1)) {
        throw std::runtime_error("invalid SET_VRING_KICK");
    }

    auto& queue = queues_[index];
    if (noDescriptor) {
        queue.kickDescriptor.reset();
        queue.polling = true;
    } else {
        setNonblocking(message.descriptors[0].get());
        queue.kickDescriptor = std::move(message.descriptors[0]);
        queue.polling = false;
    }
    queue.started = true;
}

void VhostUserServer::drainKick(std::uint32_t index)
{
    if (index >= queues_.size())
        throw std::runtime_error("invalid Virtio-GPU kick queue");
    auto& queue = queues_[index];
    std::array<std::uint8_t, 64> values {};
    bool consumed = false;
    for (;;) {
        const auto count =
            ::read(queue.kickDescriptor.get(), values.data(), values.size());
        if (count > 0) {
            consumed = true;
            continue;
        }
        if (count == 0) {
            if (consumed)
                processQueue(index);
            throw std::runtime_error("closed vhost-user kick descriptor");
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        throw std::runtime_error(
            "vhost-user kick read failed: " +
            std::string(std::strerror(errno)));
    }
    if (consumed)
        processQueue(index);
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
    bool sawWritable = false;
    for (std::uint32_t visited = 0; visited < queue.size; ++visited) {
        const auto descriptor =
            loadUnaligned<protocol::VirtqueueDescriptor>(
                descriptors +
                static_cast<std::size_t>(index) *
                    sizeof(protocol::VirtqueueDescriptor));
        constexpr std::uint16_t knownDescriptorFlags =
            protocol::descriptorNext |
            protocol::descriptorWrite |
            protocol::descriptorIndirect;
        if ((descriptor.flags & ~knownDescriptorFlags) != 0)
            throw std::runtime_error("virtqueue descriptor has reserved flags");
        if ((descriptor.flags & protocol::descriptorIndirect) != 0)
            throw std::runtime_error("indirect descriptors were not negotiated");
        const bool writable =
            (descriptor.flags & protocol::descriptorWrite) != 0;
        if (!writable && sawWritable) {
            throw std::runtime_error(
                "device-readable descriptor follows a writable descriptor");
        }
        sawWritable = sawWritable || writable;
        if (translate(descriptor.address, descriptor.length, false) == nullptr)
            throw std::runtime_error("virtqueue buffer is outside guest memory");
        segments.push_back({
            .address = descriptor.address,
            .length = descriptor.length,
            .writable = writable,
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
    if (queues_[index].callDescriptor.get() >= 0) {
        constexpr std::uint64_t completionCount = 1;
        writeExact(
            queues_[index].callDescriptor.get(),
            &completionCount,
            sizeof(completionCount),
            connectionStopRequested_);
        return;
    }
    if (queues_[index].callPolling)
        return;
    constexpr std::uint64_t requiredInbandFeatures =
        protocol::protocolFeatureInbandNotifications |
        protocol::protocolFeatureSlaveRequest |
        protocol::protocolFeatureReplyAck;
    if ((negotiatedProtocolFeatures_ & requiredInbandFeatures) !=
            requiredInbandFeatures ||
        slaveRequestDescriptor_.get() < 0) {
        throw std::runtime_error(
            "no negotiated vhost-user in-band completion channel");
    }
    protocol::VhostHeader header {
        .request =
            static_cast<std::uint32_t>(protocol::VhostSlaveRequest::vringCall),
        .flags = protocol::vhostUserVersion | protocol::vhostUserNeedReply,
        .size = sizeof(protocol::VringState),
    };
    protocol::VringState state {
        .index = index,
        .value = 0,
    };
    writeEmulatorMessage(
        slaveRequestDescriptor_.get(),
        header,
        state,
        connectionStopRequested_);

    VhostUserMessage reply;
    if (!receiveVhostUserMessage(
            slaveRequestDescriptor_.get(),
            reply,
            connectionStopRequested_)) {
        if (connectionStopRequested_ != nullptr &&
            connectionStopRequested_->load(std::memory_order_acquire)) {
            throw ConnectionStopped {};
        }
        throw std::runtime_error(
            "closed vhost-user in-band completion channel");
    }
    constexpr std::uint32_t expectedReplyFlags =
        protocol::vhostUserVersion | protocol::vhostUserReply;
    if (reply.header.request != header.request ||
        reply.header.flags != expectedReplyFlags ||
        reply.payload.size() != sizeof(std::uint64_t) ||
        !reply.descriptors.empty() ||
        payloadAs<std::uint64_t>(reply.payload) != 0) {
        throw std::runtime_error(
            "invalid vhost-user in-band completion acknowledgement");
    }
}

void VhostUserServer::processQueue(std::uint32_t index)
{
    if (index >= queues_.size())
        throw std::runtime_error("invalid virtqueue index");
    auto& queue = queues_[index];
    if (!queue.started || queue.size == 0)
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
    if (reinterpret_cast<std::uintptr_t>(available) %
                alignof(std::uint16_t) !=
            0 ||
        reinterpret_cast<std::uintptr_t>(used) % alignof(std::uint16_t) != 0) {
        /* Vhost-user addresses are opaque values from the front-end. Their
         * wire alignment does not guarantee that subtracting a skewed memory
         * region base produces an aligned host pointer. Shared ring indices
         * are accessed atomically, so reject such a translation before use. */
        throw std::runtime_error("virtqueue rings translate to misaligned memory");
    }

    const auto initialAvailableFlags =
        loadSharedRingValue(available, std::memory_order_relaxed);
    if ((initialAvailableFlags & ~protocol::availableSuppressInterrupt) != 0)
        throw std::runtime_error("virtqueue available ring has reserved flags");
    const auto availableIndex =
        loadSharedRingValue(
            available + sizeof(std::uint16_t),
            std::memory_order_acquire);
    const auto pending = static_cast<std::uint16_t>(
        availableIndex - queue.lastAvailable);
    if (pending > queue.size)
        throw std::runtime_error("virtqueue producer advanced beyond ring size");
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
        std::size_t responseCapacity = 0;
        for (const auto& segment : segments) {
            if (segment.writable)
                responseCapacity += segment.length;
            else
                requestSize += segment.length;
        }
        if (requestSize > maximumVhostUserMessagePayload)
            throw std::runtime_error("oversized Virtio-GPU request");
        const bool cursorQueue = index == protocol::gpuCursorQueue;
        if (queue.enabled && !cursorQueue &&
            responseCapacity < sizeof(protocol::GpuControlHeader)) {
            throw std::runtime_error("short Virtio-GPU response buffer");
        }

        std::vector<std::uint8_t> request;
        std::vector<std::uint8_t> response;
        if (queue.enabled) {
            request.resize(requestSize);
            std::size_t requestOffset = 0;
            for (const auto& segment : segments) {
                if (segment.writable || segment.length == 0)
                    continue;
                auto* source = translate(
                    segment.address, segment.length, false);
                std::memcpy(
                    request.data() + requestOffset, source, segment.length);
                requestOffset += segment.length;
            }

            response = gpu_.process(request, cursorQueue);
            // Validate the complete response before touching guest-writable
            // buffers. This prevents a query from leaving a partial response,
            // and the fixed-header preflight above prevents state-changing
            // control commands from running without room for their reply.
            if (response.size() > responseCapacity)
                throw std::runtime_error("short Virtio-GPU response buffer");
        }
#if UML_RUNTIME_PROFILING
        if (traceGpuCommandsEnabled() &&
            request.size() >= sizeof(protocol::GpuControlHeader) &&
            response.size() >= sizeof(protocol::GpuControlHeader)) {
            const auto requestHeader =
                loadUnaligned<protocol::GpuControlHeader>(request.data());
            const auto responseHeader =
                loadUnaligned<protocol::GpuControlHeader>(response.data());
            std::fprintf(
                stderr,
                "Virtio-GPU queue %u command=0x%x response=0x%x bytes=%zu\n",
                index,
                requestHeader.type,
                responseHeader.type,
                response.size());
        }
#endif
        std::size_t responseOffset = 0;
        for (const auto& segment : segments) {
            if (!segment.writable || responseOffset == response.size())
                continue;
            const auto count = std::min<std::size_t>(
                segment.length, response.size() - responseOffset);
            if (count == 0)
                continue;
            auto* destination = translate(segment.address, count, false);
            std::memcpy(
                destination, response.data() + responseOffset, count);
            responseOffset += count;
        }
        if (responseOffset != response.size())
            throw std::runtime_error("incomplete Virtio-GPU response write");

        const auto usedIndex =
            loadSharedRingValue(
                used + sizeof(std::uint16_t),
                std::memory_order_relaxed);
        protocol::VirtqueueUsedElement element {
            .id = head,
            .length = static_cast<std::uint32_t>(responseOffset),
        };
        const auto usedOffset =
            protocol::splitRingHeaderSize +
            static_cast<std::size_t>(usedIndex % queue.size) *
                sizeof(element);
        storeUnaligned(used + usedOffset, element);
        storeSharedRingValue(
            used + sizeof(std::uint16_t),
            static_cast<std::uint16_t>(usedIndex + 1U),
            std::memory_order_release);
        ++queue.lastAvailable;
        processed = true;
    }
    if (processed) {
        // Notification suppression is sampled after publishing used entries.
        // Sampling it before command execution can miss a mandatory interrupt
        // when the driver re-enables notifications while the queue is active.
        const auto availableFlags =
            loadSharedRingValue(available, std::memory_order_acquire);
        if ((availableFlags & ~protocol::availableSuppressInterrupt) != 0) {
            throw std::runtime_error(
                "virtqueue available ring has reserved flags");
        }
        if ((availableFlags & protocol::availableSuppressInterrupt) == 0)
            signalQueue(index);
    }
}

void VhostUserServer::handleMessage(
    int socket,
    VhostUserMessage& message)
{
    constexpr std::uint32_t allowedRequestFlags =
        protocol::vhostUserVersionMask | protocol::vhostUserNeedReply;
    if ((message.header.flags & ~allowedRequestFlags) != 0)
        throw std::runtime_error("invalid vhost-user request flags");
    const auto request =
        static_cast<protocol::VhostRequest>(message.header.request);
    bool sentReply = false;
    switch (request) {
    case protocol::VhostRequest::getFeatures:
        requirePayloadSize(message, 0, "GET_FEATURES");
        requireNoDescriptors(message, "GET_FEATURES");
        sendIntegerReply(socket, message.header, offeredFeatures);
        sentReply = true;
        break;
    case protocol::VhostRequest::setFeatures:
        requirePayloadSize(message, sizeof(std::uint64_t), "SET_FEATURES");
        requireNoDescriptors(message, "SET_FEATURES");
        negotiatedFeatures_ = payloadAs<std::uint64_t>(message.payload);
        if ((negotiatedFeatures_ & ~offeredFeatures) != 0)
            throw std::runtime_error("guest negotiated unknown features");
        featuresConfigured_ = true;
        if ((negotiatedFeatures_ & protocol::vhostFeatureProtocol) == 0) {
            for (auto& queue : queues_)
                queue.enabled = true;
        }
        break;
    case protocol::VhostRequest::getProtocolFeatures:
        requirePayloadSize(message, 0, "GET_PROTOCOL_FEATURES");
        requireNoDescriptors(message, "GET_PROTOCOL_FEATURES");
        sendIntegerReply(socket, message.header, offeredProtocolFeatures);
        sentReply = true;
        break;
    case protocol::VhostRequest::setProtocolFeatures: {
        requirePayloadSize(
            message, sizeof(std::uint64_t), "SET_PROTOCOL_FEATURES");
        requireNoDescriptors(message, "SET_PROTOCOL_FEATURES");
        const auto requestedProtocolFeatures =
            payloadAs<std::uint64_t>(message.payload);
        if ((requestedProtocolFeatures & ~offeredProtocolFeatures) != 0) {
            throw std::runtime_error(
                "guest negotiated unknown protocol features");
        }
        constexpr std::uint64_t requiredInbandFeatures =
            protocol::protocolFeatureInbandNotifications |
            protocol::protocolFeatureSlaveRequest |
            protocol::protocolFeatureReplyAck;
        if ((requestedProtocolFeatures &
             protocol::protocolFeatureInbandNotifications) != 0 &&
            (requestedProtocolFeatures & requiredInbandFeatures) !=
                requiredInbandFeatures) {
            // The protocol explicitly requires closing the connection for
            // this invalid combination because there is no earlier error
            // channel on which to report the negotiation failure.
            throw std::runtime_error(
                "in-band notifications require backend requests and reply ack");
        }
        negotiatedProtocolFeatures_ = requestedProtocolFeatures;
        break;
    }
    case protocol::VhostRequest::getQueueNum:
        requirePayloadSize(message, 0, "GET_QUEUE_NUM");
        requireNoDescriptors(message, "GET_QUEUE_NUM");
        // GET_QUEUE_NUM is itself part of protocol-feature discovery. The
        // front-end is allowed to issue it after seeing MQ in the queried
        // feature mask and before SET_PROTOCOL_FEATURES.
        sendIntegerReply(socket, message.header, queues_.size());
        sentReply = true;
        break;
    case protocol::VhostRequest::setOwner:
        requirePayloadSize(message, 0, "SET_OWNER");
        requireNoDescriptors(message, "SET_OWNER");
        break;
    case protocol::VhostRequest::resetOwner:
        requirePayloadSize(message, 0, "RESET_OWNER");
        requireNoDescriptors(message, "RESET_OWNER");
        /* RESET_OWNER is deprecated, but a front-end that still sends it is
         * explicitly relinquishing device ownership.  Keep the reusable
         * transport mappings and descriptors, while retiring every renderer
         * object that may refer to guest memory before disabling the rings.
         */
        gpu_.reset();
        for (auto& queue : queues_) {
            queue.started = false;
            queue.enabled = false;
        }
        break;
    case protocol::VhostRequest::setSlaveRequestFd:
        requirePayloadSize(message, 0, "SET_SLAVE_REQ_FD");
        requireDescriptorCount(message, 1, "SET_SLAVE_REQ_FD");
        if ((negotiatedProtocolFeatures_ &
             protocol::protocolFeatureSlaveRequest) == 0) {
            throw std::runtime_error("SET_SLAVE_REQ_FD was not negotiated");
        }
        configureOutputDescriptor(message.descriptors[0].get());
        slaveRequestDescriptor_ = std::move(message.descriptors[0]);
        break;
    case protocol::VhostRequest::setMemTable:
        configureMemory(message);
        break;
    case protocol::VhostRequest::setVringNum: {
        requirePayloadSize(message, sizeof(protocol::VringState),
                           "SET_VRING_NUM");
        requireNoDescriptors(message, "SET_VRING_NUM");
        const auto state =
            payloadAs<protocol::VringState>(message.payload);
        if (state.index >= queues_.size() || state.value == 0 ||
            state.value > protocol::maximumVirtqueueSize ||
            (state.value & (state.value - 1U)) != 0)
            throw std::runtime_error("invalid SET_VRING_NUM");
        queues_[state.index].size = state.value;
        break;
    }
    case protocol::VhostRequest::setVringBase: {
        requirePayloadSize(message, sizeof(protocol::VringState),
                           "SET_VRING_BASE");
        requireNoDescriptors(message, "SET_VRING_BASE");
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
        requirePayloadSize(message, sizeof(protocol::VringState),
                           "GET_VRING_BASE");
        requireNoDescriptors(message, "GET_VRING_BASE");
        const auto state =
            payloadAs<protocol::VringState>(message.payload);
        if (state.index >= queues_.size() || state.value != 0)
            throw std::runtime_error("invalid GET_VRING_BASE");
        const protocol::VringState reply {
            .index = state.index,
            .value = queues_[state.index].lastAvailable,
        };
        queues_[state.index].started = false;
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
        requirePayloadSize(message, sizeof(protocol::VringAddress),
                           "SET_VRING_ADDR");
        requireNoDescriptors(message, "SET_VRING_ADDR");
        const auto address =
            payloadAs<protocol::VringAddress>(message.payload);
        if (address.index >= queues_.size() || address.flags != 0 ||
            address.descriptor % 16U != 0 ||
            address.available % alignof(std::uint16_t) != 0 ||
            address.used % alignof(std::uint32_t) != 0) {
            throw std::runtime_error("invalid SET_VRING_ADDR");
        }
        auto& queue = queues_[address.index];
        queue.descriptorAddress = address.descriptor;
        queue.availableAddress = address.available;
        queue.usedAddress = address.used;
        break;
    }
    case protocol::VhostRequest::setVringEnable: {
        requirePayloadSize(message, sizeof(protocol::VringState),
                           "SET_VRING_ENABLE");
        requireNoDescriptors(message, "SET_VRING_ENABLE");
        const auto state =
            payloadAs<protocol::VringState>(message.payload);
        if ((negotiatedFeatures_ & protocol::vhostFeatureProtocol) == 0)
            throw std::runtime_error("SET_VRING_ENABLE was not negotiated");
        if (state.index >= queues_.size() || state.value > 1)
            throw std::runtime_error("invalid SET_VRING_ENABLE");
        queues_[state.index].enabled = state.value != 0;
        processQueue(state.index);
        break;
    }
    case protocol::VhostRequest::setVringKick:
        configureKick(message);
        processQueue(static_cast<std::uint32_t>(
            payloadAs<std::uint64_t>(message.payload) &
            protocol::vhostVringIndexMask));
        break;
    case protocol::VhostRequest::vringKick: {
        requirePayloadSize(message, sizeof(protocol::VringState),
                           "VRING_KICK");
        requireNoDescriptors(message, "VRING_KICK");
        if ((negotiatedProtocolFeatures_ &
             protocol::protocolFeatureInbandNotifications) == 0) {
            throw std::runtime_error("VRING_KICK was not negotiated");
        }
        if ((message.header.flags & protocol::vhostUserNeedReply) == 0)
            throw std::runtime_error("VRING_KICK requires reply acknowledgement");
        const auto state =
            payloadAs<protocol::VringState>(message.payload);
        if (state.index >= queues_.size() || state.value != 0)
            throw std::runtime_error("invalid VRING_KICK");
        queues_[state.index].started = true;
        processQueue(state.index);
        break;
    }
    case protocol::VhostRequest::setVringCall: {
        requirePayloadSize(message, sizeof(std::uint64_t), "SET_VRING_CALL");
        const auto encodedQueue =
            payloadAs<std::uint64_t>(message.payload);
        const auto index = static_cast<std::size_t>(
            encodedQueue & protocol::vhostVringIndexMask);
        const bool noDescriptor =
            (encodedQueue & protocol::vhostVringNoDescriptor) != 0;
        if (index >= queues_.size() ||
            (encodedQueue & ~protocol::vhostVringDescriptorMask) != 0 ||
            (noDescriptor && !message.descriptors.empty()) ||
            (!noDescriptor && message.descriptors.size() != 1)) {
            throw std::runtime_error("invalid SET_VRING_CALL");
        }
        if (noDescriptor) {
            queues_[index].callDescriptor.reset();
            queues_[index].callPolling = true;
        } else {
            configureOutputDescriptor(message.descriptors[0].get());
            queues_[index].callDescriptor =
                std::move(message.descriptors[0]);
            queues_[index].callPolling = false;
        }
        break;
    }
    case protocol::VhostRequest::getConfig: {
        requireNoDescriptors(message, "GET_CONFIG");
        if ((negotiatedProtocolFeatures_ &
             protocol::protocolFeatureConfig) == 0) {
            throw std::runtime_error("GET_CONFIG was not negotiated");
        }
        if (message.payload.size() < sizeof(protocol::ConfigHeader))
            throw std::runtime_error("short GET_CONFIG payload");
        const auto configRequest =
            payloadAs<protocol::ConfigHeader>(message.payload);
        const auto configuration = gpu_.config();
        if (message.payload.size() !=
            sizeof(protocol::ConfigHeader) + configRequest.size) {
            throw std::runtime_error("invalid Virtio-GPU config request size");
        }
        if (configRequest.flags != protocol::configFlagWritable ||
            configRequest.offset > sizeof(configuration) ||
            configRequest.size > sizeof(configuration) - configRequest.offset) {
            sendReply(socket, message.header, {});
            sentReply = true;
            break;
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
    case protocol::VhostRequest::setConfig: {
        requireNoDescriptors(message, "SET_CONFIG");
        if ((negotiatedProtocolFeatures_ &
             protocol::protocolFeatureConfig) == 0) {
            throw std::runtime_error("SET_CONFIG was not negotiated");
        }
        if (message.payload.size() < sizeof(protocol::ConfigHeader))
            throw std::runtime_error("short SET_CONFIG payload");
        const auto configRequest =
            payloadAs<protocol::ConfigHeader>(message.payload);
        constexpr std::uint32_t configSize = sizeof(protocol::GpuConfig);
        constexpr std::uint32_t eventsClearOffset =
            offsetof(protocol::GpuConfig, eventsClear);
        constexpr std::uint32_t eventsClearEnd =
            eventsClearOffset + sizeof(protocol::GpuConfig::eventsClear);
        const bool validFlag =
            configRequest.flags == protocol::configFlagWritable ||
            configRequest.flags == protocol::configFlagMigration;
        const bool validRange =
            configRequest.size != 0 &&
            configRequest.offset <= configSize &&
            configRequest.size <= configSize - configRequest.offset;
        const bool writableRange =
            configRequest.offset >= eventsClearOffset &&
            configRequest.offset <= eventsClearEnd &&
            configRequest.size <= eventsClearEnd - configRequest.offset;
        if (!validFlag || !validRange ||
            message.payload.size() !=
                sizeof(protocol::ConfigHeader) + configRequest.size ||
            (configRequest.flags == protocol::configFlagWritable &&
             !writableRange)) {
            throw std::runtime_error("invalid Virtio-GPU config write");
        }
        // events_clear is write-only. No asynchronous display events exist yet.
        break;
    }
    default:
        throw std::runtime_error(
            "unsupported vhost-user request " +
            std::to_string(message.header.request));
    }

    if (!sentReply &&
        (message.header.flags & protocol::vhostUserNeedReply) != 0) {
        sendAcknowledgement(socket, message.header, 0);
    }
}

void VhostUserServer::serveConnection(
    int connection,
    const std::atomic_bool& stopRequested)
{
    UniqueDescriptor ownedConnection {connection};
    struct StopRegistration final {
        const std::atomic_bool*& slot;
        VirtioGpuDevice& gpu;
        StopRegistration(
            const std::atomic_bool*& target,
            VirtioGpuDevice& device,
            const std::atomic_bool& stop)
            : slot(target)
            , gpu(device)
        {
            slot = &stop;
            gpu.setStopRequest(&stop);
        }
        ~StopRegistration()
        {
            gpu.setStopRequest(nullptr);
            slot = nullptr;
        }
    } stopRegistration {connectionStopRequested_, gpu_, stopRequested};
    try {
        while (!stopRequested.load(std::memory_order_acquire)) {
            bool activePolling = false;
            for (std::uint32_t index = 0; index < queues_.size(); ++index) {
                auto& queue = queues_[index];
                if (queue.started && queue.polling) {
                    activePolling = true;
                    processQueue(index);
                }
            }

            std::array<pollfd, protocol::gpuQueueCount + 1U> descriptors {};
            descriptors[0] = {
                .fd = ownedConnection.get(),
                .events = POLLIN,
                .revents = 0,
            };
            for (std::size_t index = 0; index < queues_.size(); ++index) {
                const auto& queue = queues_[index];
                descriptors[index + 1U] = {
                    .fd = queue.started ? queue.kickDescriptor.get() : -1,
                    .events = POLLIN,
                    .revents = 0,
                };
            }
            const int peerPoll = ::poll(
                descriptors.data(),
                static_cast<nfds_t>(descriptors.size()),
                activePolling ? 1 : socketPollTimeoutMilliseconds);
            if (peerPoll < 0) {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error("vhost-user peer poll failed");
            }
            if (peerPoll == 0)
                continue;
            if ((descriptors[0].revents & POLLNVAL) != 0)
                throw std::runtime_error("invalid vhost-user peer descriptor");
            if (descriptors[0].revents != 0) {
                VhostUserMessage message;
                if (!receiveVhostUserMessage(
                        ownedConnection.get(), message, &stopRequested))
                    break;
                handleMessage(ownedConnection.get(), message);
                // A control message can replace queue descriptors. Rebuild the
                // poll set before interpreting any queue events from this pass.
                continue;
            }
            for (std::uint32_t index = 0; index < queues_.size(); ++index) {
                const auto events = descriptors[index + 1U].revents;
                if ((events & POLLNVAL) != 0)
                    throw std::runtime_error("invalid vhost-user kick descriptor");
                if (events != 0)
                    drainKick(index);
            }
        }
    } catch (const ConnectionStopped&) {
        // Normal cooperative shutdown while a peer is not draining output.
    } catch (const std::exception& error) {
        std::fprintf(
            stderr, "vhost-user connection failed: %s\n", error.what());
    } catch (...) {
        std::fputs("vhost-user connection failed: unknown host error\n", stderr);
    }
    ownedConnection.reset();
    resetConnection();
}

void VhostUserServer::runConnected(
    int connection,
    const std::atomic_bool& stopRequested,
    const std::function<void()>& ready)
{
    if (connection < 0)
        throw std::invalid_argument("invalid in-process vhost-user socket");
    UniqueDescriptor guard {connection};
    setCloseOnExec(connection);
    configureOutputDescriptor(connection);
    std::fprintf(
        stdout,
        "Virtio-GPU vhost-user transport: in-process socketpair\n"
        "VNC display: %s:%u (%ux%u)\n",
        vncBindAddress_.c_str(),
        static_cast<unsigned int>(vncPort_),
        display_.width(),
        display_.height());
    flushReadinessOutput();
    if (ready)
        ready();
    const int transferred = guard.release();
    serveConnection(transferred, stopRequested);
}

void VhostUserServer::run(
    const std::atomic_bool& stopRequested,
    const std::function<void()>& ready)
{
    if (socketPath_.empty())
        throw std::invalid_argument("vhost-user socket path is empty");
    if (socketPath_.string().size() >= sizeof(sockaddr_un::sun_path))
        throw std::invalid_argument("vhost-user socket path is too long");
    struct stat existingPath {};
    if (::lstat(socketPath_.c_str(), &existingPath) == 0) {
        throw std::runtime_error(
            "vhost-user socket path already exists; refusing to replace it");
    }
    if (errno != ENOENT) {
        throw std::runtime_error(
            "vhost-user socket path inspection failed: " +
            std::string(std::strerror(errno)));
    }
    SocketPathCleanup socketPathCleanup {socketPath_};
    UniqueDescriptor listener {::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (listener.get() < 0)
        throw std::runtime_error(
            "socket failed: " + std::string(std::strerror(errno)));
    setCloseOnExec(listener.get());
    setNonblocking(listener.get());

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::memcpy(
        address.sun_path,
        socketPath_.c_str(),
        socketPath_.string().size() + 1U);
    if (::bind(
            listener.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0) {
        throw std::runtime_error(
            "vhost-user bind failed: " +
            std::string(std::strerror(errno)));
    }
    socketPathCleanup.adopt();
    if (::listen(listener.get(), 1) != 0) {
        throw std::runtime_error(
            "vhost-user listen failed: " +
            std::string(std::strerror(errno)));
    }
    if (::chmod(socketPath_.c_str(), 0600) != 0) {
        throw std::runtime_error(
            "vhost-user socket permission update failed: " +
            std::string(std::strerror(errno)));
    }
    std::fprintf(
        stdout,
        "Virtio-GPU vhost-user socket: %s\n"
        "VNC display: %s:%u (%ux%u)\n",
        socketPath_.c_str(),
        vncBindAddress_.c_str(),
        static_cast<unsigned int>(vncPort_),
        display_.width(),
        display_.height());
    flushReadinessOutput();
    if (ready)
        ready();

    while (!stopRequested.load(std::memory_order_acquire)) {
        pollfd listenerPoll {
            .fd = listener.get(),
            .events = POLLIN,
            .revents = 0,
        };
        const int pollResult =
            ::poll(&listenerPoll, 1, socketPollTimeoutMilliseconds);
        if (pollResult < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(
                "poll failed: " + std::string(std::strerror(errno)));
        }
        if (pollResult == 0)
            continue;

        UniqueDescriptor connection {
            ::accept(listener.get(), nullptr, nullptr)};
        if (connection.get() < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            throw std::runtime_error(
                "accept failed: " + std::string(std::strerror(errno)));
        }
        setCloseOnExec(connection.get());
        configureOutputDescriptor(connection.get());
        serveConnection(connection.release(), stopRequested);
    }
}

} // namespace uml::gpu
