// SPDX-License-Identifier: GPL-2.0-or-later
// Qualifies the guest V4L2 stateful encoder (the Virtio video device with ID
// 30) against the host's hardware encoder. Every codec, profile and raw layout
// the device advertises is encoded from generated pictures through MMAP and
// imported dma-buf input, drained, resumed and reconfigured; the coded stream is
// checked for structure, timestamps and flags, and decoded back through the
// guest's stateful decoder (when there is one) to check its quality.
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
int tryCall(int fd, unsigned long request, void* value)
{
    int result;
    do { result = ioctl(fd, request, value); } while (result < 0 && errno == EINTR);
    return result < 0 ? -errno : 0;
}
void call(int fd, unsigned long request, void* value, const char* name)
{
    const int result = tryCall(fd, request, value);
    if (result < 0) throw std::runtime_error(std::string(name) + ": " + std::strerror(-result));
}
Clock::time_point after(int seconds) { return Clock::now() + std::chrono::seconds(seconds); }

constexpr auto output = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr auto capture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

struct Device {
    int fd {-1};
    explicit Device(const std::string& path) : fd(open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC))
    { if (fd < 0) throw std::runtime_error("open " + path + ": " + std::strerror(errno)); }
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device() { if (fd >= 0) close(fd); }
};
struct Mapping {
    void* data;
    std::size_t length;
    Mapping(int fd, std::size_t size, off_t offset)
        : data(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset)), length(size)
    { require(data != MAP_FAILED, "map video buffer failed"); }
    Mapping(Mapping&& other) noexcept : data(other.data), length(other.length) { other.data = MAP_FAILED; }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping() { if (data != MAP_FAILED) munmap(data, length); }
};
struct Prime {
    int fd {-1};
    std::size_t length {};
    Prime(int descriptor, std::size_t size) : fd(descriptor), length(size) {}
    Prime(Prime&& other) noexcept : fd(other.fd), length(other.length) { other.fd = -1; }
    Prime(const Prime&) = delete;
    Prime& operator=(const Prime&) = delete;
    ~Prime() { if (fd >= 0) close(fd); }
};
using Buffers = std::vector<std::vector<Mapping>>;
using Primes = std::vector<std::vector<Prime>>;

Buffers allocate(int fd, v4l2_buf_type type, unsigned count)
{
    v4l2_requestbuffers request {};
    request.type = type; request.memory = V4L2_MEMORY_MMAP; request.count = count;
    call(fd, VIDIOC_REQBUFS, &request, "REQBUFS");
    require(request.count >= count && request.count <= 32, "unexpected video buffer count");
    Buffers result;
    for (unsigned i = 0; i < request.count; ++i) {
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = type; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = i;
        buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
        call(fd, VIDIOC_QUERYBUF, &buffer, "QUERYBUF");
        require(buffer.length > 0 && buffer.length <= VIDEO_MAX_PLANES, "invalid plane count");
        result.emplace_back();
        for (unsigned j = 0; j < buffer.length; ++j)
            result.back().emplace_back(fd, planes[j].length, planes[j].m.mem_offset);
    }
    return result;
}
void releaseBuffers(int fd, v4l2_buf_type type, unsigned memory = V4L2_MEMORY_MMAP)
{
    v4l2_requestbuffers request {};
    request.type = type; request.memory = memory; request.count = 0;
    call(fd, VIDIOC_REQBUFS, &request, "REQBUFS release");
}
Primes exportAll(int fd, v4l2_buf_type type, const Buffers& maps)
{
    Primes result;
    for (unsigned i = 0; i < maps.size(); ++i) {
        result.emplace_back();
        for (unsigned j = 0; j < maps[i].size(); ++j) {
            v4l2_exportbuffer request {};
            request.type = type; request.index = i; request.plane = j;
            request.flags = O_RDWR | O_CLOEXEC;
            call(fd, VIDIOC_EXPBUF, &request, "EXPBUF");
            require(request.fd >= 0, "EXPBUF returned no descriptor");
            result.back().emplace_back(request.fd, maps[i][j].length);
        }
    }
    return result;
}
std::uint64_t microseconds(const timeval& value)
{
    return std::uint64_t(value.tv_sec) * 1000000u + std::uint64_t(value.tv_usec);
}
timeval timestampOf(std::uint64_t us)
{
    timeval value {};
    value.tv_sec = time_t(us / 1000000u);
    value.tv_usec = suseconds_t(us % 1000000u);
    return value;
}
/* The picture n of a stream: its timestamp is never zero and never repeats. */
std::uint64_t timestampOfPicture(unsigned n) { return 40000u * (n + 1u); }

void waitFor(int fd, short events, int milliseconds)
{
    pollfd p {fd, events, 0};
    if (poll(&p, 1, milliseconds) < 0) require(errno == EINTR, "poll failed");
}

/*
 * Generated pictures. Samples stay inside the limited ranges (luma 16-235,
 * chroma 16-240), because the encoder is told nothing else about the range.
 * A picture holds 8-bit values; a ten-bit layout scales them.
 */
struct Picture {
    unsigned width {}, height {};
    std::vector<std::uint8_t> y, u, v;   // u and v are (width + 1) / 2 by (height + 1) / 2
    unsigned chromaWidth() const { return (width + 1) / 2; }
    unsigned chromaHeight() const { return (height + 1) / 2; }
};
Picture makePicture(unsigned width, unsigned height, unsigned n)
{
    Picture picture;
    picture.width = width; picture.height = height;
    picture.y.resize(std::size_t(width) * height);
    picture.u.resize(std::size_t(picture.chromaWidth()) * picture.chromaHeight());
    picture.v.resize(picture.u.size());
    for (unsigned row = 0; row < height; ++row)
        for (unsigned column = 0; column < width; ++column) {
            // A diagonal ramp that moves two samples a frame, with a block in it.
            unsigned value = 48 + (column + 2 * row + 2 * n) % 144;
            if (column / 32 == (n % 4) + 1 && row / 32 == 2) value = 220;
            picture.y[std::size_t(row) * width + column] = std::uint8_t(value);
        }
    for (unsigned row = 0; row < picture.chromaHeight(); ++row)
        for (unsigned column = 0; column < picture.chromaWidth(); ++column) {
            picture.u[std::size_t(row) * picture.chromaWidth() + column] = std::uint8_t(96 + (column + n) % 64);
            picture.v[std::size_t(row) * picture.chromaWidth() + column] = std::uint8_t(96 + (2 * row + n) % 64);
        }
    return picture;
}

