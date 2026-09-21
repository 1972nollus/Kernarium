// SPDX-License-Identifier: GPL-2.0-or-later
#include "driver.hpp"

#include <drm_fourcc.h>
#include <linux/videodev2.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <va/va_dec_vp9.h>
#include <va/va_drmcommon.h>

#include "convert.hpp"

namespace uml::va {
namespace {

constexpr const char* kVendor = "Kernarium Virtio video V4L2 backend 1.0";
constexpr std::chrono::milliseconds kDecodeWait {4000};

// The hardware writes limited-range YCbCr; the usual switch between the two
// matrices is the picture height.
bool isBt709(unsigned height) noexcept { return height >= 720; }

// A stateful decoder emits shown frames only, so a VP9 frame with
// show_frame == 0 would leave its surface empty - and a later
// show_existing_frame displays exactly that surface, without ever reaching a
// VA-API driver. Asking the decoder to show the hidden frame straight away,
// through a two-frame superframe, fills the surface without disturbing the
// reference state. UML_VA_V4L2_HIDDEN=skip submits the frame on its own
// instead, which is enough for streams that never show a hidden frame again.
bool showHiddenFrames() noexcept
{
    static const bool show = [] {
        const char* request = std::getenv("UML_VA_V4L2_HIDDEN");
        return !request || std::strcmp(request, "skip") != 0;
    }();
    return show;
}

unsigned lowestSlot(unsigned mask) noexcept
{
    for (unsigned slot = 0; slot < 8; ++slot)
        if (mask & (1u << slot)) return slot;
    return 8;
}

std::uint64_t identityOfDescriptor(int fd) noexcept
{
    struct stat status {};
    if (fd < 0 || fstat(fd, &status) < 0) return 0;
    return std::uint64_t(status.st_ino);
}

}

Driver::Driver(VADriverContextP context) : driverContext_(context)
{
    selectStorage(std::getenv("UML_VA_V4L2_SURFACES"));
    // Which backing store a client's surfaces will come from decides whether
    // an exported handle is a dma-buf at all, so it belongs in the log.
    note((std::string("surfaces ") + storageName(selectedStorage())).c_str());
    rgbPolicy_ = rgbCaptureRequest(std::getenv("UML_VA_V4L2_RGB"));
    probeCodecs();
}

Driver::~Driver() = default;

void Driver::note(const char* message) const
{
    static const char* path = std::getenv("UML_VA_V4L2_LOG");
    if (!path || !*path) return;
    if (std::FILE* file = std::fopen(path, "ae")) {
        std::fprintf(file, "uml-va %d %s\n", int(getpid()), message);
        std::fclose(file);
    }
}

void Driver::probeCodecs()
{
    device_ = findDecoderDevice(V4L2_PIX_FMT_VP9);
    if (device_.empty()) { note("no Virtio video decoder node"); return; }
    // Only the codecs whose VA-API submissions this driver can turn back into
    // the byte stream a stateful decoder needs are advertised. H.264 and HEVC
    // need their parameter sets rebuilt from the picture parameters first.
    CodecSupport vp9 {VAProfileVP9Profile0, V4L2_PIX_FMT_VP9, 16, 16, 4096, 2304};
    queryCodecLimits(device_, V4L2_PIX_FMT_VP9, vp9.minWidth, vp9.minHeight, vp9.maxWidth, vp9.maxHeight);
    codecs_.push_back(vp9);
}

// A packed decode is only worth having when the host's conversion is the one a
// client would have done itself. The device's packed output currently leaves
// the studio-swing range unapplied, so the automatic policy holds it back until
// `v4l2-dmabuf` announces a range-correct, late-negotiation-safe format.
constexpr bool kPackedDecodeTrusted = false;

void Driver::probeCapture()
{
    if (captureProbed_) return;
    captureProbed_ = true;
    rgbOffered_ = decoderOffersCapture(device_, V4L2_PIX_FMT_ABGR32);
    // Both halves are settled here, at initialisation, so that the first
    // decode session of a boot - which is the one that plays a startup movie -
    // gets the same answer as every later one.
    importCapture_ = decoderImportsCapture(device_);
}

std::uint32_t Driver::preferredCapture()
{
    if (rgbPolicy_ == RgbCapture::off) return 0;
    probeCapture();
    if (rgbPolicy_ == RgbCapture::automatic && !kPackedDecodeTrusted) return 0;
    if (!useRgbCapture(rgbPolicy_, rgbOffered_, importCapture_)) return 0;
    // The device writes B, G, R, A in memory, which is what VA-API calls
    // VA_FOURCC_ARGB and what a browser's BGRX pixmap expects.
    return V4L2_PIX_FMT_ABGR32;
}

void Driver::materialise(Surface& target)
{
    if (target.nativeValid || !target.shadow || !target.buffer) return;
    const std::uint8_t* from = target.shadow->map();
    std::uint8_t* to = target.buffer->map();
    if (!from || !to) return;
    const ConstPlaneView in {from + target.shadow->offset(0), target.shadow->pitch(0)};
    const PlaneView out[2] {{to + target.buffer->offset(0), target.buffer->pitch(0)},
                            {to + target.buffer->offset(1), target.buffer->pitch(1)}};
    const Region region {0, 0, target.width, target.height};
    convertArgbToNv12(in, region, out, region, isBt709(target.height));
    target.nativeValid = true;
}

const CodecSupport* Driver::codec(VAProfile profile) const noexcept
{
    for (const auto& entry : codecs_)
        if (entry.profile == profile) return &entry;
    return nullptr;
}

Surface* Driver::surface(VASurfaceID id)
{
    const auto found = surfaces.find(id);
    return found == surfaces.end() ? nullptr : found->second.get();
}

Context* Driver::context(VAContextID id)
{
    const auto found = contexts.find(id);
    return found == contexts.end() ? nullptr : found->second.get();
}

Buffer* Driver::buffer(VABufferID id)
{
    const auto found = buffers.find(id);
    return found == buffers.end() ? nullptr : found->second.get();
}

std::shared_ptr<PixelBuffer> Driver::knownBuffer(std::uint64_t identity)
{
    if (!identity) return {};
    const auto found = known.find(identity);
    if (found == known.end()) return {};
    auto buffer = found->second.lock();
    if (!buffer) known.erase(found);
    return buffer;
}

void Driver::reportPicture(const DecodedPicture& picture, bool packed)
{
    // The picture exactly as the capture queue delivered it, before this
    // driver has touched a byte of it: the only place where a colour question
    // can be settled between the device and everything above it.
    if (!picture.plane[0] || !picture.width || !picture.height) return;
    if (reportLimit_ < 0) {
        const char* wanted = std::getenv("UML_VA_V4L2_REPORT");
        reportLimit_ = wanted && *wanted ? std::atoi(wanted) : 1;
    }
    if (reportedPictures_ >= reportLimit_) return;
    const int index = reportedPictures_++;
    char message[256] {};
    if (packed) {
        double sum[3] {};
        for (unsigned row = 0; row < picture.height; ++row) {
            const std::uint8_t* line = picture.plane[0] + std::size_t(row) * picture.pitch[0];
            for (unsigned column = 0; column < picture.width; ++column) {
                sum[0] += line[std::size_t(column) * 4 + 0];
                sum[1] += line[std::size_t(column) * 4 + 1];
                sum[2] += line[std::size_t(column) * 4 + 2];
            }
        }
        const double samples = double(picture.width) * double(picture.height);
        const double blue = sum[0] / samples;
        const double green = sum[1] / samples;
        const double red = sum[2] / samples;
        // The studio-swing BT.709 luma the same picture would carry as NV12,
        // so a packed and an NV12 run can be compared frame by frame.
        const double luma = 16.0 + (0.2126 * red + 0.7152 * green + 0.0722 * blue) * 219.0 / 255.0;
        std::snprintf(message, sizeof(message),
                      "captured packed frame %d means B %.3f G %.3f R %.3f luma %.3f", index,
                      blue, green, red, luma);
    } else {
        double luma = 0;
        for (unsigned row = 0; row < picture.height; ++row) {
            const std::uint8_t* line = picture.plane[0] + std::size_t(row) * picture.pitch[0];
            for (unsigned column = 0; column < picture.width; ++column) luma += line[column];
        }
        std::snprintf(message, sizeof(message), "captured NV12 frame %d mean Y %.3f", index,
                      luma / (double(picture.width) * double(picture.height)));
    }
    note(message);
    const char* wantedFrame = std::getenv("UML_VA_V4L2_DUMP_FRAME");
    if (const char* path = std::getenv("UML_VA_V4L2_DUMP");
        path && *path && index == (wantedFrame && *wantedFrame ? std::atoi(wantedFrame) : 0)) {
        if (std::FILE* file = std::fopen(path, "wbe")) {
            for (unsigned row = 0; row < picture.height; ++row)
                std::fwrite(picture.plane[0] + std::size_t(row) * picture.pitch[0], 1,
                            std::size_t(picture.width) * (packed ? 4u : 1u), file);
            // A planar picture is written whole, so a reader can convert it.
            if (!packed && picture.planes > 1 && picture.plane[1])
                for (unsigned row = 0; row < (picture.height + 1) / 2; ++row)
                    std::fwrite(picture.plane[1] + std::size_t(row) * picture.pitch[1], 1,
                                picture.width, file);
            std::fclose(file);
        }
    }
}

void Driver::harvest(Context& context, std::chrono::milliseconds wait)
{
    if (!context.decoder) return;
    if (context.reportLayout && !context.decoder->captureLayout().empty()) {
        context.reportLayout = false;
        const bool packed = context.decoder->captureFormat() == V4L2_PIX_FMT_ABGR32;
        note(((packed ? "decoded packed RGB " : "decoded NV12 ") + context.decoder->captureLayout()).c_str());
    }
    DecodedPicture picture {};
    std::string error;
    while (context.decoder->collect(picture, wait, error)) {
        wait = std::chrono::milliseconds(0);
        const auto found = context.awaiting.find(picture.timestamp);
        if (found != context.awaiting.end()) {
            Surface* target = surface(found->second);
            context.awaiting.erase(found);
            const bool packed = context.decoder->captureFormat() == V4L2_PIX_FMT_ABGR32;
            if (!picture.error) reportPicture(picture, packed);
            if (target && target->buffer && target->fourcc == VA_FOURCC_NV12 && packed &&
                picture.planes >= 1 && !picture.error) {
                // The host already converted, so keep the picture packed and
                // leave the NV12 buffer stale until somebody asks for it.
                std::string error;
                if (!target->shadow)
                    target->shadow = PixelBuffer::allocate(VA_FOURCC_ARGB, target->width, target->height,
                                                           error);
                std::uint8_t* destination = target->shadow ? target->shadow->map() : nullptr;
                if (destination) {
                    const ConstPlaneView in {picture.plane[0], picture.pitch[0]};
                    const PlaneView out {destination + target->shadow->offset(0), target->shadow->pitch(0)};
                    const Region region {0, 0, std::min(target->width, picture.width),
                                         std::min(target->height, picture.height)};
                    copyPacked32(in, region, out, region);
                    target->nativeValid = false;
                    target->state = Surface::State::ready;
                } else {
                    if (!error.empty()) note(error.c_str());
                    target->state = Surface::State::failed;
                }
            } else if (target && target->buffer && target->fourcc == VA_FOURCC_NV12 && picture.planes >= 2 &&
                !picture.error) {
                if (std::uint8_t* destination = target->buffer->map()) {
                    const ConstPlaneView source[2] {
                        {picture.plane[0], picture.pitch[0]},
                        {picture.plane[1], picture.pitch[1]}};
                    const PlaneView sink[2] {
                        {destination + target->buffer->offset(0), target->buffer->pitch(0)},
                        {destination + target->buffer->offset(1), target->buffer->pitch(1)}};
                    copyNv12(source, sink, std::min(target->width, picture.width),
                             std::min(target->height, picture.height));
                    target->nativeValid = true;
                    target->state = Surface::State::ready;
                } else {
                    target->state = Surface::State::failed;
                }
            } else if (target) {
                target->state = Surface::State::failed;
            }
        }
        context.decoder->release(picture.index);
        picture = {};
    }
    if (!error.empty()) {
        context.failure = error;
        note(error.c_str());
    }
}

VAStatus Driver::waitFor(Surface& target, std::chrono::milliseconds wait)
{
    if (target.state == Surface::State::failed) return VA_STATUS_ERROR_DECODING_ERROR;
    if (target.state != Surface::State::pending) return VA_STATUS_SUCCESS;
    Context* owner = context(target.owner);
    if (!owner || !owner->decoder) { target.state = Surface::State::failed; return VA_STATUS_ERROR_DECODING_ERROR; }
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (target.state == Surface::State::pending) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        harvest(*owner, std::min(left, std::chrono::milliseconds(100)));
    }
    if (target.state == Surface::State::pending) {
        target.state = Surface::State::failed;
        note("a decoded picture never arrived");
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    return target.state == Surface::State::ready ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_DECODING_ERROR;
}

VAStatus Driver::submitFrame(Context& owner, VASurfaceID id, Surface& target)
{
    if (!owner.decoder) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!owner.haveSliceData || owner.sliceData.empty()) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (target.fourcc != VA_FOURCC_NV12) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    const std::span<const std::uint8_t> frame(owner.sliceData);
    const auto header = owner.vp9.parse(frame);
    if (!header.valid) { note("unparsable VP9 frame header"); return VA_STATUS_ERROR_INVALID_PARAMETER; }
    if (header.profile != 0) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

