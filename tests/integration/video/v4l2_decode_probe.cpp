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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numeric>
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
    /* `data` is the plane's first byte and `length` its size. A plane that
     * starts inside a dma-buf, after the planes before it, need not start on a
     * page, so the mapping begins on the page below it. */
    void* data;
    std::size_t length;
    Mapping(int fd, std::size_t size, off_t offset)
    {
        const off_t page = sysconf(_SC_PAGESIZE);
        const off_t aligned = offset - offset % page;
        skew = std::size_t(offset - aligned);
        base = mmap(nullptr, size + skew, PROT_READ | PROT_WRITE, MAP_SHARED, fd, aligned);
        require(base != MAP_FAILED, "map video buffer failed");
        data = static_cast<std::uint8_t*>(base) + skew;
        length = size;
    }
    Mapping(Mapping&& other) noexcept : data(other.data), length(other.length), base(other.base), skew(other.skew)
    { other.base = MAP_FAILED; }
    ~Mapping() { if (base != MAP_FAILED) munmap(base, length + skew); }
private:
    void* base;
    std::size_t skew;
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
    /* Where the plane starts in the dma-buf, which is not zero for the second
     * plane of a buffer whose planes share one. */
    std::size_t offset {};
    Prime() = default;
    Prime(int descriptor, std::size_t size, std::size_t start = 0) : fd(descriptor), length(size), offset(start) {}
    Prime(Prime&& other) noexcept : fd(other.fd), length(other.length), offset(other.offset) { other.fd = -1; }
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
/* How an imported capture buffer states its planes' lengths. A client may
 * pass what QUERYBUF gave it, the dma-buf's whole size, or nothing at all, in
 * which case the queue takes the dma-buf's size. */
enum class Declared { negotiated, whole, none };
void queueImported(int fd, v4l2_buf_type type, unsigned index, const std::vector<Prime>& primes,
                   const std::vector<unsigned>& sizes = {}, Declared declared = Declared::whole)
{
    v4l2_plane planes[VIDEO_MAX_PLANES] {};
    v4l2_buffer buffer {};
    buffer.type = type; buffer.memory = V4L2_MEMORY_DMABUF; buffer.index = index;
    buffer.length = primes.size(); buffer.m.planes = planes;
    buffer.timestamp.tv_sec = 7; buffer.timestamp.tv_usec = 654321;
    for (unsigned i = 0; i < primes.size(); ++i) {
        planes[i].m.fd = primes[i].fd;
        planes[i].length = declared == Declared::none ? 0
            : declared == Declared::negotiated ? sizes.at(i) : primes[i].length;
    }
    call(fd, VIDIOC_QBUF, &buffer, "QBUF dmabuf");
}
/* Where an imported capture buffer's memory comes from. By default it is the
 * capture queue's own buffers, exported and handed back, which on a host with
 * a GPU are host-visible blobs. `guestPages` instead takes guest memory a
 * client could equally have got from a dma-buf heap or another driver: the
 * coded queue's buffers, which are guest pages on every host. `shared` puts a
 * buffer's planes in one dma-buf, one after another, the way a client that
 * allocates one buffer per picture does. */
struct Import {
    bool guestPages {};
    bool shared {};
    Declared declared {Declared::whole};
};
/* Exports `count` buffers of guest pages, one dma-buf per plane or one for all
 * of a buffer's planes. The descriptors outlive the device that allocated
 * them: vb2 keeps a buffer an exported dma-buf still holds. */
Primes exportGuestPages(const char* device, unsigned coded, const std::vector<unsigned>& sizes,
                        bool shared, unsigned count)
{
    Device owner(device);
    const int fd = owner.fd;
    v4l2_format input {};
    input.type = output;
    input.fmt.pix_mp.width = 128; input.fmt.pix_mp.height = 96;
    input.fmt.pix_mp.pixelformat = coded;
    input.fmt.pix_mp.num_planes = 1;
    input.fmt.pix_mp.plane_fmt[0].sizeimage = 4096;
    call(fd, VIDIOC_S_FMT, &input, "S_FMT coded for export");
    std::vector<unsigned> chunks;
    if (shared) chunks.push_back(std::accumulate(sizes.begin(), sizes.end(), 0u));
    else chunks = sizes;
    /* One group of buffers per distinct size. A coded buffer is as large as
     * the request says, so each dma-buf is exactly what its planes need. */
    Primes result(count);
    for (const unsigned size : chunks) {
        v4l2_create_buffers create {};
        create.count = count; create.memory = V4L2_MEMORY_MMAP;
        create.format = input;
        create.format.fmt.pix_mp.plane_fmt[0].sizeimage = size;
        call(fd, VIDIOC_CREATE_BUFS, &create, "CREATE_BUFS guest pages");
        require(create.count == count, "unexpected guest page buffer count");
        for (unsigned i = 0; i < count; ++i) {
            v4l2_exportbuffer request {};
            request.type = output; request.index = create.index + i; request.plane = 0;
            request.flags = O_RDWR | O_CLOEXEC;
            call(fd, VIDIOC_EXPBUF, &request, "EXPBUF guest pages");
            require(request.fd >= 0, "EXPBUF returned no descriptor");
            const off_t extent = lseek(request.fd, 0, SEEK_END);
            require(extent >= off_t(size), "exported guest page buffer is too small");
            result[i].emplace_back(request.fd, std::size_t(extent));
        }
    }
    if (shared) {
        /* The planes follow one another in the one dma-buf. */
        std::size_t offset = 0;
        for (auto& planes : result) {
            const int whole = planes[0].fd;
            const std::size_t extent = planes[0].length;
            offset = 0;
            planes[0].offset = 0;
            for (unsigned i = 1; i < sizes.size(); ++i) {
                offset += sizes[i - 1];
                planes.emplace_back(dup(whole), extent, offset);
                require(planes.back().fd >= 0, "dup of a dma-buf failed");
            }
        }
    }
    release(fd, output);
    return result;
}
/* `memory` selects how the capture queue holds its frames: its own mapped
 * buffers, or dma-buf descriptors it imports. The imported case is the one a
 * GPU client uses, so it is qualified against the same fixtures. */
