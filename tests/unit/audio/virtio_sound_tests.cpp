// SPDX-License-Identifier: GPL-2.0-or-later

#include "period_clock.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "virtio_sound_device.hpp"
#include "wire_bytes.hpp"

#include <poll.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace protocol = uml::audio::protocol;

using uml::testing::bytes;
using uml::testing::load;
using uml::testing::require;

using namespace std::chrono_literals;

/* A period clock whose time the test advances by hand. */
class TestClock final : public uml::audio::PeriodClock {
public:
    uml::audio::ClockTime now() const noexcept override { return time; }
    void notify() const noexcept override { ++notifyCount; }
    void wakeAt(uml::audio::ClockTime deadline) noexcept override
    {
        armed = deadline;
    }
    void cancel() noexcept override { armed.reset(); }
    int descriptor() const noexcept override { return -1; }
    void drain() noexcept override { ++drainCount; }

    /* What the operating system does as time passes: a deadline that has
     * come fires once and is gone. */
    void fire()
    {
        if (armed && *armed <= time)
            armed.reset();
    }

    uml::audio::ClockTime time {std::chrono::steady_clock::time_point {} + 1h};
    std::optional<uml::audio::ClockTime> armed;
    mutable unsigned notifyCount {};
    unsigned drainCount {};
};

class TestSink final : public uml::audio::PlaybackSink {
public:
    bool configure(const uml::audio::PlaybackParameters& parameters) override
    {
        ++configureCount;
        configured = parameters;
        return configureResult;
    }

    bool prepare() override
    {
        ++prepareCount;
        return prepareResult;
    }

    bool start() override
    {
        ++startCount;
        return startResult;
    }

    void stop() noexcept override { ++stopCount; }
    void release() noexcept override { ++releaseCount; }
    void reset() noexcept override { ++resetCount; }

    uml::audio::PlaybackEnqueueResult enqueue(
        std::uint64_t requestId,
        std::span<const std::uint8_t> pcm) override
    {
        if (enqueueResult != uml::audio::PlaybackEnqueueResult::accepted)
            return enqueueResult;
        enqueuedRequest = requestId;
        enqueued.assign(pcm.begin(), pcm.end());
        return uml::audio::PlaybackEnqueueResult::accepted;
    }

    void handleEvents() override { ++drainCount; }

    bool takeCompletion(uml::audio::PlaybackCompletion& completion) override
    {
        if (completions.empty())
            return false;
        completion = completions.front();
        completions.pop_front();
        return true;
    }

    uml::audio::PlaybackParameters configured {};
    std::vector<std::uint8_t> enqueued;
    std::deque<uml::audio::PlaybackCompletion> completions;
    std::uint64_t enqueuedRequest {};
    unsigned configureCount {};
    unsigned prepareCount {};
    unsigned startCount {};
    unsigned stopCount {};
    unsigned releaseCount {};
    unsigned resetCount {};
    unsigned drainCount {};
    bool configureResult {true};
    bool prepareResult {true};
    bool startResult {true};
    uml::audio::PlaybackEnqueueResult enqueueResult {
        uml::audio::PlaybackEnqueueResult::accepted};
};

struct Fixture final {
    Fixture()
        : clock(std::make_shared<TestClock>())
    {
        auto owned = std::make_unique<TestSink>();
        sink = owned.get();
        device = std::make_unique<uml::audio::VirtioSoundDevice>(
            std::move(owned), clock);
    }

    TestSink* sink {};
    std::shared_ptr<TestClock> clock;
    std::unique_ptr<uml::audio::VirtioSoundDevice> device;
};

uml::virtio::VhostUserQueueResult control(
    uml::audio::VirtioSoundDevice& device,
    std::span<const std::uint8_t> request,
    std::size_t capacity = 256)
{
    const auto result = device.processQueue(
        protocol::controlQueue, 1, request, capacity);
    require(result.action == uml::virtio::VhostUserQueueAction::complete);
    return result;
}

protocol::Status responseStatus(
    const uml::virtio::VhostUserQueueResult& result)
{
    return static_cast<protocol::Status>(
        load<protocol::Header>(result.response).code);
}

protocol::PcmSetParameters parameters()
{
    return {
        .header = {
            .header = {
                .code = static_cast<std::uint32_t>(
                    protocol::Request::pcmSetParameters),
            },
            .streamId = protocol::streamId,
        },
        .bufferBytes = 4096,
        .periodBytes = 1024,
        .features = 0,
        .channels = protocol::channelCount,
        .format = protocol::PcmFormat::signed16,
        .rate = protocol::PcmRate::rate48000,
        .padding = 0,
    };
}

