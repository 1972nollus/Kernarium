// SPDX-License-Identifier: GPL-2.0-or-later
#include "virtio_video_device.hpp"
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
class Decoder final : public VideoDecoder {
public:
    explicit Decoder(bool ten) : ten_(ten) {}
    void submit(std::span<const std::uint8_t> packet, std::uint64_t timestamp) override
    {
        require(packet.size() == 5 && packet[0] == 1 && packet[4] == 5, "scatter input changed");
        format_ = {64, 64, ten_ ? 10u : 8u};
        DecodedVideoFrame frame {format_, timestamp, {}, {}};
        frame.luma.resize(4096 * (ten_ ? 2 : 1), 0x39);
        frame.chroma.resize(2048 * (ten_ ? 2 : 1));
        for (std::size_t i = 0; i < frame.chroma.size(); ++i) frame.chroma[i] = i % 2 ? 0x91 : 0x27;
        frames_.push_back(std::move(frame));
    }
    void drain() override {}
    std::vector<DecodedVideoFrame> takeFrames() override { return std::exchange(frames_, {}); }
    VideoFormat format() const noexcept override { return format_; }
private:
    bool ten_;
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

}
int main()
{
    try {
        codecLevelControls();
        for (const auto page : {4096u, 16384u})
            for (const auto format : {VIRTIO_VIDEO_FORMAT_NV12, VIRTIO_VIDEO_FORMAT_YUV420, VIRTIO_VIDEO_FORMAT_P010})
                exercise(format, page);
        activityLog();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
}
