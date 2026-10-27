// SPDX-License-Identifier: GPL-2.0-or-later

// Without a host allocator a guest keeps a YUV picture as the bytes of one R8
// image: the luma rows, then the chroma rows, in the layout Android defines. A
// shader that samples the picture reads the luma plane and the chroma plane or
// planes as separate textures, and the host used to have one texture for the
// whole image, so luma came back as the picture squeezed into two thirds of the
// frame and every chroma view read what luma did. Every decoder that plays
// into a GL or SurfaceView view showed stripes.
//
// The contract under test: a resource created with a packed YUV layout and the
// picture's width samples as the planes it holds, through the plane number a
// sampler view carries, and keeps doing so after the guest rewrites some of its
// rows; a resource created without the layout is the plain texture it always
// was; and a layout that does not describe a picture is refused.

#include "host_gl_stream_support.hpp"
#include "host_gl_test_support.hpp"

#include <virgl_hw.h>

#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;

// A YUV 4:2:0 picture and the image the guest allocator keeps it in.
struct Picture {
    bool biplanar;
    std::uint32_t width;
    std::uint32_t height;

    [[nodiscard]] std::uint32_t stride() const
    {
        return alignUp(width, biplanar ? 16U : 32U);
    }
    [[nodiscard]] std::uint32_t chromaRows() const { return (height + 1) / 2; }
    [[nodiscard]] std::uint32_t chromaWidth() const { return (width + 1) / 2; }
    [[nodiscard]] std::uint32_t rows() const { return height + chromaRows(); }
    [[nodiscard]] std::uint32_t planes() const { return biplanar ? 2 : 3; }
    [[nodiscard]] std::uint32_t flags() const
    {
        return (biplanar ? VIRGL_RESOURCE_FLAG_YUV_BIPLANAR
                         : VIRGL_RESOURCE_FLAG_YUV_TRIPLANAR) |
               (width << VIRGL_RESOURCE_YUV_WIDTH_SHIFT);
    }
    [[nodiscard]] std::uint32_t planeWidth(std::uint32_t plane) const
    {
        return plane == 0 ? width : chromaWidth();
    }
    [[nodiscard]] std::uint32_t planeHeight(std::uint32_t plane) const
    {
        return plane == 0 ? height : chromaRows();
    }
    [[nodiscard]] std::uint32_t planeFormat(std::uint32_t plane) const
    {
        return plane == 1 && biplanar ? virglFormatR8g8Unorm
                                      : virglFormatR8Unorm;
    }

    // The two channels a plane texel holds, read from the bytes of the image
    // the way Android lays the picture out in them.
    [[nodiscard]] std::array<std::uint8_t, 2> texel(
        const std::vector<std::uint8_t>& image,
        std::uint32_t plane,
        std::uint32_t x,
        std::uint32_t y) const
    {
        const std::size_t luma = static_cast<std::size_t>(stride()) * height;
        if (plane == 0)
            return {image[static_cast<std::size_t>(y) * stride() + x], 0};
        if (biplanar) {
            const std::size_t at =
                luma + static_cast<std::size_t>(y) * stride() + 2U * x;
            return {image[at], image[at + 1]};
        }
        const std::size_t chromaStride = stride() / 2U;
        const std::size_t at = luma +
            (plane - 1U) * chromaStride * chromaRows() +
            static_cast<std::size_t>(y) * chromaStride + x;
        return {image[at], 0};
    }
};

// Every byte of the image different from its neighbours, so a texel read from
// the wrong place cannot match.
void fill(std::vector<std::uint8_t>& image, std::uint32_t seed)
{
    for (std::size_t index = 0; index < image.size(); ++index)
        image[index] = static_cast<std::uint8_t>(index * 37U + seed);
}


