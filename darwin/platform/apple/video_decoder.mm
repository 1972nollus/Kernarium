// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_decoder.hpp"

#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <TargetConditionals.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace uml::platform {
namespace {

void checked(OSStatus status, const char* operation)
{
    if (status != noErr)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(status));
}

template<class T> class CFObject {
public:
    CFObject() = default;
    // Adopts a reference a CF "Copy" function already retained for us.
    explicit CFObject(T value) : value_(value) {}
    ~CFObject() { reset(); }
    CFObject(const CFObject&) = delete;
    CFObject& operator=(const CFObject&) = delete;
    T* address() { reset(); return &value_; }
    T get() const { return value_; }
    void reset() { if (value_) CFRelease(value_); value_ = nullptr; }
private:
    T value_ {};
};

bool high10Available()
{
#if TARGET_OS_OSX
    // The existing Virgl decoder excludes macOS: a session-only probe accepts
    // High10 headers there even though hardware rejects representative frames.
    return false;
#else
    if (@available(iOS 17.0, tvOS 17.0, *)) {
        static const bool available = [] {
            // Same profile-specific sequence as the maintained Virgl backend.
            const std::uint8_t sps[] = {0x67, 0x6e, 0x00, 0x0a, 0xa6, 0xcd, 0x94, 0x42,
                0x6c, 0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x1e, 0x03,
                0xc4, 0x89, 0x65, 0x80};
            const std::uint8_t pps[] = {0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0};
            const std::uint8_t* data[] = {sps, pps};
            const std::size_t sizes[] = {sizeof(sps), sizeof(pps)};
            CFObject<CMVideoFormatDescriptionRef> description;
            CFObject<VTDecompressionSessionRef> session;
            CFObject<CFTypeRef> hardware;
            if (CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,
                2, data, sizes, 4, description.address()) != noErr) return false;
            NSDictionary* specification = @{(__bridge NSString*)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES};
            NSDictionary* attributes = @{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:
                @(kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)};
            if (VTDecompressionSessionCreate(kCFAllocatorDefault, description.get(),
                (__bridge CFDictionaryRef)specification, (__bridge CFDictionaryRef)attributes,
                nullptr, session.address()) != noErr) return false;
            const bool result = VTSessionCopyProperty(session.get(),
                kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
                kCFAllocatorDefault, hardware.address()) == noErr && hardware.get() == kCFBooleanTrue;
            VTDecompressionSessionInvalidate(session.get());
            return result;
        }();
        return available;
    }
    return false;
#endif
}

/* VP9 is a supplemental VideoToolbox decoder: `VTIsHardwareDecodeSupported`
 * answers no until the process asks for it, which is what Chromium and FFmpeg
 * do before probing. Asking once is harmless where no such decoder exists.
 */
bool vp9Available()
{
    static const bool available = [] {
        if (@available(macOS 11.0, iOS 26.2, tvOS 26.2, *)) {
            VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);
            return bool(VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9));
        }
        return false;
    }();
    return available;
}

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::uint32_t read(unsigned count)
    {
        std::uint32_t value = 0;
        while (count-- > 0) {
            if (position_ >= bytes_.size() * 8) throw std::runtime_error("truncated VP9 header");
            value = (value << 1) | ((bytes_[position_ / 8] >> (7 - position_ % 8)) & 1);
            ++position_;
        }
        return value;
    }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ {};
};

struct Vp9KeyFrame {
    VideoFormat format;
    unsigned profile;
    unsigned colorSpace;   // VP9 CS_* as the uncompressed header states it
    bool fullRange;
};

/* VP9 carries no parameter sets: a key frame's uncompressed header is the only
 * place the profile, bit depth and frame size appear, so the format is read
 * from the stream rather than configured ahead of it. Returns nothing for a
 * frame that cannot change the format, and the caller keeps the current one.
 */
