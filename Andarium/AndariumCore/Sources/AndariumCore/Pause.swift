// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// A file that outlives the process and says Andarium was out of the
/// foreground, with a guest paused, when it ended.
///
/// iOS can end a suspended application without running any of its code, and
/// the guest it hosted is then gone: the next launch boots it again. The
/// application cannot observe that ending, so it records the edge it can
/// observe: written as the guest pauses, removed once the application is back
/// in the foreground and the guest runs again, or when it will terminate on
/// its own account. A marker found at launch therefore means the previous
/// process ended while paused.
public struct PauseMarker: Sendable {
    struct Record: Codable, Equatable {
        var pausedAt: Date
    }

    public let url: URL

    public init(url: URL) {
        self.url = url
    }

    /// Records that the guest paused at `date`. Returns whether the marker
    /// reached storage.
    @discardableResult
    public func arm(at date: Date = Date()) -> Bool {
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true
            )
            let encoder = JSONEncoder()
            encoder.dateEncodingStrategy = .secondsSince1970
            try encoder.encode(Record(pausedAt: date)).write(to: url, options: .atomic)
            return true
        } catch {
            return false
        }
    }

    public func disarm() {
        try? FileManager.default.removeItem(at: url)
    }

    /// Whether a previous process left the marker, removed so that it is
    /// reported once. A file that cannot be read still counts.
    public func consume() -> Bool {
        guard FileManager.default.fileExists(atPath: url.path) else { return false }
        disarm()
        return true
    }
}

/// What pausing the guest does to the runtime, behind one seam so the policy
/// is tested without it. The runtime calls run on a serial queue the
/// application owns, in the order asked.
@MainActor
public protocol PauseEffects: AnyObject {
    /// Holds every guest GPU request: iOS revokes the host GPU a few seconds
    /// after the application leaves the foreground, and a request that
    /// reaches it then makes the guest's Vulkan device lost for good.
    func holdGuestGPU()
    func releaseGuestGPU()
    /// Writes the guest's storage back and leaves it running. Waits for the
    /// write-back until `deadline`, or not at all when it is nil.
    func flushGuest(until deadline: DispatchTime?)
    /// Writes the guest's storage back and suspends it.
    func quiesceGuest(reason: String, until deadline: DispatchTime?)
    /// Resumes a quiesced guest behind any quiesce still in flight and calls
    /// `finished` on the main actor once it is back.
    func resumeGuest(finished: @escaping @MainActor () -> Void)
}

/// Decides what happens to the guest at each lifecycle edge: Andarium has no
/// background mode, so a guest runs in the foreground only. The edge holds
/// the GPU first and releases it only once the guest is running again, which
/// keeps the GPU safe.
///
/// A guest can also be paused in the foreground, for want of space on the
/// device (`RunSpaceWatch`). It stays paused until the person resumes it: a
/// lifecycle edge never wakes it, and none writes its storage again, because
/// writing it back would end the suspension.
@MainActor
public final class PauseController {
    public enum Phase: Equatable, Sendable {
        case foreground
        /// Out of the foreground with the guest paused.
        case paused
    }

    /// What an edge's own work may take: the watchdog answers to the edge
    /// handler as a whole.
    public static let edgeBudget = DispatchTimeInterval.seconds(4)

    public private(set) var phase = Phase.foreground
    public var isPausedForSpace: Bool { spacePaused }
    private let effects: any PauseEffects
    private let marker: PauseMarker
    /// Stops while the guest is paused, so timers on the guest's running time
    /// (the adb operations') do not run out meanwhile.
    private let runClock: GuestRunClock?
    private var guestRunning = false
    private var gpuHeld = false
    private var guestPaused = false
    /// The guest is paused for want of space, which only the person ends.
    private var spacePaused = false
    private var inForeground = true
    /// Resumes that have started and not returned.
    private var resumesInFlight = 0

