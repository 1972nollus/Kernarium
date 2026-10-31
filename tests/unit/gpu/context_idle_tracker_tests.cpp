// SPDX-License-Identifier: GPL-2.0-or-later

/* The renderer trims a guest context once a whole idle interval has passed
 * since its last activity and no fence it asked for is outstanding. The
 * tracker decides that from the times it is told, so the test drives it with
 * its own clock: a context is reported once, activity of any kind makes it
 * eligible again, a fence outstanding holds it back however long it takes, a
 * retired fence starts the interval, contexts the tracker was not told of are
 * ignored, and the delay it names is the time to the next context that could
 * be reported. */

#include "context_idle_tracker.hpp"
#include "require.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

using uml::gpu::ContextIdleTracker;
using uml::testing::require;
using Clock = ContextIdleTracker::Clock;
using namespace std::chrono_literals;

namespace {

const Clock::time_point origin {Clock::duration {1'000'000'000'000}};
constexpr std::uint32_t first = 3;
constexpr std::uint32_t second = 9;
const std::vector<std::uint32_t> none;

void checkIdleInterval()
{
    ContextIdleTracker tracker(1s);
    tracker.add(first, origin);

    require(tracker.takeIdle(origin + 999ms) == none,
        "a context was idle before the interval had passed");
    require(tracker.nextDelay(origin) == 1s,
        "the delay was not the whole interval right after creation");
    require(tracker.nextDelay(origin + 400ms) == 600ms,
        "the delay did not shrink with time");

    require(tracker.takeIdle(origin + 1s) == std::vector {first},
        "a context was not idle after the interval");
    require(tracker.takeIdle(origin + 10s) == none,
        "a context was reported twice");
    require(!tracker.nextDelay(origin + 10s).has_value(),
        "a reported context still had a delay");
}

void checkActivityMakesTheContextEligibleAgain()
{
    ContextIdleTracker tracker(1s);
    tracker.add(first, origin);
    require(tracker.takeIdle(origin + 2s) == std::vector {first},
        "the first report is missing");

    tracker.activity(first, origin + 3s);
    require(tracker.nextDelay(origin + 3s) == 1s,
        "activity did not start a new interval");
    require(tracker.takeIdle(origin + 3999ms) == none,
        "a context was reported before its new interval ended");
    require(tracker.takeIdle(origin + 4s) == std::vector {first},
        "a context active again was not reported again");

    /* An old timestamp cannot move the last activity backwards. */
    tracker.activity(first, origin + 10s);
    tracker.activity(first, origin + 5s);
    require(tracker.takeIdle(origin + 10999ms) == none,
        "an earlier activity time shortened the interval");
}

void checkPendingFences()
{
    ContextIdleTracker tracker(1s);
    tracker.add(first, origin);
    tracker.fenceCreated(first, 0, 7, origin);
    require(tracker.takeIdle(origin + 60s) == none,
        "a context with a fence outstanding was reported");
    require(!tracker.nextDelay(origin + 60s).has_value(),
        "a context with a fence outstanding had a delay");

    /* The interval starts at the retirement, not at the submission. */
    tracker.fenceRetired(first, 0, 7, origin + 60s);
    require(tracker.nextDelay(origin + 60s) == 1s,
        "a retired fence did not start the interval");
    require(tracker.takeIdle(origin + 60999ms) == none,
        "a context was reported before the retired fence's interval");
    require(tracker.takeIdle(origin + 61s) == std::vector {first},
        "a context was not reported after its last fence retired");

    /* Rings are separate timelines, and a retired cookie retires all before. */
    tracker.fenceCreated(first, 1, 5, origin + 70s);
    tracker.fenceCreated(first, 2, 6, origin + 70s);
    tracker.fenceRetired(first, 1, 5, origin + 71s);
    require(tracker.takeIdle(origin + 90s) == none,
        "a context with a fence outstanding on another ring was reported");
    tracker.fenceCreated(first, 2, 9, origin + 71s);
    tracker.fenceRetired(first, 2, 9, origin + 72s);
    require(tracker.takeIdle(origin + 72999ms) == none,
        "the merged retirement did not start the interval");
    require(tracker.takeIdle(origin + 73s) == std::vector {first},
        "a retirement covering earlier cookies left a fence outstanding");

    /* A fence that retires inside the call that asked for it is recorded
     * retired first; it must not leave the context pending. */
    tracker.fenceRetired(first, 3, 11, origin + 80s);
    tracker.fenceCreated(first, 3, 11, origin + 80s);
    require(tracker.takeIdle(origin + 81s) == std::vector {first},
        "a fence recorded retired before created left the context pending");
}

void checkSeveralContexts()
{
    ContextIdleTracker tracker(1s);
    tracker.add(first, origin);
    tracker.add(second, origin + 600ms);
    require(tracker.nextDelay(origin) == 1s,
        "the delay was not the earliest context's");
    require(tracker.takeIdle(origin + 1s) == std::vector {first},
        "only the older context should have been reported");
    require(tracker.nextDelay(origin + 1s) == 600ms,
        "the delay did not move to the remaining context");
    require(tracker.takeIdle(origin + 1600ms) == std::vector {second},
        "the second context was not reported");

    tracker.activity(first, origin + 5s);
    tracker.activity(second, origin + 5s);
    require((tracker.takeIdle(origin + 6s) == std::vector {first, second}),
        "contexts were not reported together in id order");

    tracker.remove(first);
    tracker.activity(first, origin + 7s);
    tracker.fenceCreated(first, 0, 1, origin + 7s);
    tracker.fenceRetired(first, 0, 1, origin + 7s);
    require(tracker.takeIdle(origin + 60s) == none,
        "a removed context, or one never added, was reported");

    tracker.add(first, origin + 60s);
    tracker.reset();
    require(tracker.takeIdle(origin + 120s) == none &&
            !tracker.nextDelay(origin + 120s).has_value(),
        "a reset left a context behind");
}

void checkThreads()
{
    /* Retirements come from whichever thread polls the renderer while the
     * service thread asks for idle contexts. */
    ContextIdleTracker tracker(1ms);
    tracker.add(first, Clock::now());
    std::thread retiring([&] {
        for (std::uint64_t cookie = 1; cookie <= 20000; ++cookie) {
            tracker.fenceCreated(first, 0, cookie, Clock::now());
            tracker.fenceRetired(first, 0, cookie, Clock::now());
        }
    });
    for (int round = 0; round < 2000; ++round) {
        (void)tracker.takeIdle(Clock::now());
        (void)tracker.nextDelay(Clock::now());
    }
    retiring.join();
    std::this_thread::sleep_for(5ms);
    require(tracker.takeIdle(Clock::now()) == std::vector {first},
        "the context was not idle once the threads were done");
}

} // namespace

int main()
{
    checkIdleInterval();
    checkActivityMakesTheContextEligibleAgain();
    checkPendingFences();
    checkSeveralContexts();
    checkThreads();
    return EXIT_SUCCESS;
}
