// SPDX-License-Identifier: GPL-2.0-or-later

#include "device_memory_pool.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <utility>

namespace uml::gpu {

DeviceMemoryPool::DeviceMemoryPool(std::uint64_t limit,
    std::function<std::uint64_t()> allocated,
    std::uint64_t granule)
    : limit_(limit)
    , allocated_(std::move(allocated))
    , granule_(std::max<std::uint64_t>(granule, 1))
{
}

std::uint64_t DeviceMemoryPool::charge(std::uint64_t bytes) const noexcept
{
    const std::uint64_t remainder = bytes % granule_;
    if (remainder == 0)
        return bytes;
    const std::uint64_t padding = granule_ - remainder;
    return bytes > std::numeric_limits<std::uint64_t>::max() - padding
        ? std::numeric_limits<std::uint64_t>::max()
        : bytes + padding;
}

std::uint64_t DeviceMemoryPool::usageLocked() const
{
    /* Read under the lock, so that a reservation released after `allocated`
     * came to count its allocation is counted once or twice, never not at
     * all. */
    const std::uint64_t allocated = allocated_();
    return allocated > std::numeric_limits<std::uint64_t>::max() - reserved_
        ? std::numeric_limits<std::uint64_t>::max()
        : allocated + reserved_;
}

std::uint64_t DeviceMemoryPool::usage() const
{
    std::lock_guard lock(mutex_);
    return usageLocked();
}

bool DeviceMemoryPool::tryReserve(std::uint64_t charged, std::uint64_t& used)
{
    std::lock_guard lock(mutex_);
    used = usageLocked();
    if (charged > limit_ || used > limit_ - charged)
        return false;
    reserved_ += charged;
    return true;
}

void DeviceMemoryPool::setReclaim(Reclaim reclaim)
{
    std::lock_guard lock(reclaimMutex_);
    reclaim_ = std::move(reclaim);
}

bool DeviceMemoryPool::reserve(std::uint64_t bytes)
{
    const std::uint64_t charged = charge(bytes);
    std::uint64_t used = 0;
    if (tryReserve(charged, used))
        return true;

    /* A request larger than the pool is no cache's doing. Otherwise free what
     * the host drivers cache, with no lock of the pool held: freeing returns
     * reservations to it, and the memory the drivers free lowers the usage
     * that the decision below reads again. */
    std::uint64_t released = 0;
    if (charged <= limit_) {
        std::lock_guard lock(reclaimMutex_);
        if (reclaim_) {
            released = reclaim_();
            if (tryReserve(charged, used)) {
                if (released != 0)
                    reclaimed_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
    }

    const bool first = refusals_.fetch_add(1, std::memory_order_relaxed) == 0;
    if (released != 0)
        refusalsAfterRelease_.fetch_add(1, std::memory_order_relaxed);
    if (first)
        std::fprintf(stderr,
            "uml: the GPU budget is full: it refused %llu bytes with "
            "%llu of %llu bytes in use%s\n",
            static_cast<unsigned long long>(charged),
            static_cast<unsigned long long>(used),
            static_cast<unsigned long long>(limit_),
            released != 0 ? " after releasing cached memory" : "");
    return false;
}

void DeviceMemoryPool::release(std::uint64_t bytes) noexcept
{
    const std::uint64_t charged = charge(bytes);
    std::lock_guard lock(mutex_);
    reserved_ -= std::min(charged, reserved_);
}

} // namespace uml::gpu
