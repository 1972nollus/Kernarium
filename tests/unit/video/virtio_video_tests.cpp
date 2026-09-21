// SPDX-License-Identifier: GPL-2.0-or-later
#include "virtio_video_device.hpp"
#include "shared_memory_regions.hpp"
#include "video_capabilities.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

using namespace uml::platform;
using namespace uml::video;
using namespace uml::virtio;
namespace {
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
template<class T> void append(std::vector<std::uint8_t>& data, const T& value)
{
    const auto size = data.size();
    data.resize(size + sizeof(value));
    std::memcpy(data.data() + size, &value, sizeof(value));
}
template<class T> T read(std::span<const std::uint8_t> data)
{
    require(data.size() >= sizeof(T), "short reply");
    T result;
    std::memcpy(&result, data.data(), sizeof(result));
    return result;
}
/* `packs` distinguishes a decoder that produces packed frames itself, as the
 * Apple hardware does, from one that only ever produces its planar output and
 * leaves the device to convert. */
class Decoder final : public VideoDecoder {
public:
    explicit Decoder(bool ten, bool packs = false, bool defers = false)
        : ten_(ten), packs_(packs), defers_(defers) {}
    void submit(std::span<const std::uint8_t> packet, std::uint64_t timestamp) override
    {
        require(packet.size() == 5 && packet[0] == 1 && packet[4] == 5, "scatter input changed");
        format_ = {64, 64, ten_ ? 10u : 8u};
        DecodedVideoFrame frame;
        frame.format = format_;
        frame.timestamp = timestamp;
        // A deferring decoder keeps its layout until a later picture, which is
        // what a real one does in the middle of a group of pictures.
        if (defers_ && layout_ != adopted_) { ++deferred_; if (deferred_ > 1) adopted_ = layout_; }
        else adopted_ = layout_;
        if (packs_ && adopted_ == VideoPixelLayout::packedBgra) {
            frame.layout = VideoPixelLayout::packedBgra;
            frame.format.bitDepth = 8;
            frame.pixels.resize(64 * 64 * 4);
            for (std::size_t i = 0; i < frame.pixels.size(); i += 4) {
                frame.pixels[i] = 0x10; frame.pixels[i + 1] = 0x20;
                frame.pixels[i + 2] = 0x30; frame.pixels[i + 3] = 0xff;
            }
        } else {
            frame.color = {VideoColorMatrix::bt601, false};
            frame.luma.resize(4096 * (ten_ ? 2 : 1), 0x39);
            frame.chroma.resize(2048 * (ten_ ? 2 : 1));
            for (std::size_t i = 0; i < frame.chroma.size(); ++i) frame.chroma[i] = i % 2 ? 0x91 : 0x27;
        }
        frames_.push_back(std::move(frame));
    }
    void drain() override {}
    std::vector<DecodedVideoFrame> takeFrames() override { return std::exchange(frames_, {}); }
    VideoFormat format() const noexcept override { return format_; }
    void requestOutputLayout(VideoPixelLayout layout) override { layout_ = layout; }
private:
    bool ten_;
    bool packs_ {};
    bool defers_ {};
    unsigned deferred_ {};
    VideoPixelLayout layout_ {VideoPixelLayout::planarYuv};
    VideoPixelLayout adopted_ {VideoPixelLayout::planarYuv};
    VideoFormat format_;
    std::vector<DecodedVideoFrame> frames_;
};
void exercise(unsigned format, std::size_t page)
{
    const bool ten = format == VIRTIO_VIDEO_FORMAT_P010;
    std::vector<VideoDecodeCapability> caps {{VideoCodec::h264,
        ten ? VideoProfile::h264High10 : VideoProfile::h264High, 4096, 2304, 51, ten ? 10u : 8u}};
    VirtioVideoDevice device(caps, [ten](auto) { return std::make_unique<Decoder>(ten); });
    std::vector<std::uint8_t> memory(8 * page, 0xcc);
    device.setGuestMapper([&](std::uint64_t offset, std::size_t size) {
        if (offset > memory.size() || size > memory.size() - offset) return std::span<std::uint8_t>{};
        return std::span(memory).subspan(offset, size);
    });
    std::uint64_t next = 1;
    auto command = [&](const auto& value) {
        std::vector<std::uint8_t> request;
        append(request, value);
        return device.processQueue(0, next++, request, 16384);
    };
    auto ok = [&](const auto& value) {
        auto result = command(value);
        require(read<virtio_video_cmd_hdr>(result.response).type == VIRTIO_VIDEO_RESP_OK_NODATA, "command failed");
    };
    virtio_video_stream_create create {};
    create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 7};
    create.coded_format = VIRTIO_VIDEO_FORMAT_H264;
    ok(create);
    require(device.processQueue(1, 1000, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer, "event not deferred");
    virtio_video_set_params set {};
    set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, 7};
    set.params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    set.params.format = format;
    set.params.frame_width = set.params.frame_height = 64;
    ok(set);
    auto attach = [&](unsigned queue, unsigned id, const std::vector<std::vector<virtio_video_resource_sg_entry>>& planes) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach{VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, 7, queue, id});
        for (const auto& plane : planes) {
            append(request, std::uint32_t(plane.size())); append(request, std::uint32_t(0));
            for (const auto& segment : plane) append(request, segment);
        }
        return device.processQueue(0, next++, request, 16384);
    };
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    memory[page - 2] = 1; memory[page - 1] = 2;
    memory[2 * page] = 3; memory[2 * page + 1] = 4; memory[2 * page + 2] = 5;
    require(read<virtio_video_cmd_hdr>(attach(input, 1, {{{page - 2, 2, {}}, {2 * page, 3, {}}}}).response).type == VIRTIO_VIDEO_RESP_OK_NODATA, "input attach failed");
    require(read<virtio_video_cmd_hdr>(attach(input, 9, {{{memory.size(), 1, {}}}}).response).type == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "invalid guest address accepted");
    std::vector<std::vector<virtio_video_resource_sg_entry>> planes;
    const unsigned sizes[] = {ten ? 12288u : 4096u, format == VIRTIO_VIDEO_FORMAT_YUV420 ? 1024u : 2048u, 1024u};
    const unsigned count = ten ? 1 : format == VIRTIO_VIDEO_FORMAT_YUV420 ? 3 : 2;
    for (unsigned i = 0; i < count; ++i) {
        const auto base = (3 + i) * page;
        planes.push_back({{base, 97, {}}, {base + 97, sizes[i] - 97, {}}});
    }
    require(read<virtio_video_cmd_hdr>(attach(output, 2, planes).response).type == VIRTIO_VIDEO_RESP_OK_NODATA, "output attach failed");
    virtio_video_resource_queue capture {};
    capture.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
    capture.stream_id = 7; capture.queue_type = output; capture.resource_id = 2;
    const auto captureRequest = next;
    require(command(capture).action == VhostUserQueueAction::defer, "capture did not wait for input");
    require(read<virtio_video_cmd_hdr>(command(capture).response).type == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "duplicate queue accepted");
    virtio_video_resource_queue packet {};
    packet.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
    packet.stream_id = 7; packet.queue_type = input; packet.resource_id = 1;
    packet.data_sizes[0] = 5; packet.timestamp = 7654321;
    require(read<virtio_video_resource_queue_resp>(command(packet).response).flags == 0, "decode input failed");
    VhostUserQueueCompletion completion;
    require(device.takeQueueCompletion(completion) && completion.requestId == 1000, "missing source event");
    require(read<virtio_video_event>(completion.response).event_type == VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED, "incorrect source event");
    require(device.takeQueueCompletion(completion) && completion.requestId == captureRequest, "missing capture completion");
    const auto result = read<virtio_video_resource_queue_resp>(completion.response);
    require(result.flags == 0 && result.timestamp == packet.timestamp && result.data_sizes[0] == sizes[0], "incorrect frame completion");
    require(memory[3 * page] == 0x39 && memory[3 * page + 4095] == 0x39, "luma changed");
    const auto chroma = ten ? 3 * page + 8192 : 4 * page;
    require(memory[chroma] == 0x27, "chroma changed");
    require(memory[chroma + 1] == (format == VIRTIO_VIDEO_FORMAT_YUV420 ? 0x27 : 0x91), "chroma layout changed");
    if (count == 3) require(memory[5 * page] == 0x91, "third plane changed");
    ok(virtio_video_stream_drain{{VIRTIO_VIDEO_CMD_STREAM_DRAIN, 7}});
    require(command(capture).action == VhostUserQueueAction::defer, "EOS capture failed");
    require(device.takeQueueCompletion(completion), "missing EOS");
    require(read<virtio_video_resource_queue_resp>(completion.response).flags == (VIRTIO_VIDEO_DEQUEUE_FLAG_EOS), "EOS flag missing");
    require(command(capture).action == VhostUserQueueAction::defer, "post-EOS capture failed");
    ok(virtio_video_queue_clear{{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 7}, output, {}});
    require(device.takeQueueCompletion(completion), "clear lost outstanding capture");
    require(read<virtio_video_resource_queue_resp>(completion.response).flags == (VIRTIO_VIDEO_DEQUEUE_FLAG_ERR), "clear did not cancel capture");
    ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 7}});
    device.reset();
    require(!device.takeQueueCompletion(completion), "reset retained completions");
    for (unsigned i = 0; i < 256; ++i)
        require(device.processQueue(1, 2000 + i, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer, "event descriptor was completed without an event");
    require(device.processQueue(1, 3000, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::retry, "full event queue did not apply backpressure");
    device.reset();

}

void codecLevelControls()
{
    for (const auto codec : {VideoCodec::h264, VideoCodec::hevc, VideoCodec::jpeg}) {
        VirtioVideoDevice device({{codec, codec == VideoCodec::hevc ? VideoProfile::hevcMain :
            codec == VideoCodec::jpeg ? VideoProfile::jpegBaseline : VideoProfile::h264High,
            4096, 2304, 51, 8}}, [](auto) { return std::make_unique<Decoder>(false); });
        virtio_video_stream_create create {};
        create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 1};
        create.coded_format = wireCodec(codec);
        std::vector<std::uint8_t> request;
        append(request, create);
        require(read<virtio_video_cmd_hdr>(device.processQueue(0, 1, request, 128).response).type ==
            VIRTIO_VIDEO_RESP_OK_NODATA, "control stream creation failed");
        request.clear();
        append(request, virtio_video_get_control{{VIRTIO_VIDEO_CMD_GET_CONTROL, 1}, VIRTIO_VIDEO_CONTROL_LEVEL, {}});
        const auto result = device.processQueue(0, 2, request, 128);
        const auto header = read<virtio_video_cmd_hdr>(result.response);
        if (codec == VideoCodec::jpeg) {
            require(header.type == VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL, "JPEG exposed a coded level");
        } else {
            require(header.type == VIRTIO_VIDEO_RESP_OK_GET_CONTROL, "level control failed");
            const auto value = read<virtio_video_control_val_level>(std::span(result.response).subspan(sizeof(header)));
            require(value.level == (codec == VideoCodec::h264 ? VIRTIO_VIDEO_LEVEL_H264_5_1 :
                VIRTIO_VIDEO_LEVEL_HEVC_6_2), "level belongs to the wrong codec");
        }
    }
}

