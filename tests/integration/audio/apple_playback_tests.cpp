// SPDX-License-Identifier: GPL-2.0-or-later
#include "period_clock.hpp"
#include "playback_sink.hpp"

#include <poll.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void playback(uml::audio::PlaybackSink& sink, uml::audio::PeriodClock& clock)
{
    constexpr std::uint32_t frames = 1024;
    constexpr std::uint32_t periodBytes = frames * 2 * sizeof(std::int16_t);
    constexpr std::uint64_t periods = 32;
    require(sink.configure({.bufferBytes = periodBytes * 4,
                .periodBytes = periodBytes, .sampleRate = 48000,
                .channels = 2, .bytesPerSample = 2}),
            "Apple audio format configuration failed");
    require(sink.prepare(), "Apple audio preparation failed");
    std::array<std::uint8_t, periodBytes> pcm {};
    std::uint64_t queued {};
    std::set<std::uint64_t> completed;
    bool started {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (completed.size() != periods && std::chrono::steady_clock::now() < deadline) {
        while (queued != periods) {
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                for (std::uint32_t channel = 0; channel < 2; ++channel) {
                    const auto phase = static_cast<double>(queued * frames + frame) /
                        48000.0 * (channel == 0 ? 220.0 : 440.0);
                    const auto sample = static_cast<std::int16_t>(
                        1000.0 * std::sin(phase * 6.283185307179586));
                    std::memcpy(pcm.data() + (frame * 2 + channel) * sizeof(sample),
                            &sample, sizeof(sample));
                }
            }
            const auto result = sink.enqueue(queued + 1, pcm);
            require(result != uml::audio::PlaybackEnqueueResult::failed &&
                    result != uml::audio::PlaybackEnqueueResult::discarded,
                    "Apple audio rejected a PCM period");
            if (result == uml::audio::PlaybackEnqueueResult::retry)
                break;
            ++queued;
        }
        if (!started) {
            require(sink.start(), "Apple audio output engine failed to start");
            started = true;
        }
        pollfd descriptor {clock.descriptor(), POLLIN, 0};
        require(poll(&descriptor, 1, 50) >= 0, "Apple audio completion poll failed");
        clock.drain();
        sink.handleEvents();
        uml::audio::PlaybackCompletion completion;
        while (sink.takeCompletion(completion)) {
            require(completion.outcome == uml::audio::PlaybackOutcome::played &&
                    completion.requestId > 0 &&
                    completion.requestId <= queued, "invalid Apple audio completion");
            require(completed.insert(completion.requestId).second,
                    "duplicate Apple audio completion");
        }
    }
    require(completed.size() == periods, "Apple audio playback did not complete");
    sink.stop();
    sink.release();
    std::printf("PASS Apple playback: %zu stereo PCM periods consumed\n", completed.size());
}
} // namespace

int main()
{
    try {
        const auto clock = uml::audio::makePeriodClock();
        auto sink = uml::audio::makeApplePlaybackSink(clock);
        playback(*sink, *clock);
        playback(*sink, *clock);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
