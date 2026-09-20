// SPDX-License-Identifier: GPL-2.0-or-later
#include "virtio_video_device.hpp"
#include "video_capabilities.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <unordered_map>

namespace uml::video {
namespace {
using uml::virtio::VhostUserQueueAction;
using uml::virtio::VhostUserQueueCompletion;
using uml::virtio::VhostUserQueueResult;
constexpr std::size_t maximumPacket = 8 * 1024 * 1024;
constexpr std::size_t maximumResource = 128 * 1024 * 1024;

template<class T> T load(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < sizeof(T)) throw std::invalid_argument("short video command");
    T value;
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}
template<class T> std::vector<std::uint8_t> bytes(const T& value)
{
    std::vector<std::uint8_t> result(sizeof(value));
    std::memcpy(result.data(), &value, sizeof(value));
    return result;
}
std::vector<std::uint8_t> reply(std::uint32_t stream, std::uint32_t type = VIRTIO_VIDEO_RESP_OK_NODATA)
{
    return bytes(virtio_video_cmd_hdr{type, stream});
}
bool validQueue(std::uint32_t queue)
{
    return queue == VIRTIO_VIDEO_QUEUE_TYPE_INPUT || queue == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
}
std::uint64_t resourceKey(std::uint32_t queue, std::uint32_t id)
{
    return (std::uint64_t(queue) << 32) | id;
}
const char* codecName(uml::platform::VideoCodec codec) noexcept
{
    switch (codec) {
    case uml::platform::VideoCodec::h264: return "h264";
    case uml::platform::VideoCodec::hevc: return "hevc";
    case uml::platform::VideoCodec::jpeg: return "jpeg";
    }
    return "unknown";
}
const char* profileName(uml::platform::VideoProfile profile) noexcept
{
    switch (profile) {
    case uml::platform::VideoProfile::h264Baseline: return "h264-baseline";
    case uml::platform::VideoProfile::h264ConstrainedBaseline: return "h264-constrained-baseline";
    case uml::platform::VideoProfile::h264Main: return "h264-main";
    case uml::platform::VideoProfile::h264High: return "h264-high";
    case uml::platform::VideoProfile::h264High10: return "h264-high10";
    case uml::platform::VideoProfile::hevcMain: return "hevc-main";
    case uml::platform::VideoProfile::hevcMain10: return "hevc-main10";
    case uml::platform::VideoProfile::jpegBaseline: return "jpeg-baseline";
    }
    return "unknown";
}

/* Whether an application really decoded in hardware cannot be answered from
 * inside the guest: a player that silently falls back to software shows the
 * same correct picture, and the guest only sees that it opened a device node.
 * Setting `UML_VIDEO_ACTIVITY_LOG` to a writable path makes the device append
 * what it advertised, every stream a guest opened, and what each stream
 * actually decoded. It is a file rather than stderr because the emulator's
 * stderr shares the guest console and loses lines under load. Nothing is
 * opened, formatted, or written when the variable is unset.
 */
class ActivityLog final {
public:
    ActivityLog()
    {
        const char* path = std::getenv("UML_VIDEO_ACTIVITY_LOG");
        if (path != nullptr && path[0] != '\0') file_ = std::fopen(path, "ae");
    }
    ~ActivityLog() { if (file_ != nullptr) std::fclose(file_); }
    ActivityLog(const ActivityLog&) = delete;
    ActivityLog& operator=(const ActivityLog&) = delete;