    public init(effects: any PauseEffects, marker: PauseMarker, runClock: GuestRunClock? = nil) {
        self.effects = effects
        self.marker = marker
        self.runClock = runClock
    }

    /// The runtime is running a guest. A guest not yet running when the
    /// application leaves the foreground is not paused, because nothing of it
    /// exists to pause; the runtime itself is only started in the foreground.
    public func guestDidStart() {
        guestRunning = true
    }

    public func guestDidEnd() {
        guestRunning = false
        // The pause went with the guest: nothing is left to resume.
        if spacePaused {
            spacePaused = false
            guestPaused = false
            if resumesInFlight == 0 { runClock?.resume() }
        }
    }

    /// Focus is lost but the process runs on: storage is written back and
    /// the guest keeps running, because stalling a user about to come straight
    /// back helps nothing.
    public func willResignActive() {
        guard guestRunning, phase == .foreground, !guestPaused else { return }
        effects.flushGuest(until: nil)
    }

    public func didEnterBackground() {
        inForeground = false
        guard guestRunning, phase == .foreground else { return }
        let deadline = DispatchTime.now() + Self.edgeBudget
        effects.holdGuestGPU()
        gpuHeld = true
        // A guest already paused for space has written everything back and
        // is suspended: only the application's own state needs saying.
        if !spacePaused {
            effects.quiesceGuest(reason: "background", until: deadline)
            guestPaused = true
            runClock?.pause()
        }
        phase = .paused
        marker.arm()
    }

    /// Returns to the foreground. The marker stays while the resume is under
    /// way: a guest whose hibernated RAM cannot be restored ends the process
    /// inside it, and the next launch should still say the guest restarted.
    public func willEnterForeground() {
        inForeground = true
        if gpuHeld {
            effects.releaseGuestGPU()
            gpuHeld = false
        }
        if guestPaused, !spacePaused {
            guestPaused = false
            resumesInFlight += 1
            effects.resumeGuest { [weak self] in self?.resumeFinished() }
        }
        phase = .foreground
        disarmWhenSettled()
    }

    /// The device is nearly out of space: the guest's storage is written back
    /// and the guest suspended, without waiting for either, and it stays so
    /// until `resumeFromSpacePause`. Returns whether it paused: not for a
    /// guest that is not running, is already paused, or whose application is
    /// out of the foreground. The GPU is not held, because a suspended guest
    /// makes no request and the foreground's GPU is valid.
    @discardableResult
    public func pauseForSpace() -> Bool {
        guard guestRunning, phase == .foreground, !guestPaused else { return false }
        effects.quiesceGuest(reason: "low space", until: nil)
        guestPaused = true
        spacePaused = true
        runClock?.pause()
        return true
    }

    /// The person resumes a guest paused for space. `finished` runs on the
    /// main actor once the guest is back; it does not run when no pause is
    /// held or the application is out of the foreground. The marker needs no change: it is armed only by a background
    /// edge, which a foreground resume does not follow.
    public func resumeFromSpacePause(finished: @escaping @MainActor () -> Void) {
        guard spacePaused, inForeground else { return }
        spacePaused = false
        guestPaused = false
        resumesInFlight += 1
        effects.resumeGuest { [weak self] in
            self?.resumeFinished()
            finished()
        }
    }

    /// The application will exit: the marker goes, since the next launch is
    /// the user's own, and the guest's storage is written back with the
    /// whole of the edge budget.
    public func willTerminate() {
        marker.disarm()
        guard guestRunning, !guestPaused else { return }
        effects.quiesceGuest(reason: "terminating", until: DispatchTime.now() + Self.edgeBudget)
        guestPaused = true
        runClock?.pause()
    }

    private func resumeFinished() {
        resumesInFlight = max(resumesInFlight - 1, 0)
        if !guestPaused, resumesInFlight == 0 { runClock?.resume() }
        disarmWhenSettled()
    }

    private func disarmWhenSettled() {
        if inForeground, resumesInFlight == 0 { marker.disarm() }
    }
}
