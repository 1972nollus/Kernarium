// SPDX-License-Identifier: GPL-2.0-or-later

#include "playback_sink.hpp"

#include "platform/descriptor.hpp"
#include "playback_mute_clock.hpp"
#include "protocol.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace uml::audio {
namespace {

using uml::platform::WakeSocket;

struct NotificationFlags final {
    explicit NotificationFlags(std::shared_ptr<WakeSocket> source)
        : wake(std::move(source))
    {
    }

    void signal(std::atomic_bool& flag) noexcept
    {
        flag.store(true, std::memory_order_release);
        (void)wake->notify();
    }

    std::shared_ptr<WakeSocket> wake;
    std::atomic_bool configurationChanged {};
#if TARGET_OS_IPHONE
    std::atomic_bool interruptionBegan {};
    std::atomic_bool interruptionEnded {};
#endif
};

class CompletionQueue final {
public:
    static constexpr std::size_t capacity = 512;

    CompletionQueue()
    {
        for (std::size_t index = 0; index < cells_.size(); ++index)
            cells_[index].sequence.store(index, std::memory_order_relaxed);
    }

    void push(const PlaybackCompletion& completion) noexcept
    {
        const auto position =
            enqueuePosition_.fetch_add(1, std::memory_order_relaxed);
        auto& cell = cells_[position % capacity];
        while (cell.sequence.load(std::memory_order_acquire) != position) {
        }
        cell.completion = completion;
        cell.sequence.store(position + 1U, std::memory_order_release);
    }

    [[nodiscard]] bool pop(PlaybackCompletion& completion) noexcept
    {
        auto& cell = cells_[dequeuePosition_ % capacity];
        if (cell.sequence.load(std::memory_order_acquire) !=
            dequeuePosition_ + 1U) {
            return false;
        }
        completion = cell.completion;
        cell.sequence.store(
            dequeuePosition_ + capacity, std::memory_order_release);
        ++dequeuePosition_;
        return true;
    }

private:
    struct Cell final {
        std::atomic<std::size_t> sequence {};
        PlaybackCompletion completion {};
    };

    std::array<Cell, capacity> cells_;
    std::atomic<std::size_t> enqueuePosition_ {};
    std::size_t dequeuePosition_ {};
};

struct PlaybackState final {
    struct Slot final {
        AVAudioPCMBuffer* __strong buffer {};
        std::atomic<std::uint64_t> ticket {};
        std::uint64_t requestId {};
    };

    explicit PlaybackState(std::shared_ptr<WakeSocket> source)
        : wake(std::move(source))
        , slots(std::make_unique<Slot[]>(protocol::maximumPeriods))
    {
    }

    void finish(
        std::uint64_t requestId,
        bool success,
        std::uint32_t periodBytes) noexcept
    {
        std::uint32_t remaining {};
        if (!consumePending(remaining))
            return;
        completions.push({
            .requestId = requestId,
            .latencyBytes = remaining * periodBytes,
            .success = success,
        });
        (void)wake->notify();
    }

