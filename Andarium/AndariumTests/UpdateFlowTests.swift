// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// A driver that answers with a fixed plan and records what the model asks.
@MainActor
final class StubDriver: RuntimeDriver {
    var plan: StartPlan?
    var begun: [(mode: StartMode, kind: BootKind)] = []
    var restored = 0
    var restoreFails = false

    init(plan: StartPlan?) { self.plan = plan }

    func create(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest { guest }
    func erase(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest { guest }
    func save(_ guest: Guest) throws {}
    func startPlan() -> StartPlan? { plan }
    func begin(guest: Guest, mode: StartMode, kind: BootKind) {
        begun.append((mode, kind))
    }
    func restoreUpdate() throws {
        if restoreFails { throw InstanceError.refused("No update is pending.") }
        restored += 1
        // The files now say the update waits for the next start.
        plan = .update(from: "r1", to: "r2")
    }
    func setMicrophone(_ on: Bool) {}

}

@MainActor
struct UpdateFlowTests {
    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func model(plan: StartPlan) -> (AppModel, StubDriver) {
        let system = SystemInfo(
            distribution: "LineageOS 24", android: "17", release: "r2", kernel: "Linux 6.18",
            architecture: "arm64, 4 KB pages", appVersion: "1"
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host, system: system, capabilities: .device,
            createRequirementBytes: 1, systemBytes: 1
        )
        let driver = StubDriver(plan: plan)
        model.driver = driver
        model.refreshPlan()
        return (model, driver)
    }

    @Test func anUpdateStartsAsAnUpgradeAndRemembersWhereFrom() {
        let (model, driver) = model(plan: .update(from: "r1", to: "r2"))
        #expect(model.bootKind(.normal) == .upgrade)
        model.start(.normal)
        #expect(model.phase == .booting(.upgrade))
        #expect(model.bootFrom == "r1")
        #expect(driver.begun.map(\.kind) == [.upgrade])
    }

    @Test func startOnAnUnfinishedUpdateTriesItAgain() {
        let (model, driver) = model(plan: .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
        #expect(model.bootKind(.normal) == .upgrade)
        model.start(.normal)
        #expect(model.phase == .booting(.upgrade))
        #expect(driver.begun.map(\.kind) == [.upgrade])
        #expect(driver.restored == 0)
    }

    @Test func restoreOnlyPutsTheDataBackAndHomeShowsThePendingUpdate() {
        let (model, driver) = model(plan: .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
        model.restoreUpdate()
        #expect(driver.restored == 1)
        #expect(model.phase == .home)
        #expect(driver.begun.isEmpty)
        #expect(model.plan == .update(from: "r1", to: "r2"))
        #expect(model.alert == nil)
        // The next start runs the update again.
        model.start(.normal)
        #expect(model.phase == .booting(.upgrade))
        #expect(model.bootFrom == "r1")
    }

    @Test func aRestoreThatFailsSaysSoAndStaysAtHome() {
        let (model, driver) = model(plan: .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
        driver.restoreFails = true
        model.restoreUpdate()
        #expect(model.alert?.title == "Android Could Not Be Restored")
        #expect(model.phase == .home)
        #expect(model.plan == .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
    }

    @Test func anAndroidThatCannotRetryWaitsForRestoreOrErase() {
        let (model, driver) = model(plan: .unfinishedUpdate(to: "r2", canRetry: false, canRestore: true))
        #expect(!model.plan.allowsStart)
        model.start(.normal)
        #expect(model.phase == .home)
        #expect(driver.begun.isEmpty)
        #expect(model.alert?.title == "Android's Update Didn't Finish")
    }

    @Test func recoveryWaitsForTheUpdateToFinish() {
        let (model, driver) = model(plan: .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
        model.start(.recovery)
        #expect(model.phase == .home)
        #expect(driver.begun.isEmpty)
        #expect(model.alert?.title == "Android's Update Didn't Finish")
    }

    @Test func aBlockedGuestSaysWhyAndStaysAtHome() {
        let (model, driver) = model(plan: .blocked(.olderAndroid))
        model.start(.normal)
        #expect(model.phase == .home)
        #expect(driver.begun.isEmpty)
        #expect(model.alert?.title == StartBlock.olderAndroid.title)
        #expect(model.alert?.message == StartBlock.olderAndroid.explanation)
    }

    @Test func theEndOfARunReadsThePlanAgain() {
        let (model, driver) = model(plan: .update(from: "r1", to: "r2"))
        model.start(.normal)
        // The update completed: the files now say an ordinary start.
        driver.plan = .normal
        model.runEnded(RunEnd(result: 0), uptimeSeconds: 60)
        #expect(model.plan == .normal)
        // A record that cannot be read leaves the start to report why.
        driver.plan = nil
        model.refreshPlan()
        #expect(model.plan == .normal)
    }
}
