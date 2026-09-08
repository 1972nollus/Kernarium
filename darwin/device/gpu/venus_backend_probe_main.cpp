// SPDX-License-Identifier: GPL-2.0-or-later
#include "kosmickrisp_capability_probe.hpp"

extern "C" {
#include <virglrenderer.h>
}

#include <cerrno>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint32_t venusCapsetId = 4;
constexpr std::uint32_t probeContextId = 1;

void retireFence(
    void*,
    std::uint32_t,
    std::uint32_t,
    std::uint64_t)
{
}

void retireGlobalFence(void*, std::uint32_t)
{
}

void rendererLog(
    virgl_log_level_flags,
    const char* message,
    void*)
{
    if (message != nullptr)
        std::cerr << "venus-renderer: " << message << '\n';
}

class RendererSession {
public:
    RendererSession()
    {
        virgl_set_log_callback(&rendererLog, nullptr, nullptr);
        callbacks_.version = VIRGL_RENDERER_CALLBACKS_VERSION;
        callbacks_.write_fence = &retireGlobalFence;
        callbacks_.write_context_fence = &retireFence;
        constexpr int flags =
            VIRGL_RENDERER_NO_VIRGL |
            VIRGL_RENDERER_VENUS |
            VIRGL_RENDERER_RENDER_SERVER |
            VIRGL_RENDERER_ASYNC_FENCE_CB;
        const int result =
            virgl_renderer_init(this, flags, &callbacks_);
        if (result != 0)
            throw std::runtime_error(
                "virgl_renderer_init failed: " +
                std::to_string(result));
        active_ = true;
    }

    ~RendererSession()
    {
        if (active_)
            virgl_renderer_cleanup(this);
    }

    RendererSession(const RendererSession&) = delete;
    RendererSession& operator=(const RendererSession&) = delete;

private:
    virgl_renderer_callbacks callbacks_ {};
    bool active_ {};
};

} // namespace

int main()
{
    try {
        RendererSession renderer;
        std::uint32_t maximumVersion = 0;
        std::uint32_t maximumSize = 0;
        virgl_renderer_get_cap_set(
            venusCapsetId, &maximumVersion, &maximumSize);
        if (maximumVersion != 0 || maximumSize == 0) {
            std::cerr << "venus-backend-probe: invalid capset 4\n";
            return 1;
        }
        const char debugName[] = "uml-venus-backend-probe";
        const int createResult = virgl_renderer_context_create_with_flags(
            probeContextId,
            venusCapsetId,
            sizeof(debugName) - 1U,
            debugName);
        if (createResult != 0) {
            std::cerr << "venus-backend-probe: context creation failed: "
                      << createResult << '\n';
            return 1;
        }

        std::cout << "UML_VENUS_BACKEND|schema=1|capset_id="
                  << venusCapsetId << "|capset_version="
                  << maximumVersion << "|capset_size="
                  << maximumSize << "|context_flags=" << venusCapsetId
                  << "|context_create=0\n";
        const int deviceResult =
            uml::gpu::run_kosmickrisp_capability_probe(std::cout);
        virgl_renderer_context_destroy(probeContextId);
        return deviceResult;
    } catch (const std::exception& error) {
        std::cerr << "venus-backend-probe: " << error.what() << '\n';
        return 1;
    }
}
