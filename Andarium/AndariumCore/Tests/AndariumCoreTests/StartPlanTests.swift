// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

private func identity(
    _ id: String, build: Int, sha: String = String(repeating: "a", count: 64), layout: Int = 1
) -> ReleaseIdentity {
    ReleaseIdentity(
        releaseID: id, buildFingerprint: "fp-\(id)", buildNumber: build, securityPatch: "2026-10-01",
        systemDiskSHA256: sha, dataLayout: layout, minimumGiB: 4, defaultGiB: 8, maximumGiB: 64
    )
}

private let r1 = identity("r1", build: 100)
private let r2 = identity("r2", build: 200)
private let r3 = identity("r3", build: 300)
private let host = HostDevice(
    name: "iPad", processorCount: 6, memory: MemoryPolicy(limitMiB: 5000), freeStorageBytes: 40_000_000_000
)

private func makeDirectory() throws -> URL {
    let root = FileManager.default.temporaryDirectory
        .appendingPathComponent("andarium-plan-tests-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    return root
}

struct StartPlanTests {
    /// An instance created with r1, started and completed, then updated to r2
    /// whose start did not complete.
    private func unfinishedUpdate(in directory: URL) throws -> InstanceFiles {
        let files = InstanceFiles(directory: directory)
        try files.apply(InstanceDecision(.create, "new"), release: r1)
        try Data("android data".utf8).write(to: files.data)
        try files.complete(release: r1)
        try files.apply(files.decide(for: r2), release: r2)
        return files
    }

    @Test func eachDecisionHasAPlan() throws {
        let directory = try makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let files = InstanceFiles(directory: directory)
        #expect(files.startPlan(for: r1) == .firstStart)

        try files.apply(InstanceDecision(.create, "new"), release: r1)
        #expect(files.startPlan(for: r1) == .firstStart)
        try Data("android data".utf8).write(to: files.data)
        #expect(files.startPlan(for: r1) == .normal)
        #expect(files.startPlan(for: r2) == .update(from: "r1", to: "r2"))

        try files.complete(release: r1)
        // An older Android does not go back; a newer data layout has no
        // migration; one release with two system disks is a mismatch.
        try files.apply(files.decide(for: r2), release: r2)
        try files.complete(release: r2)
        #expect(files.startPlan(for: r1) == .blocked(.olderAndroid))
        #expect(files.startPlan(for: identity("r3", build: 300, layout: 2)) == .blocked(.newerDataLayout))
        #expect(files.startPlan(for: identity("r2", build: 200, sha: String(repeating: "b", count: 64))) == .blocked(.mismatch))
    }

    @Test func dataNothingRecordedIsAMismatch() throws {
        let directory = try makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let files = InstanceFiles(directory: directory)
        try Data("android data".utf8).write(to: files.data)
        #expect(files.startPlan(for: r1) == .blocked(.mismatch))
    }

    @Test func anUnreadableRecordHasNoPlan() throws {
        let directory = try makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let files = InstanceFiles(directory: directory)
        try Data(#"{"schema": 0}"#.utf8).write(to: files.state)
        #expect(files.startPlan(for: r1) == nil)
    }

    @Test func anUnfinishedUpdateOffersTheWaysThatCanWork() throws {
        let directory = try makeDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let files = try unfinishedUpdate(in: directory)
        #expect(files.startPlan(for: r2) == .unfinishedUpdate(to: "r2", canRetry: true, canRestore: true))
        // A newer build has taken the place of the one that did not finish:
        // its start cannot be retried, but the data it started from can come
        // back.
        #expect(files.startPlan(for: r3) == .unfinishedUpdate(to: "r2", canRetry: false, canRestore: true))
        // Without its snapshot there is nothing to put back.
        try FileManager.default.removeItem(at: files.snapshot(for: "r2"))
        #expect(files.startPlan(for: r2) == .unfinishedUpdate(to: "r2", canRetry: true, canRestore: false))
    }

    @Test func theBootKindFollowsThePlanAndTheMode() {
        #expect(StartPlan.firstStart.bootKind(.normal) == .firstStart)
        #expect(StartPlan.normal.bootKind(.normal) == .normal)
        #expect(StartPlan.update(from: "r1", to: "r2").bootKind(.normal) == .upgrade)
        #expect(StartPlan.unfinishedUpdate(to: "r2", canRetry: true, canRestore: true).bootKind(.normal) == .upgrade)
        #expect(StartPlan.update(from: "r1", to: "r2").bootKind(.recovery) == .recovery)
        #expect(StartPlan.firstStart.bootKind(.recovery) == .recovery)
    }
}

struct StartVerdictTests {
    private let unfinished = StartPlan.unfinishedUpdate(to: "r2", canRetry: true, canRestore: false)

    @Test func theVerdictFollowsThePlan() {
        let life = Lifecycle(guest: Guest.fresh(on: host))
        #expect(life.startVerdict(.normal, host: host, plan: .firstStart) == .allowed(.firstStart))
        #expect(life.startVerdict(.normal, host: host, plan: .normal) == .allowed(.normal))
        #expect(life.startVerdict(.normal, host: host, plan: .update(from: "r1", to: "r2")) == .allowed(.upgrade))
        #expect(life.startVerdict(.recovery, host: host, plan: .update(from: "r1", to: "r2")) == .allowed(.recovery))
        #expect(life.startVerdict(.normal, host: host, plan: .blocked(.olderAndroid)) == .blocked(.olderAndroid))
        #expect(life.startVerdict(.recovery, host: host, plan: .blocked(.mismatch)) == .blocked(.mismatch))
    }

    @Test func anUnfinishedUpdateWaitsForTheUsersChoice() {
        let life = Lifecycle(guest: Guest.fresh(on: host))
        #expect(life.startVerdict(.normal, host: host, plan: unfinished) == .unfinishedUpdate)
        #expect(life.startVerdict(.recovery, host: host, plan: unfinished) == .unfinishedUpdate)
        #expect(life.startVerdict(.normal, host: host, plan: unfinished, resolution: .retry) == .allowed(.upgrade))
        // A choice that cannot work does not let the start through.
        #expect(life.startVerdict(.normal, host: host, plan: unfinished, resolution: .restoreSnapshot) == .unfinishedUpdate)
    }

    @Test func aBlockedStartLeavesTheLifeAtHome() {
        var life = Lifecycle(guest: Guest.fresh(on: host))
        #expect(life.start(.normal, host: host, plan: .blocked(.olderAndroid)) == .blocked(.olderAndroid))
        #expect(life.phase == .home)
        #expect(life.start(.normal, host: host, plan: unfinished, resolution: .retry) == .allowed(.upgrade))
        #expect(life.phase == .booting(.upgrade))
    }
}
