// SPDX-License-Identifier: GPL-2.0-or-later

#include "vhost_user_server.hpp"
#include "deferred_fences.hpp"
#include "virtio_gpu_device.hpp"

#include "socket_paths.hpp"
#include "memory/shared_backing.hpp"
#include "platform/apple/autorelease_scope.hpp"
#include "platform/guest_timer_mask.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/event.h>
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

#ifndef UML_GPU_STARTUP_TESTING
#define UML_GPU_STARTUP_TESTING 0
#endif

#if UML_GPU_STARTUP_TESTING
#include "in_process_service_testing.hpp"
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
constexpr std::uint64_t transportFeatures =
    protocol::vhostFeatureProtocol |
    protocol::virtioFeatureVersion1;
constexpr std::uint64_t baseOfferedProtocolFeatures =
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

/* A front-end request whose reply the front-end reads from the main socket
 * without draining the backend channel, and whose handling only reads state,
 * so it can be answered while a backend request is outstanding. */
[[nodiscard]] bool answeredDuringBackendWait(std::uint32_t request) noexcept
{
    switch (static_cast<protocol::VhostRequest>(request)) {
    case protocol::VhostRequest::getFeatures:
    case protocol::VhostRequest::getProtocolFeatures:
    case protocol::VhostRequest::getQueueNum:
    case protocol::VhostRequest::getSharedMemoryConfig:
    case protocol::VhostRequest::getConfig:
        return true;
    default:
        return false;
    }
}

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
        "GPU readiness output flush failed: " +
        std::string(std::strerror(flushError)));
}

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

