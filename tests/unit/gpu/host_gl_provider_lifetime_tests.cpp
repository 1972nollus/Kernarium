// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_gl_provider.hpp"
#include "virgl_renderer.hpp"

extern "C" {
#include <virglrenderer.h>
}

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <source_location>
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

} // namespace

int main()
{
    providerRejectsUnsupportedContexts();
    providerReportsExplicitEsVersionRequests();
    providerOwnsOneSharedContextTopology();
    providerCreatesSharedContextsOnAnyThread();
    rendererRetiresFencesThroughTheExternalProvider();
    std::puts("host GL provider lifetime tests passed");
    return 0;
}
