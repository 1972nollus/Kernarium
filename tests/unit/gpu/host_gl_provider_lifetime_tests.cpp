// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_gl_provider.hpp"
#include "virgl_renderer.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

// Another client of the process's EGL library opens the default surfaceless
// display the way the provider bootstraps its own, before libepoxy knows of any
// display.
#undef eglGetPlatformDisplay
extern "C" EGLDisplay eglGetPlatformDisplay(
    EGLenum platform,
    void* nativeDisplay,
    const EGLAttrib* attributes);

extern "C" {
#include <virglrenderer.h>
}

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace {

void require(
    bool condition,
    const std::source_location location = std::source_location::current())
{
    if (condition)
        return;
    std::fprintf(
        stderr,
        "require failed at %s:%u\n",
        location.file_name(),
        location.line());
    std::abort();
}

// The highest desktop version vrend probes, and the one it then asks for again
// for every sub-context it creates.
constexpr int kProbedMajor = 4;
constexpr int kProbedMinor = 6;
// The capability set a Linux guest's Virgl contexts use.
constexpr std::uint32_t kVirgl2CapsetId = 2;

void providerOwnsOneSharedContextTopology()
{
    uml::gpu::HostGlProvider provider;
    require(provider.display() != nullptr);
    require(provider.display() == provider.display());

    virgl_renderer_gl_ctx_param parameters {
        .version = 2,
        .shared = true,
        .major_ver = kProbedMajor,
        .minor_ver = kProbedMinor,
        .compat_ctx = 0,
    };
    require(provider.createContext(parameters) == nullptr);

    parameters.shared = false;
    void* root = provider.createContext(parameters);
    require(root != nullptr);
    require(provider.makeCurrent(root) == 0);

    parameters.shared = true;
    void* shared = provider.createContext(parameters);
    require(shared != nullptr);
    require(provider.makeCurrent(shared) == 0);
    require(provider.makeCurrent(nullptr) == 0);

    provider.destroyContext(shared);
    provider.destroyContext(root);
}

// vrend creates a virgl context's shared sub-context on whichever thread
// services the guest queue, never on the thread that created the display. The
// EGL rendering API a thread is bound to is per-thread state defaulting to
// GLES, so a provider that binds its desktop API only once answers
// EGL_BAD_MATCH there and every guest GL context creation fails.
void providerCreatesSharedContextsOnAnyThread()
{
    uml::gpu::HostGlProvider provider;
    virgl_renderer_gl_ctx_param parameters {
        .version = 2,
        .shared = false,
        .major_ver = kProbedMajor,
        .minor_ver = kProbedMinor,
        .compat_ctx = 0,
    };
    void* root = provider.createContext(parameters);
    require(root != nullptr);
    require(provider.makeCurrent(root) == 0);

    parameters.shared = true;
    void* shared = nullptr;
    int current = -1;
    std::thread consumer([&] {
        shared = provider.createContext(parameters);
        if (shared != nullptr)
            current = provider.makeCurrent(shared);
    });
    consumer.join();
    require(shared != nullptr);
    require(current == 0);

    provider.destroyContext(shared);
    require(provider.makeCurrent(nullptr) == 0);
    provider.destroyContext(root);
}

void providerRejectsUnsupportedContexts()
{
    uml::gpu::HostGlProvider provider;
    virgl_renderer_gl_ctx_param parameters {
        .version = 2,
        .shared = false,
        .major_ver = 3,
        .minor_ver = 0,
        .compat_ctx = 1,
    };
    require(provider.createContext(parameters) == nullptr);
    parameters.compat_ctx = 0;
    parameters.major_ver = 1;
    require(provider.createContext(parameters) == nullptr);
    // The provider serves desktop GL 3.x and 4.x core contexts.
    parameters.major_ver = kProbedMajor + 1;
    require(provider.createContext(parameters) == nullptr);
    parameters.major_ver = 3;
    parameters.version = 1;
    require(provider.createContext(parameters) == nullptr);
}

void providerReportsExplicitEsVersionRequests()
{
    uml::gpu::HostGlProvider provider;
    for (const auto [major, minor] : {
             std::pair {3U, 0U},
             std::pair {3U, 1U},
             std::pair {3U, 2U}}) {
        const auto probe = provider.probeContext(major, minor);
        require(probe.requestedMajor == major);
        require(probe.requestedMinor == minor);
        if (!probe.created) {
            require(probe.eglError != 0);
            continue;
        }
        require(
            probe.actualMajor > major ||
            (probe.actualMajor == major && probe.actualMinor >= minor));
        require(!probe.vendor.empty());
        require(!probe.renderer.empty());
        require(!probe.version.empty());
        require(!probe.shadingLanguageVersion.empty());
        require(std::ranges::is_sorted(probe.extensions));
        require(
            std::adjacent_find(
                probe.extensions.begin(), probe.extensions.end()) ==
            probe.extensions.end());
    }
}

void rendererRetiresFencesThroughTheExternalProvider()
{
    uml::gpu::VirglRenderer renderer;
    require(renderer.createFenceAndWait(0, 0, 1, nullptr) == 0);
    require(renderer.createContext(1, 1, "fence-test") == 0);
    require(renderer.createContextFence(1, 0, 1) == 0);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int status = EBUSY;
    while (status == EBUSY && std::chrono::steady_clock::now() < deadline) {
        require(renderer.pollContextFences(1) == 0);
        status = renderer.contextFenceStatus(1, 0, 1);
        if (status == EBUSY)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    require(status == 0);
    renderer.destroyContext(1);
}

// Stands in for any other client of the process's EGL library, such as the
// application's own graphics self-test: it opens the default surfaceless
// display, initializes it and terminates it again. EGL does not count
// initializations, so that terminate retires the display for everyone it was
// handed to.
void terminateTheDefaultSurfacelessDisplay()
{
    const EGLDisplay other = eglGetPlatformDisplay(
        EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
    require(other != EGL_NO_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    require(eglInitialize(other, &major, &minor) == EGL_TRUE);
    require(eglTerminate(other) == EGL_TRUE);
}

// The provider's display is its own. Once another client had terminated the
// default surfaceless display, which the provider used to share, every guest
// context creation failed with EGL_NOT_INITIALIZED and the running guest lost
// OpenGL.
void providerKeepsItsDisplayWhenAnotherClientTerminatesTheDefaultOne()
{
    uml::gpu::HostGlProvider provider;
    virgl_renderer_gl_ctx_param parameters {
        .version = 2,
        .shared = false,
        .major_ver = kProbedMajor,
        .minor_ver = kProbedMinor,
        .compat_ctx = 0,
    };
    void* root = provider.createContext(parameters);
    require(root != nullptr);
    require(provider.display() != eglGetPlatformDisplay(
        EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr));

    terminateTheDefaultSurfacelessDisplay();

    require(provider.makeCurrent(root) == 0);
    const auto* renderer =
        reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    require(renderer != nullptr);
    // Naming the device must still reach the packaged zink driver.
    require(std::string_view(renderer).find("zink") != std::string_view::npos);
    parameters.shared = true;
    void* shared = provider.createContext(parameters);
    require(shared != nullptr);
    require(provider.makeCurrent(shared) == 0);
    require(provider.makeCurrent(nullptr) == 0);
    provider.destroyContext(shared);
    provider.destroyContext(root);
}

void rendererCreatesContextsAfterAnotherClientTerminatesTheDefaultDisplay()
{
    uml::gpu::VirglRenderer renderer;
    require(renderer.createContext(1, kVirgl2CapsetId, "before") == 0);
    terminateTheDefaultSurfacelessDisplay();
    require(renderer.createContext(2, kVirgl2CapsetId, "after") == 0);
    renderer.destroyContext(2);
    renderer.destroyContext(1);
}

// A renderer whose own display is terminated under it can create no guest
// context, and a capability query that follows such a failure finds no GL
// context on its thread. Both must fail as errors: the query used to read the
// host renderer string through a null pointer and take the process down.
void rendererFailsCleanlyAfterLosingItsDisplay()
{
    uml::gpu::VirglRenderer renderer;
    require(renderer.createContext(1, kVirgl2CapsetId, "before") == 0);
    // The renderer's root context is current on the thread that created it.
    const EGLDisplay display = eglGetCurrentDisplay();
    require(display != EGL_NO_DISPLAY);
    require(eglTerminate(display) == EGL_TRUE);

    int created = 0;
    bool capsetRefused = false;
    // vrend serves the guest queue on a thread of its own.
    std::thread guestQueue([&] {
        created = renderer.createContext(2, kVirgl2CapsetId, "after");
        try {
            (void)renderer.capset(kVirgl2CapsetId);
        } catch (const std::runtime_error&) {
            capsetRefused = true;
        }
    });
    guestQueue.join();
    require(created != 0);
    require(capsetRefused);
    renderer.destroyContext(1);
}

} // namespace

int main()
{
    providerRejectsUnsupportedContexts();
    providerReportsExplicitEsVersionRequests();
    providerOwnsOneSharedContextTopology();
    providerCreatesSharedContextsOnAnyThread();
    rendererRetiresFencesThroughTheExternalProvider();
    providerKeepsItsDisplayWhenAnotherClientTerminatesTheDefaultOne();
    rendererCreatesContextsAfterAnotherClientTerminatesTheDefaultDisplay();
    // Last: it terminates the display every renderer in this process uses.
    rendererFailsCleanlyAfterLosingItsDisplay();
    std::puts("host GL provider lifetime tests passed");
    return 0;
}
