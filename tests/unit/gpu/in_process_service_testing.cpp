// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service_testing.hpp"

#include <atomic>
#include <stdexcept>

namespace uml::gpu::testing {
namespace {

std::atomic<StartupFailure> startupFailure {};

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

} // namespace uml::gpu::testing
