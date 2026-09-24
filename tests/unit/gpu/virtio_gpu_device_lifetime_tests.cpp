// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"
#include "virtio_gpu_device.hpp"
#include "virtio_gpu_device_testing.hpp"

#include <algorithm>
#include <cerrno>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace uml::gpu::testing {
namespace {

class FakeGpuDependencies final : public VirtioGpuDeviceDependencies {
public:
    enum class Operation {
        none,
        create_context,
        destroy_context,
        submit,
        create_resource,
        transfer,
        read_resource,
        fence,
        unref_resource,
        attach_backing,
        detach_backing,
        attach_resource,
        detach_resource,
        blob_scanout,
        display_update,
    };

    [[nodiscard]] std::uint32_t width() const noexcept override
    {
        return displayWidth;
    }
    [[nodiscard]] std::uint32_t height() const noexcept override
    {
        return displayHeight;
    }

    [[nodiscard]] bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept override
    {
        lastGuestScanoutWidth = width;
        lastGuestScanoutHeight = height;
        ++guestScanoutChangeCount;
        return guestScanoutChangeAccepted;
    }

    void resetGuestSession() noexcept override
    {
        ++guestSessionResetCount;
    }

    void clear() noexcept override
    {
        std::fill(framebuffer.begin(), framebuffer.end(), 0);
        ++clearCount;
    }

    void update(
        std::span<const std::uint8_t> source,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        std::size_t sourceRowBytes,
        protocol::GpuFormat,
        std::uint32_t sourceX,
        std::uint32_t sourceY,
        std::uint32_t destinationX,
        std::uint32_t destinationY,
        std::uint32_t rectangleWidth,
        std::uint32_t rectangleHeight) override
    {
        failIfRequested(Operation::display_update);
        lastDisplaySourceWidth = sourceWidth;
        lastDisplaySourceHeight = sourceHeight;
        lastDisplaySourceRowBytes = sourceRowBytes;
        lastDisplaySourceX = sourceX;
        lastDisplaySourceY = sourceY;
        lastDisplayDestinationX = destinationX;
        lastDisplayDestinationY = destinationY;
        lastDisplayWidth = rectangleWidth;
        lastDisplayHeight = rectangleHeight;
        for (std::uint32_t row = 0; row < rectangleHeight; ++row) {
            const auto sourceOffset =
                static_cast<std::size_t>(sourceY + row) * sourceRowBytes +
                static_cast<std::size_t>(sourceX) *
                    protocol::gpuBytesPerPixel;
            const auto destinationOffset =
                (static_cast<std::size_t>(destinationY + row) * width() +
                    destinationX) * protocol::gpuBytesPerPixel;
            std::memcpy(
                framebuffer.data() + destinationOffset,
                source.data() + sourceOffset,
                static_cast<std::size_t>(rectangleWidth) *
                    protocol::gpuBytesPerPixel);
        }
        ++displayUpdateCount;
    }

    [[nodiscard]] VirglRenderer::Capset capset(
        std::uint32_t capsetId) const override
    {
        return {
            .maximumVersion = capsetId == protocol::venusCapsetId ? 0U : 1U,
            .data = {0, 0, 0, 0},
        };
    }

    [[nodiscard]] int createContext(
        std::uint32_t contextId,
        std::uint32_t flags,
        std::string_view) noexcept override
    {
        if (reject(Operation::create_context))
            return EIO;
        try {
            contexts.insert(contextId);
        } catch (...) {
            return ENOMEM;
        }
        lastContextFlags = flags;
        ++createContextCount;
        return 0;
    }

    void destroyContext(std::uint32_t contextId) noexcept override
    {
        contexts.erase(contextId);
        ++destroyContextCount;
    }

    [[nodiscard]] int submit(
        std::uint32_t,
        std::span<const std::uint8_t>) noexcept override
    {
        if (reject(Operation::submit))
            return EIO;
        ++submitCount;
        return 0;
    }

    [[nodiscard]] int createResource3d(
        const VirglRenderer::Resource3d& resource) noexcept override
    {
        if (reject(Operation::create_resource))
            return EIO;
        try {
            resources.insert(resource.resourceId);
        } catch (...) {
            return ENOMEM;
        }
        ++createResourceCount;
        return 0;
    }

    [[nodiscard]] int createResourceBlob(
        const VirglRenderer::ResourceBlob& resource) noexcept override
    {
        if (reject(Operation::create_resource))
            return EIO;
        try {
            resources.insert(resource.resourceId);
        } catch (...) {
            return ENOMEM;
        }
        lastBlobId = resource.blobId;
        ++createResourceCount;
        return 0;
    }

    [[nodiscard]] int exportResourceBlob(
        std::uint32_t resourceId,
        std::uint32_t& descriptorType,
        int& descriptor) noexcept override
    {
        if (!resources.contains(resourceId))
            return ENOENT;
        descriptorType = VirglRenderer::sharedMemoryBlobDescriptor;
        descriptor = ::dup(STDERR_FILENO);
        return descriptor < 0 ? errno : 0;
    }

    [[nodiscard]] int resourceMapInfo(
        std::uint32_t resourceId,
        std::uint32_t& mapInfo) noexcept override
    {
        if (!resources.contains(resourceId))
            return ENOENT;
        mapInfo = protocol::mapCacheCached;
        return 0;
    }

    [[nodiscard]] int mapResource(
        std::uint32_t resourceId,
        void*& mapping,
        std::uint64_t& size) noexcept override
    {
        if (!resources.contains(resourceId) ||
            reject(Operation::blob_scanout)) {
            mapping = nullptr;
            size = 0;
            return ENOTSUP;
        }
        mapping = blobPixels.data();
        size = blobPixels.size();
        ++mapResourceCount;
        return 0;
    }

    void unmapResource(std::uint32_t resourceId) noexcept override
    {
        lastUnmappedResource = resourceId;
        ++unmapResourceCount;
    }

    [[nodiscard]] int createBlobScanout(
        std::uint32_t resourceId,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t format,
        std::uint32_t stride,
        std::uint32_t offset,
        void*& texture) noexcept override
    {
        ++createBlobScanoutCount;
        if (!resources.contains(resourceId) ||
            reject(Operation::blob_scanout)) {
            texture = nullptr;
            return ENOTSUP;
        }
        lastTextureResource = resourceId;
        lastBlobScanoutWidth = width;
        lastBlobScanoutHeight = height;
        lastBlobScanoutFormat = format;
        lastBlobScanoutStride = stride;
        lastBlobScanoutOffset = offset;
        texture = blobScanoutTexture;
        return 0;
    }

    void releaseBlobScanout(void* texture) noexcept override
    {
        lastReleasedBlobScanout = texture;
        ++releaseBlobScanoutCount;
    }

    [[nodiscard]] int transferResource(
        bool,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        const VirglRenderer::Box&,
        std::uint64_t) noexcept override
    {
        if (reject(Operation::transfer))
            return EIO;
        ++transferCount;
        return 0;
    }

    int readResourceRectangle(
        std::uint32_t resourceId,
        std::span<std::uint8_t> destination,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height) override
    {
        if (reject(Operation::read_resource))
            return EIO;
        failIfRequested(Operation::read_resource);
        const auto rowBytes =
            static_cast<std::size_t>(width) * protocol::gpuBytesPerPixel;
        if (width == 0 || height == 0 ||
            height > destination.size() / rowBytes ||
            destination.size() !=
                static_cast<std::size_t>(height) * rowBytes) {
            return EINVAL;
        }
        lastReadResource = resourceId;
        lastReadX = x;
        lastReadY = y;
        lastReadWidth = width;
        lastReadHeight = height;
        lastReadDestinationBytes = destination.size();
        for (std::uint32_t row = 0; row < height; ++row) {
            for (std::uint32_t column = 0; column < width; ++column) {
                const auto offset =
                    (static_cast<std::size_t>(row) * width + column) *
                    protocol::gpuBytesPerPixel;
                destination[offset] =
                    static_cast<std::uint8_t>(x + column);
                destination[offset + 1U] =
                    static_cast<std::uint8_t>(y + row);
                destination[offset + 2U] = readResourceTag;
                destination[offset + 3U] = 0xffU;
            }
        }
        ++readResourceCount;
        return 0;
    }

    int resourceMetalTexture(
        std::uint32_t resourceId,
        void*& texture) noexcept override
    {
        ++nativeResourceRequestCount;
        if (!nativeResourceAvailable) {
            texture = nullptr;
            return ENOTSUP;
        }
        texture = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1234));
        lastTextureResource = resourceId;
        ++resourceMetalTextureCount;
        return 0;
    }

    PresentOutcome presentMetal(
        const MetalScanout& scanout) noexcept override
    {
        if (!directPresentationEnabled)
            return PresentOutcome::unsupported;
        lastMetalScanout = scanout;
        ++directPresentationCount;
        if (directPresentationDrops) {
            ++directPresentationDropCount;
            return PresentOutcome::dropped;
        }
        return PresentOutcome::presented;
    }

    bool needsFullCpuFallback() const noexcept override
    {
        return fullCpuFallback;
    }

    void recordCpuFallbackFrame() noexcept override
    {
        ++cpuFallbackFrameCount;
    }

    [[nodiscard]] int createFenceAndWait(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceId,
        const std::atomic_bool* stopRequested) noexcept override
    {
        lastFence = {contextId, ringIndex, fenceId};
        lastFenceStopRequest = stopRequested;
        if (stopRequested != nullptr &&
            stopRequested->load(std::memory_order_acquire)) {
            return ECANCELED;
        }
        if (reject(Operation::fence))
            return EIO;
        ++fenceCount;
        return 0;
    }

    [[nodiscard]] int createContextFence(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceCookie) noexcept override
    {
        if (reject(Operation::fence))
            return EIO;
        try {
            retiredContextFences.try_emplace(
                contextRingKey(contextId, ringIndex), 0);
            contextFenceCookies.push_back(fenceCookie);
        } catch (...) {
            return ENOMEM;
        }
        ++contextFenceCount;
        return 0;
    }

    [[nodiscard]] int pollContextFences(
        std::uint32_t contextId) noexcept override
    {
        lastPolledContext = contextId;
        ++contextFencePollCount;
        return 0;
    }

    [[nodiscard]] int pollRenderer() noexcept override
    {
        ++rendererPollCount;
        return 0;
    }

    [[nodiscard]] bool hasPendingWork() noexcept override
    {
        return rendererPendingWork;
    }

    [[nodiscard]] int contextFenceStatus(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceCookie) noexcept override
    {
        const auto iterator = retiredContextFences.find(
            contextRingKey(contextId, ringIndex));
        if (iterator == retiredContextFences.end())
            return EIO;
        return iterator->second >= fenceCookie ? 0 : EBUSY;
    }

    void unrefResource(std::uint32_t resourceId) noexcept override
    {
        resources.erase(resourceId);
        ++unrefResourceCount;
    }

    [[nodiscard]] int attachBacking(
        std::uint32_t resourceId,
        std::span<iovec> backing) noexcept override
    {
        if (reject(Operation::attach_backing))
            return EIO;
        attachedBackingResource = resourceId;
        attachedBackingAddress = backing.data();
        ++attachBackingCount;
        return 0;
    }

    void detachBacking(std::uint32_t resourceId) noexcept override
    {
        if (attachedBackingResource == resourceId) {
            attachedBackingResource = 0;
            attachedBackingAddress = nullptr;
        }
        ++detachBackingCount;
    }

    int attachResource(
        std::uint32_t,
        std::uint32_t) noexcept override
    {
        if (reject(Operation::attach_resource))
            return EIO;
        ++attachResourceCount;
        return 0;
    }

    int detachResource(
        std::uint32_t,
        std::uint32_t) noexcept override
    {
        if (reject(Operation::detach_resource))
            return EIO;
        ++detachResourceCount;
        return 0;
    }

    void resetFenceState() noexcept override
    {
        retiredContextFences.clear();
        contextFenceCookies.clear();
        ++resetFenceCount;
    }

    void retireContextFence(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceCookie)
    {
        retiredContextFences[contextRingKey(contextId, ringIndex)] =
            fenceCookie;
    }

    void rejectNext(Operation operation) noexcept
    {
        rejectedOperation = operation;
    }

    void throwNext(Operation operation) noexcept
    {
        throwingOperation = operation;
    }

    void failAllocationNext(Operation operation) noexcept
    {
        allocationFailureOperation = operation;
    }

    std::array<std::uint8_t, 64U * 64U * protocol::gpuBytesPerPixel>
        framebuffer {};
    std::array<std::uint8_t, 64U * 64U * protocol::gpuBytesPerPixel>
        blobPixels {};
    std::unordered_set<std::uint32_t> contexts;
    std::unordered_set<std::uint32_t> resources;
    void* attachedBackingAddress {};
    std::uint32_t attachedBackingResource {};
    unsigned clearCount {};
    unsigned displayUpdateCount {};
    unsigned createContextCount {};
    std::uint32_t lastContextFlags {};
    unsigned destroyContextCount {};
    unsigned submitCount {};
    unsigned createResourceCount {};
    unsigned transferCount {};
    unsigned readResourceCount {};
    std::uint32_t lastReadResource {};
    std::uint32_t lastReadX {};
    std::uint32_t lastReadY {};
    std::uint32_t lastReadWidth {};
    std::uint32_t lastReadHeight {};
    std::size_t lastReadDestinationBytes {};
    std::uint8_t readResourceTag {0x5aU};
    unsigned nativeResourceRequestCount {};
    unsigned resourceMetalTextureCount {};
    unsigned directPresentationCount {};
    unsigned directPresentationDropCount {};
    unsigned cpuFallbackFrameCount {};
    unsigned createBlobScanoutCount {};
    unsigned releaseBlobScanoutCount {};
    unsigned mapResourceCount {};
    unsigned unmapResourceCount {};
    bool directPresentationEnabled {};
    bool directPresentationDrops {};
    bool nativeResourceAvailable {};
    bool fullCpuFallback {};
    std::uint32_t displayWidth {64};
    std::uint32_t displayHeight {64};
    std::uint32_t lastDisplaySourceWidth {};
    std::uint32_t lastDisplaySourceHeight {};
    std::size_t lastDisplaySourceRowBytes {};
    std::uint32_t lastDisplaySourceX {};
    std::uint32_t lastDisplaySourceY {};
    std::uint32_t lastDisplayDestinationX {};
    std::uint32_t lastDisplayDestinationY {};
    std::uint32_t lastDisplayWidth {};
    std::uint32_t lastDisplayHeight {};
    std::uint32_t lastGuestScanoutWidth {};
    std::uint32_t lastGuestScanoutHeight {};
    unsigned guestScanoutChangeCount {};
    bool guestScanoutChangeAccepted {true};
    unsigned guestSessionResetCount {};
    std::uint32_t lastTextureResource {};
    std::uint32_t lastUnmappedResource {};
    std::uint32_t lastBlobScanoutWidth {};
    std::uint32_t lastBlobScanoutHeight {};
    std::uint32_t lastBlobScanoutFormat {};
    std::uint32_t lastBlobScanoutStride {};
    std::uint32_t lastBlobScanoutOffset {};
    std::uint64_t lastBlobId {};
    void* blobScanoutTexture {
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x5678))};
    void* lastReleasedBlobScanout {};
    MetalScanout lastMetalScanout {};
    unsigned fenceCount {};
    unsigned contextFenceCount {};
    unsigned contextFencePollCount {};
    unsigned rendererPollCount {};
    bool rendererPendingWork {};
    std::uint32_t lastPolledContext {};
    std::vector<std::uint64_t> contextFenceCookies;
    VirtioGpuDevice::DeferredFence lastFence {};
    const std::atomic_bool* lastFenceStopRequest {};
    unsigned unrefResourceCount {};
    unsigned attachBackingCount {};
    unsigned detachBackingCount {};
    unsigned attachResourceCount {};
    unsigned detachResourceCount {};
    unsigned resetFenceCount {};

