// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

/// The settings of a launch, answered as the application's environment
/// reader would.
private func launch(
    _ settings: [LaunchOverrides.Setting: String], processorCount: Int = 4
) -> LaunchOverrides {
    LaunchOverrides(processorCount: processorCount) { settings[$0] }
}

struct LaunchOverridesTests {
    @Test func noSettingAsksNothing() {
        let overrides = launch([:], processorCount: 8)
        #expect(overrides.isEmpty)
        #expect(overrides.start == nil)
        #expect(!overrides.erase)
    }

    @Test func readsTheSharedSettings() {
        let overrides = launch(
            [
                .vCPUCount: "2",
                .memoryMiB: "1536",
                .diskGiB: "32",
                .retina: "1",
            ],
            processorCount: 6
        )
        #expect(overrides.vCPUCount == 2)
        #expect(overrides.memoryMiB == 1536)
        #expect(overrides.capacityGiB == 32)
        #expect(overrides.retina == true)
    }

    @Test func aStoredFollowWindowValueIsIgnored() throws {
        // The guest's display always follows the window: an earlier version's
        // stored choice is read past, not migrated.
        let old = Data(#"{"retina":true,"followWindow":false}"#.utf8)
        #expect(try JSONDecoder().decode(DisplayOptions.self, from: old) == DisplayOptions(retina: true))
    }

    @Test func ignoresValuesOutOfRangeOrOffStep() {
        let overrides = launch(
            [
                .vCPUCount: "9",
                .memoryMiB: "1000",
                .diskGiB: "x",
                .retina: "2",
            ],
            processorCount: 6
        )
        #expect(overrides.isEmpty)
    }

    /// The application passes the host's vCPU limit as `processorCount`, so
    /// the vCPU count states 1 through the limit and nothing above it.
    @Test func theVCPUCountIsBoundedByTheHostsLimit() {
        func stated(_ value: String, limit: Int) -> Int? {
            launch([.vCPUCount: value], processorCount: limit).vCPUCount
        }
        for limit in [6, 8] {
            #expect(stated("1", limit: limit) == 1)
            #expect(stated(String(limit), limit: limit) == limit)
            #expect(stated(String(limit + 1), limit: limit) == nil)
            #expect(stated("0", limit: limit) == nil)
            #expect(stated("many", limit: limit) == nil)
        }
        // Ten is no longer offered, whatever the device has.
        #expect(stated("10", limit: 8) == nil)
        #expect(stated("20", limit: 8) == nil)
    }

    @Test func startModes() {
        #expect(launch([.start: "normal"]).start == .normal)
        #expect(launch([.start: "recovery"]).start == .recovery)
        #expect(launch([.start: "other"]).start == nil)
        #expect(launch([.bootMode: "recovery"]).start == .recovery)
        #expect(launch([.bootMode: "normal"]).start == nil)
    }

    @Test func erase() {
        #expect(launch([.erase: "1"]).erase)
        #expect(!launch([.erase: "0"]).erase)
        #expect(!launch([:]).erase)
    }

    @Test func statistics() {
        #expect(launch([.stats: "1"]).statistics)
        #expect(!launch([.stats: "0"]).statistics)
        #expect(!launch([.stats: "yes"]).statistics)
        #expect(!launch([:]).statistics)
        #expect(!launch([.stats: "1"]).isEmpty)
    }

    @Test func isolatedInstanceNamesCannotEscapeTheirDirectory() {
        #expect(launch([.instance: "armeabi-legacy_1"]).instance == "armeabi-legacy_1")
        for name in ["", ".", "..", "../Guest", "/tmp/guest", "a/b", "a b", "游戏", String(repeating: "a", count: 65)] {
            let overrides = launch([.instance: name, .start: "normal", .erase: "1"])
            #expect(overrides.instance == nil)
            #expect(overrides.start == nil)
            #expect(!overrides.erase)
        }
    }

    @Test func appliesToAGuest() {
        var overrides = LaunchOverrides()
        overrides.vCPUCount = 8
        overrides.memoryMiB = 2048
        overrides.capacityGiB = 24
        let host = HostDevice(
            name: "iPad", processorCount: 4,
            memory: MemoryPolicy(maximumsMiB: [4096, 4096, 3840, 3776]), freeStorageBytes: 200_000_000_000
        )
        let guest = overrides.applied(to: Guest.fresh(on: host), on: host)
        #expect(guest.cores == 4)
        #expect(guest.memoryMiB == 2048)
        #expect(guest.effectiveCapacityGiB >= 24)
    }
}

struct ReportTests {
    @Test func guestKeysUseLinuxCodes() {
        #expect(GuestKey.back.rawValue == 158)
        #expect(GuestKey.home.rawValue == 172)
        #expect(GuestKey.power.rawValue == 116)
    }

