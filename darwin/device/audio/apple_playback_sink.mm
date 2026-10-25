// SPDX-License-Identifier: GPL-2.0-or-later

#include "playback_sink.hpp"

#include "protocol.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace uml::audio {
namespace {

struct NotificationFlags final {
    explicit NotificationFlags(std::shared_ptr<const Waker> source)
        : wake(std::move(source))
    {
    }

    void signal(std::atomic_bool& flag) noexcept
    {
        flag.store(true, std::memory_order_release);
        wake->notify();
    }

    std::shared_ptr<const Waker> wake;
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

    explicit PlaybackState(std::shared_ptr<const Waker> source)
        : wake(std::move(source))
        , slots(std::make_unique<Slot[]>(protocol::maximumPeriods))
    {
    }

    void finish(
        std::uint64_t requestId,
        PlaybackOutcome outcome,
        std::uint32_t periodBytes) noexcept
    {
        std::uint32_t remaining {};
        if (!consumePending(remaining))
            return;
        completions.push({
            .requestId = requestId,
            .latencyBytes = remaining * periodBytes,
            .outcome = outcome,
        });
        wake->notify();
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
    std::shared_ptr<const Waker> wake;
    std::unique_ptr<Slot[]> slots;
    CompletionQueue completions;
    std::atomic<std::uint32_t> pending {};
    std::size_t slotCount {};
    std::uint32_t periodBytes {};
    std::uint64_t nextTicket {1};
    std::size_t nextSlot {};
};

class ApplePlaybackSink final : public PlaybackSink {
public:
    explicit ApplePlaybackSink(std::shared_ptr<const Waker> waker)
        : wake_(std::move(waker))
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
        /* iOS 27 deprecates this notification for
         * AVAudioSessionDidBecomeInactiveNotification and
         * AVAudioSessionResumptionRecommendationNotification. It is still
         * posted, and the sink resumes on every end it reports; the
         * replacements carry a resume recommendation instead, so moving to
         * them needs a device test of an interruption. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        interruptionObserver_ = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVAudioSessionInterruptionNotification
                        object:[AVAudioSession sharedInstance]
                         queue:nil
                    usingBlock:^(NSNotification* notification) {
                      const auto flags = weakNotifications.lock();
                      if (!flags)
                          return;
                      const NSNumber* type = notification.userInfo[
                          AVAudioSessionInterruptionTypeKey];
                      if (type.unsignedIntegerValue ==
                          AVAudioSessionInterruptionTypeBegan) {
                          flags->signal(flags->interruptionBegan);
                      } else {
                          flags->signal(flags->interruptionEnded);
                      }
                    }];
#pragma clang diagnostic pop
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
                removeObserver:interruptionObserver_];
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

    /* An output that will not start does not fail the guest's stream. The
     * stream runs, the sink discards what it is given (enqueue() reports
     * it), the device paces the guest on its own clock, and the next route
     * change or end of an interruption tries the output again. A false
     * return means only that the sink was not prepared. */
    [[nodiscard]] bool start() override
    {
        if (!configured_ || !prepared_)
            return false;
        running_ = true;
        if (!activateSession()) {
            failToStart();
            return true;
        }
        @try {
            NSError* error = nil;
            if ((!engine_.isRunning && ![engine_ startAndReturnError:&error]) ||
                ![player_ playAndReturnError:&error]) {
                deactivateSession();
                failToStart();
                return true;
            }
            faulted_ = false;
            return true;
        } @catch (NSException*) {
            deactivateSession();
            failToStart();
            return true;
        }
    }

    void stop() noexcept override
    {
        @try {
            [player_ pause];
        } @catch (NSException*) {
        }
        running_ = false;
    }

