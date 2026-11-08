// SPDX-License-Identifier: GPL-2.0-or-later

/* The host GPU gives the guest a budget the pool enforces in bytes, and the
 * ledger says whose bytes they are. A Virgl resource is credited to every
 * context it is attached to and counted once in the total, a blob to the
 * context that made it, and a Venus context's device memory to the context
 * the renderer reports it for, without counting a Venus context's blobs
 * again. The guest kernel names the process that created a context, and the
 * snapshot adds up a process's contexts, leaves out the holders that are
 * active or small, and ranks the rest. The test drives the ledger with its
 * own clock and with the command sequences a guest sends. */

#include "context_memory_ledger.hpp"
#include "require.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using uml::gpu::ContextMemoryLedger;
using uml::gpu::parseContextOwner;
using uml::testing::require;
using Clock = ContextMemoryLedger::Clock;
using namespace std::chrono_literals;

namespace {

const Clock::time_point origin {Clock::duration {1'000'000'000'000}};
constexpr std::uint64_t mebibyte = 1U << 20;

const ContextMemoryLedger::Holder* find(
    const ContextMemoryLedger::Snapshot& snapshot, std::uint32_t pid)
{
    for (const auto& holder : snapshot.holders) {
        if (holder.pid == pid)
            return &holder;
    }
    return nullptr;
}

/* The device reserves a resource's room, then tells the ledger how big the
 * renderer made it. */
void made(
    ContextMemoryLedger& ledger, std::uint32_t resource, std::uint64_t bytes)
{
    ledger.reserveResource(resource);
    ledger.setResourceBytes(resource, bytes);
}

void madeBlob(
    ContextMemoryLedger& ledger,
    std::uint32_t resource,
    std::uint32_t context,
    std::uint64_t bytes)
{
    ledger.reserveResource(resource);
    ledger.blobMade(resource, context, bytes);
}

bool attach(
    ContextMemoryLedger& ledger, std::uint32_t context, std::uint32_t resource)
{
    return ledger.resourceAttached(context, resource);
}

ContextMemoryLedger::Snapshot all(
    const ContextMemoryLedger& ledger, Clock::time_point now)
{
    return ledger.snapshot(now, 100, Clock::duration::zero(), 0);
}

void namesAreParsedOnlyWhenTheKernelGaveAProcess()
{
    const auto named = parseContextOwner("1234:org.example.app");
    require(named && named->pid == 1234 && named->comm == "org.example.app",
        "a name with a process was not parsed");
    const auto colons = parseContextOwner("462:binder:462_1");
    require(colons && colons->pid == 462 && colons->comm == "binder:462_1",
        "a command name with a colon was cut");

    /* Every name a kernel without the topic gives is a thread's name. */
    for (const char* thread : {"RenderThread", "binder:462_1",
             "Chrome_InProcGp", "GrallocUploadTh", "", ":x", "0:x", "12:",
             "12", "x12:y", "-5:y", "99999999999:y", "4294967296:y"}) {
        require(!parseContextOwner(thread).has_value(),
            std::string("a thread name was taken for a process: ") + thread);
    }
    require(parseContextOwner("4294967295:max")->pid == 4294967295U,
        "the largest process ID was refused");
}

void aResourceIsCreditedToEveryContextThatHoldsIt()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, "10:allocator", false, origin);
    ledger.contextCreated(3, "20:app", false, origin);
    ledger.contextCreated(4, "30:compositor", false, origin);

    made(ledger, 100, 4 * mebibyte);
    require(ledger.totalBytes() == 0,
        "a resource no context holds counted in the total");
    attach(ledger, 2, 100);
    attach(ledger, 3, 100);
    attach(ledger, 3, 100);
    attach(ledger, 4, 100);
    auto snapshot = all(ledger, origin);
    require(ledger.totalBytes() == 4 * mebibyte &&
            snapshot.totalBytes == 4 * mebibyte,
        "a resource three contexts hold was counted more than once");
    for (const std::uint32_t pid : {10U, 20U, 30U}) {
        const auto* holder = find(snapshot, pid);
        require(holder && holder->bytes == 4 * mebibyte,
            "a context that holds the resource was not credited");
    }

    ledger.resourceDetached(2, 100);
    snapshot = all(ledger, origin);
    require(find(snapshot, 10) == nullptr,
        "a context that let go of a resource kept the credit");
    require(ledger.totalBytes() == 4 * mebibyte,
        "a resource held by two contexts left the total");
    ledger.resourceDetached(3, 100);
    ledger.resourceDetached(4, 100);
    require(ledger.totalBytes() == 0,
        "a resource no context holds stayed in the total");
    ledger.resourceDetached(4, 100);
    require(ledger.totalBytes() == 0, "a second detach changed the total");

    attach(ledger, 3, 100);
    ledger.resourceReleased(100);
    require(ledger.totalBytes() == 0 && all(ledger, origin).holders.empty(),
        "an unreferenced resource was still credited");
    attach(ledger, 3, 100);
    require(ledger.totalBytes() == 0,
        "a resource the guest destroyed was attached again");
}