void writeFrameWithDescriptor(
    int socket,
    const protocol::VhostHeader& header,
    std::span<const std::uint8_t> payload,
    int transferredDescriptor,
    const std::atomic_bool* stopRequested)
{
    if (transferredDescriptor < 0)
        throw std::runtime_error("invalid vhost-user transfer descriptor");
    std::vector<std::uint8_t> frame(sizeof(header) + payload.size());
    std::memcpy(frame.data(), &header, sizeof(header));
    std::memcpy(frame.data() + sizeof(header), payload.data(), payload.size());

    for (;;) {
        if (stopRequested != nullptr &&
            stopRequested->load(std::memory_order_acquire)) {
            throw ConnectionStopped {};
        }
        pollfd output {
            .fd = socket,
            .events = POLLOUT,
            .revents = 0,
        };
        const int pollResult = ::poll(
            &output, 1, socketPollTimeoutMilliseconds);
        if (pollResult < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("vhost-user descriptor poll failed");
        }
        if (pollResult == 0)
            continue;

        iovec vector {
            .iov_base = frame.data(),
            .iov_len = frame.size(),
        };
        alignas(cmsghdr) std::array<
            std::uint8_t, CMSG_SPACE(sizeof(int))> control {};
        msghdr message {
            .msg_name = nullptr,
            .msg_namelen = 0,
            .msg_iov = &vector,
            .msg_iovlen = 1,
            .msg_control = control.data(),
            .msg_controllen = control.size(),
            .msg_flags = 0,
        };
        auto* ancillary = CMSG_FIRSTHDR(&message);
        ancillary->cmsg_level = SOL_SOCKET;
        ancillary->cmsg_type = SCM_RIGHTS;
        ancillary->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(
            CMSG_DATA(ancillary),
            &transferredDescriptor,
            sizeof(transferredDescriptor));
        const auto count = ::sendmsg(socket, &message, MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            throw std::runtime_error(
                "vhost-user descriptor send failed: " +
                std::string(std::strerror(errno)));
        }
        if (count == 0)
            throw std::runtime_error("short vhost-user descriptor send");
        const auto consumed = static_cast<std::size_t>(count);
        if (consumed < frame.size()) {
            writeExact(
                socket,
                frame.data() + consumed,
                frame.size() - consumed,
                stopRequested);
        }
        return;
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

class GpuVhostUserDevice final : public uml::virtio::VhostUserDevice {
public:
    GpuVhostUserDevice(
        Display& display,
        VirtioGpuDevice::GuestReader reader,
        VirtioGpuDevice::GuestMapper mapper,
        std::uint32_t dotsPerInch)
        : display_(display)
        , gpu_(display, std::move(reader), std::move(mapper), dotsPerInch)
        , hasEdid_(dotsPerInch != 0)
        , eventQueue_(::kqueue())
    {
        if (eventQueue_.get() < 0)
            throw std::runtime_error("creating GPU event queue failed");
        setCloseOnExec(eventQueue_.get());
        const int resizeDescriptor = display_.resizeEventDescriptor();
        if (resizeDescriptor >= 0) {
            struct kevent change {};
            EV_SET(
                &change,
                static_cast<std::uintptr_t>(resizeDescriptor),
                EVFILT_READ,
                EV_ADD | EV_ENABLE,
                0,
                0,
                nullptr);
            int result;
            do {
                result = ::kevent(
                    eventQueue_.get(), &change, 1, nullptr, 0, nullptr);
            } while (result < 0 && errno == EINTR);
            if (result < 0) {
                throw std::runtime_error(
                    "registering GPU display-resize event failed");
            }
        }
    }

    [[nodiscard]] const char* name() const noexcept override
    {
        return "Virtio-GPU";
    }

    [[nodiscard]] std::uint64_t features() const noexcept override
    {
        return protocol::virtioGpuFeatureVirgl |
            (hasEdid_ ? protocol::virtioGpuFeatureEdid : 0U) |
            protocol::virtioGpuFeatureResourceBlob |
            protocol::virtioGpuFeatureContextInit;
    }

    [[nodiscard]] std::size_t queueCount() const noexcept override
    {
        return protocol::gpuQueueCount;
    }

    [[nodiscard]] std::vector<std::uint8_t> configuration() const override
    {
        const auto config = gpu_.config();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&config);
        return {bytes, bytes + sizeof(config)};
    }

    void configurationRead(
        std::uint32_t offset,
        std::uint32_t size) noexcept override
    {
        gpu_.recordConfigRead(offset, size);
    }

    void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) override
    {
        constexpr std::uint32_t configSize = sizeof(protocol::GpuConfig);
        constexpr std::uint32_t eventsClearOffset =
            offsetof(protocol::GpuConfig, eventsClear);
        constexpr std::uint32_t eventsClearEnd =
            eventsClearOffset + sizeof(protocol::GpuConfig::eventsClear);
        const bool validFlag =
            flags == protocol::configFlagWritable ||
            flags == protocol::configFlagMigration;
        const bool validRange =
            !bytes.empty() && offset <= configSize &&
            bytes.size() <= configSize - offset;
        const bool writableRange =
            offset >= eventsClearOffset && offset <= eventsClearEnd &&
            bytes.size() <= eventsClearEnd - offset;
        if (!validFlag || !validRange ||
            (flags == protocol::configFlagWritable && !writableRange)) {
            throw std::runtime_error("invalid Virtio-GPU config write");
        }
        if (flags == protocol::configFlagWritable) {
            std::uint32_t clearEvents = 0;
            const auto relativeOffset =
                static_cast<std::size_t>(offset - eventsClearOffset);
            std::memcpy(
                reinterpret_cast<std::uint8_t*>(&clearEvents) + relativeOffset,
                bytes.data(),
                bytes.size());
            gpu_.clearEvents(clearEvents);
            if ((clearEvents & protocol::gpuEventDisplay) != 0 &&
                gpu_.displayEventPending()) {
                configChangePending_ = true;
            }
        }
    }

    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override
    {
        const bool cursorQueue = index == protocol::gpuCursorQueue;
        if (!cursorQueue &&
            responseCapacity < sizeof(protocol::GpuControlHeader)) {
            throw std::runtime_error("short Virtio-GPU response buffer");
        }
        if (!fences_.empty() && !fences_.orderingRelaxed() &&
            !gpu_.canProcessWhileFencesPending(request, cursorQueue)) {
            return {.action = uml::virtio::VhostUserQueueAction::retry};
        }

        const bool mayDefer = canDeferFence(cursorQueue, request);
        if (mayDefer && fences_.full())
            return {.action = uml::virtio::VhostUserQueueAction::retry};
        if (mayDefer && fences_.empty())
            armFenceTimer();

        auto result = gpu_.processWithDeferredFence(
            request, cursorQueue, requestId);
        if (result.response.size() > responseCapacity)
            throw std::runtime_error("short Virtio-GPU response buffer");
        if (result.destroyedContextId) {
            /* The context's held commands complete on the next tick. */
            const bool held = !fences_.empty();
            fences_.forgetContext(*result.destroyedContextId);
            if (held)
                armFenceTimer();
        }
        if (result.fence) {
            if (!mayDefer)
                throw std::runtime_error("invalid deferred GPU fence");
            fences_.defer(
                requestId,
                loadUnaligned<protocol::GpuControlHeader>(request.data()),
                std::move(result.response),
                *result.fence,
                DeferredFences::Clock::now());
            return {.action = uml::virtio::VhostUserQueueAction::defer};
        }
        if (fences_.empty() && gpu_.rendererHasPendingWork())
            armFenceTimer();
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = std::move(result.response),
        };
    }

    void reset() noexcept override
    {
        cancelFenceTimer();
        fences_.clear();
        gpu_.reset();
        configChangePending_ = false;
    }

    [[nodiscard]] int eventDescriptor() const noexcept override
    {
        return eventQueue_.get();
    }

    void drainEventSource() override
    {
        std::array<struct kevent, 2> events {};
        const timespec timeout {};
        int eventCount;
        do {
            eventCount = ::kevent(
                eventQueue_.get(),
                nullptr,
                0,
                events.data(),
                static_cast<int>(events.size()),
                &timeout);
        } while (eventCount < 0 && errno == EINTR);
        if (eventCount < 0)
            throw std::runtime_error("draining GPU events failed");

        bool resizeReady = false;
        bool fenceTimerReady = false;
        const int resizeDescriptor = display_.resizeEventDescriptor();
        for (int index = 0; index < eventCount; ++index) {
            const auto& event = events[static_cast<std::size_t>(index)];
            if ((event.flags & EV_ERROR) != 0)
                throw std::runtime_error("GPU event source failed");
            if (event.filter == EVFILT_TIMER &&
                event.ident == fenceTimerIdentifier) {
                fenceTimerReady = true;
                continue;
            }
            if (resizeDescriptor >= 0 && event.filter == EVFILT_READ &&
                event.ident ==
                    static_cast<std::uintptr_t>(resizeDescriptor)) {
                resizeReady = true;
                continue;
            }
            throw std::runtime_error("unexpected GPU event source");
        }

        if (resizeReady && display_.drainResizeEvent()) {
            gpu_.notifyDisplayChanged();
            configChangePending_ = true;
        }
        if (!fenceTimerReady)
            return;
        if (fences_.empty()) {
            /* A guest thread blocked on a query result spins on the buffer
             * the renderer fills and issues no further commands, so no fence
             * of its own is left to drive the scan that completes the query.
             * Keep polling for as long as the renderer owns such work. */
            if (!gpu_.rendererHasPendingWork())
                return;
            if (gpu_.pollRenderer() != 0)
                throw std::runtime_error("polling GPU fences failed");
            if (gpu_.rendererHasPendingWork())
                armFenceTimer();
            return;
        }

        /* A Virgl context retires through the renderer-wide fence scan, so
         * one poll drains every Virgl context. A Venus context owns its own
         * render-server timeline and retires only when it is polled itself,
         * so each distinct pending context needs its own poll. */
        fences_.forEachContext([this](std::uint32_t contextId) {
            if (gpu_.pollContextFences(contextId) != 0)
                throw std::runtime_error("polling GPU fences failed");
        });
        fences_.retire(
            DeferredFences::Clock::now(),
            [this](const DeferredFence& fence) {
                return gpu_.contextFenceStatus(fence);
            });
        if (!fences_.empty() || gpu_.rendererHasPendingWork())
            armFenceTimer();
    }

    [[nodiscard]] bool takeQueueCompletion(
        uml::virtio::VhostUserQueueCompletion& completion) override
    {
        DeferredFences::Completion completed;
        if (!fences_.takeCompletion(completed))
            return false;
        const auto* requestBytes =
            reinterpret_cast<const std::uint8_t*>(&completed.requestHeader);
        traceQueue(
            protocol::gpuControlQueue,
            {requestBytes, sizeof(completed.requestHeader)},
            completed.response);
        completion = {
            .requestId = completed.requestId,
            .response = std::move(completed.response),
        };
        return true;
    }

    [[nodiscard]] bool configChangePending() const noexcept override
    {
        return configChangePending_;
    }

    [[nodiscard]] bool claimConfigChange() noexcept override
    {
        return configChangePending_ && display_.claimResizeNotification();
    }

    void releaseConfigChange() noexcept override
    {
        display_.releaseResizeNotification();
    }

    void clearConfigChange() noexcept override
    {
        configChangePending_ = false;
    }

    void prepareReset() noexcept override
    {
        gpu_.prepareReset();
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
        uml::virtio::VhostUserSharedMemoryAction& destination) override
    {
        auto action = gpu_.takeSharedMemoryAction();
        if (!action)
            return false;
        destination = uml::virtio::VhostUserSharedMemoryAction {
            .type = action->type ==
                    VirtioGpuDevice::SharedMemoryAction::Type::map
                ? uml::virtio::VhostUserSharedMemoryAction::Type::map
                : uml::virtio::VhostUserSharedMemoryAction::Type::unmap,
            .actionId = action->actionId,
            .regionId = action->regionId,
            .descriptorOffset = action->descriptorOffset,
            .regionOffset = action->regionOffset,
            .length = action->length,
            .writable = action->writable,
            .descriptor = std::move(action->descriptor),
        };
        return true;
    }

    void completeSharedMemoryAction(
        std::uint64_t actionId,
        bool success) noexcept override
    {
        gpu_.completeSharedMemoryAction(actionId, success);
    }

    void setStopRequest(
        const std::atomic_bool* stopRequested) noexcept override
    {
        gpu_.setStopRequest(stopRequested);
    }

    void traceQueue(
        std::uint32_t index,
        std::span<const std::uint8_t> request,
        std::span<const std::uint8_t> response) const noexcept override
    {
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
            if (requestHeader.type == static_cast<std::uint32_t>(
                    protocol::GpuCommand::resourceCreateBlob) &&
                request.size() >= sizeof(protocol::GpuResourceCreateBlob)) {
                const auto create =
                    loadUnaligned<protocol::GpuResourceCreateBlob>(
                        request.data());
                std::fprintf(
                    stderr,
                    "Virtio-GPU blob create resource=%u size=%llu "
                    "blob=%llu flags=0x%x\n",
                    create.resourceId,
                    static_cast<unsigned long long>(create.size),
                    static_cast<unsigned long long>(create.blobId),
                    create.flags);
            } else if (requestHeader.type == static_cast<std::uint32_t>(
                           protocol::GpuCommand::resourceMapBlob) &&
                       request.size() >=
                           sizeof(protocol::GpuResourceMapBlob)) {
                const auto map = loadUnaligned<protocol::GpuResourceMapBlob>(
                    request.data());
                std::fprintf(
                    stderr,
                    "Virtio-GPU blob map resource=%u offset=%llu\n",
                    map.resourceId,
                    static_cast<unsigned long long>(map.offset));
            } else if (requestHeader.type == static_cast<std::uint32_t>(
                           protocol::GpuCommand::resourceUnmapBlob) &&
                       request.size() >=
                           sizeof(protocol::GpuResourceUnmapBlob)) {
                const auto unmap =
                    loadUnaligned<protocol::GpuResourceUnmapBlob>(
                        request.data());
                std::fprintf(
                    stderr,
                    "Virtio-GPU blob unmap resource=%u\n",
                    unmap.resourceId);
            }
        }
