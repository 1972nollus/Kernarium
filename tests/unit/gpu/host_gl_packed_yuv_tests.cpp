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

#include "host_gl_test_support.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"

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

using uml::gpu::protocol::virgl2CapsetId;
using uml::gpu::testing::pipeTexture2d;
using uml::gpu::testing::skipWithoutRenderer;
using uml::gpu::testing::virglBindRenderTarget;
using uml::gpu::testing::virglBindSamplerView;
using uml::gpu::testing::virglFormatR8g8b8a8Unorm;
using uml::gpu::VirglRenderer;
using uml::testing::require;

// virgl_protocol.h and virgl_hw.h values the guest sends.
constexpr std::uint32_t commandCreateObject = 1;
constexpr std::uint32_t commandBindObject = 2;
constexpr std::uint32_t commandSetViewportState = 4;
constexpr std::uint32_t commandSetFramebufferState = 5;
constexpr std::uint32_t commandSetVertexBuffers = 6;
constexpr std::uint32_t commandDrawVbo = 8;
constexpr std::uint32_t commandSetSamplerViews = 10;
constexpr std::uint32_t commandBindSamplerStates = 18;
constexpr std::uint32_t commandBindShader = 31;
constexpr std::uint32_t objectBlend = 1;
constexpr std::uint32_t objectRasterizer = 2;
constexpr std::uint32_t objectDepthStencilAlpha = 3;
constexpr std::uint32_t objectShader = 4;
constexpr std::uint32_t objectVertexElements = 5;
constexpr std::uint32_t objectSamplerView = 6;
constexpr std::uint32_t objectSamplerState = 7;
constexpr std::uint32_t objectSurface = 8;
constexpr std::uint32_t pipeBuffer = 0;
constexpr std::uint32_t shaderVertex = 0;
constexpr std::uint32_t shaderFragment = 1;
constexpr std::uint32_t primitiveTriangleStrip = 5;
constexpr std::uint32_t virglFormatR8Unorm = 64;
constexpr std::uint32_t virglFormatR8g8Unorm = 65;
constexpr std::uint32_t virglFormatR32g32b32a32Float = 31;
constexpr std::uint32_t virglBindVertexBuffer = 1U << 4U;

constexpr std::uint32_t contextId = 1;
constexpr std::uint32_t vertexBufferId = 2;
constexpr std::uint32_t vertexStride = 8 * sizeof(float);

// Wire handles of the objects the pipeline is made of. Those that come and go
// with a picture are numbered from firstTransientHandle and never reused.
constexpr std::uint32_t blendHandle = 1;
constexpr std::uint32_t depthHandle = 2;
constexpr std::uint32_t rasterizerHandle = 3;
constexpr std::uint32_t vertexShaderHandle = 4;
constexpr std::uint32_t fragmentShaderHandle = 5;
constexpr std::uint32_t elementsHandle = 6;
constexpr std::uint32_t samplerStateHandle = 7;
constexpr std::uint32_t firstTransientHandle = 16;

[[nodiscard]] std::uint32_t bits(float value) noexcept
{
    return std::bit_cast<std::uint32_t>(value);
}

[[nodiscard]] std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment)
{
    return (value + alignment - 1U) / alignment * alignment;
}

// One command buffer for the context, in 32-bit words.
class Commands final {
public:
    void add(
        std::uint32_t command,
        std::uint32_t object,
        std::initializer_list<std::uint32_t> payload)
    {
        words_.push_back(
            (static_cast<std::uint32_t>(payload.size()) << 16U) |
            (object << 8U) | command);
        words_.insert(words_.end(), payload.begin(), payload.end());
    }

    // CREATE_OBJECT of a shader from TGSI text, which is sent whole with its
    // terminator and padded to a word.
    void addShader(
        std::uint32_t handle, std::uint32_t type, std::string_view text)
    {
        std::vector<std::uint32_t> payload {
            handle, type, static_cast<std::uint32_t>(text.size() + 1), 300, 0};
        std::string padded(text);
        padded.resize(alignUp(static_cast<std::uint32_t>(text.size() + 1), 4U));
        for (std::size_t offset = 0; offset < padded.size(); offset += 4) {
            std::uint32_t word = 0;
            std::memcpy(&word, padded.data() + offset, sizeof(word));
            payload.push_back(word);
        }
        words_.push_back(
            (static_cast<std::uint32_t>(payload.size()) << 16U) |
            (objectShader << 8U) | commandCreateObject);
        words_.insert(words_.end(), payload.begin(), payload.end());
    }

