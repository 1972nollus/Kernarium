// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

namespace uml::gpu {

/* The device memory the host grants a Venus guest, as one pool that every
 * consumer charges before it allocates: the renderer for each allocation the
 * guest asks for, and the host GPU driver for memory it allocates for itself,
 * such as KosmicKrisp's geometry heap.
 *
 * Its usage is what the host GPU holds for the process, which `allocated`
 * reports, plus the reservations of allocations under way that `allocated`
 * does not count yet. A reservation is admitted only when the usage and it
 * fit the limit, decided under the pool's lock, and its consumer releases it
 * once `allocated` counts the allocation or the allocation has failed. So
 * allocations that each fit alone never together take the process past the
 * limit, whichever threads make them. Reservations are charged in whole
 * granules, the unit in which `allocated` counts an allocation. */
class DeviceMemoryPool final {
public:
    DeviceMemoryPool(std::uint64_t limit,
        std::function<std::uint64_t()> allocated,
        std::uint64_t granule);

    DeviceMemoryPool(const DeviceMemoryPool&) = delete;
    DeviceMemoryPool& operator=(const DeviceMemoryPool&) = delete;

    [[nodiscard]] std::uint64_t limit() const noexcept { return limit_; }

    /* The bytes allocated and reserved now. */
    [[nodiscard]] std::uint64_t usage() const;

    /* Charges `bytes` to the pool, or returns false and charges nothing when
     * the pool cannot hold them. */
    [[nodiscard]] bool reserve(std::uint64_t bytes);

    /* Returns a reservation of `bytes`. */
    void release(std::uint64_t bytes) noexcept;

private:
    [[nodiscard]] std::uint64_t charge(std::uint64_t bytes) const noexcept;
    [[nodiscard]] std::uint64_t usageLocked() const;

    const std::uint64_t limit_;
    const std::function<std::uint64_t()> allocated_;
    const std::uint64_t granule_;
    mutable std::mutex mutex_;
    std::uint64_t reserved_ {};
};

} // namespace uml::gpu
