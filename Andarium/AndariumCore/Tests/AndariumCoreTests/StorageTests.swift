// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

/// Writes a small file instead of decoding a seed, and records what it was
/// asked to do.
final class FakeSeedWriter: SeedWriter, @unchecked Sendable {
    struct Failure: Error {}

    private let lock = NSLock()
    private var _writes: [Int] = []
    private var _grows: [Int] = []
    var failNextWrite = false
    /// Bytes the "seed" occupies.
    let seedBytes = 64 * 1024

    var writes: [Int] { lock.withLock { _writes } }
    var grows: [Int] { lock.withLock { _grows } }

    func write(
        seed: SeedImage, to destination: URL, imageGiB: Int,
        progress: @escaping @Sendable (Double) -> Void
    ) async throws {
        progress(0.5)
        try lock.withLock {
            _writes.append(imageGiB)
            if failNextWrite {
                failNextWrite = false
                try Data(count: 4096).write(to: destination)
                throw Failure()
            }
        }
        try Data(repeating: 0xA5, count: seedBytes).write(to: destination)
    }

    func grow(image: URL, toGiB imageGiB: Int) async throws {
        lock.withLock { _grows.append(imageGiB) }
    }
}

private func makeStorage(
    writer: FakeSeedWriter, release: String = "20261031-los24-r4"
) throws -> (SingleImageStorage, URL) {
    let root = FileManager.default.temporaryDirectory
        .appendingPathComponent("andarium-core-tests-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let seed = SeedImage(
        url: root.appendingPathComponent("seed.img.zst"), release: release, expandedBytes: 3_274_702_848
    )
    let storage = SingleImageStorage(
        locations: GuestLocations(directory: root.appendingPathComponent("Guest")),
        seed: seed, appVersion: "1.0 (1)", writer: writer
    )
    return (storage, root)
}

private let host = HostDevice(
    name: "iPad", processorCount: 6, memory: MemoryPolicy(limitMiB: 5000), freeStorageBytes: 40_000_000_000
)

struct StorageTests {
    @Test func aStorageWithNothingInItHasNoGuest() throws {
        let (storage, root) = try makeStorage(writer: FakeSeedWriter())
        defer { try? FileManager.default.removeItem(at: root) }
        #expect(try storage.load() == nil)
    }

    @Test func createWritesTheDiskTheSettingsAndTheInstanceRecord() async throws {
        let writer = FakeSeedWriter()
        let (storage, root) = try makeStorage(writer: writer)
        defer { try? FileManager.default.removeItem(at: root) }

        let created = try await storage.create(Guest.fresh(on: host))
        #expect(writer.writes == [8 + SingleImageStorage.systemRegionGiB])
        #expect(created.usedBytes == 0)
        #expect(FileManager.default.fileExists(atPath: storage.locations.image.path))
        #expect(!FileManager.default.fileExists(atPath: storage.locations.partialImage.path))

        let record = try JSONDecoder().decode(InstanceRecord.self, from: Data(contentsOf: storage.locations.instance))
        #expect(record.dataLayout == "single-image")
        #expect(record.releaseID == "20261031-los24-r4")
        #expect(record.createdWith == "1.0 (1)")
        #expect(record.baselineBytes > 0)

        let loaded = try #require(try storage.load())
        #expect(loaded.cores == created.cores)
        #expect(loaded.capacityGiB == 8)
        #expect(loaded.usedBytes == 0)
    }

    @Test func theInstanceRecordUsesTheDocumentedKeys() async throws {
        let (storage, root) = try makeStorage(writer: FakeSeedWriter())
        defer { try? FileManager.default.removeItem(at: root) }
        _ = try await storage.create(Guest.fresh(on: host))
        let text = try String(contentsOf: storage.locations.instance, encoding: .utf8)
        for key in ["\"schema\"", "\"data_layout\"", "\"release_id\"", "\"created_with\""] {
            #expect(text.contains(key))
        }
        // Schema 0 is the single-image layout's; the two-disk layout's is 1.
        let record = try JSONDecoder().decode(InstanceRecord.self, from: Data(contentsOf: storage.locations.instance))
        #expect(record.schema == 0)
        #expect(record.dataLayout == "single-image")
    }

    @Test func dataAndroidWritesCountsAsUsed() async throws {
        let (storage, root) = try makeStorage(writer: FakeSeedWriter())
        defer { try? FileManager.default.removeItem(at: root) }
        _ = try await storage.create(Guest.fresh(on: host))

        let handle = try FileHandle(forWritingTo: storage.locations.image)
        try handle.seekToEnd()
        try handle.write(contentsOf: Data(repeating: 1, count: 1 << 20))
        try handle.synchronize()
        try handle.close()

        let loaded = try #require(try storage.load())
        #expect(loaded.usedBytes >= 1 << 20)
        #expect(loaded.usedBytes < 2 << 20)
    }