void decode(const char* device, const std::string& directory, const char* name, unsigned coded, unsigned raw,
            unsigned memory = V4L2_MEMORY_MMAP, Import origin = {})
{
    const std::string memoryName = memory != V4L2_MEMORY_DMABUF ? "mmap"
        : !origin.guestPages ? "dmabuf"
        : std::string(origin.shared ? "dmabuf-guest-shared-" : "dmabuf-guest-") +
            (origin.declared == Declared::negotiated ? "negotiated" : origin.declared == Declared::whole ? "whole" : "none");
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
    Buffers outputs;
    Primes primes;
    std::vector<unsigned> sizes;
    for (unsigned i = 0; i < format.fmt.pix_mp.num_planes; ++i)
        sizes.push_back(format.fmt.pix_mp.plane_fmt[i].sizeimage);
    if (origin.guestPages) {
        require(memory == V4L2_MEMORY_DMABUF, "guest page buffers are imported");
        /* The queue says how many buffers it keeps; the descriptors are then
         * exported for exactly those. */
        primes = exportGuestPages(device, coded, sizes, origin.shared, import(fd, capture, 4));
        for (auto& planes : primes) {
            outputs.emplace_back();
            for (unsigned i = 0; i < planes.size(); ++i)
                outputs.back().emplace_back(planes[i].fd, sizes.at(i), planes[i].offset);
        }
    } else {
        outputs = allocate(fd, capture, 4);
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
    }
    for (unsigned i = 0; i < outputs.size(); ++i) {
        if (memory == V4L2_MEMORY_DMABUF) queueImported(fd, capture, i, primes[i], sizes, origin.declared);
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
                memoryName.c_str(), maximum, checked);
            std::fflush(stdout);
            continue;
        }
        /* A 10-bit stream captured as 8-bit planes is the 10-bit picture
         * rounded to 8 bits, so its reference is half the size. */
        const unsigned step = raw == V4L2_PIX_FMT_P010 ? 2 : 1;
        const unsigned referenceStep = reference.size() == 128u * 96u * 3u ? 2 : 1;
        require(actual.size() == reference.size() / referenceStep * step, "decoded plane sizes differ");
        if (raw == V4L2_PIX_FMT_YUV420M) {
            const auto planar = actual;
            for (unsigned i = 0; i < 128 * 96 / 4; ++i) {
                actual[128 * 96 + 2 * i] = planar[128 * 96 + i];
                actual[128 * 96 + 2 * i + 1] = planar[128 * 96 * 5 / 4 + i];
            }
        }
        unsigned maximum = 0;
        for (std::size_t i = 0; i < actual.size(); i += step) {
            const std::size_t at = i / step * referenceStep;
            const int got = step == 1 ? actual[i] : (actual[i] | (actual[i + 1] << 8)) >> 6;
            int wanted = referenceStep == 1 ? reference[at] : (reference[at] | (reference[at + 1] << 8)) >> 6;
            if (referenceStep > step) wanted = (wanted + 2) >> 2;
            maximum = std::max(maximum, unsigned(std::abs(got - wanted)));
        }
        require(maximum <= (coded == V4L2_PIX_FMT_MJPEG ? 4u : 2u), "V4L2 pixels differ from reference");
        std::printf("PASS %s raw=%08x memory=%s max-error=%u\n", name, raw,
            memoryName.c_str(), maximum);
        std::fflush(stdout);
    }
    require(frame && eos, "capture frame/EOS timeout");
    type = output; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF output");
    type = capture; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF capture");
}
}
/* The imported buffers a client that is not the driver's own exporter queues:
 * guest memory, with its planes in separate dma-bufs or in one, and their
 * lengths stated as QUERYBUF gave them, as the whole dma-buf, or not at all.
 * A plane after the first in a shared dma-buf starts past its own length, so
 * only the dma-buf's size can bound it. */