/* The activity log is how an observer outside the guest tells a real hardware
 * decode from a silent software fallback, so it has to stay silent unless it
 * is asked for and to name what each stream actually decoded when it is.
 */
void activityLog()
{
    const auto path = std::filesystem::temp_directory_path() /
        ("uml-video-activity-" + std::to_string(::getpid()) + ".log");
    std::filesystem::remove(path);
    ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
    exercise(VIRTIO_VIDEO_FORMAT_NV12, 4096);
    require(!std::filesystem::exists(path), "the video activity log is not off by default");

    ::setenv("UML_VIDEO_ACTIVITY_LOG", path.c_str(), 1);
    exercise(VIRTIO_VIDEO_FORMAT_NV12, 4096);
    ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
    std::ostringstream buffer;
    buffer << std::ifstream(path).rdbuf();
    const auto text = buffer.str();
    std::filesystem::remove(path);
    require(text.find("advertise codec=h264 profile=h264-high max=4096x2304 depth=8") != std::string::npos,
        "the video activity log omits the advertised profile");
    require(text.find("stream=7 open codec=h264 profile=h264-high") != std::string::npos,
        "the video activity log omits the stream a guest opened");
    require(text.find("stream=7 close=destroy packets=1 bytes=5 frames=1 output=64x64") != std::string::npos,
        "the video activity log omits what the stream decoded");
}