struct Layout {
    const char* name;
    unsigned fourcc;
    unsigned planes;       // memory planes
    bool tenBit;
    bool interleaved;      // NV12-like: one chroma plane with U and V interleaved
};
const Layout layouts[] = {
    {"NV12", V4L2_PIX_FMT_NV12, 1, false, true},
    {"NM12", V4L2_PIX_FMT_NV12M, 2, false, true},
    {"YU12", V4L2_PIX_FMT_YUV420, 1, false, false},
    {"YM12", V4L2_PIX_FMT_YUV420M, 3, false, false},
    {"P010", V4L2_PIX_FMT_P010, 1, true, true},
};
const Layout* layoutOf(unsigned fourcc)
{
    for (const auto& layout : layouts) if (layout.fourcc == fourcc) return &layout;
    return nullptr;
}
std::string fourccName(unsigned fourcc)
{
    return std::string(reinterpret_cast<const char*>(&fourcc), 4);
}

/* Writes `picture` into the mapped planes of a buffer negotiated as `format`. */
void pack(const Picture& picture, const Layout& layout, const v4l2_pix_format_mplane& format,
          const std::vector<Mapping>& maps)
{
    require(maps.size() == layout.planes && format.num_planes == layout.planes, "plane count differs from the layout");
    const unsigned sample = layout.tenBit ? 2 : 1;
    const auto chromaWidth = picture.chromaWidth();
    const auto chromaHeight = picture.chromaHeight();
    const auto value = [&](std::uint8_t byte, std::uint8_t* destination) {
        if (!layout.tenBit) { *destination = byte; return; }
        const unsigned word = unsigned(byte) << 8;   // 10-bit value in the high bits of 16
        destination[0] = std::uint8_t(word & 0xff);
        destination[1] = std::uint8_t(word >> 8);
    };
    const auto stride = [&](unsigned plane) { return format.plane_fmt[plane].bytesperline; };
    const auto need = [&](unsigned plane, std::size_t end) {
        require(end <= maps[plane].length, "buffer plane is shorter than the picture");
    };
    auto* base0 = static_cast<std::uint8_t*>(maps[0].data);
    if (layout.planes == 1) {
        // Luma, then the chroma, all in one buffer, each plane after the one before.
        const std::size_t lumaSize = std::size_t(stride(0)) * picture.height;
        for (unsigned row = 0; row < picture.height; ++row)
            for (unsigned column = 0; column < picture.width; ++column) {
                need(0, std::size_t(row) * stride(0) + (column + 1) * sample);
                value(picture.y[std::size_t(row) * picture.width + column], base0 + std::size_t(row) * stride(0) + column * sample);
            }
        if (layout.interleaved) {
            for (unsigned row = 0; row < chromaHeight; ++row)
                for (unsigned column = 0; column < chromaWidth; ++column) {
                    const std::size_t at = lumaSize + std::size_t(row) * stride(0) + std::size_t(column) * 2 * sample;
                    need(0, at + 2 * sample);
                    value(picture.u[std::size_t(row) * chromaWidth + column], base0 + at);
                    value(picture.v[std::size_t(row) * chromaWidth + column], base0 + at + sample);
                }
        } else {
            const std::size_t chromaStride = stride(0) / 2;
            const std::size_t chromaSize = chromaStride * chromaHeight;
            for (unsigned row = 0; row < chromaHeight; ++row)
                for (unsigned column = 0; column < chromaWidth; ++column) {
                    const std::size_t at = lumaSize + row * chromaStride + column;
                    need(0, at + chromaSize + 1);
                    base0[at] = picture.u[std::size_t(row) * chromaWidth + column];
                    base0[at + chromaSize] = picture.v[std::size_t(row) * chromaWidth + column];
                }
        }
        return;
    }
    for (unsigned row = 0; row < picture.height; ++row)
        for (unsigned column = 0; column < picture.width; ++column) {
            need(0, std::size_t(row) * stride(0) + (column + 1) * sample);
            value(picture.y[std::size_t(row) * picture.width + column], base0 + std::size_t(row) * stride(0) + column * sample);
        }
    for (unsigned row = 0; row < chromaHeight; ++row)
        for (unsigned column = 0; column < chromaWidth; ++column) {
            if (layout.interleaved) {
                auto* base1 = static_cast<std::uint8_t*>(maps[1].data);
                const std::size_t at = std::size_t(row) * stride(1) + std::size_t(column) * 2 * sample;
                need(1, at + 2 * sample);
                value(picture.u[std::size_t(row) * chromaWidth + column], base1 + at);
                value(picture.v[std::size_t(row) * chromaWidth + column], base1 + at + sample);
            } else {
                auto* base1 = static_cast<std::uint8_t*>(maps[1].data);
                auto* base2 = static_cast<std::uint8_t*>(maps[2].data);
                need(1, std::size_t(row) * stride(1) + column + 1);
                need(2, std::size_t(row) * stride(2) + column + 1);
                base1[std::size_t(row) * stride(1) + column] = picture.u[std::size_t(row) * chromaWidth + column];
                base2[std::size_t(row) * stride(2) + column] = picture.v[std::size_t(row) * chromaWidth + column];
            }
        }
}

/* Luma PSNR of `got` (rows of `stride` bytes, `bytes` per sample, 8 or 10 bits)
 * against the region of the reference picture at (x, y). */
double lumaPsnr(const Picture& reference, unsigned x, unsigned y, unsigned width, unsigned height,
                const std::uint8_t* got, std::size_t stride, unsigned bytes)
{
    double error = 0;
    for (unsigned row = 0; row < height; ++row)
        for (unsigned column = 0; column < width; ++column) {
            const auto* sample = got + std::size_t(row) * stride + std::size_t(column) * bytes;
            const int actual = bytes == 1 ? sample[0] : ((sample[0] | (sample[1] << 8)) >> 8);
            const int wanted = reference.y[std::size_t(y + row) * reference.width + x + column];
            error += double(actual - wanted) * double(actual - wanted);
        }
    if (error == 0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / (error / (double(width) * height)));
}

/* NAL unit types of an Annex B access unit. */
std::vector<unsigned> nalTypes(const std::vector<std::uint8_t>& bytes, bool hevc)
{
    std::vector<unsigned> types;
    for (std::size_t i = 0; i + 3 < bytes.size(); ++i)
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
            types.push_back(hevc ? (bytes[i + 3] >> 1) & 0x3f : bytes[i + 3] & 0x1f);
            i += 2;
        }
    return types;
}
bool contains(const std::vector<unsigned>& types, unsigned type)
{
    return std::find(types.begin(), types.end(), type) != types.end();
}
bool hasIdr(const std::vector<unsigned>& types, bool hevc)
{
    if (!hevc) return contains(types, 5);
    return std::any_of(types.begin(), types.end(), [](unsigned t) { return t >= 16 && t <= 21; });
}

struct Coded {
    std::uint64_t timestamp {};
    unsigned flags {};
    std::vector<std::uint8_t> bytes;
    bool isConfig() const
    { return !bytes.empty() && !(flags & (V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME | V4L2_BUF_FLAG_BFRAME)); }
};

/*
 * The guest's decoder, used to see what an encoded stream looks like. It takes
 * one access unit at a time, as the Virtio device does, and returns the luma of
 * each picture it produces by the timestamp of the access unit it came from.
 */
