// SPDX-License-Identifier: GPL-2.0-or-later
// The VA-API encode entry points against an encoder node written in memory,
// driven the way a browser's VA-API wrapper drives them: what the driver
// advertises, how one picture's buffers become a V4L2 frame, how a finished
// frame reaches the coded buffer, and what happens when a client does something
// the encoder cannot.
#include <linux/videodev2.h>
#include <va/va_backend.h>
#include <va/va_backend_vpp.h>
#include <va/va_enc_h264.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../driver.hpp"
#include "fake_v4l2.hpp"
#include "picture.hpp"
#include "test_support.hpp"

namespace uml::va::test {
namespace {

constexpr unsigned kCodedWidth = 640;
constexpr unsigned kCodedHeight = 368;
constexpr unsigned kVisibleHeight = 360;

struct Stream {
    unsigned bitrate {2000000};
    unsigned percentage {100};
    unsigned framerate {30};
    unsigned minQp {10};
    unsigned maxQp {45};
    unsigned level {31};
    unsigned idrPeriod {2048};
    unsigned ipPeriod {1};
    bool cabac {true};
    bool transform8x8 {false};
    unsigned codedWidth {kCodedWidth};
    unsigned codedHeight {kCodedHeight};
    unsigned visibleHeight {kVisibleHeight};
    unsigned initQp {26};
    int qpDelta {0};
    unsigned hrdBits {3000000};
};

std::vector<std::uint8_t> miscBuffer(VAEncMiscParameterType type, const void* payload, std::size_t size)
{
    std::vector<std::uint8_t> out(sizeof(VAEncMiscParameterBuffer) + size, 0);
    std::memcpy(out.data(), &type, sizeof(type));
    std::memcpy(out.data() + sizeof(VAEncMiscParameterBuffer), payload, size);
    return out;
}

// The rig is a driver over a fake node, reached the way libva reaches it.
struct Va {
    FakeEncoderNode device;
    DescriptorPool pool;
    VADriverContext context {};
    VADriverVTable table {};
    VADriverVTableVPP proc {};
    VAStatus initialised = VA_STATUS_ERROR_OPERATION_FAILED;

    explicit Va(const FakeEncoderNode::Options& options = {}, const char* poolSize = "2,0")
        : device(options), pool(device)
    {
        // The decoder is not under test, and nothing is searched for under
        // /dev: the nodes are the fake's.
        setenv("UML_VA_V4L2_DEVICE", "/dev/uml-no-decoder", 1);
        setenv("UML_VA_V4L2_ENCODER_DEVICE", FakeEncoderNode::kPath, 1);
        setenv("UML_VA_V4L2_SURFACES", "memory", 1);
        setenv("UML_VA_V4L2_POOL", poolSize, 1);
        context.vtable = &table;
        context.vtable_vpp = &proc;
        initialised = initializeDriver(&context, pool);
    }
    ~Va()
    {
        if (initialised == VA_STATUS_SUCCESS) table.vaTerminate(&context);
    }
    Va(const Va&) = delete;
    Va& operator=(const Va&) = delete;

    bool ok() const { return initialised == VA_STATUS_SUCCESS; }

    std::vector<VAProfile> profiles()
    {
        std::vector<VAProfile> out(context.max_profiles);
        int count = 0;
        table.vaQueryConfigProfiles(&context, out.data(), &count);
        out.resize(std::size_t(count));
        return out;
    }
    std::vector<VAEntrypoint> entrypoints(VAProfile profile)
    {
        std::vector<VAEntrypoint> out(context.max_entrypoints);
        int count = 0;
        if (table.vaQueryConfigEntrypoints(&context, profile, out.data(), &count) != VA_STATUS_SUCCESS) return {};
        out.resize(std::size_t(count));
        return out;
    }
    unsigned attribute(VAProfile profile, VAConfigAttribType type, VAEntrypoint entrypoint = VAEntrypointEncSlice)
    {
        VAConfigAttrib attrib {type, 0};
        if (table.vaGetConfigAttributes(&context, profile, entrypoint, &attrib, 1) != VA_STATUS_SUCCESS)
            return VA_ATTRIB_NOT_SUPPORTED;
        return attrib.value;
    }
    VAStatus config(VAProfile profile, unsigned rateControl, VAConfigID& id,
                    VAEntrypoint entrypoint = VAEntrypointEncSlice)
    {
        VAConfigAttrib attributes[2] {{VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420},
                                      {VAConfigAttribRateControl, rateControl}};
        return table.vaCreateConfig(&context, profile, entrypoint, attributes, rateControl ? 2 : 1, &id);
    }
    VASurfaceID surface(unsigned width = kCodedWidth, unsigned height = kCodedHeight)
    {
        VASurfaceID id = VA_INVALID_SURFACE;
        VASurfaceAttrib attribute {};
        attribute.type = VASurfaceAttribPixelFormat;
        attribute.flags = VA_SURFACE_ATTRIB_SETTABLE;
        attribute.value.type = VAGenericValueTypeInteger;
        attribute.value.value.i = VA_FOURCC_NV12;
        table.vaCreateSurfaces2(&context, VA_RT_FORMAT_YUV420, width, height, &id, 1, &attribute, 1);
        return id;
    }
    VAContextID encodeContext(VAConfigID config, unsigned width, unsigned height, VAStatus* status = nullptr)
    {
        VAContextID id = VA_INVALID_ID;
        const VAStatus result = table.vaCreateContext(&context, config, int(width), int(height), VA_PROGRESSIVE,
                                                      nullptr, 0, &id);
        if (status) *status = result;
        return id;
    }
    VABufferID buffer(VAContextID owner, VABufferType type, const void* data, std::size_t size)
    {
        VABufferID id = VA_INVALID_ID;
        table.vaCreateBuffer(&context, owner, type, unsigned(size), 1, const_cast<void*>(data), &id);
        return id;
    }
    // Writes a picture into a surface through a derived image, the way a
    // client uploads one.
    bool upload(VASurfaceID id, const Picture& picture)
    {
        VAImage image {};
        if (table.vaDeriveImage(&context, id, &image) != VA_STATUS_SUCCESS) return false;
        void* mapped = nullptr;
        bool good = table.vaMapBuffer(&context, image.buf, &mapped) == VA_STATUS_SUCCESS;
        if (good) {
            auto* base = static_cast<std::uint8_t*>(mapped);
            for (unsigned row = 0; row < picture.height; ++row)
                std::memcpy(base + image.offsets[0] + row * image.pitches[0], picture.luma.data() + row * picture.lumaPitch,
                            picture.width);
            for (unsigned row = 0; row < (picture.height + 1) / 2; ++row)
                std::memcpy(base + image.offsets[1] + row * image.pitches[1],
                            picture.chroma.data() + row * picture.chromaPitch, picture.width);
            table.vaUnmapBuffer(&context, image.buf);
        }
        table.vaDestroyImage(&context, image.image_id);
        return good;
    }

