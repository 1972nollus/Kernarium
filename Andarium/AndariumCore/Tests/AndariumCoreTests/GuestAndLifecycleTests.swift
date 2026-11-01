// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

private let ipad = HostDevice(
    name: "iPad", processorCount: 6, memory: MemoryPolicy(limitMiB: 5000), freeStorageBytes: 40_000_000_000
)
private let fullIpad = HostDevice(
    name: "iPad", processorCount: 6, memory: MemoryPolicy(limitMiB: 5000), freeStorageBytes: 1_000_000_000
)

struct GuestTests {
    @Test func aFreshGuestTakesTheHostsChoices() {
        let guest = Guest.fresh(on: ipad)
        #expect(guest.cores == 6)
        #expect(guest.memoryMiB == 1536)
        #expect(guest.capacityGiB == 8)
        #expect(guest.neverStarted)
        #expect(!guest.includeInBackups)
        #expect(!guest.microphone)
    }

    @Test func storageOnlyGrowsAndTheGrowthWaitsForTheNextStart() {
        var guest = Guest.fresh(on: ipad)
        guest.setCapacity(24)
        #expect(guest.capacityGiB == 8)
        #expect(guest.pendingCapacityGiB == 24)
        #expect(guest.effectiveCapacityGiB == 24)

        guest.setCapacity(4)
        #expect(guest.pendingCapacityGiB == nil)
        #expect(guest.effectiveCapacityGiB == 8)

        guest.setCapacity(500)
        #expect(guest.pendingCapacityGiB == 64)
        #expect(!guest.canGrow)
    }

    @Test func choosingTheCurrentCapacityCancelsAPendingGrowth() {
        var guest = Guest.fresh(on: ipad)
        guest.setCapacity(16)
        guest.setCapacity(8)
        #expect(guest.pendingCapacityGiB == nil)
    }

    @Test func bootKindFollowsTheGuestsHistory() {
        var guest = Guest.fresh(on: ipad)
        #expect(guest.bootKind(.normal) == .firstStart)
        #expect(guest.bootKind(.recovery) == .recovery)
        guest.didBoot(release: "r1")
        #expect(guest.bootKind(.normal) == .normal)
        #expect(guest.bootKind(.normal, currentRelease: "r1") == .normal)
        #expect(guest.bootKind(.normal, currentRelease: "r2") == .upgrade)
        #expect(guest.bootKind(.recovery, currentRelease: "r2") == .recovery)
    }

    @Test func aFinishedBootAppliesAPendingGrowth() {
        var guest = Guest.fresh(on: ipad)
        guest.setCapacity(24)
        guest.didBoot(release: "r1")
        #expect(guest.capacityGiB == 24)
        #expect(guest.pendingCapacityGiB == nil)
        #expect(guest.systemRelease == "r1")
    }

    @Test func eraseKeepsTheChoicesAndTheCapacityTheNextStartRunsWith() {
        var guest = Guest.fresh(on: ipad)
        guest.microphone = true
        guest.includeInBackups = true
        guest.memoryMiB = 2048
        guest.setCapacity(24)
        guest.didBoot(release: "r1")
        guest.usedBytes = 5_000_000_000
        guest.apps = 12
        guest.setCapacity(32)

        let erased = guest.erased
        #expect(erased.capacityGiB == 32)
        #expect(erased.pendingCapacityGiB == nil)
        #expect(erased.neverStarted)
        #expect(erased.usedBytes == 0)
        #expect(erased.apps == nil)
        #expect(erased.microphone)
        #expect(erased.includeInBackups)
        #expect(erased.memoryMiB == 2048)
    }

    @Test func onlyTheChoicesAreStored() throws {
        var guest = Guest.fresh(on: ipad)
        guest.didBoot(release: "r1")
        guest.usedBytes = 123
        guest.apps = 9
        let data = try JSONEncoder().encode(guest)
        let text = String(decoding: data, as: UTF8.self)
        #expect(!text.contains("usedBytes"))
        #expect(!text.contains("apps"))

        let decoded = try JSONDecoder().decode(Guest.self, from: data)
        #expect(decoded.usedBytes == 0)
        #expect(decoded.apps == nil)
        #expect(decoded.systemRelease == "r1")
        #expect(decoded.cores == guest.cores)
    }
}

