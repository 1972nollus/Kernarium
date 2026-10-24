// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_encoder.hpp"

namespace uml::platform {

void prepareVideoEncodeCapabilities() {}
std::vector<VideoEncodeCapability> videoEncodeCapabilities() { return {}; }
std::unique_ptr<VideoEncoder> makeVideoEncoder() { return nullptr; }

} // namespace uml::platform
