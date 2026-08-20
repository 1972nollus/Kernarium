// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <mutex>

struct virgl_renderer_gl_ctx_param;

namespace uml::gpu {

// Owns the single product-fixed host GL facade. The public renderer retains
// only an opaque pointer to this object, so provider state cannot change the
// layout seen by protocol, transport, or application consumers.
class HostGlProvider final {
public:
    HostGlProvider() = default;
    ~HostGlProvider();

    HostGlProvider(const HostGlProvider&) = delete;
    HostGlProvider& operator=(const HostGlProvider&) = delete;

    [[nodiscard]] void* createContext(
        const virgl_renderer_gl_ctx_param& parameters) noexcept;
    void destroyContext(void* context) noexcept;
    [[nodiscard]] int makeCurrent(void* context) noexcept;
    [[nodiscard]] void* display() noexcept;

private:
    // EGL display creation and context topology changes are serialized.
    // makeCurrent remains per-thread and lock-free after display publication;
    // renderer cleanup completes before this object's destructor can retire
    // that immutable display.
    std::mutex mutex_;
    std::atomic<void*> display_ {};
    void* config_ {};
    void* sharedContext_ {};
};

} // namespace uml::gpu