    void advertised(const char* codec, const char* profile, std::uint32_t width,
                    std::uint32_t height, std::uint32_t bitDepth)
    {
        if (file_ == nullptr) return;
        std::fprintf(file_, "uml-video %.3f advertise codec=%s profile=%s max=%ux%u depth=%u\n",
            elapsed(), codec, profile, width, height, bitDepth);
        std::fflush(file_);
    }
    void opened(std::uint32_t stream, const char* codec, const char* profile)
    {
        if (file_ == nullptr) return;
        std::fprintf(file_, "uml-video %.3f stream=%u open codec=%s profile=%s\n",
            elapsed(), stream, codec, profile);
        std::fflush(file_);
    }
    void closed(std::uint32_t stream, const char* reason, std::uint64_t packets,
                std::uint64_t packetBytes, std::uint64_t frames,
                std::uint32_t width, std::uint32_t height)
    {
        if (file_ == nullptr) return;
        std::fprintf(file_,
            "uml-video %.3f stream=%u close=%s packets=%llu bytes=%llu frames=%llu output=%ux%u\n",
            elapsed(), stream, reason, static_cast<unsigned long long>(packets),
            static_cast<unsigned long long>(packetBytes),
            static_cast<unsigned long long>(frames), width, height);
        std::fflush(file_);
    }

private:
    [[nodiscard]] double elapsed() const noexcept
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
    std::FILE* file_ {};
    std::chrono::steady_clock::time_point start_ {std::chrono::steady_clock::now()};
};
virtio_video_params params(std::uint32_t queue, std::uint32_t format, std::uint32_t width, std::uint32_t height)
{
    virtio_video_params p {};
    p.queue_type = queue;
    p.format = format;
    p.frame_width = width;
    p.frame_height = height;
    p.min_buffers = queue == VIRTIO_VIDEO_QUEUE_TYPE_INPUT ? 1 : 4;
    p.max_buffers = 32;
    p.crop.width = width;
    p.crop.height = height;
    p.frame_rate = 30;
    if (queue == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
        p.num_planes = 1;
        p.plane_formats[0].plane_size = maximumPacket;
    } else {
        const auto y = width * height;
        const auto cw = (width + 1) / 2;
        const auto ch = (height + 1) / 2;
        if (format == VIRTIO_VIDEO_FORMAT_P010) {
            p.num_planes = 1;
            p.plane_formats[0] = {2 * y + 4 * cw * ch, 2 * width};
        } else {
            p.num_planes = format == VIRTIO_VIDEO_FORMAT_YUV420 ? 3 : 2;
            p.plane_formats[0] = {y, width};
            p.plane_formats[1] = {cw * ch * (p.num_planes == 2 ? 2u : 1u), cw * (p.num_planes == 2 ? 2u : 1u)};
            if (p.num_planes == 3) p.plane_formats[2] = {cw * ch, cw};
        }
    }
    return p;
}
}

class VirtioVideoDevice::Implementation {
public:
    struct Segment { std::uint64_t address; std::uint32_t length; };
    struct Resource { std::vector<std::vector<Segment>> planes; bool queued {}; };
    struct Output { std::uint64_t request; std::uint32_t resource; };
    struct Stream {
        std::uint32_t coded {};
        std::uint32_t profile {};
        virtio_video_params input {};
        virtio_video_params output {};
        bool sourceKnown {};
        bool draining {};
        bool eosSent {};
        std::uint64_t packets {};
        std::uint64_t packetBytes {};
        std::uint64_t delivered {};
        std::unique_ptr<uml::platform::VideoDecoder> decoder;
        std::unordered_map<std::uint64_t, Resource> resources;
        std::deque<Output> outputs;
        std::deque<uml::platform::DecodedVideoFrame> frames;
    };

    ActivityLog log;
    std::vector<uml::platform::VideoDecodeCapability> capabilities;
    DecoderFactory factory;
    GuestMapper mapper;
    std::unordered_map<std::uint32_t, Stream> streams;
    std::deque<std::uint64_t> eventBuffers;
    std::deque<virtio_video_event> events;
    std::deque<VhostUserQueueCompletion> completions;

    ~Implementation() { closeAll("shutdown"); }

    void record(std::uint32_t id, const Stream& stream, const char* reason) noexcept
    {
        log.closed(id, reason, stream.packets, stream.packetBytes, stream.delivered,
                   stream.output.frame_width, stream.output.frame_height);
    }

    void closeAll(const char* reason) noexcept
    {
        for (const auto& [id, stream] : streams) record(id, stream, reason);
    }

