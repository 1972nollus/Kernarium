// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// How far a boot has got, in the order a boot passes through it.
public enum BootMilestone: Int, Comparable, Sendable {
    /// The runtime has been asked to run the guest.
    case starting = 0
    /// The kernel is running.
    case kernelRunning
    /// Android's init has taken over.
    case androidStarting
    /// Android has finished booting.
    case androidReady

    public static func < (lhs: Self, rhs: Self) -> Bool { lhs.rawValue < rhs.rawValue }
}

/// Finds boot milestones in the guest's console output.
///
/// This reads the console because the runtime reports no boot events yet;
/// when it does, the events replace this scanner and nothing else changes.
/// The console arrives in chunks that split lines anywhere, so the scanner
/// keeps the unfinished line.
public struct ConsoleScanner: Sendable {
    /// A line longer than this is dropped: no marker is that long.
    static let maximumLine = 4096

    private var pending = ""
    private var discarding = false

    public init() {}

    /// The milestones the text completes, in order of appearance.
    public mutating func feed(_ text: String) -> [BootMilestone] {
        var found: [BootMilestone] = []
        // Scalars, not characters: "\r\n" is one character.
        for scalar in text.unicodeScalars {
            if scalar == "\n" || scalar == "\r" {
                if !discarding, let milestone = Self.milestone(in: pending) { found.append(milestone) }
                pending.removeAll(keepingCapacity: true)
                discarding = false
            } else if !discarding {
                pending.unicodeScalars.append(scalar)
                if pending.utf8.count > Self.maximumLine {
                    pending.removeAll(keepingCapacity: true)
                    discarding = true
                }
            }
        }
        return found
    }

    public mutating func feed(_ data: Data) -> [BootMilestone] {
        feed(String(decoding: data, as: UTF8.self))
    }

    static func milestone(in line: String) -> BootMilestone? {
        if line.contains("processing action (sys.boot_completed=1)") { return .androidReady }
        if line.contains("init: init first stage started!") { return .androidStarting }
        if line.hasPrefix("Linux version ") || line.contains("] Linux version ") { return .kernelRunning }
        return nil
    }
}

/// The boot's progress, monotonic: a milestone never moves it back.
public struct BootTracker: Equatable, Sendable {
    public private(set) var milestone: BootMilestone = .starting
    public private(set) var reached: [BootMilestone: Int] = [.starting: 0]

    public init() {}

    /// Records `milestone` reached `seconds` into the boot. Returns whether
    /// it advanced.
    @discardableResult
    public mutating func reach(_ milestone: BootMilestone, atSeconds seconds: Int) -> Bool {
        guard milestone > self.milestone else { return false }
        self.milestone = milestone
        reached[milestone] = seconds
        return true
    }

    public var isReady: Bool { milestone == .androidReady }
}