void importGuestPages(const char* device, const std::string& directory, const char* name,
                      unsigned coded, unsigned raw)
{
    for (const bool shared : {false, true})
        for (const auto declared : {Declared::negotiated, Declared::whole, Declared::none})
            decode(device, directory, name, coded, raw, V4L2_MEMORY_DMABUF, Import{true, shared, declared});
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
            const auto index = unsigned(sequence % inputs.size());
            feed(index, ++sequence);
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

/* The access units of an Annex B stream in which every unit opens with an
 * access unit delimiter. */
std::vector<std::vector<std::uint8_t>> delimitedUnits(const std::vector<std::uint8_t>& stream)
{
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 4 < stream.size(); ++i)
        if (stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 0 && stream[i + 3] == 1 &&
            (stream[i + 4] & 0x1f) == 9)
            starts.push_back(i);
    require(!starts.empty() && starts.front() == 0, "the stream does not open with an access unit delimiter");
    std::vector<std::vector<std::uint8_t>> units;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        const auto end = i + 1 < starts.size() ? starts[i + 1] : stream.size();
        units.emplace_back(stream.begin() + std::ptrdiff_t(starts[i]), stream.begin() + std::ptrdiff_t(end));
    }
    return units;
}
/* A stream that changes its picture size twice, decoded the way a stateful
 * client does (Android's V4L2 codec among them): the capture queue is set up
 * at the first source change and again at each later one, and a client that
 * handles a change drops every picture it has not dequeued by then. So the
 * device has to return every picture of the old size, then a buffer marked
 * V4L2_BUF_FLAG_LAST, before the source change is visible, and the client
 * must see no picture of the new size in a buffer set up for the old one.
 * Every picture is flat gray, 8 levels brighter than the one before it, so the
 * brightness names the picture and a lost, repeated or reordered one shows.
 *
 * The eager client ends the stream the way such a client does: it asks for its
 * drain as soon as every coded buffer is back and takes the first LAST buffer
 * it sees as the end of that drain. The device must therefore not let a change
 * be pending then, which it does by keeping the coded buffer that carries the
 * change until the client has handled it, as a hardware decoder does. */
