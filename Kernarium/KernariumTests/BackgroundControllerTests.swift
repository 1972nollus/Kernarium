// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
@testable import Kernarium

/// What the background policy asks for at each lifecycle edge, in order. The
/// order is the point: the host GPU is held before the guest can reach it
/// while it is revoked, and released only once the guest is paused or kept
/// alive.
@MainActor
struct BackgroundControllerTests {
    private final class Deadline: BackgroundCancellable {
        let action: @MainActor () -> Void
        var cancelled = false

        init(_ action: @escaping @MainActor () -> Void) {
            self.action = action
        }

        func cancel() { cancelled = true }
    }

    private final class Grace: BackgroundCancellable {
        let name: String
        weak var effects: Effects?

        init(_ name: String, _ effects: Effects) {
            self.name = name
            self.effects = effects
        }

        func cancel() { effects?.calls.append("end \(name)") }
    }

    /// Records every request and lets a test fire the policy's timer.
    private final class Effects: BackgroundEffects {
        var calls: [String] = []
        var pictureInPictureCanStart = false
        var pictureInPictureActive = false
        var deadlines: [Deadline] = []

        func holdGuestGPU() { calls.append("hold") }
        func releaseGuestGPU() { calls.append("release") }
        func flushGuest(until deadline: DispatchTime?) {
            calls.append("flush")
        }
        func quiesceGuest(reason: String, until deadline: DispatchTime?) {
            calls.append("quiesce \(reason)")
        }
        func resumeGuest() { calls.append("resume") }
        func setPageCacheTrimming(_ enabled: Bool) {
            calls.append("trim \(enabled ? "on" : "off")")
        }
        func setBackgroundThrottle(_ enabled: Bool) {
            calls.append("throttle \(enabled ? "on" : "off")")
        }
        func stopPictureInPicture() { calls.append("stop pip") }
        func keepScheduled(named name: String) -> any BackgroundCancellable {
            calls.append("begin \(name)")
            return Grace(name, self)
        }
        func schedule(
            after seconds: TimeInterval,
            _ action: @escaping @MainActor () -> Void
        ) -> any BackgroundCancellable {
            let deadline = Deadline(action)
            deadlines.append(deadline)
            return deadline
        }

        func expire() {
            for deadline in deadlines where !deadline.cancelled {
                deadline.action()
            }
        }
    }

    /// What reached the runtime's page-cache trimming switch, from any thread.
    private nonisolated final class TrimmingSwitch: @unchecked Sendable {
        private let lock = NSLock()
        private var settings: [Int32] = []
        private var onMainThread = false

        func set(_ enabled: Int32) -> Int32 {
            lock.withLock {
                settings.append(enabled)
                onMainThread = onMainThread || Thread.isMainThread
            }
            return 0
        }

        var received: [Int32] { lock.withLock { settings } }
        var reachedOnMainThread: Bool { lock.withLock { onMainThread } }
    }

    private func controller(
        _ mode: BackgroundMode,
        _ effects: Effects
    ) -> BackgroundController {
        BackgroundController(effects: effects, mode: { mode })
    }

