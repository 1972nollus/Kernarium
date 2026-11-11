// SPDX-License-Identifier: GPL-2.0-or-later

// What a full virtio-input queue owes the guest. A report the queue drops
// can carry the lift of a contact or the release of a key, and the guest
// would hold it until something else released it. The queue therefore
// releases what the guest was told is held before the next report it
// accepts.

#include "held_input.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "touchscreen.hpp"
#include "virtio_input_device.hpp"
#include "wire_bytes.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using uml::input::virtio::DeviceKind;
using uml::input::virtio::EventQueue;
using uml::input::virtio::HeldInput;
using uml::input::virtio::Touchscreen;
using uml::input::virtio::VirtioInputDevice;
namespace protocol = uml::input::virtio::protocol;

using uml::testing::require;

using Events = std::vector<protocol::InputEvent>;

// EventQueue's limit, which the tests fill exactly.
constexpr std::size_t queueCapacity = 4096;

constexpr std::uint16_t eventRelative = 0x02;
constexpr std::uint16_t relativeX = 0x00;
constexpr std::uint16_t buttonLeft = 0x110;
constexpr std::uint16_t buttonRight = 0x111;
constexpr std::uint16_t keyA = 30;
constexpr std::uint16_t keyB = 48;

constexpr protocol::InputEvent synchronize()
{
    return {protocol::eventSynchronize, protocol::synchronizeReport, 0};
}

constexpr protocol::InputEvent key(std::uint16_t code, std::int32_t value)
{
    return {protocol::eventKey, code, value};
}

constexpr protocol::InputEvent absolute(
    std::uint16_t code, std::int32_t value)
{
    return {protocol::eventAbsolute, code, value};
}

constexpr protocol::InputEvent slot(std::int32_t index)
{
    return absolute(protocol::absoluteMtSlot, index);
}

constexpr protocol::InputEvent lift()
{
    return absolute(protocol::absoluteMtTrackingId, -1);
}

Events concat(std::initializer_list<Events> parts)
{
    Events all;
    for (const auto& part : parts)
        all.insert(all.end(), part.begin(), part.end());
    return all;
}

Events touchEvents(const uml::input::virtio::TouchscreenReport& report)
{
    return Events(report.span().begin(), report.span().end());
}

std::string describe(const protocol::InputEvent& event)
{
    char text[64];
    std::snprintf(
        text, sizeof(text), "{type %u code %u value %d}",
        static_cast<unsigned>(event.type),
        static_cast<unsigned>(event.code),
        static_cast<int>(event.value));
    return text;
}

// Compares two event streams and names the first difference.
void requireStream(
    const Events& got,
    const Events& want,
    const std::source_location where = std::source_location::current())
{
    const auto common = std::min(got.size(), want.size());
    for (std::size_t index = 0; index < common; ++index) {
        if (std::memcmp(&got[index], &want[index], sizeof(got[index])) != 0) {
            uml::testing::fail(
                "event " + std::to_string(index) + " is " +
                    describe(got[index]) + ", wanted " +
                    describe(want[index]),
                where);
        }
    }
    if (got.size() != want.size()) {
        uml::testing::fail(
            "the stream has " + std::to_string(got.size()) +
                " events, wanted " + std::to_string(want.size()),
            where);
    }
}

// What a guest's input core believes after the events it was given.
struct Guest final {
    std::bitset<0x300> keys;
    std::bitset<16> contacts;
    std::int32_t slot {};

    void apply(const protocol::InputEvent& event)
    {
        if (event.type == protocol::eventKey && event.code < keys.size()) {
            keys.set(event.code, event.value != 0);
        } else if (event.type == protocol::eventAbsolute &&
                   event.code == protocol::absoluteMtSlot) {
            slot = event.value;
        } else if (event.type == protocol::eventAbsolute &&
                   event.code == protocol::absoluteMtTrackingId &&
                   slot >= 0 &&
                   static_cast<std::size_t>(slot) < contacts.size()) {
            contacts.set(static_cast<std::size_t>(slot), event.value != -1);
        }
    }

    void apply(const Events& events)
    {
        for (const auto& event : events)
            apply(event);
    }

    [[nodiscard]] bool idle() const
    {
        return keys.none() && contacts.none();
    }
};

// An EventQueue and a count of what it holds, which lets a test fill it
// exactly: a report goes in whole or not at all.
class Harness final {
public:
    EventQueue queue;

    // A report the queue has room for.
    void send(const Events& events)
    {
        require(events.size() <= queueCapacity - queued_);
        queue.enqueue(events);
        queued_ += events.size();
    }