    [[nodiscard]] int submit(VirglRenderer& renderer)
    {
        const int result = renderer.submit(
            contextId,
            {reinterpret_cast<const std::uint8_t*>(words_.data()),
                words_.size() * sizeof(std::uint32_t)});
        words_.clear();
        return result;
    }

private:
    std::vector<std::uint32_t> words_;
};

// A resource with guest storage the renderer reads and writes by transfer.
class Image final {
public:
    Image(
        VirglRenderer& renderer,
        std::uint32_t resourceId,
        std::uint32_t target,
        std::uint32_t format,
        std::uint32_t bind,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t flags,
        std::size_t bytes)
        : renderer_ {renderer}
        , resourceId_ {resourceId}
        , storage_(bytes)
    {
        require(create(renderer, resourceId, target, format, bind, width,
                    height, flags) == 0,
            "the image could not be created");
        vectors_[0] = {.iov_base = storage_.data(), .iov_len = storage_.size()};
        require(renderer_.attachBacking(resourceId_, vectors_) == 0,
            "the image's guest storage could not be attached");
        require(renderer_.attachResource(contextId, resourceId_) == 0,
            "the image could not be attached to the context");
    }

    ~Image()
    {
        (void)renderer_.detachResource(contextId, resourceId_);
        renderer_.detachBacking(resourceId_);
        renderer_.unrefResource(resourceId_);
    }

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    [[nodiscard]] static int create(
        VirglRenderer& renderer,
        std::uint32_t resourceId,
        std::uint32_t target,
        std::uint32_t format,
        std::uint32_t bind,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t flags)
    {
        return renderer.createResource3d(VirglRenderer::Resource3d {
            .resourceId = resourceId,
            .target = target,
            .format = format,
            .bind = bind,
            .width = width,
            .height = height,
            .depth = 1,
            .arraySize = 1,
            .flags = flags,
        });
    }

    [[nodiscard]] std::vector<std::uint8_t>& bytes() noexcept
    {
        return storage_;
    }

    // Sends rows [first, first + count) of a `stride` byte wide image.
    void write(std::uint32_t stride, std::uint32_t first, std::uint32_t count)
    {
        const VirglRenderer::Box box {
            .x = 0,
            .y = first,
            .z = 0,
            .width = stride,
            .height = count,
            .depth = 1,
        };
        require(renderer_.transferResource(
                    true, resourceId_, contextId, 0, stride, 0, box,
                    static_cast<std::uint64_t>(first) * stride) == 0,
            "the rows could not be written");
    }

private:
    VirglRenderer& renderer_;
    std::uint32_t resourceId_;
    std::vector<std::uint8_t> storage_;
    std::array<iovec, 1> vectors_ {};
};

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

// The draw that shows a texture's texels one for one in a render target of its
// own size: a viewport-filling strip that samples with nearest filtering.
class Pipeline final {
public:
    explicit Pipeline(VirglRenderer& renderer)
        : renderer_ {renderer}
        , vertices_(
              renderer, vertexBufferId, pipeBuffer, virglFormatR8Unorm,
              virglBindVertexBuffer, 4U * vertexStride, 1, 0, 4U * vertexStride)
    {
        // Positions in clip space with the texture coordinate at the same
        // corner of the unit square.
        const float corners[4][8] = {
            {-1, -1, 0, 1, 0, 0, 0, 1},
            {1, -1, 0, 1, 1, 0, 0, 1},
            {-1, 1, 0, 1, 0, 1, 0, 1},
            {1, 1, 0, 1, 1, 1, 0, 1},
        };
        std::memcpy(vertices_.bytes().data(), corners, sizeof(corners));
        const VirglRenderer::Box box {
            .width = 4U * vertexStride,
            .height = 1,
            .depth = 1,
        };
        require(renderer_.transferResource(
                    true, vertexBufferId, contextId, 0, 0, 0, box, 0) == 0,
            "the vertices could not be written");

        Commands commands;
        commands.add(commandCreateObject, objectBlend,
            {blendHandle, 0, 0, 0xfU << 27U, 0, 0, 0, 0, 0, 0, 0});
        commands.add(commandBindObject, objectBlend, {blendHandle});
        commands.add(commandCreateObject, objectDepthStencilAlpha,
            {depthHandle, 0, 0, 0, 0});
        commands.add(commandBindObject, objectDepthStencilAlpha, {depthHandle});
        // depth_clip, half_pixel_center and bottom_edge_rule, with unit point
        // size and line width.
        commands.add(commandCreateObject, objectRasterizer,
            {rasterizerHandle, (1U << 1U) | (1U << 29U) | (1U << 30U),
                bits(1.0F), 0, 0, bits(1.0F), 0, 0, 0});
        commands.add(commandBindObject, objectRasterizer, {rasterizerHandle});
        commands.addShader(vertexShaderHandle, shaderVertex,
            "VERT\n"
            "DCL IN[0]\n"
            "DCL IN[1]\n"
            "DCL OUT[0], POSITION\n"
            "DCL OUT[1], GENERIC[0]\n"
            "  0: MOV OUT[1], IN[1]\n"
            "  1: MOV OUT[0], IN[0]\n"
            "  2: END\n");
        commands.add(commandBindShader, 0, {vertexShaderHandle, shaderVertex});
        commands.addShader(fragmentShaderHandle, shaderFragment,
            "FRAG\n"
            "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
            "DCL OUT[0], COLOR\n"
            "DCL SAMP[0]\n"
            "DCL SVIEW[0], 2D, FLOAT\n"
            "  0: TEX OUT[0], IN[0], SAMP[0], 2D\n"
            "  1: END\n");
        commands.add(
            commandBindShader, 0, {fragmentShaderHandle, shaderFragment});
        commands.add(commandCreateObject, objectVertexElements,
            {elementsHandle, 0, 0, 0, virglFormatR32g32b32a32Float,
                4U * sizeof(float), 0, 0, virglFormatR32g32b32a32Float});
        commands.add(commandBindObject, objectVertexElements, {elementsHandle});
        commands.add(commandSetVertexBuffers, 0,
            {vertexStride, 0, vertexBufferId});
        // Clamp to the edge, nearest filtering, no mip levels.
        commands.add(commandCreateObject, objectSamplerState,
            {samplerStateHandle, 2U | (2U << 3U) | (2U << 6U) | (2U << 11U),
                0, 0, 0, 0, 0, 0, 0});
        commands.add(commandBindSamplerStates, 0,
            {shaderFragment, 0, samplerStateHandle});
        require(commands.submit(renderer_) == 0,
            "the pipeline could not be created");
    }