std::optional<Vp9KeyFrame> vp9KeyFrame(std::span<const std::uint8_t> bytes)
{
    BitReader reader(bytes);
    if (reader.read(2) != 2) throw std::runtime_error("invalid VP9 frame marker");
    const unsigned low = reader.read(1);
    const unsigned high = reader.read(1);
    const unsigned profile = (high << 1) | low;
    if (profile == 3) reader.read(1);
    if (reader.read(1) != 0) return std::nullopt;   // show_existing_frame
    const bool key = reader.read(1) == 0;
    reader.read(1);                                 // show_frame
    reader.read(1);                                 // error_resilient_mode
    if (!key) return std::nullopt;
    if (reader.read(24) != 0x498342) throw std::runtime_error("invalid VP9 frame sync code");
    unsigned bitDepth = 8;
    if (profile >= 2) bitDepth = reader.read(1) != 0 ? 12 : 10;
    unsigned colorSpace = 0, colorRange = 0;
    colorSpace = reader.read(3);
    if (colorSpace != 7 /* SRGB */) {
        colorRange = reader.read(1);
        if (profile == 1 || profile == 3) {
            if (reader.read(1) != 1 || reader.read(1) != 1)
                throw std::runtime_error("VP9 decoder requires 4:2:0 chroma");
            reader.read(1);
        }
    } else {
        throw std::runtime_error("VP9 4:4:4 is not a supported decoder input");
    }
    const std::uint32_t width = reader.read(16) + 1;
    const std::uint32_t height = reader.read(16) + 1;
    return Vp9KeyFrame {{width, height, bitDepth}, profile, colorSpace, colorRange != 0};
}

/* VP9 states its color space in the frame header rather than in a parameter
 * set, so the configuration record has to carry it across or the decoder
 * resolves an unstated description by frame size and a stream that means
 * BT.601 at HD size is converted with the wrong matrix. The values are the
 * ISO/IEC 23001-8 code points the record is defined in. */
struct Vp9ColorDescription { std::uint8_t primaries, transfer, matrix; };

Vp9ColorDescription vp9ColorDescription(unsigned colorSpace)
{
    switch (colorSpace) {
    case 1: return {5, 6, 6};    // CS_BT_601
    case 2: return {1, 1, 1};    // CS_BT_709
    case 3: return {6, 6, 6};    // CS_SMPTE_170
    case 4: return {7, 7, 7};    // CS_SMPTE_240
    case 5: return {9, 14, 9};   // CS_BT_2020
    default: return {2, 2, 2};   // unstated; the decoder resolves it
    }
}

// The VP9 codec configuration box a CMVideoFormatDescription needs, in the
// same shape Chromium and FFmpeg build it.
NSData* vp9Configuration(unsigned profile, unsigned bitDepth, unsigned colorSpace, bool fullRange)
{
    const auto color = vp9ColorDescription(colorSpace);
    const std::uint8_t atom[12] = {
        1, 0, 0, 0,                                 // version and flags
        std::uint8_t(profile),
        41,                                         // level 4.1; advisory only
        // bit depth, 4:2:0 colocated chroma, and the stream's range
        std::uint8_t((bitDepth << 4) | (1u << 1) | (fullRange ? 1u : 0u)),
        color.primaries, color.transfer, color.matrix,
        0, 0,                                       // codecInitializationDataSize
    };
    return [NSData dataWithBytes:atom length:sizeof(atom)];
}

std::vector<std::span<const std::uint8_t>> annexB(std::span<const std::uint8_t> bytes)
{
    std::vector<std::span<const std::uint8_t>> units;
    const auto prefix = [bytes](std::size_t p) -> std::size_t {
        if (p + 3 <= bytes.size() && bytes[p] == 0 && bytes[p + 1] == 0) {
            if (bytes[p + 2] == 1) return 3;
            if (p + 4 <= bytes.size() && bytes[p + 2] == 0 && bytes[p + 3] == 1) return 4;
        }
        return 0;
    };
    std::size_t begin = bytes.size();
    for (std::size_t i = 0; i < bytes.size();) {
        const auto length = prefix(i);
        if (!length) { ++i; continue; }
        if (begin < i) units.push_back(bytes.subspan(begin, i - begin));
        begin = i + length;
        i = begin;
    }
    if (begin < bytes.size()) units.push_back(bytes.subspan(begin));
    if (units.empty()) throw std::runtime_error("video packet has no Annex B units");
    return units;
}

