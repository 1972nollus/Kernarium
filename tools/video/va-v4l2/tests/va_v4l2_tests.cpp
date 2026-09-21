// SPDX-License-Identifier: GPL-2.0-or-later
// Host-independent checks for the parts of the VA-API backend that do not need
// a decoder: the VP9 header reader, the superframe assembly the hidden-frame
// rule depends on, and the pixel paths. Runs natively on any Linux build host.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <va/va.h>

#include "../convert.hpp"
#include "../storage.hpp"
#include "../vp9.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (condition) return;
    std::fprintf(stderr, "FAIL %s\n", what);
    ++failures;
}

// Writes VP9 uncompressed headers bit by bit, which is the only way to build
// the fixtures this parser has to read.
class HeaderWriter {
public:
    void bit(unsigned value)
    {
        if (count_ == 0) { bytes_.push_back(0); count_ = 8; }
        --count_;
        if (value) bytes_.back() = std::uint8_t(bytes_.back() | (1u << count_));
    }
    void bits(unsigned value, unsigned width)
    {
        for (unsigned index = width; index-- > 0;) bit((value >> index) & 1u);
    }
    void syncCode() { bits(0x49, 8); bits(0x83, 8); bits(0x42, 8); }
    std::vector<std::uint8_t> take()
    {
        // A real frame carries a compressed header and tile data after this;
        // a few trailing bytes stand in for them.
        bytes_.insert(bytes_.end(), {0x00, 0x00, 0x00, 0x00});
        return bytes_;
    }

private:
    std::vector<std::uint8_t> bytes_;
    unsigned count_ {};
};

std::vector<std::uint8_t> keyFrame(unsigned width, unsigned height, bool show = true)
{
    HeaderWriter writer;
    writer.bits(2, 2);       // frame_marker
    writer.bit(0);           // profile_low_bit
    writer.bit(0);           // profile_high_bit
    writer.bit(0);           // show_existing_frame
    writer.bit(0);           // frame_type: key
    writer.bit(show ? 1 : 0);
    writer.bit(0);           // error_resilient_mode
    writer.syncCode();
    writer.bits(2, 3);       // color_space: BT.709
    writer.bit(0);           // color_range
    writer.bits(width - 1, 16);
    writer.bits(height - 1, 16);
    writer.bit(0);           // render_and_frame_size_different
    return writer.take();
}

std::vector<std::uint8_t> interFrame(bool show, unsigned refreshFlags)
{
    HeaderWriter writer;
    writer.bits(2, 2);
    writer.bit(0);
    writer.bit(0);
    writer.bit(0);           // show_existing_frame
    writer.bit(1);           // frame_type: inter
    writer.bit(show ? 1 : 0);
    writer.bit(0);           // error_resilient_mode
    if (!show) writer.bit(0);// intra_only
    writer.bits(0, 2);       // reset_frame_context
    writer.bits(refreshFlags, 8);
    for (unsigned index = 0; index < 3; ++index) { writer.bits(index, 3); writer.bit(0); }
    writer.bit(1);           // found_ref for the first reference
    writer.bit(0);           // render_and_frame_size_different
    return writer.take();
}

void testHeaders()
{
    uml::va::Vp9Context context;
    const auto key = keyFrame(1920, 1200);
    auto header = context.parse(key);
    check(header.valid, "a key frame header parses");
    check(header.keyFrame && header.showFrame, "a key frame is a shown key frame");
    check(header.width == 1920 && header.height == 1200, "a key frame carries its size");
    check(header.refreshFrameFlags == 0xff, "a key frame refreshes every slot");
    check(header.profile == 0, "profile 0 is read");

    const auto hidden = interFrame(false, 0x04);
    header = context.parse(hidden);
    check(header.valid, "a hidden inter frame parses");
    check(!header.showFrame && !header.keyFrame, "a hidden frame is not shown");
    check(header.refreshFrameFlags == 0x04, "the refresh mask is read");
    check(header.width == 1920 && header.height == 1200, "an inter frame takes its size from a reference");

    HeaderWriter writer;
    writer.bits(2, 2);
    writer.bit(0);
    writer.bit(0);
    writer.bit(1);          // show_existing_frame
    writer.bits(2, 3);      // frame_to_show_map_idx
    const auto existing = writer.take();
    header = context.parse(existing);
    check(header.valid && header.showExistingFrame, "show_existing_frame parses");
    check(header.frameToShowMapIndex == 2, "the shown slot is read");
    check(header.width == 1920, "the shown slot keeps its size");

    const std::uint8_t truncated[] {0x82};
    header = context.parse(truncated);
    check(!header.valid, "a truncated header is rejected");
    const std::uint8_t wrongMarker[] {0x00, 0x00, 0x00};
    header = context.parse(wrongMarker);
    check(!header.valid, "a frame without the VP9 marker is rejected");
}

