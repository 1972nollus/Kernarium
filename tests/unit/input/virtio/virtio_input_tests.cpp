// SPDX-License-Identifier: GPL-2.0-or-later

#include "require.hpp"
#include "virtio_input_device.hpp"
#include "wire_bytes.hpp"

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
    return uml::testing::loadExact<protocol::InputConfig>(
        device.configuration());
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

// Android's Generic key layout maps KEY_BACK and KEY_HOMEPAGE to BACK and
// HOME, and the guest's input core delivers only advertised keys.
void keyboardAdvertisesBackAndHomeAndNothingElseAbove127()
{
    require(protocol::keyBack == 158);
    require(protocol::keyHomepage == 172);

    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::keyboard, events);
    select(device, protocol::configEventBits, protocol::eventKey);
    const auto keys = configuration(device);
    require(bit(keys, protocol::keyBack));
    require(bit(keys, protocol::keyHomepage));
    // The map ends at the highest advertised key.
    require(keys.size == protocol::keyHomepage / 8U + 1U);

    constexpr std::uint16_t bitmapBits = 128U * 8U;
    for (std::uint16_t code = 0; code < bitmapBits; ++code) {
        const bool expected = (code >= 1U && code <= 127U) ||
            code == 158U || code == 172U;
        require(bit(keys, code) == expected);
        require(protocol::keyboardAdvertisesKey(code) == expected);
    }
    // A key beyond the bitmap is never advertised either.
    require(!protocol::keyboardAdvertisesKey(bitmapBits));
    require(!protocol::keyboardAdvertisesKey(0xffff));

    // The touchscreen's key map stays BTN_TOUCH alone.
    VirtioInputDevice touchscreen(DeviceKind::touchscreen, events);
    select(touchscreen, protocol::configEventBits, protocol::eventKey);
    const auto touchKeys = configuration(touchscreen);
    require(!bit(touchKeys, protocol::keyBack));
    require(!bit(touchKeys, protocol::keyHomepage));
}

void keyboardDeliversBackAndHomeAsKeyEvents()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::keyboard, events);

    // The runtime forwards a key as the key event and a report.
    std::array<protocol::InputEvent, 8> input {};
    std::size_t count = 0;
    const std::array keyCodes {protocol::keyBack, protocol::keyHomepage};
    for (const std::uint16_t code : keyCodes) {
        for (const std::int32_t value : {1, 0}) {
            input[count++] = {
                .type = protocol::eventKey,
                .code = code,
                .value = value,
            };
            input[count++] = {
                .type = protocol::eventSynchronize,
                .code = protocol::synchronizeReport,
                .value = 0,
            };
        }
    }
    events->enqueue(input);
    pollfd readiness {
        .fd = device.eventDescriptor(),
        .events = POLLIN,
        .revents = 0,
    };
    require(::poll(&readiness, 1, 1000) == 1);
    device.drainEventSource();
    for (const auto& sent : input) {
        const auto result =
            device.processQueue(protocol::eventQueue, 3, {}, 8);
        require(result.action == uml::virtio::VhostUserQueueAction::complete);
        const auto returned =
            uml::testing::loadExact<protocol::InputEvent>(result.response);
        require(returned.type == sent.type);
        require(returned.code == sent.code);
        require(returned.value == sent.value);
    }
    require(device.processQueue(protocol::eventQueue, 4, {}, 8).action ==
        uml::virtio::VhostUserQueueAction::retry);
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
    const auto returned =
        uml::testing::loadExact<protocol::InputEvent>(result.response);
    require(returned.type == protocol::eventAbsolute);
    require(returned.code == protocol::absoluteX);
    require(returned.value == 123);
}

} // namespace

int main()
{
    touchscreenConfigurationIsDirectWithAFixedRange();
    keyboardAdvertisesEvdevKeysAndLeds();
    keyboardAdvertisesBackAndHomeAndNothingElseAbove127();
    keyboardDeliversBackAndHomeAsKeyEvents();
    eventQueueDefersAndThenReturnsOneWireEvent();
    return EXIT_SUCCESS;
}
