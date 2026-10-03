// SPDX-License-Identifier: GPL-2.0-or-later

#include "allocation_failure.hpp"
#include "gpu_test_support.hpp"
#include "vhost_user_message.hpp"
#include "vhost_user_message_testing.hpp"

#include <cstdio>
#include <stdexcept>
#include <thread>

namespace uml::gpu::testing {
namespace {

template<typename Operation>
void requireNoDescriptorLeak(Operation operation)
{
    const auto before = descriptorCount();
    operation();
    const auto after = descriptorCount();
    if (after != before)
        std::fprintf(stderr, "descriptor imbalance: %zu -> %zu\n", before, after);
    require(after == before);
}

void malformedVersionDoesNotLeak()
{
    requireNoDescriptorLeak([] {
        auto sockets = makeSocketPair();
        auto sent = openTestDescriptor();
        const protocol::VhostHeader header {
            .request = 1,
            .flags = 2,
            .size = 0,
        };
        const std::array descriptors {sent.get()};
        sendMessageWithDescriptors(
            sockets.descriptors[0].get(), header, descriptors);
        VhostUserMessage message;
        bool failed = false;
        try {
            (void)receiveVhostUserMessage(
                sockets.descriptors[1].get(), message);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed);
    });
}

void oversizedMessageDoesNotLeak()
{
    requireNoDescriptorLeak([] {
        auto sockets = makeSocketPair();
        auto sent = openTestDescriptor();
        const protocol::VhostHeader header {
            .request = 1,
            .flags = protocol::vhostUserVersion,
            .size = static_cast<std::uint32_t>(
                maximumVhostUserMessagePayload + 1U),
        };
        const std::array descriptors {sent.get()};
        sendMessageWithDescriptors(
            sockets.descriptors[0].get(), header, descriptors);
        VhostUserMessage message;
        bool failed = false;
        try {
            (void)receiveVhostUserMessage(
                sockets.descriptors[1].get(), message);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed);
    });
}

void truncatedFramesDoNotLeak()
{
    for (const bool partialPayload : {false, true}) {
        requireNoDescriptorLeak([partialPayload] {
            auto sockets = makeSocketPair();
            auto sent = openTestDescriptor();
            const protocol::VhostHeader header {
                .request = 1,
                .flags = protocol::vhostUserVersion,
                .size = partialPayload ? 2U : 0U,
            };
            const std::array descriptors {sent.get()};
            if (partialPayload) {
                constexpr std::uint8_t payload = 0;
                sendMessageWithDescriptors(
                    sockets.descriptors[0].get(),
                    header,
                    descriptors,
                    std::span {&payload, 1U});
            } else {
                sendBytesWithDescriptors(
                    sockets.descriptors[0].get(),
                    &header,
                    sizeof(header) / 2U,
                    descriptors);
            }
            require(::shutdown(
                sockets.descriptors[0].get(), SHUT_WR) == 0);
            VhostUserMessage message;
            require(!receiveVhostUserMessage(
                sockets.descriptors[1].get(), message));
        });
    }
}

void allocationFailureDoesNotLeak()
{
    requireNoDescriptorLeak([] {
        auto sockets = makeSocketPair();
        auto sent = openTestDescriptor();
        const protocol::VhostHeader header {
            .request = 1,
            .flags = protocol::vhostUserVersion,
            .size = 1,
        };
        constexpr std::uint8_t payload = 0;
        const std::array descriptors {sent.get()};
        sendMessageWithDescriptors(
            sockets.descriptors[0].get(),
            header,
            descriptors,
            std::span {&payload, 1U});
        VhostUserMessage message;
        ::uml::testing::fail_next_allocation();
        bool failed = false;
        try {
            (void)receiveVhostUserMessage(
                sockets.descriptors[1].get(), message);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        (void)::uml::testing::cancel_pending_allocation_failure();
        require(failed);
    });
}

void validTransferHasSingleOwner()
{
    requireNoDescriptorLeak([] {
        auto sockets = makeSocketPair();
        auto sent = openTestDescriptor();
        const protocol::VhostHeader header {
            .request = static_cast<std::uint32_t>(
                protocol::VhostRequest::setSlaveRequestFd),
            .flags = protocol::vhostUserVersion,
            .size = 0,
        };
        const std::array descriptors {sent.get()};
        sendMessageWithDescriptors(
            sockets.descriptors[0].get(), header, descriptors);
        UniqueDescriptor transferred;
        {
            VhostUserMessage message;
            require(receiveVhostUserMessage(
                sockets.descriptors[1].get(), message));
            require(message.descriptors.size() == 1);
            require(
                (::fcntl(message.descriptors[0].get(), F_GETFD) & FD_CLOEXEC) !=
                0);
            transferred = std::move(message.descriptors[0]);
        }
        require(::fcntl(transferred.get(), F_GETFD) >= 0);
    });
}

void fragmentedHeaderAdoptsLaterDescriptors()
{
    requireNoDescriptorLeak([] {
        auto sockets = makeSocketPair();
        auto sent = openTestDescriptor();
        const protocol::VhostHeader header {
            .request = static_cast<std::uint32_t>(
                protocol::VhostRequest::setSlaveRequestFd),
            .flags = protocol::vhostUserVersion,
            .size = 0,
        };
        constexpr std::size_t firstFragment = sizeof(std::uint32_t);
        require(::write(
            sockets.descriptors[0].get(),
            &header,
            firstFragment) == static_cast<ssize_t>(firstFragment));

        VhostUserMessage message;
        bool received = false;
        testing::resetStopWaitObserver();
        std::thread receiver {[&] {
            received = receiveVhostUserMessage(
                sockets.descriptors[1].get(), message);
        }};
        // The receiver waits for more only once it has consumed the first
        // fragment, so the rest arrives in a fragment of its own.
        const bool waiting = testing::waitForStopWaitCount(1, 2000);
        const std::array descriptors {sent.get()};
        sendBytesWithDescriptors(
            sockets.descriptors[0].get(),
            reinterpret_cast<const std::uint8_t*>(&header) + firstFragment,
            sizeof(header) - firstFragment,
            descriptors);
        receiver.join();
        require(waiting);
        require(received);
        require(message.descriptors.size() == 1);
        require(
            (::fcntl(message.descriptors[0].get(), F_GETFD) & FD_CLOEXEC) != 0);
    });
}

void descriptorLimitIsCoherent()
{
    requireNoDescriptorLeak([] {
        auto sockets = makeSocketPair();
        std::array<UniqueDescriptor, maximumVhostUserDescriptors> owned;
        std::array<int, maximumVhostUserDescriptors> raw {};
        for (std::size_t index = 0; index < owned.size(); ++index) {
            owned[index] = openTestDescriptor();
            raw[index] = owned[index].get();
        }
        const protocol::VhostHeader header {
            .request = 1,
            .flags = protocol::vhostUserVersion,
            .size = 0,
        };
        sendMessageWithDescriptors(
            sockets.descriptors[0].get(), header, raw);
        VhostUserMessage message;
        require(receiveVhostUserMessage(
            sockets.descriptors[1].get(), message));
        require(message.descriptors.size() == maximumVhostUserDescriptors);
    });

    requireNoDescriptorLeak([] {
        auto sockets = makeSocketPair();
        std::array<UniqueDescriptor, maximumVhostUserDescriptors + 1U> owned;
        std::array<int, maximumVhostUserDescriptors + 1U> raw {};
        for (std::size_t index = 0; index < owned.size(); ++index) {
            owned[index] = openTestDescriptor();
            raw[index] = owned[index].get();
        }
        const protocol::VhostHeader header {
            .request = 1,
            .flags = protocol::vhostUserVersion,
            .size = 0,
        };
        sendMessageWithDescriptors(
            sockets.descriptors[0].get(), header, raw);
        VhostUserMessage message;
        bool failed = false;
        try {
            (void)receiveVhostUserMessage(
                sockets.descriptors[1].get(), message);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed);
    });
}

} // namespace

} // namespace uml::gpu::testing

int main()
{
    using namespace uml::gpu::testing;

    for (unsigned iteration = 0; iteration < 8; ++iteration) {
        malformedVersionDoesNotLeak();
        oversizedMessageDoesNotLeak();
        truncatedFramesDoNotLeak();
        allocationFailureDoesNotLeak();
        validTransferHasSingleOwner();
    }
    fragmentedHeaderAdoptsLaterDescriptors();
    descriptorLimitIsCoherent();
    std::puts("GPU transport wire ownership tests passed");
    return 0;
}
