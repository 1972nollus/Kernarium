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
/* Neighbouring samples differ along both axes, so a plane copied with the
 * wrong stride cannot reproduce the picture: every destination row would hold
 * a window that starts at the wrong sample. A flat fill survives any stride
 * error, which is why the fixtures below never use one. */
std::uint8_t patternLuma(std::uint32_t row, std::uint32_t column) noexcept
{
    return std::uint8_t(0x20 + (row * 5u + column * 3u) % 0xc0u);
}
/* The same pattern addressed by byte offset in a 64x64 luma plane, where a
 * ten-bit sample repeats its value in both of its bytes. */
std::uint8_t patternLumaByte(std::size_t index, bool ten) noexcept
{
    const auto sample = index / (ten ? 2u : 1u);
    return patternLuma(std::uint32_t(sample / 64), std::uint32_t(sample % 64));
}

/* The BT.601 limited-range conversion a device performs for a client that
 * asked for packed pixels, written independently of the device's own loop so
 * that a test sees placement and colour separately. */
void convertBt601Limited(std::uint8_t y, std::uint8_t cb, std::uint8_t cr, std::uint8_t* out)
{
    const double kr = 0.299, kb = 0.114, kg = 1.0 - kr - kb;
    const double chroma = 255.0 / 224.0;
    const double luma = (255.0 / 219.0) * (double(y) - 16.0);
    const double u = double(cb) - 128.0, v = double(cr) - 128.0;
    const auto byte = [](double value) { return std::uint8_t(std::clamp(value, 0.0, 255.0) + 0.5); };
    out[0] = byte(luma + chroma * 2.0 * (1.0 - kb) * u);
    out[1] = byte(luma - chroma * 2.0 * kb * (1.0 - kb) / kg * u
                       - chroma * 2.0 * kr * (1.0 - kr) / kg * v);
    out[2] = byte(luma + chroma * 2.0 * (1.0 - kr) * v);
}

/* `packs` distinguishes a decoder that produces packed frames itself, as the
 * Apple hardware does, from one that only ever produces its planar output and
 * leaves the device to convert. */