protocol::PcmHeader pcmCommand(protocol::Request request)
{
    return {
        .header = {.code = static_cast<std::uint32_t>(request)},
        .streamId = protocol::streamId,
    };
}

void exposesOneStereoPlaybackStream()
{
    Fixture fixture;
    require(fixture.device->features() == 0);
    require(fixture.device->queueCount() == protocol::queueCount);
    const auto configuration = fixture.device->configuration();
    const auto config = load<protocol::Configuration>(configuration);
    require(config.jacks == 0);
    require(config.streams == 1);
    require(config.channelMaps == 1);
    require(config.controls == 0);

    const protocol::QueryInfo query {
        .header = {
            .code = static_cast<std::uint32_t>(protocol::Request::pcmInfo),
        },
        .startId = 0,
        .count = 1,
        .size = sizeof(protocol::PcmInfo),
    };
    const auto result = control(*fixture.device, bytes(query));
    require(responseStatus(result) == protocol::Status::ok);
    const auto info = load<protocol::PcmInfo>(
        result.response, sizeof(protocol::Header));
    require(info.direction == protocol::Direction::output);
    require(info.minimumChannels == 2 && info.maximumChannels == 2);
    require((info.formats &
             (1ULL << static_cast<unsigned>(protocol::PcmFormat::signed16))) !=
            0);
    require((info.rates &
             (1ULL << static_cast<unsigned>(protocol::PcmRate::rate48000))) !=
            0);
}

void exposesFrontLeftAndRightChannelMap()
{
    Fixture fixture;
    const protocol::QueryInfo query {
        .header = {
            .code = static_cast<std::uint32_t>(
                protocol::Request::channelMapInfo),
        },
        .startId = 0,
        .count = 1,
        .size = sizeof(protocol::ChannelMapInfo),
    };
    const auto result = control(*fixture.device, bytes(query));
    require(responseStatus(result) == protocol::Status::ok);
    const auto info = load<protocol::ChannelMapInfo>(
        result.response, sizeof(protocol::Header));
    require(info.direction == protocol::Direction::output);
    require(info.channels == 2);
    require(info.positions[0] == static_cast<std::uint8_t>(
        protocol::ChannelPosition::frontLeft));
    require(info.positions[1] == static_cast<std::uint8_t>(
        protocol::ChannelPosition::frontRight));
}

void rejectsMalformedAndOutOfOrderControlRequests()
{
    Fixture fixture;
    const std::array<std::uint8_t, 3> shortHeader {};
    require(responseStatus(control(*fixture.device, shortHeader)) ==
            protocol::Status::badMessage);

    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmStart)))) ==
            protocol::Status::ioError);

    const protocol::QueryInfo invalidQuery {
        .header = {
            .code = static_cast<std::uint32_t>(protocol::Request::pcmInfo),
        },
        .startId = 0,
        .count = 2,
        .size = sizeof(protocol::PcmInfo),
    };
    require(responseStatus(control(*fixture.device, bytes(invalidQuery))) ==
            protocol::Status::badMessage);

    const auto event = fixture.device->processQueue(
        protocol::eventQueue, 2, {}, sizeof(protocol::Header));
    require(event.action == uml::virtio::VhostUserQueueAction::retry);
    const auto receive = fixture.device->processQueue(
        protocol::receiveQueue, 3, {}, 0);
    require(receive.action == uml::virtio::VhostUserQueueAction::complete);
    require(receive.response.empty());
}

void playbackDefersUntilTheAppleSinkConsumesAPeriod()
{
    Fixture fixture;
    const auto selected = parameters();
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(fixture.sink->configured.bufferBytes == selected.bufferBytes);
    require(fixture.sink->configured.sampleRate == protocol::sampleRate);
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmPrepare)))) ==
            protocol::Status::ok);
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmStart)))) ==
            protocol::Status::ok);

    const protocol::PcmTransfer transfer {.streamId = protocol::streamId};
    std::vector<std::uint8_t> pcm(selected.periodBytes, 0x5a);
    auto request = bytes(transfer);
    request.insert(request.end(), pcm.begin(), pcm.end());
    const auto queued = fixture.device->processQueue(
        protocol::transmitQueue, 42, request, sizeof(protocol::PcmStatus));
    require(queued.action == uml::virtio::VhostUserQueueAction::defer);
    require(fixture.sink->enqueuedRequest == 42);
    require(fixture.sink->enqueued == pcm);

    fixture.sink->completions.push_back({
        .requestId = 42,
        .latencyBytes = selected.periodBytes,
    });
    uml::virtio::VhostUserQueueCompletion completion;
    require(fixture.device->takeQueueCompletion(completion));
    require(completion.requestId == 42);
    const auto status = load<protocol::PcmStatus>(completion.response);
    require(status.status == protocol::Status::ok);
    require(status.latencyBytes == selected.periodBytes);
    require(!fixture.device->takeQueueCompletion(completion));

    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmStop)))) ==
            protocol::Status::ok);
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmRelease)))) ==
            protocol::Status::ok);
    require(fixture.sink->stopCount == 1);
    require(fixture.sink->releaseCount == 1);
}

