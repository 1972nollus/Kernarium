// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import Testing
@testable import Andarium

/// The pieces of the device application that touch the host: the system it
/// carries, the console it reads and the options it keeps.
struct SystemFolderTests {
    private func folder(archive: String? = "android.img.zst", layout: String? = #"{"disk_size_bytes": 3274702848}"#) throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("andarium-system-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        if let archive { try Data("x".utf8).write(to: url.appendingPathComponent(archive)) }
        if let layout { try Data(layout.utf8).write(to: url.appendingPathComponent(SystemFolder.layout)) }
        return url
    }

    @Test func theSeedIsTheArchiveWithTheDeclaredSize() throws {
        let url = try folder()
        defer { try? FileManager.default.removeItem(at: url) }
        let seed = try SystemFolder.seed(in: url, release: "20261031-los24-r4", stagedName: nil)
        #expect(seed.url.lastPathComponent == "android.img.zst")
        #expect(seed.expandedBytes == 3_274_702_848)
        #expect(seed.release == "20261031-los24-r4")
    }

    @Test func aFolderWithoutAnArchiveOrSizeIsRefused() throws {
        let noArchive = try folder(archive: nil)
        let noSize = try folder(layout: #"{"other": 1}"#)
        defer {
            try? FileManager.default.removeItem(at: noArchive)
            try? FileManager.default.removeItem(at: noSize)
        }
        #expect(throws: SystemFolder.Problem.self) { try SystemFolder.seed(in: noArchive, release: "r", stagedName: nil) }
        #expect(throws: SystemFolder.Problem.self) { try SystemFolder.seed(in: noSize, release: "r", stagedName: nil) }
    }

    @Test func aStagedImageThatIsNotThereIsRefused() throws {
        let url = try folder()
        defer { try? FileManager.default.removeItem(at: url) }
        #expect(throws: SystemFolder.Problem.self) {
            try SystemFolder.seed(in: url, release: "r", stagedName: "no-such-image-\(UUID().uuidString).img")
        }
    }
}

struct ConsoleSinkTests {
    private nonisolated final class Collector: @unchecked Sendable {
        private let lock = NSLock()
        private var found: [BootMilestone] = []
        var milestones: [BootMilestone] { lock.withLock { found } }
        func add(_ more: [BootMilestone]) { lock.withLock { found += more } }
    }

    @Test func milestonesArriveFromAnyThreadInOrder() async {
        let collector = Collector()
        let sink = ConsoleSink { collector.add($0) }
        sink.feed(Data("Linux ver".utf8))
        await Task.detached { sink.feed(Data("sion 6.18\ninit: init first stage started!\n".utf8)) }.value
        #expect(collector.milestones == [.kernelRunning, .androidStarting])
    }
}

struct DisplayOptionsStoreTests {
    @Test func optionsSurviveTheStore() {
        let saved = DisplayOptionsStore.load()
        defer { DisplayOptionsStore.save(saved) }
        DisplayOptionsStore.save(DisplayOptions(retina: true, followWindow: false))
        #expect(DisplayOptionsStore.load() == DisplayOptions(retina: true, followWindow: false))
    }
}

struct HostDeviceTests {
    @Test func theHostReadsThisDevice() {
        let host = AppEntry.hostDevice()
        #expect(host.processorCount >= 1)
        #expect(host.memory.limitMiB >= 1024)
        #expect(AppEntry.freeStorageBytes() > 0)
    }
}

/// The system Andarium carries, written the way Create writes it: decoded
/// sparse from the bundled archive and grown to the guest's size. It is the
/// one test that reads the real archive, so it takes tens of seconds.
struct LiveSeedWriterTests {
    private nonisolated final class Progress: @unchecked Sendable {
        private let lock = NSLock()
        private var values: [Double] = []
        var all: [Double] { lock.withLock { values } }
        func add(_ value: Double) { lock.withLock { values.append(value) } }
    }

    @Test func theBundledSystemBecomesAGPTDiskOfTheRequestedSize() async throws {
        let folder = try #require(Bundle.main.resourceURL?.appendingPathComponent(SystemFolder.name))
        let seed = try SystemFolder.seed(in: folder, release: "test", stagedName: nil)
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("andarium-seed-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let destination = directory.appendingPathComponent("guest.img")
        let imageGiB = SingleImageStorage.imageGiB(forCapacity: 8)
        let progress = Progress()

        try await LiveSeedWriter().write(seed: seed, to: destination, imageGiB: imageGiB) { progress.add($0) }

        let size = try #require(try FileManager.default.attributesOfItem(atPath: destination.path)[.size] as? UInt64)
        #expect(size == UInt64(imageGiB) << 30)
        // The GPT header sits at LBA 1 and its backup at the last LBA.
        let handle = try FileHandle(forReadingFrom: destination)
        try handle.seek(toOffset: 512)
        #expect(try handle.read(upToCount: 8) == Data("EFI PART".utf8))
        try handle.seek(toOffset: size - 512)
        #expect(try handle.read(upToCount: 8) == Data("EFI PART".utf8))
        // Progress only moves forward and ends at the whole.
        #expect(progress.all == progress.all.sorted())
        #expect(progress.all.last == 1)
    }
}