class Decoder final : public VideoDecoder {
public:
    explicit Decoder(bool ten, bool packs = false, bool defers = false,
                     std::uint32_t width = 64, std::uint32_t height = 64)
        : ten_(ten), packs_(packs), defers_(defers), width_(width), height_(height) {}
    /* Reads what the packet declares and decodes nothing, so a caller can
     * report the picture size before it has to choose an output layout. */
    void inspect(std::span<const std::uint8_t> packet) override
    {
        require(packet.size() == 5 && packet[0] == 1 && packet[4] == 5, "scatter input changed");
        format_ = {width_, height_, ten_ ? 10u : 8u};
    }
    void submit(std::span<const std::uint8_t> packet, std::uint64_t timestamp) override
    {
        require(packet.size() == 5 && packet[0] == 1 && packet[4] == 5, "scatter input changed");
        format_ = {width_, height_, ten_ ? 10u : 8u};
        DecodedVideoFrame frame;
        frame.format = format_;
        frame.timestamp = timestamp;
        /* A deferring decoder keeps its layout until a later picture, which is
         * what a real one does in the middle of a group of pictures. Before
         * any picture there is no group to be in the middle of, so a layout
         * asked for then takes effect on the first one. */
        if (defers_ && pictures_ && layout_ != adopted_) { ++deferred_; if (deferred_ > 1) adopted_ = layout_; }
        else adopted_ = layout_;
        ++pictures_;
        if (packs_ && adopted_ == VideoPixelLayout::packedBgra) {
            frame.layout = VideoPixelLayout::packedBgra;
            frame.format.bitDepth = 8;
            /* Every pixel differs from its neighbours in both directions, so
             * a row written at the wrong offset cannot go unnoticed. A flat
             * colour would survive any stride error. */
            frame.pixels.resize(std::size_t(width_) * height_ * 4);
            for (unsigned row = 0; row < height_; ++row)
                for (unsigned column = 0; column < width_; ++column) {
                    auto* out = frame.pixels.data() + (std::size_t(row) * width_ + column) * 4;
                    out[0] = std::uint8_t(row); out[1] = std::uint8_t(column);
                    out[2] = std::uint8_t(row + column); out[3] = 0xff;
                }
        } else {
            frame.color = {VideoColorMatrix::bt601, false};
            const std::size_t step = ten_ ? 2 : 1;
            frame.luma.resize(std::size_t(width_) * height_ * step);
            for (std::uint32_t row = 0; row < height_; ++row)
                for (std::uint32_t column = 0; column < width_; ++column) {
                    auto* out = frame.luma.data() + (std::size_t(row) * width_ + column) * step;
                    for (std::size_t byte = 0; byte < step; ++byte) out[byte] = patternLuma(row, column);
                }
            // Flat chroma keeps the converted colour a function of luma alone.
            frame.chroma.resize(std::size_t(width_) * ((height_ + 1) / 2) * step);
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
    unsigned pictures_ {};
    std::uint32_t width_ {64};
    std::uint32_t height_ {64};
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
    require(memory[3 * page] == patternLumaByte(0, ten) &&
            memory[3 * page + 4095] == patternLumaByte(4095, ten), "luma changed");
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
void sharedMemoryCapture(); // exercised again below, with the log on
void packedCapture(std::size_t page, bool decoderPacks, std::uint32_t width, std::uint32_t height);
void packedDefaultThroughStreamon(std::size_t page);

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
    /* The capture fields say where a client's capture memory lives and
     * whether it ever gives a filled buffer back, which is how a starved
     * client is told apart from a device that stops filling one. They are
     * appended, so the older prefix above still matches. */
    require(text.find("output=64x64 capture=guest-pages queued=3 distinct=1 "
                      "requeued=2 pending_at_close=0 raw=nv12 converted=0 coded=h264")
            != std::string::npos,
        "the video activity log omits the capture-queue facts");

    ::setenv("UML_VIDEO_ACTIVITY_LOG", path.c_str(), 1);
    sharedMemoryCapture();
    ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
    std::ostringstream shared;
    shared << std::ifstream(path).rdbuf();
    const auto sharedText = shared.str();
    std::filesystem::remove(path);
    const auto closeLine = sharedText.find("stream=3 close=");
    require(closeLine != std::string::npos,
        "the video activity log omits a shared-memory stream");
    const auto line = sharedText.substr(closeLine, sharedText.find('\n', closeLine) - closeLine);
    require(line.find("capture=shm-region") != std::string::npos,
        "a shared-memory capture buffer is not named as one");
    require(line.find("queued=2 distinct=1 requeued=1") != std::string::npos,
        "the video activity log does not count a re-queued capture buffer");

    /* Which capture format a client negotiated, and how many of its frames the
     * device had to repack itself, separate a decoder that hands the client its
     * own layout from one whose every frame costs a per-pixel conversion. */
    for (const bool decoderPacks : {true, false}) {
        ::setenv("UML_VIDEO_ACTIVITY_LOG", path.c_str(), 1);
        packedCapture(16384u, decoderPacks, 64, 64);
        ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
        std::ostringstream packed;
        packed << std::ifstream(path).rdbuf();
        const auto packedText = packed.str();
        std::filesystem::remove(path);
        require(packedText.find(decoderPacks ? "raw=bgra8888 converted=0"
                                             : "raw=bgra8888 converted=1") != std::string::npos,
            "the video activity log miscounts device-side packing");
    }

    /* Chromium's order, where the client never names a capture format and
     * simply starts the queue: the log has to show the packed format it was
     * given and no conversion at all, because the device held the encoded
     * packets until that silence became a choice. This is the number the
     * guest proof reads out of a real session. */
    ::setenv("UML_VIDEO_ACTIVITY_LOG", path.c_str(), 1);
    packedDefaultThroughStreamon(16384u);
    ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
    std::ostringstream streamon;
    streamon << std::ifstream(path).rdbuf();
    const auto streamonText = streamon.str();
    std::filesystem::remove(path);
    require(streamonText.find("stream=8 close=destroy packets=2 bytes=10 frames=2 output=64x64")
            != std::string::npos,
        "the video activity log omits what a client that never chose a format decoded");
    require(streamonText.find("raw=bgra8888 converted=0") != std::string::npos,
        "a client that never chose a capture format still cost a device-side conversion");

    /* A guest driver creates its stream when the device node is opened, before
     * the client has chosen a codec, and switches the codec with SET_PARAMS. The
     * open line therefore names the codec the driver created with, and only the
     * close line can name the one that decoded. */
    std::vector<VideoDecodeCapability> both {
        {VideoCodec::h264, VideoProfile::h264High, 4096, 2304, 51, 8},
        {VideoCodec::vp9, VideoProfile::vp9Profile0, 4096, 2304, 0, 8}};
    ::setenv("UML_VIDEO_ACTIVITY_LOG", path.c_str(), 1);
    {
        VirtioVideoDevice device(both, [](auto) { return std::make_unique<Decoder>(false); });
        std::uint64_t next = 1;
        auto send = [&](const auto& value) {
            std::vector<std::uint8_t> request;
            append(request, value);
            auto result = device.processQueue(0, next++, request, 16384);
            require(read<virtio_video_cmd_hdr>(result.response).type == VIRTIO_VIDEO_RESP_OK_NODATA,
                    "a codec switch command failed");
        };
        virtio_video_stream_create create {};
        create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 9};
        create.coded_format = VIRTIO_VIDEO_FORMAT_H264;
        send(create);
        virtio_video_set_params set {};
        set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, 9};
        set.params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
        set.params.format = VIRTIO_VIDEO_FORMAT_VP9;
        set.params.frame_width = 1920; set.params.frame_height = 1200;
        send(set);
        send(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 9}});
    }
    ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
    std::ostringstream switched;
    switched << std::ifstream(path).rdbuf();
    const auto switchedText = switched.str();
    std::filesystem::remove(path);
    require(switchedText.find("stream=9 open codec=h264") != std::string::npos,
        "the open line does not name the codec the stream was created with");
    require(switchedText.find("stream=9 close=destroy") != std::string::npos &&
            switchedText.find("coded=vp9") != std::string::npos,
        "the close line does not name the codec that decoded");
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
            written = frame[0] == patternLumaByte(0, false) &&
                frame[4095] == patternLumaByte(4095, false) && frame[4096] == 0x27 &&
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
void packedCapture(std::size_t page, bool decoderPacks, std::uint32_t width = 64,
                   std::uint32_t height = 64)
{
    std::vector<VideoDecodeCapability> caps {{VideoCodec::h264, VideoProfile::h264High, 4096, 2304, 51, 8}};
    VirtioVideoDevice device(caps, [&](auto) {
        return std::make_unique<Decoder>(false, decoderPacks, false, width, height);
    });
    const std::size_t plane = std::size_t(width) * height * 4;
    std::vector<std::uint8_t> memory(3 * page + plane + page, 0xcc);
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
    set.params.frame_width = width; set.params.frame_height = height;
    ok(set);
    virtio_video_get_params get {};
    get.hdr = {VIRTIO_VIDEO_CMD_GET_PARAMS, 7};
    get.queue_type = output;
    const auto negotiated = read<virtio_video_get_params_resp>(command(get).response).params;
    require(negotiated.format == VIRTIO_VIDEO_FORMAT_BGRA8888, "packed capture was substituted");
    require(negotiated.num_planes == 1, "packed capture is not a single plane");
    require(negotiated.plane_formats[0].stride == width * 4, "packed capture stride differs");
    require(negotiated.plane_formats[0].plane_size == plane, "packed capture plane size differs");

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
    require(read<virtio_video_cmd_hdr>(attach(output, 2, {{{base, std::uint32_t(plane), {}}}}).response).type == VIRTIO_VIDEO_RESP_OK_NODATA, "packed output attach failed");
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
    require(result.data_sizes[0] == plane, "packed frame size differs");
    /* BT.601 limited range, matching what the platform decoder infers from an
     * unspecified color attachment. */
    /* Every row, not a corner and not the first quarter: a destination row
     * written at the wrong offset holds a window of the picture that starts at
     * the wrong sample, which only a per-row comparison sees. */
    for (unsigned row = 0; row < height; ++row)
        for (unsigned column = 0; column < width; ++column) {
            const auto* got = memory.data() + base + (std::size_t(row) * width + column) * 4;
            std::uint8_t expected[4] = {std::uint8_t(row), std::uint8_t(column),
                                        std::uint8_t(row + column), 0xff};
            if (!decoderPacks) convertBt601Limited(patternLuma(row, column), 0x27, 0x91, expected);
            require(got[3] == 0xff, "packed alpha differs");
            // A rounding step apart is the most two correct conversions differ.
            const int slack = decoderPacks ? 0 : 1;
            for (unsigned channel = 0; channel < 3; ++channel)
                require(std::abs(int(got[channel]) - int(expected[channel])) <= slack,
                        "packed pixels differ");
        }
    ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 7}});
    device.reset();
}