    if (header.showExistingFrame) {
        // A stateful decoder was never asked to decode this frame, so serve it
        // from the surface that last wrote the slot.
        Surface* source = surface(owner.slot[header.frameToShowMapIndex]);
        if (!source || !source->buffer || !target.buffer) return VA_STATUS_ERROR_INVALID_SURFACE;
        const auto status = waitFor(*source, kDecodeWait);
        if (status != VA_STATUS_SUCCESS) return status;
        if (source->shadow && !source->nativeValid && target.shadow) {
            // Both sides hold packed pixels, so serve the repeat frame from
            // the shadow and leave the NV12 buffers alone.
            const std::uint8_t* packed = source->shadow->map();
            std::uint8_t* out = target.shadow->map();
            if (!packed || !out) return VA_STATUS_ERROR_OPERATION_FAILED;
            const Region region {0, 0, std::min(source->width, target.width),
                                 std::min(source->height, target.height)};
            copyPacked32(ConstPlaneView {packed + source->shadow->offset(0), source->shadow->pitch(0)},
                         region, PlaneView {out + target.shadow->offset(0), target.shadow->pitch(0)},
                         region);
            target.nativeValid = false;
            target.state = Surface::State::ready;
            return VA_STATUS_SUCCESS;
        }
        materialise(*source);
        const std::uint8_t* from = source->buffer->map();
        std::uint8_t* to = target.buffer->map();
        if (!from || !to) return VA_STATUS_ERROR_OPERATION_FAILED;
        const ConstPlaneView in[2] {{from + source->buffer->offset(0), source->buffer->pitch(0)},
                                    {from + source->buffer->offset(1), source->buffer->pitch(1)}};
        const PlaneView out[2] {{to + target.buffer->offset(0), target.buffer->pitch(0)},
                                {to + target.buffer->offset(1), target.buffer->pitch(1)}};
        copyNv12(in, out, std::min(source->width, target.width), std::min(source->height, target.height));
        // The NV12 buffer is now the truth for this surface, whatever an
        // earlier packed decode left in its shadow.
        target.nativeValid = true;
        target.state = Surface::State::ready;
        return VA_STATUS_SUCCESS;
    }

    std::vector<std::uint8_t> packet;
    std::uint8_t tail = 0;
    const unsigned slot = header.showFrame || !showHiddenFrames() ? 8u : lowestSlot(header.refreshFrameFlags);
    if (!header.showFrame && slot < 8) {
        // The decoder emits shown frames only, so ask it to show this hidden
        // frame straight away through a superframe. A later
        // show_existing_frame in the stream displays this surface.
        tail = vp9ShowExistingFrameByte(slot);
        const std::span<const std::uint8_t> parts[2] {frame, std::span<const std::uint8_t>(&tail, 1)};
        packet = vp9BuildSuperframe(std::span<const std::span<const std::uint8_t>>(parts, 2));
    } else {
        packet.assign(frame.begin(), frame.end());
    }
    if (packet.empty()) return VA_STATUS_ERROR_OPERATION_FAILED;