    void release() noexcept override
    {
        @try {
            [player_ stop];
            [engine_ pause];
        } @catch (NSException*) {
        }
        abortPending();
        deactivateSession();
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
            return PlaybackEnqueueResult::discarded;
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
                return PlaybackEnqueueResult::discarded;
            std::memcpy(audioBuffer->mBuffers[0].mData, pcm.data(), pcm.size());

            ticket = state->nextTicket++;
            selected->requestId = requestId;
            selected->ticket.store(ticket, std::memory_order_release);
            state->pending.fetch_add(1, std::memory_order_release);
            const auto periodBytes = state->periodBytes;
            [player_
                scheduleBuffer:selected->buffer
                completionCallbackType:AVAudioPlayerNodeCompletionDataConsumed
                completionHandler:^(AVAudioPlayerNodeCompletionCallbackType) {
                  auto& slot = state->slots[selectedIndex];
                  auto expected = ticket;
                  if (slot.ticket.compare_exchange_strong(
                          expected, 0, std::memory_order_acq_rel)) {
                      state->finish(
                          requestId, PlaybackOutcome::played, periodBytes);
                  }
                }];
            return PlaybackEnqueueResult::accepted;
        } @catch (NSException*) {
            if (ticket != 0) {
                auto expected = ticket;
                if (selected->ticket.compare_exchange_strong(
                        expected, 0, std::memory_order_acq_rel))
                    state->discard();
            }
            faulted_ = true;
            return PlaybackEnqueueResult::discarded;
        }
    }

    void handleEvents() override
    {
        @try {
#if TARGET_OS_IPHONE
            if (notifications_->interruptionBegan.exchange(
                    false, std::memory_order_acq_rel)) {
                interrupted_ = true;
                [player_ pause];
                [engine_ pause];
            }
            const bool interruptionEnded =
                notifications_->interruptionEnded.exchange(
                    false, std::memory_order_acq_rel);
            if (interruptionEnded)
                interrupted_ = false;
#endif
            const bool configurationChanged =
                notifications_->configurationChanged.exchange(
                    false, std::memory_order_acq_rel);
            if (configurationChanged && configured_) {
                [player_ stop];
                [engine_ stop];
                abortPending();
                [engine_ prepare];
                faulted_ = false;
            }
#if TARGET_OS_IPHONE
            if ((interruptionEnded || configurationChanged) && running_ &&
                !interrupted_)
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
                    abortPending();
                    running_ = false;
                    faulted_ = true;
                }
            }
        } @catch (NSException*) {
            abortPending();
            running_ = false;
            faulted_ = true;
        }
    }

    [[nodiscard]] bool takeCompletion(
        PlaybackCompletion& completion) override
    {
        return state_->completions.pop(completion);
    }

private:
    void failToStart() noexcept
    {
        // Periods queued before the start will never play.
        abortPending();
        faulted_ = true;
    }

    /* Hands back every queued period unplayed. The device completes them
     * on its period clock rather than at once. */
    void abortPending() noexcept
    {
        auto state = state_;
        for (std::size_t index = 0; index < state->slotCount; ++index) {
            const auto ticket =
                state->slots[index].ticket.exchange(0, std::memory_order_acq_rel);
            if (ticket != 0)
                state->finish(state->slots[index].requestId,
                              PlaybackOutcome::discarded, state->periodBytes);
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

    std::shared_ptr<const Waker> wake_;
    std::shared_ptr<NotificationFlags> notifications_;
    std::shared_ptr<PlaybackState> state_;
    AVAudioEngine* __strong engine_;
    AVAudioPlayerNode* __strong player_;
    AVAudioFormat* __strong format_;
    id __strong configurationObserver_;
#if TARGET_OS_IPHONE
    id __strong routeObserver_;
    id __strong interruptionObserver_;
    bool interrupted_ {};
#endif
    bool configured_ {};
    bool prepared_ {};
    bool running_ {};
    bool faulted_ {};
};

} // namespace

std::unique_ptr<PlaybackSink> makeApplePlaybackSink(
    std::shared_ptr<const Waker> waker)
{
    @try {
        return std::make_unique<ApplePlaybackSink>(std::move(waker));
    } @catch (NSException* exception) {
        const char* reason = exception.reason.UTF8String;
        throw std::runtime_error(
            "initializing Apple audio failed: " +
            std::string(reason == nullptr ? "unknown AVFAudio exception" : reason));
    }
}

} // namespace uml::audio