VideoFormat jpegFormat(std::span<const std::uint8_t> data)
{
    if (data.size() < 4 || data[0] != 0xff || data[1] != 0xd8)
        throw std::runtime_error("invalid JPEG start marker");
    for (std::size_t p = 2; p + 4 <= data.size();) {
        if (data[p++] != 0xff) throw std::runtime_error("invalid JPEG marker");
        while (p < data.size() && data[p] == 0xff) ++p;
        if (p >= data.size()) break;
        const auto marker = data[p++];
        if (marker == 0xd9 || marker == 0xda) break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (p + 2 > data.size()) break;
        const std::size_t length = (std::size_t(data[p]) << 8) | data[p + 1];
        if (length < 2 || length > data.size() - p) break;
        if (marker == 0xc0) {
            if (length < 17 || data[p + 2] != 8 || data[p + 7] != 3 ||
                data[p + 9] != 0x22 || data[p + 12] != 0x11 || data[p + 15] != 0x11)
                throw std::runtime_error("JPEG decoder requires baseline 8-bit 4:2:0");
            return {std::uint32_t(data[p + 5]) * 256 + data[p + 6],
                    std::uint32_t(data[p + 3]) * 256 + data[p + 4], 8};
        }
        p += length;
    }
    throw std::runtime_error("JPEG baseline frame header is missing");
}

class AppleVideoDecoder final : public VideoDecoder {
public:
    explicit AppleVideoDecoder(VideoCodec codec) : codec_(codec) {}
    ~AppleVideoDecoder() override
    {
        if (session_.get()) {
            VTDecompressionSessionWaitForAsynchronousFrames(session_.get());
            VTDecompressionSessionInvalidate(session_.get());
        }
    }

    void submit(std::span<const std::uint8_t> bytes, std::uint64_t timestamp) override
    {
        const auto packet = prepare(bytes);
        if (packet.empty()) return;
        if (!session_.get()) throw std::runtime_error("video picture precedes its parameter sets");
        decode(packet, timestamp);
    }

    /* Everything `submit` does before the picture reaches the hardware:
     * parse the packet's headers, adopt what they declare, and build or
     * rebuild the decompression session when that changed or when a random
     * access point lets the requested layout take effect for free. A caller
     * that needs the picture size before it can choose a layout runs this
     * alone, then submits the same packet, which repeats the parse without
     * repeating its effect. */
    void inspect(std::span<const std::uint8_t> bytes) override { prepare(bytes); }

    void drain() override
    {
        if (!session_.get()) return;
        checked(VTDecompressionSessionFinishDelayedFrames(session_.get()), "drain video decoder");
        checked(VTDecompressionSessionWaitForAsynchronousFrames(session_.get()), "wait for video drain");
        checkCallback();
    }
    std::vector<DecodedVideoFrame> takeFrames() override
    {
        std::lock_guard lock(mutex_);
        std::vector<DecodedVideoFrame> result;
        result.swap(frames_);
        return result;
    }
    VideoFormat format() const noexcept override { return format_; }

