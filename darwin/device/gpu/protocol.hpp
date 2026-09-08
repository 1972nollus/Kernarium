// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::gpu::protocol {

constexpr std::uint32_t vhostUserVersion = 1;
constexpr std::uint32_t vhostUserVersionMask = 0x3U;
constexpr std::uint32_t vhostUserReply = 1U << 2U;
constexpr std::uint32_t vhostUserNeedReply = 1U << 3U;

enum class VhostRequest : std::uint32_t {
    getFeatures = 1,
    setFeatures = 2,
    setOwner = 3,
    resetOwner = 4,
    setMemTable = 5,
    setVringNum = 8,
    setVringAddr = 9,
    setVringBase = 10,
    getVringBase = 11,
    setVringKick = 12,
    setVringCall = 13,
    getProtocolFeatures = 15,
    setProtocolFeatures = 16,
    getQueueNum = 17,
    setVringEnable = 18,
    setSlaveRequestFd = 21,
    getConfig = 24,
    setConfig = 25,
    // In-band queue notification enabled by PROTOCOL_F_INBAND_NOTIFICATIONS.
    vringKick = 35,
    getSharedMemoryConfig = 44,
};

enum class VhostSlaveRequest : std::uint32_t {
    configChange = 2,
    vringCall = 4,
    sharedMemoryMap = 9,
    sharedMemoryUnmap = 10,
};

constexpr std::uint64_t vhostFeatureProtocol = 1ULL << 30U;
constexpr std::uint64_t virtioFeatureVersion1 = 1ULL << 32U;
// Virtio-GPU device feature bits from include/uapi/linux/virtio_gpu.h.
constexpr std::uint64_t virtioGpuFeatureVirgl = 1ULL << 0U;
constexpr std::uint64_t virtioGpuFeatureEdid = 1ULL << 1U;
constexpr std::uint64_t virtioGpuFeatureResourceBlob = 1ULL << 3U;
constexpr std::uint64_t virtioGpuFeatureContextInit = 1ULL << 4U;
constexpr std::uint64_t protocolFeatureMultipleQueues = 1ULL << 0U;
constexpr std::uint64_t protocolFeatureReplyAck = 1ULL << 3U;
constexpr std::uint64_t protocolFeatureSlaveRequest = 1ULL << 5U;
constexpr std::uint64_t protocolFeatureConfig = 1ULL << 9U;
constexpr std::uint64_t protocolFeatureInbandNotifications = 1ULL << 14U;
constexpr std::uint64_t protocolFeatureSharedMemoryMap = 1ULL << 22U;

struct [[gnu::packed]] VhostHeader {
    std::uint32_t request;
    std::uint32_t flags;
    std::uint32_t size;
};

struct [[gnu::packed]] VringState {
    std::uint32_t index;
    std::uint32_t value;
};

struct [[gnu::packed]] VringAddress {
    std::uint32_t index;
    std::uint32_t flags;
    std::uint64_t descriptor;
    std::uint64_t used;
    std::uint64_t available;
    std::uint64_t log;
};

struct [[gnu::packed]] MemoryRegion {
    std::uint64_t guestAddress;
    std::uint64_t size;
    std::uint64_t userAddress;
    std::uint64_t mmapOffset;
};

struct [[gnu::packed]] MemoryTableHeader {
    std::uint32_t regionCount;
    std::uint32_t padding;
};

struct [[gnu::packed]] ConfigHeader {
    std::uint32_t offset;
    std::uint32_t size;
    std::uint32_t flags;
};

constexpr std::size_t maximumSharedMemoryRegions = 256;

struct [[gnu::packed]] SharedMemoryConfig {
    std::uint32_t regionCount;
    std::uint32_t padding;
    std::array<std::uint64_t, maximumSharedMemoryRegions> regionSizes;
};

struct [[gnu::packed]] SharedMemoryMap {
    std::uint8_t regionId;
    std::array<std::uint8_t, 7> padding;
    std::uint64_t descriptorOffset;
    std::uint64_t regionOffset;
    std::uint64_t length;
    std::uint64_t flags;
};

constexpr std::uint64_t sharedMemoryMapReadOnly = 0;
constexpr std::uint64_t sharedMemoryMapReadWrite = 1;

struct [[gnu::packed]] VirtqueueDescriptor {
    std::uint64_t address;
    std::uint32_t length;
    std::uint16_t flags;
    std::uint16_t next;
};

struct [[gnu::packed]] VirtqueueUsedElement {
    std::uint32_t id;
    std::uint32_t length;
};