    @Test func bootStepsFollowMilestones() {
        #expect(BootSteps.current(preparingDisk: true, milestone: .starting) == 0)
        #expect(BootSteps.current(preparingDisk: false, milestone: .starting) == 1)
        #expect(BootSteps.current(preparingDisk: false, milestone: .kernelRunning) == 2)
        #expect(BootSteps.current(preparingDisk: false, milestone: .androidStarting) == 3)
        // Recovery has a fifth step for its first frame.
        #expect(BootSteps.current(preparingDisk: false, milestone: .screenStarting, recovery: true) == 4)
        #expect(BootSteps.current(preparingDisk: false, milestone: .ready, recovery: true) == 5)
        #expect(BootSteps.count(recovery: true) == 5)
    }

    @Test func aNormalBootStaysInStartupUntilBootCompleted() {
        // Four steps: Startup is the last, and nothing before sys.boot_completed
        // moves past it, the graphics service included.
        #expect(BootSteps.count(recovery: false) == 4)
        #expect(BootSteps.current(preparingDisk: false, milestone: .androidStarting) == 3)
        #expect(BootSteps.current(preparingDisk: false, milestone: .screenStarting) == 3)
        #expect(BootSteps.current(preparingDisk: false, milestone: .ready) == 4)
    }

    @Test func aTrackerIsReadyOnlyAtTheEnd() {
        var tracker = BootTracker()
        #expect(!tracker.isReady)
        tracker.reach(.screenStarting, atSeconds: 8)
        #expect(!tracker.isReady)
        tracker.reach(.ready, atSeconds: 12)
        #expect(tracker.isReady)
        tracker.restart()
        #expect(!tracker.isReady)
        #expect(tracker.milestone == .starting)
    }

    @Test func aStartFailureEndsAsAFailure() {
        let end = RunEnd(result: -1, startFailure: "the disk could not be written")
        #expect(!end.poweredOff)
        #expect(end.phase == .failed)
    }
}

@MainActor
private final class FakeEffects: PauseEffects {
    var calls: [String] = []
    var pendingResume: (@MainActor () -> Void)?