    // A report the queue has no room for: it is dropped.
    void reject(const Events& events)
    {
        require(events.size() > queueCapacity - queued_);
        queue.enqueue(events);
    }

    // Fills the queue with empty reports, up to its capacity.
    void fill()
    {
        while (queued_ < queueCapacity)
            send(Events {synchronize()});
    }

    Events take(std::size_t count)
    {
        Events taken;
        protocol::InputEvent event {};
        while (taken.size() < count && queue.take(event))
            taken.push_back(event);
        require(taken.size() == count);
        queued_ -= count;
        return taken;
    }

    Events drainAll()
    {
        Events taken;
        protocol::InputEvent event {};
        while (queue.take(event))
            taken.push_back(event);
        queued_ = 0;
        return taken;
    }

    void forget()
    {
        queued_ = 0;
    }

private:
    std::size_t queued_ {};
};

// Standard error, captured for the test's duration.
class CapturedStderr final {
public:
    CapturedStderr()
        : file_(std::tmpfile())
        , saved_(::dup(STDERR_FILENO))
    {
        require(file_ != nullptr && saved_ >= 0);
        std::fflush(stderr);
        require(::dup2(::fileno(file_), STDERR_FILENO) >= 0);
    }

    CapturedStderr(const CapturedStderr&) = delete;
    CapturedStderr& operator=(const CapturedStderr&) = delete;

    std::string finish()
    {
        std::fflush(stderr);
        require(::dup2(saved_, STDERR_FILENO) >= 0);
        ::close(saved_);
        std::string text;
        std::rewind(file_);
        std::array<char, 256> buffer {};
        std::size_t count = 0;
        while ((count = std::fread(buffer.data(), 1, buffer.size(), file_)) !=
               0) {
            text.append(buffer.data(), count);
        }
        std::fclose(file_);
        return text;
    }

private:
    std::FILE* file_;
    int saved_;
};

std::size_t occurrences(const std::string& text, const char* needle)
{
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

// The report that lifts a lone contact in slot 0 and nothing else.
Events liftOfSlotZero()
{
    return {key(protocol::buttonTouch, 0), slot(0), lift(), synchronize()};
}

void aDroppedLiftIsReleasedBeforeTheNextReport()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.fill();
    h.reject(touchEvents(screen.touch(0, 100, 200, false)));

    // The guest reads everything the queue held and still has the touch.
    const auto stale = h.drainAll();
    require(stale.size() == queueCapacity);
    Guest guest;
    guest.apply(stale);
    require(guest.contacts.test(0));
    require(guest.keys.test(protocol::buttonTouch));

    const auto next = touchEvents(screen.touch(0, 300, 400, true));
    h.send(next);
    const auto after = h.drainAll();
    requireStream(after, concat({liftOfSlotZero(), next}));

    guest.apply(Events(after.begin(), after.begin() + 4));
    require(guest.idle());
    guest.apply(Events(after.begin() + 4, after.end()));
    require(guest.contacts.test(0));
}

void aDroppedMoveStillReleasesTheContactItHeld()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(1, 100, 200, true)));
    h.fill();
    h.reject(touchEvents(screen.touch(1, 150, 250, true)));
    Guest guest;
    guest.apply(h.drainAll());
    require(guest.contacts.test(1));

    // The contact stays down on the host. The guest is told it lifted
    // before the report that follows, which moves it.
    const auto move = touchEvents(screen.touch(1, 500, 600, true));
    h.send(move);
    const Events released {
        key(protocol::buttonTouch, 0), slot(1), lift(), synchronize()};
    const auto after = h.drainAll();
    requireStream(after, concat({released, move}));
    // The move names no tracking ID, so it does not start the contact
    // again; the report still sets BTN_TOUCH, which the contact's real lift
    // clears.
    guest.apply(Events(after.begin(), after.begin() + released.size()));
    require(guest.idle());
    guest.apply(move);
    require(guest.contacts.none());

    // The slot takes a new touch again, which is a new contact.
    (void)screen.touch(1, 500, 600, false);
    guest.apply(touchEvents(screen.touch(1, 10, 20, true)));
    require(guest.contacts.test(1));
}