void acceptsTheVirtioPcmLifecycleTransitions()
{
    Fixture fixture;
    auto selected = parameters();
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(fixture.sink->configureCount == 2);

    const auto prepare = bytes(pcmCommand(protocol::Request::pcmPrepare));
    require(responseStatus(control(*fixture.device, prepare)) ==
            protocol::Status::ok);
    require(responseStatus(control(*fixture.device, prepare)) ==
            protocol::Status::ok);
    require(fixture.sink->prepareCount == 2);

    selected.periodBytes = 2048;
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(fixture.sink->configured.periodBytes == selected.periodBytes);
    require(responseStatus(control(*fixture.device, prepare)) ==
            protocol::Status::ok);

    const auto release = bytes(pcmCommand(protocol::Request::pcmRelease));
    require(responseStatus(control(*fixture.device, release)) ==
            protocol::Status::ok);
    require(fixture.sink->releaseCount == 1);
    require(responseStatus(control(*fixture.device, prepare)) ==
            protocol::Status::ok);
    require(fixture.sink->prepareCount == 4);
}

void rejectsUnsupportedParametersAndRetriesWhenFull()
{
    Fixture fixture;
    auto selected = parameters();
    selected.rate = static_cast<protocol::PcmRate>(6);
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::notSupported);

    selected = parameters();
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmPrepare)))) ==
            protocol::Status::ok);
    fixture.sink->enqueueResult = uml::audio::PlaybackEnqueueResult::retry;
    const protocol::PcmTransfer transfer {.streamId = protocol::streamId};
    std::vector<std::uint8_t> pcm(selected.periodBytes);
    auto request = bytes(transfer);
    request.insert(request.end(), pcm.begin(), pcm.end());
    const auto result = fixture.device->processQueue(
        protocol::transmitQueue, 99, request, sizeof(protocol::PcmStatus));
    require(result.action == uml::virtio::VhostUserQueueAction::retry);

    fixture.sink->enqueueResult = uml::audio::PlaybackEnqueueResult::failed;
    const auto failed = fixture.device->processQueue(
        protocol::transmitQueue, 100, request, sizeof(protocol::PcmStatus));
    require(failed.action == uml::virtio::VhostUserQueueAction::complete);
    require(load<protocol::PcmStatus>(failed.response).status ==
            protocol::Status::ioError);
}

/* A transfer's buffer belongs to the parameters it was queued under, so new
 * parameters wait until every queued transfer has come back. */
void refusesParametersWhileTransfersAreQueued()
{
    Fixture fixture;
    const auto selected = parameters();
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmPrepare)))) ==
            protocol::Status::ok);
    const protocol::PcmTransfer transfer {.streamId = protocol::streamId};
    std::vector<std::uint8_t> pcm(selected.periodBytes);
    auto request = bytes(transfer);
    request.insert(request.end(), pcm.begin(), pcm.end());
    require(fixture.device->processQueue(
                protocol::transmitQueue, 7, request,
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);

    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ioError);
    require(fixture.sink->configureCount == 1);

    fixture.sink->completions.push_back({
        .requestId = 7,
        .latencyBytes = selected.periodBytes,
    });
    uml::virtio::VhostUserQueueCompletion completion;
    require(fixture.device->takeQueueCompletion(completion) &&
            completion.requestId == 7);
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(fixture.sink->configureCount == 2);

    /* A device reset drops whatever the sink still held. */
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmPrepare)))) ==
            protocol::Status::ok);
    require(fixture.device->processQueue(
                protocol::transmitQueue, 8, request,
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);
    fixture.device->reset();
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
}