private:
    [[nodiscard]] static std::uint64_t contextRingKey(
        std::uint32_t contextId,
        std::uint32_t ringIndex) noexcept
    {
        return (static_cast<std::uint64_t>(contextId) << 8U) |
            (ringIndex & 0xffU);
    }

    [[nodiscard]] bool reject(Operation operation) noexcept
    {
        if (rejectedOperation != operation)
            return false;
        rejectedOperation = Operation::none;
        return true;
    }

    void failIfRequested(Operation operation)
    {
        if (allocationFailureOperation == operation) {
            allocationFailureOperation = Operation::none;
            throw std::bad_alloc {};
        }
        if (throwingOperation != operation)
            return;
        throwingOperation = Operation::none;
        throw std::runtime_error("forced GPU dependency failure");
    }

    Operation rejectedOperation {Operation::none};
    Operation throwingOperation {Operation::none};
    Operation allocationFailureOperation {Operation::none};
    std::unordered_map<std::uint64_t, std::uint64_t>
        retiredContextFences;
};

template<typename Request>
[[nodiscard]] std::span<const std::uint8_t> requestBytes(
    const Request& request)
{
    return {
        reinterpret_cast<const std::uint8_t*>(&request),
        sizeof(request),
    };
}

[[nodiscard]] protocol::GpuResponse responseType(
    const std::vector<std::uint8_t>& response)
{
    require(response.size() >= sizeof(protocol::GpuControlHeader));
    protocol::GpuControlHeader header {};
    std::memcpy(&header, response.data(), sizeof(header));
    return static_cast<protocol::GpuResponse>(header.type);
}

[[nodiscard]] protocol::GpuControlHeader responseHeader(
    const std::vector<std::uint8_t>& response)
{
    require(response.size() >= sizeof(protocol::GpuControlHeader));
    protocol::GpuControlHeader header {};
    std::memcpy(&header, response.data(), sizeof(header));
    return header;
}

template<typename Header>
[[nodiscard]] std::vector<std::uint8_t> requestWithEntry(
    const Header& header,
    const protocol::GpuMemoryEntry& entry)
{
    std::vector<std::uint8_t> request(sizeof(header) + sizeof(entry));
    std::memcpy(request.data(), &header, sizeof(header));
    std::memcpy(request.data() + sizeof(header), &entry, sizeof(entry));
    return request;
}

template<typename Request>
void requireFirstAllocationFailsBeforeCommit(
    VirtioGpuDevice& device,
    const Request& request)
{
    failNextAllocation();
    bool failed = false;
    try {
        const auto result = device.process(requestBytes(request), false);
        failed = responseType(result) != protocol::GpuResponse::okNoData;
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    clearAllocationFailure();
    require(failed);
}

void requireFirstAllocationFailsBeforeCommit(
    VirtioGpuDevice& device,
    const std::vector<std::uint8_t>& request)
{
    failNextAllocation();
    bool failed = false;
    try {
        const auto result = device.process(request, false);
        failed = responseType(result) != protocol::GpuResponse::okNoData;
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    clearAllocationFailure();
    require(failed);
}

[[nodiscard]] protocol::GpuResourceCreate2d create2d(
    std::uint32_t resourceId,
    std::uint32_t width = 4,
    std::uint32_t height = 4)
{
    protocol::GpuResourceCreate2d create {};
    create.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::resourceCreate2d);
    create.resourceId = resourceId;
    create.format =
        static_cast<std::uint32_t>(protocol::GpuFormat::b8g8r8a8Unorm);
    create.width = width;
    create.height = height;
    return create;
}

[[nodiscard]] protocol::GpuResourceCreate3d create3d(
    std::uint32_t resourceId,
    std::uint32_t width = 4,
    std::uint32_t height = 4)
{
    protocol::GpuResourceCreate3d create {};
    create.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::resourceCreate3d);
    create.resourceId = resourceId;
    create.target = 2;
    create.format =
        static_cast<std::uint32_t>(protocol::GpuFormat::b8g8r8a8Unorm);
    create.bind = 1U << 3U;
    create.width = width;
    create.height = height;
    create.depth = 1;
    create.arraySize = 1;
    return create;
}

[[nodiscard]] protocol::GpuContextCreate createContext(
    std::uint32_t contextId,
    std::uint32_t contextInit = protocol::virglCapsetId)
{
    protocol::GpuContextCreate context {};
    context.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::contextCreate);
    context.header.contextId = contextId;
    context.contextInit = contextInit;
    return context;
}

