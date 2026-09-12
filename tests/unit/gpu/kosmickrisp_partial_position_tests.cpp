// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "shaders/partial_position_spirv.hpp"

#include <array>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using uml::gpu::test::DrawConfig;
using uml::gpu::test::DrawContext;
using uml::gpu::test::DrawResult;

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

/* A clip coordinate is four components whatever the shader stores into it, so
 * a vertex shader that writes only gl_Position.xy must still build. A backend
 * that sizes the position output from the store declares a two-component
 * position, which the shading language rejects outright: the pipeline never
 * builds and every draw that needs it is lost. Covering the fully written
 * shader alongside it keeps the check honest about what changed.
 */
void draw_triangle(DrawContext &context, std::span<const uint32_t> vertex,
                   const char *name, bool defines_whole_position)
{
    DrawConfig config;
    config.vertex_shader = vertex;
    config.fragment_shader = pp_fragment;
    config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    config.vertex_count = 3;
    /* Reaching a result at all is the assertion for the partial shader: a
     * backend that declares the position at the shape of the store cannot
     * build its shader library, so pipeline creation fails before any draw.
     * Where the clip coordinate is fully defined the triangle must also
     * cover the framebuffer; where it is not, the spec leaves the rasterized
     * position undefined and only the build is meaningful.
     */
    const DrawResult result = context.draw(config);
    require(result.pixels.size() == static_cast<size_t>(config.width) * config.height,
            std::string(name) + ": unexpected framebuffer size");
    if (!defines_whole_position)
        return;
    const std::array<uint8_t, 4> red {255, 0, 0, 255};
    require(result.pixels[32 * config.width + 32] == red,
            std::string(name) + ": the covering triangle did not reach the framebuffer");
}

} // namespace

int main(int argc, char **argv)
{
    std::string current;
    try {
        require(argc <= 2, "expected at most one exact test selector");
        DrawContext context;
        const struct {
            const char *name;
            std::span<const uint32_t> vertex;
            bool defines_whole_position;
        } cases[] = {
            {"position-split-stores", pp_vertex_split, true},
            {"position-xy-only", pp_vertex_xy, false},
        };
        size_t completed = 0;
        for (const auto &entry : cases) {
            current = entry.name;
            if (argc == 2 && std::string_view(argv[1]) != current)
                continue;
            draw_triangle(context, entry.vertex, entry.name,
                          entry.defines_whole_position);
            std::printf("partial position probe PASS %s\n", entry.name);
            std::fflush(stdout);
            ++completed;
        }
        require(completed != 0, "no matching test case");
        std::printf("partial position probe PASS: %zu cases\n", completed);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "partial position probe FAIL %s: %s\n", current.c_str(),
                     error.what());
        return 1;
    }
}
