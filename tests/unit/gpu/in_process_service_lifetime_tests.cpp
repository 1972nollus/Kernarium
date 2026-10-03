// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"
#include "in_process_service.hpp"
#include "in_process_service_testing.hpp"

#include <fcntl.h>
#include <memory>
#include <stdexcept>

namespace uml::gpu::testing {
namespace {

void forceRepeatedFailure(StartupFailure failure)
{
    for (unsigned iteration = 0; iteration < 16; ++iteration) {
        const auto before = descriptorCount();
        failNextStartup(failure);
        bool failed = false;
        try {
            InProcessGpuService service {InProcessGpuOptions {}};
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed);
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

    bool transportRejected = false;
    bool inputRejected = false;
    try {
        (void)service.takeConnectionName();
    } catch (const std::logic_error&) {
        transportRejected = true;
    }
    try {
        (void)service.takeInputConnectionName();
    } catch (const std::logic_error&) {
        inputRejected = true;
    }
    require(transportRejected && inputRejected);
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

} // namespace

void runInProcessServiceTests()
{
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
