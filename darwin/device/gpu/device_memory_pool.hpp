// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
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
 * granules, the unit in which `allocated` counts an allocation.
 *
 * Memory the host driver caches for reuse counts against the pool, and the
 * driver releases it only when a client of that driver is refused, never for
 * another consumer's refusal. So the pool takes one reclaim hook, which frees
 * what the host drivers cache and reports how many cached allocations it
 * freed, and calls it before it refuses: a reservation that does not fit
 * runs the hook and is decided once more. */
class DeviceMemoryPool final {
public:
    /* Frees what the host drivers cache for reuse, on whichever thread was
     * refused, and returns the number of cached allocations freed. It runs
     * with none of the pool's locks held, so it may release reservations and
     * read the usage, and at most one runs at a time. The refused thread may
     * hold locks of the host driver that the hook must not take. */
    using Reclaim = std::function<std::uint64_t()>;

    DeviceMemoryPool(std::uint64_t limit,
        std::function<std::uint64_t()> allocated,
        std::uint64_t granule);

    DeviceMemoryPool(const DeviceMemoryPool&) = delete;
    DeviceMemoryPool& operator=(const DeviceMemoryPool&) = delete;

    [[nodiscard]] std::uint64_t limit() const noexcept { return limit_; }

    /* The bytes allocated and reserved now. */
    [[nodiscard]] std::uint64_t usage() const;

    /* How many reservations the pool has refused. The first is reported on
     * stderr, so a run says where its budget ended. */
    [[nodiscard]] std::uint64_t refusals() const noexcept
    {
        return refusals_.load(std::memory_order_relaxed);
    }

    /* How many reservations were refused although the reclaim hook had just
     * freed something: the refusals that cached memory did not explain. */
    [[nodiscard]] std::uint64_t refusalsAfterRelease() const noexcept
    {
        return refusalsAfterRelease_.load(std::memory_order_relaxed);
    }

    /* How many reservations did not fit until the reclaim hook freed
     * something, and then did. */
    [[nodiscard]] std::uint64_t reclaimed() const noexcept
    {
        return reclaimed_.load(std::memory_order_relaxed);
    }

    /* Sets the reclaim hook, or clears it with an empty function. Returns
     * once no call of the previous one is running. */
    void setReclaim(Reclaim reclaim);

    /* Charges `bytes` to the pool, or returns false and charges nothing when
     * the pool cannot hold them. A reservation that does not fit runs the
     * reclaim hook, if there is one, and is decided once more. */
    [[nodiscard]] bool reserve(std::uint64_t bytes);

    /* Returns a reservation of `bytes`. */
    void release(std::uint64_t bytes) noexcept;

private:
    [[nodiscard]] std::uint64_t charge(std::uint64_t bytes) const noexcept;
    [[nodiscard]] std::uint64_t usageLocked() const;
    /* Admits `charged` when it fits; `used` is the usage it was decided at. */
    [[nodiscard]] bool tryReserve(std::uint64_t charged, std::uint64_t& used);

    const std::uint64_t limit_;
    const std::function<std::uint64_t()> allocated_;
    const std::uint64_t granule_;
    mutable std::mutex mutex_;
    std::uint64_t reserved_ {};
    std::atomic<std::uint64_t> refusals_ {};
    std::atomic<std::uint64_t> refusalsAfterRelease_ {};
    std::atomic<std::uint64_t> reclaimed_ {};
    /* Held while the hook runs, which keeps it to one at a time and lets
     * setReclaim wait for a running one. Never held with mutex_. */
    std::mutex reclaimMutex_;
    Reclaim reclaim_;
};

} // namespace uml::gpu