    // The three miscellaneous buffers a client sends with every picture.
    void addMiscellaneous(VAContextID owner, const Stream& stream, std::vector<VABufferID>& ids)
    {
        VAEncMiscParameterRateControl rate {};
        rate.bits_per_second = stream.bitrate;
        rate.target_percentage = stream.percentage;
        rate.window_size = 1500;
        rate.initial_qp = stream.initQp;
        rate.min_qp = stream.minQp;
        rate.max_qp = stream.maxQp;
        rate.rc_flags.bits.disable_frame_skip = 1;
        const auto rateBuffer = miscBuffer(VAEncMiscParameterTypeRateControl, &rate, sizeof(rate));
        ids.push_back(buffer(owner, VAEncMiscParameterBufferType, rateBuffer.data(), rateBuffer.size()));
        VAEncMiscParameterFrameRate framerate {};
        framerate.framerate = stream.framerate;
        const auto rateFrames = miscBuffer(VAEncMiscParameterTypeFrameRate, &framerate, sizeof(framerate));
        ids.push_back(buffer(owner, VAEncMiscParameterBufferType, rateFrames.data(), rateFrames.size()));
        VAEncMiscParameterHRD hrd {};
        hrd.buffer_size = stream.hrdBits;
        hrd.initial_buffer_fullness = stream.hrdBits / 2;
        const auto hrdBuffer = miscBuffer(VAEncMiscParameterTypeHRD, &hrd, sizeof(hrd));
        ids.push_back(buffer(owner, VAEncMiscParameterBufferType, hrdBuffer.data(), hrdBuffer.size()));
    }

    // One HEVC picture, with the parameters an HEVC client sends.
    VAStatus encodeHevc(VAContextID owner, VASurfaceID input, VABufferID coded, const Stream& stream, bool idr,
                        unsigned bitDepth = 8)
    {
        std::vector<VABufferID> ids;
        VAEncSequenceParameterBufferHEVC seq {};
        seq.general_profile_idc = 1;
        seq.general_level_idc = std::uint8_t(stream.level);
        seq.intra_period = 30;
        seq.intra_idr_period = 0;
        seq.ip_period = stream.ipPeriod;
        seq.bits_per_second = stream.bitrate;
        seq.pic_width_in_luma_samples = std::uint16_t(stream.codedWidth);
        seq.pic_height_in_luma_samples = std::uint16_t(stream.codedHeight);
        seq.seq_fields.bits.chroma_format_idc = 1;
        seq.seq_fields.bits.bit_depth_luma_minus8 = bitDepth - 8;
        seq.seq_fields.bits.bit_depth_chroma_minus8 = bitDepth - 8;
        seq.vui_parameters_present_flag = 1;
        seq.vui_fields.bits.vui_timing_info_present_flag = 1;
        seq.vui_num_units_in_tick = 1;
        seq.vui_time_scale = stream.framerate;
        ids.push_back(buffer(owner, VAEncSequenceParameterBufferType, &seq, sizeof(seq)));
        VAEncPictureParameterBufferHEVC pic {};
        pic.decoded_curr_pic.picture_id = input;
        pic.coded_buf = coded;
        pic.pic_init_qp = std::uint8_t(stream.initQp);
        pic.pic_fields.bits.idr_pic_flag = idr;
        ids.push_back(buffer(owner, VAEncPictureParameterBufferType, &pic, sizeof(pic)));
        VAEncSliceParameterBufferHEVC slice {};
        slice.slice_type = idr ? 2 : 1;
        slice.slice_qp_delta = std::int8_t(stream.qpDelta);
        ids.push_back(buffer(owner, VAEncSliceParameterBufferType, &slice, sizeof(slice)));
        addMiscellaneous(owner, stream, ids);
        VAStatus status = table.vaBeginPicture(&context, owner, input);
        if (status == VA_STATUS_SUCCESS) status = table.vaRenderPicture(&context, owner, ids.data(), int(ids.size()));
        if (status == VA_STATUS_SUCCESS) status = table.vaEndPicture(&context, owner);
        for (const VABufferID id : ids) table.vaDestroyBuffer(&context, id);
        return status;
    }