void resolutionChange(const char* device, const std::string& directory, unsigned coded,
                      unsigned memory = V4L2_MEMORY_MMAP, Import origin = {}, bool eager = false)
{
    struct Sequence { unsigned width, height, pictures; };
    const Sequence sequences[] = {{128, 96, 8}, {96, 64, 8}, {160, 112, 8}};
    constexpr unsigned count = 3;
    constexpr unsigned serial = 424242;
    const auto units = delimitedUnits(read(directory + "/h264-resolution-change.bin"));
    unsigned total = 0;
    std::size_t largest = 0;
    for (const auto& sequence : sequences) total += sequence.pictures;
    for (const auto& unit : units) largest = std::max(largest, unit.size());
    require(units.size() == total, "the resolution-change stream has the wrong number of pictures");

    Device owner(device);
    const int fd = owner.fd;
    v4l2_event_subscription subscription {};
    subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    call(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription, "SUBSCRIBE_EVENT");
    v4l2_format input {};
    input.type = output;
    input.fmt.pix_mp.width = sequences[0].width; input.fmt.pix_mp.height = sequences[0].height;
    input.fmt.pix_mp.pixelformat = coded;
    input.fmt.pix_mp.num_planes = 1;
    input.fmt.pix_mp.plane_fmt[0].sizeimage = unsigned(largest);
    call(fd, VIDIOC_S_FMT, &input, "S_FMT coded");
    auto inputs = allocate(fd, output, 4);
    for (const auto& buffer : inputs) require(buffer[0].length >= largest, "coded buffer too short");
    std::vector<bool> free(inputs.size(), true);
    auto type = output;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON output");

    Buffers frames;
    Primes primes;
    std::vector<unsigned> sizes;
    bool capturing = false, last = false, draining = false, drained = false;
    unsigned sequence = 0, received = 0, fed = 0, changes = 0, drainedAfter = 0, drainedChanges = 0;
    const auto firstOf = [&](unsigned index) {
        unsigned first = 0;
        for (unsigned i = 0; i < index; ++i) first += sequences[i].pictures;
        return first;
    };
    const auto set_up = [&](const Sequence& want) {
        v4l2_format format {};
        format.type = capture;
        call(fd, VIDIOC_G_FMT, &format, "G_FMT capture");
        require(format.fmt.pix_mp.width == want.width && format.fmt.pix_mp.height == want.height,
                "the capture format does not name the new picture size");
        if (capturing) {
            type = capture;
            call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF capture");
            frames.clear();
            primes.clear();
            release(fd, capture);
        }
        format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
        call(fd, VIDIOC_S_FMT, &format, "S_FMT capture");
        require(format.fmt.pix_mp.width == want.width && format.fmt.pix_mp.height == want.height,
                "S_FMT changed the picture size");
        require(format.fmt.pix_mp.num_planes == 2, "the raw format is not two planes");
        sizes.clear();
        for (unsigned i = 0; i < format.fmt.pix_mp.num_planes; ++i)
            sizes.push_back(format.fmt.pix_mp.plane_fmt[i].sizeimage);
        if (memory != V4L2_MEMORY_DMABUF) {
            frames = allocate(fd, capture, 6);
            for (unsigned i = 0; i < frames.size(); ++i) queue(fd, capture, i, frames[i]);
        } else {
            /* A client that draws what it decodes brings new dma-bufs of the
             * new size. */
            if (origin.guestPages) {
                primes = exportGuestPages(device, coded, sizes, origin.shared, import(fd, capture, 4));
            } else {
                auto maps = allocate(fd, capture, 6);
                primes = exportAndRelease(fd, capture, maps);
                maps.clear();
                release(fd, capture);
                require(import(fd, capture, unsigned(primes.size())) == primes.size(), "imported buffer count differs");
            }
            frames.clear();
            for (auto& planes : primes) {
                frames.emplace_back();
                for (unsigned i = 0; i < planes.size(); ++i)
                    frames.back().emplace_back(planes[i].fd, origin.guestPages ? sizes.at(i) : planes[i].length,
                                               planes[i].offset);
            }
            for (unsigned i = 0; i < primes.size(); ++i)
                queueImported(fd, capture, i, primes[i], sizes, origin.declared);
        }
        type = capture;
        call(fd, VIDIOC_STREAMON, &type, "STREAMON capture");
        capturing = true;
        last = false;
    };
    const auto check = [&](const v4l2_buffer& buffer, const v4l2_plane* planes) {
        const auto& now = sequences[sequence];
        const unsigned id = unsigned(buffer.timestamp.tv_sec);
        require(buffer.timestamp.tv_usec == serial, "a picture lost its timestamp");
        require(id == received, "a picture was lost, repeated or returned out of order");
        require(id >= firstOf(sequence) && id < firstOf(sequence) + now.pictures,
                "a picture of another size came back in a buffer set up for this one");
        require(buffer.index < frames.size() && buffer.length == 2, "capture buffer is not two planes");
        const std::size_t luma = std::size_t(now.width) * now.height;
        require(planes[0].bytesused == luma && planes[1].bytesused == luma / 2,
                "a picture does not fill exactly the size the sequence has");
        const auto level = [](const void* data, std::size_t length, int wanted) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            return std::all_of(bytes, bytes + length, [wanted](std::uint8_t v) { return std::abs(int(v) - wanted) <= 2; });
        };
        require(level(frames[buffer.index][0].data, luma, 16 + 8 * int(id)), "a picture is not the one its timestamp names");
        require(level(frames[buffer.index][1].data, luma / 2, 128), "a picture has the wrong chroma");
    };
    /* Dequeues what is ready; the LAST buffer ends it, and anything after it
     * is refused with EPIPE. */
    const auto collect = [&]() {
        while (capturing && !last) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = capture; buffer.memory = memory;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
                require(errno == EAGAIN, "DQBUF capture failed");
                return;
            }
            require(!(buffer.flags & V4L2_BUF_FLAG_ERROR), "capture buffer has decoder error");
            if (planes[0].bytesused) {
                check(buffer, planes);
                ++received;
            }
            if (buffer.flags & V4L2_BUF_FLAG_LAST) {
                last = true;
                /* The LAST buffer ends a drain. A client that asked for one
                 * takes the first it sees as the end, so every picture has to
                 * be back by then. */
                if (draining) {
                    if (received != total) {
                        char text[160];
                        std::snprintf(text, sizeof text,
                                      "the LAST buffer of a drain came after %u of %u pictures, with %u size changes handled",
                                      received, total, changes);
                        throw std::runtime_error(text);
                    }
                    drained = true;
                }
                return;
            }
            if (memory == V4L2_MEMORY_DMABUF) queueImported(fd, capture, buffer.index, primes[buffer.index], sizes, origin.declared);
            else queue(fd, capture, buffer.index, frames[buffer.index]);
        }
    };
    const auto refused = [&]() {
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = capture; buffer.memory = memory;
        buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
        require(ioctl(fd, VIDIOC_DQBUF, &buffer) < 0 && errno == EPIPE,
                "a capture buffer came after the LAST one");
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    std::chrono::steady_clock::time_point announced_at {};
    while (std::chrono::steady_clock::now() < deadline) {
        while (fed < units.size()) {
            const auto slot = std::find(free.begin(), free.end(), true);
            if (slot == free.end()) break;
            const auto index = unsigned(slot - free.begin());
            std::memcpy(inputs[index][0].data, units[fed].data(), units[fed].size());
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
            buffer.length = inputs[index].size(); buffer.m.planes = planes;
            buffer.timestamp.tv_sec = fed; buffer.timestamp.tv_usec = serial;
            planes[0].length = inputs[index][0].length;
            planes[0].bytesused = unsigned(units[fed].size());
            call(fd, VIDIOC_QBUF, &buffer, "QBUF coded");
            *slot = false;
            ++fed;
        }
        pollfd p {fd, POLLIN | POLLOUT | POLLPRI, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll resolution change failed");
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
                require(errno == EAGAIN, "DQBUF coded failed");
                break;
            }
            require(!(buffer.flags & V4L2_BUF_FLAG_ERROR), "a coded buffer came back with an error");
            free.at(buffer.index) = true;
        }
        /* A client ends its stream with a drain as soon as every coded buffer
         * is back, without waiting for the pictures: it does not know a size
         * change is still on its way. Hardware holds back the coded buffer
         * that carries the change until the client has handled it, so for such
         * a client the drain never starts first. The patient client below
         * waits for the pictures instead. */
        if (!draining && capturing && fed == units.size() && (eager || received == total) &&
            std::all_of(free.begin(), free.end(), [](bool v) { return v; })) {
            v4l2_decoder_cmd drain {};
            drain.cmd = V4L2_DEC_CMD_STOP;
            call(fd, VIDIOC_DECODER_CMD, &drain, "DECODER_CMD drain");
            draining = true;
            drainedAfter = received;
            drainedChanges = changes;
        }
        /* Pictures first, then the event, which is what a client does: it
         * answers an event that was there when it woke, with the pictures that
         * were ready at the same moment. */
        bool event = p.revents & POLLPRI;
        /* The eager client is slow to look at events: it asks for its drain
         * while a size change is still on its way, which is the order a
         * client's own threads can produce. A device that holds the coded
         * buffer carrying the change back has not let it ask yet. */
        if (event && eager && changes > 0 && !draining) {
            const auto now = std::chrono::steady_clock::now();
            if (announced_at == std::chrono::steady_clock::time_point {}) announced_at = now;
            event = now - announced_at >= std::chrono::seconds(1);
            if (!event) usleep(20000);
        }
        if (event) announced_at = {};
        collect();
        if (event) {
            for (;;) {
                v4l2_event announced {};
                if (ioctl(fd, VIDIOC_DQEVENT, &announced) < 0) {
                    /* A non-blocking descriptor with no event pending is ENOENT. */
                    require(errno == ENOENT || errno == EAGAIN, "DQEVENT failed");
                    break;
                }
                if (announced.type != V4L2_EVENT_SOURCE_CHANGE ||
                    !(announced.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION))
                    continue;
                if (changes++ == 0) {
                    set_up(sequences[0]);
                    continue;
                }
                require(sequence + 1 < count, "more size changes than the stream has");
                require(received == firstOf(sequence) + sequences[sequence].pictures,
                        "the source change came before every picture of the old size");
                /* The drain a client has asked for ends the old size by itself,
                 * at the end of the new one, so a change inside it has no
                 * LAST buffer of its own. */
                if (!draining) {
                    require(last, "the source change came before the LAST buffer");
                    refused();
                }
                set_up(sequences[++sequence]);
            }
        }
        if (drained) break;
    }
    require(drained, "the drain timed out");
    require(received == total, "the drain ended before every picture was returned");
    require(changes == count, "the stream's size changes were not all announced");
    refused();
    type = output; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF output");
    type = capture; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF capture");
    std::printf("PASS resolution-change sequences=%u pictures=%u memory=%s drain=%s drained-after=%u pictures, %u changes\n",
        count, received, memory != V4L2_MEMORY_DMABUF ? "mmap" : !origin.guestPages ? "dmabuf" : "dmabuf-guest",
        eager ? "eager" : "after-pictures", drainedAfter, drainedChanges);
    std::fflush(stdout);
}