constexpr std::uint16_t descriptorNext = 1U;
constexpr std::uint16_t descriptorWrite = 2U;
constexpr std::uint16_t descriptorIndirect = 4U;
constexpr std::uint16_t availableSuppressInterrupt = 1U;
// Vhost-user encodes the queue index and "no descriptor" flag in one u64.
constexpr std::uint64_t vhostVringIndexMask = 0xffU;
constexpr std::uint64_t vhostVringNoDescriptor = 1U << 8U;
constexpr std::uint64_t vhostVringDescriptorMask =
    vhostVringIndexMask | vhostVringNoDescriptor;
constexpr std::uint32_t configFlagWritable = 0;
constexpr std::uint32_t configFlagMigration = 1;
constexpr std::size_t splitRingHeaderSize =
    2U * sizeof(std::uint16_t); // flags followed by the producer index.
constexpr std::size_t gpuControlQueue = 0;
constexpr std::size_t gpuCursorQueue = 1;
constexpr std::size_t gpuQueueCount = 2;
constexpr std::uint32_t maximumVirtqueueSize = 256;
// Device ID 16 is VIRTIO_ID_GPU in the stable Virtio device ID registry.
constexpr std::uint32_t virtioGpuDeviceId = 16;

enum class GpuCommand : std::uint32_t {
    getDisplayInfo = 0x0100,
    resourceCreate2d = 0x0101,
    resourceUnref = 0x0102,
    setScanout = 0x0103,
    resourceFlush = 0x0104,
    transferToHost2d = 0x0105,
    resourceAttachBacking = 0x0106,
    resourceDetachBacking = 0x0107,
    getCapsetInfo = 0x0108,
    getCapset = 0x0109,
    getEdid = 0x010a,
    resourceCreateBlob = 0x010c,
    setScanoutBlob = 0x010d,
    contextCreate = 0x0200,
    contextDestroy = 0x0201,
    contextAttachResource = 0x0202,
    contextDetachResource = 0x0203,
    resourceCreate3d = 0x0204,
    transferToHost3d = 0x0205,
    transferFromHost3d = 0x0206,
    submit3d = 0x0207,
    resourceMapBlob = 0x0208,
    resourceUnmapBlob = 0x0209,
    updateCursor = 0x0300,
    moveCursor = 0x0301,
};

enum class GpuResponse : std::uint32_t {
    okNoData = 0x1100,
    okDisplayInfo = 0x1101,
    okCapsetInfo = 0x1102,
    okCapset = 0x1103,
    okEdid = 0x1104,
    okMapInfo = 0x1106,
    errorUnspecified = 0x1200,
    errorOutOfMemory = 0x1201,
    errorInvalidScanout = 0x1202,
    errorInvalidResource = 0x1203,
    errorInvalidContext = 0x1204,
    errorInvalidParameter = 0x1205,
};

// Values are fixed by enum virtio_gpu_formats in the Virtio-GPU wire ABI.
enum class GpuFormat : std::uint32_t {
    b8g8r8a8Unorm = 1,
    b8g8r8x8Unorm = 2,
    a8r8g8b8Unorm = 3,
    x8r8g8b8Unorm = 4,
    r8g8b8a8Unorm = 67,
    x8b8g8r8Unorm = 68,
    a8b8g8r8Unorm = 121,
    r8g8b8x8Unorm = 134,
};

constexpr std::uint32_t gpuFlagFence = 1U << 0U;
constexpr std::uint32_t gpuFlagInfoRingIndex = 1U << 1U;
constexpr std::size_t maxGpuScanouts = 16;
constexpr std::size_t gpuBytesPerPixel = 4;
constexpr std::uint32_t primaryScanoutId = 0;
constexpr std::uint32_t enabledDisplayMode = 1;
constexpr std::uint32_t gpuEventDisplay = 1U << 0U;
constexpr std::uint32_t invalidResourceId = 0;
constexpr std::uint32_t invalidContextId = 0;
// Virgl capsets from the stable Virtio-GPU ABI.
constexpr std::uint32_t virglCapsetId = 1;
constexpr std::uint32_t virgl2CapsetId = 2;
constexpr std::uint32_t venusCapsetId = 4;
constexpr std::uint32_t virglCapsetIndex = 0;
constexpr std::uint32_t virgl2CapsetIndex = 1;
constexpr std::uint32_t venusCapsetIndex = 2;
constexpr std::uint32_t contextInitCapsetMask = 0xffU;
constexpr std::uint32_t venusContextRingCount = 64;
constexpr std::size_t maximumContextDebugNameLength = 64;
constexpr std::size_t gpuCommandWordSize = sizeof(std::uint32_t);
constexpr std::size_t maximumGpuSubmitSize = 16U * 1024U * 1024U;
constexpr std::size_t maximumEdidSize = 1024;
constexpr std::size_t baseEdidSize = 128;
constexpr std::uint32_t maximumEdidDimension = 4095;
constexpr std::uint32_t maximumDisplayDimension = 8192;
constexpr std::uint32_t referenceDotsPerInch = 96;
// Defensive bound for the variable-size RESOURCE_ATTACH_BACKING request.
constexpr std::uint32_t maximumResourceBackingEntries = 16'384;
constexpr std::uint32_t blobMemoryGuest = 0x0001;
constexpr std::uint32_t blobMemoryHost3d = 0x0002;
constexpr std::uint32_t blobMemoryHost3dGuest = 0x0003;
constexpr std::uint32_t blobFlagMappable = 0x0001;
constexpr std::uint32_t blobFlagShareable = 0x0002;
constexpr std::uint32_t blobFlagCrossDevice = 0x0004;
constexpr std::uint32_t blobFlagMask =
    blobFlagMappable | blobFlagShareable | blobFlagCrossDevice;
