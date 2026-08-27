// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_battery_device.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace uml::battery {
namespace {

protocol::ChargeStatus wireStatus(
    uml::platform::HostBatteryStatus status) noexcept
{
    switch (status) {
    case uml::platform::HostBatteryStatus::charging:
        return protocol::ChargeStatus::charging;
    case uml::platform::HostBatteryStatus::discharging:
        return protocol::ChargeStatus::discharging;
    case uml::platform::HostBatteryStatus::notCharging:
        return protocol::ChargeStatus::notCharging;
    case uml::platform::HostBatteryStatus::full:
        return protocol::ChargeStatus::full;
    case uml::platform::HostBatteryStatus::unknown:
        return protocol::ChargeStatus::unknown;
    }
}

template<typename Value>
Value load(std::span<const std::uint8_t> source)
{
    static_assert(std::is_trivially_copyable_v<Value>);
    Value result {};
    std::memcpy(&result, source.data(), sizeof(result));
    return result;
}

template<typename Value>
std::vector<std::uint8_t> bytes(const Value& value)
{
    static_assert(std::is_trivially_copyable_v<Value>);
    std::vector<std::uint8_t> result(sizeof(value));
    std::memcpy(result.data(), &value, sizeof(value));
    return result;
}

} // namespace

VirtioBatteryDevice::VirtioBatteryDevice(
    std::unique_ptr<uml::platform::HostBatteryProvider> provider)
    : provider_(std::move(provider))
{
    if (provider_ == nullptr)
        throw std::invalid_argument("Virtio battery provider is required");
}

const char* VirtioBatteryDevice::name() const noexcept
{
    return "Virtio Kernarium battery";
}

std::uint64_t VirtioBatteryDevice::features() const noexcept
{
    return 0;
}

std::size_t VirtioBatteryDevice::queueCount() const noexcept
{
    return protocol::queueCount;
}

std::vector<std::uint8_t> VirtioBatteryDevice::configuration() const
{
    return {};
}

void VirtioBatteryDevice::writeConfiguration(
    std::uint32_t,
    std::uint32_t,
    std::span<const std::uint8_t>)
{
}

uml::virtio::VhostUserQueueResult VirtioBatteryDevice::processQueue(
    std::uint32_t index,
    std::uint64_t,
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    if (index != protocol::statusQueue ||
        request.size() != sizeof(protocol::Request) ||
        responseCapacity < sizeof(protocol::State) ||
        load<protocol::Request>(request).version != protocol::version) {
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = {},
        };
    }
    const auto host = provider_->state();
    const protocol::State response {
        .version = protocol::version,
        .percentage = std::min<std::uint8_t>(host.percentage, 100),
        .status = wireStatus(host.status),
        .present = static_cast<std::uint8_t>(host.present),
        .externalPower = static_cast<std::uint8_t>(host.externalPower),
    };
    return {
        .action = uml::virtio::VhostUserQueueAction::complete,
        .response = bytes(response),
    };
}

void VirtioBatteryDevice::reset() noexcept
{
}

} // namespace uml::battery