void contextInitUsesVirgl2ForTheDeviceSelectedDefault()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [](std::uint64_t, std::span<std::uint8_t>) { return false; },
        [](std::uint64_t, std::size_t) {
            return std::span<std::uint8_t> {};
        }};

    const auto deviceSelected = createContext(401, 0);
    require(responseType(device.process(requestBytes(deviceSelected), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.lastContextFlags == protocol::virgl2CapsetId);

    auto resource = create3d(402);
    require(responseType(device.process(requestBytes(resource), false)) ==
        protocol::GpuResponse::okNoData);
    protocol::GpuContextResource association {};
    association.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::contextAttachResource);
    association.header.contextId = deviceSelected.header.contextId;
    association.resourceId = resource.resourceId;
    require(responseType(device.process(requestBytes(association), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.attachResourceCount == 1);

    const auto virgl = createContext(403, protocol::virglCapsetId);
    require(responseType(device.process(requestBytes(virgl), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.lastContextFlags == protocol::virglCapsetId);

    const auto virgl2 = createContext(404, protocol::virgl2CapsetId);
    require(responseType(device.process(requestBytes(virgl2), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.lastContextFlags == protocol::virgl2CapsetId);

    const auto invalidCapset = createContext(405, 3);
    require(responseType(device.process(requestBytes(invalidCapset), false)) ==
        protocol::GpuResponse::errorInvalidParameter);
    const auto reservedBits = createContext(406, 1U << 8U);
    require(responseType(device.process(requestBytes(reservedBits), false)) ==
        protocol::GpuResponse::errorInvalidParameter);
    require(dependencies.createContextCount == 3);
}

void lifecycleCommandsAllocateBeforeCommitAndRetry()
{
    std::array<std::uint8_t, 256> guestMemory {};
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [](std::uint64_t, std::span<std::uint8_t>) { return false; },
        [&guestMemory](std::uint64_t address, std::size_t size) {
            if (address != 0x1000 || size > guestMemory.size())
                return std::span<std::uint8_t> {};
            return std::span<std::uint8_t> {guestMemory}.first(size);
        }};
    auto context = createContext(501);
    requireFirstAllocationFailsBeforeCommit(device, context);
    require(dependencies.createContextCount == 0);
    require(responseType(device.process(requestBytes(context), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.createContextCount == 1);

    auto rendered = create3d(601);
    requireFirstAllocationFailsBeforeCommit(device, rendered);
    require(dependencies.createResourceCount == 0);
    require(responseType(device.process(requestBytes(rendered), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.createResourceCount == 1);

    protocol::GpuContextResource association {};
    association.header.contextId = context.header.contextId;
    association.resourceId = rendered.resourceId;
    association.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::contextAttachResource);
    requireFirstAllocationFailsBeforeCommit(device, association);
    require(dependencies.attachResourceCount == 0);
    dependencies.rejectNext(
        FakeGpuDependencies::Operation::attach_resource);
    require(responseType(device.process(requestBytes(association), false)) ==
        protocol::GpuResponse::errorUnspecified);
    require(dependencies.attachResourceCount == 0);
    require(responseType(device.process(requestBytes(association), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.attachResourceCount == 1);

    association.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::contextDetachResource);
    requireFirstAllocationFailsBeforeCommit(device, association);
    require(dependencies.detachResourceCount == 0);
    dependencies.rejectNext(
        FakeGpuDependencies::Operation::detach_resource);
    require(responseType(device.process(requestBytes(association), false)) ==
        protocol::GpuResponse::errorUnspecified);
    require(dependencies.detachResourceCount == 0);
    require(responseType(device.process(requestBytes(association), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.detachResourceCount == 1);

    protocol::GpuAttachBacking attach {};
    attach.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceAttachBacking);
    attach.resourceId = rendered.resourceId;
    attach.entryCount = 1;
    const protocol::GpuMemoryEntry entry {
        .address = 0x1000,
        .length = static_cast<std::uint32_t>(guestMemory.size()),
    };
    const auto attachRequest = requestWithEntry(attach, entry);
    requireFirstAllocationFailsBeforeCommit(device, attachRequest);
    require(dependencies.attachBackingCount == 0);
    dependencies.rejectNext(
        FakeGpuDependencies::Operation::attach_backing);
    require(responseType(device.process(attachRequest, false)) ==
        protocol::GpuResponse::errorInvalidParameter);
    require(dependencies.attachedBackingAddress == nullptr);
    require(responseType(device.process(attachRequest, false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.attachBackingCount == 1);
    require(dependencies.attachedBackingAddress != nullptr);

    protocol::GpuResourceReference reference {};
    reference.resourceId = rendered.resourceId;
    reference.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceDetachBacking);
    requireFirstAllocationFailsBeforeCommit(device, reference);
    require(dependencies.detachBackingCount == 0);
    require(dependencies.attachedBackingAddress != nullptr);
    require(responseType(device.process(requestBytes(reference), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.detachBackingCount == 1);
    require(dependencies.attachedBackingAddress == nullptr);

    reference.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceUnref);
    requireFirstAllocationFailsBeforeCommit(device, reference);
    require(dependencies.unrefResourceCount == 0);
    require(responseType(device.process(requestBytes(reference), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.unrefResourceCount == 1);
    require(!dependencies.resources.contains(rendered.resourceId));

    protocol::GpuControlHeader destroy = context.header;
    destroy.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::contextDestroy);
    requireFirstAllocationFailsBeforeCommit(device, destroy);
    require(dependencies.destroyContextCount == 0);
    require(responseType(device.process(requestBytes(destroy), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.destroyContextCount == 1);
    require(!dependencies.contexts.contains(context.header.contextId));

    require(responseType(device.process(requestBytes(destroy), false)) ==
        protocol::GpuResponse::errorInvalidContext);

    auto software = create2d(602);
    requireFirstAllocationFailsBeforeCommit(device, software);
    require(responseType(device.process(requestBytes(software), false)) ==
        protocol::GpuResponse::okNoData);

    attach.resourceId = software.resourceId;
    const auto softwareAttachRequest = requestWithEntry(attach, entry);
    requireFirstAllocationFailsBeforeCommit(device, softwareAttachRequest);
    require(responseType(device.process(softwareAttachRequest, false)) ==
        protocol::GpuResponse::okNoData);
    reference.resourceId = software.resourceId;
    reference.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceDetachBacking);
    requireFirstAllocationFailsBeforeCommit(device, reference);
    require(responseType(device.process(requestBytes(reference), false)) ==
        protocol::GpuResponse::okNoData);
    reference.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceUnref);
    requireFirstAllocationFailsBeforeCommit(device, reference);
    require(responseType(device.process(requestBytes(reference), false)) ==
        protocol::GpuResponse::okNoData);
}

void edidReportsDisplayPixelsAndEffectiveDensity()
{
    FakeGpuDependencies dependencies;
    dependencies.displayWidth = 1290;
    dependencies.displayHeight = 2796;
    constexpr std::uint32_t dotsPerInch = 288;
    VirtioGpuDevice device {
        dependencies,
        [](std::uint64_t, std::span<std::uint8_t>) { return false; },
        [](std::uint64_t, std::size_t) {
            return std::span<std::uint8_t> {};
        },
        dotsPerInch};

    protocol::GpuGetEdid request {};
    request.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::getEdid);
    request.header.flags = protocol::gpuFlagFence;
    request.header.fenceId = 0x1234;
    request.scanoutId = protocol::primaryScanoutId;
    const auto bytes = device.process(requestBytes(request), false);
    require(bytes.size() == sizeof(protocol::GpuEdid));
    protocol::GpuEdid response {};
    std::memcpy(&response, bytes.data(), sizeof(response));
    require(responseType(bytes) == protocol::GpuResponse::okEdid);
    require(response.header.flags == protocol::gpuFlagFence);
    require(response.header.fenceId == request.header.fenceId);
    require(response.size == protocol::baseEdidSize);

    unsigned checksum = 0;
    for (std::size_t index = 0; index < response.size; ++index)
        checksum += response.edid[index];
    require((checksum & 0xffU) == 0);
    require(std::equal(
        response.edid.begin(), response.edid.begin() + 8U,
        std::array<std::uint8_t, 8> {
            0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00}.begin()));

    const auto horizontalPixels = static_cast<std::uint32_t>(
        response.edid[56] | ((response.edid[58] & 0xf0U) << 4U));
    const auto verticalPixels = static_cast<std::uint32_t>(
        response.edid[59] | ((response.edid[61] & 0xf0U) << 4U));
    const auto horizontalMillimeters = static_cast<std::uint32_t>(
        response.edid[66] | ((response.edid[68] & 0xf0U) << 4U));
    const auto verticalMillimeters = static_cast<std::uint32_t>(
        response.edid[67] | ((response.edid[68] & 0x0fU) << 8U));
    require(horizontalPixels == dependencies.displayWidth);
    require(verticalPixels == dependencies.displayHeight);
    require(horizontalMillimeters == 114);
    require(verticalMillimeters == 247);

    request.scanoutId = 1;
    require(responseType(device.process(requestBytes(request), false)) ==
        protocol::GpuResponse::errorInvalidScanout);
    protocol::GpuControlHeader truncated {};
    truncated.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::getEdid);
    require(responseType(device.process(requestBytes(truncated), false)) ==
        protocol::GpuResponse::errorInvalidParameter);
}

void displayChangeEventsPreserveAResizeThatRacesGuestClear()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [](std::uint64_t, std::span<std::uint8_t>) { return false; },
        [](std::uint64_t, std::size_t) {
            return std::span<std::uint8_t> {};
        }};

    const auto recordFullConfigRead = [&device] {
        device.recordConfigRead(0, sizeof(protocol::GpuConfig));
    };

    require(device.config().eventsRead == 0);
    recordFullConfigRead();
    device.notifyDisplayChanged();
    require(device.config().eventsRead == protocol::gpuEventDisplay);
    recordFullConfigRead();
    require(device.displayEventPending());

    /* The host publishes another geometry after the guest read events_read
     * but before its matching events_clear write. Clearing the reported
     * generation must leave that newer change asserted. */
    device.notifyDisplayChanged();
    require(device.config().scanoutCount == 1);
    device.recordConfigRead(
        offsetof(protocol::GpuConfig, scanoutCount),
        sizeof(protocol::GpuConfig::scanoutCount));
    device.clearEvents(protocol::gpuEventDisplay);
    require(device.config().eventsRead == protocol::gpuEventDisplay);
    recordFullConfigRead();
    require(device.displayEventPending());
    device.clearEvents(protocol::gpuEventDisplay);
    require(device.config().eventsRead == 0);
    require(!device.displayEventPending());

    dependencies.displayWidth = 1024;
    dependencies.displayHeight = 768;
    protocol::GpuControlHeader request {};
    request.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::getDisplayInfo);
    const auto bytes = device.process(requestBytes(request), false);
    require(bytes.size() == sizeof(protocol::GpuDisplayInfo));
    protocol::GpuDisplayInfo information {};
    std::memcpy(&information, bytes.data(), sizeof(information));
    require(information.modes[0].rectangle.width == 1024);
    require(information.modes[0].rectangle.height == 768);

    device.notifyDisplayChanged();
    device.reset();
    require(device.config().eventsRead == 0);
}

void rendererExecutionFailuresPreserveDeviceState()
{
    std::array<std::uint8_t, 256> guestMemory {};
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [](std::uint64_t, std::span<std::uint8_t>) { return false; },
        [&guestMemory](std::uint64_t address, std::size_t size) {
            if (address != 0x1000 || size > guestMemory.size())
                return std::span<std::uint8_t> {};
            return std::span<std::uint8_t> {guestMemory}.first(size);
        }};
    std::atomic_bool stopRequested {};
    device.setStopRequest(&stopRequested);

    auto context = createContext(701);
    dependencies.rejectNext(
        FakeGpuDependencies::Operation::create_context);
    require(responseType(device.process(requestBytes(context), false)) ==
        protocol::GpuResponse::errorInvalidContext);
    require(!dependencies.contexts.contains(context.header.contextId));
    require(responseType(device.process(requestBytes(context), false)) ==
        protocol::GpuResponse::okNoData);

    auto resource = create3d(702, 4, 4);
    dependencies.rejectNext(
        FakeGpuDependencies::Operation::create_resource);
    require(responseType(device.process(requestBytes(resource), false)) ==
        protocol::GpuResponse::errorInvalidParameter);
    require(!dependencies.resources.contains(resource.resourceId));
    require(responseType(device.process(requestBytes(resource), false)) ==
        protocol::GpuResponse::okNoData);

    protocol::GpuAttachBacking attach {};
    attach.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceAttachBacking);
    attach.resourceId = resource.resourceId;
    attach.entryCount = 1;
    const protocol::GpuMemoryEntry entry {
        .address = 0x1000,
        .length = static_cast<std::uint32_t>(guestMemory.size()),
    };
    const auto attachRequest = requestWithEntry(attach, entry);
    require(responseType(device.process(attachRequest, false)) ==
        protocol::GpuResponse::okNoData);

    protocol::GpuSubmit3d submit {};
    submit.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::submit3d);
    submit.header.contextId = context.header.contextId;
    requireFirstAllocationFailsBeforeCommit(device, submit);
    require(dependencies.submitCount == 0);
    dependencies.rejectNext(FakeGpuDependencies::Operation::submit);
    require(responseType(device.process(requestBytes(submit), false)) ==
        protocol::GpuResponse::errorInvalidContext);
    require(dependencies.submitCount == 0);
    require(responseType(device.process(requestBytes(submit), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.submitCount == 1);

    protocol::GpuTransferHost3d transfer {};
    transfer.header.contextId = context.header.contextId;
    transfer.box = {.width = 1, .height = 1, .depth = 1};
    transfer.resourceId = resource.resourceId;
    for (const auto command : {
             protocol::GpuCommand::transferToHost3d,
             protocol::GpuCommand::transferFromHost3d}) {
        transfer.header.type = static_cast<std::uint32_t>(command);
        const auto before = dependencies.transferCount;
        requireFirstAllocationFailsBeforeCommit(device, transfer);
        require(dependencies.transferCount == before);
        dependencies.rejectNext(FakeGpuDependencies::Operation::transfer);
        require(responseType(device.process(requestBytes(transfer), false)) ==
            protocol::GpuResponse::errorInvalidParameter);
        require(dependencies.transferCount == before);
        require(responseType(device.process(requestBytes(transfer), false)) ==
            protocol::GpuResponse::okNoData);
        require(dependencies.transferCount == before + 1);
    }

    submit.header.flags = protocol::gpuFlagFence;
    submit.header.fenceId = 17;
    dependencies.rejectNext(FakeGpuDependencies::Operation::fence);
    const auto submittedBeforeFenceFailure = dependencies.submitCount;
    require(responseType(device.process(requestBytes(submit), false)) ==
        protocol::GpuResponse::errorUnspecified);
    // Submission acceptance is irreversible. A later fence-status failure is
    // reported exactly once and never causes an internal retry.
    require(dependencies.submitCount == submittedBeforeFenceFailure + 1);
    require(dependencies.fenceCount == 0);
    require(dependencies.lastFence.contextId == context.header.contextId);
    require(dependencies.lastFence.ringIndex == 0);
    require(dependencies.lastFence.cookie == submit.header.fenceId);

    protocol::GpuSetScanout scanout {};
    scanout.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::setScanout);
    scanout.rectangle = {.x = 1, .y = 1, .width = 2, .height = 2};
    scanout.scanoutId = protocol::primaryScanoutId;
    scanout.resourceId = resource.resourceId;
    requireFirstAllocationFailsBeforeCommit(device, scanout);
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::okNoData);

    protocol::GpuResourceFlush flush {};
    flush.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::resourceFlush);
    flush.rectangle = scanout.rectangle;
    flush.resourceId = resource.resourceId;
    requireFirstAllocationFailsBeforeCommit(device, flush);
    require(dependencies.readResourceCount == 0);
    require(dependencies.displayUpdateCount == 0);
    dependencies.rejectNext(FakeGpuDependencies::Operation::read_resource);
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::errorUnspecified);
    require(dependencies.displayUpdateCount == 0);
    dependencies.throwNext(FakeGpuDependencies::Operation::read_resource);
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::errorUnspecified);
    require(dependencies.displayUpdateCount == 0);
    dependencies.failAllocationNext(
        FakeGpuDependencies::Operation::display_update);
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::errorOutOfMemory);
    require(dependencies.displayUpdateCount == 0);
    dependencies.throwNext(
        FakeGpuDependencies::Operation::display_update);
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::errorUnspecified);
    require(dependencies.displayUpdateCount == 0);
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.readResourceCount == 3);
    require(dependencies.displayUpdateCount == 1);

    // Once setScanout has sized the packed readback staging area, a fallback
    // flush allocates only its response. Any second allocation is a regression
    // to per-frame staging or cloning.
    std::vector<std::uint8_t> warmedFlush;
    bool warmedFlushAllocated = false;
    failAfterAllocations(1);
    try {
        warmedFlush = device.process(requestBytes(flush), false);
    } catch (const std::bad_alloc&) {
        warmedFlushAllocated = true;
    }
    clearAllocationFailure();
    require(!warmedFlushAllocated);
    require(responseType(warmedFlush) == protocol::GpuResponse::okNoData);
    require(dependencies.readResourceCount == 4);
    require(dependencies.displayUpdateCount == 2);

    // A nonzero resource-space damage box is packed at the start of the
    // staging span and translated into scanout-relative display coordinates.
    const auto beforePartialFlush = dependencies.framebuffer;
    dependencies.readResourceTag = 0xa5U;
    flush.rectangle = {.x = 2, .y = 2, .width = 1, .height = 1};
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.lastReadResource == resource.resourceId);
    require(dependencies.lastReadX == 2);
    require(dependencies.lastReadY == 2);
    require(dependencies.lastReadWidth == 1);
    require(dependencies.lastReadHeight == 1);
    require(dependencies.lastReadDestinationBytes ==
        protocol::gpuBytesPerPixel);
    require(dependencies.lastDisplaySourceWidth == 1);
    require(dependencies.lastDisplaySourceHeight == 1);
    require(dependencies.lastDisplaySourceRowBytes ==
        protocol::gpuBytesPerPixel);
    require(dependencies.lastDisplaySourceX == 0);
    require(dependencies.lastDisplaySourceY == 0);
    require(dependencies.lastDisplayDestinationX == 1);
    require(dependencies.lastDisplayDestinationY == 1);
    require(dependencies.lastDisplayWidth == 1);
    require(dependencies.lastDisplayHeight == 1);
    constexpr std::size_t changedPixel =
        (64U + 1U) * protocol::gpuBytesPerPixel;
    const std::array<std::uint8_t, protocol::gpuBytesPerPixel>
        expectedPixel {2U, 2U, 0xa5U, 0xffU};
    require(std::memcmp(
        dependencies.framebuffer.data(),
        beforePartialFlush.data(),
        changedPixel) == 0);
    require(std::memcmp(
        dependencies.framebuffer.data() + changedPixel,
        expectedPixel.data(),
        expectedPixel.size()) == 0);
    require(std::memcmp(
        dependencies.framebuffer.data() + changedPixel +
            protocol::gpuBytesPerPixel,
        beforePartialFlush.data() + changedPixel +
            protocol::gpuBytesPerPixel,
        dependencies.framebuffer.size() - changedPixel -
            protocol::gpuBytesPerPixel) == 0);
    require(dependencies.readResourceCount == 5);
    require(dependencies.displayUpdateCount == 3);

    // A display that cannot consume partial damage requests the complete
    // nonzero-origin scanout, still packed at the start of the staging buffer.
    dependencies.fullCpuFallback = true;
    dependencies.readResourceTag = 0xc3U;
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.lastReadX == scanout.rectangle.x);
    require(dependencies.lastReadY == scanout.rectangle.y);
    require(dependencies.lastReadWidth == scanout.rectangle.width);
    require(dependencies.lastReadHeight == scanout.rectangle.height);
    require(dependencies.lastReadDestinationBytes ==
        scanout.rectangle.width * scanout.rectangle.height *
            protocol::gpuBytesPerPixel);
    require(dependencies.lastDisplaySourceWidth == scanout.rectangle.width);
    require(dependencies.lastDisplaySourceHeight == scanout.rectangle.height);
    require(dependencies.lastDisplaySourceX == 0);
    require(dependencies.lastDisplaySourceY == 0);
    require(dependencies.lastDisplayDestinationX == 0);
    require(dependencies.lastDisplayDestinationY == 0);
    const std::array<std::uint8_t, protocol::gpuBytesPerPixel>
        expectedFullOrigin {1U, 1U, 0xc3U, 0xffU};
    require(std::memcmp(
        dependencies.framebuffer.data(),
        expectedFullOrigin.data(),
        expectedFullOrigin.size()) == 0);
    dependencies.fullCpuFallback = false;
    dependencies.readResourceTag = 0x5aU;
    flush.rectangle = scanout.rectangle;

    dependencies.nativeResourceAvailable = true;
    dependencies.directPresentationEnabled = true;
    const auto readsBeforeDirect = dependencies.readResourceCount;
    const auto updatesBeforeDirect = dependencies.displayUpdateCount;
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.resourceMetalTextureCount == 1);
    require(dependencies.directPresentationCount == 1);
    require(dependencies.lastTextureResource == resource.resourceId);
    require(dependencies.lastMetalScanout.texture ==
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1234)));
    require(dependencies.lastMetalScanout.width == scanout.rectangle.width);
    require(dependencies.lastMetalScanout.height == scanout.rectangle.height);
    require(dependencies.lastMetalScanout.damageX == flush.rectangle.x);
    require(dependencies.lastMetalScanout.damageY == flush.rectangle.y);
    require(dependencies.lastMetalScanout.damageWidth == flush.rectangle.width);
    require(
        dependencies.lastMetalScanout.damageHeight == flush.rectangle.height);
    require(dependencies.readResourceCount == readsBeforeDirect);
    require(dependencies.displayUpdateCount == updatesBeforeDirect);

    /* A frame the display drops because host presentation is behind must cost
     * the guest exactly what a presented frame costs: no GPU-to-CPU readback
     * and no CPU display update. That is what decouples the two timelines. */
    dependencies.directPresentationDrops = true;
    const auto readsBeforeDrop = dependencies.readResourceCount;
    const auto updatesBeforeDrop = dependencies.displayUpdateCount;
    const auto fallbacksBeforeDrop = dependencies.cpuFallbackFrameCount;
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.directPresentationDropCount == 1);
    require(dependencies.readResourceCount == readsBeforeDrop);
    require(dependencies.displayUpdateCount == updatesBeforeDrop);
    require(dependencies.cpuFallbackFrameCount == fallbacksBeforeDrop);
    dependencies.directPresentationDrops = false;

    /* An unsupported scanout still takes the readback path, and the display is
     * told so. */
    dependencies.directPresentationEnabled = false;
    const auto fallbacksBeforeRefusal = dependencies.cpuFallbackFrameCount;
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.cpuFallbackFrameCount == fallbacksBeforeRefusal + 1);

    scanout.resourceId = protocol::invalidResourceId;
    requireFirstAllocationFailsBeforeCommit(device, scanout);
    const auto clearsBeforeDisable = dependencies.clearCount;
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.clearCount == clearsBeforeDisable + 1);

    scanout.resourceId = resource.resourceId;
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::okNoData);
    protocol::GpuResourceReference unref {};
    unref.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::resourceUnref);
    unref.resourceId = resource.resourceId;
    const auto clearsBeforeUnref = dependencies.clearCount;
    require(responseType(device.process(requestBytes(unref), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.clearCount == clearsBeforeUnref + 1);
}

void softwareScanoutProgressesWhileRendererFenceIsPending()
{
    const std::array<std::uint8_t, 4> pixel {11, 22, 33, 255};
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [&](std::uint64_t address, std::span<std::uint8_t> destination) {
            if (address != 0x1000 || destination.size() != pixel.size())
                return false;
            std::copy(pixel.begin(), pixel.end(), destination.begin());
            return true;
        },
        [](std::uint64_t, std::size_t) {
            return std::span<std::uint8_t> {};
        }};

    const auto context = createContext(811, protocol::venusCapsetId);
    const auto software = create2d(812, 1, 1);
    const auto rendered = create3d(813, 1, 1);
    require(responseType(device.process(requestBytes(context), false)) ==
        protocol::GpuResponse::okNoData);
    require(responseType(device.process(requestBytes(software), false)) ==
        protocol::GpuResponse::okNoData);
    require(responseType(device.process(requestBytes(rendered), false)) ==
        protocol::GpuResponse::okNoData);

    protocol::GpuAttachBacking attach {};
    attach.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceAttachBacking);
    attach.resourceId = software.resourceId;
    attach.entryCount = 1;
    const protocol::GpuMemoryEntry entry {
        .address = 0x1000,
        .length = static_cast<std::uint32_t>(pixel.size()),
    };
    require(responseType(device.process(
        requestWithEntry(attach, entry), false)) ==
        protocol::GpuResponse::okNoData);
    protocol::GpuSetScanout scanout {};
    scanout.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::setScanout);
    scanout.rectangle = {.x = 0, .y = 0, .width = 1, .height = 1};
    scanout.resourceId = software.resourceId;
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::okNoData);

    protocol::GpuSubmit3d waitSubmit {};
    waitSubmit.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::submit3d);
    waitSubmit.header.contextId = context.header.contextId;
    waitSubmit.header.flags =
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    waitSubmit.header.ringIndex = 1;
    const auto pending = device.processWithDeferredFence(
        requestBytes(waitSubmit), false, 41);
    require(pending.fence.has_value());
    require(device.contextFenceStatus(*pending.fence) == EBUSY);

    /* A console update can precede the submit carrying the host semaphore
     * signal. Both software commands must pass the transport's production
     * predicate without retiring the renderer fence. */
    protocol::GpuTransferToHost2d transfer {};
    transfer.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::transferToHost2d);
    transfer.rectangle = scanout.rectangle;
    transfer.resourceId = software.resourceId;
    require(device.canProcessWhileFencesPending(requestBytes(transfer), false));
    require(responseType(device.process(requestBytes(transfer), false)) ==
        protocol::GpuResponse::okNoData);
    protocol::GpuResourceFlush flush {};
    flush.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceFlush);
    flush.rectangle = scanout.rectangle;
    flush.resourceId = software.resourceId;
    require(device.canProcessWhileFencesPending(requestBytes(flush), false));
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    require(std::equal(
        pixel.begin(), pixel.end(), dependencies.framebuffer.begin()));
    require(device.contextFenceStatus(*pending.fence) == EBUSY);

    /* The same opcode must not admit renderer readback or unknown resources.
     * A software transfer cannot be used to access a renderer resource. */
    flush.resourceId = rendered.resourceId;
    require(!device.canProcessWhileFencesPending(requestBytes(flush), false));
    flush.resourceId = 9999;
    require(!device.canProcessWhileFencesPending(requestBytes(flush), false));
    require(!device.canProcessWhileFencesPending(
        requestBytes(flush).first(sizeof(protocol::GpuControlHeader)), false));
    transfer.resourceId = rendered.resourceId;
    require(responseType(device.process(requestBytes(transfer), false)) ==
        protocol::GpuResponse::errorInvalidResource);
    require(dependencies.transferCount == 0);
    for (const auto command : {
             protocol::GpuCommand::resourceUnref,
             protocol::GpuCommand::resourceDetachBacking,
             protocol::GpuCommand::contextDetachResource,
             protocol::GpuCommand::contextDestroy,
         }) {
        protocol::GpuResourceReference mutation {};
        mutation.header.type = static_cast<std::uint32_t>(command);
        mutation.header.contextId = context.header.contextId;
        mutation.resourceId = rendered.resourceId;
        require(!device.canProcessWhileFencesPending(
            requestBytes(mutation), false));
    }
    auto signalSubmit = waitSubmit;
    signalSubmit.header.flags = 0;
    signalSubmit.header.ringIndex = 0;
    require(device.canProcessWhileFencesPending(
        requestBytes(signalSubmit), false));
    require(responseType(device.process(requestBytes(signalSubmit), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.submitCount == 2);
    dependencies.retireContextFence(context.header.contextId, 1, 41);
    require(device.pollContextFences(context.header.contextId) == 0);
    require(device.contextFenceStatus(*pending.fence) == 0);

    /* Classification follows current ownership when an ID is recycled. */
    protocol::GpuResourceReference unref {};
    unref.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceUnref);
    unref.resourceId = software.resourceId;
    require(responseType(device.process(requestBytes(unref), false)) ==
        protocol::GpuResponse::okNoData);
    flush.resourceId = software.resourceId;
    require(!device.canProcessWhileFencesPending(requestBytes(flush), false));
    const auto replacement = create3d(software.resourceId, 1, 1);
    require(responseType(device.process(requestBytes(replacement), false)) ==
        protocol::GpuResponse::okNoData);
    require(!device.canProcessWhileFencesPending(requestBytes(flush), false));
}