/* The access units of an Annex B HEVC or H.264 stream: a new one starts at a
 * parameter set, an access unit delimiter or a prefix SEI after a picture's
 * slices, or at a slice that begins a picture. */
std::vector<std::vector<std::uint8_t>> accessUnits(const std::vector<std::uint8_t>& stream, bool avc = false)
{
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 3 < stream.size(); ++i)
        if (stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 1)
            starts.push_back(i > 0 && stream[i - 1] == 0 ? i - 1 : i);
    std::vector<std::vector<std::uint8_t>> units;
    bool picture = false;
    std::size_t begin = 0;
    for (const auto start : starts) {
        const auto nal = start + (stream[start] == 0 && stream[start + 1] == 0 && stream[start + 2] == 0 ? 4 : 3);
        if (nal + 2 >= stream.size()) break;
        const unsigned type = avc ? stream[nal] & 0x1f : (stream[nal] >> 1) & 0x3f;
        const bool slice = avc ? type == 1 || type == 5 : type < 32;
        // first_mb_in_slice (H.264) or first_slice_segment_in_pic_flag (HEVC)
        const bool first = slice && (stream[nal + (avc ? 1 : 2)] & 0x80);
        const bool boundary = avc ? type >= 6 && type <= 9 : (type >= 32 && type <= 35) || type == 39;
        if (picture && (first || boundary)) {
            units.emplace_back(stream.begin() + begin, stream.begin() + start);
            begin = start;
            picture = false;
        }
        if (slice) picture = true;
    }
    units.emplace_back(stream.begin() + begin, stream.end());
    return units;
}
/* The access unit without its parameter sets, as a player seeking to a random
 * access point sends it: it has them already. */
