// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Decides when a running guest is paused for want of space on the device.
///
/// The guest's data disk is a sparse file that grows as Android writes, and
/// the guest mounts it so that a write the device cannot take stops it
/// (`errors=panic`). The start check (`Lifecycle.startVerdict`) covers only the
/// first writes, so while the guest runs the application reads the free space
/// the way Home states it and pauses the guest before the disk can fail to
/// grow. The runtime's suspension does the pausing (`PauseController`); this
/// type holds the numbers and the one rule that follows from them.
public struct RunSpaceWatch: Equatable, Sendable {
    public enum Verdict: Equatable, Sendable {
        case keepRunning
        /// Free space fell below `Limits.pauseBelowBytes`.
        case pause
    }

    /// The writes one check interval is assumed to take, per second, when the
    /// next check is placed. A guest writes this fast only briefly, and a
    /// guess too high costs a few more checks, not a missed pause.
    public static let assumedWriteRate: UInt64 = 128 * Limits.bytesPerMiB
    public static let shortestInterval = Duration.seconds(2)
    public static let longestInterval = Duration.seconds(30)

    public private(set) var isPaused = false

    public init() {}

    /// One reading while the guest runs.
    public mutating func sample(freeBytes: UInt64) -> Verdict {
        guard freeBytes < Limits.pauseBelowBytes else { return .keepRunning }
        isPaused = true
        return .pause
    }

    /// Whether a paused guest may run again at this much free space; when it
    /// may, the guest counts as running.
    public mutating func resume(freeBytes: UInt64) -> Bool {
        guard isPaused, Self.allowsResume(freeBytes: freeBytes) else { return false }
        isPaused = false
        return true
    }

    /// The guest does not run any more or the watch starts over.
    public mutating func reset() {
        isPaused = false
    }

    public static func allowsResume(freeBytes: UInt64) -> Bool {
        freeBytes >= Limits.resumeAtBytes
    }

    /// How long to wait before the next reading: the time the guest would
    /// take to write what separates `freeBytes` from the pause, at
    /// `assumedWriteRate`, so a nearly full device is read often and a roomy
    /// one rarely. Never less than `shortestInterval` or more than
    /// `longestInterval`.
    public static func nextCheck(freeBytes: UInt64) -> Duration {
        let margin = freeBytes > Limits.pauseBelowBytes ? freeBytes - Limits.pauseBelowBytes : 0
        let seconds = Double(margin) / Double(assumedWriteRate)
        return min(max(.seconds(seconds), shortestInterval), longestInterval)
    }
}