/* A capture buffer may be a host-visible blob: the guest names an offset in
 * the shared-memory region its GPU backend published instead of a
 * guest-physical address, and the decoder writes the frame into the host
 * memory behind that region. A range withdrawn while a buffer is queued must
 * fail that buffer rather than reach memory the guest no longer has.
 */
void sharedMemoryCapture()
{
    constexpr std::uint8_t regionId = 1;
    constexpr std::uint64_t regionOffset = 64 * 1024;
    constexpr std::size_t regionLength = 64 * 1024;
    const auto path = std::filesystem::temp_directory_path() /
        ("uml-video-shm-" + std::to_string(::getpid()));
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    require(descriptor >= 0, "shared-memory fixture could not be created");
    std::filesystem::remove(path);
    require(::ftruncate(descriptor, regionLength) == 0, "shared-memory fixture could not be sized");

    auto regions = std::make_shared<SharedMemoryRegions>();
    require(regions->publish(regionId, regionOffset, regionLength, descriptor, 0),
        "publishing a shared-memory range failed");
    require(!regions->publish(regionId, regionOffset + 4096, 4096, descriptor, 0),
        "an overlapping shared-memory range was published");
    require(regions->contains(regionId, regionOffset, 16) &&
        regions->contains(regionId, regionOffset + regionLength - 1, 1),
        "a published range does not contain its own memory");
    require(!regions->contains(regionId, regionOffset - 1, 1) &&
        !regions->contains(regionId, regionOffset + regionLength - 1, 2) &&
        !regions->contains(regionId + 1, regionOffset, 1),
        "an unpublished range resolved");

    std::vector<VideoDecodeCapability> caps {
        {VideoCodec::h264, VideoProfile::h264High, 4096, 2304, 51, 8}};
    VirtioVideoDevice device(caps, [](auto) { return std::make_unique<Decoder>(false); });
    require((device.features() & (1 << VIRTIO_VIDEO_F_RESOURCE_SHM_REGION)) == 0,
        "a device with no shared-memory index offered the region feature");
    device.setSharedMemoryRegions(regions);
    require((device.features() & (1 << VIRTIO_VIDEO_F_RESOURCE_SHM_REGION)) != 0,
        "a device with a shared-memory index withheld the region feature");
    std::vector<std::uint8_t> memory(8 * 4096, 0xcc);
    device.setGuestMapper([&](std::uint64_t offset, std::size_t size) {
        if (offset > memory.size() || size > memory.size() - offset) return std::span<std::uint8_t>{};
        return std::span(memory).subspan(offset, size);
    });
    std::uint64_t next = 1;
    auto command = [&](const auto& value) {
        std::vector<std::uint8_t> request;
        append(request, value);
        return device.processQueue(0, next++, request, 16384);
    };
    auto ok = [&](const auto& value) {
        auto result = command(value);
        require(read<virtio_video_cmd_hdr>(result.response).type == VIRTIO_VIDEO_RESP_OK_NODATA,
            "command failed");
    };
    virtio_video_stream_create create {};
    create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 3};
    create.coded_format = VIRTIO_VIDEO_FORMAT_H264;
    ok(create);
    virtio_video_set_params set {};
    set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, 3};
    set.params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    set.params.format = VIRTIO_VIDEO_FORMAT_NV12;
    set.params.frame_width = set.params.frame_height = 64;
    ok(set);
    auto attach = [&](unsigned queue, unsigned id,
                      const std::vector<std::vector<virtio_video_resource_sg_entry>>& planes) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach{VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, 3, queue, id});
        for (const auto& plane : planes) {
            append(request, std::uint32_t(plane.size())); append(request, std::uint32_t(0));
            for (const auto& segment : plane) append(request, segment);
        }
        return device.processQueue(0, next++, request, 16384);
    };
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    memory[0] = 1; memory[1] = 2; memory[2] = 3; memory[3] = 4; memory[4] = 5;
    require(read<virtio_video_cmd_hdr>(attach(input, 1, {{{0, 5, {}}}}).response) .type ==
        VIRTIO_VIDEO_RESP_OK_NODATA, "input attach failed");

    const std::uint32_t shared = VIRTIO_VIDEO_RESOURCE_SG_FLAG_SHM_REGION |
        (std::uint32_t(regionId) << VIRTIO_VIDEO_RESOURCE_SG_SHM_ID_SHIFT);
    /* NV12 at 64x64: a 4096-byte luma plane followed by a 2048-byte chroma
     * plane, both inside the published range. */
    const std::vector<std::vector<virtio_video_resource_sg_entry>> planes {
        {{regionOffset, 4096, shared}},
        {{regionOffset + 4096, 2048, shared}},
    };
    require(read<virtio_video_cmd_hdr>(attach(output, 2, planes).response).type ==
        VIRTIO_VIDEO_RESP_OK_NODATA, "shared-memory attach failed");
    require(read<virtio_video_cmd_hdr>(attach(output, 3, {
            {{regionOffset + regionLength, 4096, shared}},
            {{regionOffset, 2048, shared}},
        }).response).type == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
        "a range outside the published region was accepted");
    require(read<virtio_video_cmd_hdr>(attach(output, 4, {
            {{regionOffset, 4096, shared | 0x8000u}},
            {{regionOffset + 4096, 2048, shared}},
        }).response).type == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
        "an unknown scatter entry flag was accepted");
    /* Rebinding a buffer index to other memory reattaches its identifier. */
    require(read<virtio_video_cmd_hdr>(attach(output, 2, planes).response).type ==
        VIRTIO_VIDEO_RESP_OK_NODATA, "reattaching an idle resource failed");

    virtio_video_resource_queue capture {};
    capture.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
    capture.stream_id = 3; capture.queue_type = output; capture.resource_id = 2;
    const auto captureRequest = next;
    require(command(capture).action == VhostUserQueueAction::defer, "capture did not wait for input");
    require(read<virtio_video_cmd_hdr>(attach(output, 2, planes).response).type ==
        VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a queued resource was reattached");
    virtio_video_resource_queue packet {};
    packet.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
    packet.stream_id = 3; packet.queue_type = input; packet.resource_id = 1;
    packet.data_sizes[0] = 5; packet.timestamp = 12345;
    require(read<virtio_video_resource_queue_resp>(command(packet).response).flags == 0,
        "decode input failed");
    VhostUserQueueCompletion completion;
    bool delivered = false;
    while (device.takeQueueCompletion(completion)) {
        if (completion.requestId != captureRequest) continue;
        const auto result = read<virtio_video_resource_queue_resp>(completion.response);
        require(result.flags == 0 && result.timestamp == packet.timestamp,
            "shared-memory capture did not complete");
        delivered = true;
    }
    require(delivered, "shared-memory capture produced no completion");
    bool written = false;
    require(regions->resolve(regionId, regionOffset, 4096 + 2048,
        [&](std::span<std::uint8_t> frame) {
            written = frame[0] == 0x39 && frame[4095] == 0x39 && frame[4096] == 0x27 &&
                frame[4097] == 0x91;
        }), "the published range could not be resolved");
    require(written, "the decoded frame did not reach the shared-memory range");

    /* A blob revoked while its buffer is queued fails that buffer. */
    const auto revokedRequest = next;
    require(command(capture).action == VhostUserQueueAction::defer, "second capture failed");
    regions->revoke(regionId, regionOffset, regionLength);
    require(!regions->contains(regionId, regionOffset, 1), "revoking a range left it published");
    require(read<virtio_video_resource_queue_resp>(command(packet).response).flags == 0,
        "decode input after revocation failed");
    bool failed = false;
    while (device.takeQueueCompletion(completion)) {
        if (completion.requestId != revokedRequest) continue;
        failed = read<virtio_video_resource_queue_resp>(completion.response).flags ==
            VIRTIO_VIDEO_DEQUEUE_FLAG_ERR;
    }
    require(failed, "a capture buffer whose region was revoked did not fail");
    ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 3}});
    device.reset();
    regions->revokeAll();
    ::close(descriptor);
}

