// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The system image a guest is created from: a raw GPT disk, or its zstd
/// archive, and the release it carries.
public struct SeedImage: Equatable, Sendable {
    public var url: URL
    public var release: String
    /// The size of the disk once expanded.
    public var expandedBytes: UInt64

    public init(url: URL, release: String, expandedBytes: UInt64) {
        self.url = url
        self.release = release
        self.expandedBytes = expandedBytes
    }
}

/// What a zstd frame header declares about its content.
public enum ZstdFrame {
    /// The decompressed size the frame declares, if it declares one.
    public static func contentSize(header: Data) -> UInt64? {
        let bytes = [UInt8](header)
        guard bytes.count >= 6, Array(bytes[0..<4]) == [0x28, 0xB5, 0x2F, 0xFD] else { return nil }
        let descriptor = bytes[4]
        let singleSegment = descriptor & 0x20 != 0
        let dictionaryBytes = [0, 1, 2, 4][Int(descriptor & 0x03)]
        let width = [singleSegment ? 1 : 0, 2, 4, 8][Int(descriptor >> 6)]
        let offset = 5 + (singleSegment ? 0 : 1) + dictionaryBytes
        guard width > 0, bytes.count >= offset + width else { return nil }
        var value: UInt64 = 0
        for index in 0..<width {
            value |= UInt64(bytes[offset + index]) << UInt64(8 * index)
        }
        // A two-byte size counts from 256.
        return width == 2 ? value + 256 : value
    }
}

/// Turns a seed into a bootable disk. The application supplies it, because
/// zstd decoding and GPT editing belong to the host code the applications
/// share, not to this package.
public protocol SeedWriter: Sendable {
    /// Writes the seed to `destination` as a disk of `imageGiB` gibibytes
    /// whose Android data partition fills the added space.
    /// `progress` reports the fraction written, from 0 to 1, from any thread.
    func write(
        seed: SeedImage, to destination: URL, imageGiB: Int,
        progress: @escaping @Sendable (Double) -> Void
    ) async throws
    /// Grows the disk at `image` to `imageGiB` gibibytes.
    func grow(image: URL, toGiB imageGiB: Int) async throws
}

