// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_gpu_device.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>

namespace uml::gpu {
namespace {

template<typename Structure>
const Structure* readStructure(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < sizeof(Structure))
        return nullptr;
    return reinterpret_cast<const Structure*>(bytes.data());
}

bool supportedFormat(std::uint32_t format)
{
    switch (static_cast<protocol::GpuFormat>(format)) {
    case protocol::GpuFormat::b8g8r8a8Unorm:
    case protocol::GpuFormat::b8g8r8x8Unorm:
    case protocol::GpuFormat::a8r8g8b8Unorm:
    case protocol::GpuFormat::x8r8g8b8Unorm:
    case protocol::GpuFormat::r8g8b8a8Unorm:
    case protocol::GpuFormat::x8b8g8r8Unorm:
    case protocol::GpuFormat::a8b8g8r8Unorm:
    case protocol::GpuFormat::r8g8b8x8Unorm:
        return true;
    default:
        return false;
    }
}

bool rectangleFits(
    const protocol::GpuRectangle& rectangle,
    std::uint32_t width,
    std::uint32_t height)
{
    return rectangle.width != 0 && rectangle.height != 0 &&
        rectangle.x <= width && rectangle.y <= height &&
        rectangle.width <= width - rectangle.x &&
        rectangle.height <= height - rectangle.y;
}

std::optional<protocol::GpuRectangle> intersectRectangles(
    const protocol::GpuRectangle& first,
    const protocol::GpuRectangle& second)
{
    const auto left = std::max(first.x, second.x);
    const auto top = std::max(first.y, second.y);
    const auto right = std::min(
        static_cast<std::uint64_t>(first.x) + first.width,
        static_cast<std::uint64_t>(second.x) + second.width);
    const auto bottom = std::min(
        static_cast<std::uint64_t>(first.y) + first.height,
        static_cast<std::uint64_t>(second.y) + second.height);
    if (right <= left || bottom <= top)
        return std::nullopt;
    return protocol::GpuRectangle {
        .x = left,
        .y = top,
        .width = static_cast<std::uint32_t>(right - left),
        .height = static_cast<std::uint32_t>(bottom - top),
    };
}

} // namespace

VirtioGpuDevice::VirtioGpuDevice(
    VncDisplay& display,
    GuestReader guestReader,
    GuestMapper guestMapper)
    : display_(display)
    , guestReader_(std::move(guestReader))
    , guestMapper_(std::move(guestMapper))
{
}

protocol::GpuConfig VirtioGpuDevice::config() const noexcept
{
    return {
        .eventsRead = 0,
        .eventsClear = 0,
        .scanoutCount = protocol::primaryScanoutId + 1U,
        .capsetCount = protocol::virgl2CapsetIndex + 1U,
    };
}

