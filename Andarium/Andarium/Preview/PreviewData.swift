// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import UIKit

/// Mock devices, system and guests for the Simulator preview and its
/// screenshots. Nothing here is read from a real device or runtime.
enum PreviewData {
    /// The release the mock system carries, and the one before it.
    static let release = "20261024"
    static let previousRelease = "20261002"

    static let system = SystemInfo(
        version: "17", release: release,
        securityPatch: "5 October 2026", kernel: "Linux 6.18.54",
        architecture: "arm64, 4 KiB pages", appVersion: "1.0 (1)"
    )

    /// An iPad Pro or an iPhone, by idiom. The memory bounds are made-up ones
    /// of a wide iPad ruler and a short iPhone one that falls with the vCPU
    /// count; a device reads them from the runtime's budget formula. The iPad
    /// has ten cores and offers eight vCPUs, the most the budget is measured
    /// for.
    static func host(lowSpace: Bool) -> HostDevice {
        if UIDevice.current.userInterfaceIdiom == .pad {
            return HostDevice(
                name: "iPad", processorCount: 8, memory: MemoryPolicy(maximumsMiB: Array(repeating: 4096, count: 8)),
                freeStorageBytes: gibibytes(lowSpace ? 1.2 : 118.3)
            )
        }
        return HostDevice(
            name: "iPhone", processorCount: 6,
            memory: MemoryPolicy(maximumsMiB: [3136, 3072, 3008, 2944, 2880, 2816]),
            freeStorageBytes: gibibytes(lowSpace ? 1.2 : 41.6)
        )
    }

    /// A mock size, in gibibytes.
    private static func gibibytes(_ count: Double) -> UInt64 { UInt64(count * Double(Limits.bytesPerGiB)) }

    /// The seed the mock storage would write: about as large as the real one.
    static let createRequirementBytes: UInt64 = 3_274_702_848 + Limits.firstStartReserveBytes

    /// A guest in everyday use.
    static func sampleGuest(on host: HostDevice) -> Guest {
        Guest(
            cores: host.processorCount, memoryMiB: min(3072, host.memory.maximumMiB(vCPUs: host.processorCount)), capacityGiB: 16,
            usedBytes: gibibytes(5.4)
        )
    }

    /// A sparse file named like the sample APK, for the preview's install.
    static func sampleApk() -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(ApkInstall.sampleFile)
        if !FileManager.default.fileExists(atPath: url.path) {
            FileManager.default.createFile(atPath: url.path, contents: nil)
            if let handle = try? FileHandle(forWritingTo: url) {
                try? handle.truncate(atOffset: ApkInstall.sampleSizeBytes)
                try? handle.close()
            }
        }
        return url
    }
}
