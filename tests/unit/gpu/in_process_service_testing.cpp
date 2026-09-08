// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service_testing.hpp"
#include "vhost_user_device.hpp"
#include "vhost_user_server.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace uml::gpu::testing {
namespace {

std::atomic<StartupFailure> startupFailure {};

class TransportTestDevice final : public uml::virtio::VhostUserDevice {
public:
    [[nodiscard]] const char* name() const noexcept override
    {
        return "transport-test";
    }

    [[nodiscard]] std::uint64_t features() const noexcept override
    {
        return 0;
    }

    [[nodiscard]] std::size_t queueCount() const noexcept override
    {
        return 2;
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

    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t,
        std::uint64_t,
        std::span<const std::uint8_t>,
        std::size_t responseCapacity) override
    {
        std::vector<std::uint8_t> response;
        if (responseCapacity != 0)
            response.push_back(0);
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = std::move(response),
        };
    }

    void reset() noexcept override {}
};

} // namespace

void failNextStartup(const StartupFailure failure) noexcept
{
    startupFailure.store(failure, std::memory_order_release);
}

void startupCheckpoint(const StartupFailure failure)
{
    auto expected = failure;
    if (startupFailure.compare_exchange_strong(
            expected,
            StartupFailure::none,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        throw std::runtime_error("forced in-process GPU startup failure");
    }
}

std::unique_ptr<VhostUserServer> makeTransportTestServer()
{
    return std::make_unique<VhostUserServer>(
        std::make_unique<TransportTestDevice>());
}

} // namespace uml::gpu::testing
