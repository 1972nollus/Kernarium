// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What guest memory the host can honour, at each vCPU count. The bound is
/// the runtime's own budget formula (`runtime/guest_memory_budget.hpp`), which
/// the application reads through the loader the way Kernarium does: the most
/// guest RAM at a vCPU count is the largest multiple of 64 MiB in 512...4096
/// that leaves the host GPU its floor under the memory limit the system sets
/// this process. Each added vCPU holds more of the process's memory, so the
/// bound does not rise with the count. The limit is fixed for the process, so
/// the bounds are read once, for every count the host has.
public struct MemoryPolicy: Equatable, Sendable {
    /// The most guest RAM at 1, 2, ... vCPUs, each on the runtime's steps and
    /// inside its range.
    private let maximums: [Int]

    /// The bounds as they are: each is taken down to the runtime's steps and
    /// into its range.
    public init(maximumsMiB: [Int]) {
        let values = maximumsMiB.isEmpty ? [Limits.runtimeMemoryRangeMiB.upperBound] : maximumsMiB
        maximums = values.map { Self.steppedDown($0) }
    }

    /// The bounds of 1...`processorCount` vCPUs from `bound`, the loader's
    /// answer for a count.
    public init(processorCount: Int, bound: (Int) -> Int) {
        self.init(maximumsMiB: (1...max(processorCount, 1)).map(bound))
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

    /// What a new guest gets at `vCPUs` vCPUs: the bound (the safe limit),
    /// rounded down to a 0.5 GiB step and held within 1.5 to 4 GiB, and never
    /// above the bound itself. A Mac, which sets no limit, has the 4 GiB top.
    public func defaultMemoryMiB(vCPUs: Int) -> Int {
        let bound = maximumMiB(vCPUs: vCPUs)
        let step = Limits.defaultMemoryStepMiB
        let range = Limits.defaultMemoryRangeMiB
        let held = min(max(bound / step * step, range.lowerBound), range.upperBound)
        return min(held, bound)
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
