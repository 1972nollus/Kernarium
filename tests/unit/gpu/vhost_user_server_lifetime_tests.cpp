// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace uml::gpu::testing {
namespace {

[[nodiscard]] pid_t launchChild(const char* executable, const char* mode)
{
    const pid_t child = ::fork();
    require(child >= 0);
    if (child == 0) {
        ::execl(executable, executable, mode, nullptr);
        std::_Exit(127);
    }
    return child;
}

void waitForConnectionClose(int socket)
{
    pollfd descriptor {.fd = socket, .events = POLLIN | POLLHUP};
    require(::poll(&descriptor, 1, 2000) == 1);
    std::uint8_t byte {};
    require(::read(socket, &byte, sizeof(byte)) == 0);
}

void verifyControlRetry(VhostUserServer& server)
{
    withServerConnection(server, [](int socket) {
        sendRequest(
            socket,
            protocol::VhostRequest::getFeatures,
            {},
            {},
            false);
        protocol::VhostHeader reply {};
        std::uint64_t features {};
        readExact(socket, &reply, sizeof(reply));
        readExact(socket, &features, sizeof(features));
        require(
            reply.request == static_cast<std::uint32_t>(
                protocol::VhostRequest::getFeatures));
        require(
            reply.flags ==
            (protocol::vhostUserVersion | protocol::vhostUserReply));
        require(reply.size == sizeof(features));
        require(features != 0);
    });
}

[[noreturn]] void runPartialFrameChild(
    bool partialPayload,
    bool carryDescriptor)
{
    int exitStatus = EXIT_SUCCESS;
    {
        VhostUserServer server {
            {}, "127.0.0.1", 0, {}, 64, 64, false};
        // Compare against the live server's stable ownership. LibVNCServer and
        // the renderer may retain process-lifetime runtime descriptors after
        // first initialization, which are unrelated to a transport frame.
        const auto before = descriptorCount();
        {
            auto sockets = makeSocketPair();
            auto sent = carryDescriptor ?
                openTestDescriptor() : UniqueDescriptor {};
            std::atomic_bool stopRequested {};
            std::atomic_bool ready {};
            std::atomic_bool finished {};
            std::thread worker {[&] {
                server.runConnected(
                    sockets.descriptors[1].release(),
                    stopRequested,
                    [&] { ready.store(true, std::memory_order_release); });
                finished.store(true, std::memory_order_release);
            }};
            while (!ready.load(std::memory_order_acquire))
                std::this_thread::yield();

            const protocol::VhostHeader header {
                .request = static_cast<std::uint32_t>(
                    protocol::VhostRequest::setFeatures),
                .flags = protocol::vhostUserVersion,
                .size = partialPayload ?
                    static_cast<std::uint32_t>(sizeof(std::uint64_t)) : 0U,
            };
            const std::array descriptors {sent.get()};
            const std::span<const int> sentDescriptors = carryDescriptor ?
                std::span<const int> {descriptors} : std::span<const int> {};
            if (partialPayload) {
                std::array<
                    std::uint8_t,
                    sizeof(header) + sizeof(std::uint32_t)> fragment {};
                std::memcpy(fragment.data(), &header, sizeof(header));
                sendBytesWithDescriptors(
                    sockets.descriptors[0].get(),
                    fragment.data(),
                    fragment.size(),
                    sentDescriptors);
            } else {
                sendBytesWithDescriptors(
                    sockets.descriptors[0].get(),
                    &header,
                    sizeof(header) / 2U,
                    sentDescriptors);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds {30});
            stopRequested.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds {2};
            while (!finished.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds {10});
            }
            if (!finished.load(std::memory_order_acquire))
                std::_Exit(101);
            worker.join();
        }
        verifyControlRetry(server);
        const auto after = descriptorCount();
        if (after != before) {
            std::fprintf(
                stderr,
                "partial-frame child descriptor imbalance: %zu -> %zu\n",
                before,
                after);
            exitStatus = 102;
        }
    }
    std::_Exit(exitStatus);
}

