// SPDX-License-Identifier: GPL-2.0-or-later

#include "require.hpp"
#include "virtio_input_device.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>

namespace {

using uml::input::virtio::DeviceKind;
using uml::input::virtio::EventQueue;
using uml::input::virtio::VirtioInputDevice;
namespace protocol = uml::input::virtio::protocol;

using uml::testing::require;

protocol::InputConfig configuration(VirtioInputDevice& device)
{
    const auto bytes = device.configuration();
    require(bytes.size() == sizeof(protocol::InputConfig));
    protocol::InputConfig result {};
    std::memcpy(&result, bytes.data(), sizeof(result));
    return result;
}

void select(
    VirtioInputDevice& device,
    std::uint8_t selected,
    std::uint8_t subselected)
{
    device.writeConfiguration(0, 0, std::span {&selected, 1U});
    device.writeConfiguration(1, 0, std::span {&subselected, 1U});
}

bool bit(const protocol::InputConfig& config, std::uint16_t index)
{
    return (config.value.bitmap[index / 8U] & (1U << (index % 8U))) != 0;
}

void touchscreenConfigurationIsDirectWithAFixedRange()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::touchscreen, events);
    require(device.queueCount() == 2);
    require(device.features() == 0);

    select(device, protocol::configIdName, 0);
    const auto name = configuration(device);
    require(name.size == std::strlen("Kernarium Touchscreen"));
    require(std::memcmp(
                name.value.string.data(),
                "Kernarium Touchscreen",
                name.size) == 0);

    select(device, protocol::configPropertyBits, 0);
    const auto properties = configuration(device);
    require(properties.size == 1);
    require(bit(properties, protocol::propertyDirect));

    select(device, protocol::configEventBits, protocol::eventKey);
    const auto keys = configuration(device);
    require(bit(keys, protocol::buttonTouch));
    require(!bit(keys, 0x110)); // BTN_LEFT
    require(!bit(keys, 0x111)); // BTN_RIGHT
    require(!bit(keys, 0x112)); // BTN_MIDDLE
    select(device, protocol::configEventBits, 0x02); // EV_REL
    require(configuration(device).size == 0);
    select(device, protocol::configEventBits, protocol::eventAbsolute);
    const auto absoluteBits = configuration(device);
    require(bit(absoluteBits, protocol::absoluteX));
    require(bit(absoluteBits, protocol::absoluteY));

    select(device, protocol::configAbsoluteInfo, protocol::absoluteX);
    const auto x = configuration(device);
    require(x.size == sizeof(protocol::InputAbsoluteInfo));
    require(x.value.absolute.minimum == 0);
    require(x.value.absolute.maximum == protocol::touchscreenAbsoluteMaximum);
    select(device, protocol::configAbsoluteInfo, protocol::absoluteY);
    const auto y = configuration(device);
    require(y.value.absolute.minimum == 0);
    require(y.value.absolute.maximum == protocol::touchscreenAbsoluteMaximum);
}

void keyboardAdvertisesEvdevKeysAndLeds()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::keyboard, events);
    select(device, protocol::configEventBits, protocol::eventKey);
    const auto keys = configuration(device);
    require(bit(keys, 1));
    require(bit(keys, 28));
    require(bit(keys, 57));
    require(bit(keys, protocol::keyboardKeyMaximum));

    select(device, protocol::configEventBits, protocol::eventLed);
    const auto leds = configuration(device);
    require(bit(leds, 0));
    require(bit(leds, 1));
    require(bit(leds, 2));
}

void eventQueueDefersAndThenReturnsOneWireEvent()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::touchscreen, events);
    const auto empty = device.processQueue(protocol::eventQueue, 1, {}, 8);
    require(empty.action == uml::virtio::VhostUserQueueAction::retry);

    const std::array input {
        protocol::InputEvent {
            .type = protocol::eventAbsolute,
            .code = protocol::absoluteX,
            .value = 123,
        },
    };
    events->enqueue(input);
    pollfd readiness {
        .fd = device.eventDescriptor(),
        .events = POLLIN,
        .revents = 0,
    };
    require(::poll(&readiness, 1, 1000) == 1);
    device.drainEventSource();
    const auto result = device.processQueue(protocol::eventQueue, 2, {}, 8);
    require(result.action == uml::virtio::VhostUserQueueAction::complete);
    require(result.response.size() == sizeof(protocol::InputEvent));
    protocol::InputEvent returned {};
    std::memcpy(&returned, result.response.data(), sizeof(returned));
    require(returned.type == protocol::eventAbsolute);
    require(returned.code == protocol::absoluteX);
    require(returned.value == 123);
}

} // namespace

int main()
{
    touchscreenConfigurationIsDirectWithAFixedRange();
    keyboardAdvertisesEvdevKeysAndLeds();
    eventQueueDefersAndThenReturnsOneWireEvent();
    return EXIT_SUCCESS;
}
