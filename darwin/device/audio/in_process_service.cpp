// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "playback_sink.hpp"
#include "vhost_user_server.hpp"
#include "virtio_sound_device.hpp"

namespace uml::audio {

std::unique_ptr<uml::gpu::InProcessVhostUserService> makeInProcessService()
{
    return std::make_unique<uml::gpu::InProcessVhostUserService>(
        "Virtio Sound", "uml-audio", [] {
            return std::make_unique<VirtioSoundDevice>(
                makeApplePlaybackSink());
        });
}

} // namespace uml::audio