protocol::PcmSetParameters pacedParameters()
{
    auto selected = parameters();
    selected.bufferBytes = 3840;
    selected.periodBytes = 960;  // 5 ms of 48 kHz stereo S16.
    return selected;
}

std::vector<std::uint8_t> transferOf(std::uint32_t periodBytes)
{
    const protocol::PcmTransfer transfer {.streamId = protocol::streamId};
    std::vector<std::uint8_t> pcm(periodBytes, 0x5a);
    auto request = bytes(transfer);
    request.insert(request.end(), pcm.begin(), pcm.end());
    return request;
}

void prepareStream(Fixture& fixture, const protocol::PcmSetParameters& selected)
{
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
    require(responseStatus(control(
                *fixture.device,
                bytes(pcmCommand(protocol::Request::pcmPrepare)))) ==
            protocol::Status::ok);
}

void command(Fixture& fixture, protocol::Request request)
{
    require(responseStatus(control(
                *fixture.device, bytes(pcmCommand(request)))) ==
            protocol::Status::ok);
}

/* The device runs when the transport finds the descriptor readable: it
 * drains the event source, then takes every completion. */
std::vector<std::pair<std::uint64_t, protocol::PcmStatus>> serve(
    Fixture& fixture)
{
    fixture.clock->fire();
    fixture.device->drainEventSource();
    std::vector<std::pair<std::uint64_t, protocol::PcmStatus>> completed;
    uml::virtio::VhostUserQueueCompletion completion;
    while (fixture.device->takeQueueCompletion(completion)) {
        completed.emplace_back(
            completion.requestId,
            load<protocol::PcmStatus>(completion.response));
    }
    return completed;
}

/* An output that cannot play must not make the guest's stream run fast:
 * each discarded period completes one period of real time after the one
 * before it, whatever the sink does. */
void pacesPlaybackWhileTheSinkCannotPlay()
{
    Fixture fixture;
    const auto selected = pacedParameters();
    prepareStream(fixture, selected);
    fixture.sink->enqueueResult = uml::audio::PlaybackEnqueueResult::discarded;
    const auto request = transferOf(selected.periodBytes);
    const auto start = fixture.clock->time;

    // Periods queued before the stream starts wait for it.
    for (const std::uint64_t id : {1, 2}) {
        require(fixture.device->processQueue(
                    protocol::transmitQueue, id, request,
                    sizeof(protocol::PcmStatus)).action ==
                uml::virtio::VhostUserQueueAction::defer);
    }
    require(!fixture.clock->armed);
    require(serve(fixture).empty());
    require(fixture.sink->enqueuedRequest == 0);

    command(fixture, protocol::Request::pcmStart);
    require(fixture.clock->armed && *fixture.clock->armed == start + 5ms);

    fixture.clock->time = start + 4900us;
    require(serve(fixture).empty());
    fixture.clock->time = start + 5ms;
    auto completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 1);
    require(completed[0].second.status == protocol::Status::ok);
    require(completed[0].second.latencyBytes == selected.periodBytes);
    require(*fixture.clock->armed == start + 10ms);

    // The guest refills as its periods complete; the clock keeps its pace.
    require(fixture.device->processQueue(
                protocol::transmitQueue, 3, request,
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);
    fixture.clock->time = start + 10ms;
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 2);
    require(completed[0].second.latencyBytes == selected.periodBytes);
    fixture.clock->time = start + 15ms;
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 3);
    require(completed[0].second.latencyBytes == 0);
    require(!fixture.clock->armed);

    // After an underrun the next period starts from now, not from the past.
    fixture.clock->time = start + 100ms;
    require(fixture.device->processQueue(
                protocol::transmitQueue, 4, request,
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);
    require(*fixture.clock->armed == start + 105ms);
    fixture.clock->time = start + 105ms;
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 4);
    require(fixture.sink->enqueuedRequest == 0);
}

/* A period that falls behind by more than one tick completes in order, one
 * per period, however late the device runs. */
void pacedPlaybackCatchesUpOnePeriodAtATime()
{
    Fixture fixture;
    const auto selected = pacedParameters();
    prepareStream(fixture, selected);
    fixture.sink->enqueueResult = uml::audio::PlaybackEnqueueResult::discarded;
    command(fixture, protocol::Request::pcmStart);
    const auto request = transferOf(selected.periodBytes);
    const auto start = fixture.clock->time;
    for (const std::uint64_t id : {1, 2, 3, 4}) {
        require(fixture.device->processQueue(
                    protocol::transmitQueue, id, request,
                    sizeof(protocol::PcmStatus)).action ==
                uml::virtio::VhostUserQueueAction::defer);
    }
    fixture.clock->time = start + 12ms;
    auto completed = serve(fixture);
    require(completed.size() == 2 && completed[0].first == 1 &&
            completed[1].first == 2);
    require(*fixture.clock->armed == start + 15ms);
    fixture.clock->time = start + 500ms;
    completed = serve(fixture);
    require(completed.size() == 2 && completed[0].first == 3 &&
            completed[1].first == 4);
    require(!fixture.clock->armed);
}