struct Decoded {
    unsigned width {}, height {};
    std::vector<std::uint8_t> luma;   // rows of width * bytes
    unsigned bytes {};
};
std::map<std::uint64_t, Decoded> decodeBack(const std::string& path, unsigned codec, const std::vector<std::vector<std::uint8_t>>& units,
                                            const std::vector<std::uint64_t>& stamps, unsigned rawCapture)
{
    Device owner(path);
    const int fd = owner.fd;
    v4l2_event_subscription subscription {};
    subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    call(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription, "SUBSCRIBE_EVENT decoder");
    std::size_t largest = 0;
    for (const auto& unit : units) largest = std::max(largest, unit.size());
    v4l2_format input {};
    input.type = output;
    input.fmt.pix_mp.width = 128; input.fmt.pix_mp.height = 96;
    input.fmt.pix_mp.pixelformat = codec;
    input.fmt.pix_mp.num_planes = 1;
    input.fmt.pix_mp.plane_fmt[0].sizeimage = unsigned(std::max<std::size_t>(largest, 1u << 16));
    call(fd, VIDIOC_S_FMT, &input, "S_FMT decoder coded");
    require(input.fmt.pix_mp.pixelformat == codec, "decoder substituted the coded format");
    auto inputs = allocate(fd, output, 4);
    for (const auto& planes : inputs)
        require(planes[0].length >= largest, "decoder input buffer is shorter than a coded unit");
    auto type = output;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON decoder output");
    std::vector<unsigned> freeInputs;
    for (unsigned i = 0; i < inputs.size(); ++i) freeInputs.push_back(i);
    std::size_t next = 0;
    const auto feed = [&]() {
        while (next < units.size() && !freeInputs.empty()) {
            const auto index = freeInputs.back();
            freeInputs.pop_back();
            std::memcpy(inputs[index][0].data, units[next].data(), units[next].size());
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
            buffer.length = 1; buffer.m.planes = planes;
            buffer.timestamp = timestampOf(stamps[next]);
            planes[0].length = inputs[index][0].length;
            planes[0].bytesused = unsigned(units[next].size());
            call(fd, VIDIOC_QBUF, &buffer, "QBUF decoder coded");
            ++next;
        }
    };
    const auto reclaim = [&]() {
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            const int result = tryCall(fd, VIDIOC_DQBUF, &buffer);
            if (result == -EAGAIN) break;
            require(result == 0, "DQBUF decoder coded failed");
            freeInputs.push_back(buffer.index);
        }
    };
    feed();
    const auto deadline = after(60);
    bool source = false;
    while (!source && Clock::now() < deadline) {
        waitFor(fd, POLLPRI, 100);
        v4l2_event event {};
        const int result = tryCall(fd, VIDIOC_DQEVENT, &event);
        if (result == 0) source = event.type == V4L2_EVENT_SOURCE_CHANGE;
        else require(result == -ENOENT || result == -EAGAIN, "DQEVENT decoder failed");
    }
    require(source, "decoder source-change timeout");
    v4l2_format format {};
    format.type = capture;
    call(fd, VIDIOC_G_FMT, &format, "G_FMT decoder capture");
    format.fmt.pix_mp.pixelformat = rawCapture;
    call(fd, VIDIOC_S_FMT, &format, "S_FMT decoder capture");
    require(format.fmt.pix_mp.pixelformat == rawCapture, "decoder substituted the capture format");
    const auto negotiated = format.fmt.pix_mp;
    auto outputs = allocate(fd, capture, 6);
    for (unsigned i = 0; i < outputs.size(); ++i) {
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = capture; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = i;
        buffer.length = unsigned(outputs[i].size()); buffer.m.planes = planes;
        for (unsigned j = 0; j < outputs[i].size(); ++j) planes[j].length = unsigned(outputs[i][j].length);
        call(fd, VIDIOC_QBUF, &buffer, "QBUF decoder capture");
    }
    type = capture;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON decoder capture");
    std::map<std::uint64_t, Decoded> pictures;
    bool drained = false, last = false;
    const unsigned bytes = rawCapture == V4L2_PIX_FMT_P010 ? 2 : 1;
    while (!last && Clock::now() < deadline) {
        feed();
        reclaim();
        if (next == units.size() && freeInputs.size() == inputs.size() && !drained) {
            v4l2_decoder_cmd drain {};
            drain.cmd = V4L2_DEC_CMD_STOP;
            call(fd, VIDIOC_DECODER_CMD, &drain, "DECODER_CMD drain");
            drained = true;
        }
        waitFor(fd, POLLIN | POLLOUT, 100);
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = capture; buffer.memory = V4L2_MEMORY_MMAP;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            const int result = tryCall(fd, VIDIOC_DQBUF, &buffer);
            if (result == -EAGAIN || result == -EPIPE) break;
            require(result == 0, "DQBUF decoder capture failed");
            require(buffer.index < outputs.size(), "decoder capture index outside allocation");
            if (buffer.flags & V4L2_BUF_FLAG_LAST) last = true;
            if (planes[0].bytesused && !(buffer.flags & V4L2_BUF_FLAG_ERROR)) {
                // The luma rows are kept with their pitch, so the comparison reads them back as they were written.
                Decoded decoded;
                decoded.width = negotiated.plane_fmt[0].bytesperline / bytes;
                decoded.height = negotiated.height;
                decoded.bytes = bytes;
                const std::size_t size = std::size_t(negotiated.plane_fmt[0].bytesperline) * decoded.height;
                require(size <= outputs[buffer.index][0].length, "decoded luma is shorter than its rows");
                const auto* data = static_cast<const std::uint8_t*>(outputs[buffer.index][0].data);
                decoded.luma.assign(data, data + size);
                pictures[microseconds(buffer.timestamp)] = std::move(decoded);
            }
            if (last) break;
            // Hand the picture back to the decoder.
            v4l2_plane again[VIDEO_MAX_PLANES] {};
            v4l2_buffer requeue {};
            requeue.type = capture; requeue.memory = V4L2_MEMORY_MMAP; requeue.index = buffer.index;
            requeue.length = buffer.length; requeue.m.planes = again;
            for (unsigned j = 0; j < buffer.length; ++j) again[j].length = unsigned(outputs[buffer.index][j].length);
            call(fd, VIDIOC_QBUF, &requeue, "QBUF decoder capture again");
        }
    }
    require(last, "decoder did not end the stream");
    type = output; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF decoder output");
    type = capture; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF decoder capture");
    return pictures;
}

