// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_gpu_device.hpp"

#if UML_GPU_DEVICE_TESTING
#include "virtio_gpu_device_testing.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>

#ifndef UML_GPU_ANGLE_METAL
#define UML_GPU_ANGLE_METAL 0
#endif

namespace uml::gpu {
namespace {

#if UML_GPU_DEVICE_TESTING
#define UML_GPU_RENDERER dependencies_
#define UML_GPU_DISPLAY dependencies_
#else
#define UML_GPU_RENDERER renderer_
#define UML_GPU_DISPLAY display_
#endif

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

bool backingRangeFits(std::uint64_t address, std::uint32_t length)
{
    return length != 0 &&
        address <= std::numeric_limits<std::uint64_t>::max() - length;
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

std::optional<std::size_t> pixelBufferSize(
    std::uint32_t width,
    std::uint32_t height)
{
    if (width == 0 || height == 0)
        return std::nullopt;
    const auto convertedWidth = static_cast<std::size_t>(width);
    if (height >
        std::numeric_limits<std::size_t>::max() / convertedWidth) {
        return std::nullopt;
    }
    const auto pixels = convertedWidth * static_cast<std::size_t>(height);
    if (pixels >
        std::numeric_limits<std::size_t>::max() /
            protocol::gpuBytesPerPixel) {
        return std::nullopt;
    }
    return pixels * protocol::gpuBytesPerPixel;
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

std::uint16_t physicalMillimeters(
    std::uint32_t pixels,
    std::uint32_t dotsPerInch)
{
    const auto numerator =
        static_cast<std::uint64_t>(pixels) * 254U +
        static_cast<std::uint64_t>(dotsPerInch) * 5U;
    const auto millimeters = numerator /
        (static_cast<std::uint64_t>(dotsPerInch) * 10U);
    return static_cast<std::uint16_t>(std::clamp<std::uint64_t>(
        millimeters, 1U, protocol::maximumEdidDimension));
}

void writeMonitorDescriptor(
    std::array<std::uint8_t, protocol::baseEdidSize>& edid,
    std::size_t offset,
    std::uint8_t tag,
    std::string_view text = {})
{
    std::fill_n(edid.begin() + offset, 18, 0);
    edid[offset + 3U] = tag;
    if (text.empty())
        return;
    std::fill_n(edid.begin() + offset + 5U, 13, ' ');
    const auto length = std::min<std::size_t>(text.size(), 12U);
    std::copy_n(text.begin(), length, edid.begin() + offset + 5U);
    edid[offset + 5U + length] = '\n';
}

std::array<std::uint8_t, protocol::baseEdidSize> makeEdid(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t dotsPerInch)
{
    std::array<std::uint8_t, protocol::baseEdidSize> edid {};
    constexpr std::array<std::uint8_t, 8> header {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    std::copy(header.begin(), header.end(), edid.begin());
    // Big-endian EISA manufacturer ID "UML" and a little-endian product ID.
    edid[8] = 0x55;
    edid[9] = 0xac;
    edid[10] = 0x01;
    edid[16] = 1;
    edid[17] = 36; // 2026, relative to EDID's 1990 epoch.
    edid[18] = 1;
    edid[19] = 4;
    edid[20] = 0x80; // Digital display input.

    const auto widthMillimeters =
        physicalMillimeters(width, dotsPerInch);
    const auto heightMillimeters =
        physicalMillimeters(height, dotsPerInch);
    edid[21] = static_cast<std::uint8_t>(std::clamp<unsigned>(
        (widthMillimeters + 5U) / 10U, 1U, 255U));
    edid[22] = static_cast<std::uint8_t>(std::clamp<unsigned>(
        (heightMillimeters + 5U) / 10U, 1U, 255U));
    edid[23] = 120; // Gamma 2.2.
    edid[24] = 0x06; // sRGB and the first detailed timing is preferred.
    constexpr std::array<std::uint8_t, 10> srgbChromaticity {
        0xee, 0x91, 0xa3, 0x54, 0x4c,
        0x99, 0x26, 0x0f, 0x50, 0x54};
    std::copy(
        srgbChromaticity.begin(), srgbChromaticity.end(),
        edid.begin() + 25U);
    std::fill(edid.begin() + 38U, edid.begin() + 54U, 0x01);

    const auto horizontalBlank = std::min<std::uint32_t>(
        protocol::maximumEdidDimension,
        std::max<std::uint32_t>(160U, width / 5U));
    const auto verticalBlank = std::min<std::uint32_t>(
        protocol::maximumEdidDimension,
        std::max<std::uint32_t>(30U, height / 20U));
    const auto horizontalSyncOffset = std::min<std::uint32_t>(
        1023U, std::max<std::uint32_t>(8U, horizontalBlank / 3U));
    const auto horizontalSyncPulse = std::min<std::uint32_t>(
        1023U, std::max<std::uint32_t>(8U, horizontalBlank / 3U));
    constexpr std::uint32_t verticalSyncOffset = 3;
    constexpr std::uint32_t verticalSyncPulse = 5;
    const auto clock = std::clamp<std::uint64_t>(
        ((static_cast<std::uint64_t>(width) + horizontalBlank) *
             (static_cast<std::uint64_t>(height) + verticalBlank) * 60U +
            5000U) /
            10000U,
        1U,
        65535U);

    constexpr std::size_t timing = 54;
    edid[timing] = static_cast<std::uint8_t>(clock);
    edid[timing + 1U] = static_cast<std::uint8_t>(clock >> 8U);
    edid[timing + 2U] = static_cast<std::uint8_t>(width);
    edid[timing + 3U] = static_cast<std::uint8_t>(horizontalBlank);
    edid[timing + 4U] = static_cast<std::uint8_t>(
        ((width >> 8U) << 4U) | (horizontalBlank >> 8U));
    edid[timing + 5U] = static_cast<std::uint8_t>(height);
    edid[timing + 6U] = static_cast<std::uint8_t>(verticalBlank);
    edid[timing + 7U] = static_cast<std::uint8_t>(
        ((height >> 8U) << 4U) | (verticalBlank >> 8U));
    edid[timing + 8U] = static_cast<std::uint8_t>(horizontalSyncOffset);
    edid[timing + 9U] = static_cast<std::uint8_t>(horizontalSyncPulse);
    edid[timing + 10U] = static_cast<std::uint8_t>(
        (verticalSyncOffset << 4U) | verticalSyncPulse);
    edid[timing + 11U] = static_cast<std::uint8_t>(
        ((horizontalSyncOffset >> 8U) << 6U) |
        ((horizontalSyncPulse >> 8U) << 4U));
    edid[timing + 12U] = static_cast<std::uint8_t>(widthMillimeters);
    edid[timing + 13U] = static_cast<std::uint8_t>(heightMillimeters);
    edid[timing + 14U] = static_cast<std::uint8_t>(
        ((widthMillimeters >> 8U) << 4U) |
        (heightMillimeters >> 8U));
    edid[timing + 17U] = 0x1e; // Non-interlaced separate positive sync.

    writeMonitorDescriptor(edid, 72, 0xfc, "Kernarium");
    writeMonitorDescriptor(edid, 90, 0x10);
    writeMonitorDescriptor(edid, 108, 0x10);
    edid[126] = 0;
    unsigned checksum = 0;
    for (std::size_t index = 0; index < 127U; ++index)
        checksum += edid[index];
    edid[127] = static_cast<std::uint8_t>(0U - checksum);
    return edid;
}

} // namespace

#if UML_GPU_DEVICE_TESTING
VirtioGpuDevice::VirtioGpuDevice(
    testing::VirtioGpuDeviceDependencies& dependencies,
    GuestReader guestReader,
    GuestMapper guestMapper,
    std::uint32_t dotsPerInch)
    : dependencies_(dependencies)
    , guestReader_(std::move(guestReader))
    , guestMapper_(std::move(guestMapper))
    , dotsPerInch_(dotsPerInch)
{
}
#else
VirtioGpuDevice::VirtioGpuDevice(
    Display& display,
    GuestReader guestReader,
    GuestMapper guestMapper,
    std::uint32_t dotsPerInch)
    : display_(display)
    , guestReader_(std::move(guestReader))
    , guestMapper_(std::move(guestMapper))
    , dotsPerInch_(dotsPerInch)
{
}
#endif

VirtioGpuDevice::~VirtioGpuDevice()
{
    reset();
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

void VirtioGpuDevice::setStopRequest(
    const std::atomic_bool* stopRequested) noexcept
{
    stopRequested_ = stopRequested;
}

void VirtioGpuDevice::reset() noexcept
{
    /* Renderer objects may retain iovecs into the current connection's guest
     * mappings. Release them before the transport unmaps those regions. Each
     * tracked renderer object is retired once, then all host bookkeeping is
     * returned to the pristine connection state. */
    for (auto& [resourceId, resource] : virglResources_) {
        if (!resource.vectors.empty()) {
            try {
                UML_GPU_RENDERER.detachBacking(resourceId);
            } catch (...) {
                // Continue to retire the renderer resource itself.
            }
        }
        try {
            UML_GPU_RENDERER.unrefResource(resourceId);
        } catch (...) {
            // Reset is also used by teardown and must finish clearing owners.
        }
    }
    for (const auto contextId : contexts_) {
        try {
            UML_GPU_RENDERER.destroyContext(contextId);
        } catch (...) {
            // Continue retiring the remaining independently owned objects.
        }
    }
    UML_GPU_RENDERER.resetFenceState();
    contexts_.clear();
    virglResources_.clear();
    resources_.clear();
    transferStaging_.clear();
    scanoutResource_ = protocol::invalidResourceId;
    scanoutRectangle_ = {};
    UML_GPU_DISPLAY.clear();
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
        .width = UML_GPU_DISPLAY.width(),
        .height = UML_GPU_DISPLAY.height(),
    };
    information.modes[protocol::primaryScanoutId].enabled =
        protocol::enabledDisplayMode;
    std::vector<std::uint8_t> bytes(sizeof(information));
    std::memcpy(bytes.data(), &information, sizeof(information));
    return bytes;
}

std::vector<std::uint8_t> VirtioGpuDevice::edid(
    const protocol::GpuGetEdid& request) const
{
    const auto width = UML_GPU_DISPLAY.width();
    const auto height = UML_GPU_DISPLAY.height();
    if (request.scanoutId != protocol::primaryScanoutId) {
        return response(
            request.header, protocol::GpuResponse::errorInvalidScanout);
    }
    if (dotsPerInch_ == 0 || width == 0 || height == 0 ||
        width > protocol::maximumEdidDimension ||
        height > protocol::maximumEdidDimension) {
        return response(
            request.header, protocol::GpuResponse::errorInvalidParameter);
    }
    protocol::GpuEdid information {};
    information.header = request.header;
    information.header.type =
        static_cast<std::uint32_t>(protocol::GpuResponse::okEdid);
    information.header.flags &=
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    information.size = protocol::baseEdidSize;
    const auto base = makeEdid(width, height, dotsPerInch_);
    std::copy(base.begin(), base.end(), information.edid.begin());
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
            if (entry.address >
                std::numeric_limits<std::uint64_t>::max() - entryOffset) {
                return false;
            }
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
        if (entry == nullptr ||
            !backingRangeFits(entry->address, entry->length))
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
        const auto virglCapset =
            UML_GPU_RENDERER.capset(information.capsetId);
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
    const auto virglCapset = UML_GPU_RENDERER.capset(request.capsetId);
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

    case protocol::GpuCommand::getEdid: {
        const auto* edidRequest =
            readStructure<protocol::GpuGetEdid>(request);
        if (edidRequest == nullptr)
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        return edid(*edidRequest);
    }

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
        auto success = response(*header, protocol::GpuResponse::okNoData);
        contexts_.insert(create->header.contextId);
        if (UML_GPU_RENDERER.createContext(
                create->header.contextId,
                create->contextInit,
                debugName) != 0) {
            contexts_.erase(create->header.contextId);
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        }
        return success;
    }

    case protocol::GpuCommand::contextDestroy: {
        if (header->contextId == protocol::invalidContextId ||
            !contexts_.contains(header->contextId)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        }
        auto success = response(*header, protocol::GpuResponse::okNoData);
        UML_GPU_RENDERER.destroyContext(header->contextId);
        contexts_.erase(header->contextId);
        return success;
    }

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
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            if (virglResources_.contains(contextResource->resourceId)) {
                if (UML_GPU_RENDERER.attachResource(
                        header->contextId,
                        contextResource->resourceId) != 0) {
                    return response(
                        *header, protocol::GpuResponse::errorUnspecified);
                }
            }
            /*
             * Linux also associates legacy RESOURCE_CREATE_2D buffers with a
             * Virgl context. Those buffers remain on the device's software
             * 2D path, so there is no virglrenderer object to attach; context
             * association is valid bookkeeping and intentionally a no-op.
             */
            return success;
        }
        auto success = response(*header, protocol::GpuResponse::okNoData);
        if (contexts_.contains(header->contextId) &&
            virglResources_.contains(contextResource->resourceId)) {
            /*
             * RESOURCE_UNREF asks virglrenderer to detach the resource from
             * every context before removing it, and context destruction may
             * race the guest's resource bookkeeping. Preserve
             * virglrenderer's idempotent no-op semantics when either object
             * has already gone away.
             */
            if (UML_GPU_RENDERER.detachResource(
                    header->contextId,
                    contextResource->resourceId) != 0) {
                return response(
                    *header, protocol::GpuResponse::errorUnspecified);
            }
        }
        return success;
    }

