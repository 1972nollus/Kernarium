// SPDX-License-Identifier: GPL-2.0-or-later
/* The Virtio balloon against a recording host: the target it publishes, and
 * which host pages its inflate and deflate requests release and commit. */

#include "protocol.hpp"
#include "require.hpp"
#include "virtio_balloon_device.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <poll.h>

namespace {

namespace protocol = uml::balloon::protocol;
using uml::balloon::BalloonOptions;
using uml::balloon::VirtioBalloonDevice;

constexpr std::uint64_t mib = 1ULL << 20;
constexpr std::size_t hostPage = 16 * 1024;

using uml::testing::require;

struct Call final {
    bool release;
    std::uint64_t offset;
    std::uint64_t length;

    bool operator==(const Call&) const = default;
};

class RecordingMemory final : public uml::balloon::BalloonMemory {
public:
    explicit RecordingMemory(std::vector<Call>& calls) : calls_(calls) {}

    int release(std::uint64_t offset, std::uint64_t length) noexcept override
    {
        calls_.push_back({true, offset, length});
        return 0;
    }

    int commit(std::uint64_t offset, std::uint64_t length) noexcept override
    {
        calls_.push_back({false, offset, length});
        return 0;
    }

private:
    std::vector<Call>& calls_;
};

struct Fixture final {
    explicit Fixture(std::size_t pageSize = hostPage,
        std::chrono::milliseconds interval = std::chrono::milliseconds {500})
    {
        device = std::make_unique<VirtioBalloonDevice>(
            BalloonOptions {
                .guestMemoryBytes = 256 * mib,
                .target = [this] { return wanted; },
                .interval = interval,
                .hostPageSize = pageSize,
            },
            std::make_unique<RecordingMemory>(calls));
    }

    [[nodiscard]] protocol::Configuration configuration() const
    {
        const auto bytes = device->configuration();
        require(bytes.size() == sizeof(protocol::Configuration),
            "the configuration has the standard size");
        protocol::Configuration result {};
        std::memcpy(&result, bytes.data(), sizeof(result));
        return result;
    }

    void send(std::uint32_t queue, const std::vector<std::uint32_t>& frames)
    {
        std::vector<std::uint8_t> request(frames.size() * sizeof(std::uint32_t));
        std::memcpy(request.data(), frames.data(), request.size());
        const auto result = device->processQueue(queue, 1, request, 0);
        require(result.action == uml::virtio::VhostUserQueueAction::complete,
            "balloon requests complete at once");
        require(result.response.empty(), "balloon requests carry no response");
    }

