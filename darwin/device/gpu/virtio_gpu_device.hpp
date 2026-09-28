// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "deferred_fences.hpp"
#include "protocol.hpp"
#include "unique_descriptor.hpp"
#include "virgl_renderer.hpp"
#include "display.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_set>
#include <unordered_map>
#include <variant>
#include <vector>
#include <sys/uio.h>

#ifndef UML_GPU_DEVICE_TESTING
#define UML_GPU_DEVICE_TESTING 0
#endif

namespace uml::gpu {

#if UML_GPU_DEVICE_TESTING
namespace testing {
class VirtioGpuDeviceDependencies;
}
#endif

class VirtioGpuDevice {
public:
    using GuestReader =
        std::function<bool(std::uint64_t, std::span<std::uint8_t>)>;
    using GuestMapper =
        std::function<std::span<std::uint8_t>(std::uint64_t, std::size_t)>;
    using DeferredFence = gpu::DeferredFence;
    struct ProcessResult {
        std::vector<std::uint8_t> response;
        std::optional<DeferredFence> fence;
        /* The context this command destroyed. Its renderer fence state is
         * gone, so nothing the transport holds for it can retire any more. */
        std::optional<std::uint32_t> destroyedContextId;
    };

    struct SharedMemoryAction {
        enum class Type : std::uint8_t {
            map,
            unmap,
        };

        Type type {};
        std::uint64_t actionId {};
        std::uint32_t resourceId {};
        std::uint8_t regionId {};
        std::uint64_t descriptorOffset {};
        std::uint64_t regionOffset {};
        std::uint64_t length {};
        bool writable {};
        UniqueDescriptor descriptor;
    };
#if UML_GPU_DEVICE_TESTING
    VirtioGpuDevice(
        testing::VirtioGpuDeviceDependencies& dependencies,
        GuestReader guestReader,
        GuestMapper guestMapper,
        std::uint32_t dotsPerInch = 0);
#else
    VirtioGpuDevice(
        Display& display,
        GuestReader guestReader,
        GuestMapper guestMapper,
        std::uint32_t dotsPerInch = 0);
#endif
    ~VirtioGpuDevice();

    [[nodiscard]] protocol::GpuConfig config() const noexcept;
    void recordConfigRead(
        std::uint32_t offset,
        std::uint32_t size) noexcept;
    void notifyDisplayChanged() noexcept;
    void clearEvents(std::uint32_t events) noexcept;
    [[nodiscard]] bool displayEventPending() const noexcept;
    // The caller owns this flag and must keep it alive until it clears the
    // registration after process() has returned.
    void setStopRequest(const std::atomic_bool* stopRequested) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::vector<std::uint8_t> process(
        std::span<const std::uint8_t> request,
        bool cursorQueue);
    [[nodiscard]] ProcessResult processWithDeferredFence(
        std::span<const std::uint8_t> request,
        bool cursorQueue,
        std::uint64_t fenceCookie);
    [[nodiscard]] bool canProcessWhileFencesPending(
        std::span<const std::uint8_t> request,
        bool cursorQueue) const noexcept;
    [[nodiscard]] int pollContextFences(
        std::uint32_t contextId) noexcept;
    [[nodiscard]] int pollRenderer() noexcept;
    [[nodiscard]] bool rendererHasPendingWork() noexcept;
    [[nodiscard]] int contextFenceStatus(
        const DeferredFence& fence) noexcept;
    [[nodiscard]] std::optional<SharedMemoryAction>
        takeSharedMemoryAction() noexcept;
    void completeSharedMemoryAction(
        std::uint64_t actionId,
        bool success) noexcept;
    void prepareReset() noexcept;

private:
    struct BackingEntry {
        std::uint64_t address {};
        std::uint32_t length {};
    };

    struct Resource {
        protocol::GpuFormat format {};
        std::uint32_t width {};
        std::uint32_t height {};
        std::vector<std::uint8_t> pixels;
        std::vector<BackingEntry> backing;
    };

    struct VirglResource {
        protocol::GpuFormat format {};
        std::uint32_t target {};
        std::uint32_t width {};
        std::uint32_t height {};
        std::vector<BackingEntry> backing;
        std::vector<iovec> vectors;
    };

    struct BlobResource {
        enum class MappingState : std::uint8_t {
            unmapped,
            mapping,
            mapped,
            unmapping,
        };