/* A client that only draws through a 32-bit RGB surface negotiates packed
 * capture. The frames the decoder produces packed arrive unchanged; a frame it
 * produced planar before the negotiation is converted by the device, so the
 * client never sees the format it did not ask for. */
void packedCapture(std::size_t page, bool decoderPacks)
{
    std::vector<VideoDecodeCapability> caps {{VideoCodec::h264, VideoProfile::h264High, 4096, 2304, 51, 8}};
    VirtioVideoDevice device(caps, [decoderPacks](auto) { return std::make_unique<Decoder>(false, decoderPacks); });
    std::vector<std::uint8_t> memory(8 * page, 0xcc);
    device.setGuestMapper([&](std::uint64_t offset, std::size_t size) {
        if (offset > memory.size() || size > memory.size() - offset) return std::span<std::uint8_t>{};
        return std::span(memory).subspan(offset, size);
    });
    std::uint64_t next = 1;
    auto command = [&](const auto& value) {
        std::vector<std::uint8_t> request;
        append(request, value);
        return device.processQueue(0, next++, request, 16384);
    };
    auto ok = [&](const auto& value) {
        auto result = command(value);
        require(read<virtio_video_cmd_hdr>(result.response).type == VIRTIO_VIDEO_RESP_OK_NODATA, "command failed");
    };
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;

    // The capture queue advertises the packed format for every stream.
    std::vector<std::uint8_t> query;
    append(query, virtio_video_query_capability{{VIRTIO_VIDEO_CMD_QUERY_CAPABILITY, 0}, output, {}});
    const auto capability = device.processQueue(0, next++, query, 65536).response;
    auto offset = sizeof(virtio_video_query_capability_resp);
    bool advertised = false;
    for (unsigned i = 0; i < read<virtio_video_query_capability_resp>(capability).num_descs; ++i) {
        const auto desc = read<virtio_video_format_desc>(
            std::span(capability).subspan(offset));
        if (desc.format == VIRTIO_VIDEO_FORMAT_BGRA8888) {
            require(desc.planes_layout == VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER, "packed capture is not one buffer");
            require(desc.mask != 0, "packed capture reaches no codec");
            advertised = true;
        }
        offset += sizeof(virtio_video_format_desc);
        for (unsigned frame = 0; frame < desc.num_frames; ++frame) {
            const auto described = read<virtio_video_format_frame>(std::span(capability).subspan(offset));
            offset += sizeof(virtio_video_format_frame) +
                described.num_rates * sizeof(virtio_video_format_range);
        }
    }
    require(advertised, "packed capture format is not advertised");

    virtio_video_stream_create create {};
    create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 7};
    create.coded_format = VIRTIO_VIDEO_FORMAT_H264;
    ok(create);
    virtio_video_set_params set {};
    set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, 7};
    set.params.queue_type = output;
    set.params.format = VIRTIO_VIDEO_FORMAT_BGRA8888;
    set.params.frame_width = set.params.frame_height = 64;
    ok(set);
    virtio_video_get_params get {};
    get.hdr = {VIRTIO_VIDEO_CMD_GET_PARAMS, 7};
    get.queue_type = output;
    const auto negotiated = read<virtio_video_get_params_resp>(command(get).response).params;
    require(negotiated.format == VIRTIO_VIDEO_FORMAT_BGRA8888, "packed capture was substituted");
    require(negotiated.num_planes == 1, "packed capture is not a single plane");
    require(negotiated.plane_formats[0].stride == 64 * 4, "packed capture stride differs");
    require(negotiated.plane_formats[0].plane_size == 64 * 64 * 4, "packed capture plane size differs");

    auto attach = [&](unsigned queue, unsigned id, const std::vector<std::vector<virtio_video_resource_sg_entry>>& planes) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach{VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, 7, queue, id});
        for (const auto& plane : planes) {
            append(request, std::uint32_t(plane.size())); append(request, std::uint32_t(0));
            for (const auto& segment : plane) append(request, segment);
        }
        return device.processQueue(0, next++, request, 16384);
    };
    memory[page - 2] = 1; memory[page - 1] = 2;
    memory[2 * page] = 3; memory[2 * page + 1] = 4; memory[2 * page + 2] = 5;
    require(read<virtio_video_cmd_hdr>(attach(input, 1, {{{page - 2, 2, {}}, {2 * page, 3, {}}}}).response).type == VIRTIO_VIDEO_RESP_OK_NODATA, "input attach failed");
    const std::uint64_t base = 3 * page;
    require(read<virtio_video_cmd_hdr>(attach(output, 2, {{{base, 64 * 64 * 4, {}}}}).response).type == VIRTIO_VIDEO_RESP_OK_NODATA, "packed output attach failed");
    virtio_video_resource_queue capture {};
    capture.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
    capture.stream_id = 7; capture.queue_type = output; capture.resource_id = 2;
    const auto captureRequest = next;
    require(command(capture).action == VhostUserQueueAction::defer, "capture did not wait for input");
    virtio_video_resource_queue packet {};
    packet.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
    packet.stream_id = 7; packet.queue_type = input; packet.resource_id = 1;
    packet.data_sizes[0] = 5; packet.timestamp = 7654321;
    require(read<virtio_video_resource_queue_resp>(command(packet).response).flags == 0, "decode input failed");
    VhostUserQueueCompletion completion;
    require(device.takeQueueCompletion(completion) && completion.requestId == captureRequest, "missing capture completion");
    const auto result = read<virtio_video_resource_queue_resp>(completion.response);
    require(result.flags == 0 && result.timestamp == packet.timestamp, "packed frame was not delivered");
    require(result.data_sizes[0] == 64 * 64 * 4, "packed frame size differs");
    /* BT.601 limited range for a frame this small, matching what the platform
     * decoder infers from an unspecified color attachment. */
    const std::uint8_t expected[4] = {decoderPacks ? std::uint8_t(0x10) : std::uint8_t(0),
                                      decoderPacks ? std::uint8_t(0x20) : std::uint8_t(69),
                                      decoderPacks ? std::uint8_t(0x30) : std::uint8_t(75), 0xff};
    for (std::size_t pixel : {std::size_t(0), std::size_t(64 * 64 - 1)})
        for (unsigned channel = 0; channel < 4; ++channel)
            require(memory[base + pixel * 4 + channel] == expected[channel], "packed pixels differ");
    ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 7}});
    device.reset();
}

