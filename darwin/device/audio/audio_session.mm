// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_session.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#include <mutex>

namespace uml::audio::session {

#if TARGET_OS_IPHONE

namespace {

std::mutex lock;
bool playbackNeeds {};
bool captureNeeds {};

/* Sets the category the current clients need and activates the session. */
bool apply() noexcept
{
    @try {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* error = nil;
        const bool recording = captureNeeds;
        if (![session setCategory:recording ? AVAudioSessionCategoryPlayAndRecord
                                            : AVAudioSessionCategoryPlayback
                             mode:AVAudioSessionModeDefault
                          options:recording
                              ? (AVAudioSessionCategoryOptionMixWithOthers |
                                 AVAudioSessionCategoryOptionDefaultToSpeaker |
                                 AVAudioSessionCategoryOptionAllowBluetoothA2DP)
                              : 0
                            error:&error]) {
            return false;
        }
        return [session setActive:YES error:&error];
    } @catch (NSException*) {
        return false;
    }
}

} // namespace

bool acquire(Client client) noexcept
{
    const std::lock_guard guard(lock);
    bool& needs = client == Client::capture ? captureNeeds : playbackNeeds;
    const bool before = needs;
    needs = true;
    if (apply())
        return true;
    needs = before;
    return false;
}

void release(Client client) noexcept
{
    const std::lock_guard guard(lock);
    (client == Client::capture ? captureNeeds : playbackNeeds) = false;
    if (playbackNeeds || captureNeeds) {
        (void)apply();
        return;
    }
    @try {
        NSError* error = nil;
        [[AVAudioSession sharedInstance]
            setActive:NO
          withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                error:&error];
    } @catch (NSException*) {
    }
}

#else

bool acquire(Client) noexcept
{
    return true;
}

void release(Client) noexcept {}

#endif

} // namespace uml::audio::session
