// SPDX-License-Identifier: GPL-2.0-or-later
//
// Plays a stream through the Apple playback sink in the iOS Simulator and posts
// the audio-session notifications that iOS posts when another app takes the
// session and gives it back. While the session is interrupted the sink must
// play nothing yet keep the guest's clock running; it must take the session
// back when the system says so, or when no other app plays primary audio even
// though iOS never says so, and never while another app does; and it must
// never wedge. run_ios_interruption.py builds this file with the sink for the
// Simulator and runs it. The notifications are posted rather than produced by
// iOS because the Simulator does not interrupt audio sessions, and the
// session's activation, its hint about other apps' audio and the player's
// scheduling are observed and driven through hooks on AVFAudio's methods.

#include "playback_sink.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <poll.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using uml::audio::PlaybackEnqueueResult;

constexpr std::uint32_t frames = 1024;
constexpr std::uint32_t channels = 2;
constexpr std::uint32_t periodBytes = frames * channels * sizeof(std::int16_t);
// A period lasts about 21 ms, so a stream that plays completes about 47 a
// second. Anything near a tenth of that is playing.
constexpr std::uint64_t playingPerWindow = 10;
constexpr double nominalRate = 48000.0 / frames;
// The sink asks about other apps' audio once a second.
constexpr auto recoveryLimit = 2500ms;

int failures = 0;

void check(bool passed, const char* name, const std::string& detail = {})
{
    failures += passed ? 0 : 1;
    std::printf("%s %s%s%s\n", passed ? "PASS" : "FAIL", name,
        detail.empty() ? "" : ": ", detail.c_str());
    std::fflush(stdout);
}

// What the tests drive and observe on AVFAudio. The Simulator's session
// answers "no other app is playing" and succeeds every activation, so the
// hooks stand in for the apps and the system that iOS interrupts a device with.
std::atomic<bool> otherAudioPlaying {};
std::atomic<std::uint64_t> activations {};
std::atomic<int> activationsToFail {};
std::atomic<std::uint64_t> scheduledPeriods {};

void installHooks()
{
    Method hint = class_getInstanceMethod([AVAudioSession class],
        @selector(secondaryAudioShouldBeSilencedHint));
    method_setImplementation(hint, imp_implementationWithBlock(^BOOL(id) {
        return otherAudioPlaying.load();
    }));

    Method activate = class_getInstanceMethod(
        [AVAudioSession class], @selector(setActive:error:));
    using Activate = BOOL (*)(id, SEL, BOOL, NSError* __autoreleasing*);
    static const auto activateOriginal =
        reinterpret_cast<Activate>(method_getImplementation(activate));
    method_setImplementation(activate,
        imp_implementationWithBlock(^BOOL(
            id session, BOOL active, NSError* __autoreleasing* error) {
          if (active) {
              ++activations;
              if (activationsToFail.load() > 0) {
                  --activationsToFail;
                  if (error != nullptr) {
                      *error = [NSError errorWithDomain:NSOSStatusErrorDomain
                                                   code:560557684
                                               userInfo:nil];
                  }
                  return NO;
              }
          }
          return activateOriginal(
              session, @selector(setActive:error:), active, error);
        }));

    Method schedule = class_getInstanceMethod([AVAudioPlayerNode class],
        @selector(scheduleBuffer:completionCallbackType:completionHandler:));
    using Schedule = void (*)(id, SEL, AVAudioPCMBuffer*,
        AVAudioPlayerNodeCompletionCallbackType, AVAudioNodeCompletionHandler);
    static const auto scheduleOriginal =
        reinterpret_cast<Schedule>(method_getImplementation(schedule));
    method_setImplementation(schedule,
        imp_implementationWithBlock(^(id player, AVAudioPCMBuffer* buffer,
            AVAudioPlayerNodeCompletionCallbackType type,
            AVAudioNodeCompletionHandler handler) {
          ++scheduledPeriods;
          scheduleOriginal(player,
              @selector(scheduleBuffer:completionCallbackType:completionHandler:),
              buffer, type, handler);
        }));
}

// What the guest saw, and what reached the player and the session, over a span.
struct Observed final {
    std::uint64_t completed {};
    std::uint64_t scheduled {};
    double seconds {};