int findNode(const char* card, std::string& path)
{
    for (unsigned index = 0; index < 64; ++index) {
        const std::string candidate = "/dev/video" + std::to_string(index);
        const int fd = open(candidate.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        v4l2_capability cap {};
        const bool match = ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
                           std::strcmp(reinterpret_cast<const char*>(cap.card), card) == 0;
        close(fd);
        if (match) { path = candidate; return 0; }
    }
    return -1;
}

bool hasControl(int fd, unsigned id)
{
    v4l2_queryctrl control {};
    control.id = id;
    return tryCall(fd, VIDIOC_QUERYCTRL, &control) == 0 && !(control.flags & V4L2_CTRL_FLAG_DISABLED);
}
int setControl(int fd, unsigned id, int value)
{
    v4l2_control control {};
    control.id = id; control.value = value;
    return tryCall(fd, VIDIOC_S_CTRL, &control);
}
bool menuHas(int fd, unsigned id, int index)
{
    v4l2_querymenu menu {};
    menu.id = id; menu.index = unsigned(index);
    return tryCall(fd, VIDIOC_QUERYMENU, &menu) == 0;
}

struct Plan {
    unsigned coded {};
    unsigned raw {};
    unsigned width {320}, height {240};
    unsigned frames {12};
    unsigned memory {V4L2_MEMORY_MMAP};
    unsigned profileId {};
    int profileValue {};
    bool separateHeader {false};
    int forceAt {-1};
    bool crop {false};
    bool resume {false};
    bool resize {false};
    bool abortFirst {false};   // frames in flight are abandoned by STREAMOFF before the stream under test
};

class Encoder {
public:
    explicit Encoder(const std::string& path) : device(path), fd(device.fd) {}

    void query()
    {
        v4l2_capability cap {};
        call(fd, VIDIOC_QUERYCAP, &cap, "QUERYCAP");
        require((cap.device_caps & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING)) ==
                (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING), "device is not a streaming mem2mem encoder");
        require(std::strcmp(reinterpret_cast<const char*>(cap.driver), "virtio-video") == 0, "unexpected driver name");
    }
    void subscribe()
    {
        v4l2_event_subscription subscription {};
        subscription.type = V4L2_EVENT_EOS;
        call(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription, "SUBSCRIBE_EVENT eos");
    }
    // The coded format first; the raw formats then listed are the ones it takes.
    void setCoded(unsigned coded)
    {
        v4l2_format candidate {};
        candidate.type = capture;
        candidate.fmt.pix_mp.pixelformat = coded;
        candidate.fmt.pix_mp.plane_fmt[0].sizeimage = 1u << 16;
        call(fd, VIDIOC_TRY_FMT, &candidate, "TRY_FMT coded");
        v4l2_format format = candidate;
        call(fd, VIDIOC_S_FMT, &format, "S_FMT coded");
        require(format.fmt.pix_mp.pixelformat == coded, "the coded format was substituted");
        require(format.fmt.pix_mp.num_planes == 1, "a coded format is not one buffer");
        require(format.fmt.pix_mp.plane_fmt[0].sizeimage >= (1u << 15), "coded buffers are too small");
        require(candidate.fmt.pix_mp.plane_fmt[0].sizeimage == format.fmt.pix_mp.plane_fmt[0].sizeimage,
                "TRY_FMT and S_FMT disagree on the coded buffer size");
        capFormat = format.fmt.pix_mp;
    }
    bool rawListed(unsigned raw)
    {
        for (unsigned index = 0;; ++index) {
            v4l2_fmtdesc fmt {};
            fmt.type = output; fmt.index = index;
            const int result = tryCall(fd, VIDIOC_ENUM_FMT, &fmt);
            if (result < 0) { require(result == -EINVAL, "ENUM_FMT raw failed"); return false; }
            if (fmt.pixelformat == raw) return true;
        }
    }
    void setRaw(unsigned raw, unsigned width, unsigned height)
    {
        const auto* layout = layoutOf(raw);
        require(layout, "a raw format without a layout");
        v4l2_format candidate {};
        candidate.type = output;
        candidate.fmt.pix_mp.width = width; candidate.fmt.pix_mp.height = height;
        candidate.fmt.pix_mp.pixelformat = raw;
        call(fd, VIDIOC_TRY_FMT, &candidate, "TRY_FMT raw");
        v4l2_format format = candidate;
        call(fd, VIDIOC_S_FMT, &format, "S_FMT raw");
        const auto& negotiated = format.fmt.pix_mp;
        require(negotiated.pixelformat == raw, "the raw format was substituted: " + fourccName(negotiated.pixelformat));
        require(negotiated.width == width && negotiated.height == height, "the raw size was changed");
        require(negotiated.num_planes == layout->planes, "the raw plane count differs from its format");
        for (unsigned i = 0; i < negotiated.num_planes; ++i) {
            require(negotiated.plane_fmt[i].bytesperline >= (i == 0 || layout->interleaved ? width : width / 2) * (layout->tenBit ? 2u : 1u),
                    "a raw line is shorter than its row");
            require(negotiated.plane_fmt[i].sizeimage >= negotiated.plane_fmt[i].bytesperline, "a raw plane is shorter than a row");
            require(candidate.fmt.pix_mp.plane_fmt[i].bytesperline == negotiated.plane_fmt[i].bytesperline &&
                    candidate.fmt.pix_mp.plane_fmt[i].sizeimage == negotiated.plane_fmt[i].sizeimage,
                    "TRY_FMT and S_FMT disagree on a raw plane");
        }
        v4l2_format current {};
        current.type = output;
        call(fd, VIDIOC_G_FMT, &current, "G_FMT raw");
        require(current.fmt.pix_mp.pixelformat == raw && current.fmt.pix_mp.width == width, "G_FMT differs from S_FMT");
        outFormat = negotiated;
        outLayout = layout;
        visible = {0, 0, width, height};
        refreshCoded();
    }
    void refreshCoded()
    {
        v4l2_format format {};
        format.type = capture;
        call(fd, VIDIOC_G_FMT, &format, "G_FMT coded");
        capFormat = format.fmt.pix_mp;
    }
    void setCrop(int left, int top, unsigned width, unsigned height)
    {
        v4l2_selection selection {};
        selection.type = output; selection.target = V4L2_SEL_TGT_CROP_BOUNDS;
        call(fd, VIDIOC_G_SELECTION, &selection, "G_SELECTION bounds");
        require(selection.r.width == outFormat.width && selection.r.height == outFormat.height, "the crop bounds are not the frame");
        selection = {};
        selection.type = output; selection.target = V4L2_SEL_TGT_CROP;
        selection.r.left = left; selection.r.top = top; selection.r.width = width; selection.r.height = height;
        call(fd, VIDIOC_S_SELECTION, &selection, "S_SELECTION crop");
        require(selection.r.left == left && selection.r.top == top && selection.r.width == width && selection.r.height == height,
                "the visible rectangle was adjusted");
        visible = selection.r;
        refreshCoded();
        require(capFormat.width == width && capFormat.height == height, "the coded size does not follow the visible rectangle");
    }
    void setInterval(unsigned numerator, unsigned denominator)
    {
        v4l2_streamparm parm {};
        parm.type = output;
        parm.parm.output.timeperframe.numerator = numerator;
        parm.parm.output.timeperframe.denominator = denominator;
        call(fd, VIDIOC_S_PARM, &parm, "S_PARM");
        require(parm.parm.output.timeperframe.numerator == numerator && parm.parm.output.timeperframe.denominator == denominator,
                "the frame interval was adjusted");
        v4l2_streamparm coded {};
        coded.type = capture;
        call(fd, VIDIOC_G_PARM, &coded, "G_PARM coded");
        require(coded.parm.capture.timeperframe.numerator == numerator && coded.parm.capture.timeperframe.denominator == denominator,
                "the coded frame interval does not follow the raw one");
    }
    void allocateBuffers(unsigned memory_)
    {
        memory = memory_;
        capMaps = allocate(fd, capture, 4);
        outMaps = allocate(fd, output, 4);
        outPrimes.clear();
        if (memory == V4L2_MEMORY_DMABUF) {
            // The raw frames live in buffers this queue exported and now imports.
            auto primes = exportAll(fd, output, outMaps);
            outMaps.clear();
            releaseBuffers(fd, output);
            v4l2_requestbuffers request {};
            request.type = output; request.memory = V4L2_MEMORY_DMABUF; request.count = unsigned(primes.size());
            call(fd, VIDIOC_REQBUFS, &request, "REQBUFS dmabuf");
            require(request.count == primes.size(), "imported buffer count differs");
            outPrimes = std::move(primes);
            for (const auto& planes : outPrimes) {
                outMaps.emplace_back();
                for (const auto& plane : planes) outMaps.back().emplace_back(plane.fd, plane.length, 0);
            }
        }
        freeOut.clear();
        for (unsigned i = 0; i < outMaps.size(); ++i) freeOut.push_back(i);
    }
    void start()
    {
        for (unsigned i = 0; i < capMaps.size(); ++i) queueCoded(i);
        auto type = output;
        call(fd, VIDIOC_STREAMON, &type, "STREAMON output");
        type = capture;
        call(fd, VIDIOC_STREAMON, &type, "STREAMON capture");
    }
    // STREAMOFF on both queues without a drain, then streaming again on the same buffers.
    void restart()
    {
        auto type = output;
        call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF output");
        type = capture;
        call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF capture");
        coded.clear();
        seenLast = false;
        lastIndex = -1;
        eosEvents = 0;
        freeOut.clear();
        for (unsigned i = 0; i < outMaps.size(); ++i) freeOut.push_back(i);
        for (unsigned i = 0; i < capMaps.size(); ++i) queueCoded(i);
        type = output;
        call(fd, VIDIOC_STREAMON, &type, "STREAMON output again");
        type = capture;
        call(fd, VIDIOC_STREAMON, &type, "STREAMON capture again");
    }
    void stop()
    {
        auto type = output;
        call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF output");
        type = capture;
        call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF capture");
        capMaps.clear(); outMaps.clear(); outPrimes.clear(); freeOut.clear();
        releaseBuffers(fd, capture);
        releaseBuffers(fd, output, memory);
        seenLast = false;
        lastIndex = -1;
        eosEvents = 0;
        coded.clear();
    }
    // Queues one picture, waiting for a free raw buffer; the pictures are kept by timestamp.
    void submit(unsigned n, const Picture& picture)
    {
        const auto deadline = after(30);
        while (freeOut.empty()) {
            require(Clock::now() < deadline, "no raw buffer came back");
            pump(100);
        }
        const auto index = freeOut.back();
        freeOut.pop_back();
        pack(picture, *outLayout, outFormat, outMaps[index]);
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = output; buffer.memory = memory; buffer.index = index;
        buffer.length = outFormat.num_planes; buffer.m.planes = planes;
        buffer.timestamp = timestampOf(timestampOfPicture(n));
        for (unsigned i = 0; i < outFormat.num_planes; ++i) {
            planes[i].length = unsigned(outMaps[index][i].length);
            planes[i].bytesused = outFormat.plane_fmt[i].sizeimage;
            if (memory == V4L2_MEMORY_DMABUF) planes[i].m.fd = outPrimes[index][i].fd;
        }
        call(fd, VIDIOC_QBUF, &buffer, "QBUF raw");
    }
    void pump(int milliseconds)
    {
        waitFor(fd, POLLIN | POLLOUT | POLLPRI, milliseconds);
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = output; buffer.memory = memory;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            const int result = tryCall(fd, VIDIOC_DQBUF, &buffer);
            if (result == -EAGAIN) break;
            require(result == 0, "DQBUF raw failed");
            require(!(buffer.flags & V4L2_BUF_FLAG_ERROR), "a raw buffer came back with an error");
            freeOut.push_back(buffer.index);
        }
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = capture; buffer.memory = V4L2_MEMORY_MMAP;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            const int result = tryCall(fd, VIDIOC_DQBUF, &buffer);
            if (result == -EAGAIN) break;
            if (result == -EPIPE) { require(seenLast, "EPIPE before the last buffer"); break; }
            require(result == 0, "DQBUF coded failed");
            require(buffer.index < capMaps.size(), "coded index outside allocation");
            require(!(buffer.flags & V4L2_BUF_FLAG_ERROR), "a coded buffer came back with an error");
            require(planes[0].bytesused <= capMaps[buffer.index][0].length, "a coded buffer claims more bytes than it holds");
            Coded item;
            item.timestamp = microseconds(buffer.timestamp);
            item.flags = buffer.flags;
            const auto* data = static_cast<const std::uint8_t*>(capMaps[buffer.index][0].data);
            item.bytes.assign(data, data + planes[0].bytesused);
            const bool last = buffer.flags & V4L2_BUF_FLAG_LAST;
            if (!item.bytes.empty() || !last) coded.push_back(std::move(item));
            if (last) { seenLast = true; lastIndex = int(buffer.index); continue; }
            queueCoded(buffer.index);
        }
        v4l2_event event {};
        while (tryCall(fd, VIDIOC_DQEVENT, &event) == 0)
            if (event.type == V4L2_EVENT_EOS) ++eosEvents;
    }
    // The drain sequence: stop, collect every coded buffer up to the last, and see no more.
    void drain()
    {
        v4l2_encoder_cmd command {};
        command.cmd = V4L2_ENC_CMD_STOP;
        call(fd, VIDIOC_ENCODER_CMD, &command, "ENCODER_CMD stop");
        const auto deadline = after(60);
        while (!seenLast) {
            require(Clock::now() < deadline, "the drain did not end with a last buffer");
            pump(100);
        }
        pump(10);
        require(eosEvents > 0, "no end-of-stream event");
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = capture; buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
        require(tryCall(fd, VIDIOC_DQBUF, &buffer) == -EPIPE, "a buffer followed the last one");
        // Every raw buffer is back with the client once the encoder is drained.
        while (freeOut.size() < outMaps.size()) {
            require(Clock::now() < deadline, "raw buffers were not returned by the drain");
            pump(100);
        }
    }
    void resume()
    {
        v4l2_encoder_cmd command {};
        command.cmd = V4L2_ENC_CMD_START;
        call(fd, VIDIOC_ENCODER_CMD, &command, "ENCODER_CMD start");
        // The buffer that carried the last flag goes back to the encoder only now.
        if (lastIndex >= 0) queueCoded(unsigned(lastIndex));
        lastIndex = -1;
        seenLast = false;
        eosEvents = 0;
    }
    std::vector<Coded> take()
    {
        std::vector<Coded> result;
        result.swap(coded);
        return result;
    }

    Device device;
    int fd;
    v4l2_pix_format_mplane outFormat {}, capFormat {};
    const Layout* outLayout {nullptr};
    v4l2_rect visible {};
    unsigned memory {V4L2_MEMORY_MMAP};
    Buffers outMaps, capMaps;
    Primes outPrimes;
    std::vector<unsigned> freeOut;
    std::vector<Coded> coded;
    int lastIndex {-1};
    bool seenLast {false};
    unsigned eosEvents {};

