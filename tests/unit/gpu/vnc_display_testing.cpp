// SPDX-License-Identifier: GPL-2.0-or-later

#include "vnc_display_testing.hpp"

#include "vnc_display.hpp"

#include <atomic>
#include <stdexcept>

namespace uml::gpu::testing {
namespace {

std::atomic_bool failEventThreadStart {};
std::atomic<std::uint16_t> failedPort {};

} // namespace

void failNextEventThreadStart() noexcept
{
    failedPort.store(0, std::memory_order_relaxed);
    failEventThreadStart.store(true, std::memory_order_release);
}

void eventThreadStartCheckpoint(const std::uint16_t port)
{
    if (!failEventThreadStart.exchange(false, std::memory_order_acq_rel))
        return;
    failedPort.store(port, std::memory_order_release);
    throw std::runtime_error("forced VNC event-thread startup failure");
}

std::uint16_t failedEventThreadPort() noexcept
{
    return failedPort.load(std::memory_order_acquire);
}

} // namespace uml::gpu::testing
