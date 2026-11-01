// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The ranges, defaults and reserves the application offers.
public enum Limits {
    /// A guest's storage capacity, in whole gigabytes. Storage only grows.
    public static let capacityRangeGiB = 4...64
    public static let defaultCapacityGiB = 8

    /// The memory ruler starts here and counts in steps of this size; its top
    /// comes from `MemoryPolicy`.
    public static let minimumMemoryMiB = 1024
    public static let memoryStepMiB = 256
    /// The default where the ruler reaches it; a lower ruler top wins.
    public static let defaultMemoryMiB = 1536
    /// What the runtime accepts for a guest's memory.
    public static let runtimeMemoryRangeMiB = 512...4096
    public static let runtimeMemoryStepMiB = 64

    /// Free space a new guest needs for Android's first start, on top of the
    /// space its storage takes. Decimal bytes, as iOS reports free space.
    public static let firstStartReserveBytes: UInt64 = 3_000_000_000
    /// Free space any start needs for Android's own writes.
    public static let startReserveBytes: UInt64 = 1_500_000_000

    public static let bytesPerGiB: UInt64 = 1 << 30
}

/// Display text. Sizes the host reports (free space) are decimal gigabytes,
/// as iOS Settings shows them; a guest's capacity and data are binary
/// gigabytes, labelled the same way.
public enum Formats {
    /// A size in gigabytes, as "12 GB", "2.4 GB" or "480 MB".
    public static func gb(_ value: Double) -> String {
        if value <= 0 { return "0 GB" }
        if value < 0.95 { return String(format: "%.0f MB", value * 1000) }
        if value >= 10 || value.rounded() == value { return String(format: "%.0f GB", value) }
        return String(format: "%.1f GB", value)
    }

    /// Host-reported bytes in decimal gigabytes.
    public static func hostGB(bytes: UInt64) -> String { gb(Double(bytes) / 1e9) }

    /// Guest data in binary gigabytes.
    public static func guestGB(bytes: UInt64) -> String { gb(Double(bytes) / Double(Limits.bytesPerGiB)) }

    public static func memoryNumber(_ mib: Int) -> String {
        let gb = Double(mib) / 1024
        if gb.rounded() == gb { return String(format: "%.0f", gb) }
        if (gb * 2).rounded() == gb * 2 { return String(format: "%.1f", gb) }
        if (gb * 4).rounded() == gb * 4 { return String(format: "%.2f", gb) }
        return String(format: "%.1f", gb)
    }

    public static func memory(_ mib: Int) -> String { memoryNumber(mib) + " GB" }

    public static func elapsed(_ seconds: Int) -> String {
        String(format: "%d:%02d", seconds / 60, seconds % 60)
    }
}
