// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace uml::platform {

enum class VideoCodec { h264, hevc, jpeg, vp9 };
enum class VideoProfile {
    h264Baseline, h264ConstrainedBaseline, h264Main, h264High, h264High10,
    hevcMain, hevcMain10, jpegBaseline, vp9Profile0,
};

struct VideoDecodeCapability {
    VideoCodec codec;
    VideoProfile profile;
    std::uint32_t maximumWidth;
    std::uint32_t maximumHeight;
    std::uint32_t maximumLevel;
    std::uint32_t bitDepth;
};

struct VideoFormat {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t bitDepth {8};
    friend bool operator==(const VideoFormat&, const VideoFormat&) = default;
};

// Tightly packed NV12 (8-bit) or P010 (10-bit); timestamps retain the caller's
// units and are returned in presentation order, including delayed drain output.
struct DecodedVideoFrame {
    VideoFormat format;
    std::uint64_t timestamp {};
    std::vector<std::uint8_t> luma;
    std::vector<std::uint8_t> chroma;
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    virtual void submit(std::span<const std::uint8_t> bytes,
                        std::uint64_t timestamp) = 0;
    virtual void drain() = 0;
    virtual std::vector<DecodedVideoFrame> takeFrames() = 0;
    [[nodiscard]] virtual VideoFormat format() const noexcept = 0;
};

[[nodiscard]] std::vector<VideoDecodeCapability> videoDecodeCapabilities();
[[nodiscard]] std::unique_ptr<VideoDecoder> makeVideoDecoder(VideoCodec codec);

} // namespace uml::platform
