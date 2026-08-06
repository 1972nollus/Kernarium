// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"

#include <cstdlib>
#include <limits>
#include <new>

namespace {

constexpr auto noScheduledFailure = std::numeric_limits<std::size_t>::max();
thread_local std::size_t allocationsBeforeFailure = noScheduledFailure;

[[nodiscard]] void* allocate(std::size_t size)
{
    if (allocationsBeforeFailure == 0) {
        allocationsBeforeFailure = noScheduledFailure;
        throw std::bad_alloc {};
    }
    if (allocationsBeforeFailure != noScheduledFailure)
        --allocationsBeforeFailure;
    if (size == 0)
        size = 1;
    if (void* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc {};
}

} // namespace

namespace uml::gpu::testing {

void failNextAllocation() noexcept
{
    allocationsBeforeFailure = 0;
}

void failAfterAllocations(std::size_t count) noexcept
{
    allocationsBeforeFailure = count;
}

void clearAllocationFailure() noexcept
{
    allocationsBeforeFailure = noScheduledFailure;
}

} // namespace uml::gpu::testing

void* operator new(std::size_t size)
{
    return allocate(size);
}

void* operator new[](std::size_t size)
{
    return allocate(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}
