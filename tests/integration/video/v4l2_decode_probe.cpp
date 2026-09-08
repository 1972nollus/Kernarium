// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void call(int fd, unsigned long request, void* value, const char* name)
{
    int result;
    do { result = ioctl(fd, request, value); } while (result < 0 && errno == EINTR);
    if (result < 0) throw std::runtime_error(std::string(name) + ": " + std::strerror(errno));
}
struct Device {
    int fd;
    explicit Device(const char* path) : fd(open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC))
    { if (fd < 0) throw std::runtime_error(std::string("open decoder: ") + std::strerror(errno)); }
    ~Device() { close(fd); }
};
struct Mapping {
    void* data;
    std::size_t length;
    Mapping(int fd, std::size_t size, off_t offset) : data(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset)), length(size)
    { require(data != MAP_FAILED, "map video buffer failed"); }
    Mapping(Mapping&& other) noexcept : data(other.data), length(other.length) { other.data = MAP_FAILED; }
    ~Mapping() { if (data != MAP_FAILED) munmap(data, length); }
};
std::vector<std::uint8_t> read(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "video fixture missing");
    return {std::istreambuf_iterator<char>(input), {}};
}
constexpr auto output = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr auto capture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
using Buffers = std::vector<std::vector<Mapping>>;
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
void queue(int fd, v4l2_buf_type type, unsigned index, const std::vector<Mapping>& maps, unsigned bytes = 0)
{
    v4l2_plane planes[VIDEO_MAX_PLANES] {};
    v4l2_buffer buffer {};
    buffer.type = type; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
    buffer.length = maps.size(); buffer.m.planes = planes;
    buffer.timestamp.tv_sec = 7; buffer.timestamp.tv_usec = 654321;
    for (unsigned i = 0; i < maps.size(); ++i) planes[i].length = maps[i].length;
    planes[0].bytesused = bytes;
    call(fd, VIDIOC_QBUF, &buffer, "QBUF");
}
void decode(const char* device, const std::string& directory, const char* name, unsigned coded, unsigned raw)
{
    const auto packet = read(directory + "/" + name + ".bin");
    const auto reference = read(directory + "/" + name + ".raw");
    Device owner(device);
    const int fd = owner.fd;
    v4l2_capability cap {};
    call(fd, VIDIOC_QUERYCAP, &cap, "QUERYCAP");
    require((cap.device_caps & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING)) ==
            (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING), "device is not a streaming mem2mem decoder");
    v4l2_event_subscription subscription {};
    subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    call(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription, "SUBSCRIBE_EVENT");
    v4l2_format input {};
    input.type = output;
    input.fmt.pix_mp.width = 128; input.fmt.pix_mp.height = 96;
    input.fmt.pix_mp.pixelformat = coded;
    input.fmt.pix_mp.num_planes = 1;
    input.fmt.pix_mp.plane_fmt[0].sizeimage = packet.size();
    call(fd, VIDIOC_S_FMT, &input, "S_FMT coded");
    require(input.fmt.pix_mp.pixelformat == coded, "coded format was substituted");
    auto inputs = allocate(fd, output, 2);
    require(inputs[0][0].length >= packet.size(), "coded buffer too short");
    std::memcpy(inputs[0][0].data, packet.data(), packet.size());
    auto type = output;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON output");
    queue(fd, output, 0, inputs[0], packet.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool source = false;
    while (!source && std::chrono::steady_clock::now() < deadline) {
        pollfd p {fd, POLLPRI, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll source change failed");
        v4l2_event event {};
        if (ioctl(fd, VIDIOC_DQEVENT, &event) == 0)
            source = event.type == V4L2_EVENT_SOURCE_CHANGE && (event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION);
        else require(errno == EAGAIN, "DQEVENT failed");
    }
    require(source, "decoder source-change timeout");
    v4l2_format format {};
    format.type = capture;
    call(fd, VIDIOC_G_FMT, &format, "G_FMT capture");
    require(format.fmt.pix_mp.width == 128 && format.fmt.pix_mp.height == 96, "decoded dimensions differ");
    format.fmt.pix_mp.pixelformat = raw;
    call(fd, VIDIOC_S_FMT, &format, "S_FMT capture");
    require(format.fmt.pix_mp.pixelformat == raw, "raw format was substituted");
    auto outputs = allocate(fd, capture, 4);
    for (unsigned i = 0; i < outputs.size(); ++i) queue(fd, capture, i, outputs[i]);
    type = capture;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON capture");
    v4l2_decoder_cmd drain {};
    drain.cmd = V4L2_DEC_CMD_STOP;
    call(fd, VIDIOC_DECODER_CMD, &drain, "DECODER_CMD drain");
    bool frame = false, eos = false;
    while (!eos && std::chrono::steady_clock::now() < deadline) {
        pollfd p {fd, POLLIN, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll capture failed");
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = capture; buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
        if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
            require(errno == EAGAIN, "DQBUF capture failed"); continue;
        }
        require(!(buffer.flags & V4L2_BUF_FLAG_ERROR), "capture buffer has decoder error");
        if (buffer.flags & V4L2_BUF_FLAG_LAST) { eos = true; if (!planes[0].bytesused) continue; }
        require(!frame, "decoder returned duplicate frame");
        frame = true;
        require(buffer.timestamp.tv_sec == 7 && buffer.timestamp.tv_usec == 654321, "frame timestamp differs");
        require(buffer.index < outputs.size(), "capture index outside allocation");
        std::vector<std::uint8_t> actual;
        for (unsigned i = 0; i < buffer.length; ++i) {
            require(planes[i].data_offset <= planes[i].bytesused && planes[i].bytesused <= outputs[buffer.index][i].length, "invalid decoded plane extent");
            const auto* data = static_cast<const std::uint8_t*>(outputs[buffer.index][i].data);
            actual.insert(actual.end(), data + planes[i].data_offset, data + planes[i].bytesused);
        }
        require(actual.size() == reference.size(), "decoded plane sizes differ");
        if (raw == V4L2_PIX_FMT_YUV420M) {
            const auto planar = actual;
            for (unsigned i = 0; i < 128 * 96 / 4; ++i) {
                actual[128 * 96 + 2 * i] = planar[128 * 96 + i];
                actual[128 * 96 + 2 * i + 1] = planar[128 * 96 * 5 / 4 + i];
            }
        }
        unsigned maximum = 0;
        const unsigned step = raw == V4L2_PIX_FMT_P010 ? 2 : 1;
        for (std::size_t i = 0; i < actual.size(); i += step) {
            const int got = step == 1 ? actual[i] : (actual[i] | (actual[i + 1] << 8)) >> 6;
            const int wanted = step == 1 ? reference[i] : (reference[i] | (reference[i + 1] << 8)) >> 6;
            maximum = std::max(maximum, unsigned(std::abs(got - wanted)));
        }
        require(maximum <= (coded == V4L2_PIX_FMT_MJPEG ? 4u : 2u), "V4L2 pixels differ from reference");
        std::printf("PASS %s raw=%08x max-error=%u\n", name, raw, maximum);
        std::fflush(stdout);
    }
    require(frame && eos, "capture frame/EOS timeout");
    type = output; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF output");
    type = capture; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF capture");
}
}
int main(int argc, char** argv)
{
    try {
        require(argc >= 2, "usage: v4l2_decode_probe FIXTURE_DIRECTORY [DEVICE]");
        const char* device = argc > 2 ? argv[2] : "/dev/video0";
        std::printf("V4L2 guest page size: %ld\n", sysconf(_SC_PAGESIZE));
        struct Profile { unsigned value; const char* name; unsigned raw; };
        const Profile h264[] = {
            {V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE, "h264-baseline", V4L2_PIX_FMT_NV12M},
            {V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE, "h264-constrained-baseline", V4L2_PIX_FMT_NV12M},
            {V4L2_MPEG_VIDEO_H264_PROFILE_MAIN, "h264-main", V4L2_PIX_FMT_NV12M},
            {V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, "h264-high", V4L2_PIX_FMT_NV12M},
            {V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10, "h264-high10", V4L2_PIX_FMT_P010},
        };
        const Profile hevc[] = {
            {V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN, "hevc-main", V4L2_PIX_FMT_NV12M},
            {V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10, "hevc-main10", V4L2_PIX_FMT_P010},
        };
        Device capabilities(device);
        unsigned tested = 0;
        for (unsigned index = 0;; ++index) {
            v4l2_fmtdesc fmt {};
            fmt.type = output; fmt.index = index;
            if (ioctl(capabilities.fd, VIDIOC_ENUM_FMT, &fmt) < 0) {
                require(errno == EINVAL, "ENUM_FMT failed"); break;
            }
            if (fmt.pixelformat == V4L2_PIX_FMT_MJPEG) {
                decode(device, argv[1], "jpeg", fmt.pixelformat, V4L2_PIX_FMT_NV12M);
                ++tested; continue;
            }
            require(fmt.pixelformat == V4L2_PIX_FMT_H264 || fmt.pixelformat == V4L2_PIX_FMT_HEVC, "advertised codec lacks a fixture");
            const bool avc = fmt.pixelformat == V4L2_PIX_FMT_H264;
            const unsigned id = avc ? V4L2_CID_MPEG_VIDEO_H264_PROFILE : V4L2_CID_MPEG_VIDEO_HEVC_PROFILE;
            v4l2_queryctrl control {}; control.id = id;
            call(capabilities.fd, VIDIOC_QUERYCTRL, &control, "QUERYCTRL profiles");
            require(control.minimum >= 0 && control.maximum <= 32, "invalid profile menu bounds");
            for (int value = control.minimum; value <= control.maximum; ++value) {
                v4l2_querymenu menu {}; menu.id = id; menu.index = value;
                if (ioctl(capabilities.fd, VIDIOC_QUERYMENU, &menu) < 0) {
                    require(errno == EINVAL, "QUERYMENU profile failed"); continue;
                }
                const auto* first = avc ? std::begin(h264) : std::begin(hevc);
                const auto* last = avc ? std::end(h264) : std::end(hevc);
                const auto* profile = std::find_if(first, last, [value](const auto& p) { return p.value == unsigned(value); });
                require(profile != last, "advertised profile lacks a fixture");
                decode(device, argv[1], profile->name, fmt.pixelformat, profile->raw);
                ++tested;
                if (avc && profile->value == V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)
                    decode(device, argv[1], profile->name, fmt.pixelformat, V4L2_PIX_FMT_YUV420M);
            }
        }
        require(tested > 0, "decoder advertised no supported profiles");
        std::puts("UML_V4L2_DECODE: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
}