    std::uint64_t wanted {};
    std::vector<Call> calls;
    std::unique_ptr<VirtioBalloonDevice> device;
};

std::vector<std::uint32_t> frames(std::uint32_t first, std::uint32_t count)
{
    std::vector<std::uint32_t> result;
    for (std::uint32_t frame = first; frame < first + count; ++frame)
        result.push_back(frame);
    return result;
}

void advertises_the_standard_device()
{
    Fixture fixture;
    require(fixture.device->features() == protocol::featureMustTellHost,
        "only MUST_TELL_HOST is offered");
    require(fixture.device->queueCount() == 5,
        "the standard queue layout, of which only inflate and deflate run");
    const auto configuration = fixture.configuration();
    require(configuration.numPages == 0 && configuration.actual == 0,
        "an idle balloon starts empty");
    require(!fixture.device->configChangePending(),
        "an empty target raises no interrupt");
}

void the_target_follows_whole_granules()
{
    Fixture fixture;
    fixture.wanted = 40 * mib;
    fixture.device->updateTarget();
    require(fixture.device->configChangePending(), "a new target interrupts");
    require(fixture.configuration().numPages == 32 * mib / protocol::pageSize,
        "the target rounds down to the granule");
    fixture.device->clearConfigChange();

    fixture.wanted = 47 * mib;
    fixture.device->updateTarget();
    require(!fixture.device->configChangePending(),
        "a move of less than a granule is ignored");
    fixture.wanted = 17 * mib;
    fixture.device->updateTarget();
    require(!fixture.device->configChangePending(),
        "a fall of less than a granule is ignored");

    fixture.wanted = 1024 * mib;
    fixture.device->updateTarget();
    require(fixture.device->targetBytes() == 256 * mib,
        "the target never exceeds guest memory");
    fixture.device->clearConfigChange();
    fixture.wanted = 0;
    fixture.device->updateTarget();
    require(fixture.device->targetBytes() == 0 &&
            fixture.device->configChangePending(),
        "an emptied target releases the balloon");
}

void the_driver_reports_what_it_holds()
{
    Fixture fixture;
    const std::uint32_t actual = 4096;
    std::uint8_t bytes[sizeof(actual)];
    std::memcpy(bytes, &actual, sizeof(actual));
    fixture.device->writeConfiguration(protocol::actualOffset, 0, bytes);
    require(fixture.configuration().actual == actual,
        "the driver's actual count is kept");
    require(fixture.device->actualBytes() == 16 * mib, "actual is in 4 KiB");
    fixture.device->writeConfiguration(0, 0, bytes);
    require(fixture.configuration().numPages == 0,
        "the target is not writable by the driver");
}

void only_whole_host_pages_leave()
{
    Fixture fixture;
    /* Three of the four frames of host page 2. */
    fixture.send(protocol::inflateQueue, frames(8, 3));
    require(fixture.calls.empty(), "a partial host page stays");
    fixture.send(protocol::inflateQueue, {11});
    require((fixture.calls == std::vector<Call> {{true, 2 * hostPage, hostPage}}),
        "the page leaves once all of it is ballooned");
    fixture.calls.clear();

    /* Two runs of whole pages, out of order, with a duplicate. */
    auto request = frames(20, 8);
    auto second = frames(40, 4);
    request.insert(request.begin(), second.begin(), second.end());
    request.push_back(20);
    fixture.send(protocol::inflateQueue, request);
    require((fixture.calls == std::vector<Call> {
                 {true, 5 * hostPage, 2 * hostPage},
                 {true, 10 * hostPage, hostPage},
             }),
        "adjacent pages merge into one release");
    require(fixture.device->releasedBytes() == 4 * hostPage,
        "released bytes count whole host pages");
}

void deflate_commits_before_completing()
{
    Fixture fixture;
    fixture.send(protocol::inflateQueue, frames(0, 16));
    fixture.calls.clear();
    fixture.send(protocol::deflateQueue, {5});
    require((fixture.calls == std::vector<Call> {{false, hostPage, hostPage}}),
        "the first frame taken back commits its host page");
    fixture.calls.clear();
    fixture.send(protocol::deflateQueue, {4, 6, 7});
    require(fixture.calls.empty(), "an already committed page is not again");
    fixture.send(protocol::deflateQueue, {100, 1u << 30});
    require(fixture.calls.empty(), "frames the balloon does not hold are ignored");
    require(fixture.device->releasedBytes() == 3 * hostPage,
        "released bytes follow the deflate");
}

void reset_returns_every_page()
{
    Fixture fixture;
    fixture.send(protocol::inflateQueue, frames(0, 9));
    fixture.calls.clear();
    fixture.device->reset();
    require((fixture.calls == std::vector<Call> {{false, 0, 2 * hostPage}}),
        "reset commits every released page");
    require(fixture.device->releasedBytes() == 0 &&
            fixture.device->actualBytes() == 0,
        "reset forgets the balloon");
    fixture.calls.clear();
    fixture.send(protocol::inflateQueue, frames(0, 4));
    require(fixture.calls.size() == 1, "the balloon works again after reset");
}

void a_4k_host_releases_every_frame()
{
    Fixture fixture {4096};
    fixture.send(protocol::inflateQueue, {3, 4, 9});
    require((fixture.calls == std::vector<Call> {
                 {true, 3 * 4096, 2 * 4096},
                 {true, 9 * 4096, 4096},
             }),
        "each frame is a whole host page");
}

void the_timer_wakes_the_transport()
{
    Fixture fixture {hostPage, std::chrono::milliseconds {5}};
    pollfd descriptor {
        .fd = fixture.device->eventDescriptor(),
        .events = POLLIN,
        .revents = 0,
    };
    require(descriptor.fd >= 0, "the device exposes its timer");
    fixture.wanted = 64 * mib;
    require(::poll(&descriptor, 1, 5000) == 1, "the timer fires");
    fixture.device->drainEventSource();
    require(fixture.device->configChangePending() &&
            fixture.device->targetBytes() == 64 * mib,
        "a timer tick re-evaluates the target");
}

} // namespace

int main()
{
    const struct {
        const char* name;
        void (*run)();
    } tests[] = {
        {"advertises the standard device", advertises_the_standard_device},
        {"the target follows whole granules", the_target_follows_whole_granules},
        {"the driver reports what it holds", the_driver_reports_what_it_holds},
        {"only whole host pages leave", only_whole_host_pages_leave},
        {"deflate commits before completing", deflate_commits_before_completing},
        {"reset returns every page", reset_returns_every_page},
        {"a 4k host releases every frame", a_4k_host_releases_every_frame},
        {"the timer wakes the transport", the_timer_wakes_the_transport},
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::printf("ok: %s\n", test.name);
        } catch (const std::exception& error) {
            std::printf("FAIL: %s: %s\n", test.name, error.what());
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
