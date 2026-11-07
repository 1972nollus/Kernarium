// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What guest memory the host can honour, at each vCPU count. Both figures are
/// the runtime's own (`runtime/guest_memory_budget.hpp`), which the application
/// reads through the loader the way Kernarium does, and neither is computed
/// here:
///
/// - the bound is the most a person may choose: the largest multiple of 64 MiB
///   in 512...4096 that leaves the host GPU its floor under the memory limit
///   the system sets this process;
/// - the default is what a new guest gets, and the line above which iOS is
///   likely to end the application: the same measure with the launch's
///   conservative allowances for the application, the rest of the engine and
///   headroom, in 0.5 GiB steps and within 1.5 to 4 GiB, and never above the
///   bound.
///
/// Each added vCPU holds more of the process's memory, so neither rises with
/// the count. The limit is fixed for the process, so they are read once, for
/// every count the host has.
public struct MemoryPolicy: Equatable, Sendable {
    /// The most guest RAM at 1, 2, ... vCPUs, each on the runtime's steps and
    /// inside its range.
    private let maximums: [Int]
    /// What a new guest gets at the same counts, on the same steps.
    private let defaults: [Int]

    /// The bounds and the defaults as they are: each is taken down to the
    /// runtime's steps and into its range. A count without a default takes
    /// its bound.
    public init(maximumsMiB: [Int], defaultsMiB: [Int]) {
        let values = maximumsMiB.isEmpty ? [Limits.runtimeMemoryRangeMiB.upperBound] : maximumsMiB
        let bounds = values.map { Self.steppedDown($0) }
        maximums = bounds
        defaults = bounds.indices.map { index in
            index < defaultsMiB.count ? Self.steppedDown(defaultsMiB[index]) : bounds[index]
        }
    }

    /// The bounds alone: every default is its bound, so nothing is above the
    /// default. A host that sets no limit, and the fixtures that do not exercise
    /// the default.
    public init(maximumsMiB: [Int]) {
        self.init(maximumsMiB: maximumsMiB, defaultsMiB: [])
    }

    /// The bounds of 1...`processorCount` vCPUs from `bound`, the loader's
    /// answer for a count; every default is its bound.
    public init(processorCount: Int, bound: (Int) -> Int) {
        self.init(maximumsMiB: (1...max(processorCount, 1)).map(bound))
    }

    /// The bounds and the defaults of 1...`processorCount` vCPUs from the
    /// loader's answers for a count.
    public init(processorCount: Int, bound: (Int) -> Int, defaultMemory: (Int) -> Int) {
        let counts = 1...max(processorCount, 1)
        self.init(maximumsMiB: counts.map(bound), defaultsMiB: counts.map(defaultMemory))
    }

    /// The most guest RAM the picker offers at `vCPUs` vCPUs; a count above the
    /// host's takes the last bound.
    public func maximumMiB(vCPUs: Int) -> Int {
        maximums[min(max(vCPUs, 1), maximums.count) - 1]
    }

    /// Every position of the picker at `vCPUs` vCPUs: 512 MiB up to the bound,
    /// in the runtime's 64 MiB steps.
    public func stops(vCPUs: Int) -> [Int] {
        Array(stride(
            from: Limits.minimumMemoryMiB, through: maximumMiB(vCPUs: vCPUs), by: Limits.memoryStepMiB
        ))
    }

    /// What a new guest gets at `vCPUs` vCPUs; a count above the host's takes
    /// the last one.
    public func defaultMemoryMiB(vCPUs: Int) -> Int {
        defaults[min(max(vCPUs, 1), defaults.count) - 1]
    }

    /// Whether a guest with `mib` of memory and `vCPUs` vCPUs has more than a
    /// new guest gets: iOS is then likely to end the application while the
    /// guest runs. The same figure on Home and on the memory picker, so they
    /// agree at every size and count.
    public func isAboveDefault(_ mib: Int, vCPUs: Int) -> Bool {
        mib > defaultMemoryMiB(vCPUs: vCPUs)
    }

    /// A stored or requested size at `vCPUs` vCPUs: on the nearest step in the
    /// runtime's range, and no more than the bound.
    public func clamped(_ mib: Int, vCPUs: Int) -> Int {
        min(Self.nearestStep(mib), maximumMiB(vCPUs: vCPUs))
    }

    /// Whether the runtime accepts `mib` (a value of its own step in range).
    public static func runtimeAccepts(_ mib: Int) -> Bool {
        Limits.runtimeMemoryRangeMiB.contains(mib)
            && (mib - Limits.runtimeMemoryRangeMiB.lowerBound) % Limits.runtimeMemoryStepMiB == 0
    }

    private static func nearestStep(_ mib: Int) -> Int {
        let range = Limits.runtimeMemoryRangeMiB
        let offset = min(max(mib, range.lowerBound), range.upperBound) - range.lowerBound
        let step = Limits.runtimeMemoryStepMiB
        return range.lowerBound + (offset + step / 2) / step * step
    }

    private static func steppedDown(_ mib: Int) -> Int {
        let range = Limits.runtimeMemoryRangeMiB
        let offset = min(max(mib, range.lowerBound), range.upperBound) - range.lowerBound
        return range.lowerBound + offset / Limits.runtimeMemoryStepMiB * Limits.runtimeMemoryStepMiB
    }
}

/// What the application says when a guest has more memory than a new one gets
/// (`MemoryPolicy.isAboveDefault`): iOS is then likely to close Andarium while
/// the guest runs. Home and the memory picker use these words, so they say the
/// same thing.
public enum MemoryWarning {
    public static let title = "Guest Memory Is High"

    /// Home's notice: the guest's size, the size that is safer, and where to
    /// lower it.
    public static func explanation(memoryMiB: Int, safeMiB: Int, deviceName: String) -> String {
        "iOS is likely to close Andarium with \(Formats.size(mib: memoryMiB)) of guest memory on this "
            + "\(deviceName). \(Formats.size(mib: safeMiB)) or less is safer. "
            + "Lower it with Edit under Guest System."
    }

    /// The line beneath the memory picker.
    public static func note(safeMiB: Int) -> String {
        "iOS is likely to close Andarium above \(Formats.size(mib: safeMiB))."
    }
}
