// SPDX-License-Identifier: GPL-2.0-or-later

#include "protocol.hpp"
#include "virtio_sound_device.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace protocol = uml::audio::protocol;

void require(bool condition)
{
    if (!condition)
        std::abort();
}

template<typename... Values>
std::vector<std::uint8_t> bytes(const Values&... values)
{
    const std::size_t size = (sizeof(values) + ... + 0U);
    std::vector<std::uint8_t> result(size);
    std::size_t offset = 0;
    const auto append = [&](const auto& value) {
        std::memcpy(result.data() + offset, &value, sizeof(value));
        offset += sizeof(value);
    };
    (append(values), ...);
    return result;
}

template<typename Value>
Value load(std::span<const std::uint8_t> source, std::size_t offset = 0)
{
    static_assert(std::is_trivially_copyable_v<Value>);
    require(offset <= source.size() && sizeof(Value) <= source.size() - offset);
    Value result {};
    std::memcpy(&result, source.data() + offset, sizeof(result));
    return result;
}

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

    int eventDescriptor() const noexcept override { return -1; }
    void drainEventSource() override { ++drainCount; }

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
    {
        auto owned = std::make_unique<TestSink>();
        sink = owned.get();
        device = std::make_unique<uml::audio::VirtioSoundDevice>(
            std::move(owned));
    }

    TestSink* sink {};
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
        .success = true,
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

} // namespace

int main()
{
    exposesOneStereoPlaybackStream();
    exposesFrontLeftAndRightChannelMap();
    rejectsMalformedAndOutOfOrderControlRequests();
    playbackDefersUntilTheAppleSinkConsumesAPeriod();
    acceptsTheVirtioPcmLifecycleTransitions();
    rejectsUnsupportedParametersAndRetriesWhenFull();
    return EXIT_SUCCESS;
}
