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
/// type holds the numbers and the rules that follow from them.
public enum RunSpace {
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

    /// What one reading of the free space says of a running guest.
    public static func verdict(freeBytes: UInt64) -> Verdict {
        freeBytes < Limits.pauseBelowBytes ? .pause : .keepRunning
    }

    /// Whether a paused guest may run again at this much free space. More is
    /// needed than the pause stopped it at, or the next write pauses it again.
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

/// A guest paused for want of space, as the screen over the session shows it.
public struct SpacePause: Equatable, Sendable {
    public enum Working: Equatable, Sendable {
        /// Resume was asked and the guest is waking.
        case resuming
        /// Shut down was asked and the guest is going down.
        case shuttingDown
    }

    /// Free space when it was last read.
    public var freeBytes: UInt64
    /// A resume read the space again and found it still short.
    public var stillShort = false
    public var working: Working?

    public init(freeBytes: UInt64, stillShort: Bool = false, working: Working? = nil) {
        self.freeBytes = freeBytes
        self.stillShort = stillShort
        self.working = working
    }

    public static let title = "Android is paused"

    /// What is the matter and what to do about it, in the figures the device
    /// has now.
    public var detail: String {
        let needed = Formats.size(bytes: Limits.resumeAtBytes)
        let free = Formats.size(bytes: freeBytes)
        if working == .shuttingDown {
            return "Android is saving what it has and shutting down."
        }
        if stillShort {
            return "Only \(free) is free. Free up more space, then resume. Android needs \(needed) free to continue."
        }
        return "This device has \(free) of free space left, too little for Android to keep writing. "
            + "Free up space, then resume. Android needs \(needed) free to continue."
    }
}
