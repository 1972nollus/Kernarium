// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_gl_provider.hpp"
#include "virgl_renderer.hpp"

extern "C" {
#include <virglrenderer.h>
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <source_location>
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

void providerOwnsOneSharedAngleTopology()
{
    uml::gpu::HostGlProvider provider;
    require(provider.display() != nullptr);
    require(provider.display() == provider.display());

    virgl_renderer_gl_ctx_param parameters {
        .version = 2,
        .shared = true,
        .major_ver = 3,
        .minor_ver = 0,
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
    parameters.major_ver = 4;
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
    require(renderer.createFenceAndWait(1, 0, 1, nullptr) == 0);
    renderer.destroyContext(1);
}

} // namespace

int main()
{
    providerRejectsUnsupportedContexts();
    providerReportsExplicitEsVersionRequests();
    providerOwnsOneSharedAngleTopology();
    rendererRetiresFencesThroughTheExternalProvider();
    std::puts("ANGLE/Metal host GL provider lifetime tests passed");
    return 0;
}