// Draws every plane of the picture through its view and compares each texel
// with the one the layout puts there.
void checkPlanes(
    Pipeline& pipeline,
    const Picture& picture,
    const std::vector<std::uint8_t>& image,
    const std::array<std::uint32_t, 3>& views,
    const char* when)
{
    for (std::uint32_t plane = 0; plane < picture.planes(); ++plane) {
        const std::uint32_t width = picture.planeWidth(plane);
        const std::uint32_t height = picture.planeHeight(plane);
        const auto texels = pipeline.sample(views[plane], width, height);
        std::uint32_t wrong = 0;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto expected = picture.texel(image, plane, x, y);
                const std::uint8_t* const actual =
                    &texels[(static_cast<std::size_t>(y) * width + x) * 4U];
                const bool pair = plane == 1 && picture.biplanar;
                if (actual[0] != expected[0] ||
                    (pair && actual[1] != expected[1])) {
                    if (wrong++ < 4)
                        std::fprintf(stderr,
                            "%s %s %ux%u plane %u texel (%u,%u): got %u,%u "
                            "wanted %u,%u\n",
                            when, picture.biplanar ? "biplanar" : "triplanar",
                            picture.width, picture.height, plane, x, y,
                            actual[0], actual[1], expected[0], expected[1]);
                }
            }
        }
        require(wrong == 0, "a plane did not sample as the layout says");
    }
}

void checkPicture(VirglRenderer& renderer, Pipeline& pipeline, Picture picture)
{
    constexpr std::uint32_t imageId = 3;
    Image image(renderer, imageId, pipeTexture2d, virglFormatR8Unorm,
        virglBindSamplerView, picture.stride(), picture.rows(), picture.flags(),
        static_cast<std::size_t>(picture.stride()) * picture.rows());
    fill(image.bytes(), 11);
    image.write(picture.stride(), 0, picture.rows());

    // The views are made after the first write: the planes are made with the
    // first of them, from the image as it is then.
    std::array<std::uint32_t, 3> views {};
    for (std::uint32_t plane = 0; plane < picture.planes(); ++plane) {
        views[plane] = pipeline.createView(
            imageId, picture.planeFormat(plane), plane);
    }
    checkPlanes(pipeline, picture, image.bytes(), views, "first write");

    // Rewriting some rows changes the planes they feed, in the same views.
    // Rows are picked to cut across the chroma rows of a triplanar image, two
    // of which share an image row.
    const std::uint32_t first = picture.height - 1;
    const std::uint32_t count = picture.rows() - first - 1;
    for (std::uint32_t row = first; row < first + count; ++row) {
        for (std::uint32_t column = 0; column < picture.stride(); ++column)
            image.bytes()[static_cast<std::size_t>(row) * picture.stride() +
                column] = static_cast<std::uint8_t>(~(row * 53U + column * 7U));
    }
    image.write(picture.stride(), first, count);
    checkPlanes(pipeline, picture, image.bytes(), views, "partial rewrite");
}

// A resource created without a layout is the plain texture it always was, and
// a plane number on a view of it selects nothing.
void checkPlainImage(VirglRenderer& renderer, Pipeline& pipeline)
{
    constexpr std::uint32_t imageId = 3;
    constexpr std::uint32_t width = 16;
    constexpr std::uint32_t height = 9;
    Image image(renderer, imageId, pipeTexture2d, virglFormatR8Unorm,
        virglBindSamplerView, width, height, 0, width * height);
    fill(image.bytes(), 5);
    image.write(width, 0, height);
    for (std::uint32_t plane = 0; plane < 2; ++plane) {
        const std::uint32_t view =
            pipeline.createView(imageId, virglFormatR8Unorm, plane);
        const auto texels = pipeline.sample(view, width, height);
        for (std::size_t index = 0; index < width * height; ++index)
            require(texels[index * 4U] == image.bytes()[index],
                "a resource without a layout did not sample as one texture");
    }
}