std::vector<std::uint8_t> VirtioGpuDevice::response(
    const protocol::GpuControlHeader& request,
    protocol::GpuResponse type) const
{
    protocol::GpuControlHeader header = request;
    header.type = static_cast<std::uint32_t>(type);
    header.flags &=
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    std::vector<std::uint8_t> bytes(sizeof(header));
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

std::vector<std::uint8_t> VirtioGpuDevice::displayInfo(
    const protocol::GpuControlHeader& request) const
{
    protocol::GpuDisplayInfo information {};
    information.header = request;
    information.header.type =
        static_cast<std::uint32_t>(protocol::GpuResponse::okDisplayInfo);
    information.header.flags &=
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    information.modes[0].rectangle = {
        .x = 0,
        .y = 0,
        .width = display_.width(),
        .height = display_.height(),
    };
    information.modes[protocol::primaryScanoutId].enabled =
        protocol::enabledDisplayMode;
    std::vector<std::uint8_t> bytes(sizeof(information));
    std::memcpy(bytes.data(), &information, sizeof(information));
    return bytes;
}

bool VirtioGpuDevice::readBacking(
    const Resource& resource,
    std::uint64_t offset,
    std::span<std::uint8_t> destination) const
{
    std::uint64_t logicalStart = 0;
    std::size_t copied = 0;
    for (const auto& entry : resource.backing) {
        const std::uint64_t logicalEnd = logicalStart + entry.length;
        if (logicalEnd < logicalStart)
            return false;
        if (offset < logicalEnd && copied < destination.size()) {
            const std::uint64_t entryOffset =
                offset > logicalStart ? offset - logicalStart : 0;
            const auto available = static_cast<std::size_t>(
                entry.length - entryOffset);
            const auto count =
                std::min(available, destination.size() - copied);
            if (!guestReader_(
                    entry.address + entryOffset,
                    destination.subspan(copied, count))) {
                return false;
            }
            copied += count;
            offset += count;
        }
        logicalStart = logicalEnd;
    }
    return copied == destination.size();
}

bool VirtioGpuDevice::parseBacking(
    std::span<const std::uint8_t> request,
    std::size_t entriesOffset,
    std::uint32_t entryCount,
    std::vector<BackingEntry>& backing,
    std::vector<iovec>& vectors) const
{
    if (entryCount > protocol::maximumResourceBackingEntries)
        return false;
    const auto entriesSize =
        static_cast<std::size_t>(entryCount) *
        sizeof(protocol::GpuMemoryEntry);
    if (entriesOffset > request.size() ||
        entriesSize > request.size() - entriesOffset) {
        return false;
    }

    backing.clear();
    vectors.clear();
    backing.reserve(entryCount);
    vectors.reserve(entryCount);
    for (std::uint32_t index = 0; index < entryCount; ++index) {
        const auto* entry = readStructure<protocol::GpuMemoryEntry>(
            request.subspan(
                entriesOffset +
                static_cast<std::size_t>(index) *
                    sizeof(protocol::GpuMemoryEntry)));
        if (entry == nullptr || entry->length == 0)
            return false;
        auto mapping = guestMapper_(entry->address, entry->length);
        if (mapping.size() != entry->length)
            return false;
        backing.push_back({
            .address = entry->address,
            .length = entry->length,
        });
        vectors.push_back({
            .iov_base = mapping.data(),
            .iov_len = mapping.size(),
        });
    }
    return true;
}

std::vector<std::uint8_t> VirtioGpuDevice::capsetInfo(
    const protocol::GpuGetCapsetInfo& request) const
{
    protocol::GpuCapsetInfo information {};
    information.header = request.header;
    information.header.type =
        static_cast<std::uint32_t>(protocol::GpuResponse::okCapsetInfo);
    information.header.flags &=
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    if (request.capsetIndex == protocol::virglCapsetIndex ||
        request.capsetIndex == protocol::virgl2CapsetIndex) {
        information.capsetId =
            request.capsetIndex == protocol::virglCapsetIndex
            ? protocol::virglCapsetId
            : protocol::virgl2CapsetId;
        const auto virglCapset = renderer_.capset(information.capsetId);
        information.maximumVersion = virglCapset.maximumVersion;
        information.maximumSize =
            static_cast<std::uint32_t>(virglCapset.data.size());
    }
    std::vector<std::uint8_t> bytes(sizeof(information));
    std::memcpy(bytes.data(), &information, sizeof(information));
    return bytes;
}

std::vector<std::uint8_t> VirtioGpuDevice::capset(
    const protocol::GpuGetCapset& request) const
{
    if (request.capsetId != protocol::virglCapsetId &&
        request.capsetId != protocol::virgl2CapsetId) {
        return response(
            request.header, protocol::GpuResponse::errorInvalidParameter);
    }
    const auto virglCapset = renderer_.capset(request.capsetId);
    if (request.version > virglCapset.maximumVersion)
        return response(
            request.header, protocol::GpuResponse::errorInvalidParameter);

    auto header = request.header;
    header.type = static_cast<std::uint32_t>(protocol::GpuResponse::okCapset);
    header.flags &=
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    std::vector<std::uint8_t> bytes(
        sizeof(header) + virglCapset.data.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(
        bytes.data() + sizeof(header),
        virglCapset.data.data(),
        virglCapset.data.size());
    return bytes;
}

std::vector<std::uint8_t> VirtioGpuDevice::process(
    std::span<const std::uint8_t> request,
    bool cursorQueue)
{
    const auto* header = readStructure<protocol::GpuControlHeader>(request);
    if (header == nullptr) {
        protocol::GpuControlHeader empty {};
        return response(empty, protocol::GpuResponse::errorInvalidParameter);
    }
    const auto command = static_cast<protocol::GpuCommand>(header->type);

    if (cursorQueue) {
        if (command != protocol::GpuCommand::updateCursor &&
            command != protocol::GpuCommand::moveCursor) {
            return {};
        }
        const auto* cursor =
            readStructure<protocol::GpuUpdateCursor>(request);
        if (cursor == nullptr ||
            cursor->position.scanoutId != protocol::primaryScanoutId)
            return {};
        /*
         * The cursor virtqueue has only a device-readable command
         * descriptor. Unlike the control queue, cursor commands do not carry
         * a writable response buffer; completion is reported solely through
         * the used ring.
         */
        return {};
    }

    switch (command) {
    case protocol::GpuCommand::getDisplayInfo:
        return displayInfo(*header);

    case protocol::GpuCommand::getCapsetInfo: {
        const auto* capsetInfoRequest =
            readStructure<protocol::GpuGetCapsetInfo>(request);
        if (capsetInfoRequest == nullptr)
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        return capsetInfo(*capsetInfoRequest);
    }

    case protocol::GpuCommand::getCapset: {
        const auto* capsetRequest =
            readStructure<protocol::GpuGetCapset>(request);
        if (capsetRequest == nullptr)
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        return capset(*capsetRequest);
    }

    case protocol::GpuCommand::contextCreate: {
        const auto* create =
            readStructure<protocol::GpuContextCreate>(request);
        if (create == nullptr ||
            create->header.contextId == protocol::invalidContextId ||
            create->nameLength >
                protocol::maximumContextDebugNameLength ||
            ((create->contextInit & protocol::contextInitCapsetMask) !=
                 protocol::virglCapsetId &&
             (create->contextInit & protocol::contextInitCapsetMask) !=
                 protocol::virgl2CapsetId) ||
            (create->contextInit & ~protocol::contextInitCapsetMask) != 0 ||
            contexts_.contains(create->header.contextId)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        const std::string_view debugName(
            create->debugName.data(), create->nameLength);
        if (renderer_.createContext(
                create->header.contextId,
                create->contextInit,
                debugName) != 0) {
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        }
        contexts_.insert(create->header.contextId);
        return response(*header, protocol::GpuResponse::okNoData);
    }

    case protocol::GpuCommand::contextDestroy:
        if (header->contextId == protocol::invalidContextId ||
            contexts_.erase(header->contextId) == 0) {
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        }
        renderer_.destroyContext(header->contextId);
        return response(*header, protocol::GpuResponse::okNoData);

    case protocol::GpuCommand::contextAttachResource:
    case protocol::GpuCommand::contextDetachResource: {
        const auto* contextResource =
            readStructure<protocol::GpuContextResource>(request);
        if (contextResource == nullptr) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        if (command == protocol::GpuCommand::contextAttachResource) {
            if (!contexts_.contains(header->contextId)) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidContext);
            }
            if (!virglResources_.contains(contextResource->resourceId) &&
                !resources_.contains(contextResource->resourceId)) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidResource);
            }
            if (virglResources_.contains(contextResource->resourceId)) {
                renderer_.attachResource(
                    header->contextId, contextResource->resourceId);
            }
            /*
             * Linux also associates legacy RESOURCE_CREATE_2D buffers with a
             * Virgl context. Those buffers remain on the device's software
             * 2D path, so there is no virglrenderer object to attach; context
             * association is valid bookkeeping and intentionally a no-op.
             */
        } else if (contexts_.contains(header->contextId) &&
                   virglResources_.contains(contextResource->resourceId)) {
            /*
             * RESOURCE_UNREF asks virglrenderer to detach the resource from
             * every context before removing it, and context destruction may
             * race the guest's resource bookkeeping. Preserve
             * virglrenderer's idempotent no-op semantics when either object
             * has already gone away.
             */
            renderer_.detachResource(
                header->contextId, contextResource->resourceId);
        }
        return response(*header, protocol::GpuResponse::okNoData);
    }

