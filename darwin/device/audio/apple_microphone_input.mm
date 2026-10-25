// SPDX-License-Identifier: GPL-2.0-or-later

#include "microphone_capture_source.hpp"

#include "audio_session.hpp"
#include "protocol.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#include <memory>
#include <mutex>
#include <span>

namespace uml::audio {
namespace {

// A tap delivers between 100 and 400 ms of audio at a time. Converted to
// 48 kHz, 400 ms is 19200 frames; this leaves room over.
constexpr AVAudioFrameCount maximumConvertedFrames = 24000;
// The shortest delivery the tap supports, which is also its lowest latency.
constexpr double tapSeconds = 0.1;

/* The microphone's engine. Nothing here exists until start() is called, and
 * start() is called only for a guest that records while the user's setting is
 * on and permitted, so no AVAudioEngine input node is touched otherwise. */
struct Capture final : std::enable_shared_from_this<Capture> {
    ~Capture() { stop(); }

    [[nodiscard]] bool start(std::uint8_t channelCount, CaptureRing& target) noexcept
    {
        const std::lock_guard guard(lock);
        channels = channelCount;
        ring = &target;
        wanted = true;
        return open();
    }

    void stop() noexcept
    {
        const std::lock_guard guard(lock);
        wanted = false;
        close();
    }

    /* The route changed under a running engine: build it again. */
    void rebuild() noexcept
    {
        const std::lock_guard guard(lock);
        if (!wanted)
            return;
        close();
        (void)open();
    }

    void interrupt(bool began) noexcept
    {
        const std::lock_guard guard(lock);
        if (!wanted)
            return;
        close();
        if (!began)
            (void)open();
    }

    std::mutex lock;
    bool wanted {};
    std::uint8_t channels {};
    CaptureRing* ring {};
    AVAudioEngine* __strong engine {};
    id __strong routeObserver {};
    bool sessionHeld {};

private:
    [[nodiscard]] bool open() noexcept
    {
        if (engine != nil)
            return true;
        @try {
            if (!sessionHeld) {
                if (!session::acquire(session::Client::capture))
                    return false;
                sessionHeld = true;
            }
            AVAudioEngine* created = [[AVAudioEngine alloc] init];
            AVAudioInputNode* input = created.inputNode;
            AVAudioFormat* inputFormat = [input outputFormatForBus:0];
            if (inputFormat == nil || inputFormat.sampleRate <= 0 ||
                inputFormat.channelCount == 0) {
                closeSession();
                return false;
            }
            AVAudioFormat* outputFormat = [[AVAudioFormat alloc]
                initWithCommonFormat:AVAudioPCMFormatInt16
                          sampleRate:protocol::sampleRate
                            channels:channels
                         interleaved:YES];
            AVAudioConverter* converter = [[AVAudioConverter alloc]
                initFromFormat:inputFormat
                      toFormat:outputFormat];
            AVAudioPCMBuffer* converted = [[AVAudioPCMBuffer alloc]
                initWithPCMFormat:outputFormat
                    frameCapacity:maximumConvertedFrames];
            if (outputFormat == nil || converter == nil || converted == nil) {
                closeSession();
                return false;
            }
            const std::weak_ptr<Capture> weak = weak_from_this();
            CaptureRing* destination = ring;
            const std::uint32_t frameBytes =
                channels * protocol::bytesPerSample;
            NSError* tapError = nil;
            const BOOL tapped = [input
                installTapOnBus:0
                     bufferSize:static_cast<AVAudioFrameCount>(
                                    inputFormat.sampleRate * tapSeconds)
                         format:inputFormat
                          error:&tapError
                          block:^(AVAudioPCMBuffer* buffer, AVAudioTime*) {
                               __block BOOL supplied = NO;
                               converted.frameLength = 0;
                               NSError* error = nil;
                               const auto status = [converter
                                   convertToBuffer:converted
                                             error:&error
                                withInputFromBlock:^AVAudioBuffer*(
                                    AVAudioPacketCount,
                                    AVAudioConverterInputStatus* inputStatus) {
                                  if (supplied) {
                                      *inputStatus =
                                          AVAudioConverterInputStatus_NoDataNow;
                                      return nil;
                                  }
                                  supplied = YES;
                                  *inputStatus =
                                      AVAudioConverterInputStatus_HaveData;
                                  return buffer;
                                }];
                               if (status == AVAudioConverterOutputStatus_Error)
                                   return;
                               const auto* data = static_cast<const std::uint8_t*>(
                                   converted.audioBufferList->mBuffers[0].mData);
                               if (data != nullptr) {
                                   (void)destination->write(std::span(
                                       data,
                                       std::size_t {converted.frameLength} *
                                           frameBytes));
                               }
                             }];
            if (!tapped) {
                closeSession();
                return false;
            }
            [created prepare];
            NSError* startError = nil;
            if (![created startAndReturnError:&startError]) {
                [input removeTapOnBus:0];
                closeSession();
                return false;
            }
            engine = created;
            // A route change stops the engine; the observer builds it again.
            routeObserver = [NSNotificationCenter.defaultCenter
                addObserverForName:AVAudioEngineConfigurationChangeNotification
                            object:created
                             queue:nil
                        usingBlock:^(NSNotification*) {
                          if (const auto capture = weak.lock())
                              capture->rebuild();
                        }];
            return true;
        } @catch (NSException*) {
            engine = nil;
            closeSession();
            return false;
        }
    }

    void close() noexcept
    {
        if (routeObserver != nil) {
            [NSNotificationCenter.defaultCenter removeObserver:routeObserver];
            routeObserver = nil;
        }
        @try {
            if (engine != nil) {
                [engine stop];
                [engine.inputNode removeTapOnBus:0];
            }
        } @catch (NSException*) {
        }
        engine = nil;
        closeSession();
    }

    void closeSession() noexcept
    {
        if (sessionHeld) {
            session::release(session::Client::capture);
            sessionHeld = false;
        }
    }
};

class AppleMicrophoneInput final : public MicrophoneInput {
public:
    AppleMicrophoneInput()
        : capture_(std::make_shared<Capture>())
    {
#if TARGET_OS_IPHONE
        const std::weak_ptr<Capture> weak = capture_;
        /* The playback sink observes the same notification, which iOS 27
         * deprecates; see the note there. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        interruptionObserver_ = [NSNotificationCenter.defaultCenter
            addObserverForName:AVAudioSessionInterruptionNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification* notification) {
                      const auto capture = weak.lock();
                      if (!capture)
                          return;
                      const NSNumber* type = notification.userInfo
                          [AVAudioSessionInterruptionTypeKey];
                      capture->interrupt(
                          type.unsignedIntegerValue ==
                          AVAudioSessionInterruptionTypeBegan);
                    }];
#pragma clang diagnostic pop
#endif
    }

    ~AppleMicrophoneInput() override
    {
#if TARGET_OS_IPHONE
        [NSNotificationCenter.defaultCenter removeObserver:interruptionObserver_];
#endif
        capture_->stop();
    }

    [[nodiscard]] bool start(
        std::uint8_t channels,
        CaptureRing& ring) noexcept override
    {
        return capture_->start(channels, ring);
    }

    void stop() noexcept override { capture_->stop(); }

private:
    std::shared_ptr<Capture> capture_;
#if TARGET_OS_IPHONE
    id __strong interruptionObserver_;
#endif
};

} // namespace

std::unique_ptr<MicrophoneInput> makeAppleMicrophoneInput()
{
    return std::make_unique<AppleMicrophoneInput>();
}

} // namespace uml::audio
