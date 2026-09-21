// SPDX-License-Identifier: GPL-2.0-or-later
#include "convert.hpp"

#include <algorithm>
#include <cstring>

namespace uml::va {
namespace {

// Studio-swing YCbCr to RGB, in 16.16 fixed point. Luma is scaled by
// 255/219 and chroma by 255/224 before the matrix, which is what the
// coefficients below already fold in.
struct Matrix {
    int luma;   // (255/219) << 16
    int rv;
    int gu;
    int gv;
    int bu;
};

constexpr Matrix kBt601 {76309, 104597, -25675, -53279, 132201};
constexpr Matrix kBt709 {76309, 117489, -13975, -34925, 138438};

inline std::uint8_t clamp(int value) noexcept
{
    return std::uint8_t(value < 0 ? 0 : (value > 255 ? 255 : value));
}

}

void copyNv12(const ConstPlaneView source[2], const PlaneView destination[2], unsigned width, unsigned height) noexcept
{
    const unsigned chromaRows = (height + 1u) / 2u;
    const unsigned chromaBytes = ((width + 1u) / 2u) * 2u;
    for (unsigned row = 0; row < height; ++row)
        std::memcpy(destination[0].data + std::size_t(row) * destination[0].pitch,
                    source[0].data + std::size_t(row) * source[0].pitch, width);
    for (unsigned row = 0; row < chromaRows; ++row)
        std::memcpy(destination[1].data + std::size_t(row) * destination[1].pitch,
                    source[1].data + std::size_t(row) * source[1].pitch, chromaBytes);
}

void convertNv12ToArgb(const ConstPlaneView source[2], const Region& sourceRegion,
                       const PlaneView& destination, const Region& destinationRegion,
                       bool bt709) noexcept
{
    const Matrix& matrix = bt709 ? kBt709 : kBt601;
    const unsigned width = destinationRegion.width;
    const unsigned height = destinationRegion.height;
    if (!width || !height || !sourceRegion.width || !sourceRegion.height) return;
    const bool direct = width == sourceRegion.width && height == sourceRegion.height;
    for (unsigned row = 0; row < height; ++row) {
        const unsigned sourceRow = direct
            ? sourceRegion.y + row
            : sourceRegion.y + unsigned(std::uint64_t(row) * sourceRegion.height / height);
        const std::uint8_t* luma = source[0].data + std::size_t(sourceRow) * source[0].pitch;
        const std::uint8_t* chroma = source[1].data + std::size_t(sourceRow / 2) * source[1].pitch;
        std::uint8_t* out = destination.data + std::size_t(destinationRegion.y + row) * destination.pitch +
                            std::size_t(destinationRegion.x) * 4;
        for (unsigned column = 0; column < width; ++column) {
            const unsigned sourceColumn = direct
                ? sourceRegion.x + column
                : sourceRegion.x + unsigned(std::uint64_t(column) * sourceRegion.width / width);
            const int y = (int(luma[sourceColumn]) - 16) * matrix.luma;
            const int u = int(chroma[(sourceColumn & ~1u)]) - 128;
            const int v = int(chroma[(sourceColumn & ~1u) + 1]) - 128;
            const int r = (y + matrix.rv * v + 32768) >> 16;
            const int g = (y + matrix.gu * u + matrix.gv * v + 32768) >> 16;
            const int b = (y + matrix.bu * u + 32768) >> 16;
            out[4 * column + 0] = clamp(b);
            out[4 * column + 1] = clamp(g);
            out[4 * column + 2] = clamp(r);
            out[4 * column + 3] = 0xff;
        }
    }
}

void copyPacked32(const ConstPlaneView& source, const Region& sourceRegion,
                  const PlaneView& destination, const Region& destinationRegion) noexcept
{
    const unsigned width = destinationRegion.width;
    const unsigned height = destinationRegion.height;
    if (!width || !height || !sourceRegion.width || !sourceRegion.height) return;
    const bool direct = width == sourceRegion.width && height == sourceRegion.height;
    for (unsigned row = 0; row < height; ++row) {
        const unsigned sourceRow = direct
            ? sourceRegion.y + row
            : sourceRegion.y + unsigned(std::uint64_t(row) * sourceRegion.height / height);
        const std::uint8_t* in = source.data + std::size_t(sourceRow) * source.pitch;
        std::uint8_t* out = destination.data + std::size_t(destinationRegion.y + row) * destination.pitch +
                            std::size_t(destinationRegion.x) * 4;
        if (direct) {
            std::memcpy(out, in + std::size_t(sourceRegion.x) * 4, std::size_t(width) * 4);
            continue;
        }
        for (unsigned column = 0; column < width; ++column) {
            const unsigned sourceColumn = sourceRegion.x + unsigned(std::uint64_t(column) * sourceRegion.width / width);
            std::memcpy(out + std::size_t(column) * 4, in + std::size_t(sourceColumn) * 4, 4);
        }
    }
}

}