    void event(std::uint32_t stream, std::uint32_t type)
    {
        if (events.size() >= 64) throw std::runtime_error("video event queue is full");
        events.push_back({type, stream});
        while (!events.empty() && !eventBuffers.empty()) {
            completions.push_back({eventBuffers.front(), bytes(events.front())});
            eventBuffers.pop_front();
            events.pop_front();
        }
    }

    std::vector<std::uint8_t> queued(std::uint32_t stream, std::uint64_t timestamp,
                                    std::uint32_t flags, const virtio_video_params* p = nullptr)
    {
        virtio_video_resource_queue_resp r {};
        r.hdr = {VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE, stream};
        r.timestamp = timestamp;
        r.flags = flags;
        if (p) for (unsigned i = 0; i < p->num_planes; ++i) r.data_sizes[i] = p->plane_formats[i].plane_size;
        return bytes(r);
    }

    void cancelOutputs(std::uint32_t id, Stream& stream)
    {
        for (const auto& output : stream.outputs) {
            completions.push_back({output.request, queued(id, 0, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR)});
            const auto found = stream.resources.find(resourceKey(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, output.resource));
            if (found != stream.resources.end()) found->second.queued = false;
        }
        stream.outputs.clear();
    }

    bool transfer(const std::vector<Segment>& segments, std::span<std::uint8_t> data, bool write)
    {
        std::size_t offset = 0;
        for (const auto segment : segments) {
            const auto size = std::min<std::size_t>(data.size() - offset, segment.length);
            if (!size) break;
            auto memory = mapper ? mapper(segment.address, size) : std::span<std::uint8_t>{};
            if (memory.size() != size) return false;
            if (write) std::memcpy(memory.data(), data.data() + offset, size);
            else std::memcpy(data.data() + offset, memory.data(), size);
            offset += size;
        }
        return offset == data.size();
    }

    void collect(std::uint32_t id, Stream& stream)
    {
        const auto format = stream.decoder->format();
        if (format.width && (!stream.sourceKnown || stream.output.frame_width != format.width ||
            stream.output.frame_height != format.height ||
            (stream.output.format == VIRTIO_VIDEO_FORMAT_P010) != (format.bitDepth == 10))) {
            auto raw = stream.output.format;
            if (format.bitDepth == 10) raw = VIRTIO_VIDEO_FORMAT_P010;
            else if (raw == VIRTIO_VIDEO_FORMAT_P010) raw = VIRTIO_VIDEO_FORMAT_NV12;
            stream.output = params(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, raw, format.width, format.height);
            stream.sourceKnown = true;
            event(id, VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED);
        }
        for (auto& frame : stream.decoder->takeFrames()) {
            if (stream.frames.size() >= 32) throw std::runtime_error("video stream output queue is full");
            stream.frames.push_back(std::move(frame));
        }
        pump(id, stream);
    }

    void pump(std::uint32_t id, Stream& stream)
    {
        while (!stream.outputs.empty()) {
            if (stream.frames.empty() && (!stream.draining || stream.eosSent)) break;
            const auto output = stream.outputs.front();
            stream.outputs.pop_front();
            auto& resource = stream.resources.at(resourceKey(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, output.resource));
            resource.queued = false;
            if (stream.frames.empty()) {
                completions.push_back({output.request, queued(id, 0, VIRTIO_VIDEO_DEQUEUE_FLAG_EOS)});
                stream.eosSent = true;
                continue;
            }
            auto frame = std::move(stream.frames.front());
            stream.frames.pop_front();
            ++stream.delivered;
            std::vector<std::vector<std::uint8_t>> planes;
            if (stream.output.format == VIRTIO_VIDEO_FORMAT_P010) {
                frame.luma.insert(frame.luma.end(), frame.chroma.begin(), frame.chroma.end());
                planes.push_back(std::move(frame.luma));
            } else if (stream.output.format == VIRTIO_VIDEO_FORMAT_YUV420) {
                planes.push_back(std::move(frame.luma));
                planes.emplace_back(frame.chroma.size() / 2);
                planes.emplace_back(frame.chroma.size() / 2);
                for (std::size_t i = 0; i < frame.chroma.size() / 2; ++i) {
                    planes[1][i] = frame.chroma[2 * i];
                    planes[2][i] = frame.chroma[2 * i + 1];
                }
            } else {
                planes.push_back(std::move(frame.luma));
                planes.push_back(std::move(frame.chroma));
            }
            bool good = resource.planes.size() == planes.size();
            for (std::size_t i = 0; good && i < planes.size(); ++i)
                good = transfer(resource.planes[i], planes[i], true);
            completions.push_back({output.request, queued(id, frame.timestamp,
                good ? 0 : VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, good ? &stream.output : nullptr)});
        }
    }

