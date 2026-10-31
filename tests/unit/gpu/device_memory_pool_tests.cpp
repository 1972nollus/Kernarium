// SPDX-License-Identifier: GPL-2.0-or-later
#include "device_memory_pool.hpp"
#include "require.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace {

using uml::testing::require;
using uml::gpu::DeviceMemoryPool;
using namespace std::chrono_literals;

constexpr std::uint64_t page = 16384;

/* What Metal's allocation counter would report: an allocation appears in it
 * only once its owner has made it. */
struct Counter {
    std::atomic<std::uint64_t> bytes {};
};

void admitsWhatFits()
{
    Counter counter;
    DeviceMemoryPool pool(100 * page, [&] { return counter.bytes.load(); },
        page);
    require(pool.reserve(60 * page), "a reservation that fits is admitted");
    require(pool.usage() == 60 * page, "a reservation counts in the usage");
    require(pool.refusals() == 0, "nothing has been refused yet");
    require(!pool.reserve(41 * page), "one that does not is refused");
    require(pool.usage() == 60 * page, "a refusal charges nothing");
    require(pool.refusals() == 1, "a refusal is counted");
    require(pool.reserve(40 * page), "one that fills the pool is admitted");
    require(!pool.reserve(1), "a full pool admits nothing");

    // The first allocation is made, which the counter now counts, and its
    // reservation is released: the pool is exactly as full.
    counter.bytes += 60 * page;
    pool.release(60 * page);
    require(pool.usage() == 100 * page, "a made allocation replaces its "
                                        "reservation");
    pool.release(40 * page);
    require(pool.usage() == 60 * page, "a failed allocation leaves nothing");

    // Metal charges whole pages, and so does the pool.
    counter.bytes = 0;
    require(pool.reserve(1), "a byte is admitted");
    require(pool.usage() == page, "a byte is charged a page");
    require(pool.reserve(99 * page), "the other pages are admitted");
    require(!pool.reserve(1), "no page is left for another byte");
    pool.release(1);
    pool.release(99 * page);
    require(pool.usage() == 0, "every reservation is returned");
    require(!pool.reserve(std::numeric_limits<std::uint64_t>::max()),
        "a reservation past the limit is refused without overflowing");
}

void growthLeavesTheRestToTheGuest()
{
    // A heap growth reserves and is made; a guest allocation of what the
    // pool has left is then admitted, and one page more is not.
    Counter counter;
    DeviceMemoryPool pool(256 * page, [&] { return counter.bytes.load(); },
        page);
    counter.bytes = 16 * page;
    require(pool.reserve(64 * page), "the heap's growth is admitted");
    counter.bytes += 64 * page;
    pool.release(64 * page);
    require(!pool.reserve(177 * page), "the guest cannot pass the limit");
    require(pool.reserve(176 * page), "the guest gets what is left");
}

void concurrentAllocationsNeverCrossTheLimit()
{
    /* Two allocations of 60 of 100 pages, which each fit alone and not
     * together, check the pool on two threads before either is made. The
     * counter shows neither, so only a reservation can tell each about the
     * other. The counter's first two reads each wait, for a fifth of a
     * second at most, for the other: a check that only reads the counter
     * lets both in, and the pool, which reads it under its lock, keeps the
     * second reader out until the first has reserved. */
    Counter counter;
    std::mutex readers_mutex;
    std::condition_variable readers_arrived;
    unsigned readers = 0;
    auto allocated = [&] {
        const std::uint64_t bytes = counter.bytes.load();
        std::unique_lock lock(readers_mutex);
        if (++readers <= 2) {
            readers_arrived.notify_all();
            readers_arrived.wait_for(lock, 200ms, [&] { return readers >= 2; });
        }
        return bytes;
    };
    DeviceMemoryPool pool(100 * page, allocated, page);
    std::atomic<unsigned> admitted {};
    auto allocate = [&] {
        if (!pool.reserve(60 * page))
            return;
        ++admitted;
        counter.bytes += 60 * page;
        pool.release(60 * page);
    };
    std::thread first(allocate);
    std::thread second(allocate);
    first.join();
    second.join();
    require(admitted == 1, "exactly one of two allocations that fit only "
                           "alone is admitted");
    require(counter.bytes <= pool.limit(), "together they stay within the "
                                           "limit");
}

void manyThreadsNeverCrossTheLimit()
{
    /* Eight threads allocate and free random sizes against a pool that holds
     * only a few of them at once. Each allocation appears in the counter
     * after its reservation, as Metal's does, and none may take the counter
     * past the limit. */
    Counter counter;
    DeviceMemoryPool pool(256 * page, [&] { return counter.bytes.load(); },
        page);
    std::atomic<std::uint64_t> highest {};
    std::atomic<unsigned> admitted {};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            std::minstd_rand random(t + 1);
            std::uniform_int_distribution<std::uint64_t> pages(1, 96);
            for (unsigned i = 0; i < 4000; ++i) {
                const std::uint64_t size = pages(random) * page;
                if (!pool.reserve(size))
                    continue;
                ++admitted;
                std::this_thread::yield();
                const std::uint64_t now = counter.bytes += size;
                std::uint64_t seen = highest.load();
                while (now > seen && !highest.compare_exchange_weak(seen, now))
                    ;
                pool.release(size);
                std::this_thread::yield();
                counter.bytes -= size;
            }
        });
    }
    for (std::thread& thread : threads)
        thread.join();
    require(admitted > 1000, "most allocations are admitted");
    require(highest <= pool.limit(), "no allocation passes the limit");
    require(pool.usage() == 0, "every reservation is returned");
}