    [[nodiscard]] double rate() const { return completed / seconds; }
    // The guest's frame position advances at the rate of real audio.
    [[nodiscard]] bool nominal(double tolerance = 0.02) const
    {
        return std::abs(rate() / nominalRate - 1.0) <= tolerance;
    }
    [[nodiscard]] bool playing() const
    {
        return completed >= playingPerWindow && scheduled >= playingPerWindow;
    }
    // The guest's clock runs but nothing reaches the player.
    [[nodiscard]] bool muted(double tolerance = 0.02) const
    {
        return scheduled == 0 && nominal(tolerance);
    }
    [[nodiscard]] std::string text() const
    {
        char buffer[160];
        std::snprintf(buffer, sizeof buffer,
            "%llu periods completed in %.2f s (%.2f/s, nominal %.2f/s), "
            "%llu scheduled",
            static_cast<unsigned long long>(completed), seconds, rate(),
            nominalRate, static_cast<unsigned long long>(scheduled));
        return buffer;
    }
};

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

    // What the guest does when it suspends and resumes the stream.
    void pause() { sink_.stop(); }
    [[nodiscard]] bool resume() { return sink_.start(); }

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

    // What happened over `span`, after `settle` for the state change under
    // test.
    [[nodiscard]] Observed window(
        std::chrono::milliseconds settle, std::chrono::milliseconds span)
    {
        if (settle > 0ms)
            pump(settle);
        const auto begun = std::chrono::steady_clock::now();
        const auto completed = completed_;
        const auto scheduled = scheduledPeriods.load();
        pump(span);
        return {.completed = completed_ - completed,
            .scheduled = scheduledPeriods.load() - scheduled,
            .seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - begun).count()};
    }

    // How long until a period reaches the player again, or `limit`.
    [[nodiscard]] std::chrono::milliseconds untilPlayed(
        std::chrono::milliseconds limit)
    {
        const auto begun = std::chrono::steady_clock::now();
        const auto scheduled = scheduledPeriods.load();
        while (scheduledPeriods.load() == scheduled &&
            std::chrono::steady_clock::now() - begun < limit)
            pump(20ms);
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begun);
    }

    // Whether the sink wakes the device thread within `span`: a muted sink
    // does, on its timer, until the stream stops.
    [[nodiscard]] bool wakes(std::chrono::milliseconds span)
    {
        sink_.drainEventSource();
        collect();
        std::this_thread::sleep_for(100ms);
        sink_.drainEventSource();
        collect();
        pollfd descriptor {sink_.eventDescriptor(), POLLIN, 0};
        return poll(&descriptor, 1, static_cast<int>(span.count())) > 0;
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

void interruptionMutesAndResumes()
{
    auto sink = uml::audio::makeApplePlaybackSink();
    Stream stream(*sink);
    check(stream.open(), "the stream opens");
    auto seen = stream.window(500ms, 1500ms);
    check(seen.playing(), "the stream plays", seen.text());

    postDeactivation(AVAudioSessionDeactivationSourceApp, false);
    seen = stream.window(400ms, 1500ms);
    check(seen.playing(), "the app's own deactivation leaves playback running",
        seen.text());
    postDeactivation(AVAudioSessionDeactivationSourceSystem, false);
    seen = stream.window(400ms, 1500ms);
    check(seen.playing(),
        "a system deactivation that interrupts nothing leaves playback running",
        seen.text());

    // The interrupter plays on, so the sink must not take the session back.
    otherAudioPlaying = true;
    auto mark = activations.load();
    postInterruptionBegan();
    seen = stream.window(400ms, 4000ms);
    check(seen.scheduled == 0, "an interruption stops playback",
        seen.text());
    check(seen.nominal(),
        "the guest's frame position keeps its rate across the mute",
        seen.text());
    check(activations.load() == mark,
        "a muted sink leaves the session to the app that plays", seen.text());
    check(stream.healthy(), "a muted sink refuses nothing");

    // The end advises resuming; the hint has not caught up with it yet.
    postInterruptionEnded(true);
    seen = stream.window(400ms, 1500ms);
    check(seen.playing(), "an end that advises resuming resumes playback",
        seen.text());
    check(activations.load() == mark + 1,
        "the sink takes the session back once", seen.text());
    check(stream.healthy(), "the resume failed no period");

    postInterruptionBegan();
    seen = stream.window(400ms, 1000ms);
    check(seen.scheduled == 0, "a second interruption stops playback",
        seen.text());
    postInterruptionEnded(false);
    seen = stream.window(400ms, 1500ms);
    check(seen.playing(),
        "an end that does not advise resuming resumes playback too",
        seen.text());

    otherAudioPlaying = false;
    mark = activations.load();
    postInterruptionEnded(true);
    seen = stream.window(400ms, 1500ms);
    check(seen.playing() && activations.load() == mark,
        "an end without an interruption leaves playback running",
        seen.text());

    postInterruptionBegan();
    postInterruptionEnded(true);
    seen = stream.window(400ms, 1500ms);
    check(seen.playing(),
        "an interruption that ends before the sink looks leaves playback "
        "running",
        seen.text());
    check(stream.healthy(), "the sink never failed a period");
    stream.close();
}

// iOS posts no end when the interrupter stops or is killed, and the sink has
// to notice by itself.
void beganWithNoEnd()
{
    auto sink = uml::audio::makeApplePlaybackSink();
    Stream stream(*sink);
    otherAudioPlaying = false;
    check(stream.open(), "the stream opens for the interruption without an end");
    auto seen = stream.window(500ms, 1000ms);
    check(seen.playing(), "the stream plays before the interrupter", seen.text());

    otherAudioPlaying = true;
    const auto mark = activations.load();
    postInterruptionBegan();
    // The periods the player held when it was cut off complete on the clock
    // too, not all at once: 120 ms hold five or six periods, and a burst of the
    // four the player held would make nine or ten.
    seen = stream.window(0ms, 120ms);
    check(seen.completed >= 3 && seen.completed <= 8 && seen.scheduled == 0,
        "the periods the player held complete on the mute clock, not at once",
        seen.text());
    seen = stream.window(0ms, 4000ms);
    check(seen.muted(),
        "an interruption that has no end mutes playback at the guest's rate",
        seen.text());
    check(activations.load() == mark,
        "the sink does not take the session back while the interrupter plays",
        seen.text());

    // The interrupter stops without a word.
    otherAudioPlaying = false;
    const auto taken = stream.untilPlayed(recoveryLimit);
    check(taken < recoveryLimit && activations.load() == mark + 1,
        "playback returns once no other app plays, by one activation",
        std::to_string(taken.count()) + " ms");
    seen = stream.window(300ms, 1500ms);
    check(seen.playing(),
        "the stream plays on after it takes the session back", seen.text());
    check(stream.healthy(), "the mute and the recovery failed no period");

    // The session is still held elsewhere when the hint clears: every
    // attempt that fails leaves the sink muted and tries again.
    otherAudioPlaying = true;
    postInterruptionBegan();
    seen = stream.window(400ms, 1000ms);
    check(seen.muted(), "a second interruption without an end mutes again",
        seen.text());
    activationsToFail = 2;
    const auto before = activations.load();
    otherAudioPlaying = false;
    const auto retaken = stream.untilPlayed(5000ms);
    check(retaken < 5000ms && activations.load() - before == 3,
        "a failed activation leaves the sink muted and tried again",
        std::to_string(retaken.count()) + " ms, " +
            std::to_string(activations.load() - before) + " attempts");
    seen = stream.window(300ms, 1000ms);
    check(seen.playing() && stream.healthy(),
        "the stream plays after the retries", seen.text());
    stream.close();
}

// Neither a stream the guest restarts nor a route change may take the session
// from the app that holds it.
void noActivationWhileInterrupted()
{
    auto sink = uml::audio::makeApplePlaybackSink();
    Stream stream(*sink);
    otherAudioPlaying = false;
    check(stream.open(), "the stream opens for the interrupted restarts");
    (void)stream.window(300ms, 300ms);
    otherAudioPlaying = true;
    postInterruptionBegan();
    (void)stream.window(400ms, 500ms);
    const auto mark = activations.load();

    // The guest suspends and resumes the stream, as when the app returns to
    // the foreground.
    stream.pause();
    check(stream.resume(), "the guest resumes the stream while interrupted");
    auto seen = stream.window(400ms, 3000ms);
    check(seen.muted() && activations.load() == mark,
        "a resumed stream joins the mute instead of taking the session",
        seen.text());

    // The audio route changes.
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionRouteChangeNotification
                      object:[AVAudioSession sharedInstance]];
    seen = stream.window(400ms, 3000ms);
    check(seen.muted() && activations.load() == mark,
        "a route change does not take the session from the app that plays",
        seen.text());
    check(stream.healthy(), "the muted restarts failed no period");

    // The guest suspends the stream, and no timer outlives it.
    stream.pause();
    check(!stream.wakes(2500ms), "a stopped stream leaves no timer running");

    // Resumed when nobody plays: the stale interruption gives way at once.
    otherAudioPlaying = false;
    check(stream.resume(), "the guest resumes the stream once nobody plays");
    check(activations.load() == mark + 1,
        "a resumed stream takes the session at once when no other app plays");
    seen = stream.window(100ms, 1500ms);
    check(seen.playing(), "and plays", seen.text());
    stream.close();
}