    // V4L2 carries a buffer timestamp as a timeval, so a submission key has to
    // be a whole microsecond to survive the round trip through both queues.
    const auto sequence = ++owner.sequence * 1000ull;
    std::string error;
    if (!owner.decoder->submit(packet, sequence, error)) {
        note(error.c_str());
        owner.failure = error;
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    target.state = Surface::State::pending;
    target.sequence = sequence;
    owner.awaiting[sequence] = id;
    const bool expectPicture = header.showFrame || slot < 8;
    for (unsigned index = 0; index < 8; ++index)
        if (header.refreshFrameFlags & (1u << index)) owner.slot[index] = id;
    // The device decodes synchronously, so the picture is normally already on
    // its way; waiting here keeps the surface valid for every client, including
    // the ones that never call vaSyncSurface.
    if (!expectPicture) {
        // Nothing will come back for this submission. Let the decoder deliver
        // whatever else is ready and leave the surface as it is.
        harvest(owner, std::chrono::milliseconds(0));
        owner.awaiting.erase(sequence);
        target.state = Surface::State::ready;
        return VA_STATUS_SUCCESS;
    }
    return waitFor(target, kDecodeWait);
}

VAStatus Driver::process(Context& owner, Surface& target)
{
    if (!owner.havePipeline) return VA_STATUS_ERROR_INVALID_PARAMETER;
    Surface* source = surface(owner.pipeline.surface);
    if (!source || !source->buffer || !target.buffer) return VA_STATUS_ERROR_INVALID_SURFACE;
    const auto status = waitFor(*source, kDecodeWait);
    if (status != VA_STATUS_SUCCESS) return status;
    const std::uint8_t* from = source->buffer->map();
    std::uint8_t* to = target.buffer->map();
    if (!from || !to) return VA_STATUS_ERROR_OPERATION_FAILED;
    Region sourceRegion {0, 0, source->width, source->height};
    if (owner.pipelineSourceValid)
        sourceRegion = {unsigned(owner.pipelineSource.x), unsigned(owner.pipelineSource.y),
                        owner.pipelineSource.width, owner.pipelineSource.height};
    Region targetRegion {0, 0, target.width, target.height};
    if (owner.pipelineDestinationValid)
        targetRegion = {unsigned(owner.pipelineDestination.x), unsigned(owner.pipelineDestination.y),
                        owner.pipelineDestination.width, owner.pipelineDestination.height};
    if (sourceRegion.x + sourceRegion.width > source->width ||
        sourceRegion.y + sourceRegion.height > source->height ||
        targetRegion.x + targetRegion.width > target.width ||
        targetRegion.y + targetRegion.height > target.height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (source->shadow && !source->nativeValid && target.buffer->planes() == 1 &&
        (target.fourcc == VA_FOURCC_ARGB || target.fourcc == VA_FOURCC_BGRA ||
         target.fourcc == VA_FOURCC_RGBX || target.fourcc == VA_FOURCC_BGRX)) {
        // The device decoded straight into the format this blit was going to
        // produce, so the conversion the caller asked for is a copy.
        const std::uint8_t* packed = source->shadow->map();
        if (!packed) return VA_STATUS_ERROR_OPERATION_FAILED;
        const ConstPlaneView in {packed + source->shadow->offset(0), source->shadow->pitch(0)};
        const PlaneView out {to + target.buffer->offset(0), target.buffer->pitch(0)};
        copyPacked32(in, sourceRegion, out, targetRegion);
        return VA_STATUS_SUCCESS;
    }
    if (source->shadow && !source->nativeValid) {
        // Any other destination needs the NV12 the decoder did not produce.
        materialise(*source);
        from = source->buffer->map();
        if (!from) return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (source->fourcc == VA_FOURCC_NV12 && target.fourcc == VA_FOURCC_NV12) {
        if (sourceRegion.width != targetRegion.width || sourceRegion.height != targetRegion.height)
            return VA_STATUS_ERROR_UNIMPLEMENTED;
        const ConstPlaneView in[2] {{from + source->buffer->offset(0) + sourceRegion.y * source->buffer->pitch(0) +
                                         sourceRegion.x,
                                     source->buffer->pitch(0)},
                                    {from + source->buffer->offset(1) +
                                         (sourceRegion.y / 2) * source->buffer->pitch(1) + (sourceRegion.x & ~1u),
                                     source->buffer->pitch(1)}};
        const PlaneView out[2] {{to + target.buffer->offset(0) + targetRegion.y * target.buffer->pitch(0) +
                                     targetRegion.x,
                                 target.buffer->pitch(0)},
                                {to + target.buffer->offset(1) + (targetRegion.y / 2) * target.buffer->pitch(1) +
                                     (targetRegion.x & ~1u),
                                 target.buffer->pitch(1)}};
        copyNv12(in, out, targetRegion.width, targetRegion.height);
    } else if (source->fourcc == VA_FOURCC_NV12) {
        const ConstPlaneView in[2] {{from + source->buffer->offset(0), source->buffer->pitch(0)},
                                    {from + source->buffer->offset(1), source->buffer->pitch(1)}};
        const PlaneView out {to + target.buffer->offset(0), target.buffer->pitch(0)};
        convertNv12ToArgb(in, sourceRegion, out, targetRegion, isBt709(source->height));
    } else if (source->fourcc == target.fourcc) {
        const ConstPlaneView in {from + source->buffer->offset(0), source->buffer->pitch(0)};
        const PlaneView out {to + target.buffer->offset(0), target.buffer->pitch(0)};
        copyPacked32(in, sourceRegion, out, targetRegion);
    } else {
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    }
    target.state = Surface::State::ready;
    return VA_STATUS_SUCCESS;
}

namespace {

Driver& driverOf(VADriverContextP ctx) { return *static_cast<Driver*>(ctx->pDriverData); }

std::uint32_t defaultFourcc(unsigned rtFormat) noexcept
{
    switch (rtFormat) {
    case VA_RT_FORMAT_YUV420: return VA_FOURCC_NV12;
    case VA_RT_FORMAT_RGB32: return VA_FOURCC_ARGB;
    default: return 0;
    }
}

bool supportedFourcc(std::uint32_t fourcc) noexcept
{
    switch (fourcc) {
    case VA_FOURCC_NV12:
    case VA_FOURCC_ARGB:
    case VA_FOURCC_BGRA:
    case VA_FOURCC_RGBA:
    case VA_FOURCC_BGRX:
    case VA_FOURCC_RGBX:
        return true;
    default:
        return false;
    }
}

VAStatus terminate(VADriverContextP ctx)
{
    delete static_cast<Driver*>(ctx->pDriverData);
    ctx->pDriverData = nullptr;
    return VA_STATUS_SUCCESS;
}

VAStatus queryConfigProfiles(VADriverContextP ctx, VAProfile* profiles, int* count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    int written = 0;
    for (const auto& codec : driver.codecs()) profiles[written++] = codec.profile;
    if (driver.videoProcess()) profiles[written++] = VAProfileNone;
    *count = written;
    return VA_STATUS_SUCCESS;
}

VAStatus queryConfigEntrypoints(VADriverContextP ctx, VAProfile profile, VAEntrypoint* entrypoints, int* count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    if (profile == VAProfileNone) {
        if (!driver.videoProcess()) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        entrypoints[0] = VAEntrypointVideoProc;
        *count = 1;
        return VA_STATUS_SUCCESS;
    }
    if (!driver.codec(profile)) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    entrypoints[0] = VAEntrypointVLD;
    *count = 1;
    return VA_STATUS_SUCCESS;
}

VAStatus getConfigAttributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint,
                             VAConfigAttrib* attributes, int count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    const bool proc = profile == VAProfileNone && entrypoint == VAEntrypointVideoProc;
    if (!proc && (!driver.codec(profile) || entrypoint != VAEntrypointVLD))
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    for (int index = 0; index < count; ++index) {
        switch (attributes[index].type) {
        case VAConfigAttribRTFormat:
            attributes[index].value = proc ? (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32) : VA_RT_FORMAT_YUV420;
            break;
        case VAConfigAttribMaxPictureWidth:
            attributes[index].value = proc ? 4096u : driver.codec(profile)->maxWidth;
            break;
        case VAConfigAttribMaxPictureHeight:
            attributes[index].value = proc ? 4096u : driver.codec(profile)->maxHeight;
            break;
        case VAConfigAttribDecSliceMode:
            attributes[index].value = VA_DEC_SLICE_MODE_NORMAL;
            break;
        default:
            attributes[index].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus createConfig(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint,
                      VAConfigAttrib* attributes, int count, VAConfigID* id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    const bool proc = profile == VAProfileNone && entrypoint == VAEntrypointVideoProc;
    if (!proc) {
        if (!driver.codec(profile)) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        if (entrypoint != VAEntrypointVLD) return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }
    Config config;
    config.profile = profile;
    config.entrypoint = entrypoint;
    config.rtFormat = proc ? (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32) : VA_RT_FORMAT_YUV420;
    for (int index = 0; index < count; ++index) {
        if (attributes[index].type != VAConfigAttribRTFormat) continue;
        const unsigned allowed = proc ? (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32) : VA_RT_FORMAT_YUV420;
        if ((attributes[index].value & allowed) != attributes[index].value)
            return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
        config.rtFormat = attributes[index].value;
    }
    *id = driver.nextConfig++;
    driver.configs.emplace(*id, config);
    return VA_STATUS_SUCCESS;
}

VAStatus destroyConfig(VADriverContextP ctx, VAConfigID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    return driver.configs.erase(id) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_CONFIG;
}

VAStatus queryConfigAttributes(VADriverContextP ctx, VAConfigID id, VAProfile* profile, VAEntrypoint* entrypoint,
                               VAConfigAttrib* attributes, int* count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    const auto found = driver.configs.find(id);
    if (found == driver.configs.end()) return VA_STATUS_ERROR_INVALID_CONFIG;
    *profile = found->second.profile;
    *entrypoint = found->second.entrypoint;
    attributes[0].type = VAConfigAttribRTFormat;
    attributes[0].value = found->second.entrypoint == VAEntrypointVideoProc
        ? (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32)
        : VA_RT_FORMAT_YUV420;
    *count = 1;
    return VA_STATUS_SUCCESS;
}

VAStatus querySurfaceAttributes(VADriverContextP ctx, VAConfigID id, VASurfaceAttrib* attributes, unsigned* count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    const auto found = driver.configs.find(id);
    if (found == driver.configs.end()) return VA_STATUS_ERROR_INVALID_CONFIG;
    const bool proc = found->second.entrypoint == VAEntrypointVideoProc;
    const CodecSupport* codec = proc ? nullptr : driver.codec(found->second.profile);
    if (!proc && !codec) return VA_STATUS_ERROR_INVALID_CONFIG;
    std::vector<VASurfaceAttrib> list;
    const auto pixelFormat = [&list](std::uint32_t fourcc) {
        VASurfaceAttrib attribute {};
        attribute.type = VASurfaceAttribPixelFormat;
        attribute.flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
        attribute.value.type = VAGenericValueTypeInteger;
        attribute.value.value.i = int(fourcc);
        list.push_back(attribute);
    };
    const auto integer = [&list](VASurfaceAttribType type, int value) {
        VASurfaceAttrib attribute {};
        attribute.type = type;
        attribute.flags = VA_SURFACE_ATTRIB_GETTABLE;
        attribute.value.type = VAGenericValueTypeInteger;
        attribute.value.value.i = value;
        list.push_back(attribute);
    };
    pixelFormat(VA_FOURCC_NV12);
    if (proc) pixelFormat(VA_FOURCC_ARGB);
    integer(VASurfaceAttribMinWidth, proc ? 16 : int(codec->minWidth));
    integer(VASurfaceAttribMinHeight, proc ? 16 : int(codec->minHeight));
    integer(VASurfaceAttribMaxWidth, proc ? 4096 : int(codec->maxWidth));
    integer(VASurfaceAttribMaxHeight, proc ? 4096 : int(codec->maxHeight));
    VASurfaceAttrib memory {};
    memory.type = VASurfaceAttribMemoryType;
    memory.flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    memory.value.type = VAGenericValueTypeInteger;
    memory.value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME |
                           VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    list.push_back(memory);
    if (!attributes) { *count = unsigned(list.size()); return VA_STATUS_SUCCESS; }
    if (*count < list.size()) { *count = unsigned(list.size()); return VA_STATUS_ERROR_MAX_NUM_EXCEEDED; }
    std::copy(list.begin(), list.end(), attributes);
    *count = unsigned(list.size());
    return VA_STATUS_SUCCESS;
}

VAStatus createSurfaces2(VADriverContextP ctx, unsigned format, unsigned width, unsigned height,
                         VASurfaceID* ids, unsigned count, VASurfaceAttrib* attributes, unsigned attributeCount)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    std::uint32_t fourcc = defaultFourcc(format);
    unsigned memoryType = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    void* descriptor = nullptr;
    for (unsigned index = 0; index < attributeCount; ++index) {
        switch (attributes[index].type) {
        case VASurfaceAttribPixelFormat:
            fourcc = std::uint32_t(attributes[index].value.value.i);
            break;
        case VASurfaceAttribMemoryType:
            memoryType = unsigned(attributes[index].value.value.i);
            break;
        case VASurfaceAttribExternalBufferDescriptor:
            descriptor = attributes[index].value.value.p;
            break;
        default:
            break;
        }
    }
    if (!fourcc || !supportedFourcc(fourcc)) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (descriptor && count != 1) return VA_STATUS_ERROR_INVALID_PARAMETER;

    for (unsigned index = 0; index < count; ++index) {
        auto surface = std::make_unique<Surface>();
        surface->width = width;
        surface->height = height;
        surface->rtFormat = format;
        surface->fourcc = fourcc;
        if (descriptor) {
            int fd = -1;
            unsigned planes = 0;
            std::uint32_t pitch[4] {};
            std::uint32_t offset[4] {};
            std::uint64_t modifier = 0;
            std::size_t size = 0;
            if (memoryType == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
                const auto* prime = static_cast<const VADRMPRIMESurfaceDescriptor*>(descriptor);
                if (!prime->num_objects || !prime->num_layers) return VA_STATUS_ERROR_INVALID_PARAMETER;
                fourcc = prime->fourcc;
                fd = prime->objects[0].fd;
                modifier = prime->objects[0].drm_format_modifier;
                size = prime->objects[0].size;
                for (unsigned layer = 0; layer < prime->num_layers && planes < 4; ++layer)
                    for (unsigned plane = 0; plane < prime->layers[layer].num_planes && planes < 4; ++plane) {
                        pitch[planes] = prime->layers[layer].pitch[plane];
                        offset[planes] = prime->layers[layer].offset[plane];
                        ++planes;
                    }
            } else if (memoryType == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME) {
                const auto* external = static_cast<const VASurfaceAttribExternalBuffers*>(descriptor);
                if (!external->num_buffers || !external->num_planes) return VA_STATUS_ERROR_INVALID_PARAMETER;
                fourcc = external->pixel_format;
                fd = int(external->buffers[0]);
                size = external->data_size;
                planes = std::min<unsigned>(external->num_planes, 4);
                for (unsigned plane = 0; plane < planes; ++plane) {
                    pitch[plane] = external->pitches[plane];
                    offset[plane] = external->offsets[plane];
                }
            } else {
                return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
            }
            surface->fourcc = fourcc;
            if (auto shared = driver.knownBuffer(identityOfDescriptor(fd))) {
                surface->buffer = std::move(shared);
            } else {
                const int duplicate = fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
                std::string error;
                auto adopted = PixelBuffer::adopt(duplicate, fourcc, width, height, planes, pitch, offset,
                                                  modifier, size, error);
                if (!adopted) { if (duplicate >= 0) ::close(duplicate); return VA_STATUS_ERROR_INVALID_PARAMETER; }
                surface->buffer = std::shared_ptr<PixelBuffer>(adopted.release());
                driver.known.emplace(surface->buffer->identity(), surface->buffer);
            }
        } else {
            std::string error;
            auto allocated = PixelBuffer::allocate(fourcc, width, height, error);
            if (!allocated) { driver.note(error.c_str()); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
            surface->buffer = std::shared_ptr<PixelBuffer>(allocated.release());
            driver.known.emplace(surface->buffer->identity(), surface->buffer);
        }
        ids[index] = driver.nextSurface++;
        driver.surfaces.emplace(ids[index], std::move(surface));
    }
    return VA_STATUS_SUCCESS;
}

VAStatus createSurfaces(VADriverContextP ctx, int width, int height, int format, int count, VASurfaceID* ids)
{
    return createSurfaces2(ctx, unsigned(format), unsigned(width), unsigned(height), ids, unsigned(count),
                           nullptr, 0);
}

VAStatus destroySurfaces(VADriverContextP ctx, VASurfaceID* ids, int count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    for (int index = 0; index < count; ++index) {
        Surface* surface = driver.surface(ids[index]);
        if (!surface) continue;
        if (Context* owner = driver.context(surface->owner))
            std::erase_if(owner->awaiting, [&ids, index](const auto& entry) { return entry.second == ids[index]; });
        driver.surfaces.erase(ids[index]);
    }
    return VA_STATUS_SUCCESS;
}

VAStatus createContext(VADriverContextP ctx, VAConfigID configId, int width, int height, int flag,
                       VASurfaceID* targets, int targetCount, VAContextID* id)
{
    (void)flag;
    (void)targets;
    (void)targetCount;
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    const auto found = driver.configs.find(configId);
    if (found == driver.configs.end()) return VA_STATUS_ERROR_INVALID_CONFIG;
    auto context = std::make_unique<Context>();
    context->config = configId;
    context->profile = found->second.profile;
    context->entrypoint = found->second.entrypoint;
    context->width = unsigned(width);
    context->height = unsigned(height);
    for (auto& slot : context->slot) slot = VA_INVALID_SURFACE;
    if (context->entrypoint == VAEntrypointVLD) {
        const CodecSupport* codec = driver.codec(context->profile);
        if (!codec) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
        if (!width || !height) return VA_STATUS_ERROR_INVALID_PARAMETER;
        std::string error;
        context->decoder = V4l2Decoder::open(driver.device(), codec->v4l2Format, unsigned(width),
                                             unsigned(height), driver.preferredCapture(), error);
        if (!context->decoder) { driver.note(error.c_str()); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
        // The decoded format is only settled when the decoder has parsed the
        // stream, so it is reported with the layout at the first picture.
        context->reportLayout = true;
    }
    *id = driver.nextContext++;
    driver.contexts.emplace(*id, std::move(context));
    return VA_STATUS_SUCCESS;
}

VAStatus destroyContext(VADriverContextP ctx, VAContextID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Context* context = driver.context(id);
    if (!context) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (context->decoder) {
        std::string error;
        context->decoder->drain(error);
        driver.harvest(*context, std::chrono::milliseconds(200));
    }
    for (auto& entry : driver.surfaces)
        if (entry.second->owner == id) {
            entry.second->owner = VA_INVALID_ID;
            if (entry.second->state == Surface::State::pending) entry.second->state = Surface::State::failed;
        }
    driver.contexts.erase(id);
    return VA_STATUS_SUCCESS;
}

VAStatus createBuffer(VADriverContextP ctx, VAContextID, VABufferType type, unsigned size, unsigned elements,
                      void* data, VABufferID* id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    if (!size || !elements) return VA_STATUS_ERROR_INVALID_PARAMETER;
    auto buffer = std::make_unique<Buffer>();
    buffer->type = type;
    buffer->size = size;
    buffer->elements = elements;
    buffer->owned.resize(std::size_t(size) * elements);
    if (data) std::memcpy(buffer->owned.data(), data, buffer->owned.size());
    *id = driver.nextBuffer++;
    driver.buffers.emplace(*id, std::move(buffer));
    return VA_STATUS_SUCCESS;
}

VAStatus bufferSetNumElements(VADriverContextP ctx, VABufferID id, unsigned elements)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Buffer* buffer = driver.buffer(id);
    if (!buffer || buffer->external) return VA_STATUS_ERROR_INVALID_BUFFER;
    buffer->elements = elements;
    buffer->owned.resize(std::size_t(buffer->size) * elements);
    return VA_STATUS_SUCCESS;
}

VAStatus mapBuffer(VADriverContextP ctx, VABufferID id, void** address)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Buffer* buffer = driver.buffer(id);
    if (!buffer) return VA_STATUS_ERROR_INVALID_BUFFER;
    std::uint8_t* data = buffer->data();
    if (!data) return VA_STATUS_ERROR_OPERATION_FAILED;
    *address = data;
    return VA_STATUS_SUCCESS;
}

VAStatus mapBuffer2(VADriverContextP ctx, VABufferID id, void** address, uint32_t)
{
    return mapBuffer(ctx, id, address);
}

VAStatus unmapBuffer(VADriverContextP ctx, VABufferID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    return driver.buffer(id) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
}

VAStatus destroyBuffer(VADriverContextP ctx, VABufferID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    return driver.buffers.erase(id) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
}

VAStatus bufferInfo(VADriverContextP ctx, VABufferID id, VABufferType* type, unsigned* size, unsigned* elements)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Buffer* buffer = driver.buffer(id);
    if (!buffer) return VA_STATUS_ERROR_INVALID_BUFFER;
    *type = buffer->type;
    *size = buffer->size;
    *elements = buffer->elements;
    return VA_STATUS_SUCCESS;
}

VAStatus beginPicture(VADriverContextP ctx, VAContextID id, VASurfaceID target)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Context* context = driver.context(id);
    if (!context) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!driver.surface(target)) return VA_STATUS_ERROR_INVALID_SURFACE;
    context->target = target;
    context->picture = true;
    context->haveSliceData = false;
    context->sliceData.clear();
    context->havePipeline = false;
    context->pipelineSourceValid = false;
    context->pipelineDestinationValid = false;
    return VA_STATUS_SUCCESS;
}

VAStatus renderPicture(VADriverContextP ctx, VAContextID id, VABufferID* ids, int count)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Context* context = driver.context(id);
    if (!context || !context->picture) return VA_STATUS_ERROR_INVALID_CONTEXT;
    for (int index = 0; index < count; ++index) {
        Buffer* buffer = driver.buffer(ids[index]);
        if (!buffer) return VA_STATUS_ERROR_INVALID_BUFFER;
        switch (buffer->type) {
        case VASliceDataBufferType: {
            const std::uint8_t* data = buffer->data();
            const std::size_t size = std::size_t(buffer->size) * buffer->elements;
            context->sliceData.insert(context->sliceData.end(), data, data + size);
            context->haveSliceData = true;
            break;
        }
        case VAProcPipelineParameterBufferType: {
            if (std::size_t(buffer->size) * buffer->elements < sizeof(VAProcPipelineParameterBuffer))
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            std::memcpy(&context->pipeline, buffer->data(), sizeof(VAProcPipelineParameterBuffer));
            context->havePipeline = true;
            if (context->pipeline.surface_region) {
                context->pipelineSource = *context->pipeline.surface_region;
                context->pipelineSourceValid = true;
            }
            if (context->pipeline.output_region) {
                context->pipelineDestination = *context->pipeline.output_region;
                context->pipelineDestinationValid = true;
            }
            break;
        }
        case VAPictureParameterBufferType:
        case VASliceParameterBufferType:
        case VAIQMatrixBufferType:
        case VAProbabilityBufferType:
            // The stateful decoder reads these from the bitstream itself.
            break;
        default:
            break;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus endPicture(VADriverContextP ctx, VAContextID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Context* context = driver.context(id);
    if (!context || !context->picture) return VA_STATUS_ERROR_INVALID_CONTEXT;
    context->picture = false;
    Surface* target = driver.surface(context->target);
    if (!target) return VA_STATUS_ERROR_INVALID_SURFACE;
    target->owner = id;
    const auto status = context->entrypoint == VAEntrypointVideoProc
        ? driver.process(*context, *target)
        : driver.submitFrame(*context, context->target, *target);
    context->sliceData.clear();
    context->haveSliceData = false;
    return status;
}

VAStatus syncSurface(VADriverContextP ctx, VASurfaceID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    if (!surface) return VA_STATUS_ERROR_INVALID_SURFACE;
    return driver.waitFor(*surface, kDecodeWait);
}

VAStatus syncSurface2(VADriverContextP ctx, VASurfaceID id, uint64_t timeout)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    if (!surface) return VA_STATUS_ERROR_INVALID_SURFACE;
    const auto wait = timeout == VA_TIMEOUT_INFINITE
        ? kDecodeWait
        : std::chrono::milliseconds(timeout / 1000000ull);
    const auto status = driver.waitFor(*surface, wait);
    if (status != VA_STATUS_SUCCESS && surface->state == Surface::State::pending)
        return VA_STATUS_ERROR_TIMEDOUT;
    return status;
}

VAStatus syncBuffer(VADriverContextP ctx, VABufferID id, uint64_t)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    return driver.buffer(id) ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_BUFFER;
}

VAStatus querySurfaceStatus(VADriverContextP ctx, VASurfaceID id, VASurfaceStatus* status)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    if (!surface) return VA_STATUS_ERROR_INVALID_SURFACE;
    switch (surface->state) {
    case Surface::State::pending: *status = VASurfaceRendering; break;
    case Surface::State::failed: *status = VASurfaceReady; break;
    default: *status = VASurfaceReady; break;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus querySurfaceError(VADriverContextP, VASurfaceID, VAStatus, void** info)
{
    if (info) *info = nullptr;
    return VA_STATUS_SUCCESS;
}

const VAImageFormat kImageFormats[] = {
    {VA_FOURCC_NV12, VA_LSB_FIRST, 12, 0, 0, 0, 0, 0, {}},
    {VA_FOURCC_I420, VA_LSB_FIRST, 12, 0, 0, 0, 0, 0, {}},
    {VA_FOURCC_ARGB, VA_LSB_FIRST, 32, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000, {}},
};

VAStatus queryImageFormats(VADriverContextP, VAImageFormat* formats, int* count)
{
    const int total = int(sizeof(kImageFormats) / sizeof(kImageFormats[0]));
    for (int index = 0; index < total; ++index) formats[index] = kImageFormats[index];
    *count = total;
    return VA_STATUS_SUCCESS;
}

bool imageLayout(std::uint32_t fourcc, unsigned width, unsigned height, VAImage& image) noexcept
{
    const unsigned even = (width + 1u) & ~1u;
    const unsigned rows = (height + 1u) & ~1u;
    switch (fourcc) {
    case VA_FOURCC_NV12:
        image.num_planes = 2;
        image.pitches[0] = even;
        image.offsets[0] = 0;
        image.pitches[1] = even;
        image.offsets[1] = even * rows;
        image.data_size = image.offsets[1] + even * (rows / 2);
        return true;
    case VA_FOURCC_I420:
        image.num_planes = 3;
        image.pitches[0] = even;
        image.offsets[0] = 0;
        image.pitches[1] = even / 2;
        image.offsets[1] = even * rows;
        image.pitches[2] = even / 2;
        image.offsets[2] = image.offsets[1] + (even / 2) * (rows / 2);
        image.data_size = image.offsets[2] + (even / 2) * (rows / 2);
        return true;
    case VA_FOURCC_ARGB:
        image.num_planes = 1;
        image.pitches[0] = width * 4;
        image.offsets[0] = 0;
        image.data_size = image.pitches[0] * height;
        return true;
    default:
        return false;
    }
}

VAStatus createImage(VADriverContextP ctx, VAImageFormat* format, int width, int height, VAImage* out)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    if (!format || width <= 0 || height <= 0) return VA_STATUS_ERROR_INVALID_PARAMETER;
    VAImage image {};
    image.format = *format;
    image.width = static_cast<unsigned short>(width);
    image.height = static_cast<unsigned short>(height);
    if (!imageLayout(format->fourcc, unsigned(width), unsigned(height), image))
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    auto buffer = std::make_unique<Buffer>();
    buffer->type = VAImageBufferType;
    buffer->size = image.data_size;
    buffer->elements = 1;
    buffer->owned.resize(image.data_size);
    const auto bufferId = driver.nextBuffer++;
    driver.buffers.emplace(bufferId, std::move(buffer));
    image.buf = bufferId;
    image.image_id = driver.nextImage++;
    auto record = std::make_unique<Image>();
    record->image = image;
    driver.images.emplace(image.image_id, std::move(record));
    *out = image;
    return VA_STATUS_SUCCESS;
}

VAStatus deriveImage(VADriverContextP ctx, VASurfaceID id, VAImage* out)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    if (!surface || !surface->buffer) return VA_STATUS_ERROR_INVALID_SURFACE;
    const auto status = driver.waitFor(*surface, kDecodeWait);
    if (status != VA_STATUS_SUCCESS) return status;
    driver.materialise(*surface);
    std::uint8_t* data = surface->buffer->map();
    if (!data) return VA_STATUS_ERROR_OPERATION_FAILED;
    VAImage image {};
    for (const auto& format : kImageFormats)
        if (format.fourcc == surface->fourcc) image.format = format;
    if (!image.format.fourcc) return VA_STATUS_ERROR_OPERATION_FAILED;
    image.width = static_cast<unsigned short>(surface->width);
    image.height = static_cast<unsigned short>(surface->height);
    image.num_planes = surface->buffer->planes();
    for (unsigned plane = 0; plane < image.num_planes; ++plane) {
        image.pitches[plane] = unsigned(surface->buffer->pitch(plane));
        image.offsets[plane] = unsigned(surface->buffer->offset(plane));
    }
    image.data_size = unsigned(surface->buffer->size());
    auto buffer = std::make_unique<Buffer>();
    buffer->type = VAImageBufferType;
    buffer->size = image.data_size;
    buffer->elements = 1;
    buffer->external = data;
    buffer->externalSize = surface->buffer->size();
    const auto bufferId = driver.nextBuffer++;
    driver.buffers.emplace(bufferId, std::move(buffer));
    image.buf = bufferId;
    image.image_id = driver.nextImage++;
    auto record = std::make_unique<Image>();
    record->image = image;
    record->derivedFrom = id;
    driver.images.emplace(image.image_id, std::move(record));
    *out = image;
    return VA_STATUS_SUCCESS;
}

VAStatus destroyImage(VADriverContextP ctx, VAImageID id)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    const auto found = driver.images.find(id);
    if (found == driver.images.end()) return VA_STATUS_ERROR_INVALID_IMAGE;
    driver.buffers.erase(found->second->image.buf);
    driver.images.erase(found);
    return VA_STATUS_SUCCESS;
}