#else
        (void)index;
        (void)request;
        (void)response;
#endif
    }

private:
    static constexpr std::uintptr_t fenceTimerIdentifier = 1;
    static constexpr std::intptr_t fencePollMicroseconds = 100;

    [[nodiscard]] bool canDeferFence(
        bool cursorQueue,
        std::span<const std::uint8_t> request) const noexcept
    {
        if (cursorQueue || request.size() < sizeof(protocol::GpuControlHeader))
            return false;
        const auto header =
            loadUnaligned<protocol::GpuControlHeader>(request.data());
        const auto command = static_cast<protocol::GpuCommand>(header.type);
        const bool fence = (header.flags & protocol::gpuFlagFence) != 0;
        const bool pipelineCommand =
            command == protocol::GpuCommand::submit3d ||
            command == protocol::GpuCommand::transferToHost3d ||
            command == protocol::GpuCommand::transferFromHost3d;
        /* Virgl retires one fence timeline per context and Venus one per
         * guest ring. The device knows each context's capset, so it decides
         * which rings defer and completes the rest synchronously. */
        return pipelineCommand && fence && header.contextId != 0;
    }

    void armFenceTimer()
    {
        struct kevent change {};
        EV_SET(
            &change,
            fenceTimerIdentifier,
            EVFILT_TIMER,
            EV_ADD | EV_ENABLE | EV_ONESHOT,
            NOTE_USECONDS,
            fencePollMicroseconds,
            nullptr);
        int result;
        do {
            result = ::kevent(
                eventQueue_.get(), &change, 1, nullptr, 0, nullptr);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            throw std::runtime_error("arming GPU fence timer failed");
    }

    void cancelFenceTimer() noexcept
    {
        struct kevent change {};
        EV_SET(
            &change,
            fenceTimerIdentifier,
            EVFILT_TIMER,
            EV_DELETE,
            0,
            0,
            nullptr);
        while (::kevent(
                   eventQueue_.get(), &change, 1, nullptr, 0, nullptr) < 0 &&
            errno == EINTR) {
        }
    }

    Display& display_;
    VirtioGpuDevice gpu_;
    bool hasEdid_;
    bool configChangePending_ {};
    UniqueDescriptor eventQueue_;
    DeferredFences fences_;
};

} // namespace

VhostUserServer::VhostUserServer(
    std::filesystem::path socketPath,
    std::string vncBindAddress,
    std::uint16_t vncPort,
    std::string vncPassword,
    std::uint32_t width,
    std::uint32_t height,
    bool selfTestPattern,
    uml::input::vnc::VncInputSink inputSink,
    DisplayMode displayMode,
    void* nativeView,
    std::uint32_t dotsPerInch,
    uml::input::vnc::PointerMode pointerMode,
    bool autoResizeGuest)
    : socketPath_(std::move(socketPath))
    , vncBindAddress_(std::move(vncBindAddress))
    , display_(makeDisplay(
          displayMode,
          vncBindAddress_,
          vncPort,
          std::move(vncPassword),
          width,
          height,
          std::move(inputSink),
          nativeView,
          pointerMode,
          dotsPerInch == 0 ?
              protocol::maximumDisplayDimension :
              protocol::maximumEdidDimension,
          autoResizeGuest))
{
    device_ = std::make_unique<GpuVhostUserDevice>(
        *display_,
        [this](std::uint64_t address, std::span<std::uint8_t> destination) {
            return readGuest(address, destination);
        },
        [this](std::uint64_t address, std::size_t length) {
            auto* pointer = translate(address, length, false);
            return pointer == nullptr ?
                std::span<std::uint8_t> {} :
                std::span(pointer, length);
        },
        dotsPerInch);
    queues_.resize(device_->queueCount());
    if (selfTestPattern)
        display_->fillSelfTestPattern();
}