    /* A client negotiates its capture layout only after the decoder has seen
     * the stream's headers, which is the middle of a group of pictures. A
     * decompression session cannot change its output layout, and restarting
     * one there throws away the reference frames the next picture needs, which
     * the hardware reports as kVTVideoDecoderBadDataErr. The request is
     * therefore recorded and applied at the next random access point, where a
     * restart costs nothing; until then the decoder keeps producing its
     * current layout and the caller converts, which every frame states. */
    void requestOutputLayout(VideoPixelLayout layout) override
    {
        if (layout == layout_) return;
        layout_ = layout;
        if (!description_.get()) return;
        if (!session_.get()) configure();
    }

private:
    std::vector<std::uint8_t> prepare(std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty() || bytes.size() > 8 * 1024 * 1024)
            throw std::runtime_error("video packet size is outside the decoder limit");
        std::vector<std::uint8_t> packet;
        bool changed = false;
        // A point where restarting the session loses nothing, because the
        // picture that follows needs no earlier frame.
        bool randomAccess = false;
        if (codec_ == VideoCodec::jpeg) {
            const auto next = jpegFormat(bytes);
            changed = next != format_;
            format_ = next;
            randomAccess = true;   // every JPEG frame stands alone
            packet.assign(bytes.begin(), bytes.end());
        } else if (codec_ == VideoCodec::vp9) {
            // A superframe reaches the decoder whole; its first frame's
            // uncompressed header is the one that can resize the stream.
            if (const auto key = vp9KeyFrame(bytes)) {
                if (key->profile != 0) throw std::runtime_error("unsupported VP9 profile");
                changed = key->format != format_ || key->colorSpace != vp9ColorSpace_ ||
                          key->fullRange != vp9FullRange_;
                format_ = key->format;
                vp9ColorSpace_ = key->colorSpace;
                vp9FullRange_ = key->fullRange;
                randomAccess = true;
            } else if (!description_.get()) {
                throw std::runtime_error("VP9 stream does not start with a key frame");
            }
            packet.assign(bytes.begin(), bytes.end());
        } else {
            bool picture = false;
            for (const auto unit : annexB(bytes)) {
                const unsigned type = codec_ == VideoCodec::h264 ? unit[0] & 31 : (unit[0] >> 1) & 63;
                int parameter = -1;
                if (codec_ == VideoCodec::h264) {
                    if (type == 7) parameter = 0;
                    if (type == 8) parameter = 1;
                    picture |= type >= 1 && type <= 5;
                    randomAccess |= type == 5;          // IDR
                } else {
                    if (type >= 32 && type <= 34) parameter = int(type - 32);
                    picture |= type <= 31;
                    randomAccess |= type >= 16 && type <= 23;   // IRAP
                }
                if (parameter >= 0) {
                    auto& saved = parameters_[std::size_t(parameter)];
                    if (!std::equal(saved.begin(), saved.end(), unit.begin(), unit.end())) {
                        saved.assign(unit.begin(), unit.end());
                        changed = true;
                    }
                    continue;
                }
                const auto size = std::uint32_t(unit.size());
                packet.insert(packet.end(), {std::uint8_t(size >> 24), std::uint8_t(size >> 16),
                                              std::uint8_t(size >> 8), std::uint8_t(size)});
                packet.insert(packet.end(), unit.begin(), unit.end());
            }
            if (!picture) packet.clear();
        }
        if (changed || !description_.get() || (randomAccess && layout_ != sessionLayout_))
            configure();
        return packet;
    }

    void decode(const std::vector<std::uint8_t>& packet, std::uint64_t timestamp)
    {
        CFObject<CMBlockBufferRef> block;
        checked(CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, packet.size(),
            kCFAllocatorDefault, nullptr, 0, packet.size(), 0, block.address()), "allocate video packet");
        checked(CMBlockBufferReplaceDataBytes(packet.data(), block.get(), 0, packet.size()), "copy video packet");
        CFObject<CMSampleBufferRef> sample;
        const std::size_t size = packet.size();
        if (timestamp > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("video timestamp exceeds CMTime range");
        const CMSampleTimingInfo timing {kCMTimeInvalid, CMTimeMake(std::int64_t(timestamp), 1000000000), kCMTimeInvalid};
        checked(CMSampleBufferCreateReady(kCFAllocatorDefault, block.get(), description_.get(),
            1, 1, &timing, 1, &size, sample.address()), "create video sample");
        checked(VTDecompressionSessionDecodeFrame(session_.get(), sample.get(),
            kVTDecodeFrame_EnableTemporalProcessing, nullptr, nullptr), "decode video packet");
        checked(VTDecompressionSessionWaitForAsynchronousFrames(session_.get()), "wait for decoded video");
        checkCallback();
    }

    void configure()
    {
        if (codec_ != VideoCodec::jpeg && codec_ != VideoCodec::vp9) {
            const unsigned count = codec_ == VideoCodec::h264 ? 2 : 3;
            for (unsigned i = 0; i < count; ++i) if (parameters_[i].empty()) return;
        }
        drain();
        if (session_.get()) VTDecompressionSessionInvalidate(session_.get());
        session_.reset();
        if (codec_ == VideoCodec::jpeg) {
            checked(CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_JPEG,
                int(format_.width), int(format_.height), nullptr, description_.address()), "describe JPEG");
        } else if (codec_ == VideoCodec::vp9) {
            NSDictionary* extensions = @{
                (__bridge NSString*)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms:
                    @{@"vpcC": vp9Configuration(0, format_.bitDepth, vp9ColorSpace_, vp9FullRange_)},
                (__bridge NSString*)kCMFormatDescriptionExtension_FullRangeVideo: @(vp9FullRange_),
            };
            checked(CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_VP9,
                int(format_.width), int(format_.height), (__bridge CFDictionaryRef)extensions,
                description_.address()), "describe VP9");
        } else {
            std::array<const std::uint8_t*, 3> data {};
            std::array<std::size_t, 3> sizes {};
            for (unsigned i = 0; i < 3; ++i) { data[i] = parameters_[i].data(); sizes[i] = parameters_[i].size(); }
            if (codec_ == VideoCodec::h264) {
                if (parameters_[0].size() < 4) throw std::runtime_error("truncated H.264 SPS");
                const unsigned profile = parameters_[0][1];
                if (profile != 66 && profile != 77 && profile != 100 && profile != 110)
                    throw std::runtime_error("unsupported H.264 profile");
                if (profile == 110 && !high10Available())
                    throw std::runtime_error("H.264 High10 hardware is unavailable on this host");
                format_.bitDepth = profile == 110 ? 10 : 8;
                checked(CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,
                    2, data.data(), sizes.data(), 4, description_.address()), "describe H.264");
            } else {
                if (parameters_[1].size() < 4) throw std::runtime_error("truncated HEVC SPS");
                const unsigned profile = parameters_[1][3] & 31;
                if (profile != 1 && profile != 2) throw std::runtime_error("unsupported HEVC profile");
                format_.bitDepth = profile == 2 ? 10 : 8;
                checked(CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault,
                    3, data.data(), sizes.data(), 4, nullptr, description_.address()), "describe HEVC");
            }
            const auto dimensions = CMVideoFormatDescriptionGetDimensions(description_.get());
            format_.width = std::uint32_t(dimensions.width);
            format_.height = std::uint32_t(dimensions.height);
        }
        const auto maxWidth = codec_ == VideoCodec::hevc ? 8192u : 4096u;
        const auto maxHeight = codec_ == VideoCodec::hevc ? 4320u : 2304u;
        if (!format_.width || !format_.height || format_.width > maxWidth || format_.height > maxHeight)
            throw std::runtime_error("video dimensions exceed decoder capabilities");
        const OSType planarFormat = format_.bitDepth == 10 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                                                        : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        if (@available(macOS 10.9, iOS 17.0, tvOS 17.0, *)) {
            NSDictionary* specification = @{(__bridge NSString*)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES};
            VTDecompressionOutputCallbackRecord callback {output, this};
            const auto create = [&](OSType pixelFormat) {
                NSDictionary* attributes = @{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(pixelFormat),
                                            (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}};
                return VTDecompressionSessionCreate(kCFAllocatorDefault, description_.get(),
                    (__bridge CFDictionaryRef)specification, (__bridge CFDictionaryRef)attributes,
                    &callback, session_.address());
            };
            /* The hardware converts to packed BGRA as part of decoding, which
             * is why a client that draws through an RGB surface asks for it.
             * A decoder that will not emit it still decodes, in its own
             * layout, and the frame reports which layout it carries. */
            sessionLayout_ = layout_;
            if (layout_ == VideoPixelLayout::packedBgra && create(kCVPixelFormatType_32BGRA) != noErr)
                session_.reset();
            if (!session_.get()) {
                sessionLayout_ = VideoPixelLayout::planarYuv;
                checked(create(planarFormat), "create hardware video decoder");
            }
            CFObject<CFTypeRef> hardware;
            const auto status = VTSessionCopyProperty(session_.get(),
                kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
                kCFAllocatorDefault, hardware.address());
            // Apple's JPEG decoder honors the required-hardware creation flag but
            // does not implement this property, as in the existing Virgl backend.
            if (!((codec_ == VideoCodec::jpeg || codec_ == VideoCodec::vp9) &&
                  status == kVTPropertyNotSupportedErr)) {
                checked(status, "verify hardware video decoder");
                if (hardware.get() != kCFBooleanTrue)
                    throw std::runtime_error("video decoder did not select hardware");
            }
        } else {
            throw std::runtime_error("hardware video decoding requires iOS 17 or newer");
        }
    }

    static void output(void* context, void*, OSStatus status, VTDecodeInfoFlags,
                       CVImageBufferRef image, CMTime timestamp, CMTime) noexcept
    {
        auto& self = *static_cast<AppleVideoDecoder*>(context);
        std::lock_guard lock(self.mutex_);
        try {
            checked(status, "hardware video output");
            if (!image) return;
            if (self.frames_.size() >= 32) throw std::runtime_error("decoded video queue is full");
            const auto pixelFormat = CVPixelBufferGetPixelFormatType(image);
            const bool packed = pixelFormat == kCVPixelFormatType_32BGRA;
            const bool tenBit = pixelFormat == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
            if (!packed && !tenBit && pixelFormat != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
                pixelFormat != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
                throw std::runtime_error("unexpected hardware video pixel format");
            if (CVPixelBufferGetPlaneCount(image) != (packed ? 0u : 2u))
                throw std::runtime_error("invalid hardware video planes");
            DecodedVideoFrame frame;
            frame.layout = packed ? VideoPixelLayout::packedBgra : VideoPixelLayout::planarYuv;
            frame.format = {std::uint32_t(CVPixelBufferGetWidth(image)),
                            std::uint32_t(CVPixelBufferGetHeight(image)), tenBit ? 10u : 8u};
            /* Read back the description the hardware itself applied, so a
             * caller that converts this frame reaches the same picture the
             * hardware would have packed. A stream that states nothing is
             * resolved by VideoToolbox, not guessed again here. */
            frame.color.fullRange = pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
            CFObject<CFTypeRef> matrixValue {
                CVBufferCopyAttachment(image, kCVImageBufferYCbCrMatrixKey, nullptr)};
            if (const auto matrix = matrixValue.get()) {
                if (CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_601_4) ||
                    CFEqual(matrix, kCVImageBufferYCbCrMatrix_SMPTE_240M_1995))
                    frame.color.matrix = VideoColorMatrix::bt601;
                else if (CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_2020))
                    frame.color.matrix = VideoColorMatrix::bt2020;
                else frame.color.matrix = VideoColorMatrix::bt709;
            } else {
                // The same rule VideoToolbox applies to an unstated matrix.
                frame.color.matrix = frame.format.height > 576 ? VideoColorMatrix::bt709
                                                               : VideoColorMatrix::bt601;
            }
            if (!CMTIME_IS_NUMERIC(timestamp) || timestamp.value < 0)
                throw std::runtime_error("invalid decoded video timestamp");
            frame.timestamp = std::uint64_t(CMTimeConvertScale(timestamp, 1000000000, kCMTimeRoundingMethod_Default).value);
            checked(CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly), "map decoded video");
            struct Unlock {
                CVPixelBufferRef image;
                ~Unlock() { CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly); }
            } unlock {image};
            if (packed) {
                const std::size_t rowBytes = std::size_t(frame.format.width) * 4;
                const auto stride = CVPixelBufferGetBytesPerRow(image);
                const auto* source = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(image));
                if (!source || stride < rowBytes) throw std::runtime_error("truncated hardware video surface");
                frame.pixels.resize(rowBytes * frame.format.height);
                for (std::size_t row = 0; row < frame.format.height; ++row)
                    std::memcpy(frame.pixels.data() + row * rowBytes, source + row * stride, rowBytes);
                self.frames_.push_back(std::move(frame));
                return;
            }
            for (unsigned plane = 0; plane < 2; ++plane) {
                const std::size_t width = plane ? ((frame.format.width + 1) / 2) * 2 : frame.format.width;
                const std::size_t height = plane ? (frame.format.height + 1) / 2 : frame.format.height;
                const std::size_t rowBytes = width * (tenBit ? 2 : 1);
                const auto stride = CVPixelBufferGetBytesPerRowOfPlane(image, plane);
                const auto* source = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image, plane));
                if (!source || stride < rowBytes || CVPixelBufferGetHeightOfPlane(image, plane) < height)
                    throw std::runtime_error("truncated hardware video surface");
                auto& destination = plane ? frame.chroma : frame.luma;
                destination.resize(rowBytes * height);
                for (std::size_t row = 0; row < height; ++row)
                    std::memcpy(destination.data() + row * rowBytes, source + row * stride, rowBytes);
            }
            self.frames_.push_back(std::move(frame));
        } catch (const std::exception& error) {
            self.callbackError_ = error.what();
        } catch (...) {
            self.callbackError_ = "hardware video output failed";
        }
    }
    void checkCallback()
    {
        std::lock_guard lock(mutex_);
        if (!callbackError_.empty()) throw std::runtime_error(callbackError_);
    }

    VideoCodec codec_;
    VideoFormat format_;
    VideoPixelLayout layout_ {VideoPixelLayout::planarYuv};
    VideoPixelLayout sessionLayout_ {VideoPixelLayout::planarYuv};
    unsigned vp9ColorSpace_ {};
    bool vp9FullRange_ {};
    std::array<std::vector<std::uint8_t>, 3> parameters_;
    CFObject<CMVideoFormatDescriptionRef> description_;
    CFObject<VTDecompressionSessionRef> session_;
    std::mutex mutex_;
    std::vector<DecodedVideoFrame> frames_;
    std::string callbackError_;
};

} // namespace