// What the renderer will not take for a picture.
void checkRefused(VirglRenderer& renderer)
{
    constexpr std::uint32_t id = 3;
    const Picture picture {true, 10, 6};
    const std::uint32_t good = picture.flags();
    const auto create = [&](std::uint32_t stride, std::uint32_t rows,
                            std::uint32_t flags) {
        const int result = Image::create(renderer, id, pipeTexture2d,
            virglFormatR8Unorm, virglBindSamplerView, stride, rows, flags);
        if (result == 0)
            renderer.unrefResource(id);
        return result;
    };

    require(create(16, 9, good) == 0, "a valid picture was refused");
    require(create(16, 10, good) != 0,
        "an image that is not luma and chroma rows was accepted");
    require(create(16, 9,
                good | VIRGL_RESOURCE_FLAG_YUV_TRIPLANAR) != 0,
        "an image with both layouts was accepted");
    require(create(16, 9, VIRGL_RESOURCE_FLAG_YUV_BIPLANAR) != 0,
        "a picture without a width was accepted");
    require(create(16, 9, VIRGL_RESOURCE_FLAG_YUV_BIPLANAR |
                (17U << VIRGL_RESOURCE_YUV_WIDTH_SHIFT)) != 0,
        "a picture wider than its luma row was accepted");
    require(create(10, 9, VIRGL_RESOURCE_FLAG_YUV_BIPLANAR |
                (11U << VIRGL_RESOURCE_YUV_WIDTH_SHIFT)) != 0,
        "a picture wider than its luma row was accepted");
    // An odd width in a luma row of exactly that width leaves its last chroma
    // pair a byte short.
    require(create(9, 9, VIRGL_RESOURCE_FLAG_YUV_BIPLANAR |
                (9U << VIRGL_RESOURCE_YUV_WIDTH_SHIFT)) != 0,
        "chroma pairs that do not fit in a luma row were accepted");
    // The chroma rows are half a luma row wide, so the luma row is even.
    require(create(17, 9, VIRGL_RESOURCE_FLAG_YUV_TRIPLANAR |
                (17U << VIRGL_RESOURCE_YUV_WIDTH_SHIFT)) != 0,
        "chroma planes of half an odd luma row were accepted");
    require(create(16, 9, good | VIRGL_RESOURCE_FLAG_MAP_COHERENT) != 0,
        "a layout together with another resource flag was accepted");
    require(create(16, 9, 10U << VIRGL_RESOURCE_YUV_WIDTH_SHIFT) != 0,
        "a width without a layout was accepted");
}

} // namespace

int main()
{
    std::unique_ptr<VirglRenderer> renderer;
    try {
        renderer = std::make_unique<VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }

    const auto capset = renderer->capset(virgl2CapsetId).data;
    virgl_caps_v2 caps {};
    require(capset.size() >=
            offsetof(virgl_caps_v2, capability_bits_v2) +
                sizeof(caps.capability_bits_v2),
        "the capset is too short to hold the capability bits");
    std::memcpy(&caps, capset.data(), std::min(capset.size(), sizeof(caps)));
    require((caps.capability_bits_v2 & VIRGL_CAP_V2_PACKED_YUV) != 0,
        "the renderer does not advertise packed YUV images");

    require(renderer->createContext(contextId, virgl2CapsetId, "yuv") == 0,
        "the guest context could not be created");

    checkRefused(*renderer);

    Pipeline pipeline(*renderer);
    checkPlainImage(*renderer, pipeline);
    // An even picture and one whose odd sizes put the second chroma plane in
    // the middle of an image row, in each layout.
    for (const bool biplanar : {true, false}) {
        checkPicture(*renderer, pipeline, Picture {biplanar, 10, 6});
        checkPicture(*renderer, pipeline, Picture {biplanar, 9, 5});
        checkPicture(*renderer, pipeline, Picture {biplanar, 33, 17});
    }

    std::fputs("Packed YUV planes: ok\n", stderr);
    return 0;
}