void reclaimsBeforeRefusing()
{
    /* Half the pool is a cache the driver would free for the asking. A
     * reservation that does not fit asks it to, and is decided once more:
     * the hook's memory serves it. */
    Counter counter;
    DeviceMemoryPool pool(100 * page, [&] { return counter.bytes.load(); },
        page);
    counter.bytes = 70 * page;
    unsigned calls = 0;
    pool.setReclaim([&]() -> std::uint64_t {
        ++calls;
        counter.bytes = 20 * page;
        return 3;
    });
    require(pool.reserve(20 * page), "a reservation that fits needs no hook");
    require(calls == 0, "the hook is not called while reservations fit");
    pool.release(20 * page);
    require(pool.reserve(40 * page), "freed cache admits the reservation");
    require(calls == 1, "the hook ran once");
    require(pool.reclaimed() == 1, "the rescue is counted");
    require(pool.refusals() == 0, "nothing was refused");
    require(pool.usage() == 60 * page, "the reservation and the memory left");
    pool.release(40 * page);

    /* A cache that is gone leaves the next refusal to stand. */
    counter.bytes = 90 * page;
    pool.setReclaim([&]() -> std::uint64_t {
        ++calls;
        return 5;
    });
    require(!pool.reserve(20 * page), "released memory that is not enough "
                                      "still refuses");
    require(pool.refusals() == 1, "the refusal is counted");
    require(pool.refusalsAfterRelease() == 1,
        "a refusal that followed a release is counted apart");
    pool.setReclaim([&]() -> std::uint64_t {
        ++calls;
        return 0;
    });
    require(!pool.reserve(20 * page), "an empty cache refuses");
    require(pool.refusals() == 2 && pool.refusalsAfterRelease() == 1,
        "an empty release is not counted apart");

    /* A request no cache could serve does not ask. */
    const unsigned before = calls;
    require(!pool.reserve(101 * page), "a request past the limit is refused");
    require(calls == before, "the hook is not asked for the impossible");

    pool.setReclaim({});
    require(!pool.reserve(20 * page), "no hook, no rescue");
}

void hookRunsWithoutThePoolsLocks()
{
    /* The hook frees memory, which returns reservations to the pool and reads
     * its usage. Called under the pool's lock it would wait for itself; the
     * pool must also let a second thread's reservation proceed meanwhile. */
    Counter counter;
    DeviceMemoryPool pool(100 * page, [&] { return counter.bytes.load(); },
        page);
    counter.bytes = 80 * page;
    require(pool.reserve(10 * page), "a reservation to give back");
    std::atomic<bool> inside {};
    std::atomic<bool> proceed {};
    pool.setReclaim([&]() -> std::uint64_t {
        pool.release(10 * page);
        (void)pool.usage();
        inside = true;
        while (!proceed)
            std::this_thread::yield();
        counter.bytes = 0;
        return 1;
    });
    std::atomic<bool> rescued {};
    std::thread refused([&] { rescued = pool.reserve(50 * page); });
    while (!inside)
        std::this_thread::yield();
    /* The hook is running: the pool still answers a thread whose reservation
     * fits, without waiting for it. */
    counter.bytes = 0;
    require(pool.reserve(5 * page), "the pool answers while a hook runs");
    pool.release(5 * page);
    counter.bytes = 80 * page;
    proceed = true;
    refused.join();
    require(rescued, "the refused thread was admitted after the hook");
    pool.release(50 * page);
    pool.setReclaim({});
}

void setReclaimWaitsForARunningHook()
{
    Counter counter;
    DeviceMemoryPool pool(10 * page, [&] { return counter.bytes.load(); },
        page);
    counter.bytes = 10 * page;
    std::atomic<bool> inside {};
    std::atomic<bool> finished {};
    pool.setReclaim([&]() -> std::uint64_t {
        inside = true;
        std::this_thread::sleep_for(100ms);
        finished = true;
        return 0;
    });
    std::thread refused([&] { (void)pool.reserve(page); });
    while (!inside)
        std::this_thread::yield();
    pool.setReclaim({});
    require(finished, "clearing the hook waits for the call running it");
    refused.join();
}

} // namespace

int main()
{
    admitsWhatFits();
    growthLeavesTheRestToTheGuest();
    concurrentAllocationsNeverCrossTheLimit();
    manyThreadsNeverCrossTheLimit();
    reclaimsBeforeRefusing();
    hookRunsWithoutThePoolsLocks();
    setReclaimWaitsForARunningHook();
    std::puts("device memory pool: allocations that reserve first never "
              "pass the limit together");
    return 0;
}