    @Test
    func aGuestThatChoseNothingIsHeldThenQuiescedAndResumedOnReturn() {
        let effects = Effects()
        let policy = controller(.off, effects)

        policy.didEnterBackground()
        #expect(effects.calls == ["hold", "quiesce background"])
        #expect(policy.phase == .quiesced)

        policy.willEnterForeground()
        #expect(effects.calls == [
            "hold", "quiesce background", "release", "resume",
        ])
        #expect(policy.phase == .foreground)
    }

    @Test
    func aWindowThatStartsKeepsTheGuestRunningAndReleasesTheGPU() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)

        policy.didEnterBackground()
        #expect(effects.calls == [
            "hold", "flush", "begin picture-in-picture",
        ])
        #expect(policy.phase == .awaitingPictureInPicture)

        policy.pictureInPictureDidStart()
        #expect(Array(effects.calls.dropFirst(3)) == [
            "end picture-in-picture", "release", "trim on", "throttle on",
        ])
        #expect(policy.phase == .keptRunning)
        #expect(!effects.calls.contains { $0.hasPrefix("quiesce") })

        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["throttle off", "trim off"])
        #expect(!effects.calls.contains("resume"))
        #expect(policy.phase == .foreground)
    }

    /// The application's effects pass the policy's trimming edges to the
    /// runtime, in order and away from the main thread, since the switch may
    /// wait for the guest's console.
    @Test
    func theTrimmingEdgesReachTheRuntimeOffTheMainThread() async throws {
        let runtime = TrimmingSwitch()
        let effects = SystemBackgroundEffects(
            persistence: .shared,
            pictureInPicture: nil,
            pageCacheTrimming: { runtime.set($0) }
        )

        effects.setPageCacheTrimming(true)
        effects.setPageCacheTrimming(false)
        #expect(try await eventually { runtime.received.count == 2 })
        #expect(runtime.received == [1, 0])
        #expect(!runtime.reachedOnMainThread)
    }

    @Test
    func aWindowThatDoesNotStartInTimePausesTheGuest() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)
        policy.didEnterBackground()

        effects.expire()
        #expect(Array(effects.calls.dropFirst(3)) == [
            "end picture-in-picture",
            "quiesce picture in picture did not start",
        ])
        #expect(policy.phase == .quiesced)
        // The GPU stayed held from the edge: nothing released it.
        #expect(!effects.calls.contains("release"))

        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["release", "resume"])
    }

    @Test
    func aWindowThatFailsToStartPausesTheGuest() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)
        policy.didEnterBackground()

        policy.pictureInPictureDidFailToStart()
        #expect(policy.phase == .quiesced)
        #expect(effects.calls.contains(
            "quiesce picture in picture did not start"
        ))
        // The start deadline is withdrawn: it cannot pause the guest twice.
        effects.expire()
        #expect(effects.calls.filter { $0.hasPrefix("quiesce") }.count == 1)
    }

    @Test
    func closingTheWindowInTheBackgroundHoldsTheGPUAndPausesTheGuest() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)
        policy.didEnterBackground()
        policy.pictureInPictureDidStart()
        effects.calls.removeAll()

        policy.pictureInPictureDidStop()
        #expect(effects.calls == [
            "throttle off", "trim off", "hold",
            "quiesce picture in picture closed",
        ])
        #expect(policy.phase == .quiesced)

        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["release", "resume"])
    }

    @Test
    func closingTheWindowInTheForegroundChangesNothing() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)
        policy.didEnterBackground()
        policy.pictureInPictureDidStart()
        policy.willEnterForeground()
        effects.calls.removeAll()

        policy.pictureInPictureDidStop()
        #expect(effects.calls.isEmpty)
        #expect(policy.phase == .foreground)
    }

    @Test
    func aWindowThatIsAlreadyUpAtTheEdgeNeedsNoWait() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        effects.pictureInPictureActive = true
        let policy = controller(.pictureInPicture, effects)

        policy.didEnterBackground()
        #expect(effects.calls == [
            "hold", "flush", "release", "trim on", "throttle on",
        ])
        #expect(policy.phase == .keptRunning)
    }

    @Test
    func aGuestWhoseWindowCannotStartPausesLikeAnyOther() {
        let effects = Effects()
        effects.pictureInPictureCanStart = false
        let policy = controller(.pictureInPicture, effects)

        policy.didEnterBackground()
        #expect(effects.calls == ["hold", "quiesce background"])
        #expect(policy.phase == .quiesced)
    }

    @Test
    func returningWhileTheWindowIsStillAwaitedCancelsTheWait() {
        let effects = Effects()
        effects.pictureInPictureCanStart = true
        let policy = controller(.pictureInPicture, effects)
        policy.didEnterBackground()

        policy.willEnterForeground()
        #expect(effects.calls.suffix(2) == ["end picture-in-picture", "release"])
        #expect(!effects.calls.contains("resume"))
        effects.expire()
        #expect(!effects.calls.contains {
            $0.hasPrefix("quiesce")
        })
        #expect(policy.phase == .foreground)
    }

    @Test
    func becomingActiveClosesAWindowThatIsStillUp() {
        let effects = Effects()
        let policy = controller(.pictureInPicture, effects)
        policy.didBecomeActive()
        #expect(effects.calls.isEmpty)

        effects.pictureInPictureActive = true
        policy.didBecomeActive()
        #expect(effects.calls == ["stop pip"])
    }

    @Test
    func aSecondEdgeWhileInTheBackgroundIsIgnored() {
        let effects = Effects()
        let policy = controller(.off, effects)
        policy.didEnterBackground()
        effects.calls.removeAll()
        policy.didEnterBackground()
        #expect(effects.calls.isEmpty)
    }

    @Test
    func eachGuestKeepsItsOwnModeAndDefaultsToPausing() throws {
        let suite = "background-settings-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let settings = BackgroundSettings(defaults: defaults)

        #expect(settings.mode(forGuest: "kde.erofs") == .off)
        settings.setMode(.pictureInPicture, forGuest: "kde.erofs")
        #expect(settings.mode(forGuest: "kde.erofs") == .pictureInPicture)
        #expect(settings.mode(forGuest: "android.kern") == .off)

        settings.setMode(.off, forGuest: "kde.erofs")
        #expect(settings.mode(forGuest: "kde.erofs") == .off)
        #expect(defaults.dictionaryRepresentation().keys.allSatisfy {
            !$0.hasPrefix(BackgroundSettings.keyPrefix)
        })

        // Automation can force a mode for one launch without touching what
        // the user chose; a mode this build does not know is ignored.
        let forced = BackgroundSettings(
            defaults: defaults,
            environment: [BackgroundSettings.environmentKey: "pictureInPicture"]
        )
        #expect(forced.mode(forGuest: "android.kern") == .pictureInPicture)
        let unknown = BackgroundSettings(
            defaults: defaults,
            environment: [BackgroundSettings.environmentKey: "someday"]
        )
        #expect(unknown.mode(forGuest: "android.kern") == .off)

        // A kept guest is throttled unless automation turns that off.
        #expect(settings.throttlesKeptGuest)
        #expect(!BackgroundSettings(
            defaults: defaults,
            environment: [BackgroundSettings.throttleEnvironmentKey: "0"]
        ).throttlesKeptGuest)

        // A value from a newer build that this one does not know pauses.
        defaults.set("someday", forKey: BackgroundSettings.keyPrefix + "x")
        #expect(settings.mode(forGuest: "x") == .off)
    }
}
