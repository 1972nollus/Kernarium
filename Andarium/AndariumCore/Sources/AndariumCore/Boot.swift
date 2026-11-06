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
    /// Android's graphics service has started, so the display is about to
    /// have content.
    case screenStarting
    /// Android has finished booting, or Recovery is up: the guest has
    /// something to show.
    case ready

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
    /// How many times Android has reported `sys.boot_completed=1`. Recovery
    /// never does, so only a count past zero says Android itself booted.
    public private(set) var bootCompletedCount = 0

    public init() {}

    /// The milestones the text completes, in order of appearance.
    public mutating func feed(_ text: String) -> [BootMilestone] {
        var found: [BootMilestone] = []
        // Scalars, not characters: "\r\n" is one character.
        for scalar in text.unicodeScalars {
            if scalar == "\n" || scalar == "\r" {
                if !discarding {
                    if let milestone = Self.milestone(in: pending) { found.append(milestone) }
                    if pending.contains(Self.bootCompletedLine) { bootCompletedCount += 1 }
                }
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

    static let bootCompletedLine = "processing action (sys.boot_completed=1)"

    static func milestone(in line: String) -> BootMilestone? {
        if line.contains(bootCompletedLine) { return .ready }
        if line.contains("init: starting service 'recovery'") { return .ready }
        if line.contains("init: starting service 'surfaceflinger'") { return .screenStarting }
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

    /// The boot screen has done its work: there is something of the guest's
    /// to show.
    public var isReady: Bool { milestone == .ready }

    /// A restarted guest boots again from the start.
    public mutating func restart() {
        self = BootTracker()
    }
}

/// Which of the boot screen's steps is under way, as the boot reaches its
/// milestones. A normal boot has four: Andarium, Linux Kernel, Storage and
/// Startup, which stays under way from Android's init until Android reports
/// `sys.boot_completed`. Recovery adds a fifth, Screen, for its first frame.
/// `count(recovery:)` is past the last. Preparing the guest's disk is the
/// first step: it writes the system the application carries.
public enum BootSteps {
    public static func count(recovery: Bool) -> Int { recovery ? 5 : 4 }

    public static func current(preparingDisk: Bool, milestone: BootMilestone, recovery: Bool = false) -> Int {
        if preparingDisk { return 0 }
        let last = count(recovery: recovery) - 1
        switch milestone {
        case .starting: return 1
        case .kernelRunning: return 2
        case .androidStarting: return 3
        case .screenStarting: return min(4, last)
        case .ready: return count(recovery: recovery)
        }
    }
}