    // A view of `plane` of the image `resourceId`, in `format`.
    [[nodiscard]] std::uint32_t createView(
        std::uint32_t resourceId,
        std::uint32_t format,
        std::uint32_t plane)
    {
        const std::uint32_t handle = nextHandle_++;
        Commands commands;
        // The texture target rides in the top byte of the format word, and a
        // plane other than the first is sent where the layers go.
        commands.add(commandCreateObject, objectSamplerView,
            {handle, resourceId, format | (pipeTexture2d << 24U), plane, 0,
                0U | (1U << 3U) | (2U << 6U) | (3U << 9U)});
        require(commands.submit(renderer_) == 0,
            "the sampler view could not be created");
        return handle;
    }

    // Draws the view into an RGBA render target of the given size and returns
    // its texels.
    [[nodiscard]] std::vector<std::uint8_t> sample(
        std::uint32_t viewHandle,
        std::uint32_t width,
        std::uint32_t height)
    {
        const std::uint32_t targetId = 100;
        const std::uint32_t surfaceHandle = nextHandle_++;
        Image target(renderer_, targetId, pipeTexture2d,
            virglFormatR8g8b8a8Unorm,
            virglBindRenderTarget | virglBindSamplerView, width, height, 0,
            static_cast<std::size_t>(width) * height * 4U);
        Commands commands;
        commands.add(commandCreateObject, objectSurface,
            {surfaceHandle, targetId, virglFormatR8g8b8a8Unorm, 0, 0});
        commands.add(commandSetFramebufferState, 0, {1, 0, surfaceHandle});
        const float halfWidth = static_cast<float>(width) / 2.0F;
        const float halfHeight = static_cast<float>(height) / 2.0F;
        commands.add(commandSetViewportState, 0,
            {0, bits(halfWidth), bits(halfHeight), bits(0.5F), bits(halfWidth),
                bits(halfHeight), bits(0.5F)});
        commands.add(commandSetSamplerViews, 0, {shaderFragment, 0, viewHandle});
        commands.add(commandDrawVbo, 0,
            {0, 4, primitiveTriangleStrip, 0, 1, 0, 0, 0, 0, 0, 3, 0});
        require(commands.submit(renderer_) == 0, "the draw failed");
        require(renderer_.createFenceAndWait(contextId, 0, ++fence_, nullptr) ==
                0,
            "the draw did not complete");

        std::vector<std::uint8_t> texels(
            static_cast<std::size_t>(width) * height * 4U);
        require(renderer_.readResourceRectangle(
                    targetId, texels, 0, 0, width, height) == 0,
            "the render target could not be read");
        return texels;
    }

private:
    VirglRenderer& renderer_;
    Image vertices_;
    std::uint64_t fence_ {};
    std::uint32_t nextHandle_ = firstTransientHandle;
};

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