std::vector<std::uint8_t> withoutParameterSets(const std::vector<std::uint8_t>& unit, bool avc = false)
{
    std::vector<std::uint8_t> result;
    std::size_t begin = 0;
    const auto flush = [&](std::size_t end, bool keep) {
        if (keep) result.insert(result.end(), unit.begin() + begin, unit.begin() + end);
    };
    bool keep = true;
    for (std::size_t i = 0; i + 3 < unit.size(); ++i) {
        if (!(unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] == 1)) continue;
        const auto start = i > 0 && unit[i - 1] == 0 && i - 1 >= begin ? i - 1 : i;
        flush(start, keep);
        begin = start;
        const unsigned type = avc ? unit[i + 3] & 0x1f : (unit[i + 3] >> 1) & 0x3f;
        keep = avc ? type != 7 && type != 8 : type < 32 || type > 34;
    }
    flush(unit.size(), keep);
    return result;
}
/* A stateful decode of a stream of several access units, one per coded
 * buffer, collecting each picture's timestamp and brightness in the order the
 * decoder returns them. */
class OrderRun {
public:
    struct Picture { unsigned stamp; int luma; };
    OrderRun(const char* device, std::size_t largest, unsigned codec) : owner_(device), fd_(owner_.fd)
    {
        v4l2_event_subscription subscription {};
        subscription.type = V4L2_EVENT_SOURCE_CHANGE;
        call(fd_, VIDIOC_SUBSCRIBE_EVENT, &subscription, "SUBSCRIBE_EVENT");
        v4l2_format input {};
        input.type = output;
        input.fmt.pix_mp.width = 64; input.fmt.pix_mp.height = 64;
        input.fmt.pix_mp.pixelformat = codec;
        input.fmt.pix_mp.num_planes = 1;
        input.fmt.pix_mp.plane_fmt[0].sizeimage = largest;
        call(fd_, VIDIOC_S_FMT, &input, "S_FMT coded");
        inputs_ = allocate(fd_, output, 4);
        for (unsigned i = 0; i < inputs_.size(); ++i) {
            require(inputs_[i][0].length >= largest, "coded buffer too short");
            free_.push_back(i);
        }
        auto type = output;
        call(fd_, VIDIOC_STREAMON, &type, "STREAMON output");
    }
    void feed(const std::vector<std::uint8_t>& unit, unsigned stamp)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (free_.empty() && std::chrono::steady_clock::now() < deadline) pump();
        require(!free_.empty(), "no coded buffer came back");
        const auto index = free_.back();
        free_.pop_back();
        std::memcpy(inputs_[index][0].data, unit.data(), unit.size());
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
        buffer.length = 1; buffer.m.planes = planes;
        buffer.timestamp.tv_sec = 7; buffer.timestamp.tv_usec = stamp;
        planes[0].length = inputs_[index][0].length;
        planes[0].bytesused = unit.size();
        call(fd_, VIDIOC_QBUF, &buffer, "QBUF coded");
        if (!captureOn_) startCapture();
    }
    // A seek: the coded queue stops and starts again, and the capture queue runs on.
    void seek()
    {
        auto type = output;
        call(fd_, VIDIOC_STREAMOFF, &type, "STREAMOFF output");
        call(fd_, VIDIOC_STREAMON, &type, "STREAMON output");
        free_.clear();
        for (unsigned i = 0; i < inputs_.size(); ++i) free_.push_back(i);
    }
    // Drains the decoder and returns every picture it showed.
    const std::vector<Picture>& finish()
    {
        v4l2_decoder_cmd drain {};
        drain.cmd = V4L2_DEC_CMD_STOP;
        call(fd_, VIDIOC_DECODER_CMD, &drain, "DECODER_CMD drain");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!last_ && std::chrono::steady_clock::now() < deadline) pump();
        require(last_, "the decoder did not end the stream");
        return shown_;
    }

private:
    void startCapture()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool source = false;
        while (!source && std::chrono::steady_clock::now() < deadline) {
            pollfd p {fd_, POLLPRI, 0};
            if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll source change failed");
            v4l2_event event {};
            if (ioctl(fd_, VIDIOC_DQEVENT, &event) == 0) source = event.type == V4L2_EVENT_SOURCE_CHANGE;
            else require(errno == EAGAIN, "DQEVENT failed");
        }
        require(source, "decoder source-change timeout");
        v4l2_format format {};
        format.type = capture;
        call(fd_, VIDIOC_G_FMT, &format, "G_FMT capture");
        require(format.fmt.pix_mp.width == 64 && format.fmt.pix_mp.height == 64, "decoded dimensions differ");
        format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
        call(fd_, VIDIOC_S_FMT, &format, "S_FMT capture");
        outputs_ = allocate(fd_, capture, 8);
        for (unsigned i = 0; i < outputs_.size(); ++i) queue(fd_, capture, i, outputs_[i]);
        auto type = capture;
        call(fd_, VIDIOC_STREAMON, &type, "STREAMON capture");
        captureOn_ = true;
    }
    void pump()
    {
        pollfd p {fd_, POLLIN | POLLOUT, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll failed");
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            if (ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) { require(errno == EAGAIN, "DQBUF coded failed"); break; }
            free_.push_back(buffer.index);
        }
        if (!captureOn_) return;
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = capture; buffer.memory = V4L2_MEMORY_MMAP;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            if (ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
                // The queue refuses once its last buffer has been taken.
                require(errno == EAGAIN || (last_ && errno == EPIPE), "DQBUF capture failed");
                break;
            }
            require(!(buffer.flags & V4L2_BUF_FLAG_ERROR), "capture buffer has decoder error");
            require(buffer.index < outputs_.size(), "capture index outside allocation");
            if (buffer.flags & V4L2_BUF_FLAG_LAST) {
                last_ = true;
                if (!planes[0].bytesused) continue;
            }
            if (planes[0].bytesused)
                shown_.push_back({unsigned(buffer.timestamp.tv_usec),
                                  static_cast<const std::uint8_t*>(outputs_[buffer.index][0].data)[planes[0].data_offset]});
            if (!last_) queue(fd_, capture, buffer.index, outputs_[buffer.index]);
        }
    }
    Device owner_;
    int fd_;
    Buffers inputs_, outputs_;
    std::vector<unsigned> free_;
    std::vector<Picture> shown_;
    bool captureOn_ {}, last_ {};
};
/* The same for H.264, whose pictures also come back as decoded: B-pictures and
 * reference B-pictures in groups the stream's own limits describe, the same
 * stream with the limits left to the level, a stream that cannot reorder, and a
 * seek to an IDR picture whose parameter sets the decoder already has. */
