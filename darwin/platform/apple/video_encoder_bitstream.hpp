// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Annex B and parameter-set helpers of the Apple video encoder backend. They
// are plain C++ so that the checked-in decoder fixtures can test them without
// VideoToolbox, and so that discovery and the encoder read a coded stream the
// same way.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uml::platform::bitstream {

using Bytes = std::span<const std::uint8_t>;

// The units of an Annex B byte stream, each without its start code and with its
// NAL unit header. Three- and four-byte start codes both separate units, and
// the zero bytes that lead a four-byte start code are not part of the unit
// before it.
[[nodiscard]] inline std::vector<Bytes> splitAnnexB(Bytes stream)
{
    std::vector<Bytes> units;
    const auto prefix = [stream](std::size_t at) -> std::size_t {
        if (at + 3 <= stream.size() && stream[at] == 0 && stream[at + 1] == 0 && stream[at + 2] == 1)
            return 3;
        return 0;
    };
    std::size_t begin = stream.size();
    for (std::size_t i = 0; i < stream.size();) {
        const auto length = prefix(i);
        if (!length) {
            ++i;
            continue;
        }
        if (begin < i) {
            auto end = i;
            while (end > begin && stream[end - 1] == 0) --end;   // a four-byte start code's leading zero
            if (end > begin) units.push_back(stream.subspan(begin, end - begin));
        }
        begin = i + length;
        i = begin;
    }
    if (begin < stream.size()) units.push_back(stream.subspan(begin));
    return units;
}

[[nodiscard]] inline unsigned h264NalType(Bytes nal) { return nal.empty() ? 0 : nal[0] & 0x1f; }
[[nodiscard]] inline unsigned hevcNalType(Bytes nal) { return nal.empty() ? 0 : (nal[0] >> 1) & 0x3f; }

inline constexpr unsigned h264NalSps = 7;
inline constexpr unsigned h264NalPps = 8;
inline constexpr unsigned h264NalIdr = 5;
inline constexpr unsigned hevcNalVps = 32;
inline constexpr unsigned hevcNalSps = 33;
inline constexpr unsigned hevcNalPps = 34;
inline constexpr unsigned hevcNalPrefixSei = 39;

// Raw byte sequence payload: the unit with its emulation prevention bytes
// removed, after `skip` header bytes.
[[nodiscard]] inline std::vector<std::uint8_t> rbsp(Bytes nal, std::size_t skip)
{
    std::vector<std::uint8_t> out;
    if (nal.size() <= skip) return out;
    out.reserve(nal.size() - skip);
    unsigned zeros = 0;
    for (std::size_t i = skip; i < nal.size(); ++i) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        out.push_back(nal[i]);
    }
    return out;
}

class BitReader {
public:
    explicit BitReader(Bytes bytes) : bytes_(bytes) {}
    [[nodiscard]] bool ok() const noexcept { return !overrun_; }
    std::uint32_t bits(unsigned count)
    {
        std::uint32_t value = 0;
        while (count-- > 0) {
            if (position_ >= bytes_.size() * 8) {
                overrun_ = true;
                return 0;
            }
            value = (value << 1) | ((bytes_[position_ / 8] >> (7 - position_ % 8)) & 1u);
            ++position_;
        }
        return value;
    }
    std::uint32_t unsignedExpGolomb()
    {
        unsigned zeros = 0;
        while (!overrun_ && bits(1) == 0) {
            if (++zeros > 31) {
                overrun_ = true;
                return 0;
            }
        }
        if (overrun_) return 0;
        return ((1u << zeros) - 1) + (zeros ? bits(zeros) : 0);
    }
private:
    Bytes bytes_;
    std::size_t position_ {};
    bool overrun_ {};
};

struct H264SpsInfo {
    unsigned profileIdc {};
    unsigned constraintFlags {};   // the byte after profile_idc: constraint_set0..5 and reserved bits
    unsigned levelIdc {};
};

// The SPS NAL unit (with its header byte).
[[nodiscard]] inline std::optional<H264SpsInfo> parseH264Sps(Bytes nal)
{
    if (nal.size() < 4 || h264NalType(nal) != h264NalSps) return std::nullopt;
    return H264SpsInfo {nal[1], nal[2], nal[3]};
}

struct HevcSpsInfo {
    unsigned profileIdc {};
    bool highTier {};
    unsigned levelIdc {};   // general_level_idc: 30 times the level number
};

// The SPS NAL unit (with its two-byte header).
[[nodiscard]] inline std::optional<HevcSpsInfo> parseHevcSps(Bytes nal)
{
    if (nal.size() < 4 || hevcNalType(nal) != hevcNalSps) return std::nullopt;
    const auto payload = rbsp(nal, 2);
    // sps_video_parameter_set_id and the sub-layer count share the first byte;
    // profile_tier_level follows: one byte of space, tier and profile, four of
    // compatibility flags and six of constraint flags, then the level.
    if (payload.size() < 1 + 1 + 4 + 6 + 1) return std::nullopt;
    const auto profile = payload[1];
    return HevcSpsInfo {unsigned(profile & 0x1f), (profile & 0x20) != 0, payload[12]};
}

// entropy_coding_mode_flag of an H.264 PPS NAL unit: true for CABAC.
[[nodiscard]] inline std::optional<bool> h264Cabac(Bytes nal)
{
    if (nal.size() < 3 || h264NalType(nal) != h264NalPps) return std::nullopt;
    const auto payload = rbsp(nal, 1);
    BitReader reader(payload);
    reader.unsignedExpGolomb();   // pic_parameter_set_id
    reader.unsignedExpGolomb();   // seq_parameter_set_id
    const bool cabac = reader.bits(1) != 0;
    if (!reader.ok()) return std::nullopt;
    return cabac;
}

struct SeiMessage {
    unsigned type {};
    std::vector<std::uint8_t> payload;
};

// The messages of an HEVC SEI NAL unit.
[[nodiscard]] inline std::vector<SeiMessage> hevcSeiMessages(Bytes nal)
{
    std::vector<SeiMessage> messages;
    if (nal.size() < 4) return messages;
    const auto payload = rbsp(nal, 2);
    std::size_t at = 0;
    while (at + 2 <= payload.size() && payload[at] != 0x80) {
        unsigned type = 0, size = 0;
        while (at < payload.size() && payload[at] == 0xff) { type += 255; ++at; }
        if (at >= payload.size()) break;
        type += payload[at++];
        while (at < payload.size() && payload[at] == 0xff) { size += 255; ++at; }
        if (at >= payload.size()) break;
        size += payload[at++];
        if (at + size > payload.size()) break;
        messages.push_back({type, {payload.begin() + std::ptrdiff_t(at), payload.begin() + std::ptrdiff_t(at + size)}});
        at += size;
    }
    return messages;
}

// The unit's payload with emulation prevention bytes inserted, so that no
// three-byte start code prefix appears inside it.
[[nodiscard]] inline std::vector<std::uint8_t> escapeEmulation(Bytes payload)
{
    std::vector<std::uint8_t> out;
    out.reserve(payload.size() + payload.size() / 64 + 1);
    unsigned zeros = 0;
    for (const auto byte : payload) {
        if (zeros >= 2 && byte <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return out;
}

// Appends `nal` to `out` behind a four-byte start code.
inline void appendAnnexB(std::vector<std::uint8_t>& out, const std::uint8_t* nal, std::size_t size)
{
    static constexpr std::uint8_t startCode[4] = {0, 0, 0, 1};
    out.insert(out.end(), startCode, startCode + 4);
    out.insert(out.end(), nal, nal + size);
}

} // namespace uml::platform::bitstream