void partialFrameStopIsPrompt(
    const char* executable,
    bool partialPayload,
    bool carryDescriptor)
{
    const auto before = descriptorCount();
    const char* mode = partialPayload ?
        (carryDescriptor ? "--partial-payload-fd-child"
                         : "--partial-payload-child") :
        (carryDescriptor ? "--partial-header-fd-child"
                         : "--partial-header-child");
    const pid_t child = launchChild(executable, mode);

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds {10};
    int status = 0;
    for (;;) {
        const auto result = ::waitpid(child, &status, WNOHANG);
        if (result < 0 && errno == EINTR)
            continue;
        require(result >= 0);
        if (result == child)
            break;
        if (std::chrono::steady_clock::now() >= deadline) {
            require(::kill(child, SIGKILL) == 0);
            require(::waitpid(child, &status, 0) == child);
            require(false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
    }
    require(WIFEXITED(status));
    if (WEXITSTATUS(status) != EXIT_SUCCESS) {
        std::fprintf(
            stderr,
            "partial-frame child exited with status %d\n",
            WEXITSTATUS(status));
    }
    require(WEXITSTATUS(status) == EXIT_SUCCESS);
    require(descriptorCount() == before);
}

void unsupportedRequestReclaimsDescriptor()
{
    const auto before = descriptorCount();
    {
        VhostUserServer server {
            {}, "127.0.0.1", 0, {}, 64, 64, false};
        withServerConnection(server, [](int socket) {
            auto sent = openTestDescriptor();
            const protocol::VhostHeader header {
                .request = 0xffff'ffffU,
                .flags = protocol::vhostUserVersion,
                .size = 0,
            };
            const std::array descriptors {sent.get()};
            sendMessageWithDescriptors(socket, header, descriptors);
            waitForConnectionClose(socket);
        });
    }
    require(descriptorCount() == before);
}

void configureCallDescriptor(int socket, int descriptor)
{
    constexpr std::uint64_t queue = 0;
    const std::array descriptors {descriptor};
    sendRequestAndReadAcknowledgement(
        socket,
        protocol::VhostRequest::setVringCall,
        bytesOf(queue),
        descriptors);
}

void memoryReplacementReleasesQueueDescriptors()
{
    const auto before = descriptorCount();
    {
        VhostUserServer server {
            {}, "127.0.0.1", 0, {}, 64, 64, false};
        auto memory = openMemoryDescriptor();
        std::size_t stableConfiguredCount = 0;
        withServerConnection(server, [&](int socket) {
            for (unsigned iteration = 0; iteration < 16; ++iteration) {
                configureMemory(socket, memory.get());
                std::array<int, 2> pipeDescriptors {-1, -1};
                require(::pipe(pipeDescriptors.data()) == 0);
                UniqueDescriptor readEnd {pipeDescriptors[0]};
                UniqueDescriptor writeEnd {pipeDescriptors[1]};
                configureCallDescriptor(socket, writeEnd.get());
                readEnd.reset();
                writeEnd.reset();
                const auto configuredCount = descriptorCount();
                if (iteration == 0)
                    stableConfiguredCount = configuredCount;
                require(configuredCount == stableConfiguredCount);
            }
        });
    }
    require(descriptorCount() == before);
}

void failedMemoryReplacementIsRetryable(
    std::uint64_t size,
    std::uint64_t mmapOffset,
    std::size_t backingSize = 4096)
{
    const auto before = descriptorCount();
    {
        VhostUserServer server {
            {}, "127.0.0.1", 0, {}, 64, 64, false};
        auto memory = openMemoryDescriptor(backingSize);
        withServerConnection(server, [&](int socket) {
            configureMemory(socket, memory.get());
            const protocol::MemoryRegion invalidRegion {
                .guestAddress = 0x1000,
                .size = size,
                .userAddress = 0x1000'0000,
                .mmapOffset = mmapOffset,
            };
            const auto payload =
                memoryTablePayload(std::span {&invalidRegion, 1U});
            const std::array descriptors {memory.get()};
            sendRequest(
                socket,
                protocol::VhostRequest::setMemTable,
                payload,
                descriptors);
            waitForConnectionClose(socket);
        });
        withServerConnection(server, [&](int socket) {
            configureMemory(socket, memory.get(), backingSize);
        });
    }
    require(descriptorCount() == before);
}

void twoRegionFailureRollsBackEveryOwner()
{
    const auto before = descriptorCount();
    {
        VhostUserServer server {
            {}, "127.0.0.1", 0, {}, 64, 64, false};
        auto first = openMemoryDescriptor();
        auto shortSecond = openMemoryDescriptor(1024);
        const std::array regions {
            protocol::MemoryRegion {
                .guestAddress = 0x1000,
                .size = 4096,
                .userAddress = 0x1000'0000,
                .mmapOffset = 0,
            },
            protocol::MemoryRegion {
                .guestAddress = 0x3000,
                .size = 4096,
                .userAddress = 0x1000'2000,
                .mmapOffset = 0,
            },
        };
        const auto payload = memoryTablePayload(regions);
        const std::array descriptors {first.get(), shortSecond.get()};
        withServerConnection(server, [&](int socket) {
            sendRequest(
                socket,
                protocol::VhostRequest::setMemTable,
                payload,
                descriptors);
            waitForConnectionClose(socket);
        });
        withServerConnection(server, [&](int socket) {
            configureMemory(socket, first.get());
        });
    }
    require(descriptorCount() == before);
}

[[nodiscard]] std::filesystem::path temporarySocketPath()
{
    char path[] = "/tmp/uml-gpu-socket.XXXXXX";
    UniqueDescriptor descriptor {::mkstemp(path)};
    require(descriptor.get() >= 0);
    descriptor.reset();
    require(::unlink(path) == 0);
    return path;
}

void standaloneRunCanBeRetriedWithoutLeaking()
{
    const auto path = temporarySocketPath();
    VhostUserServer server {
        path, "127.0.0.1", 0, {}, 64, 64, false};
    const auto before = descriptorCount();
    std::atomic_bool alreadyStopped {true};
    server.run(alreadyStopped);
    require(descriptorCount() == before);
    require(!std::filesystem::exists(path));
    server.run(alreadyStopped);
    require(descriptorCount() == before);
    require(!std::filesystem::exists(path));
}

void standaloneRunDoesNotReplaceExistingPath()
{
    char rawPath[] = "/tmp/uml-gpu-owned-path.XXXXXX";
    UniqueDescriptor sentinel {::mkstemp(rawPath)};
    require(sentinel.get() >= 0);
    constexpr std::uint8_t marker = 0x5a;
    require(::write(sentinel.get(), &marker, sizeof(marker)) ==
        static_cast<ssize_t>(sizeof(marker)));
    const std::filesystem::path path {rawPath};
    VhostUserServer server {
        path, "127.0.0.1", 0, {}, 64, 64, false};
    std::atomic_bool alreadyStopped {true};
    bool rejected = false;
    try {
        server.run(alreadyStopped);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected);
    struct stat status {};
    require(::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode));
    std::uint8_t observed {};
    require(::pread(sentinel.get(), &observed, sizeof(observed), 0) ==
            static_cast<ssize_t>(sizeof(observed)) &&
        observed == marker);
    sentinel.reset();
    require(::unlink(path.c_str()) == 0);
}

void concurrentRendererConstructionFailsAndLaterRetries()
{
    const auto before = descriptorCount();
    {
        VhostUserServer first {
            {}, "127.0.0.1", 0, {}, 64, 64, false};
        bool rejected = false;
        try {
            VhostUserServer second {
                {}, "127.0.0.1", 0, {}, 64, 64, false};
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected);
    }
    require(descriptorCount() == before);
    {
        VhostUserServer retry {
            {}, "127.0.0.1", 0, {}, 64, 64, false};
    }
    require(descriptorCount() == before);
}

void configureQueueForOneRequest(
    int socket,
    std::span<std::uint8_t> memory,
    int callDescriptor)
{
    constexpr std::uint64_t guestBase = 0x1000;
    constexpr std::uint64_t userBase = 0x1000'0000;
    constexpr std::size_t descriptorOffset = 0x000;
    constexpr std::size_t availableOffset = 0x100;
    constexpr std::size_t usedOffset = 0x200;
    constexpr std::size_t requestOffset = 0x300;
    constexpr std::size_t responseOffset = 0x400;

    std::array<protocol::VirtqueueDescriptor, 2> descriptors {
        protocol::VirtqueueDescriptor {
            .address = guestBase + requestOffset,
            .length = sizeof(protocol::GpuControlHeader),
            .flags = protocol::descriptorNext,
            .next = 1,
        },
        protocol::VirtqueueDescriptor {
            .address = guestBase + responseOffset,
            .length = sizeof(protocol::GpuDisplayInfo),
            .flags = protocol::descriptorWrite,
            .next = 0,
        },
    };
    std::memcpy(
        memory.data() + descriptorOffset,
        descriptors.data(),
        sizeof(descriptors));
    const std::uint16_t availableIndex = 1;
    const std::uint16_t head = 0;
    std::memcpy(
        memory.data() + availableOffset + sizeof(std::uint16_t),
        &availableIndex,
        sizeof(availableIndex));
    std::memcpy(
        memory.data() + availableOffset + protocol::splitRingHeaderSize,
        &head,
        sizeof(head));
    protocol::GpuControlHeader request {};
    request.type = static_cast<std::uint32_t>(protocol::GpuCommand::getDisplayInfo);
    std::memcpy(memory.data() + requestOffset, &request, sizeof(request));

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
    configureCallDescriptor(socket, callDescriptor);
}

[[noreturn]] void runClosedNotificationDescriptorChild()
{
    (void)::signal(SIGPIPE, SIG_DFL);
    VhostUserServer server {
        {}, "127.0.0.1", 0, {}, 64, 64, false};
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
    withServerConnection(server, [&](int socket) {
        constexpr std::uint64_t deviceFeatures =
            protocol::vhostFeatureProtocol;
        sendRequest(
            socket,
            protocol::VhostRequest::setFeatures,
            bytesOf(deviceFeatures),
            {},
            false);
        constexpr std::uint64_t protocolFeatures =
            protocol::protocolFeatureInbandNotifications |
            protocol::protocolFeatureSlaveRequest |
            protocol::protocolFeatureReplyAck;
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setProtocolFeatures,
            bytesOf(protocolFeatures));
        configureMemory(socket, memoryDescriptor.get());
        std::array<int, 2> pipeDescriptors {-1, -1};
        require(::pipe(pipeDescriptors.data()) == 0);
        UniqueDescriptor readEnd {pipeDescriptors[0]};
        UniqueDescriptor writeEnd {pipeDescriptors[1]};
        configureQueueForOneRequest(socket, memory, writeEnd.get());
        readEnd.reset();
        writeEnd.reset();
        const protocol::VringState kick {.index = 0, .value = 0};
        sendRequest(
            socket,
            protocol::VhostRequest::vringKick,
            bytesOf(kick));
        waitForConnectionClose(socket);
    });
    require(::munmap(mapped, 4096) == 0);
    std::_Exit(EXIT_SUCCESS);
}