/* The stateful-decoder order a V4L2 client actually uses: the capture format
 * is chosen only after the decoder has reported the stream's size, so the
 * first frames are already decoded when the request arrives. Every frame must
 * still reach the client in the negotiated format, whether the decoder adopted
 * the layout or the device converted for it. */
void lateNegotiation(std::size_t page)
{
    std::vector<VideoDecodeCapability> caps {{VideoCodec::h264, VideoProfile::h264High, 4096, 2304, 51, 8}};
    VirtioVideoDevice device(caps, [](auto) { return std::make_unique<Decoder>(false, true, true); });
    std::vector<std::uint8_t> memory(8 * page, 0xcc);
    device.setGuestMapper([&](std::uint64_t offset, std::size_t size) {
        if (offset > memory.size() || size > memory.size() - offset) return std::span<std::uint8_t>{};
        return std::span(memory).subspan(offset, size);
    });
    std::uint64_t next = 1;
    auto command = [&](const auto& value) {
        std::vector<std::uint8_t> request;
        append(request, value);
        return device.processQueue(0, next++, request, 16384);
    };
    auto ok = [&](const auto& value) {
        require(read<virtio_video_cmd_hdr>(command(value).response).type == VIRTIO_VIDEO_RESP_OK_NODATA,
                "command failed");
    };
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    virtio_video_stream_create create {};
    create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 5};
    create.coded_format = VIRTIO_VIDEO_FORMAT_H264;
    ok(create);
    require(device.processQueue(1, 900, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
            "event not deferred");
    auto attach = [&](unsigned queue, unsigned id, const std::vector<std::vector<virtio_video_resource_sg_entry>>& planes) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach{VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, 5, queue, id});
        for (const auto& plane : planes) {
            append(request, std::uint32_t(plane.size())); append(request, std::uint32_t(0));
            for (const auto& segment : plane) append(request, segment);
        }
        return device.processQueue(0, next++, request, 16384);
    };
    memory[page - 2] = 1; memory[page - 1] = 2;
    memory[2 * page] = 3; memory[2 * page + 1] = 4; memory[2 * page + 2] = 5;
    require(read<virtio_video_cmd_hdr>(attach(input, 1, {{{page - 2, 2, {}}, {2 * page, 3, {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "input attach failed");

    // Decode before any capture format is chosen; this raises the source change.
    virtio_video_resource_queue packet {};
    packet.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
    packet.stream_id = 5; packet.queue_type = input; packet.resource_id = 1;
    packet.data_sizes[0] = 5; packet.timestamp = 11;
    require(read<virtio_video_resource_queue_resp>(command(packet).response).flags == 0, "decode input failed");
    VhostUserQueueCompletion completion;
    require(device.takeQueueCompletion(completion) && completion.requestId == 900, "missing source event");
    require(read<virtio_video_event>(completion.response).event_type == VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED,
            "incorrect source event");

    // Only now does the client choose packed capture.
    virtio_video_set_params set {};
    set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, 5};
    set.params.queue_type = output;
    set.params.format = VIRTIO_VIDEO_FORMAT_BGRA8888;
    set.params.frame_width = set.params.frame_height = 64;
    ok(set);
    const std::uint64_t base = 3 * page;
    require(read<virtio_video_cmd_hdr>(attach(output, 2, {{{base, 64 * 64 * 4, {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "packed output attach failed");

    // Three pictures: the one decoded before the request, one the decoder is
    // still deferring, and one it has adopted the layout for.
    const std::uint8_t converted[4] = {0, 69, 75, 0xff};
    const std::uint8_t adopted[4] = {0x10, 0x20, 0x30, 0xff};
    for (unsigned round = 0; round < 3; ++round) {
        std::memset(memory.data() + base, 0xcc, 64 * 64 * 4);
        virtio_video_resource_queue capture {};
        capture.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
        capture.stream_id = 5; capture.queue_type = output; capture.resource_id = 2;
        const auto captureRequest = next;
        require(command(capture).action == VhostUserQueueAction::defer, "capture was not deferred");
        if (round) {
            packet.timestamp = 11 + round;
            require(read<virtio_video_resource_queue_resp>(command(packet).response).flags == 0,
                    "later decode failed");
        }
        require(device.takeQueueCompletion(completion) && completion.requestId == captureRequest,
                "missing capture completion");
        const auto result = read<virtio_video_resource_queue_resp>(completion.response);
        require(result.flags == 0, "late negotiation failed a capture buffer");
        require(result.data_sizes[0] == 64 * 64 * 4, "packed frame size differs");
        const auto* expected = round == 2 ? adopted : converted;
        for (std::size_t pixel : {std::size_t(0), std::size_t(64 * 64 - 1)})
            for (unsigned channel = 0; channel < 4; ++channel)
                require(memory[base + pixel * 4 + channel] == expected[channel],
                        "a late-negotiated frame has the wrong pixels");
    }
    ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 5}});
    device.reset();
}

}
int main()
{
    try {
        codecLevelControls();
        sharedMemoryCapture();
        for (const auto page : {4096u, 16384u})
            for (const bool decoderPacks : {true, false})
                packedCapture(page, decoderPacks);
        for (const auto page : {4096u, 16384u}) lateNegotiation(page);
        for (const auto page : {4096u, 16384u})
            for (const auto format : {VIRTIO_VIDEO_FORMAT_NV12, VIRTIO_VIDEO_FORMAT_YUV420, VIRTIO_VIDEO_FORMAT_P010})
                exercise(format, page);
        activityLog();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
}
