// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"
#include "in_process_service.hpp"
#include "in_process_service_testing.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string_view>

namespace uml::gpu::testing {
namespace {

void forceRepeatedFailure(StartupFailure failure)
{
    for (unsigned iteration = 0; iteration < 16; ++iteration) {
        const auto before = descriptorCount();
        failNextStartup(failure);
        requireThrows<std::runtime_error>([] {
            InProcessGpuService service {InProcessGpuOptions {}};
        });
        require(descriptorCount() == before);
    }
}

void endpointsTransferExactlyOnce()
{
    InProcessGpuOptions options;
    options.vncPort = 0;
    options.width = 64;
    options.height = 64;
    InProcessGpuService service {std::move(options)};
    const auto transportName = service.takeConnectionName();
    const auto inputName = service.takeInputConnectionName();
    const int transport = descriptorFromName(transportName);
    const int input = descriptorFromName(inputName);
    require(transport >= 0 && input >= 0);

    requireThrows<std::logic_error>(
        [&] { (void)service.takeConnectionName(); });
    requireThrows<std::logic_error>(
        [&] { (void)service.takeInputConnectionName(); });
    require(::close(transport) == 0);
    require(::close(input) == 0);
    service.stop();
}

void transferredEndpointsAreNotClosedByService()
{
    const auto before = descriptorCount();
    std::vector<UniqueDescriptor> replacements;
    int transport = -1;
    int input = -1;
    {
        InProcessGpuOptions options;
        options.vncPort = 0;
        options.width = 64;
        options.height = 64;
        auto service = std::make_unique<InProcessGpuService>(
            std::move(options));
        transport = descriptorFromName(service->takeConnectionName());
        input = descriptorFromName(service->takeInputConnectionName());
        require(transport != input);
        require(::close(transport) == 0);
        require(::close(input) == 0);

        bool reusedTransport = false;
        bool reusedInput = false;
        for (unsigned attempt = 0;
             attempt < 64 && (!reusedTransport || !reusedInput);
             ++attempt) {
            auto replacement = openTestDescriptor();
            reusedTransport = reusedTransport ||
                replacement.get() == transport;
            reusedInput = reusedInput || replacement.get() == input;
            replacements.push_back(std::move(replacement));
        }
        require(reusedTransport && reusedInput);
        service->stop();
        service.reset();
        for (const auto& replacement : replacements)
            require(::fcntl(replacement.get(), F_GETFD) >= 0);
    }
    replacements.clear();
    require(descriptorCount() == before);
}

void stopWithoutEndpointTransferIsBalanced()
{
    const auto before = descriptorCount();
    {
        InProcessGpuOptions options;
        options.vncPort = 0;
        options.width = 64;
        options.height = 64;
        InProcessGpuService service {std::move(options)};
        service.stop();
        service.stop();
    }
    require(descriptorCount() == before);
}

void partialFrameStopUsesOwnedWake(bool partialPayload)
{
    const auto before = descriptorCount();
    {
        InProcessGpuOptions options;
        options.vncPort = 0;
        options.width = 64;
        options.height = 64;
        InProcessGpuService service {std::move(options)};
        const int transport =
            descriptorFromName(service.takeConnectionName());
        require(transport >= 0);
        writePartialFrame(transport, partialPayload);

        service.stop();
        require(::close(transport) == 0);
    }
    require(descriptorCount() == before);
}

/*
 * The guest endpoint of the vhost-user control socket must stay blocking.
 *
 * With in-band notifications negotiated the front-end's only queue
 * notification is a synchronous VHOST_USER_VRING_KICK round trip on this
 * socket, and virtqueue_notify() latches vq->broken permanently and
 * irreversibly the moment that round trip reports a failure. A blocking
 * endpoint is what makes every remaining failure a genuine one: there is no
 * would-block to mistake for a dead backend, and a reply delayed behind GPU
 * work -- the backend answers a kick only after running that queue -- is
 * waited out rather than failed.
 */
void guestTransportEndpointBlocks()
{
    InProcessGpuOptions options;
    options.vncPort = 0;
    options.width = 64;
    options.height = 64;
    InProcessGpuService service {std::move(options)};
    const int transport = descriptorFromName(service.takeConnectionName());
    require(transport >= 0);

    const int flags = ::fcntl(transport, F_GETFL);
    require(flags >= 0);
    require((flags & O_NONBLOCK) == 0);

    service.stop();
    require(::close(transport) == 0);
}

/* A request the guest queues while the service holds its requests must stay
 * in the ring, and the held service must run it once released. The release
 * must wake a server that has nothing else to do. `socket` is the guest end of
 * the connection the request travels on. */
void heldRequestRunsAfterRelease(InProcessGpuService& service, int socket)
{
    const SplitRing ring;
    publishDisplayInfoRequest(ring);
    constexpr std::uint8_t sentinel = 0x5a;
    std::memset(ring.memory().data() + displayInfoResponseOffset, sentinel,
        sizeof(protocol::GpuDisplayInfo));
    const auto responseType = [&] {
        protocol::GpuControlHeader response {};
        std::memcpy(
            &response,
            ring.memory().data() + displayInfoResponseOffset,
            sizeof(response));
        return response.type;
    };

    negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
    configureMemory(socket, ring.descriptor());
    const auto channels = startQueue(socket, ring);
    writeKick(channels.kick);
    pollfd call {.fd = channels.call.readEnd.get(), .events = POLLIN};
    require(::poll(&call, 1, 200) == 0);
    require(ring.usedIndex() == 0);
    require(ring.memory()[displayInfoResponseOffset] == sentinel);

    service.releaseRequests();
    awaitCall(channels.call.readEnd.get());
    require(ring.usedIndex() == 1);
    require(responseType() == static_cast<std::uint32_t>(
        protocol::GpuResponse::okDisplayInfo));
}

/* The application holds the GPU's requests before it enters the background,
 * through the service it created. */
void holdRequestsHoldsTheServicesRequests()
{
    InProcessGpuOptions options;
    options.vncPort = 0;
    options.width = 64;
    options.height = 64;
    InProcessGpuService service {std::move(options)};
    UniqueDescriptor transport {descriptorFromName(service.takeConnectionName())};
    UniqueDescriptor input {descriptorFromName(service.takeInputConnectionName())};
    require(service.holdRequests(std::chrono::milliseconds {100}));

    heldRequestRunsAfterRelease(service, transport.get());

    /* Holding again, with nothing running, is quiet and holds the next one. */
    require(service.holdRequests(std::chrono::milliseconds {100}));
    service.releaseRequests();
    service.stop();
}

/* A guest that restarts itself is a new guest on the same machine. The next
 * connection serves it like the first, the old guest's endpoints are the
 * guest's to close, and a hold the application placed before the restart is
 * still in place, because the new device starts closed to the guest. */
void nextConnectionServesTheNextGuest()
{
    const auto before = descriptorCount();
    {
        InProcessGpuOptions options;
        options.vncPort = 0;
        options.width = 64;
        options.height = 64;
        InProcessGpuService service {std::move(options)};
        {
            UniqueDescriptor transport {
                descriptorFromName(service.takeConnectionName())};
            UniqueDescriptor input {
                descriptorFromName(service.takeInputConnectionName())};
            require(service.holdRequests(std::chrono::milliseconds {100}));
            heldRequestRunsAfterRelease(service, transport.get());
            require(service.holdRequests(std::chrono::milliseconds {100}));
            /* The old guest goes: its endpoints close here. */
        }
        service.startNextConnection();
        require(service.requestsHeld());

        UniqueDescriptor transport {
            descriptorFromName(service.takeConnectionName())};
        UniqueDescriptor input {
            descriptorFromName(service.takeInputConnectionName())};
        requireThrows<std::logic_error>(
            [&] { (void)service.takeConnectionName(); });
        requireThrows<std::logic_error>(
            [&] { (void)service.takeInputConnectionName(); });
        heldRequestRunsAfterRelease(service, transport.get());
        require(!service.requestsHeld());

        /* And again: the connection after that one is as good. */
        transport = {};
        input = {};
        service.startNextConnection();
        require(!service.requestsHeld());
        UniqueDescriptor last {
            descriptorFromName(service.takeConnectionName())};
        UniqueDescriptor lastInput {
            descriptorFromName(service.takeInputConnectionName())};
        require(service.holdRequests(std::chrono::milliseconds {100}));
        heldRequestRunsAfterRelease(service, last.get());
        service.stop();
    }
    require(descriptorCount() == before);
}

/* A connection that cannot be opened leaves the service as it says it does.
 * One that fails before the old connection is touched leaves that connection
 * in place; one that fails after leaves none, with its descriptors given
 * back, and the call can be made again. */
void failedNextConnectionLeavesTheServiceRecoverable()
{
    const auto before = descriptorCount();
    {
        InProcessGpuOptions options;
        options.vncPort = 0;
        options.width = 64;
        options.height = 64;
        InProcessGpuService service {std::move(options)};

        failNextStartup(StartupFailure::afterInputSocketPair);
        requireThrows<std::runtime_error>(
            [&] { service.startNextConnection(); });
        /* The connection that was there is untouched, endpoints included. */
        UniqueDescriptor transport {
            descriptorFromName(service.takeConnectionName())};
        UniqueDescriptor input {
            descriptorFromName(service.takeInputConnectionName())};
        require(::fcntl(transport.get(), F_GETFD) >= 0);

        for (const auto failure :
             {StartupFailure::afterTransportSocketPair,
              StartupFailure::workerEntry}) {
            failNextStartup(failure);
            requireThrows<std::runtime_error>(
                [&] { service.startNextConnection(); });
            requireThrows<std::logic_error>(
                [&] { (void)service.takeConnectionName(); });
            /* And it says why: there is no connection, not that its endpoint
             * went already. */
            try {
                (void)service.takeInputConnectionName();
                require(false, "a service without a connection has an input");
            } catch (const std::logic_error& error) {
                require(
                    std::string_view {error.what()}.find("no connection") !=
                        std::string_view::npos,
                    "the error does not say there is no connection");
            }
        }
        transport = {};
        input = {};

        service.startNextConnection();
        transport = UniqueDescriptor {
            descriptorFromName(service.takeConnectionName())};
        input = UniqueDescriptor {
            descriptorFromName(service.takeInputConnectionName())};
        require(service.holdRequests(std::chrono::milliseconds {100}));
        heldRequestRunsAfterRelease(service, transport.get());
        service.stop();
        /* A stopped service has no display to carry on. */
        requireThrows<std::logic_error>(
            [&] { service.startNextConnection(); });
    }
    require(descriptorCount() == before);
}

} // namespace

void runInProcessServiceTests()
{
    holdRequestsHoldsTheServicesRequests();
    nextConnectionServesTheNextGuest();
    failedNextConnectionLeavesTheServiceRecoverable();
    guestTransportEndpointBlocks();
    forceRepeatedFailure(StartupFailure::afterTransportSocketPair);
    forceRepeatedFailure(StartupFailure::afterInputSocketPair);
    forceRepeatedFailure(StartupFailure::workerEntry);
    endpointsTransferExactlyOnce();
    transferredEndpointsAreNotClosedByService();
    stopWithoutEndpointTransferIsBalanced();
    partialFrameStopUsesOwnedWake(false);
    partialFrameStopUsesOwnedWake(true);
}

} // namespace uml::gpu::testing