void anAttachSaysWhetherItMadeTheContextAHolder()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, "10:a", false, origin);
    ledger.contextCreated(3, "20:b", false, origin);
    made(ledger, 100, mebibyte);

    require(attach(ledger, 2, 100), "the first attach did not hold");
    require(!attach(ledger, 2, 100), "a repeated attach held again");
    require(attach(ledger, 3, 100), "a second holder was refused");
    require(!attach(ledger, 3, 100), "a repeated second attach held again");
    require(!attach(ledger, 4, 100), "an unknown context held a resource");
    require(!attach(ledger, 2, 101), "an unknown resource was held");
    ledger.resourceDetached(3, 100);
    require(attach(ledger, 3, 100), "a detached context could not attach");
    require(ledger.totalBytes() == mebibyte, "an attach counted twice");
}

void storageThatArrivesAfterTheAttachIsCredited()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, "10:a", false, origin);
    ledger.contextCreated(3, "20:b", false, origin);
    ledger.reserveResource(100);
    ledger.reserveResource(100);
    require(attach(ledger, 2, 100) && attach(ledger, 3, 100), "setup");
    require(ledger.totalBytes() == 0, "a resource of no size counted");

    ledger.setResourceBytes(100, 6 * mebibyte);
    require(ledger.totalBytes() == 6 * mebibyte &&
            find(all(ledger, origin), 10)->bytes == 6 * mebibyte &&
            find(all(ledger, origin), 20)->bytes == 6 * mebibyte,
        "the size of a held resource was not credited to its holders");
    ledger.setResourceBytes(100, 2 * mebibyte);
    require(ledger.totalBytes() == 2 * mebibyte &&
            find(all(ledger, origin), 10)->bytes == 2 * mebibyte,
        "a new size was not rebased");
    ledger.setResourceBytes(101, 5);
    require(ledger.totalBytes() == 2 * mebibyte,
        "the size of an unknown resource counted");
    ledger.resourceReleased(100);
    require(ledger.totalBytes() == 0 && all(ledger, origin).holders.empty(),
        "a released resource kept its credit");

    /* A reservation the command gave back leaves nothing. */
    ledger.reserveResource(102);
    ledger.resourceReleased(102);
    require(!attach(ledger, 2, 102) && ledger.totalBytes() == 0,
        "a reservation that was given back held something");
    ledger.blobMade(102, 2, mebibyte);
    require(ledger.totalBytes() == 0, "a blob nobody reserved counted");
    madeBlob(ledger, 103, 2, mebibyte);
    ledger.blobMade(103, 3, 4 * mebibyte);
    require(ledger.totalBytes() == mebibyte,
        "a blob that was held already was made again");
}

void aContextThatGoesTakesItsCreditWithIt()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, "10:a", false, origin);
    ledger.contextCreated(3, "20:b", false, origin);
    made(ledger, 100, 8 * mebibyte);
    made(ledger, 101, 2 * mebibyte);
    attach(ledger, 2, 100);
    attach(ledger, 3, 100);
    attach(ledger, 2, 101);

    ledger.contextDestroyed(2);
    auto snapshot = all(ledger, origin);
    require(snapshot.totalBytes == 8 * mebibyte,
        "the resource only the destroyed context held stayed counted");
    require(find(snapshot, 20) && find(snapshot, 20)->bytes == 8 * mebibyte &&
            !find(snapshot, 10),
        "the survivor lost its credit or the dead context kept its own");
    ledger.contextDestroyed(3);
    require(ledger.totalBytes() == 0, "no context held anything");
    ledger.contextDestroyed(3);

    /* A context's ID may come back with another process. */
    ledger.contextCreated(2, "11:c", false, origin);
    require(all(ledger, origin).holders.empty(),
        "a new context started with the old one's credit");
}