private:
    void queueCoded(unsigned index)
    {
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = capture; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
        buffer.length = 1; buffer.m.planes = planes;
        planes[0].length = unsigned(capMaps[index][0].length);
        call(fd, VIDIOC_QBUF, &buffer, "QBUF coded");
    }
};

struct Run {
    std::string encoder;
    std::string decoder;   // empty when the guest has no decoder
};

/* Checks one drained stretch of coded buffers against the pictures sent for it. */
double verify(const Run& run, const Plan& plan, Encoder& encoder, const std::vector<Coded>& coded, const std::vector<unsigned>& pictures,
              unsigned width, unsigned height, bool firstIsKey, const char* label)
{
    const bool hevc = plan.coded == V4L2_PIX_FMT_HEVC;
    const bool jpeg = plan.coded == V4L2_PIX_FMT_JPEG;
    std::vector<const Coded*> units;
    const Coded* config = nullptr;
    for (const auto& item : coded) {
        require(!item.bytes.empty(), "an empty coded buffer was returned before the last");
        if (item.isConfig()) {
            require(plan.separateHeader && !config && units.empty(), "unexpected parameter-set buffer");
            const auto types = nalTypes(item.bytes, hevc);
            require(contains(types, hevc ? 33 : 7) && contains(types, hevc ? 34 : 8), "the parameter-set buffer lacks its sets");
            require(!hasIdr(types, hevc), "the parameter-set buffer holds a picture");
            config = &item;
            continue;
        }
        units.push_back(&item);
    }
    if (plan.separateHeader && !jpeg) require(config, "no separate parameter-set buffer");
    require(units.size() == pictures.size(),
            std::string(label) + ": " + std::to_string(units.size()) + " coded pictures for " + std::to_string(pictures.size()) + " raw frames");
    // Every raw frame comes back once under its own timestamp. A stream without
    // B-frames also keeps the order, so a reorder here is the device's doing.
    std::map<std::uint64_t, unsigned> byStamp;
    for (const auto picture : pictures) byStamp[timestampOfPicture(picture)] = picture;
    const auto pictureOf = byStamp;
    for (std::size_t i = 0; i < units.size(); ++i) {
        const auto& unit = *units[i];
        const auto known = byStamp.find(unit.timestamp);
        require(known != byStamp.end(), "a coded picture carries a timestamp that was never queued");
        require(byStamp.erase(unit.timestamp) == 1, "a timestamp came back twice");
        if (jpeg) {
            require(unit.bytes.size() > 4 && unit.bytes[0] == 0xff && unit.bytes[1] == 0xd8 &&
                    unit.bytes[unit.bytes.size() - 2] == 0xff && unit.bytes.back() == 0xd9, "not a complete JPEG image");
            continue;
        }
        const auto types = nalTypes(unit.bytes, hevc);
        const bool key = unit.flags & V4L2_BUF_FLAG_KEYFRAME;
        require(key == hasIdr(types, hevc), "the key-frame flag disagrees with the NAL units");
        if (i == 0 && firstIsKey) {
            require(key, "the first picture is not a key frame");
            if (!plan.separateHeader)
                require(contains(types, hevc ? 33 : 7) && contains(types, hevc ? 34 : 8), "the first picture lacks parameter sets");
        }
        if (plan.forceAt >= 0 && unit.timestamp == timestampOfPicture(unsigned(plan.forceAt)))
            require(key, "a forced key frame is not a key frame");
    }
    require(byStamp.empty(), "a queued picture was never returned");
    if (run.decoder.empty()) return -1;
    std::vector<std::vector<std::uint8_t>> aus;
    std::vector<std::uint64_t> stamps;
    for (std::size_t i = 0; i < units.size(); ++i) {
        auto bytes = units[i]->bytes;
        if (i == 0 && config) bytes.insert(bytes.begin(), config->bytes.begin(), config->bytes.end());
        aus.push_back(std::move(bytes));
        stamps.push_back(units[i]->timestamp);
    }
    const bool tenBit = layoutOf(plan.raw)->tenBit;
    const unsigned decoderCoded = jpeg ? V4L2_PIX_FMT_MJPEG : plan.coded;
    const auto decoded = decodeBack(run.decoder, decoderCoded, aus, stamps, tenBit ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12M);
    require(decoded.size() == units.size(), std::string(label) + ": the decoder returned " + std::to_string(decoded.size()) + " pictures");
    double sum = 0, worst = 99;
    for (std::size_t i = 0; i < units.size(); ++i) {
        const auto found = decoded.find(units[i]->timestamp);
        require(found != decoded.end(), "a coded picture was not decoded");
        const auto& picture = found->second;
        // The decoder may report the coded rectangle; the visible picture is its upper left corner.
        require(picture.height >= height, std::string(label) + ": decoded height is shorter than the picture");
        const unsigned x = unsigned(encoder.visible.left), y = unsigned(encoder.visible.top);
        const auto reference = makePicture(encoder.outFormat.width, encoder.outFormat.height, pictureOf.at(units[i]->timestamp));
        require(picture.width >= width, "decoded pitch is shorter than the picture");
        const double psnr = lumaPsnr(reference, x, y, width, height, picture.luma.data(), std::size_t(picture.width) * picture.bytes, picture.bytes);
        sum += psnr;
        worst = std::min(worst, psnr);
    }
    const double mean = sum / double(units.size());
    require(mean >= 30.0 && worst >= 24.0,
            std::string(label) + ": decoded quality is too low, mean " + std::to_string(mean) + " dB worst " + std::to_string(worst) + " dB");
    return mean;
}

