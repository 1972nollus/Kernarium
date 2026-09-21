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

// How a decoded frame carries its samples. A planar frame is the decoder's
// native 4:2:0 output; a packed frame has already left the YCbCr domain and is
// what a client that only draws through a 32-bit RGB surface needs.
enum class VideoPixelLayout { planarYuv, packedBgra };

// Timestamps retain the caller's units and are returned in presentation order,
// including delayed drain output. `layout` selects which buffers carry the
// samples and is the layout the frame actually has, not the one last requested.
struct DecodedVideoFrame {
    VideoFormat format;
    VideoPixelLayout layout {VideoPixelLayout::planarYuv};
    std::uint64_t timestamp {};
    // planarYuv: tightly packed NV12 (8-bit) or P010 (10-bit).
    std::vector<std::uint8_t> luma;
    std::vector<std::uint8_t> chroma;
    // packedBgra: tightly packed rows of 8-bit B, G, R, A samples, each row
    // `format.width * 4` bytes and alpha opaque.
    std::vector<std::uint8_t> pixels;
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    virtual void submit(std::span<const std::uint8_t> bytes,
                        std::uint64_t timestamp) = 0;
    virtual void drain() = 0;
    virtual std::vector<DecodedVideoFrame> takeFrames() = 0;
    [[nodiscard]] virtual VideoFormat format() const noexcept = 0;
    // Requests the layout of frames decoded from this point on. Frames already
    // decoded keep the layout they were produced with, and a decoder that
    // cannot produce the request keeps producing its native layout.
    virtual void requestOutputLayout(VideoPixelLayout layout) = 0;
};

[[nodiscard]] std::vector<VideoDecodeCapability> videoDecodeCapabilities();
[[nodiscard]] std::unique_ptr<VideoDecoder> makeVideoDecoder(VideoCodec codec);

} // namespace uml::platform