constexpr std::uint32_t mapCacheMask = 0x0f;
constexpr std::uint32_t mapCacheNone = 0x00;
constexpr std::uint32_t mapCacheCached = 0x01;
constexpr std::uint32_t mapCacheUncached = 0x02;
constexpr std::uint32_t mapCacheWriteCombined = 0x03;
constexpr std::uint8_t hostVisibleSharedMemoryRegionId = 1;
constexpr std::uint64_t hostVisibleSharedMemoryRegionSize =
    1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t hostVisibleSharedMemoryAlignment = 16ULL * 1024ULL;
constexpr std::size_t maximumHostVisibleBlobResources =
    hostVisibleSharedMemoryRegionSize / hostVisibleSharedMemoryAlignment;

struct [[gnu::packed]] GpuControlHeader {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint64_t fenceId;
    std::uint32_t contextId;
    std::uint8_t ringIndex;
    std::array<std::uint8_t, 3> padding;
};

struct [[gnu::packed]] GpuRectangle {
    std::uint32_t x;
    std::uint32_t y;
    std::uint32_t width;
    std::uint32_t height;
};

struct [[gnu::packed]] GpuDisplayMode {
    GpuRectangle rectangle;
    std::uint32_t enabled;
    std::uint32_t flags;
};

struct [[gnu::packed]] GpuDisplayInfo {
    GpuControlHeader header;
    std::array<GpuDisplayMode, maxGpuScanouts> modes;
};

struct [[gnu::packed]] GpuConfig {
    std::uint32_t eventsRead;
    std::uint32_t eventsClear;
    std::uint32_t scanoutCount;
    std::uint32_t capsetCount;
};

struct [[gnu::packed]] GpuResourceCreate2d {
    GpuControlHeader header;
    std::uint32_t resourceId;
    std::uint32_t format;
    std::uint32_t width;
    std::uint32_t height;
};

struct [[gnu::packed]] GpuResourceReference {
    GpuControlHeader header;
    std::uint32_t resourceId;
    std::uint32_t padding;
};

struct [[gnu::packed]] GpuSetScanout {
    GpuControlHeader header;
    GpuRectangle rectangle;
    std::uint32_t scanoutId;
    std::uint32_t resourceId;
};

struct [[gnu::packed]] GpuResourceFlush {
    GpuControlHeader header;
    GpuRectangle rectangle;
    std::uint32_t resourceId;
    std::uint32_t padding;
};

struct [[gnu::packed]] GpuTransferToHost2d {
    GpuControlHeader header;
    GpuRectangle rectangle;
    std::uint64_t offset;
    std::uint32_t resourceId;
    std::uint32_t padding;
};

struct [[gnu::packed]] GpuAttachBacking {
    GpuControlHeader header;
    std::uint32_t resourceId;
    std::uint32_t entryCount;
};

struct [[gnu::packed]] GpuMemoryEntry {
    std::uint64_t address;
    std::uint32_t length;
    std::uint32_t padding;
};

struct [[gnu::packed]] GpuGetCapsetInfo {
    GpuControlHeader header;
    std::uint32_t capsetIndex;
    std::uint32_t padding;
};

struct [[gnu::packed]] GpuCapsetInfo {
    GpuControlHeader header;
    std::uint32_t capsetId;
    std::uint32_t maximumVersion;
    std::uint32_t maximumSize;
    std::uint32_t padding;
};

struct [[gnu::packed]] GpuGetCapset {
    GpuControlHeader header;
    std::uint32_t capsetId;
    std::uint32_t version;
};