void aBlobBelongsToItsCreatorUnlessTheCreatorIsVenus()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, "10:gl", false, origin);
    ledger.contextCreated(3, "20:vk", true, origin);

    madeBlob(ledger, 200, 2, 6 * mebibyte);
    require(ledger.totalBytes() == 6 * mebibyte &&
            find(all(ledger, origin), 10)->bytes == 6 * mebibyte,
        "a blob was not credited to the context that made it");

    madeBlob(ledger, 201, 3, 16 * mebibyte);
    require(ledger.totalBytes() == 6 * mebibyte,
        "a Venus context's blob was counted besides its device memory");
    ledger.venusMemory(3, static_cast<std::int64_t>(16 * mebibyte));
    require(ledger.totalBytes() == 22 * mebibyte,
        "the Venus context's device memory was not counted");

    /* A GL context that imports the Venus blob holds none of its bytes. */
    attach(ledger, 2, 201);
    require(find(all(ledger, origin), 10)->bytes == 6 * mebibyte,
        "an importer of a Venus blob was credited with its size");
    ledger.resourceReleased(200);
    ledger.resourceReleased(201);
    require(ledger.totalBytes() == 16 * mebibyte,
        "released blobs left bytes behind");
}

void venusMemoryFollowsTheRenderersReports()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(5, "40:game", true, origin);
    ledger.contextCreated(6, "41:other", true, origin);
    ledger.venusMemory(5, 64 * static_cast<std::int64_t>(mebibyte));
    ledger.venusMemory(5, 32 * static_cast<std::int64_t>(mebibyte));
    ledger.venusMemory(6, 8 * static_cast<std::int64_t>(mebibyte));
    ledger.venusMemory(7, 500 * static_cast<std::int64_t>(mebibyte));
    require(ledger.totalBytes() == 104 * mebibyte,
        "a report for a context the ledger never saw was counted");

    ledger.venusMemory(5, -64 * static_cast<std::int64_t>(mebibyte));
    require(find(all(ledger, origin), 40)->bytes == 32 * mebibyte,
        "freed device memory was not subtracted");
    ledger.venusMemory(5, -1000 * static_cast<std::int64_t>(mebibyte));
    require(find(all(ledger, origin), 40) == nullptr &&
            ledger.totalBytes() == 8 * mebibyte,
        "a free larger than the holding went below zero");
    ledger.venusMemory(5, INT64_MIN);
    require(ledger.totalBytes() == 8 * mebibyte,
        "the most negative delta was not clamped");

    ledger.venusMemory(6, 100 * static_cast<std::int64_t>(mebibyte));
    ledger.contextDestroyed(6);
    require(ledger.totalBytes() == 0,
        "a destroyed Venus context's device memory stayed counted");
    ledger.venusMemory(6, -8 * static_cast<std::int64_t>(mebibyte));
    require(ledger.totalBytes() == 0,
        "a free that arrived after its context went was counted");
}

void aProcessIsTheSumOfItsContexts()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, "77:app", false, origin);
    ledger.contextCreated(3, "77:app", true, origin);
    ledger.contextCreated(4, "RenderThread", false, origin);
    ledger.contextCreated(5, "RenderThread", false, origin);
    made(ledger, 100, 10 * mebibyte);
    made(ledger, 101, 5 * mebibyte);
    made(ledger, 102, 3 * mebibyte);
    made(ledger, 103, 2 * mebibyte);
    attach(ledger, 2, 100);
    attach(ledger, 3, 101);
    ledger.venusMemory(3, 20 * static_cast<std::int64_t>(mebibyte));
    attach(ledger, 4, 102);
    attach(ledger, 5, 103);

    const auto snapshot = all(ledger, origin);
    require(snapshot.holders.size() == 3,
        "a process and two unnamed contexts were not three holders");
    const auto* app = find(snapshot, 77);
    require(app && app->contexts == 2 && app->bytes == 35 * mebibyte &&
            app->name == "app" && app->contextId == 3,
        "a process was not the sum of its contexts");
    require(snapshot.holders[0].pid == 77 &&
            snapshot.holders[1].bytes == 3 * mebibyte &&
            snapshot.holders[1].pid == 0 && snapshot.holders[1].contextId == 4 &&
            snapshot.holders[1].name == "RenderThread" &&
            snapshot.holders[2].bytes == 2 * mebibyte,
        "the holders were not ranked by bytes with unnamed contexts apart");
    require(snapshot.totalBytes == 40 * mebibyte, "the total was wrong");
}

