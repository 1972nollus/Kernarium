// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace uml::audio {

struct PlaybackParameters final {
    std::uint32_t bufferBytes {};
    std::uint32_t periodBytes {};
    std::uint32_t sampleRate {};
    std::uint8_t channels {};
    std::uint8_t bytesPerSample {};
};

struct PlaybackCompletion final {
    std::uint64_t requestId {};
    std::uint32_t latencyBytes {};
    bool success {};
};

enum class PlaybackEnqueueResult : std::uint8_t {
    retry,
    accepted,
    failed,
};

class PlaybackSink {
public:
    virtual ~PlaybackSink() = default;

    [[nodiscard]] virtual bool configure(
        const PlaybackParameters& parameters) = 0;
    [[nodiscard]] virtual bool prepare() = 0;
    [[nodiscard]] virtual bool start() = 0;
    virtual void stop() noexcept = 0;
    virtual void release() noexcept = 0;
    virtual void reset() noexcept = 0;
    [[nodiscard]] virtual PlaybackEnqueueResult enqueue(
        std::uint64_t requestId,
        std::span<const std::uint8_t> pcm) = 0;
    [[nodiscard]] virtual int eventDescriptor() const noexcept = 0;
    virtual void drainEventSource() = 0;
    [[nodiscard]] virtual bool takeCompletion(
        PlaybackCompletion& completion) = 0;
};

[[nodiscard]] std::unique_ptr<PlaybackSink> makeApplePlaybackSink();

} // namespace uml::audio
