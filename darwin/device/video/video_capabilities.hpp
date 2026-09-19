// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_decoder.hpp"
#include "virtio_video.h"

#include <span>

namespace uml::video {
std::uint32_t wireCodec(uml::platform::VideoCodec codec);
std::uint32_t wireProfile(uml::platform::VideoProfile profile);
std::vector<std::uint8_t> capabilityResponse(
    std::span<const uml::platform::VideoDecodeCapability> capabilities, std::uint32_t queue);
std::vector<std::uint32_t> profileValues(
    std::span<const uml::platform::VideoDecodeCapability> capabilities, std::uint32_t format);
} // namespace uml::video
