// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Display text. Every size an application shows is binary, in the IEC
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

    /// A part and its whole in the fewest characters, both in the unit the
    /// whole has alone: "1.35/2.5 GiB", "0.96/2.5 GiB", "412/534 MiB".
    public static func size(bytes part: UInt64, ofBytes whole: UInt64) -> String {
        let total = scaled(Double(whole), unit: 0)
        let used = Double(part) / pow(1024, Double(total.unit))
        return "\(number(used))/\(total.number) \(units[total.unit])"
    }

    /// A processor count and its noun: "1 vCPU", "6 vCPUs".
    public static func vcpus(_ count: Int) -> String { "\(count) \(count == 1 ? "vCPU" : "vCPUs")" }

    /// The count of gibibytes in `mib`, without its unit: the labels of a
    /// ruler whose row states the unit.
    public static func gibibytes(mib: Int) -> String { number(Double(mib) / 1024) }

    private static func size(_ value: Double, unit: Int) -> String {
        let scaled = scaled(value, unit: unit)
        return scaled.number + " " + units[scaled.unit]
    }

    /// A value in `units[unit]`, moved up a unit once it reaches 1024 of
    /// the one it is in, and in three significant digits: the number and the
    /// index of its unit.
    private static func scaled(_ value: Double, unit: Int) -> (number: String, unit: Int) {
        var value = max(value, 0)
        var unit = unit
        while unit < units.count - 1, (Double(number(value)) ?? value) >= 1024 {
            value /= 1024
            unit += 1
        }
        return (number(value), unit)
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
