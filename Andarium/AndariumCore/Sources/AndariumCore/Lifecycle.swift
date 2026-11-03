// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

public enum Phase: Equatable, Sendable {
    case home
    case booting(BootKind)
    case running(StartMode)
    case poweredOff
    case failed
}

public enum HomeNotice: Equatable, Sendable {
    /// The guest was just erased.
    case erased
    /// iOS ended Andarium while the guest was paused out of the foreground.
    case closedInBackground
}

/// Whether a start may go ahead.
public enum StartVerdict: Equatable, Sendable {
    case allowed(BootKind)
    case noGuest
    /// The host has less free space than Android's own writes need.
    case lowSpace(neededBytes: UInt64, freeBytes: UInt64)
    /// An update did not complete and this start cannot try it again (it is
    /// Recovery, or this Android is not the one updating): Restore or Erase
    /// is the way on.
    case unfinishedUpdate
    /// The guest cannot start with this Android; erasing it is the way out.
    case blocked(StartBlock)
}

/// Where Andarium's one guest is in its life. A launch moves forward only:
/// Home, one boot and its run, and how it ended; the runtime boots Android
/// once per process, and a restart Android asks for happens inside the run.
public struct Lifecycle: Equatable, Sendable {
    /// Andarium holds at most one guest.
    public var guest: Guest?
    /// Moves only through the methods below in the application; the preview
    /// and the tests set it to reach a screen directly.
    public var phase: Phase = .home
    public var notice: HomeNotice?

    public init(guest: Guest?) {
        self.guest = guest
    }

    public mutating func create(_ guest: Guest) {
        self.guest = guest
        notice = nil
    }

    /// Erases everything in the guest and leaves a fresh one with the same
    /// choices; its next start runs Android's first-run setup.
    public mutating func erase() {
        guard let guest else { return }
        self.guest = guest.erased
        notice = .erased
    }

    /// Android's own factory reset from Recovery: the result of Erase, done
    /// from inside, so there is no Home notice to show.
    public mutating func factoryReset() {
        guard let guest else { return }
        self.guest = guest.erased
    }

    /// `plan` is what the instance on disk says a start would be. Start
    /// tries an unfinished update again when it can; Recovery and a start
    /// that cannot wait for Restore or Erase.
    public func startVerdict(
        _ mode: StartMode, host: HostDevice, plan: StartPlan = .normal
    ) -> StartVerdict {
        guard guest != nil else { return .noGuest }
        guard host.hasRoom(for: Limits.startReserveBytes) else {
            return .lowSpace(neededBytes: Limits.startReserveBytes, freeBytes: host.freeStorageBytes)
        }
        switch plan {
        case .blocked(let reason):
            return .blocked(reason)
        case .unfinishedUpdate(_, let canRetry, _):
            return mode == .normal && canRetry ? .allowed(.upgrade) : .unfinishedUpdate
        case .firstStart, .normal, .update:
            return .allowed(plan.bootKind(mode))
        }
    }

    /// Starts the guest if the host allows it; the verdict says why not.
    @discardableResult
    public mutating func start(
        _ mode: StartMode, host: HostDevice, plan: StartPlan = .normal
    ) -> StartVerdict {
        let verdict = startVerdict(mode, host: host, plan: plan)
        if case let .allowed(kind) = verdict {
            phase = .booting(kind)
        }
        return verdict
    }

    /// The boot reached Android (or Recovery). A recovery boot changes
    /// nothing in the guest; any other applies a pending growth.
    public mutating func finishBoot(_ kind: BootKind) {
        if kind != .recovery {
            guest?.didBoot()
        }
        phase = .running(kind == .recovery ? .recovery : .normal)
    }

    public mutating func powerOff() { phase = .poweredOff }

    public mutating func fail() { phase = .failed }

    /// Android's own Restart boots the guest again in place.
    public mutating func restart() { phase = .booting(.restart) }
}

/// How a run ended, from the runtime's result.
public struct RunEnd: Equatable, Sendable {
    public var result: Int32
    /// The runtime stopped a guest that kept restarting within seconds of
    /// booting.
    public var restartedInALoop: Bool
    /// Why the run could not begin, when it did not.
    public var startFailure: String?

    public init(result: Int32, restartedInALoop: Bool = false, startFailure: String? = nil) {
        self.result = result
        self.restartedInALoop = restartedInALoop
        self.startFailure = startFailure
    }

    public var poweredOff: Bool { result == 0 && !restartedInALoop && startFailure == nil }

    public var phase: Phase { poweredOff ? .poweredOff : .failed }

    /// The lines the failure screen offers to copy.
    public func report(
        appVersion: String, system: SystemInfo, guest: Guest?, uptimeSeconds: Int?
    ) -> String {
        var lines = [
            "Andarium \(appVersion) · \(system.release)",
            "\(system.kernel) · \(guest?.cores ?? 0) vCPUs · \(Formats.memory(guest?.memoryMiB ?? 0))",
        ]
        let reason = if let startFailure {
            "it could not start (\(startFailure))"
        } else if restartedInALoop {
            "the guest kept restarting"
        } else {
            "runtime result \(result)"
        }
        if let uptimeSeconds {
            lines.append("Guest ended: \(reason) after \(Formats.elapsed(uptimeSeconds))")
        } else {
            lines.append("Guest ended: \(reason)")
        }
        return lines.joined(separator: "\n")
    }
}