        std::uint32_t contextId {};
        bool venus {};
        std::uint32_t memory {};
        std::uint32_t flags {};
        std::uint64_t size {};
        std::uint64_t chargedSize {};
        std::uint64_t regionOffset {};
        std::uint64_t pendingActionId {};
        std::vector<BackingEntry> backing;
        std::vector<iovec> vectors;
        MappingState mappingState {MappingState::unmapped};
        bool retireAfterAction {};
    };

    [[nodiscard]] std::vector<std::uint8_t> response(
        const protocol::GpuControlHeader& request,
        protocol::GpuResponse type) const;
    [[nodiscard]] std::vector<std::uint8_t> displayInfo(
        const protocol::GpuControlHeader& request) const;
    [[nodiscard]] std::vector<std::uint8_t> edid(
        const protocol::GpuGetEdid& request) const;
    [[nodiscard]] bool readBacking(
        const Resource& resource,
        std::uint64_t offset,
        std::span<std::uint8_t> destination) const;
    [[nodiscard]] bool parseBacking(
        std::span<const std::uint8_t> request,
        std::size_t entriesOffset,
        std::uint32_t entryCount,
        std::vector<BackingEntry>& backing,
        std::vector<iovec>& vectors) const;
    /* The renderer's capability set, or the response a query for it fails
     * with. */
    [[nodiscard]] std::variant<VirglRenderer::Capset, protocol::GpuResponse>
    rendererCapset(std::uint32_t capsetId) const;
    [[nodiscard]] std::vector<std::uint8_t> capsetInfo(
        const protocol::GpuGetCapsetInfo& request) const;
    [[nodiscard]] std::vector<std::uint8_t> capset(
        const protocol::GpuGetCapset& request) const;
    [[nodiscard]] std::vector<std::uint8_t> processInternal(
        std::span<const std::uint8_t> request,
        bool cursorQueue,
        std::uint64_t fenceCookie,
        std::optional<DeferredFence>* deferredFence,
        std::optional<std::uint32_t>* destroyedContextId);
    void releaseBlobScanout() noexcept;
    void clearScanout() noexcept;
    [[nodiscard]] protocol::GpuResponse presentReadback(
        std::uint32_t resourceId,
        protocol::GpuFormat format,
        const protocol::GpuRectangle& damage);
    [[nodiscard]] std::span<std::uint32_t> blobMappingPages(
        std::uint64_t offset, std::uint64_t size) noexcept;

#if UML_GPU_DEVICE_TESTING
    testing::VirtioGpuDeviceDependencies& dependencies_;
#else
    Display& display_;
#endif
    GuestReader guestReader_;
    GuestMapper guestMapper_;
    std::uint32_t dotsPerInch_;
#if !UML_GPU_DEVICE_TESTING
    VirglRenderer renderer_;
#endif
    std::unordered_map<std::uint32_t, Resource> resources_;
    std::unordered_map<std::uint32_t, VirglResource> virglResources_;
    std::unordered_map<std::uint32_t, BlobResource> blobResources_;
    std::vector<std::uint32_t> blobMappingOwners_;
    std::unordered_set<std::uint32_t> contexts_;
    std::unordered_set<std::uint32_t> venusContexts_;
    std::vector<std::uint8_t> transferStaging_;
    std::vector<std::uint8_t> readbackStaging_;
    const std::atomic_bool* stopRequested_ {};
    std::uint64_t displayGeneration_ {};
    std::uint64_t reportedDisplayGeneration_ {};
    std::uint64_t clearedDisplayGeneration_ {};
    std::vector<SharedMemoryAction> sharedMemoryActions_;
    std::uint64_t nextSharedMemoryActionId_ {1};
    std::uint32_t scanoutResource_ {};
    protocol::GpuRectangle scanoutRectangle_ {};
    protocol::GpuFormat scanoutFormat_ {};
    std::uint32_t scanoutWidth_ {};
    std::uint32_t scanoutHeight_ {};
    std::uint32_t scanoutStride_ {};
    std::uint32_t scanoutOffset_ {};
    void* scanoutTexture_ {};
    /* The scanout is a Virgl context's blob: each flush looks up the Metal
     * texture that stores it and reads it back only when there is none. */
    bool scanoutVirglBlob_ {};
    void* scanoutMapping_ {};
    std::uint64_t scanoutMappingSize_ {};
};

} // namespace uml::gpu
