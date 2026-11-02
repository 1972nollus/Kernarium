// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Why a guest cannot start with the Android this build carries. Only erasing
/// the guest ends each of these.
public enum StartBlock: Equatable, Sendable {
    /// The Android in this build is older than the one the data was started
    /// with; Android's key store and package manager do not go back.
    case olderAndroid
    /// The data uses a newer layout than this build can migrate.
    case newerDataLayout
    /// The guest's files do not match the Android in this build: another
    /// system disk under the same release, or data nothing recorded.
    case mismatch
}

extension StartBlock {
    public var title: String { "This Guest Cannot Start" }

    /// What Home says; erasing the guest is the way out of each.
    public var explanation: String {
        switch self {
        case .olderAndroid:
            "This Andarium carries an older Android than the one the guest last started with, and Android's data does not go back. Install a newer Andarium, or erase the guest to start again with this Android."
        case .newerDataLayout:
            "The guest's data uses a newer layout than this Andarium can open. Erase the guest to start again with this Android."
        case .mismatch:
            "The guest's files do not match the Android in this Andarium. Erase the guest to start again."
        }
    }
}

/// How the guest the user picks up on Home would start, from the instance on
/// disk (`InstanceFiles.startPlan`); nothing is changed to find out.
public enum StartPlan: Equatable, Sendable {
    /// The guest has no data yet: Android sets itself up.
    case firstStart
    /// The Android the data was last started with.
    case normal
    /// A newer Android on the same data layout: the data is snapshotted, then
    /// Android upgrades it on its first start.
    case update(from: String, to: String)
    /// An update started and did not reach a clean end. The user chooses how
    /// to go on: start it again, put back the data it started from and
    /// update afresh, or erase the guest. Each of the first two is offered
    /// only when it can work.
    case unfinishedUpdate(to: String, canRetry: Bool, canRestore: Bool)
    /// Nothing but erasing the guest helps.
    case blocked(StartBlock)

    /// What a start in `mode` is, for the words and steps of the boot.
    public func bootKind(_ mode: StartMode) -> BootKind {
        if mode == .recovery { return .recovery }
        switch self {
        case .firstStart: return .firstStart
        case .update, .unfinishedUpdate: return .upgrade
        case .normal, .blocked: return .normal
        }
    }
}

/// How the user goes on after an update that did not complete. Erasing the
/// guest, the third way out, is Erase.
public enum UpdateResolution: Equatable, Sendable {
    /// Start the update again, keeping its snapshot.
    case retry
    /// Put back the data the update started from, then update afresh.
    case restoreSnapshot

    var flags: InstanceFlags {
        switch self {
        case .retry: InstanceFlags(retry: true)
        case .restoreSnapshot: InstanceFlags(restoreSnapshot: true)
        }
    }
}

extension InstanceRules {
    /// What a start would do, in the terms of Home. It follows `decide` and
    /// adds what the choice among the ways out needs.
    public static func plan(
        state: InstanceState?, dataDiskExists: Bool, snapshotExists: Bool, release: ReleaseIdentity
    ) -> StartPlan {
        let decision = decide(
            state: state, dataDiskExists: dataDiskExists, snapshotExists: snapshotExists, release: release
        )
        switch decision.action {
        case .create: return .firstStart
        case .boot, .retry, .restore: return .normal
        case .update: return .update(from: state?.releaseID ?? "", to: release.releaseID)
        case .migrate: return .blocked(.newerDataLayout)
        case .refuseDowngrade: return .blocked(.olderAndroid)
        case .refuseConflict: return .blocked(.mismatch)
        case .refusePending:
            guard let pending = state?.pendingUpdate else { return .blocked(.mismatch) }
            return .unfinishedUpdate(
                to: pending.toReleaseID,
                canRetry: release.releaseID == pending.toReleaseID && dataDiskExists,
                canRestore: snapshotExists
            )
        case .reset: return .firstStart
        }
    }
}

extension InstanceFiles {
    /// The plan for `release` from the files, or nil when `instance.json`
    /// cannot be read (a guest an earlier layout made): a start then fails
    /// with the reason, as it always did.
    public func startPlan(for release: ReleaseIdentity) -> StartPlan? {
        let recorded: InstanceState?
        do {
            recorded = try readState()
        } catch {
            return nil
        }
        let manager = FileManager.default
        let snapshotExists = recorded?.pendingUpdate.map {
            manager.fileExists(atPath: directory.appendingPathComponent($0.snapshot).path)
        } ?? false
        return InstanceRules.plan(
            state: recorded, dataDiskExists: manager.fileExists(atPath: data.path),
            snapshotExists: snapshotExists, release: release
        )
    }
}
