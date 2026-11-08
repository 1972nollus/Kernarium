// SPDX-License-Identifier: GPL-2.0-or-later

/* The host tells a guest reclaimer when its graphics memory budget is under
 * pressure. The evaluator decides from the figures it is given and the time it
 * is told: pressure needs the contexts' bytes, the smallest overhead seen and
 * a margin to reach the budget for a whole second, and ends after five calm
 * seconds; a refused allocation is an event of its own, at most once a second;
 * an unsubscribed evaluator does nothing; and a reply reports the state, so
 * what it carries needs no event. The test drives it with its own clock. */

#include "memory_pressure.hpp"
#include "require.hpp"

#include <chrono>
#include <cstdint>

using uml::gpu::MemoryPressure;
using uml::testing::require;
using Clock = MemoryPressure::Clock;
using Reading = MemoryPressure::Reading;
using namespace std::chrono_literals;

namespace {

const Clock::time_point origin {Clock::duration {1'000'000'000'000}};
constexpr std::uint64_t mebibyte = 1U << 20;
constexpr std::uint64_t budget = 600 * mebibyte;

/* A reading with `context` bytes held, and the pool `overhead` above them. */
Reading reading(
    std::uint64_t context, std::uint64_t overhead = 20 * mebibyte,
    std::uint64_t refusals = 0)
{
    return {
        .budgetBytes = budget,
        .usageBytes = context + overhead,
        .contextBytes = context,
        .refusals = refusals,
    };
}

/* The contexts' bytes at which the condition starts to hold, for an overhead
 * of 20 MiB: B less the margin less the overhead. */
constexpr std::uint64_t threshold = budget - 96 * mebibyte - 20 * mebibyte;

MemoryPressure subscribed(const Reading& first = reading(100 * mebibyte))
{
    MemoryPressure pressure;
    pressure.subscribe(first);
    return pressure;
}

void checkMarginAndSustain()
{
    auto pressure = subscribed();
    require(!pressure.evaluate(origin, reading(threshold - 1)),
        "one byte under the margin raised an event");
    require(!pressure.deadline().has_value(),
        "a calm evaluator named a deadline");

    require(!pressure.evaluate(origin + 1s, reading(threshold)),
        "reaching the margin raised the event at once");
    require(!pressure.pressure(), "pressure began before the sustain time");
    require(pressure.deadline() == origin + 2s,
        "the deadline was not the end of the sustain time");
    require(!pressure.evaluate(origin + 1s + 999ms, reading(threshold + mebibyte)),
        "an event came before the sustain time had passed");
    require(pressure.evaluate(origin + 2s, reading(threshold + mebibyte)),
        "no event came after the sustain time");
    require(pressure.pressure(), "pressure did not begin");
    require(!pressure.deadline().has_value(),
        "pressure that lasts named a deadline");
    require(!pressure.evaluate(origin + 3s, reading(budget)),
        "a second event came while pressure lasted");
}

void checkASpikeThatPassesIsNotReported()
{
    auto pressure = subscribed();
    require(!pressure.evaluate(origin, reading(threshold)), "early event");
    /* The condition drops before the sustain time: the clock starts over. */
    require(!pressure.evaluate(origin + 900ms, reading(threshold - mebibyte)),
        "dropping under the margin raised an event");
    require(!pressure.deadline().has_value(), "a dropped spike kept a deadline");
    require(!pressure.evaluate(origin + 950ms, reading(threshold)), "early event");
    require(!pressure.evaluate(origin + 1950ms - 1ms, reading(threshold)),
        "the sustain time was not counted from the second rise");
    require(pressure.evaluate(origin + 1950ms, reading(threshold)),
        "no event a sustain time after the second rise");
}

void checkCalmEndsPressure()
{
    auto pressure = subscribed();
    (void)pressure.evaluate(origin, reading(threshold));
    require(pressure.evaluate(origin + 1s, reading(threshold)), "pressure did not begin");

    require(!pressure.evaluate(origin + 10s, reading(threshold - mebibyte)),
        "dropping under the margin ended pressure at once");
    require(pressure.pressure(), "pressure ended before the calm time");
    require(pressure.deadline() == origin + 15s,
        "the deadline was not the end of the calm time");
    /* A rise within the calm time starts the calm time over. */
    require(!pressure.evaluate(origin + 14s, reading(threshold)),
        "a rise during the calm time raised an event");
    require(!pressure.deadline().has_value(), "pressure that returned kept a deadline");
    require(!pressure.evaluate(origin + 20s, reading(threshold - mebibyte)),
        "calm raised an event at once");
    require(!pressure.evaluate(origin + 25s - 1ms, reading(threshold - mebibyte)),
        "calm ended pressure early");
    require(pressure.evaluate(origin + 25s, reading(threshold - mebibyte)),
        "no event after the calm time");
    require(!pressure.pressure(), "pressure did not end");
    require(!pressure.deadline().has_value(), "a calm evaluator named a deadline");
}

void checkTheSmallestOverheadCounts()
{
    /* The overhead the evaluator starts with is the pool's at subscription:
     * 20 MiB. A reading that finds less lowers it, and one that finds more
     * does not raise it. */
    auto pressure = subscribed(reading(100 * mebibyte, 20 * mebibyte));
    require(pressure.overheadBytes() == 20 * mebibyte, "the first overhead was not kept");
    require(!pressure.evaluate(origin, reading(200 * mebibyte, 50 * mebibyte)),
        "a reading with more overhead raised an event");
    require(pressure.overheadBytes() == 20 * mebibyte, "a larger overhead was taken");
    require(!pressure.evaluate(origin, reading(200 * mebibyte, 5 * mebibyte)),
        "a reading with less overhead raised an event");
    require(pressure.overheadBytes() == 5 * mebibyte, "a smaller overhead was not taken");

    /* 5 MiB of overhead moves the threshold up by 15 MiB. */
    require(!pressure.evaluate(origin + 10s, reading(threshold + 14 * mebibyte, 5 * mebibyte)),
        "the condition held under the smallest overhead");
    require(!pressure.deadline().has_value(), "the condition held under the smallest overhead");
    (void)pressure.evaluate(origin + 20s, reading(threshold + 15 * mebibyte, 5 * mebibyte));
    require(pressure.deadline() == origin + 21s, "the condition did not hold at the moved threshold");

    /* Memory the contexts hold above the pool's usage is no overhead. */
    auto above = subscribed();
    (void)above.evaluate(origin, {.budgetBytes = budget, .usageBytes = 10 * mebibyte,
        .contextBytes = 30 * mebibyte});
    require(above.overheadBytes() == 0, "negative overhead was taken");
}

void checkBudgetEdges()
{
    /* No budget, no pressure. */
    auto none = subscribed();
    require(!none.evaluate(origin, {.budgetBytes = 0, .usageBytes = 1000 * mebibyte,
        .contextBytes = 1000 * mebibyte}), "pressure without a budget");
    require(!none.evaluate(origin + 1h, {.budgetBytes = 0, .usageBytes = 1000 * mebibyte,
        .contextBytes = 1000 * mebibyte}), "pressure without a budget");
    require(!none.pressure(), "pressure without a budget");

    /* A budget under the margin is always under pressure, and a sum that
     * would overflow is not a calm reading. */
    auto tiny = subscribed();
    (void)tiny.evaluate(origin, {.budgetBytes = 50 * mebibyte, .usageBytes = 0, .contextBytes = 0});
    require(tiny.evaluate(origin + 1s, {.budgetBytes = 50 * mebibyte}),
        "a budget under the margin was calm");
    auto huge = subscribed();
    (void)huge.evaluate(origin, {.budgetBytes = budget, .usageBytes = UINT64_MAX,
        .contextBytes = UINT64_MAX});
    require(huge.deadline() == origin + 1s, "an overflowing reading was calm");
}

void checkRefusalEdges()
{
    auto pressure = subscribed(reading(100 * mebibyte, 20 * mebibyte, 5));
    require(!pressure.evaluate(origin, reading(100 * mebibyte, 20 * mebibyte, 5)),
        "refusals before the subscription raised an event");

    require(pressure.evaluate(origin, reading(100 * mebibyte, 20 * mebibyte, 6)),
        "a refusal raised no event");
    require(!pressure.evaluate(origin + 1ms, reading(100 * mebibyte, 20 * mebibyte, 6)),
        "one refusal raised two events");
    /* A second refusal within the interval waits for it. */
    require(!pressure.evaluate(origin + 100ms, reading(100 * mebibyte, 20 * mebibyte, 9)),
        "refusals faster than the interval each raised an event");
    require(pressure.deadline() == origin + 1s, "the deadline was not the end of the interval");
    require(!pressure.evaluate(origin + 999ms, reading(100 * mebibyte, 20 * mebibyte, 12)),
        "an event came before the interval had passed");
    require(pressure.evaluate(origin + 1s, reading(100 * mebibyte, 20 * mebibyte, 12)),
        "the refusals held back raised no event after the interval");
    require(!pressure.deadline().has_value(), "a raised refusal kept a deadline");
    require(!pressure.evaluate(origin + 10s, reading(100 * mebibyte, 20 * mebibyte, 12)),
        "a refusal raised a second event");
}

void checkEventsOfOneEvaluationAreOne()
{
    auto pressure = subscribed();
    (void)pressure.evaluate(origin, reading(threshold));
    /* The pressure begins and an allocation is refused in the same reading:
     * one event, and the refusal is not held back for another. */
    require(pressure.evaluate(origin + 1s, reading(threshold, 20 * mebibyte, 1)),
        "no event for pressure with a refusal");
    require(!pressure.deadline().has_value(), "the refusal was held back after its event");
}

void checkUnsubscribedDoesNothing()
{
    MemoryPressure pressure;
    require(!pressure.subscribed(), "a new evaluator was subscribed");
    require(!pressure.evaluate(origin, reading(budget, 0, 7)), "an unsubscribed evaluator raised an event");
    require(!pressure.evaluate(origin + 1h, reading(budget, 0, 9)), "an unsubscribed evaluator raised an event");
    require(!pressure.deadline().has_value(), "an unsubscribed evaluator named a deadline");
    require(!pressure.pressure(), "an unsubscribed evaluator had pressure");

    pressure.subscribe(reading(budget, 0, 9));
    require(pressure.subscribed(), "subscribing did not subscribe");
    require(!pressure.evaluate(origin, reading(budget, 0, 9)), "the baseline refusals raised an event");
    (void)pressure.evaluate(origin, reading(budget, 0, 9));
    require(pressure.deadline() == origin + 1s, "the subscription ignored the held budget");

    pressure.unsubscribe();
    require(!pressure.subscribed(), "unsubscribing did not unsubscribe");
    require(!pressure.deadline().has_value(), "an unsubscribed evaluator kept a deadline");
    require(!pressure.evaluate(origin + 2s, reading(budget, 0, 11)), "an unsubscribed evaluator raised an event");
    require(!pressure.pressure(), "an unsubscribed evaluator kept the pressure");
}

void checkSubscribingAgainChangesNothing()
{
    auto pressure = subscribed();
    (void)pressure.evaluate(origin, reading(threshold));
    pressure.subscribe(reading(0, 0, 100));
    require(pressure.deadline() == origin + 1s, "subscribing again cleared the evaluation");
    require(pressure.overheadBytes() == 20 * mebibyte, "subscribing again took a new overhead");
    require(pressure.evaluate(origin + 1s, reading(threshold)), "subscribing again lost the sustain time");
}

void checkAReplyReportsTheState()
{
    auto pressure = subscribed(reading(100 * mebibyte, 20 * mebibyte, 2));
    auto state = pressure.query(origin, reading(100 * mebibyte, 20 * mebibyte, 2));
    require(!state.pressure && !state.refused, "a calm first reply was not calm");

    /* A refusal is reported once, and its event is not raised after. */
    state = pressure.query(origin + 10ms, reading(100 * mebibyte, 20 * mebibyte, 4));
    require(state.refused, "the reply did not report a refusal");
    require(!pressure.evaluate(origin + 20ms, reading(100 * mebibyte, 20 * mebibyte, 4)),
        "a refusal a reply reported raised an event");
    state = pressure.query(origin + 30ms, reading(100 * mebibyte, 20 * mebibyte, 4));
    require(!state.refused, "a refusal was reported twice");

    /* A reply reports a pressure that began since the last evaluation, with
     * no event. */
    (void)pressure.evaluate(origin + 1s, reading(threshold, 20 * mebibyte, 4));
    state = pressure.query(origin + 2s, reading(threshold, 20 * mebibyte, 4));
    require(state.pressure, "the reply did not report the pressure that began");
    require(pressure.pressure(), "the reply left the evaluator calm");
    require(!pressure.evaluate(origin + 2s, reading(threshold, 20 * mebibyte, 4)),
        "a pressure a reply reported raised an event");

    /* An unsubscribed query takes its baseline from the first reply. */
    MemoryPressure idle;
    state = idle.query(origin, reading(0, 0, 40));
    require(!state.refused, "the first reply of an unsubscribed query reported old refusals");
    state = idle.query(origin, reading(0, 0, 41));
    require(state.refused, "an unsubscribed query missed a refusal");
    require(!state.pressure, "an unsubscribed query reported pressure");
}

} // namespace

int main()
{
    checkMarginAndSustain();
    checkASpikeThatPassesIsNotReported();
    checkCalmEndsPressure();
    checkTheSmallestOverheadCounts();
    checkBudgetEdges();
    checkRefusalEdges();
    checkEventsOfOneEvaluationAreOne();
    checkUnsubscribedDoesNothing();
    checkSubscribingAgainChangesNothing();
    checkAReplyReportsTheState();
    return 0;
}
