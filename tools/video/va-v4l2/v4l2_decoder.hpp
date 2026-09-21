// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace uml::va {

// A decoded picture still owned by the decoder's CAPTURE queue. It stays valid
// until it is released, which returns its buffer to the decoder.
struct DecodedPicture {
    std::uint64_t timestamp {};
    unsigned index {};
    unsigned width {};
    unsigned height {};
    unsigned planes {};
    const std::uint8_t* plane[3] {};
    std::size_t pitch[3] {};
    std::size_t bytes[3] {};
    bool error {};
};

// One stateful V4L2 memory-to-memory decode session: a device node, an OUTPUT
// queue of compressed packets and a CAPTURE queue of decoded pictures. All
// calls are serialised by the caller.
class V4l2Decoder {
public:
    // `codedFormat` is a V4L2_PIX_FMT_* value for the compressed queue.
    // `preferredCapture` asks for a decoded format other than NV12, and is
    // ignored when the device does not offer it; zero means NV12.
    static std::unique_ptr<V4l2Decoder> open(const std::string& device, std::uint32_t codedFormat,
                                             unsigned width, unsigned height,
                                             std::uint32_t preferredCapture, std::string& error);
    ~V4l2Decoder();

    V4l2Decoder(const V4l2Decoder&) = delete;
    V4l2Decoder& operator=(const V4l2Decoder&) = delete;

    // Hands one complete compressed packet to the decoder. `timestamp` comes
    // back on every picture the packet produces.
    bool submit(std::span<const std::uint8_t> packet, std::uint64_t timestamp, std::string& error);

    // Collects one decoded picture if the decoder has one ready. `wait` bounds
    // how long to wait for it. Returns false when none arrived, which is not an
    // error: a packet may legitimately produce no picture.
    bool collect(DecodedPicture& picture, std::chrono::milliseconds wait, std::string& error);

    // Returns a collected picture's buffer to the CAPTURE queue.
    void release(unsigned index);

    // Ends the current sequence so that every pending picture is emitted, then
    // starts a new one. Pictures are delivered through collect() as usual.
    bool drain(std::string& error);

    // The format the decoder really produces, which is NV12 unless a
    // preferred format was both asked for and accepted.
    std::uint32_t captureFormat() const noexcept { return captureFormat_; }
    unsigned width() const noexcept { return width_; }
    unsigned height() const noexcept { return height_; }
    bool streaming() const noexcept { return captureStreaming_; }

private:
    struct Plane {
        std::uint8_t* data {};
        std::size_t length {};
    };
    struct Buffer {
        Plane planes[3];
        unsigned count {};
        bool queued {};
    };

    V4l2Decoder() = default;
    bool setupOutput(std::uint32_t codedFormat, unsigned width, unsigned height, std::string& error);
    bool setupCapture(std::string& error);
    bool teardownCapture(std::string& error);
    bool allocate(unsigned type, unsigned count, std::vector<Buffer>& buffers, std::string& error);
    void unmap(std::vector<Buffer>& buffers);
    bool reclaimOutput(std::string& error);
    bool queueCapture(unsigned index, std::string& error);
    bool pendingSourceChange(std::string& error);

    int fd_ {-1};
    std::uint32_t preferredCapture_ {};
    std::uint32_t captureFormat_ {};
    unsigned width_ {};
    unsigned height_ {};
    unsigned capturePlanes_ {};
    std::vector<Buffer> output_;
    std::vector<Buffer> capture_;
    bool outputStreaming_ {};
    bool captureStreaming_ {};
    bool sawEos_ {};
};

// The device node the driver talks to, honouring UML_VA_V4L2_DEVICE and
// falling back to a scan for a stateful decoder that accepts `codedFormat`.
std::string findDecoderDevice(std::uint32_t codedFormat);

// Whether to ask the decoder for packed RGB rather than NV12. `off` never
// does; `automatic` does when the client has a video-processing context, which
// is the case where the colour conversion would otherwise be done on the guest
// processor; `force` does whenever the device offers it.
enum class RgbCapture { off, automatic, force };

// Parses UML_VA_V4L2_RGB. An unset or unrecognised request is `automatic`.
RgbCapture rgbCaptureRequest(const char* request) noexcept;

// The decision itself, given the policy and what is known about the device and
// the client.
bool useRgbCapture(RgbCapture policy, bool deviceOffers, bool videoProcessing) noexcept;

// Whether the device can deliver decoded pictures in `fourcc`. A device that
// does not enumerate it makes the RGB decode path unavailable, and the driver
// then decodes NV12 and converts.
bool decoderOffersCapture(const std::string& device, std::uint32_t fourcc);

// The frame sizes the device accepts for a coded format. Returns false and
// leaves the arguments untouched when the device does not say.
bool queryCodecLimits(const std::string& device, std::uint32_t codedFormat, unsigned& minWidth,
                      unsigned& minHeight, unsigned& maxWidth, unsigned& maxHeight);

}