void deferredFencesUseHostTimelineAndMergedRetirement()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [](std::uint64_t, std::span<std::uint8_t>) { return false; },
        [](std::uint64_t, std::size_t) {
            return std::span<std::uint8_t> {};
        }};

    const auto firstContext = createContext(801);
    const auto secondContext = createContext(802);
    require(responseType(device.process(requestBytes(firstContext), false)) ==
        protocol::GpuResponse::okNoData);
    require(responseType(device.process(requestBytes(secondContext), false)) ==
        protocol::GpuResponse::okNoData);

    constexpr std::array<std::uint64_t, 3> guestFenceIds {900, 2, 900};
    constexpr std::array<std::uint64_t, 3> hostFenceCookies {11, 12, 13};
    for (std::size_t index = 0; index < guestFenceIds.size(); ++index) {
        protocol::GpuSubmit3d submit {};
        submit.header.type =
            static_cast<std::uint32_t>(protocol::GpuCommand::submit3d);
        submit.header.flags = protocol::gpuFlagFence;
        submit.header.contextId = firstContext.header.contextId;
        submit.header.fenceId = guestFenceIds[index];
        const auto result = device.processWithDeferredFence(
            requestBytes(submit), false, hostFenceCookies[index]);
        require(result.fence.has_value());
        require(result.fence->contextId == firstContext.header.contextId);
        require(result.fence->ringIndex == 0);
        require(result.fence->cookie == hostFenceCookies[index]);
        const auto reply = responseHeader(result.response);
        require(reply.type == static_cast<std::uint32_t>(
            protocol::GpuResponse::okNoData));
        require(reply.fenceId == guestFenceIds[index]);
        require((reply.flags & protocol::gpuFlagFence) != 0);
    }
    require(dependencies.contextFenceCookies.size() == hostFenceCookies.size());
    require(std::ranges::equal(
        dependencies.contextFenceCookies, hostFenceCookies));

    dependencies.retireContextFence(
        firstContext.header.contextId, 0, hostFenceCookies.back());
    require(device.pollContextFences(firstContext.header.contextId) == 0);
    for (const auto cookie : hostFenceCookies) {
        require(device.contextFenceStatus({
                    .contextId = firstContext.header.contextId,
                    .ringIndex = 0,
                    .cookie = cookie,
                }) == 0);
    }

    protocol::GpuSubmit3d otherSubmit {};
    otherSubmit.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::submit3d);
    otherSubmit.header.flags = protocol::gpuFlagFence;
    otherSubmit.header.contextId = secondContext.header.contextId;
    otherSubmit.header.fenceId = 1;
    const auto otherResult = device.processWithDeferredFence(
        requestBytes(otherSubmit), false, 20);
    require(otherResult.fence.has_value());
    require(device.contextFenceStatus(*otherResult.fence) == EBUSY);
    dependencies.retireContextFence(secondContext.header.contextId, 0, 20);
    require(device.contextFenceStatus(*otherResult.fence) == 0);

    protocol::GpuSubmit3d nonzeroRing {};
    nonzeroRing.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::submit3d);
    nonzeroRing.header.flags =
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    nonzeroRing.header.contextId = firstContext.header.contextId;
    nonzeroRing.header.ringIndex = 1;
    nonzeroRing.header.fenceId = 7;
    const auto synchronous = device.processWithDeferredFence(
        requestBytes(nonzeroRing), false, 21);
    require(!synchronous.fence.has_value());
    require(responseType(synchronous.response) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.fenceCount == 1);

    protocol::GpuSubmit3d failedFence {};
    failedFence.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::submit3d);
    failedFence.header.flags = protocol::gpuFlagFence;
    failedFence.header.contextId = firstContext.header.contextId;
    dependencies.rejectNext(FakeGpuDependencies::Operation::fence);
    const auto submitsBeforeFailure = dependencies.submitCount;
    const auto failure = device.processWithDeferredFence(
        requestBytes(failedFence), false, 22);
    require(!failure.fence.has_value());
    require(responseType(failure.response) ==
        protocol::GpuResponse::errorUnspecified);
    require(dependencies.submitCount == submitsBeforeFailure + 1);
}