VhostUserServer::VhostUserServer(
    std::unique_ptr<uml::virtio::VhostUserDevice> device)
    : device_(std::move(device))
{
    constexpr std::size_t maximumQueueCount =
        protocol::vhostVringIndexMask + 1U;
    if (!device_ || device_->queueCount() == 0 ||
        device_->queueCount() > maximumQueueCount)
        throw std::invalid_argument("invalid vhost-user device queue count");
    device_->setGuestMapper([this](std::uint64_t address, std::size_t length) {
        auto* pointer = translate(address, length, false);
        return pointer ? std::span(pointer, length) : std::span<std::uint8_t>{};
    });
    queues_.resize(device_->queueCount());
}

VhostUserServer::~VhostUserServer()
{
    resetConnection();
}

void VhostUserServer::setSharedMemoryRegions(
    std::shared_ptr<uml::virtio::SharedMemoryRegions> regions)
{
    sharedMemoryRegions_ = std::move(regions);
    device_->setSharedMemoryRegions(sharedMemoryRegions_);
}

void VhostUserServer::resetConnection() noexcept
{
    resetDeviceState();
    slaveRequestDescriptor_.reset();
    if (sharedMemoryRegions_ && device_ && device_->sharedMemoryRegion())
        sharedMemoryRegions_->revokeAll();
    clearMappings();
    resetQueues();
    negotiatedFeatures_ = 0;
    negotiatedProtocolFeatures_ = 0;
    featuresConfigured_ = false;
}

void VhostUserServer::resetDeviceState() noexcept
{
    device_->prepareReset();
    try {
        dispatchSharedMemoryActions();
    } catch (...) {
        // Fail closed if an acknowledgement is missing or negative. Closing
        // the backend-request channel makes the front-end revoke every map;
        // local teardown must still retire each renderer owner below.
        slaveRequestDescriptor_.reset();
    }
    device_->reset();
    pendingRequests_.clear();
}

void VhostUserServer::resetQueues() noexcept
{
    for (auto& queue : queues_)
        queue = Queue {};
}

void VhostUserServer::clearMappings() noexcept
{
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
            auto backing = ::uml::memory::acquire_shared_backing_view(
                descriptor,
                region.mmapOffset,
                static_cast<std::size_t>(region.size),
                PROT_READ | PROT_WRITE);
            if (!backing)
                throw std::runtime_error(
                    "guest-memory mmap failed: " +
                    std::string(std::strerror(errno)));
            Mapping mapping {
                .guestAddress = region.guestAddress,
                .userAddress = region.userAddress,
                .size = region.size,
                .mmapOffset = region.mmapOffset,
                .backing = std::move(backing),
            };
            replacements.push_back(std::move(mapping));
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
    resetDeviceState();
    clearMappings();
    resetQueues();
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
        throw std::runtime_error("invalid vhost-user kick queue");
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
            auto* data = static_cast<std::uint8_t*>(mapping.backing.data());
            return data + offset;
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
        throw std::runtime_error("invalid vhost-user completion queue");
    if (queues_[index].callDescriptor.get() >= 0) {
        constexpr std::uint64_t completionCount = 1;
        writeExact(
            queues_[index].callDescriptor.get(),
            &completionCount,
            sizeof(completionCount),
            connectionStopRequested_);
        return;
    }
    if (queues_[index].callPolling) {
        return;
    }
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

    awaitBackendAcknowledgement(header, "in-band completion");
}

void VhostUserServer::signalPendingConfigChange()
{
    if (!device_->configChangePending())
        return;
    constexpr std::uint64_t requiredFeatures =
        protocol::protocolFeatureReplyAck |
        protocol::protocolFeatureSlaveRequest |
        protocol::protocolFeatureConfig;
    if ((negotiatedProtocolFeatures_ & requiredFeatures) != requiredFeatures ||
        slaveRequestDescriptor_.get() < 0) {
        return;
    }
    if (!device_->claimConfigChange())
        return;

    const protocol::VhostHeader header {
        .request = static_cast<std::uint32_t>(
            protocol::VhostSlaveRequest::configChange),
        .flags = protocol::vhostUserVersion | protocol::vhostUserNeedReply,
        .size = 0,
    };
    try {
        writeFrame(
            slaveRequestDescriptor_.get(),
            header,
            {},
            connectionStopRequested_);
    } catch (...) {
        device_->releaseConfigChange();
        throw;
    }
    /* Publication is the transition boundary. The UI can begin resizing once
     * the complete request is on the backend channel; it need not wait for a
     * potentially slow frontend acknowledgement. */
    device_->releaseConfigChange();

    awaitBackendAcknowledgement(header, "config-change");
    device_->clearConfigChange();
}

void VhostUserServer::awaitBackendAcknowledgement(
    const protocol::VhostHeader& request,
    const char* channel)
{
    /* The front-end reads the reply to a query such as GET_CONFIG from the
     * main socket without draining this backend channel, and on UML the CPU
     * that would drain it can be the one waiting for that reply. Blocking
     * here on the backend channel alone would leave each side waiting for
     * the other, so the queries are answered while the backend request is
     * outstanding. Any other request waits until the reply has arrived: its
     * sender drains the backend channel while it waits for an
     * acknowledgement, and handling it here could start another backend
     * request inside this one. */
    bool frontendOpen = frontendDescriptor_ >= 0;
    for (;;) {
        std::array<pollfd, 2> descriptors {{
            {
                .fd = slaveRequestDescriptor_.get(),
                .events = POLLIN,
                .revents = 0,
            },
            {
                .fd = frontendOpen ? frontendDescriptor_ : -1,
                .events = POLLIN,
                .revents = 0,
            },
        }};
        const int ready = ::poll(
            descriptors.data(),
            static_cast<nfds_t>(descriptors.size()),
            socketPollTimeoutMilliseconds);
        if (connectionStopRequested_ != nullptr &&
            connectionStopRequested_->load(std::memory_order_acquire)) {
            throw ConnectionStopped {};
        }
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(
                std::string("vhost-user ") + channel + " poll failed");
        }
        if (ready == 0)
            continue;
        if ((descriptors[0].revents & POLLNVAL) != 0 ||
            (descriptors[1].revents & POLLNVAL) != 0) {
            throw std::runtime_error(
                std::string("invalid descriptor while waiting on the vhost-user ") +
                channel + " channel");
        }
        if (descriptors[0].revents != 0) {
            VhostUserMessage reply;
            if (!receiveVhostUserMessage(
                    slaveRequestDescriptor_.get(),
                    reply,
                    connectionStopRequested_)) {
                if (connectionStopRequested_ != nullptr &&
                    connectionStopRequested_->load(
                        std::memory_order_acquire)) {
                    throw ConnectionStopped {};
                }
                throw std::runtime_error(
                    std::string("closed vhost-user ") + channel + " channel");
            }
            constexpr std::uint32_t expectedReplyFlags =
                protocol::vhostUserVersion | protocol::vhostUserReply;
            if (reply.header.request != request.request ||
                reply.header.flags != expectedReplyFlags ||
                reply.payload.size() != sizeof(std::uint64_t) ||
                !reply.descriptors.empty() ||
                payloadAs<std::uint64_t>(reply.payload) != 0) {
                throw std::runtime_error(
                    std::string("invalid vhost-user ") + channel +
                    " acknowledgement");
            }
            return;
        }
        if (descriptors[1].revents == 0)
            continue;
        VhostUserMessage request;
        if (!receiveVhostUserMessage(
                frontendDescriptor_, request, connectionStopRequested_)) {
            // The connection loop sees the closed socket again afterwards.
            frontendOpen = false;
            continue;
        }
        if (answeredDuringBackendWait(request.header.request))
            handleMessage(frontendDescriptor_, request);
        else
            deferredFrontendMessages_.push_back(std::move(request));
    }
}

