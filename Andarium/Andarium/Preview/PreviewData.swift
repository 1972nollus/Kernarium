import AndariumCore
import UIKit

/// Mock devices, system and guests for the Simulator preview and its
/// screenshots. Nothing here is read from a real device or runtime.
enum PreviewData {
    /// The release the mock system carries, and the one before it.
    static let release = "20261024"
    static let previousRelease = "20261002"

    static let system = SystemInfo(
        distribution: "LineageOS 24", android: "17", release: release,
        securityPatch: "5 October 2026", kernel: "Linux 6.18.54",
        architecture: "arm64, 4 KB pages", appVersion: "1.0 (1)"
    )

    /// An iPad Pro or an iPhone, by idiom. The memory limits are the ones the
    /// provisional budget terms turn into a wide iPad ruler and a short iPhone
    /// one.
    static func host(lowSpace: Bool) -> HostDevice {
        if UIDevice.current.userInterfaceIdiom == .pad {
            return HostDevice(
                name: "iPad", processorCount: 10, memory: MemoryPolicy(limitMiB: 12288),
                freeStorageBytes: lowSpace ? 1_200_000_000 : 118_300_000_000
            )
        }
        return HostDevice(
            name: "iPhone", processorCount: 6, memory: MemoryPolicy(limitMiB: 3800),
            freeStorageBytes: lowSpace ? 1_200_000_000 : 41_600_000_000
        )
    }

    /// What the mock system takes on the device.
    static let systemBytes: UInt64 = 2_800_000_000

    /// The seed the mock storage would write: about as large as the real one.
    static let createRequirementBytes: UInt64 = 3_274_702_848 + Limits.firstStartReserveBytes

    /// A guest in everyday use.
    static func sampleGuest(on host: HostDevice) -> Guest {
        Guest(
            cores: host.processorCount, memoryMiB: min(3072, host.memory.rulerTopMiB), capacityGiB: 16,
            usedBytes: UInt64(5.4 * Double(Limits.bytesPerGiB)), apps: 23
        )
    }
}