void h264DisplayOrder(const char* device, const std::string& directory)
{
    static const unsigned closedDisplay[] = {0, 4, 2, 1, 3, 8, 6, 5, 7, 11, 9, 10, 12, 16, 14, 13, 15, 19, 17, 18};
    for (const char* name : {"h264-bframes.bin", "h264-no-restriction.bin"}) {
        const auto units = accessUnits(read(directory + "/" + name), true);
        require(units.size() == 20, "the H.264 B-picture fixture is not twenty access units");
        std::size_t largest = 0;
        for (const auto& unit : units) largest = std::max(largest, unit.size());
        OrderRun run(device, largest, V4L2_PIX_FMT_H264);
        for (unsigned i = 0; i < units.size(); ++i) run.feed(units[i], i + 1);
        const auto& shown = run.finish();
        require(shown.size() == 20, "the decoder did not return every H.264 picture");
        for (unsigned i = 0; i < shown.size(); ++i) {
            require(shown[i].stamp >= 1 && shown[i].stamp <= 20 && closedDisplay[shown[i].stamp - 1] == i,
                    "H.264 pictures are not returned in display order");
            require(std::abs(shown[i].luma - int(16 + 10 * i)) <= 3, "an H.264 picture is not the one its timestamp names");
        }
        std::printf("PASS h264 display order: %s, 20 pictures\n", name);
        std::fflush(stdout);
    }
    {
        const auto units = accessUnits(read(directory + "/h264-poc2.bin"), true);
        std::size_t largest = 0;
        for (const auto& unit : units) largest = std::max(largest, unit.size());
        OrderRun run(device, largest, V4L2_PIX_FMT_H264);
        for (unsigned i = 0; i < units.size(); ++i) run.feed(units[i], i + 1);
        const auto& shown = run.finish();
        require(shown.size() == units.size(), "the decoder did not return every picture of a stream without reordering");
        for (unsigned i = 0; i < shown.size(); ++i)
            require(shown[i].stamp == i + 1 && std::abs(shown[i].luma - int(16 + 10 * i)) <= 3,
                    "a stream without reordering is shown as decoded");
        std::puts("PASS h264 order count type 2: pictures as decoded");
        std::fflush(stdout);
    }
    {
        const auto units = accessUnits(read(directory + "/h264-bframes.bin"), true);
        std::size_t largest = 0;
        for (const auto& unit : units) largest = std::max(largest, unit.size());
        OrderRun run(device, largest, V4L2_PIX_FMT_H264);
        for (unsigned i = 0; i < 7; ++i) run.feed(units[i], i + 1);
        run.seek();
        for (unsigned i = 12; i < units.size(); ++i) run.feed(i == 12 ? withoutParameterSets(units[i], true) : units[i], i + 1);
        const auto& shown = run.finish();
        std::vector<OrderRun::Picture> after;
        for (const auto& picture : shown) if (picture.stamp >= 13) after.push_back(picture);
        require(after.size() == 8, "the pictures after an H.264 seek are not those of the IDR picture");
        for (unsigned i = 0; i < after.size(); ++i)
            require(std::abs(after[i].luma - int(16 + 10 * (12 + i))) <= 3,
                    "H.264 pictures after a seek are not in display order");
        std::puts("PASS h264 seek: 8 pictures after a seek");
        std::fflush(stdout);
    }
}

/* Pictures with B-pictures leave in the order they are shown, whatever order
 * they were decoded in. The pictures of these streams are flat gray that rises
 * by 10 levels from one display position to the next, so the brightness of a
 * returned picture names its display position and its timestamp the order it
 * was decoded in. After a seek to a random access point whose leading pictures
 * need pictures the decoder never had, those are not shown. */