struct LifecycleTests {
    @Test func noGuestCannotStart() {
        var life = Lifecycle(guest: nil)
        #expect(life.start(.normal, host: ipad) == .noGuest)
        #expect(life.phase == .home)
    }

    @Test func aStartNeedsRoomForAndroidsOwnWrites() {
        var life = Lifecycle(guest: Guest.fresh(on: ipad))
        let verdict = life.start(.normal, host: fullIpad)
        #expect(verdict == .lowSpace(neededBytes: Limits.startReserveBytes, freeBytes: 1_000_000_000))
        #expect(life.phase == .home)
    }

    @Test func aFirstStartBootsThenRunsAndRecordsTheSystem() {
        var life = Lifecycle(guest: Guest.fresh(on: ipad))
        #expect(life.start(.normal, host: ipad) == .allowed(.firstStart))
        #expect(life.phase == .booting(.firstStart))
        life.finishBoot(.firstStart, release: "r1")
        #expect(life.phase == .running(.normal))
        #expect(life.guest?.systemRelease == "r1")
    }

    @Test func aRecoveryBootChangesNothingInTheGuest() {
        var guest = Guest.fresh(on: ipad)
        guest.didBoot(release: "r1")
        guest.setCapacity(24)
        var life = Lifecycle(guest: guest)
        #expect(life.start(.recovery, host: ipad) == .allowed(.recovery))
        life.finishBoot(.recovery, release: "r2")
        #expect(life.phase == .running(.recovery))
        #expect(life.guest?.systemRelease == "r1")
        #expect(life.guest?.pendingCapacityGiB == 24)
    }

    @Test func eraseLeavesAFreshGuestAndANotice() {
        var guest = Guest.fresh(on: ipad)
        guest.didBoot(release: "r1")
        var life = Lifecycle(guest: guest)
        life.erase()
        #expect(life.notice == .erased)
        #expect(life.guest?.neverStarted == true)
        life.create(Guest.fresh(on: ipad))
        #expect(life.notice == nil)
    }

    @Test func aFactoryResetFromRecoveryErasesWithoutANotice() {
        var guest = Guest.fresh(on: ipad)
        guest.didBoot(release: "r1")
        var life = Lifecycle(guest: guest)
        life.factoryReset()
        #expect(life.guest?.neverStarted == true)
        #expect(life.notice == nil)
    }

    @Test func theRunEndsInOneOfThreeWays() {
        var life = Lifecycle(guest: Guest.fresh(on: ipad))
        _ = life.start(.normal, host: ipad)
        life.finishBoot(.firstStart, release: "r1")
        life.restart()
        #expect(life.phase == .booting(.restart))
        life.powerOff()
        #expect(life.phase == .poweredOff)
        life.fail()
        #expect(life.phase == .failed)
    }

    @Test func aRunEndFollowsTheRuntimeResult() {
        #expect(RunEnd(result: 0).phase == .poweredOff)
        #expect(RunEnd(result: -5).phase == .failed)
        #expect(RunEnd(result: 0, restartedInALoop: true).phase == .failed)
    }

    @Test func theFailureReportNamesTheBuildAndTheEnd() {
        let system = SystemInfo(
            distribution: "LineageOS 24", android: "17", release: "20261031-los24-r4",
            kernel: "Linux 6.18.54", architecture: "arm64, 4 KB pages", appVersion: "1.0 (1)"
        )
        let guest = Guest(cores: 2, memoryMiB: 1536, capacityGiB: 8)
        let report = RunEnd(result: -5).report(appVersion: "1.0 (1)", system: system, guest: guest, uptimeSeconds: 74)
        #expect(report == """
            Andarium 1.0 (1) · LineageOS 24 20261031-los24-r4
            Linux 6.18.54 · 2 vCPUs · 1.5 GB
            Guest ended: runtime result -5 after 1:14
            """)
    }
}