void VhostUserServer::handleDeferredFrontendMessages(int socket)
{
    // Handling one can defer more; they all keep their arrival order.
    while (!deferredFrontendMessages_.empty()) {
        VhostUserMessage message =
            std::move(deferredFrontendMessages_.front());
        deferredFrontendMessages_.pop_front();
        handleMessage(socket, message);
    }
}

std::uint64_t VhostUserServer::allocateRequestId()
{
    if (nextRequestId_ == 0)
        throw std::runtime_error("vhost-user request identifier exhausted");
    return nextRequestId_++;
}

void VhostUserServer::completeRequest(
    const PendingRequest& request,
    std::span<const std::uint8_t> response)
{
    if (request.queueIndex >= queues_.size() ||
        response.size() > request.responseCapacity ||
        response.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("invalid deferred virtio completion");
    }
    auto& queue = queues_[request.queueIndex];
    if (queue.size == 0)
        throw std::runtime_error("completion targets an unconfigured virtqueue");

    std::size_t responseOffset = 0;
    for (const auto& segment : request.segments) {
        if (!segment.writable || responseOffset == response.size())
            continue;
        const auto count = std::min<std::size_t>(
            segment.length, response.size() - responseOffset);
        if (count == 0)
            continue;
        auto* destination = translate(segment.address, count, false);
        if (destination == nullptr)
            throw std::runtime_error("completion buffer is outside guest memory");
        std::memcpy(
            destination, response.data() + responseOffset, count);
        responseOffset += count;
    }
    if (responseOffset != response.size())
        throw std::runtime_error("incomplete virtio response write");

    auto* used = translate(
        queue.usedAddress,
        protocol::splitRingHeaderSize +
            static_cast<std::size_t>(queue.size) *
                sizeof(protocol::VirtqueueUsedElement),
        true);
    if (used == nullptr ||
        reinterpret_cast<std::uintptr_t>(used) % alignof(std::uint16_t) != 0) {
        throw std::runtime_error("virtqueue used ring is unavailable");
    }
    const auto usedIndex = loadSharedRingValue(
        used + sizeof(std::uint16_t), std::memory_order_relaxed);
    const protocol::VirtqueueUsedElement element {
        .id = request.head,
        .length = static_cast<std::uint32_t>(responseOffset),
    };
    const auto usedOffset =
        protocol::splitRingHeaderSize +
        static_cast<std::size_t>(usedIndex % queue.size) * sizeof(element);
    storeUnaligned(used + usedOffset, element);
    storeSharedRingValue(
        used + sizeof(std::uint16_t),
        static_cast<std::uint16_t>(usedIndex + 1U),
        std::memory_order_release);
}

void VhostUserServer::notifyQueue(std::uint32_t index)
{
    if (index >= queues_.size())
        throw std::runtime_error("invalid virtqueue notification index");
    const auto& queue = queues_[index];
    auto* available = translate(
        queue.availableAddress,
        protocol::splitRingHeaderSize +
            static_cast<std::size_t>(queue.size) * sizeof(std::uint16_t),
        true);
    if (available == nullptr ||
        reinterpret_cast<std::uintptr_t>(available) %
                alignof(std::uint16_t) !=
            0) {
        throw std::runtime_error("virtqueue available ring is unavailable");
    }
    // Notification suppression is sampled after publishing used entries.
    const auto flags =
        loadSharedRingValue(available, std::memory_order_acquire);
    if ((flags & ~protocol::availableSuppressInterrupt) != 0)
        throw std::runtime_error("virtqueue available ring has reserved flags");
    if ((flags & protocol::availableSuppressInterrupt) == 0)
        signalQueue(index);
}

void VhostUserServer::drainQueueCompletions()
{
    std::array<bool, protocol::vhostVringIndexMask + 1U> completedQueues {};
    uml::virtio::VhostUserQueueCompletion completion;
    while (device_->takeQueueCompletion(completion)) {
        const auto found = pendingRequests_.find(completion.requestId);
        if (found == pendingRequests_.end()) {
            completion = {};
            continue;
        }
        const auto queueIndex = found->second.queueIndex;
        completeRequest(found->second, completion.response);
        pendingRequests_.erase(found);
        completedQueues[queueIndex] = true;
        completion = {};
    }
    for (std::uint32_t index = 0; index < queues_.size(); ++index) {
        if (completedQueues[index])
            notifyQueue(index);
    }
}

