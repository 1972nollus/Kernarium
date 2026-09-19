// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_decoder.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
std::vector<std::uint8_t> read(const std::string& name)
{
    std::ifstream file(std::string(UML_VIDEO_FIXTURE_DIR) + "/" + name, std::ios::binary);
    if (!file) throw std::runtime_error("missing video fixture: " + name);
    return {std::istreambuf_iterator<char>(file), {}};
}
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    using namespace uml::platform;
    try {
        const auto capabilities = videoDecodeCapabilities();
        require(!capabilities.empty(), "no hardware video decoder available");
        const char* names[] = {"h264-baseline", "h264-constrained-baseline", "h264-main",
            "h264-high", "h264-high10", "hevc-main", "hevc-main10", "jpeg"};
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
