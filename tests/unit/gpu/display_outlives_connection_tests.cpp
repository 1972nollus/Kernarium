// SPDX-License-Identifier: GPL-2.0-or-later

/* A guest that restarts itself is a new guest on the same machine, so the
 * machine's display stays up while the guest's GPU connection is replaced.
 * This test links the shipped service, connects a VNC client, has a guest
 * show a frame, replaces the connection twice and requires that the client
 * never has to reconnect: the same listener, the same socket and the same
 * input path keep working, and what the client sees after each replacement is
 * the idle picture, with no pixel of the guest before it. */

#include "gpu_test_support.hpp"
#include "in_process_service.hpp"
#include "vnc_test_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
namespace protocol = uml::gpu::protocol;

constexpr std::uint32_t displayExtent = 64;
constexpr std::uint32_t frameExtent = 8;
constexpr int skippedExitCode = 77;
/* BGRA, as the Virtio-GPU format b8g8r8a8 lays it out, which is the byte order
 * of the VNC client's pixels too. The two guests' frames differ from each
 * other and from anything the idle picture holds. */
using Pixel = std::array<std::uint8_t, 4>;
constexpr Pixel firstFrame {0x13U, 0x57U, 0x9bU, 0xffU};
constexpr Pixel secondFrame {0xe1U, 0x4dU, 0x27U, 0xffU};
/* LibVNCServer composites its default cursor into the pixels it sends a
 * client that asked for none, so the top-left corner is the server's. */
constexpr std::uint32_t cursorExtent = 32;

/* The guest's ends of one connection: what a guest would be handed. */
struct GuestEndpoints {
    int transport {-1};
    int input {-1};

    GuestEndpoints() = default;
    GuestEndpoints(const GuestEndpoints&) = delete;
    GuestEndpoints& operator=(const GuestEndpoints&) = delete;

    /* The guest has gone: its endpoints are closed, as the service requires
     * before it opens the next connection. */
    void close() noexcept
    {
        if (transport >= 0)
            ::close(transport);
        if (input >= 0)
            ::close(input);
        transport = input = -1;
    }
    ~GuestEndpoints() { close(); }
};

[[nodiscard]] std::unique_ptr<GuestEndpoints> takeEndpoints(
    uml::gpu::InProcessGpuService& service)
{
    auto endpoints = std::make_unique<GuestEndpoints>();
    endpoints->transport = uml::gpu::testing::descriptorFromName(
        service.takeConnectionName());
    endpoints->input = uml::gpu::testing::descriptorFromName(
        service.takeInputConnectionName());
    return endpoints;
}

/* A guest's driver, as far as showing one frame goes: it speaks the
 * vhost-user protocol on `transport`, gives the device one page of memory and
 * sends the control commands of a software scanout. */
class FrameGuest {
public:
    explicit FrameGuest(int transport) : socket_(transport)
    {
        namespace testing = uml::gpu::testing;
        testing::negotiateProtocolFeatures(
            socket_, protocol::protocolFeatureReplyAck);
        testing::configureMemory(socket_, ring_.descriptor());
        channels_ = testing::startQueue(socket_, ring_);
    }