    VhostUserQueueResult command(std::uint64_t requestId, std::span<const std::uint8_t> request)
    {
        const auto header = load<virtio_video_cmd_hdr>(request);
        const auto id = header.stream_id;
        const auto done = [](std::vector<std::uint8_t> data) {
            return VhostUserQueueResult{VhostUserQueueAction::complete, std::move(data)};
        };
        if (header.type == VIRTIO_VIDEO_CMD_QUERY_CAPABILITY) {
            const auto q = load<virtio_video_query_capability>(request);
            return done(capabilityResponse(capabilities, q.queue_type));
        }
        if (header.type == VIRTIO_VIDEO_CMD_QUERY_CONTROL) {
            const auto q = load<virtio_video_query_control>(request);
            const auto format = load<virtio_video_query_control_profile>(request.subspan(sizeof(q))).format;
            std::vector<std::uint32_t> values;
            if (q.control == VIRTIO_VIDEO_CONTROL_PROFILE) values = profileValues(capabilities, format);
            else if (q.control == VIRTIO_VIDEO_CONTROL_LEVEL) {
                if (format == VIRTIO_VIDEO_FORMAT_H264)
                    for (unsigned level = VIRTIO_VIDEO_LEVEL_H264_MIN; level <= VIRTIO_VIDEO_LEVEL_H264_MAX; ++level)
                        values.push_back(level);
                if (format == VIRTIO_VIDEO_FORMAT_HEVC)
                    for (unsigned level = VIRTIO_VIDEO_LEVEL_HEVC_MIN; level <= VIRTIO_VIDEO_LEVEL_HEVC_MAX; ++level)
                        values.push_back(level);
            } else return done(reply(id, VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL));
            auto result = reply(id, VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL);
            const auto count = bytes(virtio_video_query_control_resp_profile{std::uint32_t(values.size()), {}});
            result.insert(result.end(), count.begin(), count.end());
            for (auto value : values) {
                const auto data = bytes(value);
                result.insert(result.end(), data.begin(), data.end());
            }
            return done(std::move(result));
        }
        if (header.type == VIRTIO_VIDEO_CMD_STREAM_CREATE) {
            const auto q = load<virtio_video_stream_create>(request);
            if (!id || streams.contains(id) || streams.size() >= 16 || q.in_mem_type || q.out_mem_type)
                return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER));
            const auto cap = std::find_if(capabilities.begin(), capabilities.end(),
                [&q](const auto& c) { return wireCodec(c.codec) == q.coded_format; });
            if (cap == capabilities.end()) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER));
            Stream stream;
            stream.coded = q.coded_format;
            stream.profile = wireProfile(cap->profile);
            stream.decoder = factory(cap->codec);
            stream.input = params(VIRTIO_VIDEO_QUEUE_TYPE_INPUT, q.coded_format, 128, 128);
            stream.output = params(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, VIRTIO_VIDEO_FORMAT_NV12, 128, 128);
            streams.emplace(id, std::move(stream));
            log.opened(id, codecName(cap->codec), profileName(cap->profile));
            return done(reply(id));
        }
        const auto found = streams.find(id);
        if (found == streams.end()) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID));
        auto& stream = found->second;
        switch (header.type) {
        case VIRTIO_VIDEO_CMD_STREAM_DESTROY:
            cancelOutputs(id, stream);
            record(id, stream, "destroy");
            streams.erase(found);
            return done(reply(id));
        case VIRTIO_VIDEO_CMD_GET_PARAMS: {
            const auto q = load<virtio_video_get_params>(request);
            if (!validQueue(q.queue_type)) throw std::invalid_argument("invalid parameter queue");
            return done(bytes(virtio_video_get_params_resp{{VIRTIO_VIDEO_RESP_OK_GET_PARAMS, id},
                q.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT ? stream.input : stream.output}));
        }
        case VIRTIO_VIDEO_CMD_SET_PARAMS: {
            const auto p = load<virtio_video_set_params>(request).params;
            const auto coded = p.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT ? p.format : stream.coded;
            const auto cap = std::find_if(capabilities.begin(), capabilities.end(),
                [coded](const auto& c) { return wireCodec(c.codec) == coded; });
            if (cap == capabilities.end() || !validQueue(p.queue_type) || !p.frame_width || !p.frame_height ||
                p.frame_width > cap->maximumWidth || p.frame_height > cap->maximumHeight)
                throw std::invalid_argument("invalid video parameters");
            if (p.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                if (p.format != stream.coded) {
                    if (!stream.resources.empty() || stream.sourceKnown)
                        throw std::invalid_argument("coded format changed with allocated buffers");
                    stream.decoder = factory(cap->codec);
                    stream.coded = p.format;
                    stream.profile = wireProfile(cap->profile);
                    stream.output = params(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT,
                        cap->bitDepth == 10 ? VIRTIO_VIDEO_FORMAT_P010 : VIRTIO_VIDEO_FORMAT_NV12,
                        p.frame_width, p.frame_height);
                }
                stream.input = params(p.queue_type, p.format, p.frame_width, p.frame_height);
            } else {
                if (p.format != VIRTIO_VIDEO_FORMAT_NV12 && p.format != VIRTIO_VIDEO_FORMAT_YUV420 &&
                    p.format != VIRTIO_VIDEO_FORMAT_P010) throw std::invalid_argument("unsupported raw video format");
                const bool tenBit = p.format == VIRTIO_VIDEO_FORMAT_P010;
                if (std::none_of(capabilities.begin(), capabilities.end(), [&stream, tenBit](const auto& c) {
                        return wireCodec(c.codec) == stream.coded && c.bitDepth == (tenBit ? 10u : 8u);
                    }) || (stream.sourceKnown && (stream.decoder->format().bitDepth == 10) != tenBit))
                    throw std::invalid_argument("raw video bit depth is incompatible with the stream");
                stream.output = params(p.queue_type, p.format, p.frame_width, p.frame_height);
            }
            return done(reply(id));
        }
        case VIRTIO_VIDEO_CMD_RESOURCE_ATTACH: {
            const auto q = load<virtio_video_resource_attach>(request);
            if (!validQueue(q.queue_type) || !q.resource_id || stream.resources.size() >= 128)
                throw std::invalid_argument("invalid video resource");
            const auto key = resourceKey(q.queue_type, q.resource_id);
            if (stream.resources.contains(key)) throw std::invalid_argument("duplicate video resource");
            const auto& p = q.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT ? stream.input : stream.output;
            auto payload = request.subspan(sizeof(q));
            Resource resource;
            std::size_t total = 0;
            for (unsigned plane = 0; plane < p.num_planes; ++plane) {
                if (payload.size() < 8) throw std::invalid_argument("short video scatter list");
                const auto count = load<std::uint32_t>(payload);
                payload = payload.subspan(8);
                if (!count || count > 32768 || count > payload.size() / sizeof(virtio_video_resource_sg_entry))
                    throw std::invalid_argument("invalid video scatter list length");
                resource.planes.emplace_back();
                for (unsigned i = 0; i < count; ++i) {
                    const auto entry = load<virtio_video_resource_sg_entry>(payload);
                    payload = payload.subspan(sizeof(entry));
                    if (!entry.length || entry.length > maximumResource - total || !mapper ||
                        mapper(entry.addr, entry.length).size() != entry.length)
                        throw std::invalid_argument("video resource lies outside guest memory");
                    total += entry.length;
                    resource.planes.back().push_back({entry.addr, entry.length});
                }
            }
            if (!payload.empty()) throw std::invalid_argument("trailing video resource data");
            stream.resources.emplace(key, std::move(resource));
            return done(reply(id));
        }
        case VIRTIO_VIDEO_CMD_RESOURCE_QUEUE: {
            const auto q = load<virtio_video_resource_queue>(request);
            const auto resource = stream.resources.find(resourceKey(q.queue_type, q.resource_id));
            if (resource == stream.resources.end()) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID));
            if (resource->second.queued) throw std::invalid_argument("video resource is already queued");
            if (q.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                if (!q.data_sizes[0] || q.data_sizes[0] > maximumPacket)
                    throw std::invalid_argument("invalid encoded packet size");
                std::vector<std::uint8_t> packet(q.data_sizes[0]);
                if (!transfer(resource->second.planes[0], packet, false)) throw std::invalid_argument("short encoded video resource");
                stream.draining = stream.eosSent = false;
                stream.decoder->submit(packet, q.timestamp);
                ++stream.packets;
                stream.packetBytes += q.data_sizes[0];
                collect(id, stream);
                auto result = load<virtio_video_resource_queue_resp>(queued(id, q.timestamp, 0));
                result.data_sizes[0] = q.data_sizes[0];
                return done(bytes(result));
            }
            if (stream.outputs.size() >= 32) throw std::invalid_argument("video capture queue is full");
            resource->second.queued = true;
            stream.outputs.push_back({requestId, q.resource_id});
            pump(id, stream);
            return {VhostUserQueueAction::defer, {}};
        }
        case VIRTIO_VIDEO_CMD_STREAM_DRAIN:
            stream.decoder->drain();
            stream.draining = true;
            collect(id, stream);
            return done(reply(id));
        case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
        case VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES: {
            const auto q = load<virtio_video_queue_clear>(request);
            if (!validQueue(q.queue_type)) throw std::invalid_argument("invalid clear queue");
            if (q.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) cancelOutputs(id, stream);
            else {
                const auto cap = std::find_if(capabilities.begin(), capabilities.end(),
                    [&stream](const auto& c) { return wireCodec(c.codec) == stream.coded; });
                stream.decoder = factory(cap->codec);
                stream.frames.clear();
                stream.sourceKnown = stream.draining = stream.eosSent = false;
            }
            if (header.type == VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES)
                std::erase_if(stream.resources, [&q](const auto& r) { return (r.first >> 32) == q.queue_type; });
            return done(reply(id));
        }
        case VIRTIO_VIDEO_CMD_GET_CONTROL: {
            const auto q = load<virtio_video_get_control>(request);
            auto result = reply(id, VIRTIO_VIDEO_RESP_OK_GET_CONTROL);
            std::uint32_t value;
            if (q.control == VIRTIO_VIDEO_CONTROL_PROFILE) value = stream.profile;
            else if (q.control == VIRTIO_VIDEO_CONTROL_LEVEL && stream.coded == VIRTIO_VIDEO_FORMAT_H264)
                value = VIRTIO_VIDEO_LEVEL_H264_5_1;
            else if (q.control == VIRTIO_VIDEO_CONTROL_LEVEL && stream.coded == VIRTIO_VIDEO_FORMAT_HEVC)
                value = VIRTIO_VIDEO_LEVEL_HEVC_6_2;
            else return done(reply(id, VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL));
            const auto data = bytes(virtio_video_control_val_profile{value, {}});
            result.insert(result.end(), data.begin(), data.end());
            return done(std::move(result));
        }
        default: return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION));
        }
    }
};

