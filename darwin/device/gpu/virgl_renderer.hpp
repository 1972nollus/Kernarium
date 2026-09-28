// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

struct iovec;
struct virgl_renderer_gl_ctx_param;

namespace uml::gpu {

class HostGlProvider;

// Owns the process-global virglrenderer instance. Every Apple product uses the
// same opaque EGL provider backed by Metal; provider state never changes this
// public class layout.
class VirglRenderer {
public:
    static constexpr std::uint32_t sharedMemoryBlobDescriptor = 3;

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

    /* The Metal texture a resource is stored in, as the renderer describes
     * it. The texture is borrowed: it stays valid while the resource keeps
     * its storage, so a caller asks again for every frame. */
    struct NativeTexture {
        void* texture {};
        std::uint32_t format {};
        std::uint32_t width {};
        std::uint32_t height {};
        /* The resource's first row is the bottom of the image it holds. */
        bool bottomUp {};
    };

    struct ResourceBlob {
        std::uint32_t resourceId {};
        std::uint32_t contextId {};
        std::uint32_t memory {};
        std::uint32_t flags {};
        std::uint64_t blobId {};
        std::uint64_t size {};
    };

    VirglRenderer();
    ~VirglRenderer();

    /* Bounds the device memory a Venus guest may allocate to `bytes`,
     * counted against everything the host GPU holds for this process, or
     * zero for no bound. Takes effect for the next renderer constructed. */
    static void setDeviceMemoryLimit(std::uint64_t bytes) noexcept;

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
    [[nodiscard]] int createResourceBlob(
        const ResourceBlob& resource) noexcept;
    [[nodiscard]] int exportResourceBlob(
        std::uint32_t resourceId,
        std::uint32_t& descriptorType,
        int& descriptor) noexcept;
    [[nodiscard]] int resourceMapInfo(
        std::uint32_t resourceId,
        std::uint32_t& mapInfo) noexcept;
    [[nodiscard]] int mapResource(
        std::uint32_t resourceId,
        void*& mapping,
        std::uint64_t& size) noexcept;
    void unmapResource(std::uint32_t resourceId) noexcept;
    [[nodiscard]] int createBlobScanout(
        std::uint32_t resourceId,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t format,
        std::uint32_t stride,
        std::uint32_t offset,
        void*& texture) noexcept;
    void releaseBlobScanout(void* texture) noexcept;
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
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height) noexcept;
    [[nodiscard]] int resourceMetalTexture(
        std::uint32_t resourceId,
        NativeTexture& texture) noexcept;
    [[nodiscard]] int createFenceAndWait(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceId,
        const std::atomic_bool* stopRequested) noexcept;
    [[nodiscard]] int createContextFence(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceCookie) noexcept;
    [[nodiscard]] int pollContextFences(
        std::uint32_t contextId) noexcept;
    [[nodiscard]] int pollRenderer() noexcept;
    [[nodiscard]] bool hasPendingWork() noexcept;
    [[nodiscard]] int contextFenceStatus(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceCookie) noexcept;
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
    std::condition_variable fenceCondition_;
    std::unordered_map<std::uint64_t, std::uint64_t> retiredContextFences_;
    std::atomic_bool fenceCallbackFailed_ {};
    std::unique_ptr<HostGlProvider> provider_;
    std::uint32_t retiredFence_ {};
};

} // namespace uml::gpu
