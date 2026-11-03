// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

public enum StartMode: String, Hashable, Codable, Sendable {
    case normal, recovery
}

/// Why a start takes as long as it does. An update is a normal start of a
/// newer Android; the instance on disk decides which a start is
/// (`StartPlan`).
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

    // Measured when the guest is loaded; never stored.

    /// Data the guest has written: the space its storage occupies.
    public var usedBytes: UInt64 = 0
    /// How many apps Android has installed, where the runtime can tell.
    public var apps: Int?

    private enum CodingKeys: String, CodingKey {
        case cores, memoryMiB, capacityGiB, pendingCapacityGiB, microphone
        case includeInBackups
    }

    public init(
        cores: Int, memoryMiB: Int, capacityGiB: Int, pendingCapacityGiB: Int? = nil,
        microphone: Bool = false, includeInBackups: Bool = false,
        usedBytes: UInt64 = 0, apps: Int? = nil
    ) {
        self.cores = cores
        self.memoryMiB = memoryMiB
        self.capacityGiB = capacityGiB
        self.pendingCapacityGiB = pendingCapacityGiB
        self.microphone = microphone
        self.includeInBackups = includeInBackups
        self.usedBytes = usedBytes
        self.apps = apps
    }

    public var usedGB: Double { Double(usedBytes) / Double(Limits.bytesPerGiB) }

    /// The capacity the next start runs with.
    public var effectiveCapacityGiB: Int { pendingCapacityGiB ?? capacityGiB }

    /// Storage only grows, up to the largest capacity there is.
    public var canGrow: Bool { effectiveCapacityGiB < Limits.capacityRangeGiB.upperBound }

    /// Resize: storage only grows, and the growth waits for the next start.
    /// Choosing the current capacity again cancels a growth not yet applied.
    public mutating func setCapacity(_ value: Int) {
        let value = min(max(value, capacityGiB), Limits.capacityRangeGiB.upperBound)
        pendingCapacityGiB = value > capacityGiB ? value : nil
    }

    /// A start has finished booting: a pending growth now applies.
    public mutating func didBoot() {
        if let pending = pendingCapacityGiB {
            capacityGiB = pending
            pendingCapacityGiB = nil
        }
    }

    /// The same choices with nothing in it: what Erase leaves behind.
    public var erased: Guest {
        var guest = self
        guest.capacityGiB = effectiveCapacityGiB
        guest.pendingCapacityGiB = nil
        guest.usedBytes = 0
        guest.apps = nil
        return guest
    }

    /// A new guest with the host's choices: every vCPU the host offers (its
    /// processors, at most the count the memory budget is measured for), the
    /// default memory at that count, the default capacity.
    public static func fresh(on host: HostDevice) -> Guest {
        Guest(
            cores: host.processorCount,
            memoryMiB: host.memory.defaultMemoryMiB(vCPUs: host.processorCount, onMac: host.isMac),
            capacityGiB: Limits.defaultCapacityGiB
        )
    }

    /// The processor ruler's choice. Each added vCPU holds more of the
    /// process's memory, so raising the count takes memory above the new
    /// bound down to it; lowering the count leaves memory as it is.
    public mutating func setCores(_ count: Int, on host: HostDevice) {
        let raised = count > cores
        cores = count
        if raised { memoryMiB = min(memoryMiB, host.memory.maximumMiB(vCPUs: count)) }
    }

    /// The memory this guest loads with: a stored size above the bound of its
    /// vCPU count, or off the steps, comes to the nearest allowed size.
    public func memoryWithinBound(on host: HostDevice) -> Guest {
        var guest = self
        guest.memoryMiB = host.memory.clamped(memoryMiB, vCPUs: cores)
        return guest
    }
}
