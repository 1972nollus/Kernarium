// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

public enum StartMode: String, Hashable, Codable, Sendable {
    case normal, recovery
}

/// Why a start takes as long as it does.
public enum BootKind: Hashable, Sendable {
    case normal, firstStart, upgrade, recovery, restart
}

/// The one guest Andarium keeps: Android's data with the processors, memory
/// and capacity it runs with.
public struct Guest: Equatable, Codable, Sendable {
    public var cores: Int
    public var memoryMiB: Int
    public var capacityGiB: Int
    /// A larger capacity that applies when the guest next starts.
    public var pendingCapacityGiB: Int?
    public var microphone: Bool
    /// Whether iCloud and computer backups include the guest's files. Off by
    /// default: the guest is several gigabytes that Android can set up again.
    public var includeInBackups: Bool
    /// The system release this guest last started with; nil until its first
    /// start.
    public var systemRelease: String?

    // Measured when the guest is loaded; never stored.

    /// Data the guest has written: the space its storage occupies.
    public var usedBytes: UInt64 = 0
    /// How many apps Android has installed, where the runtime can tell.
    public var apps: Int?

    private enum CodingKeys: String, CodingKey {
        case cores, memoryMiB, capacityGiB, pendingCapacityGiB, microphone
        case includeInBackups, systemRelease
    }

    public init(
        cores: Int, memoryMiB: Int, capacityGiB: Int, pendingCapacityGiB: Int? = nil,
        microphone: Bool = false, includeInBackups: Bool = false, systemRelease: String? = nil,
        usedBytes: UInt64 = 0, apps: Int? = nil
    ) {
        self.cores = cores
        self.memoryMiB = memoryMiB
        self.capacityGiB = capacityGiB
        self.pendingCapacityGiB = pendingCapacityGiB
        self.microphone = microphone
        self.includeInBackups = includeInBackups
        self.systemRelease = systemRelease
        self.usedBytes = usedBytes
        self.apps = apps
    }

    public var neverStarted: Bool { systemRelease == nil }

    public var usedGB: Double { Double(usedBytes) / Double(Limits.bytesPerGiB) }

    /// The capacity the next start runs with.
    public var effectiveCapacityGiB: Int { pendingCapacityGiB ?? capacityGiB }

    /// Storage only grows, up to the largest capacity there is.
    public var canGrow: Bool { effectiveCapacityGiB < Limits.capacityRangeGiB.upperBound }

    /// What a start of this guest is, given the system release the app
    /// carries (nil where the app has no way to move a guest to another
    /// release, so a guest keeps the system it was created with).
    public func bootKind(_ mode: StartMode, currentRelease: String? = nil) -> BootKind {
        if mode == .recovery { return .recovery }
        if neverStarted { return .firstStart }
        if let currentRelease, systemRelease != currentRelease { return .upgrade }
        return .normal
    }

    /// Resize: storage only grows, and the growth waits for the next start.
    /// Choosing the current capacity again cancels a growth not yet applied.
    public mutating func setCapacity(_ value: Int) {
        let value = min(max(value, capacityGiB), Limits.capacityRangeGiB.upperBound)
        pendingCapacityGiB = value > capacityGiB ? value : nil
    }

    /// A start has finished booting: a pending growth now applies, and the
    /// guest has run its system.
    public mutating func didBoot(release: String) {
        if let pending = pendingCapacityGiB {
            capacityGiB = pending
            pendingCapacityGiB = nil
        }
        systemRelease = release
    }

    /// The same choices with nothing in it: what Erase leaves behind.
    public var erased: Guest {
        var guest = self
        guest.capacityGiB = effectiveCapacityGiB
        guest.pendingCapacityGiB = nil
        guest.systemRelease = nil
        guest.usedBytes = 0
        guest.apps = nil
        return guest
    }

    /// A new guest with the host's choices: every processor, the default
    /// memory the host's ruler reaches, the default capacity.
    public static func fresh(on host: HostDevice) -> Guest {
        Guest(
            cores: host.processorCount, memoryMiB: host.memory.defaultMemoryMiB,
            capacityGiB: Limits.defaultCapacityGiB
        )
    }
}
