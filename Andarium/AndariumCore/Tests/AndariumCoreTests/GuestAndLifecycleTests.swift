// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

private let ipad = HostDevice(
    name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
)
private let fullIpad = HostDevice(
    name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 1_000_000_000
)

struct GuestTests {
    @Test func aFreshGuestTakesTheHostsChoices() {
        let guest = Guest.fresh(on: ipad)
        #expect(guest.cores == 6)
        #expect(guest.memoryMiB == 1536)
        #expect(guest.capacityGiB == 8)
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

    @Test func aFinishedBootAppliesAPendingGrowth() {
        var guest = Guest.fresh(on: ipad)
        guest.setCapacity(24)
        guest.didBoot()
        #expect(guest.capacityGiB == 24)
        #expect(guest.pendingCapacityGiB == nil)
    }

    @Test func eraseKeepsTheChoicesAndTheCapacityTheNextStartRunsWith() {
        var guest = Guest.fresh(on: ipad)
        guest.microphone = true
        guest.includeInBackups = true
        guest.memoryMiB = 2048
        guest.setCapacity(24)
        guest.didBoot()
        guest.usedBytes = 5_000_000_000
        guest.setCapacity(32)

        let erased = guest.erased
        #expect(erased.capacityGiB == 32)
        #expect(erased.pendingCapacityGiB == nil)
        #expect(erased.usedBytes == 0)
        #expect(erased.microphone)
        #expect(erased.includeInBackups)
        #expect(erased.memoryMiB == 2048)
    }

    @Test func onlyTheChoicesAreStored() throws {
        var guest = Guest.fresh(on: ipad)
        guest.usedBytes = 123
        let data = try JSONEncoder().encode(guest)
        let text = String(decoding: data, as: UTF8.self)
        #expect(!text.contains("usedBytes"))

        let decoded = try JSONDecoder().decode(Guest.self, from: data)
        #expect(decoded.usedBytes == 0)
        #expect(decoded.cores == guest.cores)
    }
}

struct LifecycleTests {
    @Test func noGuestCannotStart() {
        var life = Lifecycle(guest: nil)
        #expect(life.start(.normal, host: ipad) == .noGuest)
        #expect(life.phase == .home)
    }

    /// Metal 4 starts with the A14: an earlier chip cannot draw the guest, so
    /// nothing starts, with or without a guest, and the words name the chip.
    @Test func aDeviceThatCannotDrawTheGuestNeverStarts() {
        var older = ipad
        older.runsGuestGraphics = false
        var life = Lifecycle(guest: Guest.fresh(on: older))
        #expect(life.start(.normal, host: older) == .needsNewerChip)
        #expect(life.start(.recovery, host: older, plan: .firstStart) == .needsNewerChip)
        #expect(life.phase == .home)
        #expect(Lifecycle(guest: nil).startVerdict(.normal, host: older) == .needsNewerChip)
        #expect(DeviceRequirement.title.contains("A14"))
        #expect(DeviceRequirement.explanation(deviceName: "iPhone").contains("this iPhone"))
        #expect(ipad.runsGuestGraphics)
    }

    @Test func aStartNeedsRoomForAndroidsOwnWrites() {
        var life = Lifecycle(guest: Guest.fresh(on: ipad))
        let verdict = life.start(.normal, host: fullIpad)
        #expect(verdict == .lowSpace(neededBytes: Limits.startReserveBytes, freeBytes: 1_000_000_000))
        #expect(life.phase == .home)
    }

    @Test func aFirstStartBootsThenRuns() {
        var life = Lifecycle(guest: Guest.fresh(on: ipad))
        #expect(life.start(.normal, host: ipad, plan: .firstStart) == .allowed(.firstStart))
        #expect(life.phase == .booting(.firstStart))
        life.finishBoot(.firstStart)
        #expect(life.phase == .running(.normal))
    }

    @Test func aRecoveryBootChangesNothingInTheGuest() {
        var guest = Guest.fresh(on: ipad)
        guest.setCapacity(24)
        var life = Lifecycle(guest: guest)
        #expect(life.start(.recovery, host: ipad) == .allowed(.recovery))
        life.finishBoot(.recovery)
        #expect(life.phase == .running(.recovery))
        #expect(life.guest?.pendingCapacityGiB == 24)
    }

    @Test func eraseLeavesAFreshGuestAndANotice() {
        var life = Lifecycle(guest: Guest.fresh(on: ipad))
        life.erase()
        #expect(life.notice == .erased)
        life.create(Guest.fresh(on: ipad))
        #expect(life.notice == nil)
    }

    @Test func aFactoryResetFromRecoveryErasesWithoutANotice() {
        var life = Lifecycle(guest: Guest.fresh(on: ipad))
        life.factoryReset()
        #expect(life.notice == nil)
    }

    @Test func theRunEndsInOneOfThreeWays() {
        var life = Lifecycle(guest: Guest.fresh(on: ipad))
        _ = life.start(.normal, host: ipad)
        life.finishBoot(.normal)
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
            version: "17", release: "20261031-los24-r4",
            kernel: "Linux 6.18.54", architecture: "arm64, 4 KiB pages", appVersion: "1.0 (1)"
        )
        let guest = Guest(cores: 2, memoryMiB: 1536, capacityGiB: 8)
        let report = RunEnd(result: -5).report(appVersion: "1.0 (1)", system: system, guest: guest, uptimeSeconds: 74)
        #expect(report == """
            Andarium 1.0 (1) · 20261031-los24-r4
            Linux 6.18.54 · 2 vCPUs · 1.5 GiB
            Guest ended: runtime result -5 after 1:14
            """)
    }
}