void aHolderIsIdleWhenItsMostRecentContextIs()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, "50:a", false, origin);
    ledger.contextCreated(3, "50:a", false, origin);
    ledger.contextCreated(4, "51:b", false, origin);
    for (std::uint32_t id : {2U, 3U, 4U}) {
        made(ledger, 100 + id, 4 * mebibyte);
        attach(ledger, id, 100 + id);
    }
    ledger.contextActive(3, origin + 8s);
    ledger.contextActive(2, origin + 3s);
    ledger.contextActive(3, origin + 6s);
    ledger.contextActive(99, origin + 6s);

    const auto now = origin + 10s;
    const auto snapshot = ledger.snapshot(now, 10, 5s, 0);
    require(snapshot.holders.size() == 1 && snapshot.holders[0].pid == 51,
        "an active process was offered as idle");
    require(snapshot.holders[0].idle == 10s, "the idle time was wrong");
    const auto unfiltered = all(ledger, now);
    require(find(unfiltered, 50)->idle == 2s,
        "a process was not as idle as its most recent context");
    require(ledger.snapshot(now, 10, 2s, 0).holders.size() == 2 &&
            ledger.snapshot(now, 10, 11s, 0).holders.empty(),
        "the idle bound did not include exactly the quiet holders");
    require(ledger.snapshot(origin, 10, 0s, 0).holders.size() == 2 &&
            find(ledger.snapshot(origin, 10, 0s, 0), 50)->idle == 0s,
        "a clock that ran behind made a negative idle time");
}

void theSnapshotKeepsTheLargestHoldersThatAreBigEnough()
{
    ContextMemoryLedger ledger;
    for (std::uint32_t id = 1; id <= 6; ++id) {
        ledger.contextCreated(id, std::to_string(id * 10) + ":p", false, origin);
        made(ledger, id, id * mebibyte);
        attach(ledger, id, id);
    }
    ledger.contextCreated(9, "90:empty", false, origin);

    auto snapshot = ledger.snapshot(origin, 3, 0s, 0);
    require(snapshot.holders.size() == 3 && snapshot.holders[0].pid == 60 &&
            snapshot.holders[1].pid == 50 && snapshot.holders[2].pid == 40,
        "the largest holders were not kept in order");
    snapshot = ledger.snapshot(origin, 10, 0s, 4 * mebibyte);
    require(snapshot.holders.size() == 3 &&
            snapshot.holders.back().bytes == 4 * mebibyte,
        "a holder below the size bound was offered");
    require(snapshot.totalBytes == 21 * mebibyte,
        "the filters changed the total");
    require(ledger.snapshot(origin, 0, 0s, 0).holders.empty(),
        "a request for no holders returned some");
}

void clearForgetsTheGuest()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, "10:a", true, origin);
    made(ledger, 100, mebibyte);
    attach(ledger, 2, 100);
    ledger.venusMemory(2, 9);
    require(ledger.totalBytes() == mebibyte + 9, "setup");
    ledger.clear();
    require(ledger.totalBytes() == 0 && all(ledger, origin).holders.empty(),
        "a cleared ledger remembered a context");
    attach(ledger, 2, 100);
    ledger.venusMemory(2, 9);
    require(ledger.totalBytes() == 0, "a cleared ledger remembered a resource");
}

/* The renderer's threads report Venus memory while the transport thread
 * works the command stream and a statistics thread reads. */
void threadsMeetOnOneLedger()
{
    ContextMemoryLedger ledger;
    ledger.contextCreated(2, "10:a", true, origin);
    ledger.contextCreated(3, "11:b", false, origin);
    std::atomic_bool stop {false};
    std::thread reader([&] {
        while (!stop.load()) {
            const auto snapshot = all(ledger, origin);
            require(snapshot.totalBytes % mebibyte == 0,
                "a reader saw half of an update");
        }
    });
    std::thread renderer([&] {
        for (int index = 0; index < 20000; ++index) {
            ledger.venusMemory(2, mebibyte);
            ledger.venusMemory(2, -static_cast<std::int64_t>(mebibyte));
        }
    });
    for (std::uint32_t id = 100; id < 3100; ++id) {
        made(ledger, id, mebibyte);
        attach(ledger, 3, id);
        ledger.contextActive(3, origin);
        if (id % 2 == 0)
            ledger.resourceReleased(id);
        else
            ledger.resourceDetached(3, id);
    }
    renderer.join();
    stop.store(true);
    reader.join();
    require(ledger.totalBytes() == 0, "the threads left bytes behind");
}

} // namespace

int main()
{
    namesAreParsedOnlyWhenTheKernelGaveAProcess();
    aResourceIsCreditedToEveryContextThatHoldsIt();
    anAttachSaysWhetherItMadeTheContextAHolder();
    storageThatArrivesAfterTheAttachIsCredited();
    aContextThatGoesTakesItsCreditWithIt();
    aBlobBelongsToItsCreatorUnlessTheCreatorIsVenus();
    venusMemoryFollowsTheRenderersReports();
    aProcessIsTheSumOfItsContexts();
    aHolderIsIdleWhenItsMostRecentContextIs();
    theSnapshotKeepsTheLargestHoldersThatAreBigEnough();
    clearForgetsTheGuest();
    threadsMeetOnOneLedger();
    return 0;
}