// The interruption arrives while the stream is stopped, and the stream opens
// into it.
void interruptionBeforeStart()
{
    auto sink = uml::audio::makeApplePlaybackSink();
    Stream stream(*sink);
    postInterruptionBegan();
    postInterruptionEnded(true);
    check(stream.open(), "a stream opens after an interruption");
    auto seen = stream.window(500ms, 1500ms);
    check(seen.playing(), "a stream that opens after an interruption plays",
        seen.text());
    stream.close();

    otherAudioPlaying = true;
    postInterruptionBegan();
    const auto mark = activations.load();
    check(stream.open(), "a stream opens while another app plays");
    seen = stream.window(500ms, 3000ms);
    check(seen.muted() && activations.load() == mark,
        "a stream that opens during an interruption is muted", seen.text());
    otherAudioPlaying = false;
    const auto taken = stream.untilPlayed(recoveryLimit);
    check(taken < recoveryLimit, "the muted stream plays once nobody plays",
        std::to_string(taken.count()) + " ms");
    stream.close();
}

void restartDuringInterruption()
{
    auto sink = uml::audio::makeApplePlaybackSink();
    Stream stream(*sink);
    otherAudioPlaying = false;
    check(stream.open(), "the stream opens again");
    (void)stream.window(300ms, 300ms);

    otherAudioPlaying = true;
    postInterruptionBegan();
    auto seen = stream.window(400ms, 1000ms);
    check(seen.muted(), "an interruption mutes the new stream", seen.text());
    // Closing the stream releases the session, so the interruption is over
    // and the guest opening a stream is the user's wish to play.
    stream.close();
    check(stream.open(), "the guest reopens the stream while interrupted");
    seen = stream.window(500ms, 1500ms);
    check(seen.playing(), "a stream the guest restarts plays without a resume",
        seen.text());
    check(stream.healthy(), "the restart failed no period");
    stream.close();
}

