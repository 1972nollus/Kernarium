// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What a launch's environment asks of Andarium, for automation. The
/// `KERNARIUM_` names mean what they mean in Kernarium; the `ANDARIUM_` ones
/// are Andarium's own (`Andarium/README.md#automation`). A value that is out
/// of range, off its step or malformed is ignored, as it is in Kernarium.
public struct LaunchOverrides: Equatable, Sendable {
    public var vCPUCount: Int?
    public var memoryMiB: Int?
    public var capacityGiB: Int?
    public var retina: Bool?
    public var followWindow: Bool?
    /// Start at launch, creating the guest first when there is none.
    public var start: StartMode?
    /// Erase the guest's data before the start.
    public var erase = false

    public init() {}

    /// `processorCount` is the host's vCPU limit (`HostDevice.processorCount`):
    /// `KERNARIUM_VCPU_COUNT` states 1 through it and nothing outside.
    public init(environment: [String: String], processorCount: Int) {
        func integer(_ key: String, in range: ClosedRange<Int>) -> Int? {
            environment[key].flatMap { Int($0) }.flatMap { range.contains($0) ? $0 : nil }
        }
        func flag(_ key: String) -> Bool? {
            switch environment[key] {
            case "0"?: false
            case "1"?: true
            default: nil
            }
        }
        vCPUCount = integer("KERNARIUM_VCPU_COUNT", in: 1...max(processorCount, 1))
        memoryMiB = integer("KERNARIUM_GUEST_MEMORY_MIB", in: Limits.runtimeMemoryRangeMiB)
            .flatMap { MemoryPolicy.runtimeAccepts($0) ? $0 : nil }
        capacityGiB = integer("KERNARIUM_ANDROID_DISK_SIZE_GIB", in: Limits.capacityRangeGiB)
        retina = flag("KERNARIUM_RETINA_MODE")
        followWindow = flag("KERNARIUM_GPU_AUTO_RESIZE")
        switch environment["ANDARIUM_START"] {
        case "normal"?: start = .normal
        case "recovery"?: start = .recovery
        default:
            // Kernarium's boot-mode name asks for a start only where the
            // launch names Recovery; the others leave Home alone.
            if environment["KERNARIUM_ANDROID_BOOT_MODE"] == "recovery" { start = .recovery }
        }
        erase = environment["ANDARIUM_ERASE"] == "1"
    }

    public var isEmpty: Bool { self == LaunchOverrides() }

    /// The guest as this launch asks for it. A guest not yet created takes
    /// the capacity at once; an existing one can only grow. A stored vCPU
    /// count above what the host offers comes down to it. A memory size the
    /// environment states is kept as stated, for automation; any other is
    /// taken within the bound of the vCPU count the guest then has, as a
    /// stored size is on load.
    public func applied(to guest: Guest, on host: HostDevice) -> Guest {
        var guest = guest
        guest.cores = min(max(guest.cores, 1), host.processorCount)
        if let vCPUCount { guest.cores = min(vCPUCount, host.processorCount) }
        if let memoryMiB {
            guest.memoryMiB = memoryMiB
        } else {
            guest = guest.memoryWithinBound(on: host)
        }
        if let capacityGiB { guest.setCapacity(capacityGiB) }
        return guest
    }
}

/// The two display options set on Home, kept across launches.
public struct DisplayOptions: Codable, Equatable, Sendable {
    /// Whether the guest draws at the screen's pixel density.
    public var retina = false
    /// Whether the guest follows the window's size.
    public var followWindow = true

    public init(retina: Bool = false, followWindow: Bool = true) {
        self.retina = retina
        self.followWindow = followWindow
    }
}

/// The lines a run prints for the harness to read. Everything else on the
/// console is the runtime's own `Kernarium:` output.
public enum ConsoleReport {
    public enum ThermalPhase: String, Sendable {
        case start, change, finish
    }

    public static func thermalName(_ state: ProcessInfo.ThermalState) -> String {
        switch state {
        case .nominal: "nominal"
        case .fair: "fair"
        case .serious: "serious"
        case .critical: "critical"
        @unknown default: "unknown"
        }
    }

    public static func thermal(phase: ThermalPhase, state: ProcessInfo.ThermalState, uptime: TimeInterval) -> String {
        let seconds = String(format: "%.3f", locale: Locale(identifier: "en_US_POSIX"), uptime)
        return "Andarium thermal state: phase=\(phase.rawValue) state=\(thermalName(state)) uptime=\(seconds)"
    }

    public static func runtimeReturned(_ result: Int32) -> String {
        "Andarium: runtime returned \(result)"
    }

    /// The application lost or regained focus (a system alert, such as iOS
    /// asking for local network access, takes it until a person answers).
    public static func application(active: Bool) -> String {
        active ? "Andarium: application active" : "Andarium: application inactive"
    }
}

/// The Linux key codes Andarium sends the guest for its side keys. Android's
/// generic key layout maps each to the Android key of the same meaning.
public enum GuestKey: UInt16, CaseIterable, Sendable {
    case power = 116
    case volumeUp = 115
    case volumeDown = 114
    case back = 158
    case home = 172
}
