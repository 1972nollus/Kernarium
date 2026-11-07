// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The ranges, defaults and reserves the application offers.
public enum Limits {
    /// A guest's storage capacity, in whole gibibytes, which the rulers offer
    /// in steps of `capacityStepGiB`. Storage only grows.
    public static let capacityRangeGiB = 4...128
    public static let capacityStepGiB = 4
    public static let defaultCapacityGiB = 8

    /// The sizes the storage rulers offer: 4, 8, 12 GiB and on in steps of
    /// `capacityStepGiB`, through the largest the system the application
    /// carries accepts (its data disk's maximum), and never above the range.
    public static func capacityStopsGiB(releaseMaximumGiB: Int = capacityRangeGiB.upperBound) -> [Int] {
        let top = min(releaseMaximumGiB, capacityRangeGiB.upperBound)
        guard top >= capacityRangeGiB.lowerBound else { return [capacityRangeGiB.lowerBound] }
        return Array(stride(from: capacityRangeGiB.lowerBound, through: top, by: capacityStepGiB))
    }

    /// What the runtime accepts for a guest's memory, in steps of 64 MiB. The
    /// memory ruler starts at the bottom of it and counts in the same steps;
    /// its top at a vCPU count comes from `MemoryPolicy`.
    public static let runtimeMemoryRangeMiB = 512...4096
    public static let runtimeMemoryStepMiB = 64
    public static let minimumMemoryMiB = runtimeMemoryRangeMiB.lowerBound
    public static let memoryStepMiB = runtimeMemoryStepMiB
    /// The range a new guest's memory is held within, in half gibibytes:
    /// `MemoryPolicy.defaultMemoryMiB` states the figure, from the runtime's
    /// own rule. A device whose limit leaves less than the bottom of it takes
    /// the bound of its vCPU count instead.
    public static let defaultMemoryRangeMiB = 1536...4096

    /// Free space a new guest needs for Android's first start, on top of the
    /// space its storage takes.
    public static let firstStartReserveBytes: UInt64 = 3 * bytesPerGiB
    /// Free space any start needs for Android's own writes.
    public static let startReserveBytes: UInt64 = bytesPerGiB * 3 / 2

    public static let bytesPerMiB: UInt64 = 1 << 20
    public static let bytesPerGiB: UInt64 = 1 << 30
}

/// Display text. Every size the application shows is binary, in the IEC
/// units KiB, MiB and GiB: the host's free space, the guest's storage and
/// memory, the system and an installed package. One formatter states them all.
public enum Formats {
    private static let units = ["B", "KiB", "MiB", "GiB", "TiB"]

    /// A size in bytes, as "480 MiB", "5.4 GiB" or "118 GiB".
    public static func size(bytes: UInt64) -> String { size(Double(bytes), unit: 0) }

    /// A size in mebibytes, as "512 MiB" or "1.25 GiB".
    public static func size(mib: Int) -> String { size(Double(mib), unit: 2) }

    /// A size in whole gibibytes, as "8 GiB".
    public static func size(gib: Int) -> String { size(Double(gib), unit: 3) }

    /// A processor count and its noun: "1 vCPU", "6 vCPUs".
    public static func vcpus(_ count: Int) -> String { "\(count) \(count == 1 ? "vCPU" : "vCPUs")" }

    /// The count of gibibytes in `mib`, without its unit: the labels of a
    /// ruler whose row states the unit.
    public static func gibibytes(mib: Int) -> String { number(Double(mib) / 1024) }

    /// A value in `units[unit]`, moved up a unit once it reaches 1024 of
    /// the one it is in, and in three significant digits.
    private static func size(_ value: Double, unit: Int) -> String {
        var value = max(value, 0)
        var unit = unit
        while unit < units.count - 1, (Double(number(value)) ?? value) >= 1024 {
            value /= 1024
            unit += 1
        }
        return number(value) + " " + units[unit]
    }

    /// Three significant digits, without trailing zeros: "1.25", "12.6", "118".
    private static func number(_ value: Double) -> String {
        let places = value < 10 ? 2 : value < 100 ? 1 : 0
        var text = String(format: "%.\(places)f", value)
        if places > 0 {
            while text.hasSuffix("0") { text.removeLast() }
            if text.hasSuffix(".") { text.removeLast() }
        }
        return text
    }

    public static func elapsed(_ seconds: Int) -> String {
        String(format: "%d:%02d", seconds / 60, seconds % 60)
    }
}
