// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The terms of the launch-time memory budget, in MiB. The guest's graphics
/// budget is B = clamp(L - A - G - E - S, floor, G / 2); the largest guest
/// memory G that leaves B at its floor is the recommended maximum. The
/// values are measured provisional ones (IOS-MEM, the iPad mini 6): the user
/// has not chosen the budget set, and replacing it changes only this value.
public struct MemoryTerms: Equatable, Sendable {
    /// A: the application's own ceiling.
    public var appMiB: Int
    /// E: the execution engine's caches and the runtime's other memory.
    public var engineMiB: Int
    /// S: safe headroom under the limit.
    public var headroomMiB: Int
    /// The smallest graphics budget.
    public var graphicsFloorMiB: Int

    public init(appMiB: Int, engineMiB: Int, headroomMiB: Int, graphicsFloorMiB: Int) {
        self.appMiB = appMiB
        self.engineMiB = engineMiB
        self.headroomMiB = headroomMiB
        self.graphicsFloorMiB = graphicsFloorMiB
    }

    public static let provisional = MemoryTerms(
        appMiB: 82, engineMiB: 560, headroomMiB: 270, graphicsFloorMiB: 384
    )
}

/// What guest memory the host can honour. The ruler runs from 1 GB in steps
/// of 256 MB, and its top is one step below the recommended maximum, so no
/// value above the maximum can be chosen.
public struct MemoryPolicy: Equatable, Sendable {
    /// L: the memory iOS lets the application use, in MiB.
    public var limitMiB: Int
    public var terms: MemoryTerms

    public init(limitMiB: Int, terms: MemoryTerms = .provisional) {
        self.limitMiB = limitMiB
        self.terms = terms
    }

    /// The largest guest memory that keeps the graphics budget at its floor,
    /// rounded down to the runtime's step and capped at the runtime's range.
    public var recommendedMaximumMiB: Int {
        let free = limitMiB - terms.appMiB - terms.engineMiB
            - terms.headroomMiB - terms.graphicsFloorMiB
        let capped = min(free, Limits.runtimeMemoryRangeMiB.upperBound)
        let step = Limits.runtimeMemoryStepMiB
        return max(Limits.minimumMemoryMiB, capped / step * step)
    }

    /// The ruler's top: the largest ruler position one step below the
    /// recommended maximum, never below the first position.
    public var rulerTopMiB: Int {
        let below = recommendedMaximumMiB - Limits.memoryStepMiB
        let steps = max(0, (below - Limits.minimumMemoryMiB) / Limits.memoryStepMiB)
        return Limits.minimumMemoryMiB + steps * Limits.memoryStepMiB
    }

    /// Every position on the ruler.
    public var stops: [Int] {
        Array(stride(from: Limits.minimumMemoryMiB, through: rulerTopMiB, by: Limits.memoryStepMiB))
    }

    /// 1536 MB where the ruler reaches it, otherwise the ruler's top.
    public var defaultMemoryMiB: Int {
        min(Limits.defaultMemoryMiB, rulerTopMiB)
    }

    /// A stored or requested value on the ruler: the nearest position at or
    /// below it, never above the top. A value the runtime accepts but the
    /// ruler lacks (an automation launch's 1536) is kept by the caller, not
    /// here.
    public func rulerValue(_ mib: Int) -> Int {
        let clamped = min(max(mib, Limits.minimumMemoryMiB), rulerTopMiB)
        let steps = (clamped - Limits.minimumMemoryMiB) / Limits.memoryStepMiB
        return Limits.minimumMemoryMiB + steps * Limits.memoryStepMiB
    }

    /// Whether the runtime accepts `mib` (a value of its own step in range).
    public static func runtimeAccepts(_ mib: Int) -> Bool {
        Limits.runtimeMemoryRangeMiB.contains(mib)
            && (mib - Limits.runtimeMemoryRangeMiB.lowerBound) % Limits.runtimeMemoryStepMiB == 0
    }
}
