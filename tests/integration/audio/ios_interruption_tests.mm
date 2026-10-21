// SPDX-License-Identifier: GPL-2.0-or-later
//
// Plays a stream through the Apple playback sink in the iOS Simulator and posts
// the audio-session notifications that iOS posts when another app takes the
// session and gives it back, then checks that playback stops while the session
// is interrupted, resumes when the system lets it resume, and never wedges the
// sink. run_ios_interruption.py builds this file with the sink for the
// Simulator and runs it; the notifications are posted rather than produced by
// iOS because the Simulator does not interrupt audio sessions.

#include "playback_sink.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <poll.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>

namespace {

using namespace std::chrono_literals;
using uml::audio::PlaybackEnqueueResult;

constexpr std::uint32_t frames = 1024;
constexpr std::uint32_t channels = 2;
constexpr std::uint32_t periodBytes = frames * channels * sizeof(std::int16_t);
// A period lasts about 21 ms, so a stream that plays completes about 47 a
// second. Anything near a tenth of that is playing; none at all is paused.
constexpr std::uint64_t playingPerWindow = 10;

int failures = 0;

void check(bool passed, const char* name, const std::string& detail = {})
{
    failures += passed ? 0 : 1;
    std::printf("%s %s%s%s\n", passed ? "PASS" : "FAIL", name,
        detail.empty() ? "" : ": ", detail.c_str());
    std::fflush(stdout);
}

std::string counted(std::uint64_t completions)
{
    return std::to_string(completions) + " periods completed";
}

// The device thread of a Virtio sound stream: it keeps the sink's period
// queue full, sleeps on the sink's event descriptor and collects completions.
class Stream final {
public:
    explicit Stream(uml::audio::PlaybackSink& sink)
        : sink_(sink)
    {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            // Eight cycles per period, so periods join without a click.
            const auto sample = static_cast<std::int16_t>(
                600.0 * std::sin(2.0 * M_PI * 8.0 * frame / frames));
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                std::memcpy(period_.data() +
                        (frame * channels + channel) * sizeof(sample),
                    &sample, sizeof(sample));
            }
        }
    }

    [[nodiscard]] bool open()
    {
        if (!sink_.configure({.bufferBytes = periodBytes * 4,
                .periodBytes = periodBytes, .sampleRate = 48000,
                .channels = channels, .bytesPerSample = 2}) ||
            !sink_.prepare())
            return false;
        fill();
        return sink_.start();
    }

    // What the guest does when it closes the stream.
    void close()
    {
        sink_.stop();
        sink_.release();
        sink_.drainEventSource();
        collect();
    }

    void pump(std::chrono::milliseconds span)
    {
        const auto deadline = std::chrono::steady_clock::now() + span;
        do {
            fill();
            pollfd descriptor {sink_.eventDescriptor(), POLLIN, 0};
            (void)poll(&descriptor, 1, 10);
            sink_.drainEventSource();
            collect();
        } while (std::chrono::steady_clock::now() < deadline);
    }

    // Completions over `span`, after `settle` for the state change under test.
    [[nodiscard]] std::uint64_t window(
        std::chrono::milliseconds settle, std::chrono::milliseconds span)
    {
        pump(settle);
        const auto before = completed_;
        pump(span);
        return completed_ - before;
    }

    [[nodiscard]] bool healthy() const { return !failed_ && rejected_ == 0; }

private:
    void fill()
    {
        for (;;) {
            const auto result = sink_.enqueue(nextRequest_, period_);
            if (result == PlaybackEnqueueResult::failed)
                failed_ = true;
            if (result != PlaybackEnqueueResult::accepted)
                return;
            ++nextRequest_;
        }
    }

    void collect()
    {
        uml::audio::PlaybackCompletion completion;
        while (sink_.takeCompletion(completion)) {
            ++completed_;
            rejected_ += completion.success ? 0 : 1;
        }
    }

    uml::audio::PlaybackSink& sink_;
    std::array<std::uint8_t, periodBytes> period_ {};
    std::uint64_t nextRequest_ {1};
    std::uint64_t completed_ {};
    std::uint64_t rejected_ {};
    bool failed_ {};
};

} // namespace

// iOS creates the contexts its notifications carry and offers no initializer,
// so each is a subclass that answers the reads the sink makes.
@interface InterruptionContextDouble : AVAudioSessionInterruptionContext
@end

@implementation InterruptionContextDouble
- (AVAudioSessionInterruptionReason)reason
{
    return AVAudioSessionInterruptionReasonDefault;
}
@end

@interface DeactivationContextDouble : AVAudioSessionDeactivationContext
@property(nonatomic) AVAudioSessionDeactivationSource sourceDouble;
@property(nonatomic, strong) AVAudioSessionInterruptionContext* interruptionDouble;
@end

@implementation DeactivationContextDouble
- (AVAudioSessionDeactivationSource)source
{
    return self.sourceDouble;
}

- (AVAudioSessionInterruptionContext*)interruptionContext
{
    return self.interruptionDouble;
}
@end

@interface ResumptionContextDouble : AVAudioSessionResumptionContext
@property(nonatomic) AVAudioSessionResumptionRecommendation recommendationDouble;
@end

@implementation ResumptionContextDouble
- (AVAudioSessionResumptionRecommendation)recommendation
{
    return self.recommendationDouble;
}
@end