    void discard() noexcept
    {
        std::uint32_t ignored {};
        (void)consumePending(ignored);
    }

private:
    [[nodiscard]] bool consumePending(std::uint32_t& remaining) noexcept
    {
        auto previous = pending.load(std::memory_order_acquire);
        while (previous != 0 &&
               !pending.compare_exchange_weak(
                   previous, previous - 1U, std::memory_order_acq_rel)) {
        }
        if (previous == 0)
            return false;
        remaining = previous - 1U;
        return true;
    }

public:
    std::shared_ptr<WakeSocket> wake;
    std::unique_ptr<Slot[]> slots;
    CompletionQueue completions;
    std::atomic<std::uint32_t> pending {};
#if TARGET_OS_IPHONE
    // Bumped when a mute takes over the queued periods: the completion
    // handlers of buffers scheduled before it then stand down.
    std::atomic<std::uint64_t> epoch {};
#endif
    std::size_t slotCount {};
    std::uint32_t periodBytes {};
    std::uint64_t nextTicket {1};
    std::size_t nextSlot {};
};

class ApplePlaybackSink final : public PlaybackSink {
public:
    ApplePlaybackSink()
        : wake_(std::make_shared<WakeSocket>("audio readiness"))
        , notifications_(std::make_shared<NotificationFlags>(wake_))
        , state_(std::make_shared<PlaybackState>(wake_))
        , engine_([[AVAudioEngine alloc] init])
        , player_([[AVAudioPlayerNode alloc] init])
    {
        [engine_ attachNode:player_];
        const std::weak_ptr<NotificationFlags> weakNotifications =
            notifications_;
        configurationObserver_ = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVAudioEngineConfigurationChangeNotification
                        object:engine_
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      if (const auto flags = weakNotifications.lock())
                          flags->signal(flags->configurationChanged);
                    }];
#if TARGET_OS_IPHONE
        routeObserver_ = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVAudioSessionRouteChangeNotification
                        object:[AVAudioSession sharedInstance]
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      if (const auto flags = weakNotifications.lock())
                          flags->signal(flags->configurationChanged);
                    }];
        /* iOS 27 replaces the interruption notification with two: the session
         * becoming inactive, which also reports the app's own deactivations,
         * and the system's resumption recommendation. An interruption is a
         * deactivation that carries an interruption context. The sink
         * resumes on every recommendation, whether it says to resume or not:
         * the guest cannot see a pause, so a sink left paused would strand
         * its queued periods until the guest restarts the stream. */
        deactivationObserver_ = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVAudioSessionDidBecomeInactiveNotification
                        object:[AVAudioSession sharedInstance]
                         queue:nil
                    usingBlock:^(NSNotification* notification) {
                      const auto flags = weakNotifications.lock();
                      if (!flags)
                          return;
                      const AVAudioSessionDeactivationContext* context =
                          notification.userInfo[
                              AVAudioSessionDeactivationContextKey];
                      if (context.interruptionContext != nil)
                          flags->signal(flags->interruptionBegan);
                    }];
        resumptionObserver_ = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVAudioSessionResumptionRecommendationNotification
                        object:[AVAudioSession sharedInstance]
                         queue:nil
                    usingBlock:^(NSNotification*) {
                      if (const auto flags = weakNotifications.lock())
                          flags->signal(flags->interruptionEnded);
                    }];