/// What a guest keeps on disk, and how the application reaches it. The first
/// release keeps one disk image per guest (`SingleImageStorage`); a layout
/// with the system apart from the data replaces it behind this protocol.
public protocol GuestStorage: Sendable {
    /// Free space the host needs before a guest can be created.
    var createRequirementBytes: UInt64 { get }
    /// The disk the runtime boots.
    var diskPath: String { get }
    /// The guest on disk, with its measured data, or nil if there is none.
    func load() throws -> Guest?
    func create(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest
    /// Everything in the guest is replaced by a fresh disk.
    func erase(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest
    /// Makes the disk ready for a start: restores a missing disk and applies
    /// a pending growth.
    func prepareStart(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws
    /// Records the guest's choices.
    func save(_ guest: Guest) throws
}

extension GuestStorage {
    public func create(_ guest: Guest) async throws -> Guest {
        try await create(guest, progress: { _ in })
    }

    public func erase(_ guest: Guest) async throws -> Guest {
        try await erase(guest, progress: { _ in })
    }

    public func prepareStart(_ guest: Guest) async throws {
        try await prepareStart(guest, progress: { _ in })
    }
}

/// Where a guest keeps its files, under the application's Application
/// Support directory.
public struct GuestLocations: Equatable, Sendable {
    public var directory: URL

    public init(directory: URL) {
        self.directory = directory
    }

    public var image: URL { directory.appendingPathComponent("guest.img") }
    public var partialImage: URL { directory.appendingPathComponent("guest.img.partial") }
    public var settings: URL { directory.appendingPathComponent("settings.json") }
    public var instance: URL { directory.appendingPathComponent("instance.json") }
}

/// `instance.json` of the interim single-image layout: what the guest's disk
/// was made from. Its schema is 0 so that the two-disk layout's `instance.json`
/// (schema 1, `android/lineage-24/README.md`) never mistakes it for its own;
/// a guest that carries this record is a single-image guest.
public struct InstanceRecord: Codable, Equatable, Sendable {
    public var schema = 0
    /// Always "single-image" in the first release.
    public var dataLayout: String
    public var releaseID: String
    public var createdWith: String
    /// What the disk occupied right after the seed was written: the system
    /// itself, so the data Android has written is what the disk gained.
    public var baselineBytes: UInt64

    private enum CodingKeys: String, CodingKey {
        case schema
        case dataLayout = "data_layout"
        case releaseID = "release_id"
        case createdWith = "created_with"
        case baselineBytes = "baseline_bytes"
    }

    public init(releaseID: String, createdWith: String, baselineBytes: UInt64) {
        dataLayout = "single-image"
        self.releaseID = releaseID
        self.createdWith = createdWith
        self.baselineBytes = baselineBytes
    }
}

public enum StorageError: Error, Equatable {
    case notEnoughSpace(neededBytes: UInt64)
    case missingSeed
}

/// One disk image per guest: the whole system and the guest's data in a
/// single GPT image written sparse from the seed.
///
/// This is the interim layout. The system sits inside every guest's image,
/// costing the seed's size again per guest and a seed write (tens of
/// seconds) at Create and Erase, and a new system reaches an existing guest
/// only by erasing it.
public struct SingleImageStorage: GuestStorage {
    /// The seed's own partitions before the data partition grows, rounded
    /// up: an image of N GB of data is N + this many GiB.
    public static let systemRegionGiB = 4

    public let locations: GuestLocations
    public let seed: SeedImage
    public let appVersion: String
    private let writer: any SeedWriter

    public init(locations: GuestLocations, seed: SeedImage, appVersion: String, writer: any SeedWriter) {
        self.locations = locations
        self.seed = seed
        self.appVersion = appVersion
        self.writer = writer
    }

    public static func imageGiB(forCapacity capacityGiB: Int) -> Int {
        capacityGiB + systemRegionGiB
    }

    public var createRequirementBytes: UInt64 {
        seed.expandedBytes + Limits.firstStartReserveBytes
    }

    public var diskPath: String { locations.image.path }

    public func load() throws -> Guest? {
        let manager = FileManager.default
        try? manager.removeItem(at: locations.partialImage)
        guard manager.fileExists(atPath: locations.settings.path) else { return nil }
        var guest = try JSONDecoder().decode(Guest.self, from: Data(contentsOf: locations.settings))
        guest.usedBytes = measuredUsedBytes()
        return guest
    }

    public func create(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest {
        try FileManager.default.createDirectory(at: locations.directory, withIntermediateDirectories: true)
        try await writeSeed(capacityGiB: guest.effectiveCapacityGiB, progress: progress)
        var created = guest.erased
        created.usedBytes = 0
        try save(created)
        applyBackupPolicy(created)
        return created
    }

    public func erase(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest {
        // The old disk is going away either way; removing it first keeps
        // the host from holding two.
        try? FileManager.default.removeItem(at: locations.image)
        return try await create(guest, progress: progress)
    }

    public func prepareStart(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws {
        let manager = FileManager.default
        if !manager.fileExists(atPath: locations.image.path) {
            try await writeSeed(capacityGiB: guest.effectiveCapacityGiB, progress: progress)
            applyBackupPolicy(guest)
            return
        }
        if guest.pendingCapacityGiB != nil {
            try await writer.grow(image: locations.image, toGiB: Self.imageGiB(forCapacity: guest.effectiveCapacityGiB))
        }
    }

    public func save(_ guest: Guest) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(guest).write(to: locations.settings, options: .atomic)
        applyBackupPolicy(guest)
    }

    /// The data Android has written: what the disk occupies beyond the
    /// system it started with.
    func measuredUsedBytes() -> UInt64 {
        // A URL caches the resource values it has read, and the disk changes
        // under it while the guest runs, so each measure starts from the path.
        let values = try? URL(fileURLWithPath: locations.image.path).resourceValues(forKeys: [.fileAllocatedSizeKey])
        let allocated = UInt64(values?.fileAllocatedSize ?? 0)
        let baseline = (try? JSONDecoder().decode(InstanceRecord.self, from: Data(contentsOf: locations.instance)))?
            .baselineBytes ?? 0
        return allocated > baseline ? allocated - baseline : 0
    }

    private func writeSeed(capacityGiB: Int, progress: @escaping @Sendable (Double) -> Void) async throws {
        let manager = FileManager.default
        try? manager.removeItem(at: locations.partialImage)
        do {
            try await writer.write(
                seed: seed, to: locations.partialImage,
                imageGiB: Self.imageGiB(forCapacity: capacityGiB), progress: progress
            )
            try? manager.removeItem(at: locations.image)
            try manager.moveItem(at: locations.partialImage, to: locations.image)
        } catch {
            try? manager.removeItem(at: locations.partialImage)
            throw error
        }
        // A URL caches the resource values it has read, and the disk changes
        // under it while the guest runs, so each measure starts from the path.
        let values = try? URL(fileURLWithPath: locations.image.path).resourceValues(forKeys: [.fileAllocatedSizeKey])
        let record = InstanceRecord(
            releaseID: seed.release, createdWith: appVersion,
            baselineBytes: UInt64(values?.fileAllocatedSize ?? 0)
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(record).write(to: locations.instance, options: .atomic)
    }

    /// iCloud and computer backups include the guest's files only when the
    /// user chose that for the guest. The flag sits on the guest's directory,
    /// which covers whatever is later put in it, and on its files.
    private func applyBackupPolicy(_ guest: Guest) {
        var excluded = URLResourceValues()
        excluded.isExcludedFromBackup = !guest.includeInBackups
        for url in [locations.directory, locations.image, locations.instance, locations.settings] {
            var target = url
            try? target.setResourceValues(excluded)
        }
    }
}
