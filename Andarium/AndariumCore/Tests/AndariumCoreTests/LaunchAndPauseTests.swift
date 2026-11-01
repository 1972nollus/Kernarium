// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing

@testable import AndariumCore

struct LaunchOverridesTests {
    @Test func emptyEnvironmentAsksNothing() {
        let overrides = LaunchOverrides(environment: [:], processorCount: 8)
        #expect(overrides.isEmpty)
        #expect(overrides.start == nil)
        #expect(!overrides.erase)
    }

    @Test func readsTheSharedKnobs() {
        let overrides = LaunchOverrides(
            environment: [
                "KERNARIUM_VCPU_COUNT": "2",
                "KERNARIUM_GUEST_MEMORY_MIB": "1536",
                "KERNARIUM_ANDROID_DISK_SIZE_GIB": "32",
                "KERNARIUM_RETINA_MODE": "1",
                "KERNARIUM_GPU_AUTO_RESIZE": "0",
            ],
            processorCount: 6
        )
        #expect(overrides.vCPUCount == 2)
        #expect(overrides.memoryMiB == 1536)
        #expect(overrides.capacityGiB == 32)
        #expect(overrides.retina == true)
        #expect(overrides.followWindow == false)
    }

    @Test func ignoresValuesOutOfRangeOrOffStep() {
        let overrides = LaunchOverrides(
            environment: [
                "KERNARIUM_VCPU_COUNT": "9",
                "KERNARIUM_GUEST_MEMORY_MIB": "1000",
                "KERNARIUM_ANDROID_DISK_SIZE_GIB": "x",
                "KERNARIUM_RETINA_MODE": "2",
            ],
            processorCount: 6
        )
        #expect(overrides.isEmpty)
    }

    @Test func startModes() {
        #expect(LaunchOverrides(environment: ["ANDARIUM_START": "normal"], processorCount: 4).start == .normal)
        #expect(LaunchOverrides(environment: ["ANDARIUM_START": "recovery"], processorCount: 4).start == .recovery)
        #expect(LaunchOverrides(environment: ["ANDARIUM_START": "other"], processorCount: 4).start == nil)
        #expect(
            LaunchOverrides(environment: ["KERNARIUM_ANDROID_BOOT_MODE": "recovery"], processorCount: 4).start
                == .recovery
        )
        #expect(LaunchOverrides(environment: ["KERNARIUM_ANDROID_BOOT_MODE": "normal"], processorCount: 4).start == nil)
    }

    @Test func erase() {
        #expect(LaunchOverrides(environment: ["ANDARIUM_ERASE": "1"], processorCount: 4).erase)
        #expect(!LaunchOverrides(environment: ["ANDARIUM_ERASE": "0"], processorCount: 4).erase)
        #expect(!LaunchOverrides(environment: [:], processorCount: 4).erase)
    }

    @Test func appliesToAGuest() {
        var overrides = LaunchOverrides()
        overrides.vCPUCount = 8
        overrides.memoryMiB = 2048
        overrides.capacityGiB = 24
        let host = HostDevice(
            name: "iPad", processorCount: 4,
            memory: MemoryPolicy(limitMiB: 6000), freeStorageBytes: 200_000_000_000
        )
        let guest = overrides.applied(to: Guest.fresh(on: host), processorCount: 4)
        #expect(guest.cores == 4)
        #expect(guest.memoryMiB == 2048)
        #expect(guest.effectiveCapacityGiB >= 24)
    }
}

struct ReportTests {
    @Test func thermalLine() {
        #expect(
            ConsoleReport.thermal(phase: .start, state: .nominal, uptime: 12.3456)
                == "Andarium thermal state: phase=start state=nominal uptime=12.346"
        )
        #expect(
            ConsoleReport.thermal(phase: .finish, state: .serious, uptime: 0)
                == "Andarium thermal state: phase=finish state=serious uptime=0.000"
        )
    }

    @Test func runtimeLine() {
        #expect(ConsoleReport.runtimeReturned(0) == "Andarium: runtime returned 0")
        #expect(ConsoleReport.runtimeReturned(-3) == "Andarium: runtime returned -3")
    }

    @Test func focusLines() {
        #expect(ConsoleReport.application(active: true) == "Andarium: application active")
        #expect(ConsoleReport.application(active: false) == "Andarium: application inactive")
    }

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
        #expect(BootSteps.current(preparingDisk: false, milestone: .screenStarting) == 4)
        #expect(BootSteps.current(preparingDisk: false, milestone: .ready) == BootSteps.count)
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
private func makeController() throws -> (PauseController, FakeEffects, PauseMarker) {
    let directory = FileManager.default.temporaryDirectory
        .appendingPathComponent("andarium-pause-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let marker = PauseMarker(url: directory.appendingPathComponent("pause.json"))
    let effects = FakeEffects()
    return (PauseController(effects: effects, marker: marker), effects, marker)
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
