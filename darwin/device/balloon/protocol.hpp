// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

/* The standard Virtio memory balloon, as Linux's virtio_balloon drives it. */
namespace uml::balloon::protocol {

inline constexpr std::uint32_t deviceId = 5;

/* The driver tells the host before it uses a page it deflated. */
inline constexpr std::uint64_t featureMustTellHost = 1ULL << 0;

inline constexpr std::uint32_t inflateQueue = 0;
inline constexpr std::uint32_t deflateQueue = 1;
/* Linux asks for every queue of the standard layout (inflate, deflate,
 * statistics, free-page hinting, reporting) and sets up only those whose
 * feature was negotiated, so the device must count all five. */
inline constexpr std::size_t queueCount = 5;

/* Balloon page frame numbers are in 4 KiB units whatever the guest page size. */
inline constexpr unsigned pageShift = 12;
inline constexpr std::uint64_t pageSize = 1ULL << pageShift;

/* Little-endian. The two fields after `actual` belong to features this device
 * does not offer and always read as zero. */
struct [[gnu::packed]] Configuration final {
    std::uint32_t numPages;
    std::uint32_t actual;
    std::uint32_t freePageHintCommandId;
    std::uint32_t poisonValue;
};

inline constexpr std::uint32_t actualOffset = 4;

static_assert(sizeof(Configuration) == 16);

} // namespace uml::balloon::protocol