void testShowExistingByte()
{
    for (unsigned slot = 0; slot < 8; ++slot) {
        const std::uint8_t byte = uml::va::vp9ShowExistingFrameByte(slot);
        uml::va::Vp9Context context;
        const auto key = keyFrame(64, 64);
        context.parse(key);
        const std::uint8_t frame[] {byte};
        const auto header = context.parse(frame);
        check(header.valid, "the synthesised show_existing_frame parses");
        check(header.showExistingFrame, "the synthesised frame shows an existing one");
        check(header.frameToShowMapIndex == slot, "the synthesised frame names the right slot");
    }
}

void testSuperframes()
{
    const auto first = keyFrame(320, 240);
    const std::uint8_t tail = uml::va::vp9ShowExistingFrameByte(3);
    const std::span<const std::uint8_t> parts[2] {first, std::span<const std::uint8_t>(&tail, 1)};
    const auto packet = uml::va::vp9BuildSuperframe(std::span<const std::span<const std::uint8_t>>(parts, 2));
    check(!packet.empty(), "a superframe is built");
    const auto split = uml::va::vp9SuperframeParts(packet);
    check(split.size() == 2, "the superframe splits back into two frames");
    if (split.size() == 2) {
        check(split[0].size() == first.size(), "the first frame keeps its length");
        check(split[1].size() == 1, "the trailing frame is one byte");
        check(std::memcmp(split[0].data(), first.data(), first.size()) == 0, "the first frame is unchanged");
        check(split[1][0] == tail, "the trailing byte is unchanged");
    }
    const auto single = uml::va::vp9SuperframeParts(first);
    check(single.size() == 1 && single[0].size() == first.size(), "a plain frame is its own only part");

    // A byte that merely looks like a superframe marker must not split a frame.
    std::vector<std::uint8_t> pretend {0x82, 0x49, 0xc1};
    const auto pretendParts = uml::va::vp9SuperframeParts(pretend);
    check(pretendParts.size() == 1, "a false marker leaves the frame whole");
}