    // One picture, submitted the way Chromium's H.264 delegate submits it:
    // sequence, picture and slice parameters and the three miscellaneous
    // buffers, every time.
    VAStatus encode(VAContextID owner, VASurfaceID input, VABufferID coded, const Stream& stream, bool idr,
                    bool miscellaneous = true, bool sequence = true, bool packed = false)
    {
        std::vector<VABufferID> ids;
        VAEncSequenceParameterBufferH264 seq {};
        seq.level_idc = std::uint8_t(stream.level);
        seq.intra_idr_period = stream.idrPeriod;
        seq.ip_period = stream.ipPeriod;
        seq.bits_per_second = stream.bitrate;
        seq.max_num_ref_frames = 1;
        seq.picture_width_in_mbs = std::uint16_t(stream.codedWidth / 16);
        seq.picture_height_in_mbs = std::uint16_t(stream.codedHeight / 16);
        seq.seq_fields.bits.chroma_format_idc = 1;
        seq.seq_fields.bits.frame_mbs_only_flag = 1;
        if (stream.visibleHeight < stream.codedHeight) {
            seq.frame_cropping_flag = 1;
            seq.frame_crop_bottom_offset = (stream.codedHeight - stream.visibleHeight) / 2;
        }
        seq.vui_parameters_present_flag = 1;
        seq.vui_fields.bits.timing_info_present_flag = 1;
        seq.num_units_in_tick = 1;
        seq.time_scale = 2 * stream.framerate;
        if (sequence) ids.push_back(buffer(owner, VAEncSequenceParameterBufferType, &seq, sizeof(seq)));

        VAEncPictureParameterBufferH264 pic {};
        pic.CurrPic.picture_id = input;
        pic.coded_buf = coded;
        pic.pic_init_qp = std::uint8_t(stream.initQp);
        pic.pic_fields.bits.idr_pic_flag = idr;
        pic.pic_fields.bits.entropy_coding_mode_flag = stream.cabac;
        pic.pic_fields.bits.transform_8x8_mode_flag = stream.transform8x8;
        ids.push_back(buffer(owner, VAEncPictureParameterBufferType, &pic, sizeof(pic)));

        VAEncSliceParameterBufferH264 slice {};
        slice.slice_type = idr ? 2 : 0;
        slice.slice_qp_delta = std::int8_t(stream.qpDelta);
        ids.push_back(buffer(owner, VAEncSliceParameterBufferType, &slice, sizeof(slice)));

        if (miscellaneous) addMiscellaneous(owner, stream, ids);
        if (packed) {
            VAEncPackedHeaderParameterBuffer header {};
            header.type = VAEncPackedHeaderSequence;
            header.bit_length = 32;
            ids.push_back(buffer(owner, VAEncPackedHeaderParameterBufferType, &header, sizeof(header)));
            const std::uint8_t bytes[4] {0, 0, 0, 1};
            ids.push_back(buffer(owner, VAEncPackedHeaderDataBufferType, bytes, sizeof(bytes)));
        }

        VAStatus status = table.vaBeginPicture(&context, owner, input);
        if (status == VA_STATUS_SUCCESS) status = table.vaRenderPicture(&context, owner, ids.data(), int(ids.size()));
        if (status == VA_STATUS_SUCCESS) status = table.vaEndPicture(&context, owner);
        for (const VABufferID id : ids) table.vaDestroyBuffer(&context, id);
        return status;
    }

