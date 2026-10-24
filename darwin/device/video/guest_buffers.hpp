// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "shared_memory_regions.hpp"
#include "virtio_video.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace uml::video {

/* One contiguous piece of a guest buffer. `address` is a guest-physical
 * address, or an offset in shared-memory region `region` when the guest marked
 * the entry shared. */
struct GuestSegment {
    std::uint64_t address;
    std::uint32_t length;
    bool shared {};
    std::uint8_t region {};
};

/* The planes of one guest buffer, each a list of segments. */
using GuestPlanes = std::vector<std::vector<GuestSegment>>;

using GuestMapper = std::function<std::span<std::uint8_t>(std::uint64_t, std::size_t)>;

/* The planes a RESOURCE_ATTACH command describes after its fixed fields:
 * `planeCount` scatter lists, each a count and its entries. Every entry is
 * checked against guest memory, or against the published shared-memory
 * regions for an entry the guest marked shared. Throws std::invalid_argument
 * for anything malformed, out of range or left over. */
[[nodiscard]] GuestPlanes parseScatterLists(
    std::span<const std::uint8_t> payload, unsigned planeCount,
    const GuestMapper& mapper, const uml::virtio::SharedMemoryRegions* regions);

/* Copies `data.size()` bytes between `data` and the plane, starting `skip`
 * bytes into it: from the plane when `write` is false, into it when true.
 * A shared-memory range is resolved again for every transfer: the backend
 * that owns the region withdraws it from the index before the guest unmaps
 * it, so a buffer whose blob has gone completes with an error instead of
 * touching memory the guest no longer has. Returns false, having copied
 * part of the range at most, when the plane is shorter than the range or a
 * segment cannot be reached. */
[[nodiscard]] bool transferSegments(
    const GuestMapper& mapper, const uml::virtio::SharedMemoryRegions* regions,
    std::span<const GuestSegment> segments, std::span<std::uint8_t> data, bool write,
    std::uint64_t skip = 0);

} // namespace uml::video