/* A client that changes its capture format after frames have already been
 * delivered: it settles on planar, decodes, and only then asks for packed.
 * The decoder is in the middle of a group of pictures by then and cannot
 * change layout there, so every frame must still reach the client packed,
 * whether the decoder adopted the layout or the device converted for it. */
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

    /* Settle on planar first, so that pictures are delivered before the
     * client changes its mind. Until a format is settled the device holds the
     * encoded packets rather than decoding them into a layout nobody asked
     * for, which is what keeps the ordinary path free of conversion. */
    virtio_video_set_params planar {};
    planar.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, 5};
    planar.params.queue_type = output;
    planar.params.format = VIRTIO_VIDEO_FORMAT_NV12;
    planar.params.frame_width = planar.params.frame_height = 64;
    ok(planar);

    // Decoding raises the source change.
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

    /* Three pictures: the one decoded before the request, one the decoder is
     * still deferring, and one it has adopted the layout for. The request
     * reaches the decoder in the middle of a group of pictures, so a deferring
     * decoder is still planar for the first two and the device converts them. */
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
        for (unsigned row = 0; row < 64; ++row)
            for (unsigned column = 0; column < 64; ++column) {
                const auto* got = memory.data() + base + (std::size_t(row) * 64 + column) * 4;
                std::uint8_t expected[4] = {std::uint8_t(row), std::uint8_t(column),
                                            std::uint8_t(row + column), 0xff};
                if (round != 2) convertBt601Limited(patternLuma(row, column), 0x27, 0x91, expected);
                require(got[3] == 0xff, "a late-negotiated frame lost its alpha");
                for (unsigned channel = 0; channel < 3; ++channel)
                    require(std::abs(int(got[channel]) - int(expected[channel])) <= (round == 2 ? 0 : 1),
                            "a late-negotiated frame has the wrong pixels");
            }
    }
    ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 5}});
    device.reset();
}