void closedNotificationDescriptorDoesNotSignalProcess(const char* executable)
{
    const pid_t child = launchChild(
        executable, "--closed-notification-child");

    int status = 0;
    require(::waitpid(child, &status, 0) == child);
    require(WIFEXITED(status));
    require(WEXITSTATUS(status) == EXIT_SUCCESS);
}

} // namespace

[[noreturn]] void runVhostUserServerChild(std::string_view mode)
{
    if (mode == "--closed-notification-child")
        runClosedNotificationDescriptorChild();
    if (mode == "--partial-header-child")
        runPartialFrameChild(false, false);
    if (mode == "--partial-header-fd-child")
        runPartialFrameChild(false, true);
    if (mode == "--partial-payload-child")
        runPartialFrameChild(true, false);
    if (mode == "--partial-payload-fd-child")
        runPartialFrameChild(true, true);
    std::_Exit(126);
}

void runVhostUserServerTests(const char* executable)
{
    // Metal's compiler service is not a supported post-fork initialization
    // path. Exec each isolated child before it constructs the production
    // renderer, preserving process-signal/watchdog coverage without inheriting
    // framework or dispatch state.
    closedNotificationDescriptorDoesNotSignalProcess(executable);
    partialFrameStopIsPrompt(executable, false, false);
    partialFrameStopIsPrompt(executable, true, false);
    partialFrameStopIsPrompt(executable, false, true);
    partialFrameStopIsPrompt(executable, true, true);
    {
        VhostUserServer warmRuntime {
            {}, "127.0.0.1", 0, {}, 64, 64, false};
    }
    concurrentRendererConstructionFailsAndLaterRetries();
    unsupportedRequestReclaimsDescriptor();
    memoryReplacementReleasesQueueDescriptors();
    failedMemoryReplacementIsRetryable(0, 0);
    failedMemoryReplacementIsRetryable(4096, UINT64_MAX);
    failedMemoryReplacementIsRetryable(8192, 0);
    twoRegionFailureRollsBackEveryOwner();
    standaloneRunCanBeRetriedWithoutLeaking();
    standaloneRunDoesNotReplaceExistingPath();
}

} // namespace uml::gpu::testing
