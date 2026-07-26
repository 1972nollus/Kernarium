// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

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
        std::string_view debugName);
    void destroyContext(std::uint32_t contextId);

    [[nodiscard]] int submit(
        std::uint32_t contextId,
        std::span<const std::uint8_t> command);
    [[nodiscard]] int createResource3d(const Resource3d& resource);
    [[nodiscard]] int transferResource(
        bool toHost,
        std::uint32_t resourceId,
        std::uint32_t contextId,
        std::uint32_t level,
        std::uint32_t stride,
        std::uint32_t layerStride,
        const Box& box,
        std::uint64_t offset);
    void readResourceRectangle(
        std::uint32_t resourceId,
        std::span<std::uint8_t> destination,
        std::uint32_t offset,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height);
    [[nodiscard]] int createFenceAndWait(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceId);
    void unrefResource(std::uint32_t resourceId);
    [[nodiscard]] int attachBacking(
        std::uint32_t resourceId,
        std::span<iovec> backing);
    void detachBacking(std::uint32_t resourceId);
    void attachResource(
        std::uint32_t contextId,
        std::uint32_t resourceId);
    void detachResource(
        std::uint32_t contextId,
        std::uint32_t resourceId);
private:
    static void* createGlContext(
        void* cookie,
        int scanoutIndex,
        virgl_renderer_gl_ctx_param* parameters);
    static void destroyGlContext(void* cookie, void* context);
    static int makeGlContextCurrent(
        void* cookie,
        int scanoutIndex,
        void* context);
    static void writeFence(void* cookie, std::uint32_t fenceId);
    static void writeContextFence(
        void* cookie,
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceId);
    [[nodiscard]] static std::uint64_t contextRingKey(
        std::uint32_t contextId,
        std::uint32_t ringIndex) noexcept;

    mutable std::mutex mutex_;
    std::mutex glContextMutex_;
    std::condition_variable fenceCondition_;
    std::unordered_map<std::uint64_t, std::uint64_t> retiredContextFences_;
    void* sharedGlContext_ {};
    std::uint32_t retiredFence_ {};
};

} // namespace uml::gpu