void runPlan(const Run& run, const Plan& plan)
{
    Encoder encoder(run.encoder);
    encoder.query();
    encoder.subscribe();
    encoder.setCoded(plan.coded);
    require(encoder.rawListed(plan.raw), "the raw format is not listed for the coded format");
    encoder.setRaw(plan.raw, plan.width, plan.height);
    encoder.setInterval(1, 30);
    unsigned width = plan.width, height = plan.height;
    if (plan.crop) {
        encoder.setCrop(8, 8, plan.width - 24, plan.height - 16);
        width = plan.width - 24; height = plan.height - 16;
    }
    require(encoder.capFormat.width == width && encoder.capFormat.height == height, "the coded size differs from the visible size");
    if (plan.profileId) require(setControl(encoder.fd, plan.profileId, plan.profileValue) == 0, "setting the profile failed");
    if (hasControl(encoder.fd, V4L2_CID_MPEG_VIDEO_BITRATE))
        require(setControl(encoder.fd, V4L2_CID_MPEG_VIDEO_BITRATE, 2000000) == 0, "setting the bitrate failed");
    if (plan.separateHeader)
        require(setControl(encoder.fd, V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE) == 0,
                "setting the header mode failed");
    encoder.allocateBuffers(plan.memory);
    encoder.start();
    // A control the encoder cannot change while it encodes is refused with EBUSY; a dynamic one is not.
    if (hasControl(encoder.fd, V4L2_CID_MPEG_VIDEO_BITRATE_MODE)) {
        v4l2_control current {};
        current.id = V4L2_CID_MPEG_VIDEO_BITRATE_MODE;
        call(encoder.fd, VIDIOC_G_CTRL, &current, "G_CTRL bitrate mode");
        require(setControl(encoder.fd, V4L2_CID_MPEG_VIDEO_BITRATE_MODE, current.value) == -EBUSY,
                "a control that cannot change while encoding was not refused");
    }
    if (plan.abortFirst) {
        // Whatever the first frames produce is dropped with the queues, and the
        // stream that follows starts again with a key frame.
        for (unsigned n = 0; n < 6; ++n) encoder.submit(1000 + n, makePicture(plan.width, plan.height, 1000 + n));
        encoder.restart();
    }
    std::vector<unsigned> pictures;
    for (unsigned n = 0; n < plan.frames; ++n) {
        if (plan.forceAt == int(n))
            require(setControl(encoder.fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1) == 0, "forcing a key frame failed");
        if (n == plan.frames / 2 && hasControl(encoder.fd, V4L2_CID_MPEG_VIDEO_BITRATE))
            require(setControl(encoder.fd, V4L2_CID_MPEG_VIDEO_BITRATE, 3000000) == 0, "changing the bitrate while encoding failed");
        encoder.submit(n, makePicture(plan.width, plan.height, n));
        pictures.push_back(n);
    }
    encoder.drain();
    auto coded = encoder.take();
    auto mean = verify(run, plan, encoder, coded, pictures, width, height, true, "stream");
    unsigned total = 0;
    for (const auto& item : coded) total += unsigned(item.bytes.size());
    std::printf("PASS encode %s raw=%s memory=%s frames=%u bytes=%u psnr=%.1f profile=%d\n", fourccName(plan.coded).c_str(),
                fourccName(plan.raw).c_str(), plan.memory == V4L2_MEMORY_DMABUF ? "dmabuf" : "mmap", plan.frames, total, mean, plan.profileValue);
    std::fflush(stdout);
    if (plan.resume) {
        // The same encoder goes on from before the drain, without a new sequence.
        encoder.resume();
        pictures.clear();
        for (unsigned n = plan.frames; n < plan.frames + 4; ++n) {
            encoder.submit(n, makePicture(plan.width, plan.height, n));
            pictures.push_back(n);
        }
        encoder.drain();
        coded = encoder.take();
        require(coded.size() == pictures.size(), "the resumed stream did not return every picture");
        for (std::size_t i = 0; i < coded.size(); ++i)
            require(coded[i].timestamp == timestampOfPicture(pictures[i]), "the resumed stream lost a timestamp");
        std::printf("PASS resume %s frames=%zu\n", fourccName(plan.coded).c_str(), coded.size());
        std::fflush(stdout);
        // A drain with nothing queued returns the last buffer at once.
        encoder.resume();
        encoder.drain();
        require(encoder.take().empty(), "an empty drain returned a picture");
    }
    if (plan.resize) {
        encoder.stop();
        const unsigned newWidth = 256, newHeight = 192;
        encoder.setRaw(plan.raw, newWidth, newHeight);
        require(encoder.capFormat.width == newWidth && encoder.capFormat.height == newHeight, "the coded size did not follow the new raw size");
        encoder.allocateBuffers(plan.memory);
        encoder.start();
        pictures.clear();
        for (unsigned n = 0; n < 6; ++n) {
            encoder.submit(100 + n, makePicture(newWidth, newHeight, 100 + n));
            pictures.push_back(100 + n);
        }
        encoder.drain();
        coded = encoder.take();
        Plan second = plan;
        second.forceAt = -1;
        second.crop = false;
        mean = verify(run, second, encoder, coded, pictures, newWidth, newHeight, true, "resized stream");
        std::printf("PASS resize %s %ux%u psnr=%.1f\n", fourccName(plan.coded).c_str(), newWidth, newHeight, mean);
        std::fflush(stdout);
    }
    encoder.stop();
}

