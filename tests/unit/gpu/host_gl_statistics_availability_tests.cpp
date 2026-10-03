// SPDX-License-Identifier: GPL-2.0-or-later

// A GL application may poll GL_QUERY_RESULT_AVAILABLE without ever waiting and
// read the result the first time the query reports available. Zink answers
// such a poll from vkGetQueryPoolResults without VK_QUERY_RESULT_WAIT_BIT and
// keeps the first result it reads, so a host Vulkan driver that lets a
// pipeline-statistics query's availability become visible before its counts
// hands the application a zero that never corrects itself.
// KHR-GL46.pipeline_statistics_query_tests_ARB.functional_geometry_shader_
// queries polls exactly this way.
//
// The contract under test, on the host GL stack alone with no guest and no
// virtio transport: a statistics query that a poll finds available already
// holds its counts, for every single-statistic target a draw feeds.

#include "host_gl_provider.hpp"

#include <epoxy/gl.h>

extern "C" {
#include <virglrenderer.h>
}

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr int iterations = 200;

constexpr const char* vertexSource =
    "#version 330 core\n"
    "layout(location = 0) in vec2 position;\n"
    "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";

constexpr const char* fragmentSource =
    "#version 330 core\n"
    "out vec4 colour;\n"
    "void main() { colour = vec4(1.0); }\n";

struct Statistic {
    GLenum target;
    const char* name;
    GLuint expected;
};

// One triangle of three vertices, drawn without indices.
constexpr std::array<Statistic, 4> statistics {{
    {GL_VERTICES_SUBMITTED_ARB, "GL_VERTICES_SUBMITTED_ARB", 3},
    {GL_PRIMITIVES_SUBMITTED_ARB, "GL_PRIMITIVES_SUBMITTED_ARB", 1},
    {GL_VERTEX_SHADER_INVOCATIONS_ARB, "GL_VERTEX_SHADER_INVOCATIONS_ARB", 3},
    {GL_CLIPPING_INPUT_PRIMITIVES_ARB, "GL_CLIPPING_INPUT_PRIMITIVES_ARB", 1},
}};

[[nodiscard]] GLuint compile(GLenum stage, const char* source)
{
    const GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE) {
        std::array<char, 512> log {};
        glGetShaderInfoLog(shader, log.size(), nullptr, log.data());
        std::fprintf(stderr, "Host statistics availability: shader: %s\n", log.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

int main()
{
    uml::gpu::HostGlProvider provider;
    if (provider.display() == nullptr) {
        std::fputs("no host GL display; skipping\n", stderr);
        return 0;
    }
    virgl_renderer_gl_ctx_param parameters {
        .version = 2,
        .shared = false,
        .major_ver = 4,
        .minor_ver = 6,
        .compat_ctx = 0,
    };
    void* context = provider.createContext(parameters);
    if (context == nullptr) {
        std::fputs("no host GL context; skipping\n", stderr);
        return 0;
    }
    if (provider.makeCurrent(context) != 0) {
        std::fputs("host GL context would not bind; skipping\n", stderr);
        return 0;
    }
    if (epoxy_gl_version() < 46 && !epoxy_has_gl_extension("GL_ARB_pipeline_statistics_query")) {
        std::fputs("no ARB_pipeline_statistics_query; skipping\n", stderr);
        return 0;
    }

    GLuint colour = 0;
    glGenTextures(1, &colour);
    glBindTexture(GL_TEXTURE_2D, colour);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fputs("host framebuffer incomplete; skipping\n", stderr);
        return 0;
    }
    glViewport(0, 0, 64, 64);

    const GLuint vertex = compile(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, fragmentSource);
    if (vertex == 0 || fragment == 0)
        return EXIT_FAILURE;
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        std::fputs("Host statistics availability: the probe program did not link\n", stderr);
        return EXIT_FAILURE;
    }
    glUseProgram(program);

    constexpr std::array<float, 6> positions {-0.5F, -0.5F, 0.5F, -0.5F, 0.0F, 0.5F};
    GLuint array = 0;
    glGenVertexArrays(1, &array);
    glBindVertexArray(array);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    std::fprintf(
        stderr,
        "Host statistics availability: renderer=\"%s\"\n",
        reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    // A poll that never sees the query available is a failure of its own, not
    // a reason to wait forever.
    constexpr auto pollLimit = std::chrono::seconds(10);
    int failures = 0;
    for (const Statistic& statistic : statistics) {
        int early = 0;
        int unavailable = 0;
        GLuint firstBad = 0;
        for (int iteration = 0; iteration < iterations; ++iteration) {
            GLuint query = 0;
            glGenQueries(1, &query);
            glBeginQuery(statistic.target, query);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glEndQuery(statistic.target);

            const auto deadline = std::chrono::steady_clock::now() + pollLimit;
            GLuint available = GL_FALSE;
            do {
                glGetQueryObjectuiv(query, GL_QUERY_RESULT_AVAILABLE, &available);
            } while (available == GL_FALSE && std::chrono::steady_clock::now() < deadline);
            if (available == GL_FALSE) {
                ++unavailable;
            } else {
                GLuint counted = 0;
                glGetQueryObjectuiv(query, GL_QUERY_RESULT, &counted);
                if (counted != statistic.expected) {
                    if (early == 0)
                        firstBad = counted;
                    ++early;
                }
            }
            glDeleteQueries(1, &query);
        }
        std::fprintf(
            stderr,
            "Host statistics availability: %s polled %d times -> %d read "
            "before the count, %d never available (expected %u",
            statistic.name,
            iterations,
            early,
            unavailable,
            statistic.expected);
        if (early != 0)
            std::fprintf(stderr, ", first read %u", firstBad);
        std::fputs(")\n", stderr);
        failures += early + unavailable;
    }

    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &array);
    glDeleteProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &colour);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
