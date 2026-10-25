// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_sound_device.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace uml::audio {
namespace {

using uml::virtio::fromBytes;
using uml::virtio::toBytes;

[[nodiscard]] uml::virtio::VhostUserQueueResult complete(
    std::vector<std::uint8_t> response)
{
    return {
        .action = uml::virtio::VhostUserQueueAction::complete,
        .response = std::move(response),
    };
}

[[nodiscard]] uml::virtio::VhostUserQueueResult status(
    protocol::Status value,
    std::size_t responseCapacity)
{
    if (responseCapacity < sizeof(protocol::Header))
        return complete({});
    return complete(toBytes(protocol::Header {
        .code = static_cast<std::uint32_t>(value),
    }));
}

[[nodiscard]] bool validStreamHeader(
    const protocol::PcmHeader& header) noexcept
{
    return header.streamId == protocol::streamId;
}

} // namespace

VirtioSoundDevice::VirtioSoundDevice(
    std::unique_ptr<PlaybackSink> sink,
    std::shared_ptr<PeriodClock> clock)
    : sink_(std::move(sink))
    , clock_(std::move(clock))
{
    if (!sink_)
        throw std::invalid_argument("Virtio Sound requires a playback sink");
    if (!clock_)
        throw std::invalid_argument("Virtio Sound requires a period clock");
}

VirtioSoundDevice::~VirtioSoundDevice() = default;

const char* VirtioSoundDevice::name() const noexcept
{
    return "Virtio Sound";
}

std::uint64_t VirtioSoundDevice::features() const noexcept
{
    return 0;
}

std::size_t VirtioSoundDevice::queueCount() const noexcept
{
    return protocol::queueCount;
}

std::vector<std::uint8_t> VirtioSoundDevice::configuration() const
{
    const protocol::Configuration configuration {
        .jacks = 0,
        .streams = 1,
        .channelMaps = 1,
        .controls = 0,
    };
    return toBytes(configuration);
}

void VirtioSoundDevice::writeConfiguration(
    std::uint32_t,
    std::uint32_t,
    std::span<const std::uint8_t> bytes)
{
    if (!bytes.empty())
        throw std::runtime_error("Virtio Sound configuration is read-only");
}

