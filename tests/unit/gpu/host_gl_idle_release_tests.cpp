// SPDX-License-Identifier: GPL-2.0-or-later

// What the host driver caches for a context that is gone or idle goes back
// while other contexts go on running: a guest that has closed its heaviest
// application keeps drawing its compositor and its launcher, and nothing in
// that asks the driver to give back what the application's batches and
// buffers left behind, so before it did, the process stayed that much over its
// steady footprint for as long as the guest ran.
//
// Three things hold it. The slabs the driver carves small buffers from are
// freed once every entry has been free for a while, with no allocation of
// their size to find them. The screen hands a context the batch state that
// was freed last, so contexts that run keep using a few and the command
// storage of the others goes back when a host asks for it, instead of every
// state being taken again within a few frames and none ever going idle. And
// a context that is destroyed gives back the storage of all the states it
// used, whether or not a host asks.
//
// Metal's allocation counter is the observable.

#include "host_gl_test_support.hpp"
#include "host_gpu_memory.hpp"
#include "require.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

using uml::gpu::testing::colourTarget;
using uml::gpu::testing::compileShader;
using uml::gpu::testing::linkProgram;
using uml::gpu::testing::TextureStorage;
using uml::testing::require;
using namespace std::chrono_literals;

typedef EGLint (*ReleaseCachedMemory)();
typedef EGLBoolean (*TrimContext)();

constexpr std::uint64_t mebibyte = 1U << 20U;
constexpr GLsizei targetSize = 256;
constexpr int heavyFrames = 48;
constexpr int heavyDraws = 4096;
// What may stay once the driver has given back what it caches: the driver's
// own small allocations.
constexpr std::uint64_t slackBytes = 4 * mebibyte;
// What may stay once the host has released the cached memory of a context that
// went idle: the driver's own allocations and the states the others still use.
constexpr std::uint64_t keptAfterRelease = 48 * mebibyte;
// What the heavy context must have grown for the test to prove anything.
constexpr std::uint64_t grownAtLeast = 16 * mebibyte;
// How long the counter has to come back with nobody asking: the slabs wait a
// second before they go, and the cache that holds their buffers half a second
// more.
constexpr auto comeBackWithin = 6s;

constexpr const char* vertexSource = R"(#version 450 core
uniform vec4 offset;
void main()
{
    const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexID] * 0.01 + offset.xy, 0.0, 1.0);
}
)";

constexpr const char* fragmentSource = R"(#version 450 core
uniform vec4 colour;
out vec4 result;
void main()
{
    result = colour;
}
)";

[[nodiscard]] std::uint64_t allocated()
{
    return uml::platform::hostGpuAllocatedBytes();
}

[[nodiscard]] std::uint64_t grown(std::uint64_t baseline)
{
    const std::uint64_t now = allocated();
    return now > baseline ? now - baseline : 0;
}

/* Draws `frames` frames of `drawsPerFrame` draws into a small target, with
 * new uniforms for every draw, and flushes each frame without waiting. */
