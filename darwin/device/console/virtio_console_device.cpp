// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_console_device.hpp"

#include "protocol.hpp"

#include <cstring>
#include <stdexcept>

namespace uml::console {

VirtioConsoleDevice::VirtioConsoleDevice(
    std::unique_ptr<ConsoleChannel> channel)
    : channel_(std::move(channel))
{
    if (!channel_)
        throw std::invalid_argument("Virtio console requires a host channel");
}

VirtioConsoleDevice::~VirtioConsoleDevice() = default;

const char* VirtioConsoleDevice::name() const noexcept
{
    return "Virtio console";
}

std::uint64_t VirtioConsoleDevice::features() const noexcept
{
    return 0;
}

std::size_t VirtioConsoleDevice::queueCount() const noexcept
{
    return protocol::queueCount;
}

std::vector<std::uint8_t> VirtioConsoleDevice::configuration() const
{
    const protocol::Configuration configuration {};
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&configuration);
    return {bytes, bytes + sizeof(configuration)};
}

void VirtioConsoleDevice::writeConfiguration(
    std::uint32_t,
    std::uint32_t,
    std::span<const std::uint8_t> bytes)
{
    if (!bytes.empty())
        throw std::runtime_error("Virtio console configuration is read-only");
}

uml::virtio::VhostUserQueueResult VirtioConsoleDevice::processQueue(
    std::uint32_t index,
    std::uint64_t requestId,
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    (void)requestId;
    if (index == protocol::inputQueue) {
        if (!request.empty() || responseCapacity == 0) {
            throw std::runtime_error(
                "invalid Virtio console input descriptor");
        }
        auto response = channel_->read(responseCapacity);
        if (response.empty())
            return {};
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = std::move(response),
        };
    }
    if (index == protocol::outputQueue) {
        if (responseCapacity != 0) {
            throw std::runtime_error(
                "invalid Virtio console output descriptor");
        }
        channel_->write(request);
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = {},
        };
    }
    throw std::runtime_error("invalid Virtio console queue index");
}

void VirtioConsoleDevice::reset() noexcept
{
    channel_->reset();
}

int VirtioConsoleDevice::eventDescriptor() const noexcept
{
    return channel_->eventDescriptor();
}

void VirtioConsoleDevice::drainEventSource()
{
    channel_->drainEventSource();
}

} // namespace uml::console
