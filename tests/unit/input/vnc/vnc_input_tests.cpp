// SPDX-License-Identifier: GPL-2.0-or-later

#include "vnc_input.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {

struct SinkFailure final {};

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void keySymbolsMapToStableGuestCodes()
{
    using uml::input::vnc::keyCode;

    require(keyCode('a') == 30 && keyCode('A') == 30,
        "letter key symbols did not share a physical key code");
    require(keyCode('!') == 2 && keyCode('?') == 53,
        "shifted key symbols did not map to their physical keys");
    require(keyCode(0xff0d) == 28 && keyCode(0xffff) == 111,
        "special key symbols did not map to guest input codes");
    require(!keyCode(0x10ffff),
        "an unsupported key symbol unexpectedly mapped to a guest key");
}

void primaryPointerContactUsesTheDirectTouchCode()
{
    require(
        uml::input::vnc::buttonTouch == 0x14a,
        "primary pointer contact did not use Linux BTN_TOUCH");
    require(
        uml::input::vnc::buttonLeft == 0x110,
        "desktop mouse input did not retain Linux BTN_LEFT");
}

void directTouchContactSuppressesHoverAndTracksAContact()
{
    uml::input::vnc::DirectTouchContact contact;
    require(!contact.update(false),
        "an idle pointer move was exposed as a touch event");
    require(contact.update(true) && contact.update(true),
        "touch down or drag was suppressed");
    require(contact.update(false),
        "touch release was suppressed");
    require(!contact.update(false),
        "hover after touch release was exposed as a touch event");
    require(!contact.release(),
        "an idle contact reported a focus-loss release");
    require(contact.update(true) && contact.release(),
        "focus loss did not release an active touch contact");
}

void inputDeliveryContainsSinkFailuresAndCanRetry()
{
    const std::array events{
        uml::input::vnc::VncInputEvent{.type = 1, .code = 30, .value = 1},
        uml::input::vnc::VncInputEvent{.type = 0, .code = 0, .value = 0},
    };
    require(uml::input::vnc::tryDeliver({}, events),
        "an intentionally absent input sink was reported as failed");

    std::size_t delivered = 0;
    uml::input::vnc::VncInputSink sink = [&](const auto received) {
        require(received.size() == events.size() &&
                received[0].code == events[0].code &&
                received[1].type == events[1].type,
            "input delivery changed an event sequence");
        ++delivered;
    };
    require(uml::input::vnc::tryDeliver(sink, events) && delivered == 1,
        "valid input delivery failed");

    sink = [](auto) { throw std::bad_alloc {}; };
    require(!uml::input::vnc::tryDeliver(sink, events),
        "an input-sink allocation failure escaped or was reported as success");

    sink = [](auto) { throw SinkFailure {}; };
    require(!uml::input::vnc::tryDeliver(sink, events),
        "a non-standard input-sink exception escaped or reported success");

    sink = [&](auto) { ++delivered; };
    require(uml::input::vnc::tryDeliver(sink, events) && delivered == 2,
        "input delivery could not retry after a contained sink exception");
}

} // namespace

int main()
{
    try {
        keySymbolsMapToStableGuestCodes();
        primaryPointerContactUsesTheDirectTouchCode();
        directTouchContactSuppressesHoverAndTracksAContact();
        inputDeliveryContainsSinkFailuresAndCanRetry();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "VNC input test failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