    case protocol::GpuCommand::submit3d: {
        const auto* submit =
            readStructure<protocol::GpuSubmit3d>(request);
        if (submit == nullptr ||
            submit->size > protocol::maximumGpuSubmitSize ||
            submit->size % protocol::gpuCommandWordSize != 0 ||
            sizeof(*submit) > request.size() ||
            submit->size > request.size() - sizeof(*submit)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        if (!contexts_.contains(header->contextId))
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        auto success = response(*header, protocol::GpuResponse::okNoData);
        if (UML_GPU_RENDERER.submit(
                header->contextId,
                request.subspan(sizeof(*submit), submit->size)) != 0) {
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        }
        if ((header->flags & protocol::gpuFlagFence) != 0 &&
            UML_GPU_RENDERER.createFenceAndWait(
                header->contextId,
                (header->flags & protocol::gpuFlagInfoRingIndex) != 0
                    ? header->ringIndex
                    : 0,
                header->fenceId,
                stopRequested_) != 0) {
            return response(
                *header, protocol::GpuResponse::errorUnspecified);
        }
        return success;
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
        auto success = response(*header, protocol::GpuResponse::okNoData);
        try {
            virglResources_.emplace(
                create->resourceId,
                VirglResource {
                    .format =
                        static_cast<protocol::GpuFormat>(create->format),
                    .target = create->target,
                    .width = create->width,
                    .height = create->height,
                });
        } catch (const std::bad_alloc&) {
            return response(
                *header, protocol::GpuResponse::errorOutOfMemory);
        }
        if (UML_GPU_RENDERER.createResource3d(resource) != 0) {
            virglResources_.erase(create->resourceId);
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        return success;
    }

    case protocol::GpuCommand::transferToHost3d:
    case protocol::GpuCommand::transferFromHost3d: {
        const auto* transfer =
            readStructure<protocol::GpuTransferHost3d>(request);
        if (transfer == nullptr) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        if (header->contextId != protocol::invalidContextId &&
            !contexts_.contains(header->contextId)) {
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
        auto success = response(*header, protocol::GpuResponse::okNoData);
        if (UML_GPU_RENDERER.transferResource(
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
            UML_GPU_RENDERER.createFenceAndWait(
                header->contextId,
                (header->flags & protocol::gpuFlagInfoRingIndex) != 0
                    ? header->ringIndex
                    : 0,
                header->fenceId,
                stopRequested_) != 0) {
            return response(
                *header, protocol::GpuResponse::errorUnspecified);
        }
        return success;
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
        const auto size = pixelBufferSize(create->width, create->height);
        if (!size)
            return response(*header, protocol::GpuResponse::errorOutOfMemory);
        try {
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            Resource resource {
                .format = static_cast<protocol::GpuFormat>(create->format),
                .width = create->width,
                .height = create->height,
                .pixels = std::vector<std::uint8_t>(*size),
            };
            resources_.emplace(create->resourceId, std::move(resource));
            return success;
        } catch (const std::bad_alloc&) {
            return response(*header, protocol::GpuResponse::errorOutOfMemory);
        }
    }

    case protocol::GpuCommand::resourceUnref: {
        const auto* reference =
            readStructure<protocol::GpuResourceReference>(request);
        if (reference == nullptr)
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        auto success = response(*header, protocol::GpuResponse::okNoData);
        if (resources_.erase(reference->resourceId) != 0) {
            if (scanoutResource_ == reference->resourceId) {
                scanoutResource_ = protocol::invalidResourceId;
                scanoutRectangle_ = {};
                UML_GPU_DISPLAY.clear();
            }
            return success;
        }
        if (const auto virgl = virglResources_.find(reference->resourceId);
            virgl != virglResources_.end()) {
            /* The renderer retains the resource's iovec array, so release its
             * reference before erasing the vector that owns that array. */
            UML_GPU_RENDERER.unrefResource(reference->resourceId);
            virglResources_.erase(virgl);
            if (scanoutResource_ == reference->resourceId) {
                scanoutResource_ = protocol::invalidResourceId;
                scanoutRectangle_ = {};
                UML_GPU_DISPLAY.clear();
            }
            return success;
        }
        return response(*header, protocol::GpuResponse::errorInvalidResource);
    }

    case protocol::GpuCommand::resourceAttachBacking: {
        const auto* attach =
            readStructure<protocol::GpuAttachBacking>(request);
        if (attach == nullptr ||
            attach->entryCount == 0 ||
            attach->entryCount > protocol::maximumResourceBackingEntries)
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        if (const auto iterator = resources_.find(attach->resourceId);
            iterator != resources_.end()) {
            if (!iterator->second.backing.empty())
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
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
                if (!backingRangeFits(
                        entries[index].address, entries[index].length))
                    return response(
                        *header,
                        protocol::GpuResponse::errorInvalidParameter);
                backing.push_back({
                    .address = entries[index].address,
                    .length = entries[index].length,
                });
            }
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            iterator->second.backing = std::move(backing);
            return success;
        }

        const auto virgl = virglResources_.find(attach->resourceId);
        if (virgl == virglResources_.end())
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        if (!virgl->second.backing.empty() ||
            !virgl->second.vectors.empty())
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        auto success = response(*header, protocol::GpuResponse::okNoData);
        std::vector<BackingEntry> backing;
        std::vector<iovec> vectors;
        if (!parseBacking(
                request,
                sizeof(*attach),
                attach->entryCount,
                backing,
                vectors)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        const int attachResult = UML_GPU_RENDERER.attachBacking(
            attach->resourceId, vectors);
        if (attachResult != 0) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        /* Virgl retains the iovec array pointer. Swapping the successfully
         * attached allocation into the resource preserves that address and
         * makes the final bookkeeping commit non-throwing. */
        virgl->second.backing.swap(backing);
        virgl->second.vectors.swap(vectors);
        return success;
    }

    case protocol::GpuCommand::resourceDetachBacking: {
        const auto* reference =
            readStructure<protocol::GpuResourceReference>(request);
        if (reference == nullptr)
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        if (const auto iterator = resources_.find(reference->resourceId);
            iterator != resources_.end()) {
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            iterator->second.backing.clear();
            return success;
        }
        const auto virgl =
            virglResources_.find(reference->resourceId);
        if (virgl == virglResources_.end())
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        auto success = response(*header, protocol::GpuResponse::okNoData);
        UML_GPU_RENDERER.detachBacking(reference->resourceId);
        virgl->second.backing.clear();
        virgl->second.vectors.clear();
        return success;
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
        auto success = response(*header, protocol::GpuResponse::okNoData);
        try {
            transferStaging_.resize(
                rowBytes * static_cast<std::size_t>(
                    transfer->rectangle.height));
        } catch (const std::bad_alloc&) {
            return response(*header, protocol::GpuResponse::errorOutOfMemory);
        }
        const std::uint64_t sourceStride =
            static_cast<std::uint64_t>(resource.width) *
            protocol::gpuBytesPerPixel;
        for (std::uint32_t row = 0; row < transfer->rectangle.height; ++row) {
            if (static_cast<std::uint64_t>(row) >
                (std::numeric_limits<std::uint64_t>::max() -
                    transfer->offset) /
                    sourceStride) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            }
            const auto sourceOffset =
                transfer->offset +
                static_cast<std::uint64_t>(row) * sourceStride;
            if (!readBacking(
                    resource,
                    sourceOffset,
                    std::span(transferStaging_).subspan(
                        static_cast<std::size_t>(row) * rowBytes,
                        rowBytes))) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            }
        }
        for (std::uint32_t row = 0; row < transfer->rectangle.height; ++row) {
            const auto destinationOffset =
                (static_cast<std::size_t>(transfer->rectangle.y + row) *
                    resource.width +
                    transfer->rectangle.x) *
                protocol::gpuBytesPerPixel;
            std::memcpy(
                resource.pixels.data() + destinationOffset,
                transferStaging_.data() +
                    static_cast<std::size_t>(row) * rowBytes,
                rowBytes);
        }
        return success;
    }

    case protocol::GpuCommand::setScanout: {
        const auto* scanout =
            readStructure<protocol::GpuSetScanout>(request);
        if (scanout == nullptr ||
            scanout->scanoutId != protocol::primaryScanoutId)
            return response(*header, protocol::GpuResponse::errorInvalidScanout);
        if (scanout->resourceId == protocol::invalidResourceId) {
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            scanoutResource_ = protocol::invalidResourceId;
            scanoutRectangle_ = {};
            UML_GPU_DISPLAY.clear();
            return success;
        }
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        VirglResource* virglResource = nullptr;
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
            virglResource = &virgl->second;
        } else {
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        }
        if (!rectangleFits(scanout->rectangle, width, height))
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        std::vector<std::uint8_t> replacementPixels;
        bool replacePixels = false;
        if (virglResource != nullptr) {
            const auto byteCount = pixelBufferSize(width, height);
            if (!byteCount) {
                return response(
                    *header, protocol::GpuResponse::errorOutOfMemory);
            }
            if (virglResource->pixels.size() != *byteCount) {
                try {
                    replacementPixels.resize(*byteCount);
                } catch (const std::bad_alloc&) {
                    return response(
                        *header, protocol::GpuResponse::errorOutOfMemory);
                }
                replacePixels = true;
            }
        }
        auto success = response(*header, protocol::GpuResponse::okNoData);
        if (replacePixels)
            virglResource->pixels.swap(replacementPixels);
        scanoutResource_ = scanout->resourceId;
        scanoutRectangle_ = scanout->rectangle;
        return success;
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
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            try {
                UML_GPU_DISPLAY.update(
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
            } catch (const std::bad_alloc&) {
                return response(
                    *header, protocol::GpuResponse::errorOutOfMemory);
            } catch (...) {
                return response(
                    *header, protocol::GpuResponse::errorUnspecified);
            }
            return success;
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
                pixelBufferSize(resource.width, resource.height);
            if (!expectedBytes) {
                return response(
                    *header, protocol::GpuResponse::errorOutOfMemory);
            }
            if (*expectedBytes != resource.pixels.size()) {
                return response(
                    *header, protocol::GpuResponse::errorUnspecified);
            }
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
#if UML_GPU_ANGLE_METAL
            void* metalTexture = nullptr;
            if (UML_GPU_RENDERER.resourceMetalTexture(
                    flush->resourceId, metalTexture) == 0 &&
                UML_GPU_DISPLAY.presentMetal(MetalScanout {
                    .texture = metalTexture,
                    .sourceWidth = resource.width,
                    .sourceHeight = resource.height,
                    .sourceX = scanoutRectangle_.x,
                    .sourceY = scanoutRectangle_.y,
                    .width = scanoutRectangle_.width,
                    .height = scanoutRectangle_.height,
                })) {
                return success;
            }
#else
            std::uint32_t textureId = 0;
            if (UML_GPU_RENDERER.resourceTexture(
                    flush->resourceId, textureId) == 0 &&
                UML_GPU_DISPLAY.presentOpenGl(OpenGlScanout {
                    .textureId = textureId,
                    .resourceTarget = resource.target,
                    .sourceWidth = resource.width,
                    .sourceHeight = resource.height,
                    .sourceX = scanoutRectangle_.x,
                    .sourceY = scanoutRectangle_.y,
                    .width = scanoutRectangle_.width,
                    .height = scanoutRectangle_.height,
                })) {
                return success;
            }
#endif
            const auto fallbackDamage =
                UML_GPU_DISPLAY.needsFullCpuFallback() ?
                scanoutRectangle_ : *damage;
            std::vector<std::uint8_t> replacementPixels;
            try {
                replacementPixels = resource.pixels;
                if (UML_GPU_RENDERER.readResourceRectangle(
                        flush->resourceId,
                        replacementPixels,
                        0,
                        fallbackDamage.x,
                        fallbackDamage.y,
                        fallbackDamage.width,
                        fallbackDamage.height) != 0) {
                    return response(
                        *header,
                        protocol::GpuResponse::errorUnspecified);
                }
                UML_GPU_DISPLAY.update(
                    std::span<const std::uint8_t>(replacementPixels),
                    resource.width,
                    resource.height,
                    static_cast<std::size_t>(resource.width) *
                        protocol::gpuBytesPerPixel,
                    resource.format,
                    fallbackDamage.x,
                    fallbackDamage.y,
                    fallbackDamage.x - scanoutRectangle_.x,
                    fallbackDamage.y - scanoutRectangle_.y,
                    fallbackDamage.width,
                    fallbackDamage.height);
            } catch (const std::bad_alloc&) {
                return response(
                    *header, protocol::GpuResponse::errorOutOfMemory);
            } catch (...) {
                return response(
                    *header, protocol::GpuResponse::errorUnspecified);
            }
            resource.pixels.swap(replacementPixels);
            return success;
        }

        return response(*header, protocol::GpuResponse::errorInvalidResource);
    }

    default:
        return response(*header, protocol::GpuResponse::errorUnspecified);
    }
}

} // namespace uml::gpu

#undef UML_GPU_RENDERER
#undef UML_GPU_DISPLAY