uml::virtio::VhostUserQueueResult VirtioSoundDevice::processControl(
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    if (request.size() < sizeof(protocol::Header))
        return status(protocol::Status::badMessage, responseCapacity);
    const auto header = fromBytes<protocol::Header>(request);
    switch (static_cast<protocol::Request>(header.code)) {
    case protocol::Request::pcmInfo: {
        if (request.size() != sizeof(protocol::QueryInfo))
            return status(protocol::Status::badMessage, responseCapacity);
        const auto query = fromBytes<protocol::QueryInfo>(request);
        if (query.startId != protocol::streamId || query.count != 1 ||
            query.size != sizeof(protocol::PcmInfo)) {
            return status(protocol::Status::badMessage, responseCapacity);
        }
        const protocol::PcmInfo info {
            .header = {.functionGroupNodeId = 0},
            .features = 0,
            .formats = 1ULL << static_cast<std::uint8_t>(
                protocol::PcmFormat::signed16),
            .rates = 1ULL << static_cast<std::uint8_t>(
                protocol::PcmRate::rate48000),
            .direction = protocol::Direction::output,
            .minimumChannels = protocol::channelCount,
            .maximumChannels = protocol::channelCount,
            .padding = {},
        };
        if (responseCapacity < sizeof(protocol::Header) + sizeof(info))
            return status(protocol::Status::badMessage, responseCapacity);
        return complete(toBytes(
            protocol::Header {
                .code = static_cast<std::uint32_t>(protocol::Status::ok),
            },
            info));
    }
    case protocol::Request::channelMapInfo: {
        if (request.size() != sizeof(protocol::QueryInfo))
            return status(protocol::Status::badMessage, responseCapacity);
        const auto query = fromBytes<protocol::QueryInfo>(request);
        if (query.startId != protocol::channelMapId || query.count != 1 ||
            query.size != sizeof(protocol::ChannelMapInfo)) {
            return status(protocol::Status::badMessage, responseCapacity);
        }
        protocol::ChannelMapInfo info {
            .header = {.functionGroupNodeId = 0},
            .direction = protocol::Direction::output,
            .channels = protocol::channelCount,
            .positions = {},
        };
        info.positions[0] = static_cast<std::uint8_t>(
            protocol::ChannelPosition::frontLeft);
        info.positions[1] = static_cast<std::uint8_t>(
            protocol::ChannelPosition::frontRight);
        if (responseCapacity < sizeof(protocol::Header) + sizeof(info))
            return status(protocol::Status::badMessage, responseCapacity);
        return complete(toBytes(
            protocol::Header {
                .code = static_cast<std::uint32_t>(protocol::Status::ok),
            },
            info));
    }
    case protocol::Request::pcmSetParameters: {
        if (request.size() != sizeof(protocol::PcmSetParameters))
            return status(protocol::Status::badMessage, responseCapacity);
        const auto parameters = fromBytes<protocol::PcmSetParameters>(request);
        if (!validStreamHeader(parameters.header))
            return status(protocol::Status::badMessage, responseCapacity);
        if (deferredTransfers_ != 0)
            return status(protocol::Status::ioError, responseCapacity);
        if (state_ == StreamState::running ||
            state_ == StreamState::stopped ||
            parameters.bufferBytes == 0 || parameters.periodBytes == 0 ||
            parameters.bufferBytes > protocol::maximumBufferBytes ||
            parameters.bufferBytes % parameters.periodBytes != 0 ||
            parameters.bufferBytes / parameters.periodBytes >
                protocol::maximumPeriods ||
            parameters.periodBytes % protocol::frameBytes != 0 ||
            parameters.features != 0 ||
            parameters.channels != protocol::channelCount ||
            parameters.format != protocol::PcmFormat::signed16 ||
            parameters.rate != protocol::PcmRate::rate48000 ||
            parameters.padding != 0) {
            return status(protocol::Status::notSupported, responseCapacity);
        }
        PlaybackParameters selected {
            .bufferBytes = parameters.bufferBytes,
            .periodBytes = parameters.periodBytes,
            .sampleRate = protocol::sampleRate,
            .channels = protocol::channelCount,
            .bytesPerSample = protocol::bytesPerSample,
        };
        if (!sink_->configure(selected))
            return status(protocol::Status::ioError, responseCapacity);
        parameters_ = selected;
        state_ = StreamState::configured;
        return status(protocol::Status::ok, responseCapacity);
    }
    case protocol::Request::pcmPrepare:
    case protocol::Request::pcmRelease:
    case protocol::Request::pcmStart:
    case protocol::Request::pcmStop: {
        if (request.size() != sizeof(protocol::PcmHeader))
            return status(protocol::Status::badMessage, responseCapacity);
        const auto pcm = fromBytes<protocol::PcmHeader>(request);
        if (!validStreamHeader(pcm))
            return status(protocol::Status::badMessage, responseCapacity);
        const auto command = static_cast<protocol::Request>(header.code);
        if (command == protocol::Request::pcmPrepare) {
            if ((state_ != StreamState::configured &&
                 state_ != StreamState::prepared &&
                 state_ != StreamState::released) ||
                !sink_->prepare()) {
                return status(protocol::Status::ioError, responseCapacity);
            }
            state_ = StreamState::prepared;
        } else if (command == protocol::Request::pcmStart) {
            if ((state_ != StreamState::prepared &&
                 state_ != StreamState::stopped) ||
                !sink_->start()) {
                return status(protocol::Status::ioError, responseCapacity);
            }
            state_ = StreamState::running;
            timePacedTransfers();
        } else if (command == protocol::Request::pcmStop) {
            if (state_ != StreamState::running)
                return status(protocol::Status::ioError, responseCapacity);
            sink_->stop();
            state_ = StreamState::stopped;
            for (auto& transfer : paced_)
                transfer.timed = false;
        } else {
            if (state_ != StreamState::prepared &&
                state_ != StreamState::stopped) {
                return status(protocol::Status::ioError, responseCapacity);
            }
            sink_->release();
            state_ = StreamState::released;
            // A release returns every queued period at once.
            for (const auto& transfer : paced_)
                ready_.push_back({transfer.requestId, 0});
            paced_.clear();
        }
        rearmClock();
        return status(protocol::Status::ok, responseCapacity);
    }
    default:
        return status(protocol::Status::notSupported, responseCapacity);
    }
}

uml::virtio::VhostUserQueueResult VirtioSoundDevice::processTransmit(
    std::uint64_t requestId,
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    if (responseCapacity < sizeof(protocol::PcmStatus))
        return complete({});
    if (request.size() < sizeof(protocol::PcmTransfer))
        return complete(toBytes(protocol::PcmStatus {
            .status = protocol::Status::badMessage,
            .latencyBytes = 0,
        }));
    const auto transfer = fromBytes<protocol::PcmTransfer>(request);
    const auto pcm = request.subspan(sizeof(transfer));
    if (transfer.streamId != protocol::streamId ||
        (state_ != StreamState::prepared &&
         state_ != StreamState::running) ||
        pcm.size() != parameters_.periodBytes) {
        return complete(toBytes(protocol::PcmStatus {
            .status = protocol::Status::badMessage,
            .latencyBytes = 0,
        }));
    }
    const auto enqueueResult = sink_->enqueue(requestId, pcm);
    if (enqueueResult == PlaybackEnqueueResult::retry)
        return {};
    if (enqueueResult == PlaybackEnqueueResult::failed) {
        return complete(toBytes(protocol::PcmStatus {
            .status = protocol::Status::ioError,
            .latencyBytes = 0,
        }));
    }
    ++deferredTransfers_;
    if (enqueueResult == PlaybackEnqueueResult::discarded)
        pace(requestId);
    return {
        .action = uml::virtio::VhostUserQueueAction::defer,
        .response = {},
    };
}

