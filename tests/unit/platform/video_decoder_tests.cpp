// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_decoder.hpp"
#include "require.hpp"

#include <algorithm>
#include <utility>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
std::string fixtureDirectory;

std::vector<std::uint8_t> read(const std::string& name)
{
    const auto path = fixtureDirectory + "/" + name;
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("missing video fixture: " + path);
    return {std::istreambuf_iterator<char>(file), {}};
}
using uml::testing::require;
}

int main(int argc, char** argv)
{
    using namespace uml::platform;
    /* The registration names the fixture directory, so the binary carries no
     * source path and runs wherever a copy of the fixtures is. */
    if (argc != 2) {
        std::fprintf(stderr, "usage: uml_video_decoder_tests FIXTURE_DIRECTORY\n"
                             "  FIXTURE_DIRECTORY holds a copy of tests/support/video\n");
        return 2;
    }
    fixtureDirectory = argv[1];
    try {
        const auto capabilities = videoDecodeCapabilities();
        require(!capabilities.empty(), "no hardware video decoder available");
        VideoColorimetry colorimetry[16] {};
        const char* names[] = {"h264-baseline", "h264-constrained-baseline", "h264-main",
            "h264-high", "h264-high10", "hevc-main", "hevc-main10", "jpeg", "vp9-profile0"};
        for (const auto& cap : capabilities) {
            const char* name = names[unsigned(cap.profile)];
            std::printf("Decoding %s\n", name);
            std::fflush(stdout);
            auto decoder = makeVideoDecoder(cap.codec);
            const auto encoded = read(std::string(name) + ".bin");
            decoder->submit(encoded, 123456789);
            decoder->drain();
            auto frames = decoder->takeFrames();
            require(frames.size() == 1, "decoder did not produce exactly one frame");
            const auto& frame = frames[0];
            require(frame.timestamp == 123456789, "decoder changed the frame timestamp");
            require(frame.format == VideoFormat{128, 96, cap.bitDepth}, "decoder output format differs");
            /* The hardware states which description it applied. These fixtures
             * carry no color description and are below the standard-definition
             * line, so the answer is limited-range BT.601; the point of
             * reading it back is that the conversion below never guesses. */
            require(frame.color.matrix == VideoColorMatrix::bt601 && !frame.color.fullRange,
                    "decoder reported an unexpected color description");
            colorimetry[unsigned(cap.profile)] = frame.color;
            const auto reference = read(std::string(name) + ".raw");
            std::vector<std::uint8_t> actual = frame.luma;
            actual.insert(actual.end(), frame.chroma.begin(), frame.chroma.end());
            require(actual.size() == reference.size(), "decoder output plane sizes differ");
            unsigned maximum = 0;
            std::uint64_t sum = 0;
            const unsigned step = cap.bitDepth == 10 ? 2 : 1;
            for (std::size_t i = 0; i < actual.size(); i += step) {
                const int got = step == 1 ? actual[i] : (actual[i] | (actual[i + 1] << 8)) >> 6;
                const int wanted = step == 1 ? reference[i] : (reference[i] | (reference[i + 1] << 8)) >> 6;
                const unsigned difference = unsigned(std::abs(got - wanted));
                maximum = std::max(maximum, difference);
                sum += difference;
            }
            std::printf("%s: max pixel error %u, total %llu\n", name, maximum,
                        static_cast<unsigned long long>(sum));
            require(maximum <= (cap.codec == VideoCodec::jpeg ? 4u : 2u),
                    "hardware decoded pixels differ from the reference");
            require(decoder->takeFrames().empty(), "decoder returned a frame twice");
        }
        /* The same fixtures decoded straight to packed BGRA. A client that
         * draws through a 32-bit RGB surface asks for this so that no frame is
         * converted by a processor, and the reference is the planar fixture
         * taken through the limited-range BT.601 matrix a frame this small
         * carries when the stream states nothing. */
        for (const auto& cap : capabilities) {
            const char* name = names[unsigned(cap.profile)];
            auto decoder = makeVideoDecoder(cap.codec);
            decoder->requestOutputLayout(VideoPixelLayout::packedBgra);
            decoder->submit(read(std::string(name) + ".bin"), 123456789);
            decoder->drain();
            auto frames = decoder->takeFrames();
            require(frames.size() == 1, "packed decoder did not produce exactly one frame");
            const auto& frame = frames[0];
            require(frame.layout == VideoPixelLayout::packedBgra, "decoder did not produce packed pixels");
            require(frame.format == VideoFormat{128, 96, 8}, "packed output format differs");
            require(frame.chroma.empty() && frame.luma.empty(), "packed frame also carries planes");
            require(frame.pixels.size() == 128u * 96u * 4u, "packed frame size differs");
            const auto reference = read(std::string(name) + ".raw");
            const unsigned step = cap.bitDepth == 10 ? 2 : 1;
            const auto sample = [&](std::size_t index) {
                return int(reference[step * index + step - 1]);
            };
            /* Converted with the description the hardware reported for the
             * planar decode of this same fixture, so the two paths cannot
             * disagree about matrix or range. */
            const auto color = colorimetry[unsigned(cap.profile)];
            double kr = 0.2126, kb = 0.0722;
            if (color.matrix == VideoColorMatrix::bt601) { kr = 0.299; kb = 0.114; }
            else if (color.matrix == VideoColorMatrix::bt2020) { kr = 0.2627; kb = 0.0593; }
            const double kg = 1.0 - kr - kb;
            const double lumaGain = color.fullRange ? 1.0 : 255.0 / 219.0;
            const double lumaFloor = color.fullRange ? 0.0 : 16.0;
            const double chromaGain = color.fullRange ? 1.0 : 255.0 / 224.0;
            const double vToR = chromaGain * 2.0 * (1.0 - kr);
            const double uToB = chromaGain * 2.0 * (1.0 - kb);
            const double uToG = chromaGain * 2.0 * kb * (1.0 - kb) / kg;
            const double vToG = chromaGain * 2.0 * kr * (1.0 - kr) / kg;
            /* Hardware interpolates the subsampled chroma field, so only a
             * pixel whose chroma neighborhood is constant has one defined
             * answer. Everywhere else the two upsamplings legitimately
             * disagree, and the reference cannot say which is right. */
            const auto chroma = [&](std::ptrdiff_t cellRow, std::ptrdiff_t cellColumn) {
                cellRow = std::clamp<std::ptrdiff_t>(cellRow, 0, 47);
                cellColumn = std::clamp<std::ptrdiff_t>(cellColumn, 0, 63);
                const auto pair = std::size_t(128 * 96 + cellRow * 128 + cellColumn * 2);
                return std::pair{sample(pair), sample(pair + 1)};
            };
            unsigned maximum = 0, checked = 0;
            for (std::size_t row = 0; row < 96; ++row) {
                for (std::size_t column = 0; column < 128; ++column) {
                    const auto* got = frame.pixels.data() + (row * 128 + column) * 4;
                    require(got[3] == 0xff, "packed frame is not opaque");
                    const auto cell = chroma(std::ptrdiff_t(row / 2), std::ptrdiff_t(column / 2));
                    bool flat = true;
                    for (std::ptrdiff_t dy = -1; dy <= 1; ++dy)
                        for (std::ptrdiff_t dx = -1; dx <= 1; ++dx)
                            flat = flat && chroma(std::ptrdiff_t(row / 2) + dy,
                                                  std::ptrdiff_t(column / 2) + dx) == cell;
                    if (!flat) continue;
                    ++checked;
                    const double luma = lumaGain * (sample(row * 128 + column) - lumaFloor);
                    const double u = cell.first - 128, v = cell.second - 128;
                    const double wanted[3] = {luma + uToB * u, luma - uToG * u - vToG * v,
                                              luma + vToR * v};
                    for (unsigned channel = 0; channel < 3; ++channel)
                        maximum = std::max(maximum, unsigned(std::abs(
                            int(got[channel]) - int(std::clamp(wanted[channel], 0.0, 255.0) + 0.5))));
                }
            }
            std::printf("%s packed: max channel error %u over %u pixels\n", name, maximum, checked);
            std::fflush(stdout);
            require(checked >= 128 * 96 / 8, "too little of the packed frame could be verified");
            require(maximum <= 8, "packed pixels differ from the converted reference");
        }
        /* A V4L2 client negotiates its capture format only after the decoder
         * has reported the stream's size, which is the middle of a group of
         * pictures. Asking there must not disturb decoding: restarting the
         * session would throw away the reference frames the next picture needs
         * and the hardware would answer kVTVideoDecoderBadDataErr. */
        for (const auto& cap : capabilities) {
            const char* name = names[unsigned(cap.profile)];
            const auto encoded = read(std::string(name) + ".bin");
            auto decoder = makeVideoDecoder(cap.codec);
            decoder->submit(encoded, 1);
            require(decoder->format().width == 128, "decoder did not report the stream size");
            decoder->requestOutputLayout(VideoPixelLayout::packedBgra);
            decoder->submit(encoded, 2);
            decoder->drain();
            auto frames = decoder->takeFrames();
            require(frames.size() == 2, "late format negotiation lost a frame");
            for (const auto& frame : frames)
                require(!frame.pixels.empty() || !frame.luma.empty(), "late negotiation emptied a frame");
            /* Each fixture is a single random access picture, so the request
             * takes effect on the next one. A stream whose next picture needs
             * an earlier one keeps its layout until the next such point, which
             * every frame states and the caller converts. */
            require(frames[1].layout == VideoPixelLayout::packedBgra,
                    "a random access picture did not adopt the requested layout");
            require(frames[0].layout == VideoPixelLayout::planarYuv,
                    "a frame decoded before the request changed layout");
        }
        /* The order the device uses so that nothing has to be converted: read
         * the size out of the packet's headers, report it to the client, and
         * submit the same packet once the client has chosen a layout. Because
         * no picture has been decoded yet, the first one already carries that
         * layout, even though the request would otherwise have to wait for a
         * random access point. */
        for (const auto& cap : capabilities) {
            const char* name = names[unsigned(cap.profile)];
            const auto encoded = read(std::string(name) + ".bin");
            auto decoder = makeVideoDecoder(cap.codec);
            decoder->inspect(encoded);
            require(decoder->format().width == 128 && decoder->format().height == 96,
                    "inspecting a packet did not report the stream size");
            require(decoder->takeFrames().empty(), "inspecting a packet decoded it");
            decoder->requestOutputLayout(VideoPixelLayout::packedBgra);
            decoder->submit(encoded, 7);
            decoder->drain();
            auto frames = decoder->takeFrames();
            require(frames.size() == 1, "an inspected packet did not decode exactly once");
            require(frames[0].timestamp == 7, "an inspected packet changed its timestamp");
            require(frames[0].layout == VideoPixelLayout::packedBgra,
                    "the first picture after an inspected packet was not packed");
        }
        auto decoder = makeVideoDecoder(capabilities.front().codec);
        bool rejected = false;
        try { decoder->submit({}, 0); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "empty encoded packet was accepted");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
