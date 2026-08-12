// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

struct iovec;
struct virgl_renderer_gl_ctx_param;

namespace uml::gpu {

// Owns the process-global virglrenderer instance. Virgl OpenGL contexts are
// supplied by the emulator through CGL, so rendering stays in-process and does
// not depend on an EGL, GLX, or helper-process winsys.
class VirglRenderer {
public:
    struct Capset {
        std::uint32_t maximumVersion {};
        std::vector<std::uint8_t> data;
    };

    struct Resource3d {
        std::uint32_t resourceId {};
        std::uint32_t target {};
        std::uint32_t format {};
        std::uint32_t bind {};
        std::uint32_t width {};
        std::uint32_t height {};
        std::uint32_t depth {};
        std::uint32_t arraySize {};
        std::uint32_t lastLevel {};
        std::uint32_t sampleCount {};
        std::uint32_t flags {};
    };

    struct Box {
        std::uint32_t x {};
        std::uint32_t y {};
        std::uint32_t z {};
        std::uint32_t width {};
        std::uint32_t height {};
        std::uint32_t depth {};
    };

    VirglRenderer();
    ~VirglRenderer();

    VirglRenderer(const VirglRenderer&) = delete;
    VirglRenderer& operator=(const VirglRenderer&) = delete;

    [[nodiscard]] Capset capset(std::uint32_t capsetId) const;
    [[nodiscard]] int createContext(
        std::uint32_t contextId,
        std::uint32_t flags,
        std::string_view debugName) noexcept;
    void destroyContext(std::uint32_t contextId) noexcept;

    [[nodiscard]] int submit(
        std::uint32_t contextId,
        std::span<const std::uint8_t> command) noexcept;
    [[nodiscard]] int createResource3d(
        const Resource3d& resource) noexcept;
    [[nodiscard]] int transferResource(
        bool toHost,
        std::uint32_t resourceId,
        std::uint32_t contextId,
        std::uint32_t level,
        std::uint32_t stride,
        std::uint32_t layerStride,
        const Box& box,
        std::uint64_t offset) noexcept;
    [[nodiscard]] int readResourceRectangle(
        std::uint32_t resourceId,
        std::span<std::uint8_t> destination,
        std::uint32_t offset,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height) noexcept;
    [[nodiscard]] int resourceTexture(
        std::uint32_t resourceId,
        std::uint32_t& textureId) noexcept;
    [[nodiscard]] int resourceMetalTexture(
        std::uint32_t resourceId,
        void*& texture) noexcept;
    [[nodiscard]] int createFenceAndWait(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceId,
        const std::atomic_bool* stopRequested) noexcept;
    void unrefResource(std::uint32_t resourceId) noexcept;
    [[nodiscard]] int attachBacking(
        std::uint32_t resourceId,
        std::span<iovec> backing) noexcept;
    void detachBacking(std::uint32_t resourceId) noexcept;
    [[nodiscard]] int attachResource(
        std::uint32_t contextId,
        std::uint32_t resourceId) noexcept;
    [[nodiscard]] int detachResource(
        std::uint32_t contextId,
        std::uint32_t resourceId) noexcept;
    void resetFenceState() noexcept;
private:
    static void* createGlContext(
        void* cookie,
        int scanoutIndex,
        virgl_renderer_gl_ctx_param* parameters) noexcept;
    static void destroyGlContext(void* cookie, void* context) noexcept;
    static int makeGlContextCurrent(
        void* cookie,
        int scanoutIndex,
        void* context) noexcept;
    static void* getEglDisplay(void* cookie) noexcept;
    static void writeFence(void* cookie, std::uint32_t fenceId) noexcept;
    static void writeContextFence(
        void* cookie,
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceId) noexcept;
    [[nodiscard]] static std::uint64_t contextRingKey(
        std::uint32_t contextId,
        std::uint32_t ringIndex) noexcept;

    mutable std::mutex mutex_;
    std::mutex glContextMutex_;
    std::condition_variable fenceCondition_;
    std::unordered_map<std::uint64_t, std::uint64_t> retiredContextFences_;
    std::atomic_bool fenceCallbackFailed_ {};
    void* sharedGlContext_ {};
#if UML_GPU_ANGLE_METAL
    void* eglDisplay_ {};
    void* eglConfig_ {};
#endif
    std::uint32_t retiredFence_ {};
};

} // namespace uml::gpu