struct [[gnu::packed]] GpuGetEdid {
    GpuControlHeader header;
    std::uint32_t scanoutId;
    std::uint32_t padding;
};

struct [[gnu::packed]] GpuEdid {
    GpuControlHeader header;
    std::uint32_t size;
    std::uint32_t padding;
    std::array<std::uint8_t, maximumEdidSize> edid;
};

struct [[gnu::packed]] GpuContextCreate {
    GpuControlHeader header;
    std::uint32_t nameLength;
    std::uint32_t contextInit;
    std::array<char, maximumContextDebugNameLength> debugName;
};

struct [[gnu::packed]] GpuContextResource {
    GpuControlHeader header;
    std::uint32_t resourceId;
    std::uint32_t padding;
};

struct [[gnu::packed]] GpuSubmit3d {
    GpuControlHeader header;
    std::uint32_t size;
    std::uint32_t padding;
};

struct [[gnu::packed]] GpuBox {
    std::uint32_t x;
    std::uint32_t y;
    std::uint32_t z;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
};

struct [[gnu::packed]] GpuTransferHost3d {
    GpuControlHeader header;
    GpuBox box;
    std::uint64_t offset;
    std::uint32_t resourceId;
    std::uint32_t level;
    std::uint32_t stride;
    std::uint32_t layerStride;
};

struct [[gnu::packed]] GpuResourceCreate3d {
    GpuControlHeader header;
    std::uint32_t resourceId;
    std::uint32_t target;
    std::uint32_t format;
    std::uint32_t bind;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
    std::uint32_t arraySize;
    std::uint32_t lastLevel;
    std::uint32_t sampleCount;
    std::uint32_t flags;
    std::uint32_t padding;
};

struct [[gnu::packed]] GpuResourceCreateBlob {
    GpuControlHeader header;
    std::uint32_t resourceId;
    std::uint32_t memory;
    std::uint32_t flags;
    std::uint32_t entryCount;
    std::uint64_t blobId;
    std::uint64_t size;
};

struct [[gnu::packed]] GpuSetScanoutBlob {
    GpuControlHeader header;
    GpuRectangle rectangle;
    std::uint32_t scanoutId;
    std::uint32_t resourceId;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t format;
    std::uint32_t padding;
    std::array<std::uint32_t, 4> strides;
    std::array<std::uint32_t, 4> offsets;
};

struct [[gnu::packed]] GpuResourceMapBlob {
    GpuControlHeader header;
    std::uint32_t resourceId;
    std::uint32_t padding;
    std::uint64_t offset;
};

struct [[gnu::packed]] GpuResourceUnmapBlob {
    GpuControlHeader header;
    std::uint32_t resourceId;
    std::uint32_t padding;
};

struct [[gnu::packed]] GpuMapInfo {
    GpuControlHeader header;
    std::uint32_t mapInfo;
    std::uint32_t padding;
};

struct [[gnu::packed]] GpuCursorPosition {
    std::uint32_t scanoutId;
    std::uint32_t x;
    std::uint32_t y;
    std::uint32_t padding;
};

struct [[gnu::packed]] GpuUpdateCursor {
    GpuControlHeader header;
    GpuCursorPosition position;
    std::uint32_t resourceId;
    std::uint32_t hotX;
    std::uint32_t hotY;
    std::uint32_t padding;
};

static_assert(sizeof(VhostHeader) == 12);
static_assert(sizeof(VringAddress) == 40);
static_assert(sizeof(MemoryTableHeader) == 8);
static_assert(sizeof(MemoryRegion) == 32);
static_assert(sizeof(SharedMemoryConfig) == 2056);
static_assert(sizeof(SharedMemoryMap) == 40);
static_assert(sizeof(VirtqueueDescriptor) == 16);
static_assert(sizeof(GpuControlHeader) == 24);
static_assert(sizeof(GpuDisplayInfo) == 408);
static_assert(sizeof(GpuCapsetInfo) == 40);
static_assert(sizeof(GpuGetEdid) == 32);
static_assert(sizeof(GpuEdid) == 1056);
static_assert(sizeof(GpuContextCreate) == 96);
static_assert(sizeof(GpuResourceCreateBlob) == 56);
static_assert(sizeof(GpuSetScanoutBlob) == 96);
static_assert(sizeof(GpuResourceMapBlob) == 40);
static_assert(sizeof(GpuResourceUnmapBlob) == 32);
static_assert(sizeof(GpuMapInfo) == 32);
static_assert(sizeof(GpuConfig) == 16);

} // namespace uml::gpu::protocol