void VhostUserServer::dispatchSharedMemoryActions()
{
    uml::virtio::VhostUserSharedMemoryAction action;
    while (device_->takeSharedMemoryAction(action)) {
        bool completed = false;
        try {
            constexpr std::uint64_t requiredSharedMemoryFeatures =
                protocol::protocolFeatureSharedMemoryMap |
                protocol::protocolFeatureSlaveRequest |
                protocol::protocolFeatureReplyAck;
            if ((negotiatedProtocolFeatures_ &
                 requiredSharedMemoryFeatures) !=
                    requiredSharedMemoryFeatures ||
                slaveRequestDescriptor_.get() < 0) {
                throw std::runtime_error(
                    "GPU shared-memory action was not negotiated");
            }
            const auto region = device_->sharedMemoryRegion();
            if (!region || action.regionId != region->id ||
                action.length == 0 ||
                (action.descriptorOffset |
                 action.regionOffset |
                 action.length) %
                    protocol::hostVisibleSharedMemoryAlignment != 0 ||
                action.regionOffset > region->size ||
                action.length > region->size - action.regionOffset) {
                throw std::runtime_error(
                    "invalid GPU shared-memory action range");
            }
            const bool map = action.type ==
                uml::virtio::VhostUserSharedMemoryAction::Type::map;
            if (map) {
                struct stat descriptorStatus {};
                if (action.descriptor.get() < 0 ||
                    ::fstat(action.descriptor.get(), &descriptorStatus) != 0 ||
                    descriptorStatus.st_size < 0 ||
                    action.descriptorOffset >
                        static_cast<std::uint64_t>(descriptorStatus.st_size) ||
                    action.length >
                        static_cast<std::uint64_t>(descriptorStatus.st_size) -
                            action.descriptorOffset) {
                    throw std::runtime_error(
                        "invalid GPU shared-memory action descriptor");
                }
            } else if (action.descriptor.get() >= 0 ||
                       action.descriptorOffset != 0 || action.writable) {
                throw std::runtime_error(
                    "invalid GPU shared-memory unmap action");
            }
            const protocol::SharedMemoryMap payload {
                .regionId = action.regionId,
                .descriptorOffset = action.descriptorOffset,
                .regionOffset = action.regionOffset,
                .length = action.length,
                .flags = action.writable
                    ? protocol::sharedMemoryMapReadWrite
                    : protocol::sharedMemoryMapReadOnly,
            };
            const protocol::VhostHeader header {
                .request = static_cast<std::uint32_t>(map
                    ? protocol::VhostSlaveRequest::sharedMemoryMap
                    : protocol::VhostSlaveRequest::sharedMemoryUnmap),
                .flags = protocol::vhostUserVersion |
                    protocol::vhostUserNeedReply,
                .size = sizeof(payload),
            };
            const auto bytes = std::span {
                reinterpret_cast<const std::uint8_t*>(&payload),
                sizeof(payload),
            };
            /* A revoked range leaves the index before the guest is told
             * to unmap it, and a published one enters only once the guest
             * holds it, so no other backend can resolve a range the guest
             * does not have. */
            if (sharedMemoryRegions_ && !map) {
                sharedMemoryRegions_->revoke(
                    action.regionId, action.regionOffset, action.length);
            }
            if (map) {
                writeFrameWithDescriptor(
                    slaveRequestDescriptor_.get(),
                    header,
                    bytes,
                    action.descriptor.get(),
                    connectionStopRequested_);
            } else {
                writeFrame(
                    slaveRequestDescriptor_.get(),
                    header,
                    bytes,
                    connectionStopRequested_);
            }

            // Do not expose the Virtio-GPU mapping transition until the
            // front-end has installed or revoked the corresponding GPA range.
            awaitBackendAcknowledgement(header, "shared-memory");
            if (sharedMemoryRegions_ && map &&
                !sharedMemoryRegions_->publish(
                    action.regionId,
                    action.regionOffset,
                    action.length,
                    action.descriptor.get(),
                    action.descriptorOffset)) {
                throw std::runtime_error(
                    "shared-memory range could not be published to the host");
            }
            device_->completeSharedMemoryAction(action.actionId, true);
            completed = true;
        } catch (...) {
            if (!completed) {
                device_->completeSharedMemoryAction(action.actionId, false);
            }
            throw;
        }
        action = {};
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
    const auto availableIndex = loadSharedRingValue(
        available + sizeof(std::uint16_t), std::memory_order_acquire);
    const auto pending = static_cast<std::uint16_t>(
        availableIndex - queue.lastAvailable);
    if (pending > queue.size)
        throw std::runtime_error("virtqueue producer advanced beyond ring size");

    bool completed = false;
    while (queue.lastAvailable != availableIndex) {
        const auto ringOffset =
            protocol::splitRingHeaderSize +
            static_cast<std::size_t>(queue.lastAvailable % queue.size) *
                sizeof(std::uint16_t);
        const auto head =
            loadUnaligned<std::uint16_t>(available + ringOffset);
        auto segments = descriptorChain(queue, head);

        std::size_t requestSize = 0;
        std::size_t responseCapacity = 0;
        for (const auto& segment : segments) {
            if (segment.writable)
                responseCapacity += segment.length;
            else
                requestSize += segment.length;
        }
        if (requestSize > maximumVhostUserMessagePayload)
            throw std::runtime_error("oversized virtio request");

        if (!queue.enabled) {
            const PendingRequest disabled {
                .requestId = 0,
                .queueIndex = index,
                .head = head,
                .responseCapacity = responseCapacity,
                .segments = std::move(segments),
            };
            completeRequest(disabled, {});
            ++queue.lastAvailable;
            completed = true;
            continue;
        }

        std::vector<std::uint8_t> request(requestSize);
        std::size_t requestOffset = 0;
        for (const auto& segment : segments) {
            if (segment.writable || segment.length == 0)
                continue;
            auto* source = translate(segment.address, segment.length, false);
            std::memcpy(
                request.data() + requestOffset, source, segment.length);
            requestOffset += segment.length;
        }

        const auto requestId = allocateRequestId();
        PendingRequest accepted {
            .requestId = requestId,
            .queueIndex = index,
            .head = head,
            .responseCapacity = responseCapacity,
            .segments = std::move(segments),
        };
        auto [position, inserted] = pendingRequests_.emplace(
            requestId, std::move(accepted));
        if (!inserted)
            throw std::runtime_error("duplicate vhost-user request identifier");

        uml::virtio::VhostUserQueueResult result;
        try {
            result = device_->processQueue(
                index, requestId, request, responseCapacity);
            // Map/unmap backend requests must be visible before the matching
            // Virtio-GPU control response is published to the guest.
            dispatchSharedMemoryActions();
        } catch (...) {
            pendingRequests_.erase(position);
            throw;
        }
        switch (result.action) {
        case uml::virtio::VhostUserQueueAction::retry:
            pendingRequests_.erase(position);
            // A device may consume one descriptor and then run out of data on
            // the next descriptor in the same available-ring snapshot.  The
            // completed prefix must be visible to the driver before waiting
            // for the external source to become ready again.
            if (completed)
                notifyQueue(index);
            return;
        case uml::virtio::VhostUserQueueAction::defer:
            if (!result.response.empty())
                throw std::runtime_error("deferred request returned a response");
            ++queue.lastAvailable;
            break;
        case uml::virtio::VhostUserQueueAction::complete:
            if (result.response.size() > responseCapacity)
                throw std::runtime_error("short virtio response buffer");
            device_->traceQueue(index, request, result.response);
            completeRequest(position->second, result.response);
            pendingRequests_.erase(position);
            ++queue.lastAvailable;
            completed = true;
            break;
        }
    }
    if (completed)
        notifyQueue(index);
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
    const std::uint64_t offeredFeatures =
        transportFeatures | device_->features();
    const auto sharedMemoryRegion = device_->sharedMemoryRegion();
    if (sharedMemoryRegion && sharedMemoryRegion->size == 0) {
        throw std::runtime_error("invalid shared-memory region declaration");
    }
    const std::uint64_t offeredProtocolFeatures =
        baseOfferedProtocolFeatures |
        (sharedMemoryRegion
            ? protocol::protocolFeatureSharedMemoryMap
            : 0U);
    bool sentReply = false;
    const auto acknowledgeAcceptedRequest = [&] {
        if (sentReply ||
            (message.header.flags & protocol::vhostUserNeedReply) == 0) {
            return;
        }
        sendAcknowledgement(socket, message.header, 0);
        sentReply = true;
    };
    const auto processAcceptedQueue = [&](std::uint32_t index) {
        try {
            processQueue(index);
        } catch (...) {
            // Once a kick is accepted, release the front-end's synchronous
            // sender before the connection is closed for a queue-side error.
            acknowledgeAcceptedRequest();
            throw;
        }
    };
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
        constexpr std::uint64_t requiredSharedMemoryFeatures =
            protocol::protocolFeatureSharedMemoryMap |
            protocol::protocolFeatureSlaveRequest |
            protocol::protocolFeatureReplyAck;
        if ((requestedProtocolFeatures &
             protocol::protocolFeatureSharedMemoryMap) != 0 &&
            (requestedProtocolFeatures & requiredSharedMemoryFeatures) !=
                requiredSharedMemoryFeatures) {
            throw std::runtime_error(
                "shared-memory mapping requires backend requests and reply ack");
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
    case protocol::VhostRequest::getSharedMemoryConfig: {
        requirePayloadSize(message, 0, "GET_SHMEM_CONFIG");
        requireNoDescriptors(message, "GET_SHMEM_CONFIG");
        if (!sharedMemoryRegion ||
            (negotiatedProtocolFeatures_ &
             protocol::protocolFeatureSharedMemoryMap) == 0) {
            throw std::runtime_error(
                "GET_SHMEM_CONFIG was not negotiated");
        }
        protocol::SharedMemoryConfig config {};
        // num_regions counts non-zero entries; the array index remains the
        // Virtio shared-memory region identifier.
        config.regionCount = 1;
        config.regionSizes[sharedMemoryRegion->id] = sharedMemoryRegion->size;
        const auto bytes = std::span {
            reinterpret_cast<const std::uint8_t*>(&config), sizeof(config)};
        sendReply(socket, message.header, bytes);
        sentReply = true;
        break;
    }
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
        resetDeviceState();
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
        if (!pendingRequests_.empty())
            throw std::runtime_error(
                "SET_VRING_NUM while deferred requests are pending");
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
        if (!pendingRequests_.empty())
            throw std::runtime_error(
                "SET_VRING_BASE while deferred requests are pending");
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
        if (!pendingRequests_.empty())
            resetDeviceState();
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
        if (!pendingRequests_.empty())
            throw std::runtime_error(
                "SET_VRING_ADDR while deferred requests are pending");
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
        if (state.value == 0 && !pendingRequests_.empty()) {
            resetDeviceState();
        }
        queues_[state.index].enabled = state.value != 0;
        if (queues_[state.index].enabled)
            processAcceptedQueue(state.index);
        break;
    }
    case protocol::VhostRequest::setVringKick:
        configureKick(message);
        processAcceptedQueue(static_cast<std::uint32_t>(
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
        processAcceptedQueue(state.index);
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
        if (message.payload.size() !=
            sizeof(protocol::ConfigHeader) + configRequest.size) {
            throw std::runtime_error("invalid Virtio-GPU config request size");
        }
        if (configRequest.flags != protocol::configFlagWritable) {
            sendReply(socket, message.header, {});
            sentReply = true;
            break;
        }
        const auto configuration = device_->configuration();
        if (
            configRequest.offset > configuration.size() ||
            configRequest.size > configuration.size() - configRequest.offset) {
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
            configuration.data() + configRequest.offset,
            configRequest.size);
        sendReply(socket, message.header, payload);
        device_->configurationRead(configRequest.offset, configRequest.size);
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
        if (message.payload.size() !=
            sizeof(protocol::ConfigHeader) + configRequest.size) {
            throw std::runtime_error("invalid virtio config write size");
        }
        device_->writeConfiguration(
            configRequest.offset,
            configRequest.flags,
            std::span(
                message.payload.data() + sizeof(protocol::ConfigHeader),
                configRequest.size));
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
    /* Every arriving descriptor is installed in this process's file table, and
     * a backend that publishes a shared-memory region hands the guest one more
     * for every host-visible blob it maps. Take the ceiling the host is willing
     * to give before the first connection serves a request; the platform
     * default is an application's, not a machine's.
     */
    static std::once_flag descriptorCeilingOnce;
    std::call_once(descriptorCeilingOnce, [] {
        (void)raiseDescriptorCeiling();
        // A plain flush: this note must never be able to fail a connection.
        std::fprintf(
            stdout,
            "vhost-user transport: %s\n",
            describeDescriptorCeiling().c_str());
        (void)std::fflush(stdout);
    });
    struct StopRegistration final {
        const std::atomic_bool*& slot;
        uml::virtio::VhostUserDevice& device;
        StopRegistration(
            const std::atomic_bool*& target,
            uml::virtio::VhostUserDevice& registeredDevice,
            const std::atomic_bool& stop)
            : slot(target)
            , device(registeredDevice)
        {
            slot = &stop;
            device.setStopRequest(&stop);
        }
        ~StopRegistration()
        {
            device.setStopRequest(nullptr);
            slot = nullptr;
        }
    } stopRegistration {connectionStopRequested_, *device_, stopRequested};
    std::vector<pollfd> descriptors(queues_.size() + 2U);
    const std::size_t deviceEventIndex = queues_.size() + 1U;
    frontendDescriptor_ = ownedConnection.get();
    try {
        while (!stopRequested.load(std::memory_order_acquire)) {
            /* The renderer drives Metal from this thread, and Metal
             * autoreleases per submission. This thread has no run loop to
             * drain that for it, so bound a pool to the work of one pass;
             * otherwise the objects live as long as the connection and the
             * process grows by gigabytes within a minute of a GL workload. */
            const uml::platform::apple::AutoreleaseScope autoreleaseScope;

            // Requests read while a backend reply was outstanding come
            // before anything still unread on the socket.
            handleDeferredFrontendMessages(ownedConnection.get());

            /* This also publishes a resize which arrived before the guest
             * installed its backend-request descriptor. Running it at the
             * loop boundary ensures any SET_SLAVE_REQ_FD acknowledgement was
             * sent on the main channel before waiting for this reply. */
            signalPendingConfigChange();
            bool activePolling = false;
            for (std::uint32_t index = 0; index < queues_.size(); ++index) {
                auto& queue = queues_[index];
                if (queue.started && queue.polling) {
                    activePolling = true;
                    processQueue(index);
                }
            }
            // Queue work can complete an earlier deferred request without an
            // external readiness edge (for example a decoder produces a frame
            // for a capture buffer already queued by the guest).
            drainQueueCompletions();
            // A backend wait in this pass may have deferred requests, and
            // they must be handled before the socket is read again.
            if (!deferredFrontendMessages_.empty())
                continue;

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
            descriptors[deviceEventIndex] = {
                .fd = device_->eventDescriptor(),
                .events = POLLIN,
                .revents = 0,
            };
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
            const auto deviceEvents = descriptors[deviceEventIndex].revents;
            if ((deviceEvents & POLLNVAL) != 0)
                throw std::runtime_error("invalid virtio device event descriptor");
            if (deviceEvents != 0) {
                device_->drainEventSource();
                signalPendingConfigChange();
                drainQueueCompletions();
                for (std::uint32_t index = 0; index < queues_.size(); ++index)
                    processQueue(index);
            }
        }
    } catch (const ConnectionStopped&) {
        // Normal cooperative shutdown while a peer is not draining output.
    } catch (const std::exception& error) {
        std::fprintf(
            stderr,
            "%s vhost-user connection failed: %s\n",
            device_->name(),
            error.what());
    } catch (...) {
        std::fprintf(
            stderr,
            "%s vhost-user connection failed: unknown host error\n",
            device_->name());
    }
    // Teardown can still wait on the backend channel; the socket is gone.
    frontendDescriptor_ = -1;
    deferredFrontendMessages_.clear();
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
        "%s vhost-user transport: in-process socketpair\n",
        device_->name());
    if (display_) {
        if (const auto port = display_->vncPort()) {
            std::fprintf(stdout, "VNC display: %s:%u (%ux%u)\n",
                vncBindAddress_.c_str(), static_cast<unsigned int>(*port),
                display_->width(), display_->height());
        } else {
            std::fprintf(stdout, "Native Metal display: %ux%u\n",
                display_->width(), display_->height());
        }
    }
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
    ::uml::device::SocketPath socketPathCleanup {socketPath_};
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
    std::fprintf(stdout, "%s vhost-user socket: %s\n",
        device_->name(), socketPath_.c_str());
    if (display_) {
        if (const auto port = display_->vncPort()) {
            std::fprintf(stdout, "VNC display: %s:%u (%ux%u)\n",
                vncBindAddress_.c_str(), static_cast<unsigned int>(*port),
                display_->width(), display_->height());
        } else {
            std::fprintf(stdout, "Native Metal display: %ux%u\n",
                display_->width(), display_->height());
        }
    }
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
        try {
            setCloseOnExec(connection.get());
            configureOutputDescriptor(connection.get());
        } catch (const std::exception& error) {
            /*
             * A peer that connects and closes again before this accept lands
             * leaves a fully shut down socket, which Darwin rejects with
             * EINVAL.  Drop that one connection and keep listening rather than
             * ending the server loop.
             */
            std::fprintf(
                stderr, "vhost-user connection rejected: %s\n", error.what());
            continue;
        }
        serveConnection(connection.release(), stopRequested);
    }
}

InProcessVhostUserService::InProcessVhostUserService(
    std::string label,
    std::string threadName,
    DeviceFactory makeDevice,
    std::shared_ptr<uml::virtio::SharedMemoryRegions> sharedMemoryRegions)
    : InProcessVhostUserService(
          std::move(label),
          std::move(threadName),
          ServerFactory([makeDevice = std::move(makeDevice)] {
              return std::make_unique<VhostUserServer>(makeDevice());
          }),
          std::move(sharedMemoryRegions))
{
}

InProcessVhostUserService::InProcessVhostUserService(
    std::string label,
    std::string threadName,
    ServerFactory makeServer,
    std::shared_ptr<uml::virtio::SharedMemoryRegions> sharedMemoryRegions)
    : label_(std::move(label))
    , threadName_(std::move(threadName))
    , makeServer_(std::move(makeServer))
    , sharedMemoryRegions_(std::move(sharedMemoryRegions))
{
    /* The service thread inherits this mask. The timer stays blocked here
     * too until the service has started, as it always has. */
    const uml::platform::ScopedGuestTimerBlock timerBlock(
        "blocking the UML timer around an in-process device thread");
    std::array<int, 2> descriptors {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) != 0) {
        throw std::runtime_error(
            "in-process " + label_ + " socketpair failed: " +
            std::string(std::strerror(errno)));
    }
    descriptors_[0].reset(descriptors[0]);
    descriptors_[1].reset(descriptors[1]);
    setCloseOnExec(descriptors_[0].get());
    setCloseOnExec(descriptors_[1].get());
#if UML_GPU_STARTUP_TESTING
    testing::startupCheckpoint(
        testing::StartupFailure::afterTransportSocketPair);
#endif
    stopDescriptor_.reset(::fcntl(descriptors_[0].get(), F_DUPFD_CLOEXEC, 0));
    if (stopDescriptor_.get() < 0) {
        throw std::runtime_error(
            "duplicating in-process " + label_ + " stop descriptor failed: " +
            std::string(std::strerror(errno)));
    }
    connectionName_ = "fd=" + std::to_string(descriptors_[1].get());
    thread_ = std::thread([this] { run(); });

    std::unique_lock lock(stateMutex_);
    stateChanged_.wait(lock, [this] { return ready_ || failure_; });
    if (failure_) {
        lock.unlock();
        thread_.join();
        std::rethrow_exception(failure_);
    }
}