/* A stateful client cannot name its capture format until the source change.
 * The device therefore reports the size from the packet's headers and holds
 * the packet, so the client's choice is known before anything is decoded and
 * the planar format it names is the layout its first picture already has.
 * Nothing is converted and nothing is round-tripped through another colour
 * space, so the samples the client reads are the decoder's own. */
void planarAfterSourceChange(std::size_t page)
{
    std::vector<VideoDecodeCapability> caps {{VideoCodec::h264, VideoProfile::h264High, 4096, 2304, 51, 8}};
    // A decoder that CAN pack, so a premature request for packing shows up.
    VirtioVideoDevice device(caps, [](auto) { return std::make_unique<Decoder>(false, true); });
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
    create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 6};
    create.coded_format = VIRTIO_VIDEO_FORMAT_H264;
    ok(create);
    require(device.processQueue(1, 901, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
            "event not deferred");
    auto attach = [&](unsigned queue, unsigned id, const std::vector<std::vector<virtio_video_resource_sg_entry>>& planes) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach{VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, 6, queue, id});
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

    /* The client's first act: queue a packet. Its headers say how big the
     * picture is, which is what raises the source change; the packet itself
     * waits, because the capture format is still the client's to choose. */
    virtio_video_resource_queue packet {};
    packet.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
    packet.stream_id = 6; packet.queue_type = input; packet.resource_id = 1;
    packet.data_sizes[0] = 5; packet.timestamp = 21;
    const auto packetRequest = next;
    require(command(packet).action == VhostUserQueueAction::defer,
            "the packet was decoded before the client chose a capture format");
    VhostUserQueueCompletion completion;
    require(device.takeQueueCompletion(completion) && completion.requestId == 901, "missing source event");
    require(read<virtio_video_event>(completion.response).event_type ==
            VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED, "incorrect source event");
    require(!device.takeQueueCompletion(completion),
            "an encoded buffer came back before the capture format settled");

    // Only now can it choose, and it chooses the planar format.
    virtio_video_set_params set {};
    set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, 6};
    set.params.queue_type = output;
    set.params.format = VIRTIO_VIDEO_FORMAT_NV12;
    set.params.frame_width = set.params.frame_height = 64;
    ok(set);
    const std::uint64_t base = 3 * page;
    const std::size_t luma = 64 * 64, chroma = 64 * 32;
    require(read<virtio_video_cmd_hdr>(attach(output, 2,
                {{{base, std::uint32_t(luma), {}}}, {{base + luma, std::uint32_t(chroma), {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "planar output attach failed");

    /* Naming the format releases what was held, so the picture is decoded
     * into the layout the client just named. */
    require(device.takeQueueCompletion(completion) && completion.requestId == packetRequest,
            "settling the capture format did not release the held packet");
    require(read<virtio_video_resource_queue_resp>(completion.response).flags == 0,
            "the released encoded buffer came back failed");

    virtio_video_resource_queue capture {};
    capture.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
    capture.stream_id = 6; capture.queue_type = output; capture.resource_id = 2;
    const auto captureRequest = next;
    require(command(capture).action == VhostUserQueueAction::defer, "capture was not deferred");
    require(device.takeQueueCompletion(completion) && completion.requestId == captureRequest,
            "missing capture completion");
    const auto result = read<virtio_video_resource_queue_resp>(completion.response);
    require(result.flags == 0,
            "the picture decoded before the client chose was failed instead of delivered");
    require(result.data_sizes[0] == luma && result.data_sizes[1] == chroma, "planar frame sizes differ");
    for (std::uint32_t row = 0; row < 64; ++row)
        for (std::uint32_t column = 0; column < 64; ++column)
            require(memory[base + row * 64 + column] == patternLuma(row, column),
                    "the planar frame has the wrong pixels");
    ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 6}});
    device.reset();
}

/* Chromium's order: a client that never names a format and simply starts the
 * capture queue with the packed one it was given. Starting the queue settles
 * the choice, and because nothing has been decoded yet the decoder takes the
 * packed layout for its very first picture - even this decoder, which like the
 * hardware can only change layout at a random access point. Every frame is
 * therefore packed by the decoder and none is converted by the device, which
 * the pixels prove: a converted frame cannot be exact through a BT.601 round
 * trip and a hardware-packed one is. */
void packedDefaultThroughStreamon(std::size_t page)
{
    std::vector<VideoDecodeCapability> caps {{VideoCodec::h264, VideoProfile::h264High, 4096, 2304, 51, 8}};
    VirtioVideoDevice device(caps, [](auto) { return std::make_unique<Decoder>(false, true, true); });
    const std::size_t plane = 64 * 64 * 4;
    std::vector<std::uint8_t> memory(3 * page + plane + page, 0xcc);
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
    create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 8};
    create.coded_format = VIRTIO_VIDEO_FORMAT_H264;
    ok(create);
    // The format the client is given without asking is the packed one.
    virtio_video_get_params get {};
    get.hdr = {VIRTIO_VIDEO_CMD_GET_PARAMS, 8};
    get.queue_type = output;
    require(read<virtio_video_get_params_resp>(command(get).response).params.format
            == VIRTIO_VIDEO_FORMAT_BGRA8888, "the default capture format is not the packed one");
    auto attach = [&](unsigned queue, unsigned id, const std::vector<std::vector<virtio_video_resource_sg_entry>>& planes) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach{VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, 8, queue, id});
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
    const std::uint64_t base = 3 * page;
    require(read<virtio_video_cmd_hdr>(attach(output, 2, {{{base, std::uint32_t(plane), {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "packed output attach failed");

    virtio_video_resource_queue packet {};
    packet.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
    packet.stream_id = 8; packet.queue_type = input; packet.resource_id = 1;
    packet.data_sizes[0] = 5;
    VhostUserQueueCompletion completion;
    for (unsigned round = 0; round < 2; ++round) {
        std::uint64_t packetRequest = 0;
        if (round == 0) {
            /* Queue a packet before the capture queue starts. Its headers say
             * how big the picture is; the packet itself waits, because the
             * client's silence is not yet a choice. */
            packet.timestamp = 31;
            packetRequest = next;
            require(command(packet).action == VhostUserQueueAction::defer,
                    "the packet was decoded before the capture queue started");
        }
        std::memset(memory.data() + base, 0xcc, plane);
        virtio_video_resource_queue capture {};
        capture.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
        capture.stream_id = 8; capture.queue_type = output; capture.resource_id = 2;
        const auto captureRequest = next;
        require(command(capture).action == VhostUserQueueAction::defer, "capture was not deferred");
        if (round) {
            packet.timestamp = 32;
            require(read<virtio_video_resource_queue_resp>(command(packet).response).flags == 0,
                    "later decode failed");
        }
        if (round == 0) {
            require(device.takeQueueCompletion(completion) && completion.requestId == packetRequest,
                    "starting the capture queue did not release the held packet");
            require(read<virtio_video_resource_queue_resp>(completion.response).flags == 0,
                    "the released encoded buffer came back failed");
        }
        require(device.takeQueueCompletion(completion) && completion.requestId == captureRequest,
                "missing capture completion");
        const auto result = read<virtio_video_resource_queue_resp>(completion.response);
        require(result.flags == 0, "a picture was failed for a client that never chose a format");
        require(result.data_sizes[0] == plane, "packed frame size differs");
        for (unsigned row = 0; row < 64; ++row)
            for (unsigned column = 0; column < 64; ++column) {
                const auto* got = memory.data() + base + (std::size_t(row) * 64 + column) * 4;
                const std::uint8_t expected[4] = {std::uint8_t(row), std::uint8_t(column),
                                                  std::uint8_t(row + column), 0xff};
                require(got[3] == 0xff, "a packed frame lost its alpha");
                for (unsigned channel = 0; channel < 3; ++channel)
                    require(got[channel] == expected[channel],
                            "a picture was converted by the device instead of packed by the decoder");
            }
    }
    ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 8}});
    device.reset();
}

/* A client feeds packets at its own pace and takes frames at its display
 * rate, so a decoder that runs faster than the display accumulates pictures.
 * The device must answer that by keeping the encoded buffer instead of
 * failing the command: a failure here reaches the client as EIO on both
 * queues and ends playback. */
/* The wait for a capture format is bounded. A client that queues encoded data
 * and never says what it wants must not be waited on forever, so at the bound
 * the device stops waiting and decodes what it holds, in the planar layout
 * nobody overrode, rather than failing the stream. (The byte bound is the same
 * rule for a client whose packets are large rather than many; this decoder
 * only accepts a five-byte packet, so the packet bound is the one a test can
 * reach.) */
void unsettledHoldIsBounded(std::size_t page)
{
    std::vector<VideoDecodeCapability> caps {{VideoCodec::h264, VideoProfile::h264High, 4096, 2304, 51, 8}};
    VirtioVideoDevice device(caps, [](auto) { return std::make_unique<Decoder>(false, true); });
    const std::size_t plane = 64 * 64 * 4;
    std::vector<std::uint8_t> memory(3 * page + plane + page, 0xcc);
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
    create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 9};
    create.coded_format = VIRTIO_VIDEO_FORMAT_H264;
    ok(create);
    require(device.processQueue(1, 950, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
            "event not deferred");
    auto attach = [&](unsigned queue, unsigned id, const std::vector<std::vector<virtio_video_resource_sg_entry>>& planes) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach{VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, 9, queue, id});
        for (const auto& plane : planes) {
            append(request, std::uint32_t(plane.size())); append(request, std::uint32_t(0));
            for (const auto& segment : plane) append(request, segment);
        }
        return device.processQueue(0, next++, request, 16384);
    };
    memory[page - 2] = 1; memory[page - 1] = 2;
    memory[2 * page] = 3; memory[2 * page + 1] = 4; memory[2 * page + 2] = 5;
    /* One encoded buffer more than the device will hold unanswered, each with
     * its own identifier because a held one stays queued. */
    const unsigned packets = 33;
    for (unsigned id = 1; id <= packets; ++id)
        require(read<virtio_video_cmd_hdr>(
                    attach(input, id, {{{page - 2, 2, {}}, {2 * page, 3, {}}}}).response).type
                == VIRTIO_VIDEO_RESP_OK_NODATA, "input attach failed");
    require(read<virtio_video_cmd_hdr>(attach(output, 100, {{{3 * page, std::uint32_t(plane), {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "capture attach failed");

    // Never name a format, never start the capture queue: only queue input.
    std::deque<std::uint64_t> waiting;
    for (unsigned id = 1; id <= packets; ++id) {
        virtio_video_resource_queue packet {};
        packet.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
        packet.stream_id = 9; packet.queue_type = input; packet.resource_id = id;
        packet.data_sizes[0] = 5; packet.timestamp = 200 + id;
        const auto request = next;
        const auto result = command(packet);
        if (result.action == VhostUserQueueAction::defer) waiting.push_back(request);
        else {
            const auto answer = read<virtio_video_resource_queue_resp>(result.response);
            require(answer.hdr.type == VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE && answer.flags == 0,
                    "an encoded buffer failed instead of being decoded");
        }
    }
    VhostUserQueueCompletion completion;
    unsigned released = 0;
    while (device.takeQueueCompletion(completion)) {
        if (completion.requestId == 950) {
            require(read<virtio_video_event>(completion.response).event_type ==
                    VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED, "incorrect source event");
            continue;
        }
        require(!waiting.empty() && completion.requestId == waiting.front(),
                "an encoded buffer came back out of order");
        waiting.pop_front();
        ++released;
        require(read<virtio_video_resource_queue_resp>(completion.response).flags == 0,
                "a released encoded buffer came back failed");
    }
    require(released > 0, "the device waited for a capture format that was never going to come");

    /* The stream is still usable: the client can still take frames, and every
     * buffer it gave comes back. */
    while (!waiting.empty()) {
        virtio_video_resource_queue capture {};
        capture.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
        capture.stream_id = 9; capture.queue_type = output; capture.resource_id = 100;
        const auto captureRequest = next;
        require(command(capture).action == VhostUserQueueAction::defer, "capture was not deferred");
        bool frame = false;
        while (device.takeQueueCompletion(completion)) {
            if (completion.requestId == captureRequest) {
                frame = true;
                const auto result = read<virtio_video_resource_queue_resp>(completion.response);
                require(result.flags == 0, "a frame was failed after the bounded wait");
                require(result.data_sizes[0] == plane, "the frame has the wrong size");
            } else {
                require(!waiting.empty() && completion.requestId == waiting.front(),
                        "an encoded buffer came back out of order");
                waiting.pop_front();
            }
        }
        require(frame, "a returned capture buffer produced no frame");
    }
    ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 9}});
    device.reset();
}

void inputBackpressure(std::size_t page)
{
    std::vector<VideoDecodeCapability> caps {{VideoCodec::h264, VideoProfile::h264High, 4096, 2304, 51, 8}};
    VirtioVideoDevice device(caps, [](auto) { return std::make_unique<Decoder>(false, true); });
    const std::size_t plane = 64 * 64 * 4;
    std::vector<std::uint8_t> memory(3 * page + plane + page, 0xcc);
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
    create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 4};
    create.coded_format = VIRTIO_VIDEO_FORMAT_H264;
    ok(create);
    /* Choose the capture format up front, so the only thing that can hold an
     * encoded buffer here is the frame queue filling up. */
    virtio_video_set_params chosen {};
    chosen.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, 4};
    chosen.params.queue_type = output;
    chosen.params.format = VIRTIO_VIDEO_FORMAT_BGRA8888;
    chosen.params.frame_width = chosen.params.frame_height = 64;
    ok(chosen);
    auto attach = [&](unsigned queue, unsigned id, const std::vector<std::vector<virtio_video_resource_sg_entry>>& planes) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach{VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, 4, queue, id});
        for (const auto& plane : planes) {
            append(request, std::uint32_t(plane.size())); append(request, std::uint32_t(0));
            for (const auto& segment : plane) append(request, segment);
        }
        return device.processQueue(0, next++, request, 16384);
    };
    memory[page - 2] = 1; memory[page - 1] = 2;
    memory[2 * page] = 3; memory[2 * page + 1] = 4; memory[2 * page + 2] = 5;
    // Twelve encoded buffers over the same five bytes, as a client's ring.
    const unsigned packets = 12;
    for (unsigned id = 1; id <= packets; ++id)
        require(read<virtio_video_cmd_hdr>(
                    attach(input, id, {{{page - 2, 2, {}}, {2 * page, 3, {}}}}).response).type
                == VIRTIO_VIDEO_RESP_OK_NODATA, "input attach failed");
    require(read<virtio_video_cmd_hdr>(attach(output, 100, {{{3 * page, std::uint32_t(plane), {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "capture attach failed");

    std::deque<std::uint64_t> waiting;
    unsigned answered = 0;
    for (unsigned id = 1; id <= packets; ++id) {
        virtio_video_resource_queue packet {};
        packet.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
        packet.stream_id = 4; packet.queue_type = input; packet.resource_id = id;
        packet.data_sizes[0] = 5; packet.timestamp = 100 + id;
        const auto request = next;
        const auto result = command(packet);
        if (result.action == VhostUserQueueAction::defer) waiting.push_back(request);
        else {
            ++answered;
            const auto answer = read<virtio_video_resource_queue_resp>(result.response);
            require(answer.hdr.type == VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE && answer.flags == 0,
                    "an encoded buffer failed instead of waiting");
        }
    }
    require(answered > 0 && !waiting.empty(),
            "the device did not hold an encoded buffer against a full frame queue");

    /* Every capture buffer the client gives back releases one held packet, so
     * the stream keeps running instead of erroring out. */
    const auto held = waiting.size();
    for (std::size_t round = 0; round < held; ++round) {
        virtio_video_resource_queue capture {};
        capture.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
        capture.stream_id = 4; capture.queue_type = output; capture.resource_id = 100;
        const auto captureRequest = next;
        require(command(capture).action == VhostUserQueueAction::defer, "capture was not deferred");
        bool frame = false, released = false;
        VhostUserQueueCompletion completion;
        while (device.takeQueueCompletion(completion)) {
            if (completion.requestId == captureRequest) {
                frame = true;
                const auto result = read<virtio_video_resource_queue_resp>(completion.response);
                require(result.flags == 0, "a frame was failed under backpressure");
                require(result.data_sizes[0] == plane, "the frame has the wrong size");
            } else {
                require(!waiting.empty() && completion.requestId == waiting.front(),
                        "an encoded buffer came back out of order");
                waiting.pop_front();
                released = true;
                const auto result = read<virtio_video_resource_queue_resp>(completion.response);
                require(result.flags == 0 && result.data_sizes[0] == 5,
                        "a held encoded buffer came back failed");
            }
        }
        require(frame && released, "a returned capture buffer did not release a held packet");
    }
    require(waiting.empty(), "the device kept an encoded buffer with room to decode");
    ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 4}});
    device.reset();
}

/* A format descriptor's `mask` names positions in the other queue's list, in
 * the order the same response carries them, and a guest that walks that list
 * offers its client exactly the formats the bits select. This checks both
 * directions against the lists actually sent, so a raw format added in the
 * middle of the list cannot silently stop being reachable. */
void capabilityMasks()
{
    std::vector<VideoDecodeCapability> caps {
        {VideoCodec::vp9, VideoProfile::vp9Profile0, 4096, 2304, 0, 8},
        {VideoCodec::hevc, VideoProfile::hevcMain, 8192, 4320, 0, 8},
        {VideoCodec::hevc, VideoProfile::hevcMain10, 8192, 4320, 0, 10},
    };
    VirtioVideoDevice device(caps, [](auto) { return std::make_unique<Decoder>(false); });
    std::uint64_t next = 1;
    auto formats = [&](std::uint32_t queue) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_query_capability{{VIRTIO_VIDEO_CMD_QUERY_CAPABILITY, 0}, queue, {}});
        const auto reply = device.processQueue(0, next++, request, 65536).response;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> described;
        auto offset = sizeof(virtio_video_query_capability_resp);
        for (unsigned i = 0; i < read<virtio_video_query_capability_resp>(reply).num_descs; ++i) {
            const auto desc = read<virtio_video_format_desc>(std::span(reply).subspan(offset));
            described.emplace_back(desc.format, desc.mask);
            offset += sizeof(virtio_video_format_desc);
            for (unsigned frame = 0; frame < desc.num_frames; ++frame) {
                const auto range = read<virtio_video_format_frame>(std::span(reply).subspan(offset));
                offset += sizeof(virtio_video_format_frame) +
                    range.num_rates * sizeof(virtio_video_format_range);
            }
        }
        return described;
    };
    const auto coded = formats(VIRTIO_VIDEO_QUEUE_TYPE_INPUT);
    const auto raw = formats(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT);
    require(coded.size() == 2 && raw.size() == 4, "capability lists changed shape");

    /* A client that enumerates the capture list and never negotiates receives
     * the first entry, so the head of that list and the format a fresh stream
     * reports have to be the same thing: the packed format every renderer can
     * draw. A client that picks by preference and a client that picks nothing
     * then agree, instead of one of them being handed planar data it did not
     * ask for. */
    require(raw.front().first == VIRTIO_VIDEO_FORMAT_BGRA8888,
            "the capture list does not offer the packed format first");
    virtio_video_stream_create create {};
    create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 11};
    create.coded_format = VIRTIO_VIDEO_FORMAT_VP9;
    {
        std::vector<std::uint8_t> request;
        append(request, create);
        require(read<virtio_video_cmd_hdr>(
                    device.processQueue(0, next++, request, 16384).response).type
                == VIRTIO_VIDEO_RESP_OK_NODATA, "stream create failed");
    }
    virtio_video_get_params get {};
    get.hdr = {VIRTIO_VIDEO_CMD_GET_PARAMS, 11};
    get.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    {
        std::vector<std::uint8_t> request;
        append(request, get);
        const auto params = read<virtio_video_get_params_resp>(
            device.processQueue(0, next++, request, 16384).response).params;
        require(params.format == raw.front().first,
                "a fresh stream's capture format is not the first one advertised");
        require(params.num_planes == 1 && params.plane_formats[0].stride == 128 * 4,
                "the default capture format is not a single packed plane");
    }

    auto selected = [](const auto& list, std::uint64_t mask) {
        std::vector<std::uint32_t> chosen;
        for (std::size_t index = 0; index < list.size(); ++index)
            if (mask & (std::uint64_t(1) << index)) chosen.push_back(list[index].first);
        std::sort(chosen.begin(), chosen.end());
        return chosen;
    };
    auto sorted = [](std::vector<std::uint32_t> values) {
        std::sort(values.begin(), values.end());
        return values;
    };

    for (const auto& [format, mask] : coded) {
        // VP9 is 8-bit here, HEVC has both an 8-bit and a 10-bit capability.
        const auto expected = format == VIRTIO_VIDEO_FORMAT_VP9
            ? sorted({VIRTIO_VIDEO_FORMAT_NV12, VIRTIO_VIDEO_FORMAT_YUV420,
                      VIRTIO_VIDEO_FORMAT_BGRA8888})
            : sorted({VIRTIO_VIDEO_FORMAT_NV12, VIRTIO_VIDEO_FORMAT_YUV420,
                      VIRTIO_VIDEO_FORMAT_BGRA8888, VIRTIO_VIDEO_FORMAT_P010});
        require(selected(raw, mask) == expected, "a coded format names the wrong capture formats");
    }
    for (const auto& [format, mask] : raw) {
        const auto expected = format == VIRTIO_VIDEO_FORMAT_P010
            ? sorted({VIRTIO_VIDEO_FORMAT_HEVC})
            : sorted({VIRTIO_VIDEO_FORMAT_VP9, VIRTIO_VIDEO_FORMAT_HEVC});
        require(selected(coded, mask) == expected, "a capture format names the wrong codecs");
    }
    device.reset();
}

}
int main()
{
    try {
        codecLevelControls();
        capabilityMasks();
        sharedMemoryCapture();
        for (const auto page : {4096u, 16384u})
            for (const bool decoderPacks : {true, false})
                packedCapture(page, decoderPacks);
        // The geometry a browser actually decodes, where a stride error shows.
        for (const bool decoderPacks : {true, false})
            packedCapture(16384u, decoderPacks, 1920, 1200);
        for (const auto page : {4096u, 16384u}) lateNegotiation(page);
        for (const auto page : {4096u, 16384u}) inputBackpressure(page);
        for (const auto page : {4096u, 16384u}) unsettledHoldIsBounded(page);
        for (const auto page : {4096u, 16384u}) planarAfterSourceChange(page);
        for (const auto page : {4096u, 16384u}) packedDefaultThroughStreamon(page);
        for (const auto page : {4096u, 16384u})
            for (const auto format : {VIRTIO_VIDEO_FORMAT_NV12, VIRTIO_VIDEO_FORMAT_YUV420, VIRTIO_VIDEO_FORMAT_P010})
                exercise(format, page);
        activityLog();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
}