// Copies a surface region into an image buffer, converting between the few
// layouts clients ask for.
VAStatus readSurface(Driver& driver, Surface& surface, const Region& region, Image& target)
{
    driver.materialise(surface);
    std::uint8_t* destination = nullptr;
    if (Buffer* buffer = driver.buffer(target.image.buf)) destination = buffer->data();
    const std::uint8_t* source = surface.buffer ? surface.buffer->map() : nullptr;
    if (!destination || !source) return VA_STATUS_ERROR_OPERATION_FAILED;
    const auto width = std::min<unsigned>(region.width, target.image.width);
    const auto height = std::min<unsigned>(region.height, target.image.height);
    if (surface.fourcc == VA_FOURCC_NV12 && target.image.format.fourcc == VA_FOURCC_NV12) {
        const ConstPlaneView in[2] {
            {source + surface.buffer->offset(0) + region.y * surface.buffer->pitch(0) + region.x,
             surface.buffer->pitch(0)},
            {source + surface.buffer->offset(1) + (region.y / 2) * surface.buffer->pitch(1) + (region.x & ~1u),
             surface.buffer->pitch(1)}};
        const PlaneView out[2] {{destination + target.image.offsets[0], target.image.pitches[0]},
                                {destination + target.image.offsets[1], target.image.pitches[1]}};
        copyNv12(in, out, width, height);
        return VA_STATUS_SUCCESS;
    }
    if (surface.fourcc == VA_FOURCC_NV12 && target.image.format.fourcc == VA_FOURCC_I420) {
        const std::uint8_t* luma = source + surface.buffer->offset(0) + region.y * surface.buffer->pitch(0) + region.x;
        const std::uint8_t* chroma = source + surface.buffer->offset(1) +
                                     (region.y / 2) * surface.buffer->pitch(1) + (region.x & ~1u);
        for (unsigned row = 0; row < height; ++row)
            std::memcpy(destination + target.image.offsets[0] + std::size_t(row) * target.image.pitches[0],
                        luma + std::size_t(row) * surface.buffer->pitch(0), width);
        for (unsigned row = 0; row < (height + 1) / 2; ++row) {
            std::uint8_t* u = destination + target.image.offsets[1] + std::size_t(row) * target.image.pitches[1];
            std::uint8_t* v = destination + target.image.offsets[2] + std::size_t(row) * target.image.pitches[2];
            const std::uint8_t* pair = chroma + std::size_t(row) * surface.buffer->pitch(1);
            for (unsigned column = 0; column < (width + 1) / 2; ++column) {
                u[column] = pair[2 * column];
                v[column] = pair[2 * column + 1];
            }
        }
        return VA_STATUS_SUCCESS;
    }
    if (surface.fourcc == VA_FOURCC_NV12 && target.image.format.fourcc == VA_FOURCC_ARGB) {
        const ConstPlaneView in[2] {{source + surface.buffer->offset(0), surface.buffer->pitch(0)},
                                    {source + surface.buffer->offset(1), surface.buffer->pitch(1)}};
        const PlaneView out {destination + target.image.offsets[0], target.image.pitches[0]};
        convertNv12ToArgb(in, region, out, Region {0, 0, width, height}, isBt709(surface.height));
        return VA_STATUS_SUCCESS;
    }
    if (surface.fourcc == target.image.format.fourcc && surface.buffer->planes() == 1) {
        const ConstPlaneView in {source + surface.buffer->offset(0), surface.buffer->pitch(0)};
        const PlaneView out {destination + target.image.offsets[0], target.image.pitches[0]};
        copyPacked32(in, region, out, Region {0, 0, width, height});
        return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus getImage(VADriverContextP ctx, VASurfaceID id, int x, int y, unsigned width, unsigned height,
                  VAImageID imageId)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    const auto found = driver.images.find(imageId);
    if (!surface || found == driver.images.end()) return VA_STATUS_ERROR_INVALID_IMAGE;
    if (x < 0 || y < 0) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (unsigned(x) + width > surface->width || unsigned(y) + height > surface->height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    const auto status = driver.waitFor(*surface, kDecodeWait);
    if (status != VA_STATUS_SUCCESS) return status;
    return readSurface(driver, *surface, Region {unsigned(x), unsigned(y), width, height}, *found->second);
}

VAStatus putImage(VADriverContextP ctx, VASurfaceID id, VAImageID imageId, int sourceX, int sourceY,
                  unsigned sourceWidth, unsigned sourceHeight, int destinationX, int destinationY,
                  unsigned destinationWidth, unsigned destinationHeight)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    const auto found = driver.images.find(imageId);
    if (!surface || found == driver.images.end() || !surface->buffer) return VA_STATUS_ERROR_INVALID_IMAGE;
    if (sourceX < 0 || sourceY < 0 || destinationX < 0 || destinationY < 0) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (sourceWidth != destinationWidth || sourceHeight != destinationHeight) return VA_STATUS_ERROR_UNIMPLEMENTED;
    const VAImage& image = found->second->image;
    if (image.format.fourcc != surface->fourcc) return VA_STATUS_ERROR_UNIMPLEMENTED;
    Buffer* buffer = driver.buffer(image.buf);
    std::uint8_t* destination = surface->buffer->map();
    if (!buffer || !destination) return VA_STATUS_ERROR_OPERATION_FAILED;
    const std::uint8_t* source = buffer->data();
    if (unsigned(destinationX) + destinationWidth > surface->width ||
        unsigned(destinationY) + destinationHeight > surface->height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    // Whatever the decoder left in a shadow is now stale.
    surface->shadow.reset();
    surface->nativeValid = true;
    if (surface->fourcc == VA_FOURCC_NV12) {
        const ConstPlaneView in[2] {
            {source + image.offsets[0] + std::size_t(sourceY) * image.pitches[0] + sourceX, image.pitches[0]},
            {source + image.offsets[1] + std::size_t(sourceY / 2) * image.pitches[1] + (unsigned(sourceX) & ~1u),
             image.pitches[1]}};
        const PlaneView out[2] {
            {destination + surface->buffer->offset(0) + std::size_t(destinationY) * surface->buffer->pitch(0) +
                 destinationX,
             surface->buffer->pitch(0)},
            {destination + surface->buffer->offset(1) +
                 std::size_t(destinationY / 2) * surface->buffer->pitch(1) + (unsigned(destinationX) & ~1u),
             surface->buffer->pitch(1)}};
        copyNv12(in, out, destinationWidth, destinationHeight);
    } else {
        const ConstPlaneView in {source + image.offsets[0], image.pitches[0]};
        const PlaneView out {destination + surface->buffer->offset(0), surface->buffer->pitch(0)};
        copyPacked32(in, Region {unsigned(sourceX), unsigned(sourceY), sourceWidth, sourceHeight}, out,
                     Region {unsigned(destinationX), unsigned(destinationY), destinationWidth, destinationHeight});
    }
    surface->state = Surface::State::ready;
    return VA_STATUS_SUCCESS;
}

VAStatus exportSurfaceHandle(VADriverContextP ctx, VASurfaceID id, uint32_t memoryType, uint32_t flags,
                             void* out)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    if (!surface || !surface->buffer) return VA_STATUS_ERROR_INVALID_SURFACE;
    if (memoryType != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    // A descriptor names the surface's own format, so it has to hold the
    // surface's own pixels even when the decoder delivered packed RGB.
    driver.materialise(*surface);
    const int fd = surface->buffer->exportDescriptor();
    if (fd < 0) return VA_STATUS_ERROR_OPERATION_FAILED;
    auto* descriptor = static_cast<VADRMPRIMESurfaceDescriptor*>(out);
    std::memset(descriptor, 0, sizeof(*descriptor));
    descriptor->fourcc = surface->fourcc;
    descriptor->width = surface->width;
    descriptor->height = surface->height;
    descriptor->num_objects = 1;
    descriptor->objects[0].fd = fd;
    descriptor->objects[0].size = unsigned(surface->buffer->size());
    descriptor->objects[0].drm_format_modifier = surface->buffer->modifier();
    const unsigned planes = surface->buffer->planes();
    if (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) {
        descriptor->num_layers = planes;
        for (unsigned plane = 0; plane < planes; ++plane) {
            descriptor->layers[plane].drm_format = surface->fourcc == VA_FOURCC_NV12
                ? (plane == 0 ? DRM_FORMAT_R8 : DRM_FORMAT_GR88)
                : DRM_FORMAT_ARGB8888;
            descriptor->layers[plane].num_planes = 1;
            descriptor->layers[plane].object_index[0] = 0;
            descriptor->layers[plane].offset[0] = unsigned(surface->buffer->offset(plane));
            descriptor->layers[plane].pitch[0] = unsigned(surface->buffer->pitch(plane));
        }
    } else {
        descriptor->num_layers = 1;
        descriptor->layers[0].drm_format = surface->fourcc == VA_FOURCC_NV12 ? DRM_FORMAT_NV12 : DRM_FORMAT_ARGB8888;
        descriptor->layers[0].num_planes = planes;
        for (unsigned plane = 0; plane < planes; ++plane) {
            descriptor->layers[0].object_index[plane] = 0;
            descriptor->layers[0].offset[plane] = unsigned(surface->buffer->offset(plane));
            descriptor->layers[0].pitch[plane] = unsigned(surface->buffer->pitch(plane));
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus lockSurface(VADriverContextP ctx, VASurfaceID id, unsigned* fourcc, unsigned* lumaStride,
                     unsigned* chromaUStride, unsigned* chromaVStride, unsigned* lumaOffset,
                     unsigned* chromaUOffset, unsigned* chromaVOffset, unsigned* name, void** address)
{
    auto& driver = driverOf(ctx);
    std::lock_guard guard(driver.lock());
    Surface* surface = driver.surface(id);
    if (!surface || !surface->buffer) return VA_STATUS_ERROR_INVALID_SURFACE;
    const auto status = driver.waitFor(*surface, kDecodeWait);
    if (status != VA_STATUS_SUCCESS) return status;
    if (fourcc) *fourcc = surface->fourcc;
    if (lumaStride) *lumaStride = unsigned(surface->buffer->pitch(0));
    if (lumaOffset) *lumaOffset = unsigned(surface->buffer->offset(0));
    const unsigned chromaPlane = surface->buffer->planes() > 1 ? 1 : 0;
    if (chromaUStride) *chromaUStride = unsigned(surface->buffer->pitch(chromaPlane));
    if (chromaVStride) *chromaVStride = unsigned(surface->buffer->pitch(chromaPlane));
    if (chromaUOffset) *chromaUOffset = unsigned(surface->buffer->offset(chromaPlane));
    if (chromaVOffset) *chromaVOffset = unsigned(surface->buffer->offset(chromaPlane));
    if (name) *name = 0;
    if (address) *address = surface->buffer->map();
    return VA_STATUS_SUCCESS;
}

VAStatus unlockSurface(VADriverContextP, VASurfaceID) { return VA_STATUS_SUCCESS; }

// libva refuses a driver with a hole in its table, so the parts of VA-API this
// backend does not serve answer honestly instead of being absent.
VAStatus setImagePalette(VADriverContextP, VAImageID, unsigned char*) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus createSubpicture(VADriverContextP, VAImageID, VASubpictureID*) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus destroySubpicture(VADriverContextP, VASubpictureID) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus setSubpictureImage(VADriverContextP, VASubpictureID, VAImageID) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus setSubpictureChromakey(VADriverContextP, VASubpictureID, unsigned, unsigned, unsigned)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
VAStatus setSubpictureGlobalAlpha(VADriverContextP, VASubpictureID, float) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus associateSubpicture(VADriverContextP, VASubpictureID, VASurfaceID*, int, short, short, unsigned short,
                             unsigned short, short, short, unsigned short, unsigned short, unsigned)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
VAStatus deassociateSubpicture(VADriverContextP, VASubpictureID, VASurfaceID*, int)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus unimplemented() { return VA_STATUS_ERROR_UNIMPLEMENTED; }

VAStatus putSurface(VADriverContextP, VASurfaceID, void*, short, short, unsigned short, unsigned short,
                    short, short, unsigned short, unsigned short, VARectangle*, unsigned, unsigned)
{
    return unimplemented();
}

VAStatus querySubpictureFormats(VADriverContextP, VAImageFormat*, unsigned*, unsigned* count)
{
    *count = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus queryDisplayAttributes(VADriverContextP, VADisplayAttribute*, int* count)
{
    *count = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus getDisplayAttributes(VADriverContextP, VADisplayAttribute*, int) { return VA_STATUS_ERROR_UNIMPLEMENTED; }
VAStatus setDisplayAttributes(VADriverContextP, VADisplayAttribute*, int) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

VAStatus queryVideoProcFilters(VADriverContextP, VAContextID, VAProcFilterType*, unsigned* count)
{
    *count = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus queryVideoProcFilterCaps(VADriverContextP, VAContextID, VAProcFilterType, void*, unsigned* count)
{
    *count = 0;
    return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
}

VAStatus queryVideoProcPipelineCaps(VADriverContextP, VAContextID, VABufferID*, unsigned count,
                                    VAProcPipelineCaps* caps)
{
    if (count) return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
    if (!caps) return VA_STATUS_ERROR_INVALID_PARAMETER;
    std::memset(caps, 0, sizeof(*caps));
    caps->pipeline_flags = 0;
    caps->filter_flags = 0;
    caps->num_forward_references = 0;
    caps->num_backward_references = 0;
    caps->rotation_flags = 1u << VA_ROTATION_NONE;
    caps->min_input_width = 16;
    caps->min_input_height = 16;
    caps->max_input_width = 4096;
    caps->max_input_height = 4096;
    caps->min_output_width = 16;
    caps->min_output_height = 16;
    caps->max_output_width = 4096;
    caps->max_output_height = 4096;
    return VA_STATUS_SUCCESS;
}

}
}

extern "C" VAStatus __vaDriverInit_1_22(VADriverContextP ctx);

extern "C" VAStatus __vaDriverInit_1_22(VADriverContextP ctx)
{
    using namespace uml::va;
    if (!ctx) return VA_STATUS_ERROR_INVALID_CONTEXT;
    ctx->version_major = VA_MAJOR_VERSION;
    ctx->version_minor = VA_MINOR_VERSION;
    ctx->max_profiles = 8;
    ctx->max_entrypoints = 2;
    ctx->max_attributes = 8;
    ctx->max_image_formats = 4;
    ctx->max_subpic_formats = 1;
    ctx->max_display_attributes = 1;
    ctx->str_vendor = kVendor;

    auto* driver = new (std::nothrow) Driver(ctx);
    if (!driver) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (driver->codecs().empty()) {
        driver->note("no usable decoder; refusing to initialise");
        delete driver;
        return VA_STATUS_ERROR_HW_BUSY;
    }
    ctx->pDriverData = driver;

    auto* table = ctx->vtable;
    table->vaTerminate = terminate;
    table->vaQueryConfigProfiles = queryConfigProfiles;
    table->vaQueryConfigEntrypoints = queryConfigEntrypoints;
    table->vaGetConfigAttributes = getConfigAttributes;
    table->vaCreateConfig = createConfig;
    table->vaDestroyConfig = destroyConfig;
    table->vaQueryConfigAttributes = queryConfigAttributes;
    table->vaCreateSurfaces = createSurfaces;
    table->vaCreateSurfaces2 = createSurfaces2;
    table->vaDestroySurfaces = destroySurfaces;
    table->vaQuerySurfaceAttributes = querySurfaceAttributes;
    table->vaCreateContext = createContext;
    table->vaDestroyContext = destroyContext;
    table->vaCreateBuffer = createBuffer;
    table->vaBufferSetNumElements = bufferSetNumElements;
    table->vaMapBuffer = mapBuffer;
    table->vaMapBuffer2 = mapBuffer2;
    table->vaUnmapBuffer = unmapBuffer;
    table->vaDestroyBuffer = destroyBuffer;
    table->vaBufferInfo = bufferInfo;
    table->vaBeginPicture = beginPicture;
    table->vaRenderPicture = renderPicture;
    table->vaEndPicture = endPicture;
    table->vaSyncSurface = syncSurface;
    table->vaSyncSurface2 = syncSurface2;
    table->vaSyncBuffer = syncBuffer;
    table->vaQuerySurfaceStatus = querySurfaceStatus;
    table->vaQuerySurfaceError = querySurfaceError;
    table->vaPutSurface = putSurface;
    table->vaQueryImageFormats = queryImageFormats;
    table->vaCreateImage = createImage;
    table->vaDeriveImage = deriveImage;
    table->vaDestroyImage = destroyImage;
    table->vaGetImage = getImage;
    table->vaPutImage = putImage;
    table->vaQuerySubpictureFormats = querySubpictureFormats;
    table->vaQueryDisplayAttributes = queryDisplayAttributes;
    table->vaGetDisplayAttributes = getDisplayAttributes;
    table->vaSetDisplayAttributes = setDisplayAttributes;
    table->vaSetImagePalette = setImagePalette;
    table->vaCreateSubpicture = createSubpicture;
    table->vaDestroySubpicture = destroySubpicture;
    table->vaSetSubpictureImage = setSubpictureImage;
    table->vaSetSubpictureChromakey = setSubpictureChromakey;
    table->vaSetSubpictureGlobalAlpha = setSubpictureGlobalAlpha;
    table->vaAssociateSubpicture = associateSubpicture;
    table->vaDeassociateSubpicture = deassociateSubpicture;
    table->vaLockSurface = lockSurface;
    table->vaUnlockSurface = unlockSurface;
    table->vaExportSurfaceHandle = exportSurfaceHandle;

    if (auto* vpp = ctx->vtable_vpp) {
        vpp->version = VA_DRIVER_VTABLE_VPP_VERSION;
        vpp->vaQueryVideoProcFilters = queryVideoProcFilters;
        vpp->vaQueryVideoProcFilterCaps = queryVideoProcFilterCaps;
        vpp->vaQueryVideoProcPipelineCaps = queryVideoProcPipelineCaps;
    }
    return VA_STATUS_SUCCESS;
}
