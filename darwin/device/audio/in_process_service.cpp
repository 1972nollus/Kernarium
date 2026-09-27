// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "playback_sink.hpp"
#include "vhost_user_server.hpp"
#include "virtio_sound_device.hpp"

namespace uml::audio {

InProcessAudioService::InProcessAudioService()
    : service_(std::make_unique<uml::gpu::InProcessVhostUserService>(
          "Virtio Sound", "uml-audio", [] {
              return std::make_unique<VirtioSoundDevice>(
                  makeApplePlaybackSink());
          }))
{
}

InProcessAudioService::~InProcessAudioService() = default;

std::string InProcessAudioService::takeConnectionName()
{
    return service_->takeConnectionName();
}

void InProcessAudioService::stop() noexcept
{
    service_->stop();
}

} // namespace uml::audio
