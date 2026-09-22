// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <functional>
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
    // True when the picture was written into a buffer the caller lent the
    // decoder, so its pixels are already where the caller wants them and
    // `plane` is null.
    bool imported {};
};

// A buffer the caller lends the decoder to write one picture into. The
// descriptors stay owned by the caller: the decoder only queues them.
struct CaptureTarget {
    int fd[3] {-1, -1, -1};
    std::size_t offset[3] {};
    std::size_t length[3] {};
    std::size_t pitch[3] {};
    unsigned planes {};
};

// Hands the decoder a buffer for capture slot `index`. `fresh` marks the first
// slot of a queue the decoder is building, so the caller can take back what it
// lent the previous one. Returning false makes the session fall back to
// buffers the device allocates itself.
using CaptureSource = std::function<bool(unsigned index, bool fresh, CaptureTarget& target)>;

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

    // Returns a collected picture's buffer to the CAPTURE queue. A queue that
    // imports the caller's buffers takes a descriptor back, which need not be
    // the one the picture arrived in: the caller keeps that one for as long as
    // something is still looking at the picture.
    void release(unsigned index);
    bool release(unsigned index, const CaptureTarget& target, std::string& error);

    // Lends the decoder the caller's own buffers, when the device's capture
    // queue accepts them. Set before the first packet; the session falls back
    // to device-allocated buffers whenever the queue or the caller declines.
    void setCaptureSource(CaptureSource source) { captureSource_ = std::move(source); }
    bool importsCapture() const noexcept { return captureImports_; }
    // Why a capture queue is not importing, when it was asked to.
    const std::string& importFailure() const noexcept { return importFailure_; }
    // What a lent buffer has to look like: the device's own plane count, bytes
    // per row and size for the decoded format it settled on.
    unsigned capturePlaneCount() const noexcept { return capturePlanes_; }
    std::size_t capturePitch(unsigned plane) const noexcept { return capturePitch_[plane]; }
    std::size_t capturePlaneSize(unsigned plane) const noexcept { return captureSize_[plane]; }

    // Ends the current sequence so that every pending picture is emitted, then
    // starts a new one. Pictures are delivered through collect() as usual.
    bool drain(std::string& error);

    // The format the decoder really produces, which is NV12 unless a
    // preferred format was both asked for and accepted.
    std::uint32_t captureFormat() const noexcept { return captureFormat_; }
    // The decoded plane layout the device settled on, for diagnostics.
    const std::string& captureLayout() const noexcept { return layout_; }
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
    bool queueImported(unsigned index, const CaptureTarget& target, std::string& error);
    bool importCapture(std::string& error);
    bool pendingSourceChange(std::string& error);

    int fd_ {-1};
    std::uint32_t preferredCapture_ {};
    std::uint32_t captureFormat_ {};
    std::string layout_;
    unsigned width_ {};
    unsigned height_ {};
    unsigned capturePlanes_ {};
    // The device's own bytes per row for each decoded plane. It is the picture
    // width only for a format of one byte per sample: a packed format carries
    // four, and stepping a row by the width then shears the picture.
    unsigned capturePitch_[4] {};
    std::size_t captureSize_[4] {};
    CaptureSource captureSource_;
    bool captureImports_ {};
    std::string importFailure_;
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
// does; `automatic` and `force` do whenever the device offers it.
enum class RgbCapture { off, automatic, force };

// Parses UML_VA_V4L2_RGB. An unset or unrecognised request is `automatic`.
RgbCapture rgbCaptureRequest(const char* request) noexcept;

// The decision itself, made once at initialisation so that the first decode
// session of a boot gets the same answer as every later one. Asking for
// packed RGB is what makes a browser's mandatory blit cheap: measured over a
// whole 392-frame 1920x1200 stream, 156 ms a frame converting NV12 against
// 1 ms copying what the host already packed, and 128.2 s against 74.8 s for
// the stream. Carrying 2.6 times the bytes costs about 6 ms a frame, which is
// nothing beside that. A client that reads NV12 back pays the conversion the
// other way, so `off` stays for one that wants the device's own planes.
bool useRgbCapture(RgbCapture policy, bool deviceOffers) noexcept;

// Whether to lend the decoder the driver's own surfaces for its decoded
// pictures. `automatic` does whenever a surface of the kind this graphics
// stack makes is one the capture queue accepts; `device` always leaves the
// decoder its own buffers, and `lend` tries regardless of the probe.
enum class CaptureMemory { automatic, lend, device };

// Parses UML_VA_V4L2_CAPTURE. An unset or unrecognised request is `automatic`.
CaptureMemory captureMemoryRequest(const char* request) noexcept;

// Whether the device can deliver decoded pictures in `fourcc`. A device that
// does not enumerate it makes the RGB decode path unavailable, and the driver
// then decodes NV12 and converts.
bool decoderOffersCapture(const std::string& device, std::uint32_t fourcc);

// The frame sizes the device accepts for a coded format. Returns false and
// leaves the arguments untouched when the device does not say.
bool queryCodecLimits(const std::string& device, std::uint32_t codedFormat, unsigned& minWidth,
                      unsigned& minHeight, unsigned& maxWidth, unsigned& maxHeight);

}