std::vector<VideoDecodeCapability> videoDecodeCapabilities()
{
    if (@available(macOS 10.9, iOS 17.0, tvOS 17.0, *)) {
        // Hardware-only session selection is available on these versions.
    } else {
        return {};
    }
    std::vector<VideoDecodeCapability> result;
    if (VTIsHardwareDecodeSupported(kCMVideoCodecType_H264)) {
        for (auto profile : {VideoProfile::h264Baseline, VideoProfile::h264ConstrainedBaseline,
                             VideoProfile::h264Main, VideoProfile::h264High})
            result.push_back({VideoCodec::h264, profile, 4096, 2304, 51, 8});
        if (high10Available())
            result.push_back({VideoCodec::h264, VideoProfile::h264High10, 4096, 2304, 51, 10});
    }
    if (VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC)) {
        result.push_back({VideoCodec::hevc, VideoProfile::hevcMain, 8192, 4320, 186, 8});
        result.push_back({VideoCodec::hevc, VideoProfile::hevcMain10, 8192, 4320, 186, 10});
    }
    if (VTIsHardwareDecodeSupported(kCMVideoCodecType_JPEG))
        result.push_back({VideoCodec::jpeg, VideoProfile::jpegBaseline, 4096, 2304, 0, 8});
    if (vp9Available())
        result.push_back({VideoCodec::vp9, VideoProfile::vp9Profile0, 4096, 2304, 0, 8});
    return result;
}

std::unique_ptr<VideoDecoder> makeVideoDecoder(VideoCodec codec)
{
    const auto capabilities = videoDecodeCapabilities();
    if (std::none_of(capabilities.begin(), capabilities.end(),
                    [codec](const auto& cap) { return cap.codec == codec; }))
        throw std::runtime_error("requested video hardware decoder is unavailable");
    return std::make_unique<AppleVideoDecoder>(codec);
}

} // namespace uml::platform
