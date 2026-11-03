// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the iPhone, iPad or Mac running Andarium offers.
public struct HostDevice: Equatable, Sendable {
    public var name: String
    /// The vCPUs a guest may have, and the count a new guest takes: every
    /// processor of the device, at most the count the memory budget is
    /// measured for. The application states it from the loader
    /// (`GuestProcessorLimit`), so it is also the number of rows `memory` has
    /// and of positions the processor ruler offers.
    public var processorCount: Int
    /// The processors the device itself has, which Info states.
    public var physicalProcessorCount: Int
    public var memory: MemoryPolicy
    public var freeStorageBytes: UInt64
    /// Whether this is a Mac running the iPad application, which sets no
    /// memory limit.
    public var isMac: Bool

    /// `processorCount` is the vCPU limit; the device's own processors default
    /// to it.
    public init(
        name: String, processorCount: Int, memory: MemoryPolicy, freeStorageBytes: UInt64, isMac: Bool = false,
        physicalProcessorCount: Int? = nil
    ) {
        self.name = name
        self.processorCount = max(1, processorCount)
        self.physicalProcessorCount = max(self.processorCount, physicalProcessorCount ?? 0)
        self.memory = memory
        self.freeStorageBytes = freeStorageBytes
        self.isMac = isMac
    }

    public var freeStorageGB: Double { Formats.decimalGB(bytes: freeStorageBytes) }

    public func hasRoom(for bytes: UInt64) -> Bool { freeStorageBytes >= bytes }
}