void resetRetiresEachExternalOwnerExactlyOnce()
{
    std::array<std::uint8_t, 64> guestMemory {};
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [](std::uint64_t, std::span<std::uint8_t>) { return false; },
        [&guestMemory](std::uint64_t address, std::size_t size) {
            if (address != 0x1000 || size > guestMemory.size())
                return std::span<std::uint8_t> {};
            return std::span<std::uint8_t> {guestMemory}.first(size);
        }};

    const auto context = createContext(801);
    const auto first = create3d(802);
    const auto second = create3d(803);
    require(responseType(device.process(requestBytes(context), false)) ==
        protocol::GpuResponse::okNoData);
    require(responseType(device.process(requestBytes(first), false)) ==
        protocol::GpuResponse::okNoData);
    require(responseType(device.process(requestBytes(second), false)) ==
        protocol::GpuResponse::okNoData);

    protocol::GpuAttachBacking attach {};
    attach.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceAttachBacking);
    attach.resourceId = first.resourceId;
    attach.entryCount = 1;
    const protocol::GpuMemoryEntry entry {
        .address = 0x1000,
        .length = static_cast<std::uint32_t>(guestMemory.size()),
    };
    require(responseType(device.process(
        requestWithEntry(attach, entry), false)) ==
        protocol::GpuResponse::okNoData);

    device.reset();
    require(dependencies.detachBackingCount == 1);
    require(dependencies.unrefResourceCount == 2);
    require(dependencies.destroyContextCount == 1);
    require(dependencies.resetFenceCount == 1);
    require(dependencies.clearCount == 1);
    require(dependencies.contexts.empty());
    require(dependencies.resources.empty());
    require(dependencies.attachedBackingAddress == nullptr);

    device.reset();
    require(dependencies.detachBackingCount == 1);
    require(dependencies.unrefResourceCount == 2);
    require(dependencies.destroyContextCount == 1);
    require(dependencies.resetFenceCount == 2);
    require(dependencies.clearCount == 2);

    require(responseType(device.process(requestBytes(context), false)) ==
        protocol::GpuResponse::okNoData);
    require(responseType(device.process(requestBytes(first), false)) ==
        protocol::GpuResponse::okNoData);
}

void resetIsPristineAndAllocationRetryIsAtomic()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [](std::uint64_t, std::span<std::uint8_t>) { return false; },
        [](std::uint64_t, std::size_t) {
            return std::span<std::uint8_t> {};
        }};

    protocol::GpuContextCreate context {};
    context.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::contextCreate);
    context.header.contextId = 41;
    context.contextInit = protocol::virglCapsetId;
    require(responseType(device.process(requestBytes(context), false)) ==
        protocol::GpuResponse::okNoData);

    auto resource = create2d(73);
    require(responseType(device.process(requestBytes(resource), false)) ==
        protocol::GpuResponse::okNoData);

    device.reset();
    device.reset();
    require(responseType(device.process(requestBytes(context), false)) ==
        protocol::GpuResponse::okNoData);
    require(responseType(device.process(requestBytes(resource), false)) ==
        protocol::GpuResponse::okNoData);

    device.reset();
    context.header.contextId = 42;
    failNextAllocation();
    bool contextFailed = false;
    try {
        (void)device.process(requestBytes(context), false);
    } catch (const std::bad_alloc&) {
        contextFailed = true;
    }
    clearAllocationFailure();
    require(contextFailed);
    require(responseType(device.process(requestBytes(context), false)) ==
        protocol::GpuResponse::okNoData);

    resource.resourceId = 74;
    failNextAllocation();
    const auto failedResource = device.process(requestBytes(resource), false);
    clearAllocationFailure();
    require(responseType(failedResource) ==
        protocol::GpuResponse::errorOutOfMemory);
    require(responseType(device.process(requestBytes(resource), false)) ==
        protocol::GpuResponse::okNoData);

    device.reset();
    const auto overflowingResource =
        create2d(76, UINT32_MAX, UINT32_MAX);
    require(responseType(
        device.process(requestBytes(overflowingResource), false)) ==
        protocol::GpuResponse::errorOutOfMemory);
    const auto overflowRetry = create2d(76);
    require(responseType(device.process(requestBytes(overflowRetry), false)) ==
        protocol::GpuResponse::okNoData);
}

void rendererBackingAttachIsRetryableAfterAllocationFailure()
{
    std::array<std::uint8_t, 64> guestMemory {};
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [](std::uint64_t, std::span<std::uint8_t>) { return false; },
        [&guestMemory](std::uint64_t address, std::size_t size) {
            if (address != 0x1000 || size > guestMemory.size())
                return std::span<std::uint8_t> {};
            return std::span<std::uint8_t> {guestMemory}.first(size);
        }};

    const auto create = create3d(75);
    require(responseType(device.process(requestBytes(create), false)) ==
        protocol::GpuResponse::okNoData);
    protocol::GpuAttachBacking attach {};
    attach.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceAttachBacking);
    attach.resourceId = create.resourceId;
    attach.entryCount = 1;
    const protocol::GpuMemoryEntry entry {
        .address = 0x1000,
        .length = static_cast<std::uint32_t>(guestMemory.size()),
    };
    const auto request = requestWithEntry(attach, entry);

    failAfterAllocations(2);
    bool failed = false;
    try {
        (void)device.process(request, false);
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    clearAllocationFailure();
    require(failed);
    require(responseType(device.process(request, false)) ==
        protocol::GpuResponse::okNoData);
}

void backingRangesAndAttachStateAreValidated()
{
    std::array<std::uint8_t, 64> guestMemory {};
    unsigned mapperCalls = 0;
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [](std::uint64_t, std::span<std::uint8_t>) { return false; },
        [&](std::uint64_t address, std::size_t size) {
            ++mapperCalls;
            if (address != 0x1000 || size > guestMemory.size())
                return std::span<std::uint8_t> {};
            return std::span<std::uint8_t> {guestMemory}.first(size);
        }};

    const auto software = create2d(81);
    const auto rendered = create3d(82);
    require(responseType(device.process(requestBytes(software), false)) ==
        protocol::GpuResponse::okNoData);
    require(responseType(device.process(requestBytes(rendered), false)) ==
        protocol::GpuResponse::okNoData);

    for (const auto resourceId : {software.resourceId, rendered.resourceId}) {
        protocol::GpuAttachBacking attach {};
        attach.header.type = static_cast<std::uint32_t>(
            protocol::GpuCommand::resourceAttachBacking);
        attach.resourceId = resourceId;
        attach.entryCount = 0;
        require(responseType(device.process(requestBytes(attach), false)) ==
            protocol::GpuResponse::errorInvalidParameter);

        attach.entryCount = 1;
        const protocol::GpuMemoryEntry overflow {
            .address = std::numeric_limits<std::uint64_t>::max() - 31U,
            .length = 64,
        };
        require(responseType(device.process(
            requestWithEntry(attach, overflow), false)) ==
            protocol::GpuResponse::errorInvalidParameter);

        const protocol::GpuMemoryEntry valid {
            .address = 0x1000,
            .length = static_cast<std::uint32_t>(guestMemory.size()),
        };
        const auto request = requestWithEntry(attach, valid);
        require(responseType(device.process(request, false)) ==
            protocol::GpuResponse::okNoData);
        require(responseType(device.process(request, false)) ==
            protocol::GpuResponse::errorInvalidParameter);

        protocol::GpuResourceReference detach {};
        detach.header.type = static_cast<std::uint32_t>(
            protocol::GpuCommand::resourceDetachBacking);
        detach.resourceId = resourceId;
        require(responseType(device.process(requestBytes(detach), false)) ==
            protocol::GpuResponse::okNoData);
        require(responseType(device.process(request, false)) ==
            protocol::GpuResponse::okNoData);
    }
    // Only the rendered resource asks the mapper for valid backing. Invalid
    // wrapping ranges are rejected before the callback.
    require(mapperCalls == 2);
}