/* What a client learns before it builds an encoder: formats, sizes and intervals. */
std::vector<unsigned> discover(const std::string& path)
{
    Device owner(path);
    const int fd = owner.fd;
    v4l2_capability cap {};
    call(fd, VIDIOC_QUERYCAP, &cap, "QUERYCAP");
    require(std::strcmp(reinterpret_cast<const char*>(cap.card), "stateful-encoder") == 0, "the node is not the encoder");
    std::vector<unsigned> codecs;
    for (unsigned index = 0;; ++index) {
        v4l2_fmtdesc fmt {};
        fmt.type = capture; fmt.index = index;
        const int result = tryCall(fd, VIDIOC_ENUM_FMT, &fmt);
        if (result < 0) { require(result == -EINVAL, "ENUM_FMT coded failed"); break; }
        require(fmt.flags & V4L2_FMT_FLAG_COMPRESSED, "a coded format is not marked compressed");
        require(fmt.flags & V4L2_FMT_FLAG_ENC_CAP_FRAME_INTERVAL, "a coded format does not offer its own frame interval");
        v4l2_frmsizeenum sizes {};
        sizes.index = 0; sizes.pixel_format = fmt.pixelformat;
        call(fd, VIDIOC_ENUM_FRAMESIZES, &sizes, "ENUM_FRAMESIZES");
        require(sizes.type == V4L2_FRMSIZE_TYPE_STEPWISE, "coded frame sizes are not stepwise");
        require(sizes.stepwise.min_width <= 128 && sizes.stepwise.max_width >= 1280 && sizes.stepwise.step_width >= 1,
                "the advertised coded sizes are unusable");
        v4l2_frmivalenum intervals {};
        intervals.index = 0; intervals.pixel_format = fmt.pixelformat; intervals.width = 320; intervals.height = 240;
        call(fd, VIDIOC_ENUM_FRAMEINTERVALS, &intervals, "ENUM_FRAMEINTERVALS");
        require(intervals.type != 0, "no frame interval is advertised");
        std::printf("encoder format %s\n", fourccName(fmt.pixelformat).c_str());
        codecs.push_back(fmt.pixelformat);
    }
    require(!codecs.empty(), "the encoder advertised no coded formats");
    // The raw formats follow the coded one, and every one of them has sizes.
    for (const auto coded : codecs) {
        v4l2_format format {};
        format.type = capture;
        format.fmt.pix_mp.pixelformat = coded;
        format.fmt.pix_mp.plane_fmt[0].sizeimage = 1u << 16;
        call(fd, VIDIOC_S_FMT, &format, "S_FMT coded");
        unsigned raws = 0;
        for (unsigned index = 0;; ++index) {
            v4l2_fmtdesc fmt {};
            fmt.type = output; fmt.index = index;
            const int result = tryCall(fd, VIDIOC_ENUM_FMT, &fmt);
            if (result < 0) { require(result == -EINVAL, "ENUM_FMT raw failed"); break; }
            v4l2_frmsizeenum sizes {};
            sizes.index = 0; sizes.pixel_format = fmt.pixelformat;
            call(fd, VIDIOC_ENUM_FRAMESIZES, &sizes, "ENUM_FRAMESIZES raw");
            std::printf("encoder %s accepts %s\n", fourccName(coded).c_str(), fourccName(fmt.pixelformat).c_str());
            ++raws;
        }
        require(raws > 0, "a coded format accepts no raw format");
    }
    require(hasControl(fd, V4L2_CID_MIN_BUFFERS_FOR_OUTPUT), "the minimum buffer count is not published");
    return codecs;
}
/* A plan that fails does not stop the others, so one run shows every defect. */
std::vector<std::string> failures;
bool attempt(const Run& run, const Plan& plan, const char* what)
{
    try {
        runPlan(run, plan);
        return true;
    } catch (const std::exception& error) {
        const std::string message = std::string("FAIL ") + what + " " + fourccName(plan.coded) + " raw=" + fourccName(plan.raw) +
            " profile=" + std::to_string(plan.profileValue) + ": " + error.what();
        std::printf("%s\n", message.c_str());
        std::fflush(stdout);
        failures.push_back(message);
        return false;
    }
}
}