    @Test func aFailedSeedWriteLeavesNoDisk() async throws {
        let writer = FakeSeedWriter()
        let (storage, root) = try makeStorage(writer: writer)
        defer { try? FileManager.default.removeItem(at: root) }
        writer.failNextWrite = true
        await #expect(throws: FakeSeedWriter.Failure.self) {
            _ = try await storage.create(Guest.fresh(on: host))
        }
        #expect(!FileManager.default.fileExists(atPath: storage.locations.image.path))
        #expect(!FileManager.default.fileExists(atPath: storage.locations.partialImage.path))
        #expect(try storage.load() == nil)
    }

    @Test func eraseReplacesTheDiskAndKeepsTheChoices() async throws {
        let writer = FakeSeedWriter()
        let (storage, root) = try makeStorage(writer: writer)
        defer { try? FileManager.default.removeItem(at: root) }
        var guest = try await storage.create(Guest.fresh(on: host))
        guest.microphone = true
        guest.memoryMiB = 1792
        guest.didBoot(release: "20261031-los24-r4")
        guest.setCapacity(16)
        try storage.save(guest)

        let erased = try await storage.erase(guest)
        #expect(writer.writes == [8 + 4, 16 + 4])
        #expect(erased.neverStarted)
        #expect(erased.capacityGiB == 16)
        #expect(erased.pendingCapacityGiB == nil)
        #expect(erased.microphone)
        #expect(erased.memoryMiB == 1792)
        let loaded = try #require(try storage.load())
        #expect(loaded.capacityGiB == 16)
        #expect(loaded.neverStarted)
    }

    @Test func aPendingGrowthGrowsTheDiskAtTheNextStart() async throws {
        let writer = FakeSeedWriter()
        let (storage, root) = try makeStorage(writer: writer)
        defer { try? FileManager.default.removeItem(at: root) }
        var guest = try await storage.create(Guest.fresh(on: host))
        try await storage.prepareStart(guest)
        #expect(writer.grows.isEmpty)

        guest.setCapacity(24)
        try await storage.prepareStart(guest)
        #expect(writer.grows == [24 + 4])
        #expect(writer.writes.count == 1)
    }

    @Test func aMissingDiskIsWrittenAgainAtStart() async throws {
        let writer = FakeSeedWriter()
        let (storage, root) = try makeStorage(writer: writer)
        defer { try? FileManager.default.removeItem(at: root) }
        let guest = try await storage.create(Guest.fresh(on: host))
        try FileManager.default.removeItem(at: storage.locations.image)
        try await storage.prepareStart(guest)
        #expect(writer.writes.count == 2)
        #expect(FileManager.default.fileExists(atPath: storage.locations.image.path))
    }

    @Test func aLeftoverPartialDiskIsRemovedOnLoad() async throws {
        let (storage, root) = try makeStorage(writer: FakeSeedWriter())
        defer { try? FileManager.default.removeItem(at: root) }
        _ = try await storage.create(Guest.fresh(on: host))
        try Data(count: 10).write(to: storage.locations.partialImage)
        _ = try storage.load()
        #expect(!FileManager.default.fileExists(atPath: storage.locations.partialImage.path))
    }

    @Test func creatingNeedsTheExpandedSeedPlusTheFirstStartReserve() throws {
        let (storage, root) = try makeStorage(writer: FakeSeedWriter())
        defer { try? FileManager.default.removeItem(at: root) }
        #expect(storage.createRequirementBytes == 3_274_702_848 + Limits.firstStartReserveBytes)
        #expect(storage.diskPath.hasSuffix("/Guest/guest.img"))
    }

    @Test func theGuestIsExcludedFromBackupsUnlessTheUserAsks() async throws {
        let (storage, root) = try makeStorage(writer: FakeSeedWriter())
        defer { try? FileManager.default.removeItem(at: root) }
        var guest = try await storage.create(Guest.fresh(on: host))

        // A URL caches the resource values it has read, so each read starts
        // from the path.
        func excluded(_ url: URL) throws -> Bool? {
            try URL(fileURLWithPath: url.path).resourceValues(forKeys: [.isExcludedFromBackupKey]).isExcludedFromBackup
        }
        for url in [storage.locations.directory, storage.locations.image, storage.locations.settings, storage.locations.instance] {
            #expect(try excluded(url) == true)
        }

        guest.includeInBackups = true
        try storage.save(guest)
        for url in [storage.locations.directory, storage.locations.image, storage.locations.settings, storage.locations.instance] {
            #expect(try excluded(url) == false)
        }
    }
}

struct ZstdFrameTests {
    private func header(descriptor: UInt8, afterDescriptor: [UInt8]) -> Data {
        Data([0x28, 0xB5, 0x2F, 0xFD, descriptor] + afterDescriptor)
    }

    @Test func eightByteSizeInASingleSegmentFrame() {
        let size: UInt64 = 3_274_702_848
        let bytes = (0..<8).map { UInt8((size >> UInt64(8 * $0)) & 0xFF) }
        #expect(ZstdFrame.contentSize(header: header(descriptor: 0xE0, afterDescriptor: bytes)) == size)
    }

    @Test func oneByteSizeInASingleSegmentFrame() {
        #expect(ZstdFrame.contentSize(header: header(descriptor: 0x20, afterDescriptor: [200, 0])) == 200)
    }

    @Test func twoByteSizeCountsFromTwoFiftySix() {
        // A window byte precedes the size when the frame is not a single segment.
        #expect(ZstdFrame.contentSize(header: header(descriptor: 0x40, afterDescriptor: [0x10, 0x01, 0x00])) == 257)
    }

    @Test func aFrameWithNoDeclaredSize() {
        #expect(ZstdFrame.contentSize(header: header(descriptor: 0x00, afterDescriptor: [0x10, 0, 0])) == nil)
    }

    @Test func notAZstdFrameOrTruncated() {
        #expect(ZstdFrame.contentSize(header: Data([1, 2, 3, 4, 5, 6, 7])) == nil)
        #expect(ZstdFrame.contentSize(header: header(descriptor: 0xE0, afterDescriptor: [1, 2])) == nil)
    }
}
