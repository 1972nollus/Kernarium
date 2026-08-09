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
    using uml::gpu::input::keyCode;

    require(keyCode('a') == 30 && keyCode('A') == 30,
        "letter key symbols did not share a physical key code");
    require(keyCode('!') == 2 && keyCode('?') == 53,
        "shifted key symbols did not map to their physical keys");
    require(keyCode(0xff0d) == 28 && keyCode(0xffff) == 111,
        "special key symbols did not map to guest input codes");
    require(!keyCode(0x10ffff),
        "an unsupported key symbol unexpectedly mapped to a guest key");
}

void inputDeliveryContainsSinkFailuresAndCanRetry()
{
    const std::array events{
        uml::gpu::VncInputEvent{.type = 1, .code = 30, .value = 1},
        uml::gpu::VncInputEvent{.type = 0, .code = 0, .value = 0},
    };
    require(uml::gpu::input::tryDeliver({}, events),
        "an intentionally absent input sink was reported as failed");

    std::size_t delivered = 0;
    uml::gpu::VncInputSink sink = [&](const auto received) {
        require(received.size() == events.size() &&
                received[0].code == events[0].code &&
                received[1].type == events[1].type,
            "input delivery changed an event sequence");
        ++delivered;
    };
    require(uml::gpu::input::tryDeliver(sink, events) && delivered == 1,
        "valid input delivery failed");

    sink = [](auto) { throw std::bad_alloc {}; };
    require(!uml::gpu::input::tryDeliver(sink, events),
        "an input-sink allocation failure escaped or was reported as success");

    sink = [](auto) { throw SinkFailure {}; };
    require(!uml::gpu::input::tryDeliver(sink, events),
        "a non-standard input-sink exception escaped or reported success");

    sink = [&](auto) { ++delivered; };
    require(uml::gpu::input::tryDeliver(sink, events) && delivered == 2,
        "input delivery could not retry after a contained sink exception");
}

} // namespace

int main()
{
    try {
        keySymbolsMapToStableGuestCodes();
        inputDeliveryContainsSinkFailuresAndCanRetry();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "VNC input test failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