namespace {

template <class Context> Context* make()
{
    return static_cast<Context*>(class_createInstance([Context class], 0));
}

// What iOS 27 posts when the session becomes inactive: for the app's own
// deactivation, or for the system's, which carries an interruption context
// when another app caused it.
void postDeactivation(
    AVAudioSessionDeactivationSource source, bool interrupted)
{
    auto* context = make<DeactivationContextDouble>();
    context.sourceDouble = source;
    context.interruptionDouble =
        interrupted ? make<InterruptionContextDouble>() : nil;
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionDidBecomeInactiveNotification
                      object:[AVAudioSession sharedInstance]
                    userInfo:@{
                        AVAudioSessionDeactivationContextKey : context
                    }];
}

void postInterruptionBegan()
{
    postDeactivation(AVAudioSessionDeactivationSourceSystem, true);
}

// What iOS 27 posts when the interrupter lets go, with the system's advice on
// resuming.
void postInterruptionEnded(bool shouldResume)
{
    auto* context = make<ResumptionContextDouble>();
    context.recommendationDouble = shouldResume ?
        AVAudioSessionResumptionRecommendationShouldResume :
        AVAudioSessionResumptionRecommendationShouldNotResume;
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionResumptionRecommendationNotification
                      object:[AVAudioSession sharedInstance]
                    userInfo:@{
                        AVAudioSessionResumptionContextKey : context
                    }];
}

void interruptionPausesAndResumes()
{
    auto sink = uml::audio::makeApplePlaybackSink();
    Stream stream(*sink);
    check(stream.open(), "the stream opens");
    auto played = stream.window(500ms, 1500ms);
    check(played >= playingPerWindow, "the stream plays", counted(played));

    postDeactivation(AVAudioSessionDeactivationSourceApp, false);
    played = stream.window(400ms, 1500ms);
    check(played >= playingPerWindow,
        "the app's own deactivation leaves playback running",
        counted(played));
    postDeactivation(AVAudioSessionDeactivationSourceSystem, false);
    played = stream.window(400ms, 1500ms);
    check(played >= playingPerWindow,
        "a system deactivation that interrupts nothing leaves playback running",
        counted(played));

    postInterruptionBegan();
    auto paused = stream.window(400ms, 800ms);
    check(paused == 0, "an interruption pauses playback", counted(paused));
    check(stream.healthy(), "a paused sink refuses nothing");

    postInterruptionEnded(true);
    auto resumed = stream.window(400ms, 1500ms);
    check(resumed >= playingPerWindow,
        "an end that advises resuming resumes playback", counted(resumed));

    postInterruptionBegan();
    paused = stream.window(400ms, 800ms);
    check(paused == 0, "a second interruption pauses playback",
        counted(paused));
    postInterruptionEnded(false);
    resumed = stream.window(400ms, 1500ms);
    check(resumed >= playingPerWindow,
        "an end that does not advise resuming resumes playback too",
        counted(resumed));

    postInterruptionEnded(true);
    played = stream.window(400ms, 1500ms);
    check(played >= playingPerWindow,
        "an end without an interruption leaves playback running",
        counted(played));

    postInterruptionBegan();
    postInterruptionEnded(true);
    played = stream.window(400ms, 1500ms);
    check(played >= playingPerWindow,
        "an interruption that ends before the sink looks leaves playback "
        "running",
        counted(played));
    check(stream.healthy(), "the sink never failed a period");
    stream.close();
}

void restartDuringInterruption()
{
    auto sink = uml::audio::makeApplePlaybackSink();
    Stream stream(*sink);
    check(stream.open(), "the stream opens again");
    (void)stream.window(300ms, 300ms);

    postInterruptionBegan();
    auto paused = stream.window(400ms, 800ms);
    check(paused == 0, "an interruption pauses the new stream",
        counted(paused));
    stream.close();
    check(stream.open(), "the guest reopens the stream while interrupted");
    const auto played = stream.window(500ms, 1500ms);
    check(played >= playingPerWindow,
        "a stream the guest restarts plays without a resume",
        counted(played));
    check(stream.healthy(), "the restart failed no period");
    stream.close();
}

void interruptionBeforeStart()
{
    auto sink = uml::audio::makeApplePlaybackSink();
    Stream stream(*sink);
    postInterruptionBegan();
    postInterruptionEnded(true);
    check(stream.open(), "a stream opens after an interruption");
    const auto played = stream.window(500ms, 1500ms);
    check(played >= playingPerWindow,
        "a stream that opens after an interruption plays", counted(played));
    stream.close();
}

void destroyWhileInterrupted()
{
    {
        auto sink = uml::audio::makeApplePlaybackSink();
        Stream stream(*sink);
        check(stream.open(), "a stream opens before the sink is destroyed");
        (void)stream.window(300ms, 300ms);
        postInterruptionBegan();
        (void)stream.window(300ms, 300ms);
    }
    // The observers went with the sink.
    postInterruptionEnded(true);
    postInterruptionBegan();
    postInterruptionEnded(false);

    auto sink = uml::audio::makeApplePlaybackSink();
    Stream stream(*sink);
    check(stream.open(), "a new sink opens after a destroyed one");
    const auto played = stream.window(500ms, 1500ms);
    check(played >= playingPerWindow,
        "a new sink plays after notifications for a destroyed one",
        counted(played));
    stream.close();
}

} // namespace

int main()
{
    @autoreleasepool {
        interruptionPausesAndResumes();
        restartDuringInterruption();
        interruptionBeforeStart();
        destroyWhileInterrupted();
    }
    std::printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL",
        failures);
    return failures == 0 ? 0 : 1;
}