/* A sink that fails on a period it already accepted, a route change for
 * one, hands it back unplayed; the device paces it like any other. */
void pacesPeriodsTheSinkHandsBackUnplayed()
{
    Fixture fixture;
    const auto selected = pacedParameters();
    prepareStream(fixture, selected);
    command(fixture, protocol::Request::pcmStart);
    const auto start = fixture.clock->time;
    const auto request = transferOf(selected.periodBytes);
    for (const std::uint64_t id : {1, 2}) {
        require(fixture.device->processQueue(
                    protocol::transmitQueue, id, request,
                    sizeof(protocol::PcmStatus)).action ==
                uml::virtio::VhostUserQueueAction::defer);
    }
    require(!fixture.clock->armed);

    fixture.clock->time = start + 1ms;
    for (const std::uint64_t id : {1, 2}) {
        fixture.sink->completions.push_back({
            .requestId = id,
            .latencyBytes = 0,
            .outcome = uml::audio::PlaybackOutcome::discarded,
        });
    }
    require(serve(fixture).empty());
    require(*fixture.clock->armed == start + 6ms);
    fixture.clock->time = start + 6ms;
    auto completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 1 &&
            completed[0].second.status == protocol::Status::ok);
    fixture.clock->time = start + 11ms;
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 2);

    // A period the sink played keeps the sink's own latency.
    require(fixture.device->processQueue(
                protocol::transmitQueue, 5, request,
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);
    fixture.sink->completions.push_back({
        .requestId = 5,
        .latencyBytes = 1920,
    });
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 5 &&
            completed[0].second.latencyBytes == 1920);

    // A period the sink could not handle is an error, and is not delayed.
    require(fixture.device->processQueue(
                protocol::transmitQueue, 6, request,
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);
    fixture.sink->completions.push_back({
        .requestId = 6,
        .outcome = uml::audio::PlaybackOutcome::failed,
    });
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 6 &&
            completed[0].second.status == protocol::Status::ioError);
}

/* Stopping holds the queued periods and starting plays them from then on; a
 * release hands every one back at once, as the specification requires. */
void stoppedAndReleasedStreamsReturnPacedPeriods()
{
    Fixture fixture;
    const auto selected = pacedParameters();
    prepareStream(fixture, selected);
    fixture.sink->enqueueResult = uml::audio::PlaybackEnqueueResult::discarded;
    command(fixture, protocol::Request::pcmStart);
    const auto request = transferOf(selected.periodBytes);
    for (const std::uint64_t id : {1, 2, 3}) {
        require(fixture.device->processQueue(
                    protocol::transmitQueue, id, request,
                    sizeof(protocol::PcmStatus)).action ==
                uml::virtio::VhostUserQueueAction::defer);
    }
    fixture.clock->time += 5ms;
    require(serve(fixture).size() == 1);

    command(fixture, protocol::Request::pcmStop);
    require(!fixture.clock->armed);
    fixture.clock->time += 1s;
    require(serve(fixture).empty());

    command(fixture, protocol::Request::pcmStart);
    require(*fixture.clock->armed == fixture.clock->time + 5ms);
    fixture.clock->time += 5ms;
    auto completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 2);

    command(fixture, protocol::Request::pcmStop);
    command(fixture, protocol::Request::pcmRelease);
    require(!fixture.clock->armed);
    completed = serve(fixture);
    require(completed.size() == 1 && completed[0].first == 3 &&
            completed[0].second.status == protocol::Status::ok);

    // Nothing is outstanding, so the stream can be configured again.
    require(responseStatus(control(*fixture.device, bytes(selected))) ==
            protocol::Status::ok);
}