void testPixels()
{
    constexpr unsigned width = 32;
    constexpr unsigned height = 16;
    std::vector<std::uint8_t> luma(std::size_t(width) * height);
    std::vector<std::uint8_t> chroma(std::size_t(width) * height / 2);
    for (std::size_t index = 0; index < luma.size(); ++index) luma[index] = std::uint8_t(index & 0xff);
    for (std::size_t index = 0; index < chroma.size(); ++index) chroma[index] = 128;

    std::vector<std::uint8_t> copyLuma(luma.size(), 0);
    std::vector<std::uint8_t> copyChroma(chroma.size(), 0);
    const uml::va::ConstPlaneView source[2] {{luma.data(), width}, {chroma.data(), width}};
    const uml::va::PlaneView destination[2] {{copyLuma.data(), width}, {copyChroma.data(), width}};
    uml::va::copyNv12(source, destination, width, height);
    check(copyLuma == luma && copyChroma == chroma, "an NV12 copy is exact");

    // A wider destination pitch must not disturb the payload.
    const unsigned pitch = width + 16;
    std::vector<std::uint8_t> wideLuma(std::size_t(pitch) * height, 0xaa);
    std::vector<std::uint8_t> wideChroma(std::size_t(pitch) * height / 2, 0xaa);
    const uml::va::PlaneView wide[2] {{wideLuma.data(), pitch}, {wideChroma.data(), pitch}};
    uml::va::copyNv12(source, wide, width, height);
    bool rowsMatch = true;
    for (unsigned row = 0; row < height; ++row)
        rowsMatch = rowsMatch && std::memcmp(wideLuma.data() + std::size_t(row) * pitch,
                                             luma.data() + std::size_t(row) * width, width) == 0;
    check(rowsMatch, "an NV12 copy honours a wider pitch");

    std::vector<std::uint8_t> argb(std::size_t(width) * height * 4, 0);
    const uml::va::PlaneView out {argb.data(), std::size_t(width) * 4};
    uml::va::convertNv12ToArgb(source, {0, 0, width, height}, out, {0, 0, width, height}, true);
    // Neutral chroma keeps the three colour channels equal, and the alpha is
    // opaque.
    bool neutral = true;
    for (std::size_t pixel = 0; pixel < std::size_t(width) * height; ++pixel) {
        neutral = neutral && argb[4 * pixel] == argb[4 * pixel + 1] && argb[4 * pixel + 1] == argb[4 * pixel + 2];
        neutral = neutral && argb[4 * pixel + 3] == 0xff;
    }
    check(neutral, "neutral chroma converts to grey with opaque alpha");
    // Luma 0..16 is below the studio-swing floor and clamps to black, so a
    // sample well inside the range is the one that has to be brighter.
    check(argb[4 * 200] > argb[4 * 20], "brighter luma converts to a brighter pixel");
    check(argb[4 * 5] == 0, "luma below the studio floor is black");
}

void testLayout()
{
    unsigned planes = 0;
    std::size_t pitch[4] {};
    std::size_t offset[4] {};
    std::size_t size = 0;
    check(uml::va::planeLayout(VA_FOURCC_NV12, 1920, 1200, planes, pitch, offset, size), "NV12 has a layout");
    check(planes == 2, "NV12 has two planes");
    check(pitch[0] == 1920 && offset[0] == 0, "the NV12 luma plane is first and tightly packed");
    check(offset[1] == std::size_t(1920) * 1200, "the NV12 chroma plane follows the luma plane");
    check(size == std::size_t(1920) * 1200 * 3 / 2, "an NV12 surface is one and a half times its luma");
    check(uml::va::planeLayout(VA_FOURCC_ARGB, 1280, 720, planes, pitch, offset, size), "ARGB has a layout");
    check(planes == 1 && pitch[0] == 1280 * 4, "ARGB is one packed plane");
    check(!uml::va::planeLayout(VA_FOURCC_P010, 64, 64, planes, pitch, offset, size), "P010 is refused for now");
}

void testMemoryStorage()
{
    uml::va::selectStorage("memory");
    std::string error;
    auto buffer = uml::va::PixelBuffer::allocate(VA_FOURCC_NV12, 64, 48, error);
    check(buffer != nullptr, "a memory surface allocates");
    if (!buffer) return;
    check(buffer->planes() == 2, "a memory NV12 surface has two planes");
    check(buffer->map() != nullptr, "a memory surface maps");
    const int fd = buffer->exportDescriptor();
    check(fd >= 0, "a memory surface exports a descriptor");
    if (fd >= 0) {
        check(buffer->identity() != 0, "an exported surface has an identity");
        ::close(fd);
    }
    buffer->map()[0] = 0x5a;
    check(buffer->map()[0] == 0x5a, "a memory surface keeps what is written to it");
}

}

int main()
{
    testHeaders();
    testShowExistingByte();
    testSuperframes();
    testPixels();
    testLayout();
    testMemoryStorage();
    if (failures) {
        std::fprintf(stderr, "UML_VA_V4L2_TESTS: FAIL (%d)\n", failures);
        return 1;
    }
    std::puts("UML_VA_V4L2_TESTS: PASS");
    return 0;
}
