// SPDX-License-Identifier: GPL-2.0-or-later

#include "device_memory_pool.hpp"

#include <algorithm>
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

bool DeviceMemoryPool::reserve(std::uint64_t bytes)
{
    const std::uint64_t charged = charge(bytes);
    std::lock_guard lock(mutex_);
    const std::uint64_t used = usageLocked();
    if (charged > limit_ || used > limit_ - charged)
        return false;
    reserved_ += charged;
    return true;
}

void DeviceMemoryPool::release(std::uint64_t bytes) noexcept
{
    const std::uint64_t charged = charge(bytes);
    std::lock_guard lock(mutex_);
    reserved_ -= std::min(charged, reserved_);
}

} // namespace uml::gpu