uml::virtio::VhostUserQueueResult VirtioSoundDevice::processQueue(
    std::uint32_t index,
    std::uint64_t requestId,
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    switch (index) {
    case protocol::controlQueue:
        return processControl(request, responseCapacity);
    case protocol::eventQueue:
        return {};
    case protocol::transmitQueue:
        return processTransmit(requestId, request, responseCapacity);
    case protocol::receiveQueue:
        return complete({});
    default:
        throw std::runtime_error("invalid Virtio Sound queue index");
    }
}

void VirtioSoundDevice::reset() noexcept
{
    sink_->reset();
    clock_->cancel();
    armed_.reset();
    state_ = StreamState::unconfigured;
    parameters_ = {};
    deferredTransfers_ = 0;
    paced_.clear();
    ready_.clear();
}

int VirtioSoundDevice::eventDescriptor() const noexcept
{
    return clock_->descriptor();
}

void VirtioSoundDevice::drainEventSource()
{
    clock_->drain();
    const auto now = clock_->now();
    // A deadline that has passed has fired; one still ahead is still armed,
    // and this wake was the sink's.
    if (armed_ && *armed_ <= now)
        armed_.reset();
    sink_->handleEvents();
    releasePacedTransfers(now);
    rearmClock();
}

bool VirtioSoundDevice::takeQueueCompletion(
    uml::virtio::VhostUserQueueCompletion& completion)
{
    releasePacedTransfers(clock_->now());
    for (;;) {
        std::uint32_t latencyBytes {};
        auto outcome = PlaybackOutcome::played;
        std::uint64_t requestId {};
        if (!ready_.empty()) {
            requestId = ready_.front().requestId;
            latencyBytes = ready_.front().latencyBytes;
            ready_.pop_front();
        } else {
            PlaybackCompletion playback;
            if (!sink_->takeCompletion(playback)) {
                rearmClock();
                return false;
            }
            if (playback.outcome == PlaybackOutcome::discarded &&
                state_ == StreamState::running) {
                // The output dropped the period; the guest still waits for
                // a period of real time to pass.
                pace(playback.requestId);
                continue;
            }
            requestId = playback.requestId;
            latencyBytes = playback.latencyBytes;
            outcome = playback.outcome;
        }
        if (deferredTransfers_ != 0)
            --deferredTransfers_;
        completion = {
            .requestId = requestId,
            .response = toBytes(protocol::PcmStatus {
                .status = outcome == PlaybackOutcome::failed
                    ? protocol::Status::ioError
                    : protocol::Status::ok,
                .latencyBytes = latencyBytes,
            }),
        };
        return true;
    }
}

std::chrono::nanoseconds VirtioSoundDevice::playbackPeriod() const noexcept
{
    const std::int64_t frames =
        parameters_.periodBytes / protocol::frameBytes;
    return std::chrono::nanoseconds(
        frames * 1'000'000'000LL / protocol::sampleRate);
}

void VirtioSoundDevice::pace(std::uint64_t requestId)
{
    PacedTransfer transfer {.requestId = requestId};
    if (state_ == StreamState::running) {
        playHead_ = std::max(playHead_, clock_->now()) + playbackPeriod();
        transfer.due = playHead_;
        transfer.timed = true;
    }
    paced_.push_back(transfer);
    rearmClock();
}

/* A stream that starts plays what it has queued from now on, one period
 * after another. */
void VirtioSoundDevice::timePacedTransfers()
{
    playHead_ = clock_->now();
    for (auto& transfer : paced_) {
        playHead_ += playbackPeriod();
        transfer.due = playHead_;
        transfer.timed = true;
    }
}

void VirtioSoundDevice::releasePacedTransfers(ClockTime now)
{
    while (!paced_.empty() && paced_.front().timed &&
           paced_.front().due <= now) {
        const auto requestId = paced_.front().requestId;
        paced_.pop_front();
        ready_.push_back({
            requestId,
            static_cast<std::uint32_t>(paced_.size()) *
                parameters_.periodBytes,
        });
    }
}

void VirtioSoundDevice::rearmClock()
{
    std::optional<ClockTime> next;
    if (!paced_.empty() && paced_.front().timed)
        next = paced_.front().due;
    if (next == armed_)
        return;
    armed_ = next;
    if (next)
        clock_->wakeAt(*next);
    else
        clock_->cancel();
}

} // namespace uml::audio