void drawHeavily(int frames, int drawsPerFrame)
{
    const auto target = colourTarget(targetSize, targetSize, TextureStorage::immutable);
    require(target.complete, "the heavy context's target was incomplete");
    const GLuint program = linkProgram({compileShader(GL_VERTEX_SHADER, vertexSource),
        compileShader(GL_FRAGMENT_SHADER, fragmentSource)});
    glUseProgram(program);
    const GLint offset = glGetUniformLocation(program, "offset");
    const GLint colour = glGetUniformLocation(program, "colour");
    require(offset >= 0 && colour >= 0, "the program has no uniforms");
    GLuint vertexArray = 0;
    glGenVertexArrays(1, &vertexArray);
    glBindVertexArray(vertexArray);
    glViewport(0, 0, targetSize, targetSize);
    for (int frame = 0; frame < frames; ++frame) {
        glClear(GL_COLOR_BUFFER_BIT);
        for (int draw = 0; draw < drawsPerFrame; ++draw) {
            const float x = static_cast<float>(draw % 64) / 32.0F - 1.0F;
            const float y = static_cast<float>(draw / 64 % 64) / 32.0F - 1.0F;
            glUniform4f(offset, x, y, 0.0F, 0.0F);
            glUniform4f(colour, static_cast<float>(frame) / frames, x, y, 1.0F);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glFlush();
    }
    std::array<std::uint8_t, 4> pixel {};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    require(glGetError() == GL_NO_ERROR, "the heavy context reported an error");
    glFinish();
}


/* A context that goes on drawing something small: a target and a program it
 * keeps, one draw and a flush per frame, `pause` apart, for `duration`. */
void drawLightly(std::chrono::milliseconds duration, std::chrono::milliseconds pause)
{
    const auto target = colourTarget(targetSize, targetSize, TextureStorage::immutable);
    require(target.complete, "the light context's target was incomplete");
    const GLuint program = linkProgram({compileShader(GL_VERTEX_SHADER, vertexSource),
        compileShader(GL_FRAGMENT_SHADER, fragmentSource)});
    glUseProgram(program);
    const GLint offset = glGetUniformLocation(program, "offset");
    const GLint colour = glGetUniformLocation(program, "colour");
    require(offset >= 0 && colour >= 0, "the program has no uniforms");
    GLuint vertexArray = 0;
    glGenVertexArrays(1, &vertexArray);
    glBindVertexArray(vertexArray);
    glViewport(0, 0, targetSize, targetSize);
    const auto end = std::chrono::steady_clock::now() + duration;
    int frame = 0;
    while (std::chrono::steady_clock::now() < end) {
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform4f(offset, 0.0F, 0.0F, 0.0F, 0.0F);
        glUniform4f(colour, static_cast<float>(frame++ % 16) / 16.0F, 0.0F, 0.0F, 1.0F);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFlush();
        std::this_thread::sleep_for(pause);
    }
    glFinish();
    require(glGetError() == GL_NO_ERROR, "the light context reported an error");
}

struct Setup {
    uml::gpu::HostGlProvider& provider;
    void* first;
    ReleaseCachedMemory release;
    TrimContext trim;
};

/* The states a context that went idle leaves are not taken again while others
 * run, so their storage goes when a host asks for the cached memory. */
void checkAnIdleContextGivesItsStatesBack(const Setup& setup)
{
    setup.release();
    std::this_thread::sleep_for(200ms);
    const std::uint64_t baseline = allocated();
    void* const heavy = setup.provider.createContext(uml::gpu::testing::glContextParameters(4, 5));
    require(heavy != nullptr, "no second host GL context");
    require(setup.provider.makeCurrent(heavy) == 0, "the second context would not bind");
    drawHeavily(heavyFrames, heavyDraws);
    const std::uint64_t peak = grown(baseline);
    std::printf("the heavy context grew the counter by %llu MiB\n",
        static_cast<unsigned long long>(peak / mebibyte));
    require(peak >= grownAtLeast,
        "the heavy context grew too little for the test to prove anything");
    require(setup.trim() == EGL_TRUE, "the heavy context's trim failed");
    require(setup.provider.makeCurrent(setup.first) == 0, "the first context would not bind");
    drawLightly(2500ms, 20ms);
    const std::uint64_t beforeRelease = grown(baseline);
    setup.release();
    std::this_thread::sleep_for(200ms);
    const std::uint64_t kept = grown(baseline);
    std::printf("%llu KiB before and %llu KiB after the host released the cached memory\n",
        static_cast<unsigned long long>(beforeRelease >> 10U),
        static_cast<unsigned long long>(kept >> 10U));
    setup.provider.destroyContext(heavy);
    require(kept <= keptAfterRelease,
        "the driver kept the command storage of an idle context after the host released the cached memory");
}

/* A destroyed context's caches go back with nobody asking. */
void checkADestroyedContextLeavesNothing(const Setup& setup)
{
    setup.release();
    std::this_thread::sleep_for(200ms);
    const std::uint64_t baseline = allocated();
    void* const heavy = setup.provider.createContext(uml::gpu::testing::glContextParameters(4, 5));
    require(heavy != nullptr, "no third host GL context");
    require(setup.provider.makeCurrent(heavy) == 0, "the third context would not bind");
    drawHeavily(heavyFrames, heavyDraws);
    const std::uint64_t peak = grown(baseline);
    std::printf("the heavy context grew the counter by %llu MiB\n",
        static_cast<unsigned long long>(peak / mebibyte));
    require(peak >= grownAtLeast,
        "the heavy context grew too little for the test to prove anything");
    require(setup.provider.makeCurrent(setup.first) == 0, "the first context would not bind");
    setup.provider.destroyContext(heavy);
    const auto destroyed = std::chrono::steady_clock::now();
    while (grown(baseline) > slackBytes &&
        std::chrono::steady_clock::now() - destroyed < comeBackWithin)
        std::this_thread::sleep_for(100ms);
    const auto waited = std::chrono::steady_clock::now() - destroyed;
    std::printf("%llu KiB stayed %lld ms after the heavy context was destroyed, with no request\n",
        static_cast<unsigned long long>(grown(baseline) >> 10U),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()));
    require(grown(baseline) <= slackBytes,
        "the driver kept what a destroyed context left in its caches until it was asked");
}

} // namespace

int main()
{
    /* Nothing may be cached, so a released buffer leaves the counter at once.
     * Before the driver starts. */
    ::setenv("ZINK_BO_CACHE_MAX_MIB", "0", 1);

    uml::gpu::HostGlProvider provider;
    const auto first = uml::gpu::testing::currentContext(provider, 4, 5);
    if (first.context == nullptr)
        return uml::gpu::testing::skip(first.failure);
    const EGLDisplay display = eglGetCurrentDisplay();
    require(display != EGL_NO_DISPLAY, "no current EGL display");
    require(epoxy_has_egl_extension(display, "EGL_MESA_context_trim"),
        "the driver does not advertise EGL_MESA_context_trim");
    const auto release = reinterpret_cast<ReleaseCachedMemory>(
        eglGetProcAddress("eglReleaseCachedMemoryMESA"));
    require(release != nullptr, "eglReleaseCachedMemoryMESA was not found");
    const auto trim = reinterpret_cast<TrimContext>(eglGetProcAddress("eglTrimContextMESA"));
    require(trim != nullptr, "eglTrimContextMESA was not found");

    /* The same draws once, so that what the device makes once for every
     * context is in the baseline. */
    drawHeavily(1, 1);
    const Setup setup {provider, first.context, release, trim};

    checkAnIdleContextGivesItsStatesBack(setup);
    checkADestroyedContextLeavesNothing(setup);

    std::printf("PASS idle release\n");
    return EXIT_SUCCESS;
}