    /* Shows a frame of `pixel` on the display, as a guest's console does. */
    void show(const Pixel& pixel)
    {
        const std::uint32_t resource = ++resources_;
        auto create = request<protocol::GpuResourceCreate2d>(
            protocol::GpuCommand::resourceCreate2d);
        create.resourceId = resource;
        create.format =
            static_cast<std::uint32_t>(protocol::GpuFormat::b8g8r8a8Unorm);
        create.width = frameExtent;
        create.height = frameExtent;
        send(&create, sizeof(create));

        auto attach = request<protocol::GpuAttachBacking>(
            protocol::GpuCommand::resourceAttachBacking);
        attach.resourceId = resource;
        attach.entryCount = 1;
        const protocol::GpuMemoryEntry entry {
            .address = uml::gpu::testing::SplitRing::guestBase + pixelOffset,
            .length = frameBytes,
            .padding = 0,
        };
        std::array<std::uint8_t, sizeof(attach) + sizeof(entry)> attachment {};
        std::memcpy(attachment.data(), &attach, sizeof(attach));
        std::memcpy(attachment.data() + sizeof(attach), &entry, sizeof(entry));
        send(attachment.data(), attachment.size());

        for (std::size_t offset = 0; offset < frameBytes;
             offset += pixel.size()) {
            std::memcpy(
                ring_.memory().data() + pixelOffset + offset,
                pixel.data(),
                pixel.size());
        }
        const protocol::GpuRectangle whole {
            .x = 0, .y = 0, .width = frameExtent, .height = frameExtent};

        auto scanout = request<protocol::GpuSetScanout>(
            protocol::GpuCommand::setScanout);
        scanout.rectangle = whole;
        scanout.scanoutId = 0;
        scanout.resourceId = resource;
        send(&scanout, sizeof(scanout));

        auto transfer = request<protocol::GpuTransferToHost2d>(
            protocol::GpuCommand::transferToHost2d);
        transfer.rectangle = whole;
        transfer.offset = 0;
        transfer.resourceId = resource;
        send(&transfer, sizeof(transfer));

        auto flush = request<protocol::GpuResourceFlush>(
            protocol::GpuCommand::resourceFlush);
        flush.rectangle = whole;
        flush.resourceId = resource;
        send(&flush, sizeof(flush));
    }

private:
    static constexpr std::size_t requestOffset = 0x500;
    static constexpr std::size_t responseOffset = 0x600;
    static constexpr std::size_t pixelOffset = 0x800;
    static constexpr std::uint32_t frameBytes =
        frameExtent * frameExtent * 4U;
    static constexpr std::uint16_t queueSize = 8;

    template<typename Request>
    [[nodiscard]] static Request request(protocol::GpuCommand command)
    {
        Request built {};
        built.header.type = static_cast<std::uint32_t>(command);
        return built;
    }

    /* One control command, run to completion: it must be answered with
     * OK_NODATA. */
    void send(const void* command, std::size_t length)
    {
        require(length <= responseOffset - requestOffset);
        std::memcpy(ring_.memory().data() + requestOffset, command, length);
        ring_.writeDescriptors(uml::gpu::testing::requestChain(
            requestOffset,
            static_cast<std::uint32_t>(length),
            responseOffset,
            sizeof(protocol::GpuControlHeader)));
        const std::array<std::uint16_t, 1> head {0};
        ring_.writeHeads(head, published_ % queueSize);
        ++published_;
        ring_.setAvailableIndex(published_);
        uml::gpu::testing::writeKick(channels_.kick);
        uml::gpu::testing::awaitCall(channels_.call.readEnd.get());
        require(ring_.usedIndex() == published_, "the command did not finish");
        protocol::GpuControlHeader response {};
        std::memcpy(
            &response, ring_.memory().data() + responseOffset,
            sizeof(response));
        require(
            response.type ==
                static_cast<std::uint32_t>(protocol::GpuResponse::okNoData),
            "the device refused a scanout command");
    }

    int socket_;
    uml::gpu::testing::SplitRing ring_;
    uml::gpu::testing::QueueChannels channels_;
    std::uint16_t published_ {};
    std::uint32_t resources_ {};
};

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

/* A pointer move the client makes reaches the guest that has the
 * connection's input endpoint, as events. */
void requireInputReaches(
    VncConnection& client,
    const GuestEndpoints& current,
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
}

[[nodiscard]] bool isFrame(
    const std::vector<std::uint8_t>& image,
    const Pixel& pixel)
{
    /* The frame is the whole display: both are square. Look at a pixel well
     * away from the cursor. */
    const auto offset =
        (static_cast<std::size_t>(displayExtent - 8U) * displayExtent +
         (displayExtent - 8U)) * 4U;
    return std::equal(pixel.begin(), pixel.begin() + 3, image.begin() + offset);
}

[[nodiscard]] bool containsFrame(
    const std::vector<std::uint8_t>& image,
    const Pixel& pixel)
{
    for (std::uint32_t y = 0; y < displayExtent; ++y) {
        for (std::uint32_t x = 0; x < displayExtent; ++x) {
            if (x < cursorExtent && y < cursorExtent)
                continue;
            const auto offset =
                (static_cast<std::size_t>(y) * displayExtent + x) * 4U;
            if (std::equal(
                    pixel.begin(), pixel.begin() + 3, image.begin() + offset)) {
                return true;
            }
        }
    }
    return false;
}

/* Whether two images of the display are the same picture, apart from the
 * cursor the server draws where the client last moved its pointer. */
