// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_sound_device.hpp"

#include "protocol.hpp"

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

VirtioSoundDevice::VirtioSoundDevice(std::unique_ptr<PlaybackSink> sink)
    : sink_(std::move(sink))
{
    if (!sink_)
        throw std::invalid_argument("Virtio Sound requires a playback sink");
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
        } else if (command == protocol::Request::pcmStop) {
            if (state_ != StreamState::running)
                return status(protocol::Status::ioError, responseCapacity);
            sink_->stop();
            state_ = StreamState::stopped;
        } else {
            if (state_ != StreamState::prepared &&
                state_ != StreamState::stopped) {
                return status(protocol::Status::ioError, responseCapacity);
            }
            sink_->release();
            state_ = StreamState::released;
        }
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
    state_ = StreamState::unconfigured;
    parameters_ = {};
    deferredTransfers_ = 0;
}

int VirtioSoundDevice::eventDescriptor() const noexcept
{
    return sink_->eventDescriptor();
}

void VirtioSoundDevice::drainEventSource()
{
    sink_->drainEventSource();
}

bool VirtioSoundDevice::takeQueueCompletion(
    uml::virtio::VhostUserQueueCompletion& completion)
{
    PlaybackCompletion playback;
    if (!sink_->takeCompletion(playback))
        return false;
    if (deferredTransfers_ != 0)
        --deferredTransfers_;
    completion = {
        .requestId = playback.requestId,
        .response = toBytes(protocol::PcmStatus {
            .status = playback.success ? protocol::Status::ok
                                       : protocol::Status::ioError,
            .latencyBytes = playback.latencyBytes,
        }),
    };
    return true;
}

} // namespace uml::audio