void severalContactsAreAllReleasedInSlotOrder()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 100, true)));
    h.send(touchEvents(screen.touch(3, 200, 200, true)));
    h.send(touchEvents(screen.touch(7, 300, 300, true)));
    // The guest is told slot 3 lifts, and its current slot is then 3.
    h.send(touchEvents(screen.touch(3, 200, 200, false)));
    h.fill();
    h.reject(touchEvents(screen.touch(0, 100, 100, false)));
    h.reject(touchEvents(screen.touch(7, 310, 310, true)));

    Guest guest;
    guest.apply(h.drainAll());
    require(guest.contacts.test(0));
    require(!guest.contacts.test(3));
    require(guest.contacts.test(7));
    require(guest.slot == 3);

    const auto next = touchEvents(screen.touch(3, 50, 60, true));
    h.send(next);
    // BTN_TOUCH is held until the last contact lifts. The slot the guest
    // had selected is selected again, so the report that follows reads as
    // it was written.
    const Events releases {
        key(protocol::buttonTouch, 0),
        slot(0), lift(),
        slot(7), lift(),
        slot(3),
        synchronize(),
    };
    const auto after = h.drainAll();
    requireStream(after, concat({releases, next}));

    guest = {};
    guest.apply(Events(after.begin(), after.begin() + releases.size()));
    require(guest.idle());
    require(guest.slot == 3);
}

void aDroppedKeyReleaseReachesTheGuestThroughTheDevice()
{
    auto queue = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::keyboard, queue);
    const auto readAll = [&device] {
        Events events;
        for (;;) {
            const auto result =
                device.processQueue(protocol::eventQueue, 1, {}, 8);
            if (result.action != uml::virtio::VhostUserQueueAction::complete)
                return events;
            events.push_back(
                uml::testing::loadExact<protocol::InputEvent>(
                    result.response));
        }
    };

    queue->enqueue(Events {key(keyA, 1), synchronize()});
    for (std::size_t queued = 2; queued < queueCapacity; ++queued)
        queue->enqueue(Events {synchronize()});
    queue->enqueue(Events {key(keyA, 0), synchronize()}); // dropped

    Guest guest;
    const auto stale = readAll();
    require(stale.size() == queueCapacity);
    guest.apply(stale);
    require(guest.keys.test(keyA));

    queue->enqueue(Events {key(keyB, 1), synchronize()});
    const auto after = readAll();
    requireStream(
        after,
        {key(keyA, 0), synchronize(), key(keyB, 1), synchronize()});
    guest.apply(Events(after.begin(), after.begin() + 2));
    require(guest.idle());
}

void aDroppedMouseButtonReleaseIsReleasedWithoutTouchKnowledge()
{
    // A mouse sends relative motion and buttons, and no contacts.
    Harness h;
    h.send({key(buttonLeft, 1), synchronize()});
    h.send({key(buttonRight, 1), synchronize()});
    h.send({key(buttonRight, 0), synchronize()});
    h.send({{eventRelative, relativeX, 5}, synchronize()});
    h.fill();
    h.reject({key(buttonLeft, 0), synchronize()});

    Guest guest;
    guest.apply(h.drainAll());
    require(guest.keys.test(buttonLeft));
    require(!guest.keys.test(buttonRight));

    const Events motion {{eventRelative, relativeX, -3}, synchronize()};
    h.send(motion);
    const auto after = h.drainAll();
    requireStream(
        after,
        concat({Events {key(buttonLeft, 0), synchronize()}, motion}));
}

void everyHeldKeyIsReleasedInCodeOrder()
{
    Harness h;
    h.send({key(keyB, 1), synchronize()});
    h.send({key(keyA, 1), synchronize()});
    h.send({key(keyB, 2), synchronize()}); // autorepeat: still held
    h.send({key(buttonLeft, 1), synchronize()});
    h.send({key(keyA, 0), synchronize()});
    h.send({key(keyA, 1), synchronize()});
    h.fill();
    h.reject({key(keyB, 0), synchronize()});
    (void)h.drainAll();

    const Events next {key(keyA, 0), synchronize()};
    h.send(next);
    requireStream(
        h.drainAll(),
        concat({
            Events {
                key(keyA, 0), key(keyB, 0), key(buttonLeft, 0),
                synchronize()},
            next}));
}

void nothingIsSynthesizedWithoutAnOverflow()
{
    Harness h;
    Touchscreen screen;
    Events sent;
    const auto push = [&](const Events& events) {
        h.send(events);
        sent.insert(sent.end(), events.begin(), events.end());
    };
    const auto pushReport = [&](const auto& report) {
        push(touchEvents(report));
    };
    pushReport(screen.touch(0, 100, 200, true));
    pushReport(screen.touch(0, 110, 210, true));
    pushReport(screen.touch(1, 300, 400, true));
    push({key(keyA, 1), synchronize()});
    push({key(buttonLeft, 1), synchronize()});
    pushReport(screen.touch(0, 110, 210, false));
    push({key(keyA, 0), synchronize()});
    push({key(buttonLeft, 0), synchronize()});
    pushReport(screen.touch(1, 300, 400, false));
    pushReport(screen.touch(0, 5, 6, true));
    requireStream(h.drainAll(), sent);
}