[[nodiscard]] bool samePicture(
    const std::vector<std::uint8_t>& image,
    const std::vector<std::uint8_t>& other)
{
    if (image.size() != other.size())
        return false;
    for (std::uint32_t y = 0; y < displayExtent; ++y) {
        for (std::uint32_t x = 0; x < displayExtent; ++x) {
            if (x < cursorExtent && y < cursorExtent)
                continue;
            const auto offset =
                (static_cast<std::size_t>(y) * displayExtent + x) * 4U;
            if (!std::equal(
                    image.begin() + offset,
                    image.begin() + offset + 3,
                    other.begin() + offset)) {
                return false;
            }
        }
    }
    return true;
}

/* What the client sees once the guest's frame has arrived. */
[[nodiscard]] std::vector<std::uint8_t> awaitFrame(
    VncConnection& client,
    const Pixel& pixel)
{
    std::vector<std::uint8_t> image;
    require(
        uml::testing::waitUntil(
            [&] {
                image = client.readFramebuffer();
                return isFrame(image, pixel);
            },
            std::chrono::seconds {10},
            std::chrono::milliseconds {50}),
        "the guest's frame never reached the VNC client");
    return image;
}

/* The client's framebuffer is the idle picture: what it showed before any
 * guest drew, and nothing of the guest before it. */
void requireIdle(
    VncConnection& client,
    const std::vector<std::uint8_t>& idle,
    const Pixel& previous,
    const char* when)
{
    const auto image = client.readFramebuffer();
    if (containsFrame(image, previous)) {
        std::fprintf(stderr, "a pixel of the old guest remains %s\n", when);
        require(false, "the old guest's frame is still on the display");
    }
    if (!samePicture(image, idle)) {
        std::fprintf(stderr, "the display is not idle %s\n", when);
        require(false, "the display was not reset to its idle picture");
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

    VncConnection client {*port};
    client.handshake();
    const auto idle = client.readFramebuffer();
    require(
        !containsFrame(idle, firstFrame) && !containsFrame(idle, secondFrame),
        "the idle picture holds a test colour");

    /* The first guest shows a frame, and input from the client reaches it. */
    auto first = takeEndpoints(*service);
    {
        FrameGuest guest {first->transport};
        guest.show(firstFrame);
        const auto shown = awaitFrame(client, firstFrame);
        require(
            !samePicture(shown, idle),
            "the first frame left the display idle");
        requireInputReaches(client, *first, 3, "of the first connection");

        /* A hold the application placed stays through every restart. */
        require(
            service->holdRequests(std::chrono::milliseconds {100}),
            "the service would not hold");
        /* The guest goes: its driver and its endpoints are closed before the
         * next connection opens. */
    }
    first->close();

    service->startNextConnection();
    require(service->requestsHeld(), "the restart dropped the hold");
    require(
        service->vncPort() == port, "the restart moved the VNC listener");
    requireIdle(client, idle, firstFrame, "after the first restart");

    /* A viewer that joins now finds the display too. */
    {
        VncConnection latecomer {*port};
        latecomer.handshake();
        require(
            samePicture(latecomer.readFramebuffer(), idle),
            "a late viewer does not see the idle picture");
    }

    /* The second guest is held until the application lets it through, then
     * shows a frame of its own. */
    auto second = takeEndpoints(*service);
    {
        FrameGuest guest {second->transport};
        service->releaseRequests();
        require(!service->requestsHeld(), "the release did not open the gate");
        guest.show(secondFrame);
        const auto shown = awaitFrame(client, secondFrame);
        require(
            !containsFrame(shown, firstFrame),
            "the first guest's frame is on the display with the second's");
        requireInputReaches(client, *second, 4, "of the second connection");
        require(
            service->holdRequests(std::chrono::milliseconds {100}),
            "the service would not hold");
    }
    second->close();

    service->startNextConnection();
    require(service->requestsHeld(), "the restart dropped the hold");
    require(
        service->vncPort() == port, "the restart moved the VNC listener");
    requireIdle(client, idle, secondFrame, "after the second restart");

    auto third = takeEndpoints(*service);
    requireInputReaches(client, *third, 5, "of the third connection");
    third->close();

    service->releaseRequests();
    require(!service->requestsHeld(), "the release did not open the gate");

    service->stop();
    require(
        !service->vncPort().has_value(),
        "the display outlived the service");
    std::puts("display outlives connection tests passed");
    return 0;
}
