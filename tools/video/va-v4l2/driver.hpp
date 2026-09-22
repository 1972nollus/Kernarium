// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_backend_vpp.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "storage.hpp"
#include "v4l2_decoder.hpp"
#include "vp9.hpp"

namespace uml::va {

// The decoder profiles this driver can serve, and the V4L2 coded format each
// one needs from the Virtio video device.
struct CodecSupport {
    VAProfile profile;
    std::uint32_t v4l2Format;
    unsigned minWidth;
    unsigned minHeight;
    unsigned maxWidth;
    unsigned maxHeight;
};

struct Config {
    VAProfile profile {VAProfileNone};
    VAEntrypoint entrypoint {VAEntrypointVLD};
    unsigned rtFormat {};
};

struct Buffer {
    VABufferType type {};
    unsigned size {};
    unsigned elements {};
    std::vector<std::uint8_t> owned;
    // An image derived from a surface maps the surface's memory instead of a
    // copy, so the buffer points at it rather than owning it.
    std::uint8_t* external {};
    std::size_t externalSize {};
    // Keeps the surface allocation an image was derived from alive, because a
    // surface that decodes again may be handed a different buffer.
    std::shared_ptr<PixelBuffer> pin;
    std::uint8_t* data() { return external ? external : owned.data(); }
};

struct Surface {
    unsigned width {};
    unsigned height {};
    unsigned rtFormat {};
    std::uint32_t fourcc {};
    std::shared_ptr<PixelBuffer> buffer;
    // When the decoder delivers packed RGB, the picture lands here and the
    // NV12 buffer above is filled only if a client really asks for NV12.
    std::shared_ptr<PixelBuffer> shadow;
    bool nativeValid {true};
    // Set once a client has been given this surface's own memory - a dma-buf
    // descriptor or a derived image. The memory is then the surface's
    // identity, so a later picture has to be copied into it rather than the
    // surface being pointed at a different buffer.
    bool exported {};
    enum class State { idle, pending, ready, failed };
    State state {State::idle};
    std::uint64_t sequence {};
    VAContextID owner {VA_INVALID_ID};
};

struct Context {
    VAConfigID config {VA_INVALID_ID};
    VAProfile profile {VAProfileNone};
    VAEntrypoint entrypoint {VAEntrypointVLD};
    unsigned width {};
    unsigned height {};
    std::unique_ptr<V4l2Decoder> decoder;
    bool reportLayout {};
    Vp9Context vp9;
    std::uint64_t sequence {};
    // The frame being assembled between vaBeginPicture and vaEndPicture.
    VASurfaceID target {VA_INVALID_SURFACE};
    bool picture {};
    std::vector<std::uint8_t> sliceData;
    bool haveSliceData {};
    // Which surface last wrote each VP9 reference slot, so that a
    // show_existing_frame submission can be served without the decoder.
    VASurfaceID slot[8] {};
    std::unordered_map<std::uint64_t, VASurfaceID> awaiting;
    std::string failure;
    // The buffers lent to the decoder's capture queue, one for each slot, and
    // the ones it has handed back. A buffer is free again when nothing but the
    // pool refers to it.
    std::vector<std::shared_ptr<PixelBuffer>> captureSlot;
    std::vector<std::shared_ptr<PixelBuffer>> captureSpare;
    // Video processing only.
    VAProcPipelineParameterBuffer pipeline {};
    bool havePipeline {};
    VARectangle pipelineSource {};
    VARectangle pipelineDestination {};
    bool pipelineSourceValid {};
    bool pipelineDestinationValid {};
};

struct Image {
    VAImage image {};
    VASurfaceID derivedFrom {VA_INVALID_SURFACE};
};

class Driver {
public:
    explicit Driver(VADriverContextP context);
    ~Driver();

    std::mutex& lock() noexcept { return lock_; }

    const std::vector<CodecSupport>& codecs() const noexcept { return codecs_; }
    const CodecSupport* codec(VAProfile profile) const noexcept;
    const std::string& device() const noexcept { return device_; }
    bool videoProcess() const noexcept { return true; }

    std::map<VAConfigID, Config> configs;
    std::map<VAContextID, std::unique_ptr<Context>> contexts;
    std::map<VASurfaceID, std::unique_ptr<Surface>> surfaces;
    std::map<VABufferID, std::unique_ptr<Buffer>> buffers;
    std::map<VAImageID, std::unique_ptr<Image>> images;
    // Every allocation this driver still owns, by the identity of the
    // descriptor it exports, so that a client handing one back is served the
    // same memory instead of a copy.
    std::unordered_map<std::uint64_t, std::weak_ptr<PixelBuffer>> known;

    VAConfigID nextConfig {0x01000000};
    VAContextID nextContext {0x02000000};
    VASurfaceID nextSurface {0x03000000};
    VABufferID nextBuffer {0x04000000};
    VAImageID nextImage {0x05000000};

    Surface* surface(VASurfaceID id);
    Context* context(VAContextID id);
    Buffer* buffer(VABufferID id);
    // Finds an allocation this driver already owns by the identity of a
    // descriptor a client handed back.
    std::shared_ptr<PixelBuffer> knownBuffer(std::uint64_t identity);

    // Decode plumbing.
    VAStatus submitFrame(Context& context, VASurfaceID id, Surface& target);
    VAStatus waitFor(Surface& surface, std::chrono::milliseconds wait);
    void harvest(Context& context, std::chrono::milliseconds wait);
    // Lends the decoder a buffer for one capture slot, and keeps a pool of the
    // ones it has given back.
    // `fresh` starts a new queue: the buffers lent to the old one go back to
    // the pool first. Refilling one slot of a live queue never does that.
    bool provideCapture(Context& context, unsigned index, CaptureTarget& target, bool fresh);
    std::shared_ptr<PixelBuffer> captureBuffer(Context& context);
    void recycleCapture(Context& context, std::shared_ptr<PixelBuffer> buffer);
    // Copies a lent buffer's picture into a surface that has to keep its own
    // memory, because a client already holds it.
    bool fillFromLent(Surface& target, PixelBuffer& lent, bool packed);
    VAStatus process(Context& context, Surface& target);

    // Fills a surface's NV12 buffer from its RGB shadow, if it has one and the
    // buffer is not already current. A client that only ever uses the video
    // processing path never pays for this.
    void materialise(Surface& surface);
    // Reports the first decoded picture as the device delivered it.
    void reportPicture(const DecodedPicture& picture, bool packed);
    // The same, for a picture the device wrote straight into a lent buffer.
    void reportLent(PixelBuffer& buffer, bool packed);
    bool wantsReport();
    // The decoded format to ask a new decode session for: packed RGB when the
    // device offers it and the policy wants it, otherwise zero for NV12.
    std::uint32_t preferredCapture();
    // Whether to offer the decoder this driver's own buffers for its capture
    // queue. The queue itself decides whether to take them.
    bool importsCapture();

    void note(const char* message) const;

private:
    void probeCodecs();
    // Learns once what the decoder's capture queue can do.
    void probeCapture();

    std::mutex lock_;
    VADriverContextP driverContext_;
    std::vector<CodecSupport> codecs_;
    std::string device_;
    RgbCapture rgbPolicy_ {RgbCapture::automatic};
    CaptureMemory capturePolicy_ {CaptureMemory::automatic};
    // Probed once at initialisation, because each probe opens the device node.
    bool captureProbed_ {};
    bool rgbOffered_ {};
    bool lendCapture_ {};
    int reportedPictures_ {};
    int reportLimit_ {-1};
};

}