    func holdGuestGPU() { calls.append("hold") }
    func releaseGuestGPU() { calls.append("release") }
    func flushGuest(until deadline: DispatchTime?) {
        calls.append(deadline == nil ? "flush" : "flush-wait")
    }
    func quiesceGuest(reason: String, until deadline: DispatchTime?) {
        calls.append("quiesce(\(reason),\(deadline == nil ? "nowait" : "wait"))")
    }
    func resumeGuest(finished: @escaping @MainActor () -> Void) {
        calls.append("resume")
        pendingResume = finished
    }
}

@MainActor
private func makeController(runClock: GuestRunClock? = nil) throws -> (PauseController, FakeEffects, PauseMarker) {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("andarium-pause-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let marker = PauseMarker(url: directory.appendingPathComponent("pause.json"))
    let effects = FakeEffects()
    return (PauseController(effects: effects, marker: marker, runClock: runClock), effects, marker)
}

@MainActor
struct PauseControllerTests {
    @Test func backgroundHoldsTheGPUBeforePausing() throws {
        let (controller, effects, marker) = try makeController()
        controller.guestDidStart()
        controller.didEnterBackground()
        #expect(effects.calls == ["hold", "quiesce(background,wait)"])
        #expect(controller.phase == .paused)
        #expect(FileManager.default.fileExists(atPath: marker.url.path))
    }

    @Test func foregroundReleasesThenResumesAndKeepsTheMarkerUntilBack() throws {
        let (controller, effects, marker) = try makeController()
        controller.guestDidStart()
        controller.didEnterBackground()
        effects.calls.removeAll()
        controller.willEnterForeground()
        #expect(effects.calls == ["release", "resume"])
        #expect(FileManager.default.fileExists(atPath: marker.url.path))
        effects.pendingResume?()
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
        #expect(controller.phase == .foreground)
    }

    @Test func nothingHappensWithoutAGuest() throws {
        let (controller, effects, marker) = try makeController()
        controller.willResignActive()
        controller.didEnterBackground()
        controller.willEnterForeground()
        #expect(effects.calls.isEmpty)
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
    }

    @Test func losingFocusOnlyFlushes() throws {
        let (controller, effects, _) = try makeController()
        controller.guestDidStart()
        controller.willResignActive()
        #expect(effects.calls == ["flush"])
        #expect(controller.phase == .foreground)
    }

    @Test func aSecondBackgroundEdgeIsIgnored() throws {
        let (controller, effects, _) = try makeController()
        controller.guestDidStart()
        controller.didEnterBackground()
        controller.didEnterBackground()
        #expect(effects.calls == ["hold", "quiesce(background,wait)"])
    }

    @Test func terminationDisarmsAndWritesBack() throws {
        let (controller, effects, marker) = try makeController()
        controller.guestDidStart()
        controller.didEnterBackground()
        controller.willTerminate()
        #expect(!FileManager.default.fileExists(atPath: marker.url.path))
        #expect(effects.calls == ["hold", "quiesce(background,wait)"])

        let (second, secondEffects, _) = try makeController()
        second.guestDidStart()
        second.willTerminate()
        #expect(secondEffects.calls == ["quiesce(terminating,wait)"])
    }

    @Test func theRunClockStopsWhileTheGuestIsPausedAndRunsAgainWhenItIsBack() throws {
        let clock = GuestRunClock()
        let (controller, effects, _) = try makeController(runClock: clock)
        controller.guestDidStart()
        controller.willResignActive()
        #expect(!clock.isPaused)
        controller.didEnterBackground()
        #expect(clock.isPaused)
        controller.willEnterForeground()
        // The guest is not back until the resume has finished.
        #expect(clock.isPaused)
        effects.pendingResume?()
        #expect(!clock.isPaused)
    }

    @Test func aQuickReturnKeepsTheClockStoppedUntilTheLastResume() throws {
        let clock = GuestRunClock()
        let (controller, effects, _) = try makeController(runClock: clock)
        controller.guestDidStart()
        controller.didEnterBackground()
        controller.willEnterForeground()
        let first = effects.pendingResume
        controller.didEnterBackground()
        controller.willEnterForeground()
        let second = effects.pendingResume
        first?()
        #expect(clock.isPaused)
        second?()
        #expect(!clock.isPaused)
    }

    @Test func terminatingAlsoStopsTheClock() throws {
        let clock = GuestRunClock()
        let (controller, _, _) = try makeController(runClock: clock)
        controller.guestDidStart()
        controller.willTerminate()
        #expect(clock.isPaused)
    }

    @Test func endedGuestIsNotPaused() throws {
        let (controller, effects, _) = try makeController()
        controller.guestDidStart()
        controller.guestDidEnd()
        controller.didEnterBackground()
        #expect(effects.calls.isEmpty)
    }
}

struct PauseMarkerTests {
    @Test func consumeReportsOnce() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-marker-\(UUID().uuidString)")
        let marker = PauseMarker(url: directory.appendingPathComponent("nested/pause.json"))
        #expect(!marker.consume())
        #expect(marker.arm())
        #expect(marker.consume())
        #expect(!marker.consume())
    }

    @Test func unreadableFileStillCounts() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("andarium-marker-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let url = directory.appendingPathComponent("pause.json")
        try Data("not json".utf8).write(to: url)
        #expect(PauseMarker(url: url).consume())
    }
}