InProcessVhostUserService::~InProcessVhostUserService()
{
    stop();
}

std::string InProcessVhostUserService::takeConnectionName()
{
    if (descriptors_[1].get() < 0)
        throw std::logic_error(label_ + " endpoint already transferred");
    auto name = connectionName_;
    (void)descriptors_[1].release();
    return name;
}

void InProcessVhostUserService::stop() noexcept
{
    stopRequested_.store(true, std::memory_order_release);
    if (stopDescriptor_.get() >= 0)
        (void)::shutdown(stopDescriptor_.get(), SHUT_RDWR);
    if (thread_.joinable())
        thread_.join();
}

void InProcessVhostUserService::run() noexcept
{
    (void)::pthread_setname_np(threadName_.c_str());
    try {
        const auto server = makeServer_();
        if (sharedMemoryRegions_)
            server->setSharedMemoryRegions(sharedMemoryRegions_);
        server->runConnected(
            descriptors_[0].release(), stopRequested_, [this] {
                {
                    std::lock_guard lock(stateMutex_);
                    ready_ = true;
                }
                stateChanged_.notify_one();
            });
    } catch (...) {
        {
            std::lock_guard lock(stateMutex_);
            failure_ = std::current_exception();
        }
        stateChanged_.notify_one();
    }
}

} // namespace uml::gpu