int main(int argc, char** argv)
{
    try {
        std::printf("V4L2 guest page size: %ld\n", sysconf(_SC_PAGESIZE));
        Run run;
        if (argc > 1) run.encoder = argv[1];
        else require(findNode("stateful-encoder", run.encoder) == 0, "no stateful-encoder node");
        if (argc > 2) run.decoder = argv[2];
        else if (findNode("stateful-decoder", run.decoder) != 0) run.decoder.clear();
        std::printf("encoder %s decoder %s\n", run.encoder.c_str(), run.decoder.empty() ? "(none)" : run.decoder.c_str());
        const auto codecs = discover(run.encoder);
        unsigned encoded = 0;
        bool resumed = false, resized = false;
        for (const auto coded : codecs) {
            Encoder probe(run.encoder);
            probe.setCoded(coded);
            const bool avc = coded == V4L2_PIX_FMT_H264, hevc = coded == V4L2_PIX_FMT_HEVC;
            const unsigned profileId = avc ? V4L2_CID_MPEG_VIDEO_H264_PROFILE : hevc ? V4L2_CID_MPEG_VIDEO_HEVC_PROFILE : 0;
            std::vector<int> profiles;
            if (profileId) {
                v4l2_queryctrl control {};
                control.id = profileId;
                call(probe.fd, VIDIOC_QUERYCTRL, &control, "QUERYCTRL profile");
                require(control.minimum >= 0 && control.maximum <= 32, "invalid profile menu bounds");
                for (int value = control.minimum; value <= control.maximum; ++value)
                    if (menuHas(probe.fd, profileId, value)) profiles.push_back(value);
                require(!profiles.empty(), "a video codec lists no profile");
            } else {
                profiles.push_back(0);
            }
            const bool eightBit = probe.rawListed(V4L2_PIX_FMT_NV12M) || probe.rawListed(V4L2_PIX_FMT_NV12);
            const unsigned eight = probe.rawListed(V4L2_PIX_FMT_NV12M) ? V4L2_PIX_FMT_NV12M : V4L2_PIX_FMT_NV12;
            require(eightBit, "the encoder accepts no 8-bit 4:2:0 format");
            bool variants = false;
            for (const int profile : profiles) {
                // Ten-bit profiles take P010 and the others take 8 bits.
                const bool tenBit = hevc ? profile == V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10 : avc ? profile == V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10 : false;
                const bool wideChroma = avc && (profile == V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422 ||
                                                profile == V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE);
                // A wide-chroma profile needs raw formats this probe does not generate, and a
                // still-picture profile holds a single picture.
                if (wideChroma || (hevc && profile == V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE)) continue;
                Plan plan;
                plan.coded = coded; plan.profileId = profileId; plan.profileValue = profile;
                plan.raw = tenBit ? V4L2_PIX_FMT_P010 : eight;
                if (tenBit && !probe.rawListed(V4L2_PIX_FMT_P010)) continue;
                plan.forceAt = 6;
                plan.resume = !resumed;
                plan.resize = !resized && (avc || hevc);
                if (attempt(run, plan, "stream")) ++encoded;
                if (plan.resume) resumed = true;
                if (plan.resize) resized = true;
                if (variants) continue;
                variants = true;
                // The other ways a client hands over its frames, once per codec.
                Plan imported = plan;
                imported.memory = V4L2_MEMORY_DMABUF; imported.forceAt = -1; imported.resume = false; imported.resize = false;
                attempt(run, imported, "dmabuf");
                for (const unsigned other : {unsigned(V4L2_PIX_FMT_NV12), unsigned(V4L2_PIX_FMT_NV12M), unsigned(V4L2_PIX_FMT_YUV420),
                                             unsigned(V4L2_PIX_FMT_YUV420M)}) {
                    if (tenBit || other == plan.raw || !probe.rawListed(other)) continue;
                    Plan layout = plan;
                    layout.raw = other; layout.forceAt = -1; layout.resume = false; layout.resize = false;
                    attempt(run, layout, "layout");
                }
                Plan aborted = plan;
                aborted.abortFirst = true; aborted.forceAt = -1; aborted.resume = false; aborted.resize = false;
                attempt(run, aborted, "restart");
                Plan cropped = plan;
                cropped.crop = true; cropped.forceAt = -1; cropped.resume = false; cropped.resize = false;
                attempt(run, cropped, "crop");
                if (profileId && menuHas(probe.fd, V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE)) {
                    Plan header = plan;
                    header.separateHeader = true; header.forceAt = -1; header.resume = false; header.resize = false;
                    attempt(run, header, "header");
                }
            }
        }
        require(encoded > 0, "the encoder advertised no supported profiles");
        if (!failures.empty()) {
            for (const auto& message : failures) std::fprintf(stderr, "%s\n", message.c_str());
            return 1;
        }
        std::puts("UML_V4L2_ENCODE: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
