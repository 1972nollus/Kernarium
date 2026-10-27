// SPDX-License-Identifier: GPL-2.0-or-later

/* A guest that restarts itself is a new guest on the same machine, so the
 * machine's display stays up while the guest's GPU connection is replaced. This
 * test links the shipped service, connects a VNC client, replaces the
 * connection twice and requires that the client never has to reconnect: the
 * same listener, the same socket and the same input path keep working, and the
 * input of a new connection reaches the guest that has it and no other. */

#include "in_process_service.hpp"
#include "require.hpp"
#include "vnc_test_client.hpp"
#include "wait_until.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using uml::gpu::testing::VncConnection;
using uml::testing::require;

constexpr std::uint32_t displayExtent = 64;
constexpr int skippedExitCode = 77;
constexpr auto closeSettle = std::chrono::milliseconds {200};

/* The guest's ends of one connection: what a guest would be handed. */
struct GuestEndpoints {
    int transport {-1};
    int input {-1};

    ~GuestEndpoints()
    {
        if (transport >= 0)
            ::close(transport);
        if (input >= 0)
            ::close(input);
    }

    GuestEndpoints() = default;
    GuestEndpoints(const GuestEndpoints&) = delete;
    GuestEndpoints& operator=(const GuestEndpoints&) = delete;
};

int descriptorFromName(const std::string& name)
{
    require(name.starts_with("fd="), "an endpoint name is not a descriptor");
    return std::stoi(name.substr(3));
}

[[nodiscard]] std::unique_ptr<GuestEndpoints> takeEndpoints(
    uml::gpu::InProcessGpuService& service)
{
    auto endpoints = std::make_unique<GuestEndpoints>();
    endpoints->transport = descriptorFromName(service.takeConnectionName());
    endpoints->input = descriptorFromName(service.takeInputConnectionName());
    return endpoints;
}

/* The next display input datagram on `descriptor`, as the event bytes it
 * carries, or none within `wait`. */
[[nodiscard]] std::vector<std::uint8_t> receiveInput(
    int descriptor,
    std::chrono::milliseconds wait)
{
    pollfd ready {.fd = descriptor, .events = POLLIN};
    if (::poll(&ready, 1, static_cast<int>(wait.count())) != 1 ||
        (ready.revents & POLLIN) == 0) {
        return {};
    }
    std::vector<std::uint8_t> bytes(4096);
    const auto received = ::recv(descriptor, bytes.data(), bytes.size(), 0);
    if (received <= 0)
        return {};
    bytes.resize(static_cast<std::size_t>(received));
    return bytes;
}

/* What the client sees of the display. A restart must neither close the
 * client's socket nor change the display it was shown. */
void requireClientSeesTheDisplay(VncConnection& client, const char* when)
{
    const auto image = client.readFramebuffer();
    if (image.size() != static_cast<std::size_t>(displayExtent) *
            displayExtent * 4U) {
        std::fprintf(stderr, "client framebuffer after %s is the wrong size\n",
            when);
        require(false, "the display changed under the client");
    }
}

/* A pointer move the client makes reaches the guest that has the connection's
 * input endpoint, as events, and not the guest before it. */
void requireInputReaches(
    VncConnection& client,
    const GuestEndpoints& current,
    const GuestEndpoints* previous,
    std::uint16_t x,
    const char* when)
{
    /* Drain what the guest endpoint holds from earlier moves. */
    constexpr std::chrono::milliseconds none {0};
    while (!receiveInput(current.input, none).empty()) {
    }
    client.pointer(0, x, x);
    const auto events = receiveInput(current.input, std::chrono::seconds {10});
    if (events.empty() || events.size() % 8U != 0) {
        std::fprintf(stderr, "no display input reached the guest %s\n", when);
        require(false, "display input did not follow the new connection");
    }
    if (previous != nullptr) {
        require(
            receiveInput(previous->input, closeSettle).empty(),
            "display input reached the guest before");
    }
}

} // namespace

int main()
{
    uml::gpu::InProcessGpuOptions options;
    options.vncPort = 0;
    options.width = displayExtent;
    options.height = displayExtent;
    std::unique_ptr<uml::gpu::InProcessGpuService> service;
    try {
        service = std::make_unique<uml::gpu::InProcessGpuService>(
            std::move(options));
    } catch (const std::exception& error) {
        std::fprintf(
            stderr, "renderer unavailable: %s; skipping\n", error.what());
        return skippedExitCode;
    }

    const auto port = service->vncPort();
    require(port.has_value() && *port != 0, "the display has no VNC port");

    auto first = takeEndpoints(*service);
    VncConnection client {*port};
    client.handshake();
    requireClientSeesTheDisplay(client, "the first connection opened");
    requireInputReaches(client, *first, nullptr, 3, "of the first connection");

    /* A hold the application placed stays through every restart. */
    require(
        service->holdRequests(std::chrono::milliseconds {100}),
        "the service would not hold");

    std::unique_ptr<GuestEndpoints> previous = std::move(first);
    for (unsigned restart = 1; restart <= 2; ++restart) {
        const std::string when = restart == 1 ?
            "the first restart" : "the second restart";
        service->startNextConnection();
        require(service->requestsHeld(), "the restart dropped the hold");
        require(
            service->vncPort() == port,
            "the restart moved the VNC listener");

        auto current = takeEndpoints(*service);
        requireClientSeesTheDisplay(client, when.c_str());
        requireInputReaches(
            client,
            *current,
            previous.get(),
            static_cast<std::uint16_t>(3U + restart),
            when.c_str());

        /* A viewer that joins now finds the display too. */
        {
            VncConnection latecomer {*port};
            latecomer.handshake();
            requireClientSeesTheDisplay(latecomer, when.c_str());
        }
        previous = std::move(current);
    }

    service->releaseRequests();
    require(!service->requestsHeld(), "the release did not open the gate");

    service->stop();
    require(
        !service->vncPort().has_value(),
        "the display outlived the service");
    std::puts("display outlives connection tests passed");
    return 0;
}
