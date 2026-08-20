// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"
#include "virtio_gpu_device.hpp"
#include "virtio_gpu_device_testing.hpp"

#include <algorithm>
#include <cerrno>
#include <new>
#include <stdexcept>
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

    void clear() noexcept override
    {
        std::fill(framebuffer.begin(), framebuffer.end(), 0);
        ++clearCount;
    }

    void update(
        std::span<const std::uint8_t> source,
        std::uint32_t,
        std::uint32_t,
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
        std::uint32_t) const override
    {
        return {.maximumVersion = 1, .data = {0, 0, 0, 0}};
    }

    [[nodiscard]] int createContext(
        std::uint32_t contextId,
        std::uint32_t,
        std::string_view) noexcept override
    {
        if (reject(Operation::create_context))
            return EIO;
        try {
            contexts.insert(contextId);
        } catch (...) {
            return ENOMEM;
        }
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
        std::uint32_t,
        std::span<std::uint8_t> destination,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t) override
    {
        if (reject(Operation::read_resource))
            return EIO;
        failIfRequested(Operation::read_resource);
        std::fill(destination.begin(), destination.end(), 0x5a);
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

    bool needsFullCpuFallback() const noexcept override { return false; }

    void recordCpuFallbackFrame() noexcept override
    {
        ++cpuFallbackFrameCount;
    }

    [[nodiscard]] int createFenceAndWait(
        std::uint32_t,
        std::uint32_t,
        std::uint64_t,
        const std::atomic_bool* stopRequested) noexcept override
    {
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

    void resetFenceState() noexcept override { ++resetFenceCount; }

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
    std::unordered_set<std::uint32_t> contexts;
    std::unordered_set<std::uint32_t> resources;
    void* attachedBackingAddress {};
    std::uint32_t attachedBackingResource {};
    unsigned clearCount {};
    unsigned displayUpdateCount {};
    unsigned createContextCount {};
    unsigned destroyContextCount {};
    unsigned submitCount {};
    unsigned createResourceCount {};
    unsigned transferCount {};
    unsigned readResourceCount {};
    unsigned nativeResourceRequestCount {};
    unsigned resourceMetalTextureCount {};
    unsigned directPresentationCount {};
    unsigned directPresentationDropCount {};
    unsigned cpuFallbackFrameCount {};
    bool directPresentationEnabled {};
    bool directPresentationDrops {};
    bool nativeResourceAvailable {};
    std::uint32_t displayWidth {64};
    std::uint32_t displayHeight {64};
    std::uint32_t lastTextureResource {};
    MetalScanout lastMetalScanout {};
    unsigned fenceCount {};
    const std::atomic_bool* lastFenceStopRequest {};
    unsigned unrefResourceCount {};
    unsigned attachBackingCount {};
    unsigned detachBackingCount {};
    unsigned attachResourceCount {};
    unsigned detachResourceCount {};
    unsigned resetFenceCount {};

private:
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
    std::uint32_t contextId)
{
    protocol::GpuContextCreate context {};
    context.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::contextCreate);
    context.header.contextId = contextId;
    context.contextInit = protocol::virglCapsetId;
    return context;
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

    auto resource = create3d(702, 2, 2);
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
    require(dependencies.lastFenceStopRequest == &stopRequested);

    protocol::GpuSetScanout scanout {};
    scanout.header.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::setScanout);
    scanout.rectangle = {.width = 2, .height = 2};
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

    failNextAllocation();
    const auto retry = device.process(requestBytes(scanout), false);
    clearAllocationFailure();
    require(responseType(retry) == protocol::GpuResponse::errorOutOfMemory);
    require(responseType(device.process(requestBytes(scanout), false)) ==
        protocol::GpuResponse::okNoData);
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
    lifecycleCommandsAllocateBeforeCommitAndRetry();
    rendererExecutionFailuresPreserveDeviceState();
    resetRetiresEachExternalOwnerExactlyOnce();
    resetIsPristineAndAllocationRetryIsAtomic();
    rendererBackingAttachIsRetryableAfterAllocationFailure();
    backingRangesAndAttachStateAreValidated();
    failedTransferDoesNotPartiallyCommitPixels();
    setScanoutReplyFailureDoesNotCommitAllocation();
    malformed3dCommandsUseSpecificErrors();
}

} // namespace uml::gpu::testing