#endif
    }

    ~ApplePlaybackSink() override
    {
        @try {
            [[NSNotificationCenter defaultCenter]
                removeObserver:configurationObserver_];
#if TARGET_OS_IPHONE
            [[NSNotificationCenter defaultCenter] removeObserver:routeObserver_];
            [[NSNotificationCenter defaultCenter]
                removeObserver:deactivationObserver_];
            [[NSNotificationCenter defaultCenter]
                removeObserver:resumptionObserver_];
#endif
        } @catch (NSException*) {
        }
        reset();
        @try {
            [engine_ detachNode:player_];
        } @catch (NSException*) {
        }
    }

    [[nodiscard]] bool configure(
        const PlaybackParameters& parameters) override
    {
        // Queued periods belong to the current state, and their completions
        // arrive only there: replacing it would strand them, so they must
        // come back first, as prepare() also requires.
        if (state_->pending.load(std::memory_order_acquire) != 0)
            return false;
        @try {
            [player_ stop];
            [engine_ stop];
            deactivateSession();
#if TARGET_OS_IPHONE
            endMute();
#endif
            configured_ = false;
            prepared_ = false;
            running_ = false;
            state_ = std::make_shared<PlaybackState>(wake_);
            auto state = state_;
            state->slotCount = parameters.bufferBytes / parameters.periodBytes;
            state->periodBytes = parameters.periodBytes;
            format_ = [[AVAudioFormat alloc]
                initWithCommonFormat:AVAudioPCMFormatInt16
                         sampleRate:parameters.sampleRate
                           channels:parameters.channels
                        interleaved:YES];
            if (format_ == nil || state->slotCount == 0 ||
                state->slotCount > protocol::maximumPeriods)
                return false;
            const auto frameCapacity = static_cast<AVAudioFrameCount>(
                parameters.periodBytes /
                (parameters.channels * parameters.bytesPerSample));
#if TARGET_OS_IPHONE
            muteClock_ = MuteClock(parameters.sampleRate, frameCapacity);
#endif
            for (std::size_t index = 0; index < state->slotCount; ++index) {
                state->slots[index].buffer = [[AVAudioPCMBuffer alloc]
                    initWithPCMFormat:format_
                        frameCapacity:frameCapacity];
                if (state->slots[index].buffer == nil)
                    return false;
            }
            [engine_ disconnectNodeOutput:player_];
            NSError* connectError = nil;
            if (![engine_ connect:player_
                               to:engine_.mainMixerNode
                           format:format_
                            error:&connectError]) {
                configured_ = false;
                prepared_ = false;
                running_ = false;
                faulted_ = true;
                return false;
            }
            configured_ = true;
            prepared_ = false;
            running_ = false;
            faulted_ = false;
            return true;
        } @catch (NSException*) {
            configured_ = false;
            prepared_ = false;
            running_ = false;
            faulted_ = true;
            return false;
        }
    }

    [[nodiscard]] bool prepare() override
    {
        if (!configured_ || state_->pending.load(std::memory_order_acquire) != 0)
            return false;
        @try {
            [engine_ prepare];
            prepared_ = true;
            faulted_ = false;
            return true;
        } @catch (NSException*) {
            faulted_ = true;
            return false;
        }
    }

    [[nodiscard]] bool start() override
    {
        if (!configured_ || !prepared_)
            return false;
#if TARGET_OS_IPHONE
        // Another app holds the session: the stream starts muted and the
        // sink takes the session when that app lets go, not before.
        applyInterruptionEvents();
        bool sessionReady = false;
        if (interrupted_) {
            if (otherAudioPlaying() || !activateSession()) {
                running_ = true;
                faulted_ = false;
                enterMute();
                return true;
            }
            leaveMute();
            sessionReady = true;
        }
        if (!sessionReady && !activateSession()) {
#else
        if (!activateSession()) {
#endif
            faulted_ = true;
            return false;
        }
        @try {
            NSError* error = nil;
            if ((!engine_.isRunning && ![engine_ startAndReturnError:&error]) ||
                ![player_ playAndReturnError:&error]) {
                deactivateSession();
                faulted_ = true;
                return false;
            }
            running_ = true;
            faulted_ = false;
            return true;
        } @catch (NSException*) {
            deactivateSession();
            faulted_ = true;
            return false;
        }
    }

    void stop() noexcept override
    {
        @try {
            [player_ pause];
        } @catch (NSException*) {
        }
        running_ = false;
#if TARGET_OS_IPHONE
        cancelMuteTimer();
#endif
    }

    void release() noexcept override
    {
        @try {
            [player_ stop];
            [engine_ pause];
        } @catch (NSException*) {
        }
        abortPending(true);
        deactivateSession();
#if TARGET_OS_IPHONE
        endMute();
#endif
        prepared_ = false;
        running_ = false;
        faulted_ = false;
    }

    void reset() noexcept override
    {
        @try {
            [player_ stop];
            [engine_ stop];
        } @catch (NSException*) {
        }
        deactivateSession();
#if TARGET_OS_IPHONE
        endMute();
#endif
        try {
            state_ = std::make_shared<PlaybackState>(wake_);
        } catch (...) {
            // Reset is a transport teardown boundary and cannot report an
            // allocation failure. The stopped state remains safe to replace
            // on the next successful configure request.
        }
        configured_ = false;
        prepared_ = false;
        running_ = false;
        faulted_ = false;
    }

    [[nodiscard]] PlaybackEnqueueResult enqueue(
        std::uint64_t requestId,
        std::span<const std::uint8_t> pcm) override
    {
        auto state = state_;
        if (!prepared_ || pcm.size() != state->periodBytes ||
            state->slotCount == 0)
            return PlaybackEnqueueResult::failed;
        if (faulted_)
            return PlaybackEnqueueResult::failed;
        PlaybackState::Slot* selected = nullptr;
        std::size_t selectedIndex = 0;
        for (std::size_t offset = 0; offset < state->slotCount; ++offset) {
            const auto index = (state->nextSlot + offset) % state->slotCount;
            if (state->slots[index].ticket.load(std::memory_order_acquire) ==
                0) {
                selected = &state->slots[index];
                selectedIndex = index;
                state->nextSlot = (index + 1U) % state->slotCount;
                break;
            }
        }
        if (selected == nullptr)
            return PlaybackEnqueueResult::retry;
        std::uint64_t ticket {};
        @try {
            selected->buffer.frameLength = selected->buffer.frameCapacity;
            auto* audioBuffer = selected->buffer.mutableAudioBufferList;
            if (audioBuffer == nullptr || audioBuffer->mNumberBuffers != 1 ||
                audioBuffer->mBuffers[0].mData == nullptr ||
                static_cast<std::size_t>(
                    audioBuffer->mBuffers[0].mDataByteSize) < pcm.size())
                return PlaybackEnqueueResult::failed;
            std::memcpy(audioBuffer->mBuffers[0].mData, pcm.data(), pcm.size());

            ticket = state->nextTicket++;
            selected->requestId = requestId;
            selected->ticket.store(ticket, std::memory_order_release);
            [[maybe_unused]] const auto queued =
                state->pending.fetch_add(1, std::memory_order_release);
#if TARGET_OS_IPHONE
            if (muted_) {
                // The mute clock completes the period; nothing is played.
                if (queued == 0)
                    muteClock_.restart(MuteClock::Clock::now());
                armMute();
                return PlaybackEnqueueResult::accepted;
            }
#endif
            schedule(state, selectedIndex, ticket, requestId);
            return PlaybackEnqueueResult::accepted;
        } @catch (NSException*) {
            if (ticket != 0) {
                auto expected = ticket;
                if (selected->ticket.compare_exchange_strong(
                        expected, 0, std::memory_order_acq_rel))
                    state->discard();
            }
            faulted_ = true;
            return PlaybackEnqueueResult::failed;
        }
    }

    [[nodiscard]] int eventDescriptor() const noexcept override
    {
        return wake_->descriptor();
    }

    void drainEventSource() override
    {
        (void)wake_->drain();
        @try {
#if TARGET_OS_IPHONE
            applyInterruptionEvents();
#endif
            const bool configurationChanged =
                notifications_->configurationChanged.exchange(
                    false, std::memory_order_acq_rel);
            if (configurationChanged && configured_) {
                [player_ stop];
                [engine_ stop];
#if TARGET_OS_IPHONE
                // A muted sink's periods belong to the mute clock.
                if (!muted_)
#endif
                    abortPending(true);
                [engine_ prepare];
                faulted_ = false;
            }
#if TARGET_OS_IPHONE
            if (configurationChanged && running_ && !muted_)
#else
            if (configurationChanged && running_)
#endif
            {
                NSError* error = nil;
                if (activateSession() &&
                    (engine_.isRunning ||
                     [engine_ startAndReturnError:&error]) &&
                    [player_ playAndReturnError:&error]) {
                    faulted_ = false;
                } else {
                    abortPending(false);
                    running_ = false;
                    faulted_ = true;
                }
            }
#if TARGET_OS_IPHONE
            serviceMute();
#endif
        } @catch (NSException*) {
            abortPending(false);
            running_ = false;
            faulted_ = true;
#if TARGET_OS_IPHONE
            endMute();
#endif
        }
    }

    [[nodiscard]] bool takeCompletion(
        PlaybackCompletion& completion) override
    {
        return state_->completions.pop(completion);
    }

private:
    void schedule(
        std::shared_ptr<PlaybackState> state,
        std::size_t index,
        std::uint64_t ticket,
        std::uint64_t requestId)
    {
        const auto periodBytes = state->periodBytes;
#if TARGET_OS_IPHONE
        const auto epoch = state->epoch.load(std::memory_order_acquire);
#endif
        [player_
            scheduleBuffer:state->slots[index].buffer
            completionCallbackType:AVAudioPlayerNodeCompletionDataConsumed
            completionHandler:^(AVAudioPlayerNodeCompletionCallbackType) {
#if TARGET_OS_IPHONE
              if (state->epoch.load(std::memory_order_acquire) != epoch)
                  return;
#endif
              auto& slot = state->slots[index];
              auto expected = ticket;
              if (slot.ticket.compare_exchange_strong(
                      expected, 0, std::memory_order_acq_rel)) {
                  state->finish(requestId, true, periodBytes);
              }
            }];
    }

#if TARGET_OS_IPHONE
    using Clock = MuteClock::Clock;

    // How often a muted sink asks whether the app that took the session is
    // still playing.
    static constexpr auto hintPollInterval = std::chrono::seconds(1);
    // A mute that starts within this long of a recovery is not logged.
    static constexpr auto flapInterval = std::chrono::seconds(10);

    /* iOS posts the end of an interruption only when the interrupting app
     * deactivates its session. An app that simply stops, or is killed, posts
     * none, and a sink that waited for it would stay paused for as long as
     * the app is in front, while the guest, which sees no pause, times out
     * every write. So an interrupted sink is muted instead: its queued periods
     * complete on the schedule the audio hardware would have kept, the guest's
     * clock keeps running, and the sink takes the session back as soon as the
     * system's resumption recommendation arrives or no other app is playing
     * primary audio. */
    [[nodiscard]] bool otherAudioPlaying() const noexcept
    {
        @try {
            return [AVAudioSession sharedInstance]
                .secondaryAudioShouldBeSilencedHint;
        } @catch (NSException*) {
            return false;
        }
    }

    // Acts on the notifications that arrived since the last look, so a
    // request that follows them sees the interruption they report.
    void applyInterruptionEvents()
    {
        if (notifications_->interruptionBegan.exchange(
                false, std::memory_order_acq_rel)) {
            interrupted_ = true;
            if (running_ && !muted_)
                enterMute();
        }
        if (notifications_->interruptionEnded.exchange(
                false, std::memory_order_acq_rel)) {
            endInterruption();
        }
    }

    // The system took the session while the stream runs: mute, or restart a
    // mute the stream was stopped in.
    void enterMute()
    {
        const auto now = Clock::now();
        if (!muted_) {
            muted_ = true;
            muteStarted_ = now;
            // One line per mute, and none for a mute that follows a recovery
            // closely: a session the system keeps taking back would otherwise
            // log every second.
            muteLogged_ = now - lastResumed_ > flapInterval;
            // The handlers of buffers the player still holds stand down:
            // their periods are the clock's from here on.
            state_->epoch.fetch_add(1, std::memory_order_acq_rel);
            @try {
                [player_ stop];
                [engine_ pause];
            } @catch (NSException*) {
            }
            if (muteLogged_) {
                std::fputs(
                    "Audio muted: another app holds the audio session\n",
                    stderr);
            }
        }
        muteClock_.restart(now);
        nextHintPoll_ = now + hintPollInterval;
        armMute();
    }

    // Hands the periods the mute clock still holds back to the player, oldest
    // first, and ends the mute without touching the session.
    void leaveMute()
    {
        if (muted_) {
            const auto state = state_;
            std::array<std::pair<std::uint64_t, std::size_t>,
                protocol::maximumPeriods> queued;
            std::size_t count = 0;
            for (std::size_t index = 0; index < state->slotCount; ++index) {
                const auto ticket = state->slots[index].ticket.load(
                    std::memory_order_acquire);
                if (ticket != 0)
                    queued[count++] = {ticket, index};
            }
            std::sort(queued.begin(), queued.begin() + count);
            for (std::size_t position = 0; position < count; ++position) {
                const auto [ticket, index] = queued[position];
                schedule(state, index, ticket, state->slots[index].requestId);
            }
        }
        endMute();
    }

    // Forgets any interruption: the session is released or the sink is reset.
    void endMute() noexcept
    {
        muted_ = false;
        interrupted_ = false;
        cancelMuteTimer();
    }

    // The system says the interrupter let go.
    void endInterruption()
    {
        if (!muted_) {
            interrupted_ = false;
            return;
        }
        if (running_)
            (void)resume();
        else
            leaveMute();
    }

    /* Takes the session back and plays again. False when another app still
     * holds the session, and the sink stays muted; true when the mute is over,
     * which a failure of the engine itself also ends, as a fault. */
    [[nodiscard]] bool resume()
    {
        if (!activateSession())
            return false;
        bool playing = false;
        @try {
            NSError* error = nil;
            if (engine_.isRunning || [engine_ startAndReturnError:&error]) {
                leaveMute();
                playing = [player_ playAndReturnError:&error];
            }
        } @catch (NSException*) {
        }
        if (playing) {
            faulted_ = false;
            lastResumed_ = Clock::now();
            if (muteLogged_) {
                const auto milliseconds =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        lastResumed_ - muteStarted_);
                std::fprintf(stderr, "Audio resumed after %lld ms\n",
                    static_cast<long long>(milliseconds.count()));
            }
            return true;
        }
        abortPending(false);
        running_ = false;
        faulted_ = true;
        endMute();
        return true;
    }

    // Completes the periods that are due and asks whether to take the
    // session back; runs on the device thread, on every pass and timer tick.
    void serviceMute()
    {
        if (!muted_ || !running_)
            return;
        const auto now = Clock::now();
        const auto state = state_;
        while (state->pending.load(std::memory_order_acquire) != 0 &&
               muteClock_.take(now)) {
            completeOldest(*state);
        }
        if (now >= nextHintPoll_) {
            nextHintPoll_ = now + hintPollInterval;
            if (!otherAudioPlaying() && resume())
                return;
        }
        armMute();
    }

    void completeOldest(PlaybackState& state) noexcept
    {
        std::size_t oldest = state.slotCount;
        std::uint64_t oldestTicket {};
        for (std::size_t index = 0; index < state.slotCount; ++index) {
            const auto ticket =
                state.slots[index].ticket.load(std::memory_order_acquire);
            if (ticket != 0 && (oldest == state.slotCount ||
                                   ticket < oldestTicket)) {
                oldest = index;
                oldestTicket = ticket;
            }
        }
        if (oldest == state.slotCount)
            return;
        auto expected = oldestTicket;
        if (state.slots[oldest].ticket.compare_exchange_strong(
                expected, 0, std::memory_order_acq_rel)) {
            state.finish(state.slots[oldest].requestId, true,
                state.periodBytes);
        }
    }

    // Wakes the device thread when the next period or the next poll is due.
    void armMute()
    {
        if (!muted_ || !running_) {
            cancelMuteTimer();
            return;
        }
        auto next = nextHintPoll_;
        if (state_->pending.load(std::memory_order_acquire) != 0)
            next = std::min(next, muteClock_.deadline());
        if (muteTimer_ == nil) {
            muteTimer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0));
            if (muteTimer_ == nil)
                return;
            const std::weak_ptr<NotificationFlags> weakNotifications =
                notifications_;
            dispatch_source_set_event_handler(muteTimer_, ^{
              if (const auto flags = weakNotifications.lock())
                  (void)flags->wake->notify();
            });
            dispatch_resume(muteTimer_);
        }
        const auto delay = std::max(
            Clock::duration::zero(), next - Clock::now());
        dispatch_source_set_timer(muteTimer_,
            dispatch_time(DISPATCH_TIME_NOW,
                std::chrono::duration_cast<std::chrono::nanoseconds>(delay)
                    .count()),
            DISPATCH_TIME_FOREVER, NSEC_PER_MSEC);
    }

    void cancelMuteTimer() noexcept
    {
        if (muteTimer_ != nil) {
            dispatch_source_cancel(muteTimer_);
            muteTimer_ = nil;
        }
    }