void failedTransferDoesNotPartiallyCommitPixels()
{
    std::array<std::uint8_t, 16> backing {
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16,
    };
    unsigned reads = 0;
    bool failSecondRead = true;
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [&](std::uint64_t address, std::span<std::uint8_t> destination) {
            ++reads;
            if (failSecondRead && reads == 2)
                return false;
            if (address < 0x1000 ||
                address - 0x1000 > backing.size() ||
                destination.size() > backing.size() - (address - 0x1000)) {
                return false;
            }
            std::memcpy(
                destination.data(),
                backing.data() + (address - 0x1000),
                destination.size());
            return true;
        },
        [](std::uint64_t, std::size_t) {
            return std::span<std::uint8_t> {};
        }};

    const auto create = create2d(83, 2, 2);
    require(responseType(device.process(requestBytes(create), false)) ==
        protocol::GpuResponse::okNoData);
    protocol::GpuAttachBacking attach {};
    attach.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceAttachBacking);
    attach.resourceId = create.resourceId;
    attach.entryCount = 1;
    const protocol::GpuMemoryEntry entry {
        .address = 0x1000,
        .length = static_cast<std::uint32_t>(backing.size()),
    };
    require(responseType(device.process(
        requestWithEntry(attach, entry), false)) ==
        protocol::GpuResponse::okNoData);

    protocol::GpuSetScanout scanout {};
    scanout.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::setScanout);
    scanout.rectangle = {.x = 0, .y = 0, .width = 2, .height = 2};
    scanout.scanoutId = protocol::primaryScanoutId;
    scanout.resourceId = create.resourceId;
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::okNoData);

    protocol::GpuTransferToHost2d transfer {};
    transfer.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::transferToHost2d);
    transfer.rectangle = scanout.rectangle;
    transfer.offset = 0;
    transfer.resourceId = create.resourceId;
    requireFirstAllocationFailsBeforeCommit(device, transfer);
    require(reads == 0);
    require(responseType(device.process(requestBytes(transfer), false)) ==
        protocol::GpuResponse::errorInvalidParameter);

    protocol::GpuResourceFlush flush {};
    flush.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::resourceFlush);
    flush.rectangle = scanout.rectangle;
    flush.resourceId = create.resourceId;
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    auto framebuffer = dependencies.framebuffer;
    for (const auto value : std::span {framebuffer}.first(8U))
        require(value == 0);
    for (const auto value : std::span {framebuffer}.subspan(64U * 4U, 8U))
        require(value == 0);

    reads = 0;
    failSecondRead = false;
    require(responseType(device.process(requestBytes(transfer), false)) ==
        protocol::GpuResponse::okNoData);

    // Pixel conversion is staged before the live framebuffer is locked and
    // committed. An allocation failure therefore reports OOM without leaving
    // a partially converted display update.
    dependencies.failAllocationNext(
        FakeGpuDependencies::Operation::display_update);
    const auto failedFlush = device.process(requestBytes(flush), false);
    require(responseType(failedFlush) ==
        protocol::GpuResponse::errorOutOfMemory);
    framebuffer = dependencies.framebuffer;
    for (const auto value : std::span {framebuffer}.first(8U))
        require(value == 0);
    for (const auto value : std::span {framebuffer}.subspan(64U * 4U, 8U))
        require(value == 0);

    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    framebuffer = dependencies.framebuffer;
    require(std::memcmp(framebuffer.data(), backing.data(), 8) == 0);
    require(std::memcmp(
        framebuffer.data() + 64U * 4U, backing.data() + 8, 8) == 0);

    device.reset();
    framebuffer = dependencies.framebuffer;
    require(std::ranges::all_of(framebuffer, [](std::uint8_t value) {
        return value == 0;
    }));
}

void setScanoutReplyFailureDoesNotCommitAllocation()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [](std::uint64_t, std::span<std::uint8_t>) { return false; },
        [](std::uint64_t, std::size_t) {
            return std::span<std::uint8_t> {};
        }};
    const auto create = create3d(84);
    require(responseType(device.process(requestBytes(create), false)) ==
        protocol::GpuResponse::okNoData);

    protocol::GpuSetScanout scanout {};
    scanout.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::setScanout);
    scanout.rectangle = {.x = 0, .y = 0, .width = 4, .height = 4};
    scanout.scanoutId = protocol::primaryScanoutId;
    scanout.resourceId = create.resourceId;

    failAfterAllocations(1);
    bool responseFailed = false;
    try {
        (void)device.process(requestBytes(scanout), false);
    } catch (const std::bad_alloc&) {
        responseFailed = true;
    }
    clearAllocationFailure();
    require(responseFailed);
    require(dependencies.guestScanoutChangeCount == 0);

    failNextAllocation();
    const auto retry = device.process(requestBytes(scanout), false);
    clearAllocationFailure();
    require(responseType(retry) == protocol::GpuResponse::errorOutOfMemory);
    require(dependencies.guestScanoutChangeCount == 0);
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.guestScanoutChangeCount == 1);
    require(dependencies.lastGuestScanoutWidth == scanout.rectangle.width);
    require(dependencies.lastGuestScanoutHeight == scanout.rectangle.height);

    /* Resource rebinds at the active geometry are page flips, not guest mode
     * changes, and must not disturb the host-to-guest resize policy. */
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.guestScanoutChangeCount == 1);

    scanout.rectangle = {.x = 1, .y = 0, .width = 3, .height = 4};
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.guestScanoutChangeCount == 2);
    require(dependencies.lastGuestScanoutWidth == 3);
    require(dependencies.lastGuestScanoutHeight == 4);

    scanout.resourceId = 999;
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::errorInvalidResource);
    require(dependencies.guestScanoutChangeCount == 2);

    scanout.resourceId = create.resourceId;
    scanout.rectangle = {.x = 0, .y = 0, .width = 2, .height = 4};
    dependencies.guestScanoutChangeAccepted = false;
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::errorOutOfMemory);
    require(dependencies.guestScanoutChangeCount == 3);
    dependencies.guestScanoutChangeAccepted = true;
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.guestScanoutChangeCount == 4);
    require(dependencies.lastGuestScanoutWidth == 2);
    require(dependencies.lastGuestScanoutHeight == 4);

    device.reset();
    require(dependencies.guestSessionResetCount == 1);
}

