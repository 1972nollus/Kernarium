// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What the iPhone, iPad or Mac running Andarium offers.
public struct HostDevice: Equatable, Sendable {
    public var name: String
    public var processorCount: Int
    public var memory: MemoryPolicy
    public var freeStorageBytes: UInt64
    /// Whether this is a Mac running the iPad application, which sets no
    /// memory limit.
    public var isMac: Bool

    public init(
        name: String, processorCount: Int, memory: MemoryPolicy, freeStorageBytes: UInt64, isMac: Bool = false
    ) {
        self.name = name
        self.processorCount = max(1, processorCount)
        self.memory = memory
        self.freeStorageBytes = freeStorageBytes
        self.isMac = isMac
    }

    public var freeStorageGB: Double { Double(freeStorageBytes) / 1e9 }

    public func hasRoom(for bytes: UInt64) -> Bool { freeStorageBytes >= bytes }
}