#endif

    void abortPending(bool success) noexcept
    {
        auto state = state_;
        for (std::size_t index = 0; index < state->slotCount; ++index) {
            const auto ticket =
                state->slots[index].ticket.exchange(0, std::memory_order_acq_rel);
            if (ticket != 0)
                state->finish(state->slots[index].requestId, success,
                              state->periodBytes);
        }
    }

    [[nodiscard]] bool activateSession() noexcept
    {
#if TARGET_OS_IPHONE
        @try {
            AVAudioSession* session = [AVAudioSession sharedInstance];
            NSError* error = nil;
            if (![session setCategory:AVAudioSessionCategoryPlayback
                               mode:AVAudioSessionModeDefault
                            options:0
                              error:&error])
                return false;
            if (![session setActive:YES error:&error])
                return false;
        } @catch (NSException*) {
            return false;
        }
#endif
        return true;
    }

    void deactivateSession() noexcept
    {
#if TARGET_OS_IPHONE
        @try {
            NSError* error = nil;
            [[AVAudioSession sharedInstance]
                setActive:NO
              withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                    error:&error];
        } @catch (NSException*) {
        }
#endif
    }

    std::shared_ptr<WakeSocket> wake_;
    std::shared_ptr<NotificationFlags> notifications_;
    std::shared_ptr<PlaybackState> state_;
    AVAudioEngine* __strong engine_;
    AVAudioPlayerNode* __strong player_;
    AVAudioFormat* __strong format_;
    id __strong configurationObserver_;
#if TARGET_OS_IPHONE
    id __strong routeObserver_;
    id __strong deactivationObserver_;
    id __strong resumptionObserver_;
    // The system has the session, and no end has come yet.
    bool interrupted_ {};
    // The stream runs but plays nothing: the mute clock completes its periods.
    bool muted_ {};
    MuteClock muteClock_;
    Clock::time_point muteStarted_ {};
    Clock::time_point nextHintPoll_ {};
    Clock::time_point lastResumed_ {};
    bool muteLogged_ {};
    dispatch_source_t __strong muteTimer_ {};
#endif
    bool configured_ {};
    bool prepared_ {};
    bool running_ {};
    bool faulted_ {};
};

} // namespace

std::unique_ptr<PlaybackSink> makeApplePlaybackSink()
{
    @try {
        return std::make_unique<ApplePlaybackSink>();
    } @catch (NSException* exception) {
        const char* reason = exception.reason.UTF8String;
        throw std::runtime_error(
            "initializing Apple audio failed: " +
            std::string(reason == nullptr ? "unknown AVFAudio exception" : reason));
    }
}

} // namespace uml::audio