    case protocol::GpuCommand::submit3d: {
        const auto* submit =
            readStructure<protocol::GpuSubmit3d>(request);
        if (submit == nullptr ||
            !contexts_.contains(header->contextId) ||
            submit->size > protocol::maximumGpuSubmitSize ||
            submit->size % protocol::gpuCommandWordSize != 0 ||
            sizeof(*submit) > request.size() ||
            submit->size > request.size() - sizeof(*submit)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        if (renderer_.submit(
                header->contextId,
                request.subspan(sizeof(*submit), submit->size)) != 0) {
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        }
        if ((header->flags & protocol::gpuFlagFence) != 0 &&
            renderer_.createFenceAndWait(
                header->contextId,
                (header->flags & protocol::gpuFlagInfoRingIndex) != 0
                    ? header->ringIndex
                    : 0,
                header->fenceId) != 0) {
            return response(
                *header, protocol::GpuResponse::errorUnspecified);
        }
        return response(*header, protocol::GpuResponse::okNoData);
    }

    case protocol::GpuCommand::resourceCreate3d: {
        const auto* create =
            readStructure<protocol::GpuResourceCreate3d>(request);
        if (create == nullptr ||
            create->resourceId == protocol::invalidResourceId ||
            create->width == 0 || create->height == 0 ||
            create->depth == 0 || create->arraySize == 0 ||
            resources_.contains(create->resourceId) ||
            virglResources_.contains(create->resourceId)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        const VirglRenderer::Resource3d resource {
            .resourceId = create->resourceId,
            .target = create->target,
            .format = create->format,
            .bind = create->bind,
            .width = create->width,
            .height = create->height,
            .depth = create->depth,
            .arraySize = create->arraySize,
            .lastLevel = create->lastLevel,
            .sampleCount = create->sampleCount,
            .flags = create->flags,
        };
        if (renderer_.createResource3d(resource) != 0) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        try {
            virglResources_.emplace(
                create->resourceId,
                VirglResource {
                    .format =
                        static_cast<protocol::GpuFormat>(create->format),
                    .width = create->width,
                    .height = create->height,
                });
        } catch (const std::bad_alloc&) {
            renderer_.unrefResource(create->resourceId);
            return response(
                *header, protocol::GpuResponse::errorOutOfMemory);
        }
        return response(*header, protocol::GpuResponse::okNoData);
    }

    case protocol::GpuCommand::transferToHost3d:
    case protocol::GpuCommand::transferFromHost3d: {
        const auto* transfer =
            readStructure<protocol::GpuTransferHost3d>(request);
        if (transfer == nullptr ||
            (header->contextId != protocol::invalidContextId &&
             !contexts_.contains(header->contextId))) {
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        }
        const auto resource =
            virglResources_.find(transfer->resourceId);
        if (resource == virglResources_.end()) {
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        }
        if (resource->second.vectors.empty() ||
            transfer->box.width == 0 ||
            transfer->box.height == 0 ||
            transfer->box.depth == 0) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        const VirglRenderer::Box box {
            .x = transfer->box.x,
            .y = transfer->box.y,
            .z = transfer->box.z,
            .width = transfer->box.width,
            .height = transfer->box.height,
            .depth = transfer->box.depth,
        };
        if (renderer_.transferResource(
                command == protocol::GpuCommand::transferToHost3d,
                transfer->resourceId,
                header->contextId,
                transfer->level,
                transfer->stride,
                transfer->layerStride,
                box,
                transfer->offset) != 0) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        if ((header->flags & protocol::gpuFlagFence) != 0 &&
            renderer_.createFenceAndWait(
                header->contextId,
                (header->flags & protocol::gpuFlagInfoRingIndex) != 0
                    ? header->ringIndex
                    : 0,
                header->fenceId) != 0) {
            return response(
                *header, protocol::GpuResponse::errorUnspecified);
        }
        return response(*header, protocol::GpuResponse::okNoData);
    }

    case protocol::GpuCommand::resourceCreate2d: {
        const auto* create =
            readStructure<protocol::GpuResourceCreate2d>(request);
        if (create == nullptr ||
            create->resourceId == protocol::invalidResourceId ||
            create->width == 0 || create->height == 0 ||
            !supportedFormat(create->format)) {
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        }
        if (resources_.contains(create->resourceId) ||
            virglResources_.contains(create->resourceId)) {
            return response(*header, protocol::GpuResponse::errorInvalidResource);
        }
        const std::uint64_t size =
            static_cast<std::uint64_t>(create->width) * create->height *
            protocol::gpuBytesPerPixel;
        if (size > std::numeric_limits<std::size_t>::max())
            return response(*header, protocol::GpuResponse::errorOutOfMemory);
        try {
            Resource resource {
                .format = static_cast<protocol::GpuFormat>(create->format),
                .width = create->width,
                .height = create->height,
                .pixels = std::vector<std::uint8_t>(
                    static_cast<std::size_t>(size)),
            };
            resources_.emplace(create->resourceId, std::move(resource));
        } catch (const std::bad_alloc&) {
            return response(*header, protocol::GpuResponse::errorOutOfMemory);
        }
        return response(*header, protocol::GpuResponse::okNoData);
    }

    case protocol::GpuCommand::resourceUnref: {
        const auto* reference =
            readStructure<protocol::GpuResourceReference>(request);
        if (reference == nullptr)
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        if (resources_.erase(reference->resourceId) != 0) {
            if (scanoutResource_ == reference->resourceId)
                scanoutResource_ = protocol::invalidResourceId;
            return response(*header, protocol::GpuResponse::okNoData);
        }
        if (virglResources_.erase(reference->resourceId) != 0) {
            renderer_.unrefResource(reference->resourceId);
            if (scanoutResource_ == reference->resourceId)
                scanoutResource_ = protocol::invalidResourceId;
            return response(*header, protocol::GpuResponse::okNoData);
        }
        return response(*header, protocol::GpuResponse::errorInvalidResource);
    }

    case protocol::GpuCommand::resourceAttachBacking: {
        const auto* attach =
            readStructure<protocol::GpuAttachBacking>(request);
        if (attach == nullptr ||
            attach->entryCount > protocol::maximumResourceBackingEntries)
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        if (const auto iterator = resources_.find(attach->resourceId);
            iterator != resources_.end()) {
            const std::size_t expected =
                sizeof(*attach) +
                static_cast<std::size_t>(attach->entryCount) *
                    sizeof(protocol::GpuMemoryEntry);
            if (request.size() < expected)
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            std::vector<BackingEntry> backing;
            backing.reserve(attach->entryCount);
            const auto* entries =
                reinterpret_cast<const protocol::GpuMemoryEntry*>(
                    request.data() + sizeof(*attach));
            for (std::uint32_t index = 0;
                 index < attach->entryCount;
                 ++index) {
                if (entries[index].length == 0)
                    return response(
                        *header,
                        protocol::GpuResponse::errorInvalidParameter);
                backing.push_back({
                    .address = entries[index].address,
                    .length = entries[index].length,
                });
            }
            iterator->second.backing = std::move(backing);
            return response(*header, protocol::GpuResponse::okNoData);
        }

        const auto virgl = virglResources_.find(attach->resourceId);
        if (virgl == virglResources_.end())
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        if (!virgl->second.vectors.empty())
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        if (!parseBacking(
                request,
                sizeof(*attach),
                attach->entryCount,
                virgl->second.backing,
                virgl->second.vectors) ||
            renderer_.attachBacking(
                attach->resourceId, virgl->second.vectors) != 0) {
            virgl->second.backing.clear();
            virgl->second.vectors.clear();
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        return response(*header, protocol::GpuResponse::okNoData);
    }

    case protocol::GpuCommand::resourceDetachBacking: {
        const auto* reference =
            readStructure<protocol::GpuResourceReference>(request);
        if (reference == nullptr)
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        if (const auto iterator = resources_.find(reference->resourceId);
            iterator != resources_.end()) {
            iterator->second.backing.clear();
            return response(*header, protocol::GpuResponse::okNoData);
        }
        const auto virgl =
            virglResources_.find(reference->resourceId);
        if (virgl == virglResources_.end())
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        renderer_.detachBacking(reference->resourceId);
        virgl->second.backing.clear();
        virgl->second.vectors.clear();
        return response(*header, protocol::GpuResponse::okNoData);
    }

    case protocol::GpuCommand::transferToHost2d: {
        const auto* transfer =
            readStructure<protocol::GpuTransferToHost2d>(request);
        if (transfer == nullptr)
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        const auto iterator = resources_.find(transfer->resourceId);
        if (iterator == resources_.end())
            return response(*header, protocol::GpuResponse::errorInvalidResource);
        auto& resource = iterator->second;
        if (!rectangleFits(
                transfer->rectangle, resource.width, resource.height)) {
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        }
        const auto rowBytes =
            static_cast<std::size_t>(transfer->rectangle.width) *
            protocol::gpuBytesPerPixel;
        for (std::uint32_t row = 0; row < transfer->rectangle.height; ++row) {
            const auto sourceOffset =
                transfer->offset +
                static_cast<std::uint64_t>(row) * resource.width *
                    protocol::gpuBytesPerPixel;
            const auto destinationOffset =
                (static_cast<std::size_t>(transfer->rectangle.y + row) *
                    resource.width +
                    transfer->rectangle.x) *
                protocol::gpuBytesPerPixel;
            if (!readBacking(
                    resource,
                    sourceOffset,
                    std::span(
                        resource.pixels.data() + destinationOffset, rowBytes))) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            }
        }
        return response(*header, protocol::GpuResponse::okNoData);
    }

    case protocol::GpuCommand::setScanout: {
        const auto* scanout =
            readStructure<protocol::GpuSetScanout>(request);
        if (scanout == nullptr ||
            scanout->scanoutId != protocol::primaryScanoutId)
            return response(*header, protocol::GpuResponse::errorInvalidScanout);
        if (scanout->resourceId == protocol::invalidResourceId) {
            scanoutResource_ = protocol::invalidResourceId;
            return response(*header, protocol::GpuResponse::okNoData);
        }
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (const auto iterator = resources_.find(scanout->resourceId);
            iterator != resources_.end()) {
            width = iterator->second.width;
            height = iterator->second.height;
        } else if (const auto virgl =
                       virglResources_.find(scanout->resourceId);
                   virgl != virglResources_.end()) {
            if (!supportedFormat(
                    static_cast<std::uint32_t>(virgl->second.format))) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            }
            width = virgl->second.width;
            height = virgl->second.height;
            const std::uint64_t byteCount =
                static_cast<std::uint64_t>(width) * height *
                protocol::gpuBytesPerPixel;
            if (byteCount > std::numeric_limits<std::size_t>::max()) {
                return response(
                    *header, protocol::GpuResponse::errorOutOfMemory);
            }
            try {
                virgl->second.pixels.resize(
                    static_cast<std::size_t>(byteCount));
            } catch (const std::bad_alloc&) {
                return response(
                    *header, protocol::GpuResponse::errorOutOfMemory);
            }
        } else {
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        }
        if (!rectangleFits(scanout->rectangle, width, height))
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        scanoutResource_ = scanout->resourceId;
        scanoutRectangle_ = scanout->rectangle;
        return response(*header, protocol::GpuResponse::okNoData);
    }

    case protocol::GpuCommand::resourceFlush: {
        const auto* flush =
            readStructure<protocol::GpuResourceFlush>(request);
        if (flush == nullptr)
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        if (const auto iterator = resources_.find(flush->resourceId);
            iterator != resources_.end()) {
            const auto& resource = iterator->second;
            if (!rectangleFits(
                    flush->rectangle, resource.width, resource.height)) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            }
            if (flush->resourceId != scanoutResource_)
                return response(*header, protocol::GpuResponse::okNoData);
            const auto damage = intersectRectangles(
                flush->rectangle, scanoutRectangle_);
            if (!damage)
                return response(*header, protocol::GpuResponse::okNoData);
            display_.update(
                std::span<const std::uint8_t>(resource.pixels),
                resource.width,
                resource.height,
                static_cast<std::size_t>(resource.width) *
                    protocol::gpuBytesPerPixel,
                resource.format,
                damage->x,
                damage->y,
                damage->x - scanoutRectangle_.x,
                damage->y - scanoutRectangle_.y,
                damage->width,
                damage->height);
            return response(*header, protocol::GpuResponse::okNoData);
        }

        if (const auto virgl = virglResources_.find(flush->resourceId);
            virgl != virglResources_.end()) {
            auto& resource = virgl->second;
            if (!rectangleFits(
                    flush->rectangle, resource.width, resource.height)) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            }
            if (flush->resourceId != scanoutResource_)
                return response(
                    *header, protocol::GpuResponse::okNoData);
            const auto damage = intersectRectangles(
                flush->rectangle, scanoutRectangle_);
            if (!damage)
                return response(
                    *header, protocol::GpuResponse::okNoData);
            const auto expectedBytes =
                static_cast<std::uint64_t>(resource.width) *
                resource.height * protocol::gpuBytesPerPixel;
            if (expectedBytes != resource.pixels.size()) {
                return response(
                    *header, protocol::GpuResponse::errorUnspecified);
            }
            renderer_.readResourceRectangle(
                flush->resourceId,
                resource.pixels,
                0,
                damage->x,
                damage->y,
                damage->width,
                damage->height);
            display_.update(
                std::span<const std::uint8_t>(resource.pixels),
                resource.width,
                resource.height,
                static_cast<std::size_t>(resource.width) *
                    protocol::gpuBytesPerPixel,
                resource.format,
                damage->x,
                damage->y,
                damage->x - scanoutRectangle_.x,
                damage->y - scanoutRectangle_.y,
                damage->width,
                damage->height);
            return response(*header, protocol::GpuResponse::okNoData);
        }

        return response(*header, protocol::GpuResponse::errorInvalidResource);
    }

    default:
        return response(*header, protocol::GpuResponse::errorUnspecified);
    }
}

} // namespace uml::gpu