    struct Coded {
        VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED;
        std::vector<std::uint8_t> data;
        unsigned flags {};
        bool single {};
    };
    // What a client sees: sync the input, map the coded buffer, read its
    // segment list.
    Coded read(VASurfaceID input, VABufferID coded, bool sync = true)
    {
        Coded out;
        if (sync) {
            out.status = table.vaSyncSurface(&context, input);
            if (out.status != VA_STATUS_SUCCESS) return out;
        }
        void* mapped = nullptr;
        out.status = table.vaMapBuffer(&context, coded, &mapped);
        if (out.status != VA_STATUS_SUCCESS) return out;
        out.single = true;
        for (auto* segment = static_cast<VACodedBufferSegment*>(mapped); segment;
             segment = static_cast<VACodedBufferSegment*>(segment->next)) {
            out.flags |= segment->status;
            const auto* bytes = static_cast<const std::uint8_t*>(segment->buf);
            out.data.insert(out.data.end(), bytes, bytes + segment->size);
            if (segment->next) out.single = false;
        }
        table.vaUnmapBuffer(&context, coded);
        return out;
    }
};

struct SliceInfo {
    bool found {};
    bool key {};
    std::uint64_t checksum {};
    unsigned parameterSets {};
};

// The slice NAL unit of the fake's stream, and the checksum it carries. An
// HEVC NAL unit header is two bytes where an H.264 one is a byte.
SliceInfo parseSlice(const std::vector<std::uint8_t>& data, bool hevc = false)
{
    SliceInfo info;
    const std::size_t header = hevc ? 2 : 1;
    for (std::size_t index = 0; index + 4 + header + 8 <= data.size(); ++index) {
        if (data[index] || data[index + 1] || data[index + 2] || data[index + 3] != 1) continue;
        const std::uint8_t type = data[index + 4];
        const bool set = hevc ? (type == 0x40 || type == 0x42 || type == 0x44) : (type == 0x67 || type == 0x68);
        if (set) { ++info.parameterSets; continue; }
        const bool idr = hevc ? type == 0x26 : type == 0x65;
        const bool delta = hevc ? type == 0x02 : type == 0x41;
        if (!idr && !delta) continue;
        info.found = true;
        info.key = idr;
        for (unsigned byte = 0; byte < 8; ++byte)
            info.checksum = (info.checksum << 8) | std::uint8_t(data[index + 4 + header + byte] ^ 0x5a);
        return info;
    }
    return info;
}

bool has(const std::vector<VAProfile>& list, VAProfile profile)
{
    return std::find(list.begin(), list.end(), profile) != list.end();
}

void testAdvertising()
{
    Va va;
    check(va.ok(), "a driver over an encoder node initialises with no decoder");
    if (!va.ok()) return;
    const auto profiles = va.profiles();
    check(has(profiles, VAProfileH264ConstrainedBaseline) && has(profiles, VAProfileH264Main) &&
              has(profiles, VAProfileH264High) && has(profiles, VAProfileHEVCMain),
          "the profiles the node's menus offer are listed");
    check(!has(profiles, VAProfileHEVCMain10) && !has(profiles, VAProfileVP9Profile0) && !has(profiles, VAProfileJPEGBaseline),
          "a profile nothing backs is not listed");
    check(profiles.size() <= std::size_t(va.context.max_profiles), "the profile list fits the room libva gave it");
    const auto entrypoints = va.entrypoints(VAProfileH264Main);
    check(entrypoints.size() == 2 && entrypoints[0] == VAEntrypointEncSlice && entrypoints[1] == VAEntrypointEncSliceLP,
          "both encode entry points are offered, and no decode one");
    check(va.entrypoints(VAProfileVP8Version0_3).empty(), "an unlisted profile has no entry points");

    const unsigned rate = va.attribute(VAProfileH264Main, VAConfigAttribRateControl);
    check((rate & (VA_RC_CBR | VA_RC_VBR | VA_RC_CQP | VA_RC_ICQ)) == (VA_RC_CBR | VA_RC_VBR | VA_RC_CQP | VA_RC_ICQ),
          "the rate control modes are the node's");
    check(va.attribute(VAProfileH264Main, VAConfigAttribEncPackedHeaders) == VA_ENC_PACKED_HEADER_NONE,
          "the encoder writes the headers");
    check(va.attribute(VAProfileH264Main, VAConfigAttribEncMaxRefFrames) == 1, "one reference picture");
    check(va.attribute(VAProfileH264Main, VAConfigAttribRTFormat) == VA_RT_FORMAT_YUV420, "4:2:0 only");
    check(va.attribute(VAProfileH264Main, VAConfigAttribMaxPictureWidth) == 4096 &&
              va.attribute(VAProfileHEVCMain, VAConfigAttribMaxPictureWidth) == 8192,
          "the size limits are each codec's own");
    check(va.attribute(VAProfileH264Main, VAConfigAttribEncMaxSlices) == 1, "one slice a picture");
    check(va.attribute(VAProfileH264Main, VAConfigAttribEncROI) == VA_ATTRIB_NOT_SUPPORTED,
          "what the encoder cannot do is not claimed");
    check(va.attribute(VAProfileH264Main, VAConfigAttribRateControl, VAEntrypointVLD) == VA_ATTRIB_NOT_SUPPORTED,
          "a profile has no attributes on an entry point it lacks");

    VAConfigID id = VA_INVALID_ID;
    check(va.config(VAProfileH264Main, VA_RC_CBR, id) == VA_STATUS_SUCCESS, "a constant bit rate configuration is made");
    VAProfile profile {};
    VAEntrypoint entrypoint {};
    std::vector<VAConfigAttrib> attributes(va.context.max_attributes);
    int count = 0;
    check(va.table.vaQueryConfigAttributes(&va.context, id, &profile, &entrypoint, attributes.data(), &count) ==
                  VA_STATUS_SUCCESS &&
              profile == VAProfileH264Main && entrypoint == VAEntrypointEncSlice && count == 3 &&
              attributes[1].type == VAConfigAttribRateControl && attributes[1].value == VA_RC_CBR,
          "a configuration reports the mode it was made for");

    VAConfigID other = VA_INVALID_ID;
    check(va.config(VAProfileH264Main, VA_RC_VCM, other) != VA_STATUS_SUCCESS, "a mode the node lacks is refused");
    check(va.config(VAProfileH264Main, VA_RC_CBR, other, VAEntrypointVLD) != VA_STATUS_SUCCESS,
          "decoding H.264 is not offered");
    check(va.config(VAProfileH264Main, VA_RC_CBR, other, VAEntrypointEncPicture) != VA_STATUS_SUCCESS,
          "picture encoding is not offered");
    check(va.config(VAProfileHEVCMain10, VA_RC_CBR, other) != VA_STATUS_SUCCESS, "Main10 is not offered");
    check(va.config(VAProfileH264Main, 0, other) == VA_STATUS_SUCCESS, "a configuration with no mode named picks one");

    unsigned surfaceCount = 0;
    check(va.table.vaQuerySurfaceAttributes(&va.context, id, nullptr, &surfaceCount) == VA_STATUS_SUCCESS && surfaceCount,
          "surface attributes are counted");
    std::vector<VASurfaceAttrib> surfaceAttributes(surfaceCount);
    va.table.vaQuerySurfaceAttributes(&va.context, id, surfaceAttributes.data(), &surfaceCount);
    int maxWidth = 0, minWidth = 0, nv12 = 0;
    for (const auto& attribute : surfaceAttributes) {
        if (attribute.type == VASurfaceAttribMaxWidth) maxWidth = attribute.value.value.i;
        if (attribute.type == VASurfaceAttribMinWidth) minWidth = attribute.value.value.i;
        if (attribute.type == VASurfaceAttribPixelFormat && attribute.value.value.i == int(VA_FOURCC_NV12)) ++nv12;
    }
    check(maxWidth == 4096 && minWidth == 16 && nv12 == 1, "an encode configuration takes NV12 surfaces of the node's sizes");
}

void testRateControlModesFollowTheNode()
{
    Va onlyConstant({.bitrateModeMask = 0x2});
    check(onlyConstant.ok(), "a node with one mode initialises");
    if (onlyConstant.ok()) {
        const unsigned rate = onlyConstant.attribute(VAProfileH264Main, VAConfigAttribRateControl);
        check((rate & VA_RC_CBR) && !(rate & VA_RC_VBR) && !(rate & VA_RC_ICQ), "only the verified modes are offered");
        VAConfigID id = VA_INVALID_ID;
        check(onlyConstant.config(VAProfileH264Main, VA_RC_VBR, id) != VA_STATUS_SUCCESS, "another mode is refused");
    }
    Va noHigh({.h264ProfileMask = (1u << 2)});
    if (noHigh.ok()) {
        const auto profiles = noHigh.profiles();
        check(has(profiles, VAProfileH264Main) && !has(profiles, VAProfileH264High) &&
                  !has(profiles, VAProfileH264ConstrainedBaseline),
              "only the verified profiles are offered");
    }
    Va noHevc({.hevc = false});
    if (noHevc.ok()) check(!has(noHevc.profiles(), VAProfileHEVCMain), "a node without HEVC offers none");
    Va bare({.noControls = true});
    check(!bare.ok(), "a node that publishes nothing it can be configured with is not a driver");
}

void testEncodeAStream()
{
    Va va;
    if (!va.ok()) { check(false, "the rig initialises"); return; }
    VAConfigID config = VA_INVALID_ID;
    va.config(VAProfileH264Main, VA_RC_CBR, config);
    VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED;
    const VAContextID context = va.encodeContext(config, kCodedWidth, kCodedHeight, &status);
    check(status == VA_STATUS_SUCCESS, "an encode context is made");
    check(va.device.streamOns == 0, "the node is not started before the first picture");

    const unsigned count = 6;
    std::vector<VASurfaceID> surfaces;
    std::vector<VABufferID> coded;
    std::vector<Picture> pictures;
    for (unsigned index = 0; index < count; ++index) {
        surfaces.push_back(va.surface());
        coded.push_back(va.buffer(context, VAEncCodedBufferType, nullptr, 1u << 20));
        pictures.emplace_back(kCodedWidth, kCodedHeight, index + 1);
        check(va.upload(surfaces.back(), pictures.back()), "a picture is uploaded to a surface");
    }
    Stream stream;
    for (unsigned index = 0; index < count; ++index) {
        // The third picture is an IDR the client asked for; the fifth changes
        // the bit rate and the frame rate.
        if (index == 4) { stream.bitrate = 3000000; stream.framerate = 15; }
        check(va.encode(context, surfaces[index], coded[index], stream, index == 0 || index == 2) == VA_STATUS_SUCCESS,
              "a picture is accepted");
    }
    check(va.device.outputWidth == kCodedWidth && va.device.outputHeight == kVisibleHeight,
          "the cropping the sequence parameters name sets the size of the stream");
    check(va.device.codedFormat == V4L2_PIX_FMT_H264, "H.264 is the coded format");
    check(va.device.control(V4L2_CID_MPEG_VIDEO_H264_PROFILE) == V4L2_MPEG_VIDEO_H264_PROFILE_MAIN &&
              va.device.control(V4L2_CID_MPEG_VIDEO_H264_LEVEL) == V4L2_MPEG_VIDEO_H264_LEVEL_3_1,
          "the profile and level reach the node");
    check(va.device.control(V4L2_CID_MPEG_VIDEO_BITRATE_MODE) == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR &&
              va.device.control(V4L2_CID_MPEG_VIDEO_GOP_SIZE) == 2048 &&
              va.device.control(V4L2_CID_MPEG_VIDEO_B_FRAMES) == 0 &&
              va.device.control(V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE) == V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC &&
              va.device.control(V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM) == 0,
          "the sequence and picture parameters reach the node");
    check(va.device.control(V4L2_CID_MPEG_VIDEO_H264_MIN_QP) == 10 && va.device.control(V4L2_CID_MPEG_VIDEO_H264_MAX_QP) == 45,
          "the quantiser bounds reach the node");
    check(va.device.control(V4L2_CID_MPEG_VIDEO_HEADER_MODE) == V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME &&
              va.device.control(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR) == 1,
          "every key frame carries its parameter sets");
    check(va.device.control(V4L2_CID_MPEG_VIDEO_BITRATE) == 3000000, "a bit rate change reaches a running stream");
    check(va.device.frameRateNumerator == 15 && va.device.frameRateDenominator == 1,
          "so does a frame rate change");
    check(va.device.control(V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE) == 367, "the buffer size is given in kilobytes");

    // Read them back out of order, as a client with several in flight may.
    bool pixels = true, keys = true, framing = true;
    for (unsigned index = count; index-- > 0;) {
        const auto read = va.read(surfaces[index], coded[index]);
        framing = framing && read.status == VA_STATUS_SUCCESS && !read.data.empty() && read.single && read.flags == 0;
        const SliceInfo slice = parseSlice(read.data);
        pixels = pixels && slice.found && slice.checksum == pictures[index].checksum(kCodedWidth, kVisibleHeight);
        keys = keys && slice.key == (index == 0 || index == 2) && (slice.key ? slice.parameterSets == 2 : true);
    }
    check(framing, "every frame reaches its coded buffer whole, as one segment");
    check(pixels, "each coded buffer holds the frame of the picture that named it, cropped to the stream's size");
    check(keys, "the pictures that asked for an IDR are key frames with their parameter sets, and no others");
    check(va.device.frames.size() == count && va.device.frames[0].timestamp != va.device.frames[1].timestamp,
          "every picture reached the node once, with a timestamp of its own");

    // Mapping a coded buffer waits for its frame, with no sync before it.
    VASurfaceID again = va.surface();
    VABufferID againCoded = va.buffer(context, VAEncCodedBufferType, nullptr, 1u << 20);
    Picture fresh(kCodedWidth, kCodedHeight, 99);
    va.upload(again, fresh);
    check(va.encode(context, again, againCoded, stream, false) == VA_STATUS_SUCCESS, "another picture is accepted");
    const auto unsynced = va.read(again, againCoded, false);
    check(unsynced.status == VA_STATUS_SUCCESS && parseSlice(unsynced.data).checksum == fresh.checksum(kCodedWidth, kVisibleHeight),
          "mapping the coded buffer waits for the frame");

    // A packed header is accepted and ignored.
    va.upload(again, fresh);
    check(va.encode(context, again, againCoded, stream, false, true, true, true) == VA_STATUS_SUCCESS,
          "packed headers a client sends anyway are ignored");
    check(va.read(again, againCoded).status == VA_STATUS_SUCCESS, "and the picture still encodes");

    // The sequence parameters are not repeated on every picture.
    va.upload(again, fresh);
    check(va.encode(context, again, againCoded, stream, false, false, false) == VA_STATUS_SUCCESS &&
              va.read(again, againCoded).status == VA_STATUS_SUCCESS,
          "a picture needs no sequence parameters of its own once the stream has them");

    va.table.vaDestroyContext(&va.context, context);
    check(va.device.streamOffs == 2 && va.pool.idle(Role::encoder) == 2 && va.device.closes == 0,
          "destroying the context stops the stream and gives the descriptor back");
}

void testRateControlModes()
{
    {
        Va va;
        VAConfigID config = VA_INVALID_ID;
        va.config(VAProfileH264High, VA_RC_VBR, config);
        const VAContextID context = va.encodeContext(config, kCodedWidth, kCodedHeight);
        VASurfaceID surface = va.surface();
        VABufferID coded = va.buffer(context, VAEncCodedBufferType, nullptr, 1u << 20);
        Picture picture(kCodedWidth, kCodedHeight, 1);
        va.upload(surface, picture);
        Stream stream;
        stream.bitrate = 4000000;
        stream.percentage = 50;
        stream.transform8x8 = true;
        check(va.encode(context, surface, coded, stream, true) == VA_STATUS_SUCCESS &&
                  va.read(surface, coded).status == VA_STATUS_SUCCESS,
              "a variable bit rate picture encodes");
        check(va.device.control(V4L2_CID_MPEG_VIDEO_BITRATE_MODE) == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR &&
                  va.device.control(V4L2_CID_MPEG_VIDEO_BITRATE) == 2000000 &&
                  va.device.control(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK) == 4000000,
              "the client's maximum is the peak and the target is its percentage of it");
        check(va.device.control(V4L2_CID_MPEG_VIDEO_H264_PROFILE) == V4L2_MPEG_VIDEO_H264_PROFILE_HIGH &&
                  va.device.control(V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM) == 1,
              "the High profile and the 8x8 transform reach the node");
        stream.bitrate = 6000000;
        va.upload(surface, picture);
        check(va.encode(context, surface, coded, stream, false) == VA_STATUS_SUCCESS &&
                  va.read(surface, coded).status == VA_STATUS_SUCCESS,
              "a second picture encodes");
        check(va.device.control(V4L2_CID_MPEG_VIDEO_BITRATE) == 3000000 &&
                  va.device.control(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK) == 6000000,
              "both rates follow a change");
    }
    {
        Va va;
        VAConfigID config = VA_INVALID_ID;
        va.config(VAProfileH264Main, VA_RC_CQP, config);
        const VAContextID context = va.encodeContext(config, kCodedWidth, kCodedHeight);
        VASurfaceID surface = va.surface();
        VABufferID coded = va.buffer(context, VAEncCodedBufferType, nullptr, 1u << 20);
        Picture picture(kCodedWidth, kCodedHeight, 1);
        va.upload(surface, picture);
        Stream stream;
        stream.initQp = 30;
        stream.qpDelta = -4;
        check(va.encode(context, surface, coded, stream, true, false) == VA_STATUS_SUCCESS &&
                  va.read(surface, coded).status == VA_STATUS_SUCCESS,
              "a constant quantiser picture encodes");
        check(va.device.control(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE) == 0 &&
                  va.device.control(V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP) == 26 &&
                  va.device.control(V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP) == 26,
              "rate control is switched off and the quantiser is the picture's plus the slice's delta");
    }
    {
        Va va;
        VAConfigID config = VA_INVALID_ID;
        va.config(VAProfileH264Main, VA_RC_ICQ, config);
        const VAContextID context = va.encodeContext(config, kCodedWidth, kCodedHeight);
        VASurfaceID surface = va.surface();
        VABufferID coded = va.buffer(context, VAEncCodedBufferType, nullptr, 1u << 20);
        Picture picture(kCodedWidth, kCodedHeight, 1);
        va.upload(surface, picture);
        Stream stream;
        check(va.encode(context, surface, coded, stream, true) == VA_STATUS_SUCCESS &&
                  va.read(surface, coded).status == VA_STATUS_SUCCESS,
              "a constant quality picture encodes");
        check(va.device.control(V4L2_CID_MPEG_VIDEO_BITRATE_MODE) == V4L2_MPEG_VIDEO_BITRATE_MODE_CQ &&
                  va.device.control(V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY) == 51,
              "the neutral quality factor is the middle of the node's range");
    }
}

void testCodedBufferLimits()
{
    Va va;
    VAConfigID config = VA_INVALID_ID;
    va.config(VAProfileH264Main, VA_RC_CBR, config);
    const VAContextID context = va.encodeContext(config, kCodedWidth, kCodedHeight);
    VASurfaceID surface = va.surface();
    VABufferID small = va.buffer(context, VAEncCodedBufferType, nullptr, 64);
    Picture picture(kCodedWidth, kCodedHeight, 3);
    va.upload(surface, picture);
    Stream stream;
    check(va.encode(context, surface, small, stream, true) == VA_STATUS_SUCCESS, "a picture names a small coded buffer");
    const auto read = va.read(surface, small);
    check(read.status == VA_STATUS_SUCCESS && read.data.size() == 64 && (read.flags & VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK),
          "a frame larger than its buffer is cut to it and says so");

    // A buffer that is not a coded buffer cannot receive a frame.
    const std::uint8_t bytes[16] {};
    VABufferID wrong = va.buffer(context, VAProtectedSliceDataBufferType, bytes, sizeof(bytes));
    va.upload(surface, picture);
    check(va.encode(context, surface, wrong, stream, false) == VA_STATUS_ERROR_INVALID_BUFFER,
          "a picture whose coded buffer is no coded buffer is refused");
    check(va.encode(context, surface, VA_INVALID_ID, stream, false) == VA_STATUS_ERROR_INVALID_BUFFER,
          "so is one that names none");
}

void testRefusals()
{
    {
        Va va;
        VAConfigID config = VA_INVALID_ID;
        va.config(VAProfileH264Main, VA_RC_CBR, config);
        const VAContextID context = va.encodeContext(config, kCodedWidth, kCodedHeight);
        VASurfaceID surface = va.surface();
        VABufferID coded = va.buffer(context, VAEncCodedBufferType, nullptr, 1u << 20);
        Picture picture(kCodedWidth, kCodedHeight, 3);
        va.upload(surface, picture);
        Stream stream;
        stream.ipPeriod = 3;
        check(va.encode(context, surface, coded, stream, true) == VA_STATUS_ERROR_INVALID_PARAMETER,
              "B-frames are refused");
        check(va.table.vaSyncSurface(&va.context, surface) != VA_STATUS_SUCCESS,
              "a surface whose picture was refused does not sync as encoded");
        stream.ipPeriod = 1;
        check(va.encode(context, surface, coded, stream, true) != VA_STATUS_SUCCESS,
              "an encoder that failed stays failed");
    }
    {
        Va va;
        VAConfigID config = VA_INVALID_ID;
        va.config(VAProfileH264Main, VA_RC_CBR, config);
        const VAContextID context = va.encodeContext(config, kCodedWidth, kCodedHeight);
        VASurfaceID surface = va.surface();
        VABufferID coded = va.buffer(context, VAEncCodedBufferType, nullptr, 1u << 20);
        Picture picture(kCodedWidth, kCodedHeight, 3);
        va.upload(surface, picture);
        Stream stream;
        check(va.encode(context, surface, coded, stream, true, true, false) == VA_STATUS_ERROR_INVALID_PARAMETER,
              "a first picture with no sequence parameters is refused");
    }
    {
        Va va;
        VAConfigID config = VA_INVALID_ID;
        va.config(VAProfileH264Main, VA_RC_CBR, config);
        const VAContextID context = va.encodeContext(config, kCodedWidth, kCodedHeight);
        VASurfaceID surface = va.surface();
        VABufferID coded = va.buffer(context, VAEncCodedBufferType, nullptr, 1u << 20);
        Picture picture(kCodedWidth, kCodedHeight, 3);
        va.upload(surface, picture);
        Stream stream;
        check(va.encode(context, surface, coded, stream, true) == VA_STATUS_SUCCESS &&
                  va.read(surface, coded).status == VA_STATUS_SUCCESS,
              "a stream starts");
        stream.codedWidth = 320;
        stream.codedHeight = 192;
        stream.visibleHeight = 180;
        check(va.encode(context, surface, coded, stream, true) == VA_STATUS_ERROR_INVALID_PARAMETER,
              "a stream cannot change its size");
    }
    {
        // A context larger than the node takes.
        Va va;
        VAConfigID config = VA_INVALID_ID;
        va.config(VAProfileH264Main, VA_RC_CBR, config);
        VAStatus status = VA_STATUS_SUCCESS;
        va.encodeContext(config, 8192, 8192, &status);
        check(status == VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED, "a context beyond the node's limits is refused");
        va.encodeContext(config, 0, 0, &status);
        check(status != VA_STATUS_SUCCESS, "a context of no size is refused");
    }
    {
        // The node fails a frame.
        Va va({.errorFrame = 1});
        VAConfigID config = VA_INVALID_ID;
        va.config(VAProfileH264Main, VA_RC_CBR, config);
        const VAContextID context = va.encodeContext(config, kCodedWidth, kCodedHeight);
        Stream stream;
        std::vector<VASurfaceID> surfaces;
        std::vector<VABufferID> coded;
        for (unsigned index = 0; index < 3; ++index) {
            surfaces.push_back(va.surface());
            coded.push_back(va.buffer(context, VAEncCodedBufferType, nullptr, 1u << 20));
            Picture picture(kCodedWidth, kCodedHeight, index);
            va.upload(surfaces.back(), picture);
            va.encode(context, surfaces.back(), coded.back(), stream, index == 0);
        }
        check(va.read(surfaces[0], coded[0]).status == VA_STATUS_SUCCESS, "the frame before the failure is fine");
        check(va.table.vaSyncSurface(&va.context, surfaces[1]) == VA_STATUS_ERROR_ENCODING_ERROR,
              "a frame the node failed is an encoding error on its own surface");
        check(va.table.vaSyncSurface(&va.context, surfaces[2]) == VA_STATUS_SUCCESS,
              "and only on that one");
    }
}

void testHevc()
{
    Va va;
    if (!va.ok()) { check(false, "the rig initialises"); return; }
    check(va.entrypoints(VAProfileHEVCMain).size() == 2, "HEVC Main has both encode entry points");
    check(va.attribute(VAProfileHEVCMain, VAConfigAttribEncPackedHeaders) == VA_ENC_PACKED_HEADER_NONE,
          "the encoder writes the HEVC headers as well");
    VAConfigID config = VA_INVALID_ID;
    check(va.config(VAProfileHEVCMain, VA_RC_CBR, config) == VA_STATUS_SUCCESS, "an HEVC configuration is made");
    // The sequence parameters of HEVC carry no cropping, so the stream is the
    // size of the picture.
    const VAContextID context = va.encodeContext(config, kCodedWidth, kCodedHeight);
    Stream stream;
    stream.level = 93;
    stream.minQp = 12;
    stream.maxQp = 48;
    std::vector<VASurfaceID> surfaces;
    std::vector<VABufferID> coded;
    std::vector<Picture> pictures;
    for (unsigned index = 0; index < 3; ++index) {
        surfaces.push_back(va.surface());
        coded.push_back(va.buffer(context, VAEncCodedBufferType, nullptr, 1u << 20));
        pictures.emplace_back(kCodedWidth, kCodedHeight, index + 11);
        va.upload(surfaces.back(), pictures.back());
        check(va.encodeHevc(context, surfaces.back(), coded.back(), stream, index == 0) == VA_STATUS_SUCCESS,
              "an HEVC picture is accepted");
    }
    check(va.device.codedFormat == V4L2_PIX_FMT_HEVC && va.device.outputWidth == kCodedWidth &&
              va.device.outputHeight == kCodedHeight,
          "the stream is HEVC at the size of the picture");
    check(va.device.control(V4L2_CID_MPEG_VIDEO_HEVC_PROFILE) == V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN &&
              va.device.control(V4L2_CID_MPEG_VIDEO_HEVC_LEVEL) == V4L2_MPEG_VIDEO_HEVC_LEVEL_3_1 &&
              va.device.control(V4L2_CID_MPEG_VIDEO_HEVC_TIER) == V4L2_MPEG_VIDEO_HEVC_TIER_MAIN,
          "the profile, level and tier reach the node");
    check(va.device.control(V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP) == 12 && va.device.control(V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP) == 48,
          "the quantiser bounds reach the node's HEVC controls");
    check(va.device.control(V4L2_CID_MPEG_VIDEO_GOP_SIZE) == 30 && va.device.control(V4L2_CID_MPEG_VIDEO_B_FRAMES) == 0 &&
              va.device.control(V4L2_CID_MPEG_VIDEO_BITRATE) == 2000000,
          "the shorter key frame interval and the bit rate reach the node");
    check(va.device.frameRateNumerator == 30 && va.device.frameRateDenominator == 1,
          "the frame rate counts frames in the HEVC tick");
    bool pixels = true, keys = true;
    for (unsigned index = 0; index < 3; ++index) {
        const auto read = va.read(surfaces[index], coded[index]);
        const SliceInfo slice = parseSlice(read.data, true);
        pixels = pixels && read.status == VA_STATUS_SUCCESS && slice.found &&
                 slice.checksum == pictures[index].checksum(kCodedWidth, kCodedHeight);
        keys = keys && slice.key == (index == 0) && (slice.key ? slice.parameterSets == 3 : true);
    }
    check(pixels, "every HEVC frame holds the picture that named it");
    check(keys, "the first HEVC frame is a key frame with its parameter sets, and the others are not");

    // Main is 8-bit; a client that asks for more is refused rather than served wrongly.
    const VAContextID second = va.encodeContext(config, kCodedWidth, kCodedHeight);
    VASurfaceID surface = va.surface();
    VABufferID buffer = va.buffer(second, VAEncCodedBufferType, nullptr, 1u << 20);
    va.upload(surface, pictures[0]);
    check(va.encodeHevc(second, surface, buffer, stream, true, 10) != VA_STATUS_SUCCESS, "ten-bit parameters are refused");
}

void testDescriptors()
{
    // Two descriptors in the pool, and the node refuses to be opened again, as
    // it would inside a sandbox.
    Va va({}, "2,0");
    if (!va.ok()) { check(false, "the rig initialises"); return; }
    check(va.device.opens == 2, "the pool is opened when the driver initialises");
    va.device.denyOpen = true;

    VAConfigID config = VA_INVALID_ID;
    va.config(VAProfileH264Main, VA_RC_CBR, config);
    VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED;
    const VAContextID first = va.encodeContext(config, kCodedWidth, kCodedHeight, &status);
    check(status == VA_STATUS_SUCCESS, "the first context is served from the pool");
    const VAContextID second = va.encodeContext(config, kCodedWidth, kCodedHeight, &status);
    check(status == VA_STATUS_SUCCESS, "so is the second");
    va.encodeContext(config, kCodedWidth, kCodedHeight, &status);
    check(status == VA_STATUS_ERROR_MAX_NUM_EXCEEDED, "a third has nothing to run on and says so");

    // Run a stream on the first, destroy it, and use its descriptor again.
    VASurfaceID surface = va.surface();
    VABufferID coded = va.buffer(first, VAEncCodedBufferType, nullptr, 1u << 20);
    Picture picture(kCodedWidth, kCodedHeight, 7);
    va.upload(surface, picture);
    Stream stream;
    check(va.encode(first, surface, coded, stream, true) == VA_STATUS_SUCCESS &&
              va.read(surface, coded).status == VA_STATUS_SUCCESS,
          "a stream runs on a pooled descriptor");
    va.table.vaDestroyContext(&va.context, first);
    const VAContextID third = va.encodeContext(config, kCodedWidth, kCodedHeight, &status);
    check(status == VA_STATUS_SUCCESS, "a destroyed context's descriptor serves the next");
    check(va.device.opens == 2, "no descriptor was opened after initialisation");

    VASurfaceID next = va.surface();
    VABufferID nextCoded = va.buffer(third, VAEncCodedBufferType, nullptr, 1u << 20);
    Stream other;
    other.bitrate = 5000000;
    va.upload(next, picture);
    check(va.encode(third, next, nextCoded, other, true) == VA_STATUS_SUCCESS &&
              va.read(next, nextCoded).status == VA_STATUS_SUCCESS,
          "a new stream runs on the reused descriptor");
    check(va.device.control(V4L2_CID_MPEG_VIDEO_BITRATE) == 5000000, "with its own rate");
    va.table.vaDestroyContext(&va.context, second);
    va.table.vaDestroyContext(&va.context, third);

    // An unsandboxed process may take more than the pool holds.
    Va open({}, "1,0");
    VAConfigID openConfig = VA_INVALID_ID;
    open.config(VAProfileH264Main, VA_RC_CBR, openConfig);
    const VAContextID a = open.encodeContext(openConfig, kCodedWidth, kCodedHeight, &status);
    const VAContextID b = open.encodeContext(openConfig, kCodedWidth, kCodedHeight, &status);
    check(status == VA_STATUS_SUCCESS && open.device.opens == 2, "a process that can open the node is not held to the pool");
    open.table.vaDestroyContext(&open.context, a);
    open.table.vaDestroyContext(&open.context, b);
    check(open.pool.held(Role::encoder) == 1, "and gives the extra descriptor back when it is done");
}

}

void runDriverEncodeTests()
{
    testAdvertising();
    testRateControlModesFollowTheNode();
    testEncodeAStream();
    testRateControlModes();
    testCodedBufferLimits();
    testRefusals();
    testHevc();
    testDescriptors();
}

}