void venusBlobsAndFencesUseStandardWireState()
{
    FakeGpuDependencies dependencies;
    const auto reader = [](std::uint64_t, std::span<std::uint8_t>) {
        return false;
    };
    const auto mapper = [](std::uint64_t, std::size_t) {
        return std::span<std::uint8_t> {};
    };
    VirtioGpuDevice device {dependencies, reader, mapper};
    require(device.config().capsetCount == protocol::venusCapsetIndex + 1U);
    protocol::GpuGetCapsetInfo capsetInfo {};
    capsetInfo.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::getCapsetInfo);
    capsetInfo.capsetIndex = protocol::venusCapsetIndex;
    const auto enabledInfo = device.process(requestBytes(capsetInfo), false);
    protocol::GpuCapsetInfo venusInfo {};
    std::memcpy(&venusInfo, enabledInfo.data(), sizeof(venusInfo));
    require(venusInfo.capsetId == protocol::venusCapsetId);
    require(venusInfo.maximumVersion == 0);

    auto context = createContext(0x401, protocol::venusCapsetId);
    require(responseType(device.process(requestBytes(context), false)) ==
        protocol::GpuResponse::okNoData);

    auto invalidContextFlags = createContext(0x402, protocol::venusCapsetId);
    invalidContextFlags.contextInit |= 1U << 8U;
    require(responseType(
        device.process(requestBytes(invalidContextFlags), false)) ==
        protocol::GpuResponse::errorInvalidParameter);

    protocol::GpuResourceCreateBlob create {};
    create.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceCreateBlob);
    create.header.contextId = context.header.contextId;
    create.resourceId = 0x501;
    create.memory = protocol::blobMemoryHost3d;
    create.flags =
        protocol::blobFlagMappable | protocol::blobFlagShareable;
    create.blobId = 0;
    create.size = 4096;

    auto apertureCreate = create;
    apertureCreate.resourceId = 0x500;
    apertureCreate.size = protocol::hostVisibleSharedMemoryRegionSize;
    require(responseType(device.process(requestBytes(apertureCreate), false)) ==
        protocol::GpuResponse::okNoData);
    /* Unmapped renderer storage does not consume aperture address space. */
    require(responseType(device.process(requestBytes(create), false)) ==
        protocol::GpuResponse::okNoData);
    protocol::GpuResourceReference apertureUnref {};
    apertureUnref.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::resourceUnref);
    apertureUnref.resourceId = apertureCreate.resourceId;
    require(responseType(device.process(requestBytes(apertureUnref), false)) ==
        protocol::GpuResponse::okNoData);

    auto crossDeviceCreate = create;
    crossDeviceCreate.resourceId = 0x502;
    crossDeviceCreate.flags |= protocol::blobFlagCrossDevice;
    require(responseType(
        device.process(requestBytes(crossDeviceCreate), false)) ==
        protocol::GpuResponse::okNoData);
    protocol::GpuResourceReference crossDeviceUnref {};
    crossDeviceUnref.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceUnref);
    crossDeviceUnref.resourceId = crossDeviceCreate.resourceId;
    require(responseType(
        device.process(requestBytes(crossDeviceUnref), false)) ==
        protocol::GpuResponse::okNoData);

    auto invalidCreate = create;
    invalidCreate.resourceId = 0x502;
    invalidCreate.memory = protocol::blobMemoryHost3dGuest;
    require(responseType(device.process(requestBytes(invalidCreate), false)) ==
        protocol::GpuResponse::errorInvalidParameter);
    auto rendererBlobCreate = create;
    rendererBlobCreate.resourceId = 0x5ff;
    rendererBlobCreate.blobId = 0x12345678;
    require(responseType(
        device.process(requestBytes(rendererBlobCreate), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.lastBlobId == rendererBlobCreate.blobId);
    protocol::GpuResourceReference rendererBlobUnref {};
    rendererBlobUnref.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceUnref);
    rendererBlobUnref.resourceId = rendererBlobCreate.resourceId;
    require(responseType(
        device.process(requestBytes(rendererBlobUnref), false)) ==
        protocol::GpuResponse::okNoData);

    /*
     * Virgl contexts own host-visible storage too: coherent GL buffers reach
     * the guest through the same aperture as Venus device memory.
     */
    auto virglContext = createContext(0x403, protocol::virgl2CapsetId);
    require(responseType(device.process(requestBytes(virglContext), false)) ==
        protocol::GpuResponse::okNoData);
    auto virglBlobCreate = create;
    virglBlobCreate.header.contextId = virglContext.header.contextId;
    virglBlobCreate.resourceId = 0x5fe;
    require(responseType(
        device.process(requestBytes(virglBlobCreate), false)) ==
        protocol::GpuResponse::okNoData);
    auto virglBlobMap = protocol::GpuResourceMapBlob {};
    virglBlobMap.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceMapBlob);
    virglBlobMap.resourceId = virglBlobCreate.resourceId;
    virglBlobMap.offset = 0;
    require(responseType(device.process(requestBytes(virglBlobMap), false)) ==
        protocol::GpuResponse::okMapInfo);
    auto virglBlobAction = device.takeSharedMemoryAction();
    require(virglBlobAction.has_value());
    device.completeSharedMemoryAction(virglBlobAction->actionId, true);
    auto virglBlobUnmap = protocol::GpuResourceUnmapBlob {};
    virglBlobUnmap.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceUnmapBlob);
    virglBlobUnmap.resourceId = virglBlobCreate.resourceId;
    require(responseType(
        device.process(requestBytes(virglBlobUnmap), false)) ==
        protocol::GpuResponse::okNoData);
    auto virglBlobUnmapAction = device.takeSharedMemoryAction();
    require(virglBlobUnmapAction.has_value());
    device.completeSharedMemoryAction(virglBlobUnmapAction->actionId, true);
    protocol::GpuResourceReference virglBlobUnref {};
    virglBlobUnref.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceUnref);
    virglBlobUnref.resourceId = virglBlobCreate.resourceId;
    require(responseType(
        device.process(requestBytes(virglBlobUnref), false)) ==
        protocol::GpuResponse::okNoData);

    protocol::GpuResourceMapBlob map {};
    map.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceMapBlob);
    map.resourceId = create.resourceId;
    map.offset = protocol::hostVisibleSharedMemoryAlignment;
    const auto mapResponse = device.process(requestBytes(map), false);
    require(responseType(mapResponse) == protocol::GpuResponse::okMapInfo);
    protocol::GpuMapInfo mapInfoResponse {};
    std::memcpy(
        &mapInfoResponse, mapResponse.data(), sizeof(mapInfoResponse));
    require(mapInfoResponse.mapInfo == protocol::mapCacheCached);
    auto mapAction = device.takeSharedMemoryAction();
    require(mapAction.has_value());
    require(mapAction->type ==
        VirtioGpuDevice::SharedMemoryAction::Type::map);
    require(mapAction->descriptor.get() >= 0);
    require(mapAction->regionId ==
        protocol::hostVisibleSharedMemoryRegionId);
    require(mapAction->regionOffset == map.offset);
    require(mapAction->length ==
        protocol::hostVisibleSharedMemoryAlignment);

    protocol::GpuResourceUnmapBlob unmap {};
    unmap.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceUnmapBlob);
    unmap.resourceId = create.resourceId;
    require(responseType(device.process(requestBytes(unmap), false)) ==
        protocol::GpuResponse::errorInvalidParameter);
    device.completeSharedMemoryAction(mapAction->actionId, true);

    auto secondCreate = create;
    secondCreate.resourceId = 0x502;
    require(responseType(device.process(requestBytes(secondCreate), false)) ==
        protocol::GpuResponse::okNoData);
    auto secondMap = map;
    secondMap.resourceId = secondCreate.resourceId;
    require(responseType(device.process(requestBytes(secondMap), false)) ==
        protocol::GpuResponse::errorInvalidParameter);
    secondMap.offset += protocol::hostVisibleSharedMemoryAlignment;
    require(responseType(device.process(requestBytes(secondMap), false)) ==
        protocol::GpuResponse::okMapInfo);
    auto secondMapAction = device.takeSharedMemoryAction();
    require(secondMapAction.has_value());
    device.completeSharedMemoryAction(secondMapAction->actionId, false);
    require(responseType(device.process(requestBytes(secondMap), false)) ==
        protocol::GpuResponse::okMapInfo);
    secondMapAction = device.takeSharedMemoryAction();
    require(secondMapAction.has_value());
    device.completeSharedMemoryAction(secondMapAction->actionId, true);

    auto secondUnmap = unmap;
    secondUnmap.resourceId = secondCreate.resourceId;
    require(responseType(device.process(requestBytes(secondUnmap), false)) ==
        protocol::GpuResponse::okNoData);
    auto secondUnmapAction = device.takeSharedMemoryAction();
    require(secondUnmapAction.has_value());
    device.completeSharedMemoryAction(secondUnmapAction->actionId, true);

    require(responseType(device.process(requestBytes(unmap), false)) ==
        protocol::GpuResponse::okNoData);
    auto unmapAction = device.takeSharedMemoryAction();
    require(unmapAction.has_value());
    require(unmapAction->type ==
        VirtioGpuDevice::SharedMemoryAction::Type::unmap);
    require(unmapAction->descriptor.get() < 0);
    device.completeSharedMemoryAction(unmapAction->actionId, false);
    require(responseType(device.process(requestBytes(unmap), false)) ==
        protocol::GpuResponse::okNoData);
    unmapAction = device.takeSharedMemoryAction();
    require(unmapAction.has_value());
    device.completeSharedMemoryAction(unmapAction->actionId, true);

    protocol::GpuSubmit3d submit {};
    submit.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::submit3d);
    submit.header.contextId = context.header.contextId;
    submit.header.flags =
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    submit.header.ringIndex = 7;
    submit.header.fenceId = 0x1234;
    constexpr std::uint64_t venusFenceCookie = 0x5aa5;
    const auto deferred = device.processWithDeferredFence(
        requestBytes(submit), false, venusFenceCookie);
    require(responseType(deferred.response) ==
        protocol::GpuResponse::okNoData);
    require(deferred.fence.has_value());
    require(deferred.fence->contextId == context.header.contextId);
    require(deferred.fence->ringIndex == submit.header.ringIndex);
    require(deferred.fence->cookie == venusFenceCookie);
    require(device.contextFenceStatus(*deferred.fence) == EBUSY);
    dependencies.retireContextFence(
        context.header.contextId, submit.header.ringIndex, venusFenceCookie);
    require(device.pollContextFences(deferred.fence->contextId) == 0);
    require(device.contextFenceStatus(*deferred.fence) == 0);
    /* Renderer work with no fence of its own behind it, such as a query a
     * guest is blocked on, is reported and completed by a renderer-wide
     * poll rather than by polling a context. */
    require(!device.rendererHasPendingWork());
    dependencies.rendererPendingWork = true;
    require(device.rendererHasPendingWork());
    require(device.pollRenderer() == 0);
    require(dependencies.rendererPollCount == 1);
    dependencies.rendererPendingWork = false;

    auto invalidRingSubmit = submit;
    invalidRingSubmit.header.ringIndex =
        protocol::venusContextRingCount;
    require(responseType(
        device.process(requestBytes(invalidRingSubmit), false)) ==
        protocol::GpuResponse::errorInvalidParameter);

    protocol::GpuResourceReference unref {};
    unref.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::resourceUnref);
    unref.resourceId = secondCreate.resourceId;
    require(responseType(device.process(requestBytes(unref), false)) ==
        protocol::GpuResponse::okNoData);

    require(responseType(device.process(requestBytes(map), false)) ==
        protocol::GpuResponse::okMapInfo);
    mapAction = device.takeSharedMemoryAction();
    require(mapAction.has_value());
    device.completeSharedMemoryAction(mapAction->actionId, true);
    unref.resourceId = create.resourceId;
    require(responseType(device.process(requestBytes(unref), false)) ==
        protocol::GpuResponse::okNoData);
    unmapAction = device.takeSharedMemoryAction();
    require(unmapAction.has_value());
    require(unmapAction->type ==
        VirtioGpuDevice::SharedMemoryAction::Type::unmap);
    device.completeSharedMemoryAction(unmapAction->actionId, true);

    auto resetCreate = create;
    resetCreate.resourceId = 0x503;
    require(responseType(device.process(requestBytes(resetCreate), false)) ==
        protocol::GpuResponse::okNoData);
    auto resetMap = map;
    resetMap.resourceId = resetCreate.resourceId;
    require(responseType(device.process(requestBytes(resetMap), false)) ==
        protocol::GpuResponse::okMapInfo);
    auto resetMapAction = device.takeSharedMemoryAction();
    require(resetMapAction.has_value());
    device.completeSharedMemoryAction(resetMapAction->actionId, true);
    device.prepareReset();
    auto resetUnmapAction = device.takeSharedMemoryAction();
    require(resetUnmapAction.has_value());
    require(resetUnmapAction->type ==
        VirtioGpuDevice::SharedMemoryAction::Type::unmap);
    device.completeSharedMemoryAction(resetUnmapAction->actionId, true);
    device.reset();
}

void blobMappingCapacityFollowsTheAperture()
{
    FakeGpuDependencies dependencies;
    const auto reader = [](std::uint64_t, std::span<std::uint8_t>) {
        return false;
    };
    const auto mapper = [](std::uint64_t, std::size_t) {
        return std::span<std::uint8_t> {};
    };
    VirtioGpuDevice device {dependencies, reader, mapper};
    const auto context = createContext(0x601, protocol::venusCapsetId);
    require(responseType(device.process(requestBytes(context), false)) ==
        protocol::GpuResponse::okNoData);

    protocol::GpuResourceCreateBlob create {};
    create.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceCreateBlob);
    create.header.contextId = context.header.contextId;
    create.memory = protocol::blobMemoryHost3d;
    create.flags = protocol::blobFlagMappable;
    create.size = protocol::hostVisibleSharedMemoryAlignment;
    protocol::GpuResourceMapBlob map {};
    map.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceMapBlob);

    // Small mappings must not exhaust a record-count limit before the aperture.
    constexpr std::uint32_t smallMappings = 1025;
    for (std::uint32_t index = 0; index < smallMappings; ++index) {
        create.resourceId = index + 1U;
        require(responseType(device.process(requestBytes(create), false)) ==
            protocol::GpuResponse::okNoData);
        map.resourceId = create.resourceId;
        map.offset = index * protocol::hostVisibleSharedMemoryAlignment;
        require(responseType(device.process(requestBytes(map), false)) ==
            protocol::GpuResponse::okMapInfo);
        auto action = device.takeSharedMemoryAction();
        require(action.has_value());
        require(action->regionOffset == map.offset);
        require(action->length == create.size);
        device.completeSharedMemoryAction(action->actionId, true);
    }

    // Fill the rest with one range, then preserve overlap and bounds checks.
    create.resourceId = smallMappings + 1U;
    map.resourceId = create.resourceId;
    map.offset = smallMappings * protocol::hostVisibleSharedMemoryAlignment;
    create.size = protocol::hostVisibleSharedMemoryRegionSize - map.offset;
    require(responseType(device.process(requestBytes(create), false)) ==
        protocol::GpuResponse::okNoData);
    require(responseType(device.process(requestBytes(map), false)) ==
        protocol::GpuResponse::okMapInfo);
    auto action = device.takeSharedMemoryAction();
    require(action.has_value());
    device.completeSharedMemoryAction(action->actionId, true);

    ++create.resourceId;
    create.size = protocol::hostVisibleSharedMemoryAlignment;
    require(responseType(device.process(requestBytes(create), false)) ==
        protocol::GpuResponse::okNoData);
    map.resourceId = create.resourceId;
    map.offset = 0;
    require(responseType(device.process(requestBytes(map), false)) ==
        protocol::GpuResponse::errorInvalidParameter);
    map.offset = protocol::hostVisibleSharedMemoryRegionSize;
    require(responseType(device.process(requestBytes(map), false)) ==
        protocol::GpuResponse::errorInvalidParameter);

    protocol::GpuResourceUnmapBlob unmap {};
    unmap.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceUnmapBlob);
    unmap.resourceId = 1;
    require(responseType(device.process(requestBytes(unmap), false)) ==
        protocol::GpuResponse::okNoData);
    action = device.takeSharedMemoryAction();
    require(action.has_value());
    device.completeSharedMemoryAction(action->actionId, true);
    map.offset = 0;
    require(responseType(device.process(requestBytes(map), false)) ==
        protocol::GpuResponse::okMapInfo);
    action = device.takeSharedMemoryAction();
    require(action.has_value());
    device.completeSharedMemoryAction(action->actionId, true);
    device.prepareReset();
    std::uint32_t unmapped = 0;
    while (auto pending = device.takeSharedMemoryAction()) {
        require(pending->type == VirtioGpuDevice::SharedMemoryAction::Type::unmap);
        device.completeSharedMemoryAction(pending->actionId, true);
        ++unmapped;
    }
    require(unmapped == smallMappings + 1U);
    device.reset();
    require(dependencies.resources.empty());
}

