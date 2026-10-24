// SPDX-License-Identifier: GPL-2.0-or-later
#include "apple/video_encoder_bitstream.hpp"
#include "require.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

/* The Annex B, parameter-set and SEI helpers of the Apple video encoder
 * backend, on the streams the decoder tests check in and on units built here.
 * Nothing in this binary touches VideoToolbox. */

namespace {
using namespace uml::platform::bitstream;
using uml::testing::require;

std::string fixtureDirectory;

std::vector<std::uint8_t> read(const std::string& name)
{
    std::ifstream file(fixtureDirectory + "/" + name, std::ios::binary);
    if (!file) throw std::runtime_error("missing video fixture: " + fixtureDirectory + "/" + name);
    return {std::istreambuf_iterator<char>(file), {}};
}

using Unit = std::vector<std::uint8_t>;

std::vector<std::uint8_t> join(std::initializer_list<std::vector<std::uint8_t>> parts)
{
    std::vector<std::uint8_t> out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

void splitting()
{
    const Unit a {0x67, 1, 2, 3}, b {0x68, 4}, c {0x65, 0xff, 0x00, 0x7f};
    // Three- and four-byte start codes, and a unit whose last byte is zero
    // data that belongs to it only when no start code follows.
    const auto stream = join({{0, 0, 0, 1}, a, {0, 0, 1}, b, {0, 0, 0, 1}, c});
    const auto units = splitAnnexB(stream);
    require(units.size() == 3, "three units expected");
    require(Unit(units[0].begin(), units[0].end()) == a, "first unit");
    require(Unit(units[1].begin(), units[1].end()) == b, "second unit");
    require(Unit(units[2].begin(), units[2].end()) == c, "last unit keeps its own bytes");

    require(splitAnnexB({}).empty(), "empty stream has no units");
    const Unit onlyZeros {0, 0, 0, 0, 0};
    require(splitAnnexB(onlyZeros).empty(), "zeros are not a unit");
    const Unit leading {0, 0, 0, 0, 1, 0x09, 0x10};
    const auto lead = splitAnnexB(leading);
    require(lead.size() == 1 && lead[0].size() == 2 && lead[0][0] == 0x09, "leading zero bytes before a start code");

    // A unit's NAL header decides its type for both codecs.
    require(h264NalType(a) == h264NalSps && h264NalType(b) == h264NalPps && h264NalType(c) == h264NalIdr, "H.264 types");
    const Unit vps {0x40, 0x01, 0xaa}, sps {0x42, 0x01, 0xbb}, pps {0x44, 0x01, 0xcc}, sei {0x4e, 0x01, 0xdd};
    require(hevcNalType(vps) == hevcNalVps && hevcNalType(sps) == hevcNalSps && hevcNalType(pps) == hevcNalPps &&
            hevcNalType(sei) == hevcNalPrefixSei, "HEVC types");
}

void emulationPrevention()
{
    const Unit escaped {0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03};
    const auto plain = rbsp(escaped, 0);
    require(plain == Unit({0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00}), "prevention bytes are removed");
    require(rbsp(Unit {0x67, 0x00, 0x00, 0x03, 0x02}, 1) == Unit({0x00, 0x00, 0x02}), "header bytes are skipped");
    require(rbsp(Unit {0x67}, 4).empty(), "skipping past the unit leaves nothing");

    // The escape of any payload has no start-code prefix in it and reads back
    // as the payload.
    std::uint32_t state = 7;
    for (unsigned round = 0; round < 200; ++round) {
        Unit payload;
        for (unsigned i = 0; i < 64; ++i) {
            state = state * 1664525u + 1013904223u;
            // Favour the bytes that make a prefix.
            const unsigned pick = (state >> 24) & 7;
            payload.push_back(pick < 5 ? 0 : std::uint8_t(pick - 4));
        }
        const auto escapedPayload = escapeEmulation(payload);
        for (std::size_t i = 2; i < escapedPayload.size(); ++i)
            require(!(escapedPayload[i - 2] == 0 && escapedPayload[i - 1] == 0 && escapedPayload[i] <= 2), "escape left a prefix");
        require(rbsp(escapedPayload, 0) == payload, "escape does not read back");
    }
    // A payload that ends in zeros is not left to look like the start of a
    // start code: the trailing byte of an RBSP is nonzero, which callers add.
    const Unit trailing {0x01, 0x00, 0x00, 0x80};
    require(escapeEmulation(trailing) == trailing, "no escape is needed before a high byte");
}

void bitReading()
{
    const Unit bytes {0b1010'0110, 0b0100'0000};   // 1 | 010 | 011 | 00100 ...
    BitReader reader(bytes);
    require(reader.unsignedExpGolomb() == 0, "1 is 0");
    require(reader.unsignedExpGolomb() == 1, "010 is 1");
    require(reader.unsignedExpGolomb() == 2, "011 is 2");
    require(reader.unsignedExpGolomb() == 3, "00100 is 3");
    require(reader.ok(), "no overrun yet");
    BitReader plain(bytes);
    require(plain.bits(4) == 0b1010 && plain.bits(8) == 0b0110'0100 && plain.bits(4) == 0, "fixed-width reads");
    require(plain.ok(), "exactly the available bits");
    (void)plain.bits(1);
    require(!plain.ok(), "reading past the end is reported");
    const Unit zeros(8, 0);
    BitReader endless(zeros);
    (void)endless.unsignedExpGolomb();
    require(!endless.ok(), "a run of zeros without a one overruns");
}

void seiMessages()
{
    // Types and sizes of 255 and above use the 0xff continuation bytes.
    Unit payload;
    payload.insert(payload.end(), {137, 24});
    for (unsigned i = 0; i < 24; ++i) payload.push_back(std::uint8_t(i));
    payload.insert(payload.end(), {144, 4, 0x03, 0xe8, 0x01, 0xf4});
    payload.insert(payload.end(), {0xff, 0x05, 0xff, 0x02});   // type 260, size 257
    for (unsigned i = 0; i < 257; ++i) payload.push_back(std::uint8_t(i % 3 == 0 ? 0 : i));
    payload.push_back(0x80);
    Unit unit {0x4e, 0x01};
    const auto escaped = escapeEmulation(payload);
    unit.insert(unit.end(), escaped.begin(), escaped.end());

    const auto messages = hevcSeiMessages(unit);
    require(messages.size() == 3, "three messages");
    require(messages[0].type == 137 && messages[0].payload.size() == 24 && messages[0].payload[23] == 23, "mastering display message");
    require(messages[1].type == 144 && messages[1].payload == Unit({0x03, 0xe8, 0x01, 0xf4}), "content light message");
    require(messages[2].type == 260 && messages[2].payload.size() == 257, "extended type and size");
    // A message that claims more than the unit holds ends the list.
    Unit truncated {0x4e, 0x01, 137, 30, 1, 2, 3, 0x80};
    require(hevcSeiMessages(truncated).empty(), "a truncated message is not returned");
    require(hevcSeiMessages(Unit {0x4e, 0x01}).empty(), "a unit with no payload has no messages");
}

void fixtures()
{
    struct H264Case { const char* name; unsigned profile; unsigned constraints; bool cabac; };
    const H264Case h264[] = {
        {"h264-baseline", 66, 0x80, false},
        {"h264-constrained-baseline", 66, 0xc0, false},
        {"h264-main", 77, 0x40, true},
        {"h264-high", 100, 0x00, true},
        {"h264-high10", 110, 0x00, true},
    };
    for (const auto& c : h264) {
        const auto stream = read(std::string(c.name) + ".bin");
        const auto units = splitAnnexB(stream);
        require(units.size() >= 3, "access unit has parameter sets and a picture");
        bool sps = false, pps = false, picture = false;
        for (const auto unit : units) {
            if (const auto info = parseH264Sps(unit)) {
                sps = true;
                require(info->profileIdc == c.profile, "SPS profile_idc");
                require((info->constraintFlags & 0xc0) == (c.constraints & 0xc0), "SPS constraint_set0/1 flags");
            } else if (const auto cabac = h264Cabac(unit)) {
                pps = true;
                std::printf("%s: entropy %s\n", c.name, *cabac ? "CABAC" : "CAVLC");
                require(*cabac == c.cabac, "PPS entropy_coding_mode_flag");
            } else if (h264NalType(unit) == h264NalIdr) {
                picture = true;
            }
        }
        require(sps && pps && picture, "SPS, PPS and an IDR picture");
        // Annex B round trip through the appender.
        std::vector<std::uint8_t> rebuilt;
        for (const auto unit : units) appendAnnexB(rebuilt, unit.data(), unit.size());
        const auto again = splitAnnexB(rebuilt);
        require(again.size() == units.size(), "appending keeps the unit count");
        for (std::size_t i = 0; i < units.size(); ++i)
            require(Unit(again[i].begin(), again[i].end()) == Unit(units[i].begin(), units[i].end()), "appending keeps the unit");
    }

    struct HevcCase { const char* name; unsigned profile; };
    for (const auto& c : {HevcCase {"hevc-main", 1}, HevcCase {"hevc-main10", 2}}) {
        const auto stream = read(std::string(c.name) + ".bin");
        const auto units = splitAnnexB(stream);
        bool vps = false, sps = false, pps = false;
        for (const auto unit : units) {
            vps = vps || hevcNalType(unit) == hevcNalVps;
            pps = pps || hevcNalType(unit) == hevcNalPps;
            if (const auto info = parseHevcSps(unit)) {
                sps = true;
                std::printf("%s: level_idc %u tier %d\n", c.name, info->levelIdc, int(info->highTier));
                require(info->profileIdc == c.profile, "HEVC general_profile_idc");
                require(!info->highTier, "main tier");
                require(info->levelIdc >= 30 && info->levelIdc <= 186, "level_idc names a level");
            }
        }
        require(vps && sps && pps, "VPS, SPS and PPS");
    }

    // Units of the wrong kind or too short to read are refused, not guessed at.
    require(!parseH264Sps(Unit {0x68, 1, 2, 3}), "PPS is not an SPS");
    require(!parseH264Sps(Unit {0x67, 1}), "truncated SPS");
    require(!parseHevcSps(Unit {0x40, 0x01, 1, 2, 3}), "VPS is not an SPS");
    require(!parseHevcSps(Unit {0x42, 0x01, 1, 2, 3}), "truncated HEVC SPS");
    require(!h264Cabac(Unit {0x67, 1, 2}), "SPS is not a PPS");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: uml_video_encoder_bitstream_tests FIXTURE_DIRECTORY\n");
        return 2;
    }
    fixtureDirectory = argv[1];
    try {
        splitting();
        emulationPrevention();
        bitReading();
        seiMessages();
        fixtures();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("video encoder bitstream helpers ok");
    return 0;
}
