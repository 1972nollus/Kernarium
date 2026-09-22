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
#include <utility>
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
struct Prime {
    int fd {-1};
    std::size_t length {};
    Prime() = default;
    Prime(int descriptor, std::size_t size) : fd(descriptor), length(size) {}
    Prime(Prime&& other) noexcept : fd(other.fd), length(other.length) { other.fd = -1; }
    Prime& operator=(Prime&&) = delete;
    Prime(const Prime&) = delete;
    ~Prime() { if (fd >= 0) close(fd); }
};
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
/* Exports the queue's own buffers as dma-bufs and hands the queue back
 * empty. vb2 orphans buffers an exported dma-buf still holds, so the
 * descriptors keep their memory alive after the mapped allocation is gone
 * and the same queue can import them again. */
Primes exportAndRelease(int fd, v4l2_buf_type type, const Buffers& maps)
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
void release(int fd, v4l2_buf_type type)
{
    v4l2_requestbuffers request {};
    request.type = type; request.memory = V4L2_MEMORY_MMAP; request.count = 0;
    call(fd, VIDIOC_REQBUFS, &request, "REQBUFS release");
}
unsigned import(int fd, v4l2_buf_type type, unsigned count)
{
    v4l2_requestbuffers request {};
    request.type = type; request.memory = V4L2_MEMORY_DMABUF; request.count = count;
    call(fd, VIDIOC_REQBUFS, &request, "REQBUFS dmabuf");
    require(request.count >= count && request.count <= 32, "unexpected imported buffer count");
    return request.count;
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
void queueImported(int fd, v4l2_buf_type type, unsigned index, const std::vector<Prime>& primes)
{
    v4l2_plane planes[VIDEO_MAX_PLANES] {};
    v4l2_buffer buffer {};
    buffer.type = type; buffer.memory = V4L2_MEMORY_DMABUF; buffer.index = index;
    buffer.length = primes.size(); buffer.m.planes = planes;
    buffer.timestamp.tv_sec = 7; buffer.timestamp.tv_usec = 654321;
    for (unsigned i = 0; i < primes.size(); ++i) {
        planes[i].m.fd = primes[i].fd;
        planes[i].length = primes[i].length;
    }
    call(fd, VIDIOC_QBUF, &buffer, "QBUF dmabuf");
}
/* `memory` selects how the capture queue holds its frames: its own mapped
 * buffers, or dma-buf descriptors it imports. The imported case is the one a
 * GPU client uses, so it is qualified against the same fixtures. */
void decode(const char* device, const std::string& directory, const char* name, unsigned coded, unsigned raw,
            unsigned memory = V4L2_MEMORY_MMAP)
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
    /* A client asks what it would get by naming only the format and the
     * picture, the way Chromium's V4L2 decoder builds a candidate: the plane
     * layout is what it expects the call to fill in. */
    v4l2_format codedCandidate {};
    codedCandidate.type = output;
    codedCandidate.fmt.pix_mp.width = 128; codedCandidate.fmt.pix_mp.height = 96;
    codedCandidate.fmt.pix_mp.pixelformat = coded;
    call(fd, VIDIOC_TRY_FMT, &codedCandidate, "TRY_FMT coded");
    require(codedCandidate.fmt.pix_mp.pixelformat == coded, "TRY_FMT substituted the coded format");
    require(codedCandidate.fmt.pix_mp.num_planes == 1, "TRY_FMT coded is not a single buffer");
    require(codedCandidate.fmt.pix_mp.plane_fmt[0].sizeimage >= packet.size(),
            "TRY_FMT coded buffer is shorter than the packet");
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
    /* A client filters its candidates with TRY_FMT and drops the ones it
     * fails, so the call has to answer before the format is set. */
    v4l2_format candidate {};
    candidate.type = capture;
    candidate.fmt.pix_mp.width = format.fmt.pix_mp.width;
    candidate.fmt.pix_mp.height = format.fmt.pix_mp.height;
    candidate.fmt.pix_mp.pixelformat = raw;
    call(fd, VIDIOC_TRY_FMT, &candidate, "TRY_FMT capture");
    require(candidate.fmt.pix_mp.pixelformat == raw, "TRY_FMT substituted the raw format");
    call(fd, VIDIOC_S_FMT, &format, "S_FMT capture");
    require(format.fmt.pix_mp.pixelformat == raw, "raw format was substituted");
    /* TRY_FMT has to predict what S_FMT produces, planes included: a client
     * that compares the two, as Chromium's V4L2 decoder does, rejects a
     * capture format whose plane count or stride appears only once it is set. */
    require(candidate.fmt.pix_mp.num_planes == format.fmt.pix_mp.num_planes,
            "TRY_FMT and S_FMT disagree on the plane count");
    for (unsigned i = 0; i < format.fmt.pix_mp.num_planes; ++i) {
        require(candidate.fmt.pix_mp.plane_fmt[i].bytesperline ==
                    format.fmt.pix_mp.plane_fmt[i].bytesperline,
                "TRY_FMT and S_FMT disagree on a plane stride");
        require(candidate.fmt.pix_mp.plane_fmt[i].sizeimage ==
                    format.fmt.pix_mp.plane_fmt[i].sizeimage,
                "TRY_FMT and S_FMT disagree on a plane size");
    }
    /* The stride reported for the capture format is the one its rows are
     * written with. A consumer that steps a packed mapping by the planar
     * luma stride instead reads every row from the wrong sample, so the
     * reported value has to name the packed pitch. */
    const auto& negotiated = format.fmt.pix_mp;
    const std::uint32_t sampleBytes = raw == V4L2_PIX_FMT_ABGR32 ? 4u
        : raw == V4L2_PIX_FMT_P010 ? 2u : 1u;
    require(negotiated.plane_fmt[0].bytesperline == negotiated.width * sampleBytes,
            "capture stride is not the packed pitch of its format");
    require(negotiated.plane_fmt[0].sizeimage >=
                negotiated.plane_fmt[0].bytesperline * negotiated.height,
            "capture plane is shorter than the rows it reports");
    auto outputs = allocate(fd, capture, 4);
    Primes primes;
    if (memory == V4L2_MEMORY_DMABUF) {
        primes = exportAndRelease(fd, capture, outputs);
        outputs.clear();
        release(fd, capture);
        require(import(fd, capture, primes.size()) == primes.size(), "imported buffer count differs");
        for (auto& planes : primes) {
            outputs.emplace_back();
            for (auto& plane : planes)
                outputs.back().emplace_back(plane.fd, plane.length, 0);
        }
    }
    for (unsigned i = 0; i < outputs.size(); ++i) {
        if (memory == V4L2_MEMORY_DMABUF) queueImported(fd, capture, i, primes[i]);
        else queue(fd, capture, i, outputs[i]);
    }
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
        buffer.type = capture; buffer.memory = memory;
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
        if (raw == V4L2_PIX_FMT_ABGR32) {
            /* The host decodes straight to packed B, G, R, A. The reference is
             * the planar fixture taken through the limited-range BT.601 matrix
             * a frame this small carries when the stream states nothing, and
             * only a pixel whose chroma neighborhood is constant is compared:
             * hardware interpolates the subsampled field, so a pixel beside a
             * chroma edge has no single defined answer. */
            require(actual.size() == 128u * 96u * 4u, "packed frame size differs");
            const unsigned step = reference.size() == 128u * 96u * 3u ? 2u : 1u;
            const auto sample = [&](std::size_t index) { return int(reference[step * index + step - 1]); };
            const auto chroma = [&](long cellRow, long cellColumn) {
                cellRow = cellRow < 0 ? 0 : cellRow > 47 ? 47 : cellRow;
                cellColumn = cellColumn < 0 ? 0 : cellColumn > 63 ? 63 : cellColumn;
                const auto pair = std::size_t(128 * 96 + cellRow * 128 + cellColumn * 2);
                return std::pair<int, int>{sample(pair), sample(pair + 1)};
            };
            unsigned maximum = 0, checked = 0;
            for (long row = 0; row < 96; ++row)
                for (long column = 0; column < 128; ++column) {
                    const auto* got = actual.data() + (row * 128 + column) * 4;
                    require(got[3] == 0xff, "packed frame is not opaque");
                    const auto cell = chroma(row / 2, column / 2);
                    bool flat = true;
                    for (long dy = -1; dy <= 1; ++dy)
                        for (long dx = -1; dx <= 1; ++dx)
                            flat = flat && chroma(row / 2 + dy, column / 2 + dx) == cell;
                    if (!flat) continue;
                    ++checked;
                    const double luma = 1.164 * (sample(std::size_t(row * 128 + column)) - 16);
                    const double u = cell.first - 128, v = cell.second - 128;
                    const double wanted[3] = {luma + 2.017 * u, luma - 0.392 * u - 0.813 * v,
                                              luma + 1.596 * v};
                    for (unsigned channel = 0; channel < 3; ++channel) {
                        const double clamped = wanted[channel] < 0 ? 0 : wanted[channel] > 255 ? 255 : wanted[channel];
                        const int difference = int(got[channel]) - int(clamped + 0.5);
                        maximum = std::max(maximum, unsigned(difference < 0 ? -difference : difference));
                    }
                }
            require(checked >= 128 * 96 / 8, "too little of the packed frame could be verified");
            require(maximum <= 8, "V4L2 packed pixels differ from the reference");
            std::printf("PASS %s raw=%08x memory=%s max-error=%u pixels=%u\n", name, raw,
                memory == V4L2_MEMORY_DMABUF ? "dmabuf" : "mmap", maximum, checked);
            std::fflush(stdout);
            continue;
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
        std::printf("PASS %s raw=%08x memory=%s max-error=%u\n", name, raw,
            memory == V4L2_MEMORY_DMABUF ? "dmabuf" : "mmap", maximum);
        std::fflush(stdout);
    }
    require(frame && eos, "capture frame/EOS timeout");
    type = output; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF output");
    type = capture; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF capture");
}
}
/* A client that renders what it decodes keeps its pictures in a pool and hands
 * the same descriptors back to the capture queue as they are released, so each
 * buffer is queued once per pass around that pool. This drives exactly that: a
 * ring of imported descriptors, every one queued again as soon as it is
 * dequeued, for many more pictures than the ring holds. A defect that appears
 * only on a buffer's second use stops the run after one pass, which is what a
 * client that decodes a handful of pictures and then gives up looks like from
 * the outside. */