void blobScanoutUsesNativeTextureAndPersistentVncMapping()
{
    FakeGpuDependencies dependencies;
    const auto reader = [](std::uint64_t, std::span<std::uint8_t>) {
        return false;
    };
    const auto mapper = [](std::uint64_t, std::size_t) {
        return std::span<std::uint8_t> {};
    };
    VirtioGpuDevice empty {dependencies, reader, mapper};
    protocol::GpuSetScanoutBlob scanout {};
    scanout.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::setScanoutBlob);
    scanout.rectangle = {.width = 8, .height = 8};
    scanout.scanoutId = protocol::primaryScanoutId;
    scanout.resourceId = 0x702;
    scanout.width = 8;
    scanout.height = 8;
    scanout.format = static_cast<std::uint32_t>(
        protocol::GpuFormat::b8g8r8a8Unorm);
    scanout.strides[0] = 8U * protocol::gpuBytesPerPixel;
    /* A blob scanout must name a blob the device already holds. */
    require(responseType(empty.process(requestBytes(scanout), false)) !=
        protocol::GpuResponse::okNoData);

    VirtioGpuDevice device {dependencies, reader, mapper};
    const auto context = createContext(0x701, protocol::venusCapsetId);
    require(responseType(device.process(requestBytes(context), false)) ==
        protocol::GpuResponse::okNoData);
    protocol::GpuResourceCreateBlob create {};
    create.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceCreateBlob);
    create.header.contextId = context.header.contextId;
    create.resourceId = scanout.resourceId;
    create.memory = protocol::blobMemoryHost3d;
    create.flags =
        protocol::blobFlagShareable | protocol::blobFlagCrossDevice;
    create.blobId = 0x1234;
    create.size = dependencies.blobPixels.size();
    require(responseType(device.process(requestBytes(create), false)) ==
        protocol::GpuResponse::okNoData);

    auto invalid = scanout;
    invalid.strides[0]--;
    require(responseType(device.process(requestBytes(invalid), false)) ==
        protocol::GpuResponse::errorInvalidParameter);
    invalid = scanout;
    invalid.strides[1] = 1;
    require(responseType(device.process(requestBytes(invalid), false)) ==
        protocol::GpuResponse::errorInvalidParameter);
    invalid = scanout;
    invalid.offsets[0] = static_cast<std::uint32_t>(create.size - 1U);
    require(responseType(device.process(requestBytes(invalid), false)) ==
        protocol::GpuResponse::errorInvalidParameter);
    require(dependencies.createBlobScanoutCount == 0);

    requireFirstAllocationFailsBeforeCommit(device, scanout);
    require(dependencies.createBlobScanoutCount == 0);
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.createBlobScanoutCount == 1);
    require(dependencies.lastTextureResource == scanout.resourceId);
    require(dependencies.lastBlobScanoutWidth == scanout.width);
    require(dependencies.lastBlobScanoutHeight == scanout.height);
    require(dependencies.lastBlobScanoutFormat == scanout.format);
    require(dependencies.lastBlobScanoutStride == scanout.strides[0]);
    require(dependencies.lastBlobScanoutOffset == scanout.offsets[0]);

    protocol::GpuResourceFlush flush {};
    flush.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceFlush);
    flush.rectangle = scanout.rectangle;
    flush.resourceId = scanout.resourceId;
    dependencies.directPresentationEnabled = true;
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.directPresentationCount == 1);
    require(dependencies.lastMetalScanout.texture ==
        dependencies.blobScanoutTexture);
    require(dependencies.lastMetalScanout.damageX == flush.rectangle.x);
    require(dependencies.lastMetalScanout.damageY == flush.rectangle.y);
    require(dependencies.lastMetalScanout.damageWidth == flush.rectangle.width);
    require(
        dependencies.lastMetalScanout.damageHeight == flush.rectangle.height);
    require(dependencies.mapResourceCount == 0);
    require(dependencies.displayUpdateCount == 0);

    dependencies.directPresentationEnabled = false;
    std::fill(
        dependencies.blobPixels.begin(), dependencies.blobPixels.end(), 0x6b);
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.mapResourceCount == 1);
    require(dependencies.displayUpdateCount == 1);
    require(dependencies.framebuffer[0] == 0x6b);
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.mapResourceCount == 1);
    require(dependencies.displayUpdateCount == 2);

    dependencies.rejectNext(FakeGpuDependencies::Operation::blob_scanout);
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::errorInvalidParameter);
    require(dependencies.releaseBlobScanoutCount == 0);
    require(dependencies.unmapResourceCount == 0);

    auto disable = scanout;
    disable.resourceId = protocol::invalidResourceId;
    require(responseType(device.process(requestBytes(disable), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.releaseBlobScanoutCount == 1);
    require(dependencies.lastReleasedBlobScanout ==
        dependencies.blobScanoutTexture);
    require(dependencies.unmapResourceCount == 1);
    require(dependencies.lastUnmappedResource == scanout.resourceId);
}

void virglBlobScanoutPresentsThroughReadback()
{
    FakeGpuDependencies dependencies;
    const auto reader = [](std::uint64_t, std::span<std::uint8_t>) {
        return false;
    };
    const auto mapper = [](std::uint64_t, std::size_t) {
        return std::span<std::uint8_t> {};
    };
    VirtioGpuDevice device {dependencies, reader, mapper};
    const auto context = createContext(0x801, protocol::virgl2CapsetId);
    require(responseType(device.process(requestBytes(context), false)) ==
        protocol::GpuResponse::okNoData);
    protocol::GpuResourceCreateBlob create {};
    create.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceCreateBlob);
    create.header.contextId = context.header.contextId;
    create.resourceId = 0x802;
    create.memory = protocol::blobMemoryHost3d;
    create.flags = protocol::blobFlagShareable;
    create.blobId = 0x2345;
    create.size = dependencies.blobPixels.size();
    require(responseType(device.process(requestBytes(create), false)) ==
        protocol::GpuResponse::okNoData);

    protocol::GpuSetScanoutBlob scanout {};
    scanout.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::setScanoutBlob);
    scanout.rectangle = {.width = 8, .height = 8};
    scanout.scanoutId = protocol::primaryScanoutId;
    scanout.resourceId = create.resourceId;
    scanout.width = 8;
    scanout.height = 8;
    scanout.format = static_cast<std::uint32_t>(
        protocol::GpuFormat::r8g8b8x8Unorm);
    scanout.strides[0] = 8U * protocol::gpuBytesPerPixel;

    /* A Virgl context's blob is a GL texture with no Metal texture, the
     * framebuffer Android's gralloc allocates once blobs exist. */
    dependencies.rejectNext(FakeGpuDependencies::Operation::blob_scanout);
    requireFirstAllocationFailsBeforeCommit(device, scanout);
    require(dependencies.createBlobScanoutCount == 0);
    require(dependencies.guestScanoutChangeCount == 0);
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.createBlobScanoutCount == 1);
    require(dependencies.guestScanoutChangeCount == 1);
    require(dependencies.lastGuestScanoutWidth == scanout.rectangle.width);
    require(dependencies.lastGuestScanoutHeight == scanout.rectangle.height);

    protocol::GpuResourceFlush flush {};
    flush.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceFlush);
    flush.rectangle = {.x = 2, .y = 3, .width = 4, .height = 2};
    flush.resourceId = scanout.resourceId;
    dependencies.directPresentationEnabled = true;
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.directPresentationCount == 0);
    require(dependencies.mapResourceCount == 0);
    require(dependencies.cpuFallbackFrameCount == 1);
    require(dependencies.readResourceCount == 1);
    require(dependencies.lastReadResource == scanout.resourceId);
    require(dependencies.lastReadX == flush.rectangle.x);
    require(dependencies.lastReadY == flush.rectangle.y);
    require(dependencies.lastReadWidth == flush.rectangle.width);
    require(dependencies.lastReadHeight == flush.rectangle.height);
    require(dependencies.displayUpdateCount == 1);
    require(dependencies.lastDisplayDestinationX == flush.rectangle.x);
    require(dependencies.lastDisplayDestinationY == flush.rectangle.y);
    const auto updated =
        (static_cast<std::size_t>(flush.rectangle.y) *
                dependencies.displayWidth +
            flush.rectangle.x) *
        protocol::gpuBytesPerPixel;
    require(dependencies.framebuffer[updated + 2U] ==
        dependencies.readResourceTag);

    /* A client that needs a whole frame gets the whole scanout. */
    dependencies.fullCpuFallback = true;
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.readResourceCount == 2);
    require(dependencies.lastReadX == 0);
    require(dependencies.lastReadY == 0);
    require(dependencies.lastReadWidth == scanout.rectangle.width);
    require(dependencies.lastReadHeight == scanout.rectangle.height);

    auto disable = scanout;
    disable.resourceId = protocol::invalidResourceId;
    require(responseType(device.process(requestBytes(disable), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.releaseBlobScanoutCount == 0);
    require(dependencies.unmapResourceCount == 0);
    require(responseType(device.process(requestBytes(flush), false)) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.readResourceCount == 2);
}

void malformed3dCommandsUseSpecificErrors()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [](std::uint64_t, std::span<std::uint8_t>) { return false; },
        [](std::uint64_t, std::size_t) {
            return std::span<std::uint8_t> {};
        }};
    protocol::GpuControlHeader transfer {};
    transfer.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::transferToHost3d);
    require(responseType(device.process(requestBytes(transfer), false)) ==
        protocol::GpuResponse::errorInvalidParameter);

    protocol::GpuSubmit3d submit {};
    submit.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::submit3d);
    submit.header.contextId = 99;
    require(responseType(device.process(requestBytes(submit), false)) ==
        protocol::GpuResponse::errorInvalidContext);

    const auto dependencyCallCount = [&] {
        return dependencies.createContextCount +
            dependencies.destroyContextCount + dependencies.submitCount +
            dependencies.createResourceCount + dependencies.transferCount +
            dependencies.readResourceCount + dependencies.fenceCount +
            dependencies.unrefResourceCount +
            dependencies.attachBackingCount +
            dependencies.detachBackingCount +
            dependencies.attachResourceCount +
            dependencies.detachResourceCount +
            dependencies.displayUpdateCount;
    };
    const auto before = dependencyCallCount();
    for (const auto command : {
             protocol::GpuCommand::resourceCreate2d,
            protocol::GpuCommand::resourceUnref,
            protocol::GpuCommand::setScanout,
            protocol::GpuCommand::setScanoutBlob,
             protocol::GpuCommand::resourceFlush,
             protocol::GpuCommand::transferToHost2d,
             protocol::GpuCommand::resourceAttachBacking,
             protocol::GpuCommand::resourceDetachBacking,
             protocol::GpuCommand::contextCreate,
             protocol::GpuCommand::contextAttachResource,
             protocol::GpuCommand::contextDetachResource,
             protocol::GpuCommand::resourceCreate3d,
             protocol::GpuCommand::transferToHost3d,
             protocol::GpuCommand::transferFromHost3d,
             protocol::GpuCommand::submit3d,
             protocol::GpuCommand::updateCursor,
             protocol::GpuCommand::moveCursor}) {
        protocol::GpuControlHeader truncated {};
        truncated.type = static_cast<std::uint32_t>(command);
        const auto result = device.process(requestBytes(truncated), false);
        require(responseType(result) != protocol::GpuResponse::okNoData);
    }
    require(dependencyCallCount() == before);
}

} // namespace

void runVirtioGpuDeviceTests()
{
    edidReportsDisplayPixelsAndEffectiveDensity();
    displayChangeEventsPreserveAResizeThatRacesGuestClear();
    contextInitUsesVirgl2ForTheDeviceSelectedDefault();
    lifecycleCommandsAllocateBeforeCommitAndRetry();
    rendererExecutionFailuresPreserveDeviceState();
    softwareScanoutProgressesWhileRendererFenceIsPending();
    deferredFencesUseHostTimelineAndMergedRetirement();
    resetRetiresEachExternalOwnerExactlyOnce();
    resetIsPristineAndAllocationRetryIsAtomic();
    rendererBackingAttachIsRetryableAfterAllocationFailure();
    backingRangesAndAttachStateAreValidated();
    failedTransferDoesNotPartiallyCommitPixels();
    setScanoutReplyFailureDoesNotCommitAllocation();
    venusBlobsAndFencesUseStandardWireState();
    blobMappingCapacityFollowsTheAperture();
    blobScanoutUsesNativeTextureAndPersistentVncMapping();
    virglBlobScanoutPresentsThroughReadback();
    malformed3dCommandsUseSpecificErrors();
}

} // namespace uml::gpu::testing