void hevcDisplayOrder(const char* device, const std::string& directory)
{
    static const unsigned closedDisplay[] = {0, 4, 2, 1, 3, 8, 6, 5, 7, 11, 10, 9, 12, 16, 14, 13, 15, 19, 18, 17};
    const auto closed = accessUnits(read(directory + "/hevc-bframes.bin"));
    require(closed.size() == 20, "the B-picture fixture is not twenty access units");
    std::size_t largest = 0;
    for (const auto& unit : closed) largest = std::max(largest, unit.size());
    {
        OrderRun run(device, largest, V4L2_PIX_FMT_HEVC);
        for (unsigned i = 0; i < closed.size(); ++i) run.feed(closed[i], i + 1);
        const auto& shown = run.finish();
        require(shown.size() == 20, "the decoder did not return every picture");
        for (unsigned i = 0; i < shown.size(); ++i) {
            require(shown[i].stamp >= 1 && shown[i].stamp <= 20 && closedDisplay[shown[i].stamp - 1] == i,
                    "pictures are not returned in display order");
            require(std::abs(shown[i].luma - int(16 + 10 * i)) <= 3, "a picture is not the one its timestamp names");
        }
        std::puts("PASS hevc display order: 20 pictures with B-pictures");
        std::fflush(stdout);
    }
    const auto open = accessUnits(read(directory + "/hevc-open-gop.bin"));
    require(open.size() == 24, "the open-group fixture is not twenty-four access units");
    largest = 0;
    for (const auto& unit : open) largest = std::max(largest, unit.size());
    {
        /* The first nine access units, a seek, then the second CRA picture and
         * what follows, without the parameter sets the decoder has already. The
         * three pictures that lead the CRA are not shown. */
        OrderRun run(device, largest, V4L2_PIX_FMT_HEVC);
        for (unsigned i = 0; i < 9; ++i) run.feed(open[i], i + 1);
        run.seek();
        for (unsigned i = 13; i < open.size(); ++i) run.feed(withoutParameterSets(open[i]), i + 1);
        const auto& shown = run.finish();
        std::vector<OrderRun::Picture> after;
        for (const auto& picture : shown) if (picture.stamp >= 14) after.push_back(picture);
        require(after.size() == 8, "the pictures after a seek are not those of the random access point");
        for (unsigned i = 0; i < after.size(); ++i)
            require(std::abs(after[i].luma - int(16 + 10 * (16 + i))) <= 3,
                    "pictures after a seek are not in display order");
        std::puts("PASS hevc seek: 8 pictures after a seek, leading pictures skipped");
        std::fflush(stdout);
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
        /* A stream whose picture size changes twice, decoded as a stateful
         * client does it. */
        for (unsigned index = 0;; ++index) {
            v4l2_fmtdesc fmt {};
            fmt.type = output; fmt.index = index;
            if (ioctl(capabilities.fd, VIDIOC_ENUM_FMT, &fmt) < 0) {
                require(errno == EINVAL, "ENUM_FMT failed"); break;
            }
            if (fmt.pixelformat != V4L2_PIX_FMT_H264) continue;
            resolutionChange(device, argv[1], fmt.pixelformat);
            resolutionChange(device, argv[1], fmt.pixelformat, V4L2_MEMORY_DMABUF);
            resolutionChange(device, argv[1], fmt.pixelformat, V4L2_MEMORY_DMABUF,
                             Import{true, false, Declared::negotiated});
            resolutionChange(device, argv[1], fmt.pixelformat, V4L2_MEMORY_MMAP, {}, true);
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
                    importGuestPages(device, argv[1], profile->name, fmt.pixelformat, profile->raw);
                    decode(device, argv[1], profile->name, fmt.pixelformat, V4L2_PIX_FMT_ABGR32,
                           V4L2_MEMORY_DMABUF, Import{true, false, Declared::negotiated});
                    imported = true;
                }
                if (profile->raw == V4L2_PIX_FMT_P010)
                    decode(device, argv[1], profile->name, fmt.pixelformat, profile->raw,
                           V4L2_MEMORY_DMABUF, Import{true, false, Declared::negotiated});
                ++tested;
                if (avc && profile->value == V4L2_MPEG_VIDEO_H264_PROFILE_HIGH) {
                    decode(device, argv[1], profile->name, fmt.pixelformat, V4L2_PIX_FMT_YUV420M);
                    /* Three planes in one dma-buf, the last two starting off a page boundary. */
                    decode(device, argv[1], profile->name, fmt.pixelformat, V4L2_PIX_FMT_YUV420M,
                           V4L2_MEMORY_DMABUF, Import{true, true, Declared::negotiated});
                }
                // A 10-bit stream is also captured as the 8-bit planes Android asks for.
                if (profile->raw == V4L2_PIX_FMT_P010)
                    decode(device, argv[1], profile->name, fmt.pixelformat, V4L2_PIX_FMT_NV12M);
            }
            if (avc) h264DisplayOrder(device, argv[1]);
            else hevcDisplayOrder(device, argv[1]);
        }
        require(tested > 0, "decoder advertised no supported profiles");
        std::puts("UML_V4L2_DECODE: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
}