void ringCycle(const char* device, const std::string& directory, const char* name,
               unsigned coded, unsigned raw, unsigned ring, unsigned pictures)
{
    const auto packet = read(directory + "/" + name + ".bin");
    Device owner(device);
    const int fd = owner.fd;
    const auto output = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    const auto capture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
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
    auto inputs = allocate(fd, output, 4);
    require(inputs[0][0].length >= packet.size(), "coded buffer too short");
    auto type = output;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON output");

    const auto feed = [&](unsigned index, unsigned sequence) {
        std::memcpy(inputs[index][0].data, packet.data(), packet.size());
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
        buffer.length = inputs[index].size(); buffer.m.planes = planes;
        buffer.timestamp.tv_sec = 7; buffer.timestamp.tv_usec = sequence;
        planes[0].length = inputs[index][0].length;
        planes[0].bytesused = packet.size();
        call(fd, VIDIOC_QBUF, &buffer, "QBUF coded");
    };

    unsigned sequence = 0;
    feed(0, ++sequence);
    unsigned free_inputs = unsigned(inputs.size()) - 1;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool source = false;
    while (!source && std::chrono::steady_clock::now() < deadline) {
        pollfd p {fd, POLLPRI, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll source change failed");
        v4l2_event event {};
        if (ioctl(fd, VIDIOC_DQEVENT, &event) == 0)
            source = event.type == V4L2_EVENT_SOURCE_CHANGE;
        else require(errno == EAGAIN, "DQEVENT failed");
    }
    require(source, "decoder source-change timeout");

    v4l2_format format {};
    format.type = capture;
    call(fd, VIDIOC_G_FMT, &format, "G_FMT capture");
    format.fmt.pix_mp.pixelformat = raw;
    call(fd, VIDIOC_S_FMT, &format, "S_FMT capture");
    /* The descriptors stand in for a client's own: exported once, then the
     * queue owns none of the memory it writes into. */
    auto maps = allocate(fd, capture, ring);
    auto primes = exportAndRelease(fd, capture, maps);
    maps.clear();
    release(fd, capture);
    const unsigned slots = import(fd, capture, unsigned(primes.size()));
    require(slots == primes.size(), "imported buffer count differs");
    for (unsigned i = 0; i < primes.size(); ++i) queueImported(fd, capture, i, primes[i]);
    type = capture;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON capture");

    std::vector<unsigned> uses(primes.size(), 0);
    unsigned produced = 0, reused = 0;
    const auto started = std::chrono::steady_clock::now();
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
    while (produced < pictures && std::chrono::steady_clock::now() < deadline) {
        while (free_inputs && sequence < pictures + slots) {
            feed(unsigned(sequence % inputs.size()), ++sequence);
            --free_inputs;
        }
        pollfd p {fd, POLLIN | POLLOUT, 0};
        if (poll(&p, 1, 200) < 0 && errno != EINTR) throw std::runtime_error("poll ring failed");
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
                require(errno == EAGAIN, "DQBUF coded failed");
                break;
            }
            ++free_inputs;
        }
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = capture; buffer.memory = V4L2_MEMORY_DMABUF;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
                require(errno == EAGAIN, "DQBUF capture failed");
                break;
            }
            require(buffer.index < primes.size(), "capture index outside the ring");
            if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
                std::printf("RING-FAIL %s ring=%u picture=%u slot=%u uses=%u error-flagged\n",
                            name, ring, produced, buffer.index, uses[buffer.index]);
                std::fflush(stdout);
                require(false, "capture buffer came back error-flagged");
            }
            require(planes[0].bytesused > 0, "capture buffer came back empty");
            ++produced;
            if (uses[buffer.index]) ++reused;
            ++uses[buffer.index];
            /* The same descriptor, on the same slot, exactly as a pool hands
             * a released picture back. */
            errno = 0;
            v4l2_plane again[VIDEO_MAX_PLANES] {};
            v4l2_buffer requeue {};
            requeue.type = capture; requeue.memory = V4L2_MEMORY_DMABUF;
            requeue.index = buffer.index;
            requeue.length = primes[buffer.index].size(); requeue.m.planes = again;
            for (unsigned i = 0; i < primes[buffer.index].size(); ++i) {
                again[i].m.fd = primes[buffer.index][i].fd;
                again[i].length = primes[buffer.index][i].length;
            }
            if (ioctl(fd, VIDIOC_QBUF, &requeue) < 0) {
                std::printf("RING-FAIL %s ring=%u picture=%u slot=%u uses=%u requeue-errno=%d (%s)\n",
                            name, ring, produced, buffer.index, uses[buffer.index],
                            errno, std::strerror(errno));
                std::fflush(stdout);
                require(false, "re-queueing an imported capture buffer failed");
            }
        }
    }
    require(produced >= pictures, "the capture ring stopped producing pictures");
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::printf("PASS ring %s raw=%08x ring=%u pictures=%u reused=%u seconds=%.3f rate=%.1f\n",
                name, raw, ring, produced, reused, elapsed,
                elapsed > 0 ? produced / elapsed : 0.0);
    std::fflush(stdout);
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
        /* The discovery a V4L2 client performs before it builds a decoder.
         * Each of these answers gated Chromium's decoder entirely: a
         * continuous frame-size range left it on its built-in maximum coded
         * size, an unimplemented TRY_FMT made it discard every capture
         * candidate, and a reversed format list hid the planar formats a
         * coded format names. */
        for (unsigned index = 0;; ++index) {
            v4l2_fmtdesc coded {};
            coded.type = output; coded.index = index;
            if (ioctl(capabilities.fd, VIDIOC_ENUM_FMT, &coded) < 0) {
                require(errno == EINVAL, "ENUM_FMT failed"); break;
            }
            v4l2_frmsizeenum sizes {};
            sizes.index = 0; sizes.pixel_format = coded.pixelformat;
            call(capabilities.fd, VIDIOC_ENUM_FRAMESIZES, &sizes, "ENUM_FRAMESIZES");
            require(sizes.type == V4L2_FRMSIZE_TYPE_STEPWISE,
                    "frame sizes are not reported stepwise");
            require(sizes.stepwise.step_width >= 1 && sizes.stepwise.step_height >= 1,
                    "a stepwise frame size has a zero step");
            require(sizes.stepwise.max_width >= 1920 && sizes.stepwise.max_height >= 1200,
                    "the advertised maximum coded size is too small");
            /* A capture list in wire order names the planar format the device
             * actually decodes to; a reversed one names only the tail. */
            bool planar = false;
            for (unsigned raw = 0;; ++raw) {
                v4l2_fmtdesc fmt {};
                fmt.type = capture; fmt.index = raw;
                if (ioctl(capabilities.fd, VIDIOC_ENUM_FMT, &fmt) < 0) {
                    require(errno == EINVAL, "ENUM_FMT capture failed"); break;
                }
                planar = planar || fmt.pixelformat == V4L2_PIX_FMT_NV12M;
            }
            require(planar, "the capture list does not name the planar format");
        }
        /* The pattern a rendering client produces: a small ring of imported
         * descriptors carried through many more pictures than it holds, so
         * every buffer is used several times. It runs before the rest because
         * a ring that stops after one pass is the failure a client sees. */
        for (unsigned index = 0;; ++index) {
            v4l2_fmtdesc fmt {};
            fmt.type = output; fmt.index = index;
            if (ioctl(capabilities.fd, VIDIOC_ENUM_FMT, &fmt) < 0) {
                require(errno == EINVAL, "ENUM_FMT failed"); break;
            }
            if (fmt.pixelformat != V4L2_PIX_FMT_VP9) continue;
            ringCycle(device, argv[1], "vp9-profile0", fmt.pixelformat,
                      V4L2_PIX_FMT_NV12M, 8, 40);
            ringCycle(device, argv[1], "vp9-profile0", fmt.pixelformat,
                      V4L2_PIX_FMT_ABGR32, 8, 40);
            break;
        }
        unsigned tested = 0;
        for (unsigned index = 0;; ++index) {
            v4l2_fmtdesc fmt {};
            fmt.type = output; fmt.index = index;
            if (ioctl(capabilities.fd, VIDIOC_ENUM_FMT, &fmt) < 0) {
                require(errno == EINVAL, "ENUM_FMT failed"); break;
            }
            if (fmt.pixelformat == V4L2_PIX_FMT_MJPEG) {
                decode(device, argv[1], "jpeg", fmt.pixelformat, V4L2_PIX_FMT_NV12M);
                decode(device, argv[1], "jpeg", fmt.pixelformat, V4L2_PIX_FMT_NV12M, V4L2_MEMORY_DMABUF);
                decode(device, argv[1], "jpeg", fmt.pixelformat, V4L2_PIX_FMT_ABGR32);
                decode(device, argv[1], "jpeg", fmt.pixelformat, V4L2_PIX_FMT_ABGR32, V4L2_MEMORY_DMABUF);
                ++tested; continue;
            }
            // VP9 states its profile in the stream itself, so the driver
            // exposes no profile menu to walk: the advertised format is the
            // whole capability.
            if (fmt.pixelformat == V4L2_PIX_FMT_VP9) {
                decode(device, argv[1], "vp9-profile0", fmt.pixelformat, V4L2_PIX_FMT_NV12M);
                decode(device, argv[1], "vp9-profile0", fmt.pixelformat, V4L2_PIX_FMT_NV12M,
                       V4L2_MEMORY_DMABUF);
                decode(device, argv[1], "vp9-profile0", fmt.pixelformat, V4L2_PIX_FMT_ABGR32);
                decode(device, argv[1], "vp9-profile0", fmt.pixelformat, V4L2_PIX_FMT_ABGR32,
                       V4L2_MEMORY_DMABUF);
                ++tested; continue;
            }
            require(fmt.pixelformat == V4L2_PIX_FMT_H264 || fmt.pixelformat == V4L2_PIX_FMT_HEVC, "advertised codec lacks a fixture");
            const bool avc = fmt.pixelformat == V4L2_PIX_FMT_H264;
            const unsigned id = avc ? V4L2_CID_MPEG_VIDEO_H264_PROFILE : V4L2_CID_MPEG_VIDEO_HEVC_PROFILE;
            v4l2_queryctrl control {}; control.id = id;
            call(capabilities.fd, VIDIOC_QUERYCTRL, &control, "QUERYCTRL profiles");
            require(control.minimum >= 0 && control.maximum <= 32, "invalid profile menu bounds");
            bool imported = false;
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
                /* Packed capture is what a client that only draws through a
                 * 32-bit RGB surface negotiates, and it is offered whatever
                 * the stream's bit depth is. */
                decode(device, argv[1], profile->name, fmt.pixelformat, V4L2_PIX_FMT_ABGR32);
                if (!imported) {
                    decode(device, argv[1], profile->name, fmt.pixelformat, profile->raw,
                           V4L2_MEMORY_DMABUF);
                    decode(device, argv[1], profile->name, fmt.pixelformat, V4L2_PIX_FMT_ABGR32,
                           V4L2_MEMORY_DMABUF);
                    imported = true;
                }
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