void anOverflowWithNothingHeldSynthesizesNothing()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.send(touchEvents(screen.touch(0, 100, 200, false)));
    h.send({key(keyA, 1), synchronize()});
    h.send({key(keyA, 0), synchronize()});
    h.fill();
    h.reject({{eventRelative, relativeX, 1}, synchronize()});
    require(h.drainAll().size() == queueCapacity);

    const Events next {key(keyB, 1), synchronize()};
    h.send(next);
    requireStream(h.drainAll(), next);
}

void theReleaseWaitsForRoomForItAndTheReport()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.fill();
    h.reject(touchEvents(screen.touch(0, 100, 200, false)));

    // The release is four events and the report two, so six must be free.
    // A queue that kept the report alone would take it at four.
    const Events report {absolute(protocol::absoluteX, 77), synchronize()};
    (void)h.take(4);
    h.queue.enqueue(report);
    (void)h.take(1);
    h.queue.enqueue(report);
    (void)h.take(1);
    h.queue.enqueue(report);

    const auto rest = h.drainAll();
    require(rest.size() == queueCapacity);
    requireStream(
        Events(rest.end() - 6, rest.end()),
        concat({liftOfSlotZero(), report}));
}

void clearForgetsAPendingRelease()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.fill();
    h.reject(touchEvents(screen.touch(0, 100, 200, false)));

    // The guest reset: it holds nothing, whatever it was told before.
    h.queue.clear();
    h.forget();
    const auto next = touchEvents(screen.touch(0, 300, 400, true));
    h.send(next);
    requireStream(h.drainAll(), next);
}

void clearForgetsWhatTheGuestHeld()
{
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.send({key(keyA, 1), synchronize()});
    h.queue.clear();
    h.forget();

    h.fill();
    h.reject({synchronize()});
    require(h.drainAll().size() == queueCapacity);
    const Events next {key(keyB, 1), synchronize()};
    h.send(next);
    requireStream(h.drainAll(), next);
}

void theOverflowIsReportedOnce()
{
    CapturedStderr captured;
    Harness h;
    Touchscreen screen;
    h.send(touchEvents(screen.touch(0, 100, 200, true)));
    h.fill();
    h.reject(touchEvents(screen.touch(0, 100, 200, false)));
    h.reject({key(keyA, 1), synchronize()});
    (void)h.drainAll();

    // The queue releases the contact, then fills and overflows again.
    h.send(touchEvents(screen.touch(0, 300, 400, true)));
    (void)h.drainAll();
    h.fill();
    h.reject(touchEvents(screen.touch(0, 300, 400, false)));
    h.reject(touchEvents(screen.touch(1, 300, 400, true)));
    const auto text = captured.finish();
    require(occurrences(
                text,
                "dropping input because the virtio queue is full") == 1);
}

Events releaseOf(HeldInput& held)
{
    std::deque<protocol::InputEvent> events;
    held.appendRelease(events);
    return Events(events.begin(), events.end());
}

constexpr protocol::InputEvent track(std::int32_t id)
{
    return absolute(protocol::absoluteMtTrackingId, id);
}

void heldInputReleasesOnlyWhatWasToldHeld()
{
    HeldInput held;
    require(!held.any());
    require(held.releaseSize() == 0);
    require(releaseOf(held).empty());

    held.observe(Events {
        key(keyA, 1), synchronize(),
        key(keyB, 1), synchronize(),
        key(keyA, 0), synchronize(),
        slot(2), track(41), synchronize(),
        slot(5), track(42), synchronize(),
        slot(2), track(-1), synchronize(),
    });
    require(held.any());
    require(held.releaseSize() == 1 + 2 + 1 + 1);
    // The guest selected slot 2 last, and slot 5 is the one released last.
    requireStream(
        releaseOf(held),
        {key(keyB, 0), slot(5), lift(), slot(2), synchronize()});
    require(!held.any());
    require(releaseOf(held).empty());
}

void heldInputReleaseSizeIsTheEventsAppended()
{
    const std::array<Events, 7> histories {{
        {key(keyA, 1), synchronize()},
        {slot(0), track(1), synchronize()},
        {slot(3), track(1), synchronize()},
        {slot(1), track(1), slot(4), track(2), slot(1), synchronize()},
        {slot(9), track(1), slot(8), track(2), key(protocol::buttonTouch, 1),
         key(keyB, 1), key(buttonLeft, 1), synchronize()},
        // A slot the guest selects beyond the tracked range is restored, and
        // a contact in it is not tracked.
        {slot(2), track(1), slot(40), track(2), synchronize()},
        // A negative slot is no slot, and nothing is restored.
        {slot(2), track(1), slot(-1), track(2), synchronize()},
    }};
    for (const auto& history : histories) {
        HeldInput held;
        held.observe(history);
        const auto size = held.releaseSize();
        require(size != 0);
        require(releaseOf(held).size() == size);
    }
}

