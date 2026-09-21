// SPDX-License-Identifier: GPL-2.0-or-later
// A VA-API client that decodes a VP9 IVF stream the way Chromium's
// VaapiVideoDecoder does: one submission per frame of a superframe, the whole
// compressed frame as the slice data, hidden frames submitted but not output.
// It writes the shown frames as planar I420 so that the result can be compared
// byte for byte with a reference decode.
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <va/va.h>
#include <va/va_dec_vp9.h>
#include <va/va_str.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

#include "../vp9.hpp"

namespace {

bool check(VAStatus status, const char* what)
{
    if (status == VA_STATUS_SUCCESS) return true;
    std::fprintf(stderr, "%s: %s\n", what, vaErrorStr(status));
    return false;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t seed = 0) noexcept
{
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320u & (~(value & 1u) + 1u));
            table[index] = value;
        }
        ready = true;
    }
    std::uint32_t value = ~seed;
    for (std::size_t index = 0; index < size; ++index)
        value = (value >> 8) ^ table[(value ^ data[index]) & 0xff];
    return ~value;
}

std::vector<std::vector<std::uint8_t>> readIvf(const char* path, unsigned& width, unsigned& height)
{
    std::vector<std::vector<std::uint8_t>> packets;
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return packets;
    std::uint8_t header[32];
    if (std::fread(header, 1, 32, file) != 32 || std::memcmp(header, "DKIF", 4) != 0) {
        std::fclose(file);
        return packets;
    }
    width = unsigned(header[12]) | (unsigned(header[13]) << 8);
    height = unsigned(header[14]) | (unsigned(header[15]) << 8);
    std::uint8_t frame[12];
    while (std::fread(frame, 1, 12, file) == 12) {
        const std::size_t size = std::size_t(frame[0]) | (std::size_t(frame[1]) << 8) |
                                 (std::size_t(frame[2]) << 16) | (std::size_t(frame[3]) << 24);
        std::vector<std::uint8_t> packet(size);
        if (std::fread(packet.data(), 1, size, file) != size) break;
        packets.push_back(std::move(packet));
    }
    std::fclose(file);
    return packets;
}

struct Output {
    std::FILE* file {};
    std::uint32_t running {};
    unsigned frames {};
    // Plane means, so two decode paths that cannot be compared byte for byte -
    // a packed decode has been through the host's own colour conversion - can
    // still be compared as pictures.
    double sum[3] {};
    std::uint64_t samples[3] {};
};

bool writeI420(Output& output, const std::uint8_t* base, const VAImage& image, unsigned width, unsigned height)
{
    std::vector<std::uint8_t> row(width);
    for (unsigned line = 0; line < height; ++line) {
        const std::uint8_t* source = base + image.offsets[0] + std::size_t(line) * image.pitches[0];
        output.running = crc32(source, width, output.running);
        for (unsigned column = 0; column < width; ++column) output.sum[0] += source[column];
        output.samples[0] += width;
        if (output.file && std::fwrite(source, 1, width, output.file) != width) return false;
    }
    const unsigned chromaWidth = (width + 1) / 2;
    const unsigned chromaHeight = (height + 1) / 2;
    for (unsigned plane = 0; plane < 2; ++plane) {
        for (unsigned line = 0; line < chromaHeight; ++line) {
            const std::uint8_t* source = base + image.offsets[1] + std::size_t(line) * image.pitches[1];
            for (unsigned column = 0; column < chromaWidth; ++column) row[column] = source[2 * column + plane];
            output.running = crc32(row.data(), chromaWidth, output.running);
            for (unsigned column = 0; column < chromaWidth; ++column) output.sum[plane + 1] += row[column];
            output.samples[plane + 1] += chromaWidth;
            if (output.file && std::fwrite(row.data(), 1, chromaWidth, output.file) != chromaWidth) return false;
        }
    }
    ++output.frames;
    return true;
}

}