/* A device reset drops the schedule with everything else. */
void resetClearsThePacedSchedule()
{
    Fixture fixture;
    const auto selected = pacedParameters();
    prepareStream(fixture, selected);
    fixture.sink->enqueueResult = uml::audio::PlaybackEnqueueResult::discarded;
    command(fixture, protocol::Request::pcmStart);
    require(fixture.device->processQueue(
                protocol::transmitQueue, 1, transferOf(selected.periodBytes),
                sizeof(protocol::PcmStatus)).action ==
            uml::virtio::VhostUserQueueAction::defer);
    require(fixture.clock->armed.has_value());
    fixture.device->reset();
    require(!fixture.clock->armed);
    fixture.clock->time += 1s;
    require(serve(fixture).empty());
}

/* The null output discards every period, and the device paces it. */
void nullOutputPlaysNothingInRealTime()
{
    auto clock = std::make_shared<TestClock>();
    uml::audio::VirtioSoundDevice device(
        uml::audio::makeNullPlaybackSink(), clock);
    const auto selected = pacedParameters();
    require(responseStatus(control(device, bytes(selected))) ==
            protocol::Status::ok);
    for (const auto request : {protocol::Request::pcmPrepare,
                               protocol::Request::pcmStart}) {
        require(responseStatus(control(
                    device, bytes(pcmCommand(request)))) ==
                protocol::Status::ok);
    }
    const auto start = clock->time;
    const auto request = transferOf(selected.periodBytes);
    for (const std::uint64_t id : {1, 2}) {
        require(device.processQueue(
                    protocol::transmitQueue, id, request,
                    sizeof(protocol::PcmStatus)).action ==
                uml::virtio::VhostUserQueueAction::defer);
    }
    uml::virtio::VhostUserQueueCompletion completion;
    device.drainEventSource();
    require(!device.takeQueueCompletion(completion));
    clock->time = start + 5ms;
    device.drainEventSource();
    require(device.takeQueueCompletion(completion) &&
            completion.requestId == 1);
    require(load<protocol::PcmStatus>(completion.response).status ==
            protocol::Status::ok);
    require(!device.takeQueueCompletion(completion));
    clock->time = start + 10ms;
    device.drainEventSource();
    require(device.takeQueueCompletion(completion) &&
            completion.requestId == 2);
}

bool readable(int descriptor, std::chrono::milliseconds timeout)
{
    pollfd entry {descriptor, POLLIN, 0};
    return ::poll(&entry, 1, static_cast<int>(timeout.count())) == 1;
}

/* The production clock: a descriptor that reads ready when notified or when
 * the deadline passes, and never before. */
void productionClockWakesOnNotifyAndOnDeadline()
{
    const auto clock = uml::audio::makePeriodClock();
    const int descriptor = clock->descriptor();
    require(descriptor >= 0);
    require(!readable(descriptor, 0ms));

    clock->notify();
    require(readable(descriptor, 1s));
    clock->drain();
    require(!readable(descriptor, 0ms));

    const auto deadline = clock->now() + 30ms;
    clock->wakeAt(deadline);
    require(!readable(descriptor, 0ms));
    require(readable(descriptor, 5s), "the deadline never woke the clock");
    require(clock->now() >= deadline - 1ms);
    clock->drain();
    require(!readable(descriptor, 0ms));

    // A deadline in the past wakes at once; a cancelled one never does.
    clock->wakeAt(clock->now() - 1s);
    require(readable(descriptor, 5s));
    clock->drain();
    clock->wakeAt(clock->now() + 20ms);
    clock->cancel();
    require(!readable(descriptor, 200ms));

    // A later deadline replaces an earlier one.
    clock->wakeAt(clock->now() + 20ms);
    clock->wakeAt(clock->now() + 400ms);
    require(!readable(descriptor, 150ms));
    require(readable(descriptor, 5s));
    clock->drain();
}

} // namespace

int main()
{
    exposesOneStereoPlaybackStream();
    exposesFrontLeftAndRightChannelMap();
    rejectsMalformedAndOutOfOrderControlRequests();
    playbackDefersUntilTheAppleSinkConsumesAPeriod();
    acceptsTheVirtioPcmLifecycleTransitions();
    rejectsUnsupportedParametersAndRetriesWhenFull();
    refusesParametersWhileTransfersAreQueued();
    pacesPlaybackWhileTheSinkCannotPlay();
    pacedPlaybackCatchesUpOnePeriodAtATime();
    pacesPeriodsTheSinkHandsBackUnplayed();
    stoppedAndReleasedStreamsReturnPacedPeriods();
    resetClearsThePacedSchedule();
    nullOutputPlaysNothingInRealTime();
    productionClockWakesOnNotifyAndOnDeadline();
    return EXIT_SUCCESS;
}