void heldInputIgnoresWhatAGuestDoesNotTake()
{
    HeldInput held;
    held.observe(Events {
        key(HeldInput::keyCount, 1),
        key(0xffff, 1),
        slot(static_cast<std::int32_t>(HeldInput::slotCount)), track(7),
        slot(-1), track(7),
        // A position or a tool type does not hold a contact.
        slot(0),
        absolute(protocol::absoluteMtPositionX, 5),
        absolute(protocol::absoluteMtToolType, protocol::toolTypePalm),
        {eventRelative, relativeX, 1},
        synchronize(),
    });
    require(!held.any());

    // The highest tracked key code and slot are held.
    held.observe(Events {
        key(HeldInput::keyCount - 1, 1),
        slot(static_cast<std::int32_t>(HeldInput::slotCount) - 1), track(1),
        synchronize(),
    });
    requireStream(
        releaseOf(held),
        {key(HeldInput::keyCount - 1, 0),
         slot(static_cast<std::int32_t>(HeldInput::slotCount) - 1), lift(),
         synchronize()});
}

void heldInputResetForgetsEverythingAndSelectsTheFirstSlot()
{
    HeldInput held;
    held.observe(Events {
        key(keyA, 1), slot(6), track(1), synchronize()});
    held.reset();
    require(!held.any());

    // Slot 0 is selected again, not slot 6 where the guest was.
    held.observe(Events {slot(1), track(1), slot(0), synchronize()});
    requireStream(
        releaseOf(held),
        {slot(1), lift(), slot(0), synchronize()});
    held.observe(Events {track(1), synchronize()});
    requireStream(releaseOf(held), {slot(0), lift(), synchronize()});
}

} // namespace

int main(int argc, char** argv)
{
    const std::array<uml::testing::NamedTest, 16> tests {{
        {"a-dropped-lift-is-released-before-the-next-report",
         aDroppedLiftIsReleasedBeforeTheNextReport},
        {"a-dropped-move-still-releases-the-contact-it-held",
         aDroppedMoveStillReleasesTheContactItHeld},
        {"several-contacts-are-all-released-in-slot-order",
         severalContactsAreAllReleasedInSlotOrder},
        {"a-dropped-key-release-reaches-the-guest-through-the-device",
         aDroppedKeyReleaseReachesTheGuestThroughTheDevice},
        {"a-dropped-mouse-button-release-is-released-without-touch-knowledge",
         aDroppedMouseButtonReleaseIsReleasedWithoutTouchKnowledge},
        {"every-held-key-is-released-in-code-order",
         everyHeldKeyIsReleasedInCodeOrder},
        {"nothing-is-synthesized-without-an-overflow",
         nothingIsSynthesizedWithoutAnOverflow},
        {"an-overflow-with-nothing-held-synthesizes-nothing",
         anOverflowWithNothingHeldSynthesizesNothing},
        {"the-release-waits-for-room-for-it-and-the-report",
         theReleaseWaitsForRoomForItAndTheReport},
        {"clear-forgets-a-pending-release", clearForgetsAPendingRelease},
        {"clear-forgets-what-the-guest-held", clearForgetsWhatTheGuestHeld},
        {"the-overflow-is-reported-once", theOverflowIsReportedOnce},
        {"held-input-releases-only-what-was-told-held",
         heldInputReleasesOnlyWhatWasToldHeld},
        {"held-input-release-size-is-the-events-appended",
         heldInputReleaseSizeIsTheEventsAppended},
        {"held-input-ignores-what-a-guest-does-not-take",
         heldInputIgnoresWhatAGuestDoesNotTake},
        {"held-input-reset-forgets-everything-and-selects-the-first-slot",
         heldInputResetForgetsEverythingAndSelectsTheFirstSlot},
    }};
    // A name on the command line runs only that case.
    if (argc > 1) {
        for (const auto& test : tests) {
            if (std::strcmp(test.name, argv[1]) == 0)
                return uml::testing::runNamedTests(
                    std::span<const uml::testing::NamedTest>(&test, 1));
        }
        std::fprintf(stderr, "no such case: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    return uml::testing::runNamedTests(tests);
}