int main(int argc, char** argv)
{
    const char* stream = nullptr;
    const char* destination = nullptr;
    const char* node = nullptr;
    unsigned limit = 0;
    bool videoProcess = false;
    bool exportCheck = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--out" && index + 1 < argc) destination = argv[++index];
        else if (argument == "--device" && index + 1 < argc) node = argv[++index];
        else if (argument == "--frames" && index + 1 < argc) limit = unsigned(std::atoi(argv[++index]));
        else if (argument == "--vpp") videoProcess = true;
        else if (argument == "--export") exportCheck = true;
        else if (!stream) stream = argv[index];
    }
    if (!stream) {
        std::fprintf(stderr,
                     "usage: va_decode_probe STREAM.ivf [--out FILE|-] [--device NODE] [--frames N]"
                     " [--vpp] [--export]\n");
        return 2;
    }
    unsigned width = 0;
    unsigned height = 0;
    const auto packets = readIvf(stream, width, height);
    if (packets.empty() || !width || !height) {
        std::fprintf(stderr, "cannot read %s as IVF\n", stream);
        return 2;
    }
    std::fprintf(stderr, "stream %s %ux%u packets %zu\n", stream, width, height, packets.size());

    // The driver ignores the device a VA display names, so any node libva
    // accepts will do when a guest has no render node.
    const char* candidates[] {node, "/dev/dri/renderD128", "/dev/dri/card0", "/dev/video-dec0", "/dev/video0"};
    int drm = -1;
    VADisplay display = nullptr;
    int major = 0;
    int minor = 0;
    for (const char* candidate : candidates) {
        if (!candidate || !*candidate) continue;
        drm = ::open(candidate, O_RDWR | O_CLOEXEC);
        if (drm < 0) continue;
        display = vaGetDisplayDRM(drm);
        if (display && vaInitialize(display, &major, &minor) == VA_STATUS_SUCCESS) {
            std::fprintf(stderr, "display %s\n", candidate);
            break;
        }
        if (display) vaTerminate(display);
        display = nullptr;
        ::close(drm);
        drm = -1;
    }
    if (!display) { std::fprintf(stderr, "no VA display\n"); return 2; }
    std::fprintf(stderr, "libva %d.%d vendor \"%s\"\n", major, minor, vaQueryVendorString(display));

    std::vector<VAProfile> profiles(std::size_t(vaMaxNumProfiles(display)));
    int profileCount = 0;
    if (!check(vaQueryConfigProfiles(display, profiles.data(), &profileCount), "vaQueryConfigProfiles")) return 1;
    bool vp9 = false;
    for (int index = 0; index < profileCount; ++index) {
        std::fprintf(stderr, "profile %s\n", vaProfileStr(profiles[index]));
        vp9 = vp9 || profiles[index] == VAProfileVP9Profile0;
    }
    if (!vp9) { std::fprintf(stderr, "the driver does not offer VP9 profile 0\n"); return 1; }

    VAConfigAttrib attribute {};
    attribute.type = VAConfigAttribRTFormat;
    if (!check(vaGetConfigAttributes(display, VAProfileVP9Profile0, VAEntrypointVLD, &attribute, 1),
               "vaGetConfigAttributes")) return 1;
    if (!(attribute.value & VA_RT_FORMAT_YUV420)) {
        std::fprintf(stderr, "VP9 profile 0 does not offer YUV420\n");
        return 1;
    }
    attribute.value = VA_RT_FORMAT_YUV420;
    VAConfigID config = VA_INVALID_ID;
    if (!check(vaCreateConfig(display, VAProfileVP9Profile0, VAEntrypointVLD, &attribute, 1, &config),
               "vaCreateConfig")) return 1;

    unsigned attributeCount = 0;
    vaQuerySurfaceAttributes(display, config, nullptr, &attributeCount);
    std::vector<VASurfaceAttrib> attributes(attributeCount);
    if (attributeCount &&
        check(vaQuerySurfaceAttributes(display, config, attributes.data(), &attributeCount),
              "vaQuerySurfaceAttributes")) {
        for (const auto& entry : attributes)
            if (entry.type == VASurfaceAttribMaxWidth || entry.type == VASurfaceAttribMaxHeight)
                std::fprintf(stderr, "limit %s %d\n",
                            entry.type == VASurfaceAttribMaxWidth ? "max-width" : "max-height",
                            entry.value.value.i);
    }

    constexpr unsigned kSurfaces = 12;
    VASurfaceID surfaces[kSurfaces];
    VASurfaceAttrib surfaceAttribute {};
    surfaceAttribute.type = VASurfaceAttribPixelFormat;
    surfaceAttribute.flags = VA_SURFACE_ATTRIB_SETTABLE;
    surfaceAttribute.value.type = VAGenericValueTypeInteger;
    surfaceAttribute.value.value.i = VA_FOURCC_NV12;
    if (!check(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height, surfaces, kSurfaces,
                                &surfaceAttribute, 1), "vaCreateSurfaces")) return 1;
    VAContextID context = VA_INVALID_ID;
    if (!check(vaCreateContext(display, config, int(width), int(height), VA_PROGRESSIVE, surfaces, kSurfaces,
                               &context), "vaCreateContext")) return 1;

    // The same video processing Chromium's image processor asks for: a
    // VAProfileNone/VAEntrypointVideoProc context with a 0x0 extent, one
    // pipeline parameter buffer per frame, an ARGB destination surface.
    VAConfigID vppConfig = VA_INVALID_ID;
    VAContextID vppContext = VA_INVALID_ID;
    VASurfaceID argb = VA_INVALID_SURFACE;
    std::uint32_t argbCrc = 0;
    double argbSum[3] {};
    std::uint64_t argbSamples = 0;
    double blitSeconds = 0;
    if (videoProcess) {
        VAConfigAttrib vppAttribute {};
        vppAttribute.type = VAConfigAttribRTFormat;
        if (!check(vaGetConfigAttributes(display, VAProfileNone, VAEntrypointVideoProc, &vppAttribute, 1),
                   "vaGetConfigAttributes vpp")) return 1;
        if (!(vppAttribute.value & VA_RT_FORMAT_RGB32)) {
            std::fprintf(stderr, "video processing does not offer RGB32\n");
            return 1;
        }
        vppAttribute.value = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32;
        if (!check(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, &vppAttribute, 1, &vppConfig),
                   "vaCreateConfig vpp")) return 1;
        if (!check(vaCreateContext(display, vppConfig, 0, 0, 0, nullptr, 0, &vppContext),
                   "vaCreateContext vpp")) return 1;
        VASurfaceAttrib rgb {};
        rgb.type = VASurfaceAttribPixelFormat;
        rgb.flags = VA_SURFACE_ATTRIB_SETTABLE;
        rgb.value.type = VAGenericValueTypeInteger;
        rgb.value.value.i = VA_FOURCC_ARGB;
        if (!check(vaCreateSurfaces(display, VA_RT_FORMAT_RGB32, width, height, &argb, 1, &rgb, 1),
                   "vaCreateSurfaces argb")) return 1;
    }

    Output output;
    if (destination) output.file = std::strcmp(destination, "-") == 0 ? stdout : std::fopen(destination, "wb");
    if (destination && !output.file) { std::perror("open output"); return 2; }

    uml::va::Vp9Context parser;
    unsigned next = 0;
    unsigned submitted = 0;
    unsigned shown = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& packet : packets) {
        for (const auto frame : uml::va::vp9SuperframeParts(packet)) {
            const auto header = parser.parse(frame);
            if (!header.valid) { std::fprintf(stderr, "unparsable frame %u\n", submitted); return 1; }
            if (header.showExistingFrame) continue; // Chromium shows it without the accelerator.
            const VASurfaceID target = surfaces[next++ % kSurfaces];

            VADecPictureParameterBufferVP9 picture {};
            picture.frame_width = static_cast<std::uint16_t>(header.width);
            picture.frame_height = static_cast<std::uint16_t>(header.height);
            for (auto& reference : picture.reference_frames) reference = VA_INVALID_SURFACE;
            picture.pic_fields.bits.frame_type = header.keyFrame ? 0 : 1;
            picture.pic_fields.bits.show_frame = header.showFrame ? 1 : 0;
            picture.pic_fields.bits.intra_only = header.intraOnly ? 1 : 0;
            picture.pic_fields.bits.error_resilient_mode = header.errorResilient ? 1 : 0;
            picture.profile = static_cast<std::uint8_t>(header.profile);
            picture.bit_depth = static_cast<std::uint8_t>(header.bitDepth);
            VASliceParameterBufferVP9 slice {};
            slice.slice_data_size = static_cast<std::uint32_t>(frame.size());
            slice.slice_data_offset = 0;
            slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

            VABufferID buffers[3] {VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID};
            if (!check(vaCreateBuffer(display, context, VAPictureParameterBufferType, sizeof(picture), 1,
                                      &picture, &buffers[0]), "vaCreateBuffer picture")) return 1;
            if (!check(vaCreateBuffer(display, context, VASliceParameterBufferType, sizeof(slice), 1, &slice,
                                      &buffers[1]), "vaCreateBuffer slice")) return 1;
            if (!check(vaCreateBuffer(display, context, VASliceDataBufferType, unsigned(frame.size()), 1,
                                      const_cast<std::uint8_t*>(frame.data()), &buffers[2]),
                       "vaCreateBuffer data")) return 1;
            if (!check(vaBeginPicture(display, context, target), "vaBeginPicture")) return 1;
            if (!check(vaRenderPicture(display, context, buffers, 3), "vaRenderPicture")) return 1;
            if (!check(vaEndPicture(display, context), "vaEndPicture")) return 1;
            for (const auto id : buffers) vaDestroyBuffer(display, id);
            ++submitted;

            if (!header.showFrame) continue;
            if (!check(vaSyncSurface(display, target), "vaSyncSurface")) return 1;
            VAImage image {};
            if (!check(vaDeriveImage(display, target, &image), "vaDeriveImage")) return 1;
            void* address = nullptr;
            if (!check(vaMapBuffer(display, image.buf, &address), "vaMapBuffer")) return 1;
            if (!writeI420(output, static_cast<const std::uint8_t*>(address), image, header.width, header.height)) {
                std::fprintf(stderr, "cannot write the decoded frame\n");
                return 1;
            }
            vaUnmapBuffer(display, image.buf);
            vaDestroyImage(display, image.image_id);

            if (videoProcess) {
                VAProcPipelineParameterBuffer pipeline {};
                VARectangle source {0, 0, static_cast<std::uint16_t>(header.width),
                                    static_cast<std::uint16_t>(header.height)};
                VARectangle sink = source;
                pipeline.surface = target;
                pipeline.surface_region = &source;
                pipeline.output_region = &sink;
                pipeline.output_background_color = 0xff000000;
                pipeline.filter_flags = VA_FILTER_SCALING_DEFAULT;
                const auto blitStart = std::chrono::steady_clock::now();
                VABufferID parameters = VA_INVALID_ID;
                if (!check(vaCreateBuffer(display, vppContext, VAProcPipelineParameterBufferType,
                                          sizeof(pipeline), 1, &pipeline, &parameters),
                           "vaCreateBuffer pipeline")) return 1;
                if (!check(vaBeginPicture(display, vppContext, argb), "vaBeginPicture vpp")) return 1;
                if (!check(vaRenderPicture(display, vppContext, &parameters, 1), "vaRenderPicture vpp")) return 1;
                if (!check(vaEndPicture(display, vppContext), "vaEndPicture vpp")) return 1;
                vaDestroyBuffer(display, parameters);
                blitSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - blitStart).count();
                VAImage converted {};
                if (!check(vaDeriveImage(display, argb, &converted), "vaDeriveImage argb")) return 1;
                void* pixels = nullptr;
                if (!check(vaMapBuffer(display, converted.buf, &pixels), "vaMapBuffer argb")) return 1;
                const auto* bytes = static_cast<const std::uint8_t*>(pixels) + converted.offsets[0];
                for (unsigned line = 0; line < header.height; ++line) {
                    const std::uint8_t* row = bytes + std::size_t(line) * converted.pitches[0];
                    for (unsigned column = 0; column < width; ++column) {
                        argbSum[0] += row[std::size_t(column) * 4 + 0];
                        argbSum[1] += row[std::size_t(column) * 4 + 1];
                        argbSum[2] += row[std::size_t(column) * 4 + 2];
                    }
                    argbSamples += width;
                    argbCrc = crc32(row, std::size_t(width) * 4, argbCrc);
                }
                vaUnmapBuffer(display, converted.buf);
                vaDestroyImage(display, converted.image_id);
            }

            if (exportCheck && shown == 0) {
                VADRMPRIMESurfaceDescriptor descriptor {};
                if (!check(vaSyncSurface(display, target), "vaSyncSurface export")) return 1;
                if (!check(vaExportSurfaceHandle(display, target, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                                 VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                                 &descriptor), "vaExportSurfaceHandle")) return 1;
                std::fprintf(stderr, "export objects %u layers %u fourcc %08x size %u modifier %llx"
                             " pitch %u/%u offset %u/%u\n",
                             descriptor.num_objects, descriptor.num_layers, descriptor.fourcc,
                             descriptor.objects[0].size,
                             static_cast<unsigned long long>(descriptor.objects[0].drm_format_modifier),
                             descriptor.layers[0].pitch[0], descriptor.layers[1].pitch[0],
                             descriptor.layers[0].offset[0], descriptor.layers[1].offset[0]);
                if (descriptor.num_objects != 1) {
                    std::fprintf(stderr, "an exported surface must be one object\n");
                    return 1;
                }
                // Import it back the way Chromium's image processor does.
                VASurfaceAttrib importAttributes[2] {};
                importAttributes[0].type = VASurfaceAttribMemoryType;
                importAttributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
                importAttributes[0].value.type = VAGenericValueTypeInteger;
                importAttributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
                importAttributes[1].type = VASurfaceAttribExternalBufferDescriptor;
                importAttributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
                importAttributes[1].value.type = VAGenericValueTypePointer;
                importAttributes[1].value.value.p = &descriptor;
                VASurfaceID imported = VA_INVALID_SURFACE;
                if (!check(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height, &imported, 1,
                                            importAttributes, 2), "vaCreateSurfaces import")) return 1;
                VAImage back {};
                if (!check(vaDeriveImage(display, imported, &back), "vaDeriveImage import")) return 1;
                void* address2 = nullptr;
                if (!check(vaMapBuffer(display, back.buf, &address2), "vaMapBuffer import")) return 1;
                Output check_ {};
                writeI420(check_, static_cast<const std::uint8_t*>(address2), back, header.width, header.height);
                std::fprintf(stderr, "import crc32 %08x (decoded %08x)\n", check_.running, output.running);
                if (check_.running != output.running) {
                    std::fprintf(stderr, "an imported surface must show the same pixels\n");
                    return 1;
                }
                vaUnmapBuffer(display, back.buf);
                vaDestroyImage(display, back.image_id);
                vaDestroySurfaces(display, &imported, 1);
                ::close(descriptor.objects[0].fd);
            }
            ++shown;
            if (limit && shown >= limit) break;
        }
        if (limit && shown >= limit) break;
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (output.file && output.file != stdout) std::fclose(output.file);
    std::fprintf(stderr, "plane means Y %.3f U %.3f V %.3f\n",
                 output.samples[0] ? output.sum[0] / double(output.samples[0]) : 0.0,
                 output.samples[1] ? output.sum[1] / double(output.samples[1]) : 0.0,
                 output.samples[2] ? output.sum[2] / double(output.samples[2]) : 0.0);
    std::fprintf(stderr, "submitted %u frames, %u shown, crc32 %08x, %.2f s (%.1f fps)\n", submitted, shown,
                 output.running, elapsed, elapsed > 0 ? shown / elapsed : 0.0);
    if (videoProcess)
        std::fprintf(stderr, "argb crc32 %08x, means B %.3f G %.3f R %.3f, %.2f s of blits (%.0f ms each)\n",
                     argbCrc, argbSamples ? argbSum[0] / double(argbSamples) : 0.0,
                     argbSamples ? argbSum[1] / double(argbSamples) : 0.0,
                     argbSamples ? argbSum[2] / double(argbSamples) : 0.0, blitSeconds,
                     shown ? 1000.0 * blitSeconds / shown : 0.0);
    if (argb != VA_INVALID_SURFACE) vaDestroySurfaces(display, &argb, 1);
    if (vppContext != VA_INVALID_ID) vaDestroyContext(display, vppContext);
    if (vppConfig != VA_INVALID_ID) vaDestroyConfig(display, vppConfig);
    vaDestroyContext(display, context);
    vaDestroySurfaces(display, surfaces, kSurfaces);
    vaDestroyConfig(display, config);
    vaTerminate(display);
    ::close(drm);
    std::fprintf(stderr, "UML_VA_DECODE: %s\n", shown ? "PASS" : "FAIL");
    return shown ? 0 : 1;
}