VirtioVideoDevice::VirtioVideoDevice(std::vector<uml::platform::VideoDecodeCapability> caps, DecoderFactory factory)
    : implementation_(std::make_unique<Implementation>())
{
    if (caps.empty() || !factory) throw std::invalid_argument("video capabilities and decoder factory are required");
    implementation_->capabilities = std::move(caps);
    implementation_->factory = std::move(factory);
    for (const auto& capability : implementation_->capabilities)
        implementation_->log.advertised(codecName(capability.codec), profileName(capability.profile),
            capability.maximumWidth, capability.maximumHeight, capability.bitDepth);
}
VirtioVideoDevice::~VirtioVideoDevice() = default;
const char* VirtioVideoDevice::name() const noexcept { return "Virtio Video decoder"; }
std::uint64_t VirtioVideoDevice::features() const noexcept
{
    return (1 << VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES) | (1 << VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG);
}
std::size_t VirtioVideoDevice::queueCount() const noexcept { return 2; }
std::vector<std::uint8_t> VirtioVideoDevice::configuration() const
{
    return bytes(virtio_video_config{UML_VIRTIO_VIDEO_VERSION, 4096, 512});
}
void VirtioVideoDevice::writeConfiguration(std::uint32_t, std::uint32_t, std::span<const std::uint8_t>) {}
void VirtioVideoDevice::setGuestMapper(GuestMapper mapper) { implementation_->mapper = std::move(mapper); }
VhostUserQueueResult VirtioVideoDevice::processQueue(std::uint32_t index, std::uint64_t id,
    std::span<const std::uint8_t> request, std::size_t capacity)
{
    auto& impl = *implementation_;
    if (index == 1) {
        if (!request.empty() || capacity < sizeof(virtio_video_event))
            return {VhostUserQueueAction::complete, {}};
        if (!impl.events.empty()) {
            auto result = bytes(impl.events.front()); impl.events.pop_front();
            return {VhostUserQueueAction::complete, std::move(result)};
        }
        if (impl.eventBuffers.size() >= uml::gpu::protocol::maximumVirtqueueSize)
            return {VhostUserQueueAction::retry, {}};
        impl.eventBuffers.push_back(id);
        return {VhostUserQueueAction::defer, {}};
    }
    if (index != 0 || request.size() < sizeof(virtio_video_cmd_hdr) || capacity < sizeof(virtio_video_cmd_hdr))
        return {VhostUserQueueAction::complete, {}};
    const auto header = load<virtio_video_cmd_hdr>(request);
    try {
        if (header.type == VIRTIO_VIDEO_CMD_RESOURCE_QUEUE && capacity < sizeof(virtio_video_resource_queue_resp))
            return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
        auto result = impl.command(id, request);
        if (result.response.size() > capacity)
            return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
        return result;
    } catch (const std::invalid_argument&) {
        return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Virtio video stream %u: %s\n", header.stream_id, error.what());
        const auto found = impl.streams.find(header.stream_id);
        if (found != impl.streams.end()) {
            impl.cancelOutputs(header.stream_id, found->second);
            impl.event(header.stream_id, VIRTIO_VIDEO_EVENT_ERROR);
        }
        if (header.type == VIRTIO_VIDEO_CMD_RESOURCE_QUEUE)
            return {VhostUserQueueAction::complete, impl.queued(header.stream_id, 0, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR)};
        return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION)};
    }
}
bool VirtioVideoDevice::takeQueueCompletion(VhostUserQueueCompletion& completion)
{
    auto& pending = implementation_->completions;
    if (pending.empty()) return false;
    completion = std::move(pending.front()); pending.pop_front(); return true;
}
void VirtioVideoDevice::reset() noexcept
{
    auto& impl = *implementation_;
    impl.closeAll("reset");
    impl.streams.clear(); impl.events.clear(); impl.eventBuffers.clear(); impl.completions.clear();
}
} // namespace uml::video