void destroyWhileInterrupted()
{
    otherAudioPlaying = true;
    {
        auto sink = uml::audio::makeApplePlaybackSink();
        Stream stream(*sink);
        check(stream.open(), "a stream opens before the sink is destroyed");
        (void)stream.window(300ms, 300ms);
        postInterruptionBegan();
        (void)stream.window(300ms, 300ms);
    }
    // The observers and the timer went with the sink.
    postInterruptionEnded(true);
    postInterruptionBegan();
    postInterruptionEnded(false);
    otherAudioPlaying = false;
    std::this_thread::sleep_for(1500ms);

    auto sink = uml::audio::makeApplePlaybackSink();
    Stream stream(*sink);
    check(stream.open(), "a new sink opens after a destroyed one");
    const auto seen = stream.window(500ms, 1500ms);
    check(seen.playing(),
        "a new sink plays after notifications for a destroyed one",
        seen.text());
    stream.close();
}

} // namespace

int main()
{
    @autoreleasepool {
        installHooks();
        interruptionMutesAndResumes();
        beganWithNoEnd();
        noActivationWhileInterrupted();
        interruptionBeforeStart();
        restartDuringInterruption();
        destroyWhileInterrupted();
    }
    std::printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL",
        failures);
    return failures == 0 ? 0 : 1;
}
