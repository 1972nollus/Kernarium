import Foundation
import Testing
@testable import Kernarium

struct KernariumTests {
    @Test
    @MainActor
    func overlayDiskIsRecreatedAtRequestedSize() throws {
        let manager = FileManager.default
        let directory = manager.temporaryDirectory.appendingPathComponent(
            UUID().uuidString,
            isDirectory: true
        )
        try manager.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? manager.removeItem(at: directory) }

        let first = try OverlayDisk.recreate(sizeGiB: 3, in: directory)
        let writer = try FileHandle(forWritingTo: first)
        try writer.write(contentsOf: Data([0xa5]))
        try writer.close()

        let second = try OverlayDisk.recreate(sizeGiB: 3, in: directory)
        let attributes = try manager.attributesOfItem(atPath: second.path)
        let size = attributes[.size] as? NSNumber
        #expect(first == second)
        #expect(size?.uint64Value == 3 * 1024 * 1024 * 1024)

        let reader = try FileHandle(forReadingFrom: second)
        let prefix = try reader.read(upToCount: 1)
        try reader.close()
        #expect(prefix == Data([0]))
    }

    @Test
    func configurationDefaultsUseMaximumCPUAndRequestedDefaults() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }

        let configuration = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6
        )

        #expect(configuration.vCPUCount == 6)
        #expect(configuration.guestMemoryMiB == 2048)
        #expect(configuration.overlaySizeGiB == 8)
        #expect(configuration.rootfsFilename == "rootfs.erofs")
    }

    @Test
    func configurationPersistsAndNormalizesStoredValues() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }

        EmulatorConfiguration(
            vCPUCount: 4,
            guestMemoryMiB: 2048,
            overlaySizeGiB: 16,
            rootfsFilename: "phosh.erofs"
        ).save(defaults: defaults)
        #expect(
            EmulatorConfiguration.load(
                defaults: defaults,
                physicalProcessorCount: 6
            ) == EmulatorConfiguration(
                vCPUCount: 4,
                guestMemoryMiB: 2048,
                overlaySizeGiB: 16,
                rootfsFilename: "phosh.erofs"
            )
        )

        defaults.set(99, forKey: "emulator.vCPUCount")
        defaults.set(1000, forKey: "emulator.guestMemoryMiB")
        defaults.set(0, forKey: "emulator.overlaySizeGiB")
        defaults.set("../phosh.erofs", forKey: "emulator.rootfsFilename")
        let normalized = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6
        )
        #expect(normalized.vCPUCount == 6)
        #expect(normalized.guestMemoryMiB == 1024)
        #expect(normalized.overlaySizeGiB == 1)
        #expect(normalized.rootfsFilename == "rootfs.erofs")
    }

    @Test
    func rootfsSelectionAcceptsOnlyDocumentsFilenames() {
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "phosh.erofs",
                environment: [:]
            ) == "phosh.erofs"
        )
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "rootfs.erofs",
                environment: [RootfsImage.environmentKey: "phosh.erofs"]
            ) == "phosh.erofs"
        )
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "rootfs.erofs",
                environment: [RootfsImage.environmentKey: "../phosh.erofs"]
            ) == nil
        )
        #expect(
            RootfsImage.selectedFilename(
                configuredFilename: "rootfs.erofs",
                environment: [RootfsImage.environmentKey: "phosh.img"]
            ) == nil
        )
    }

    @Test
    func rootfsEnumerationIncludesOnlyRegularErofsImages() throws {
        let manager = FileManager.default
        let directory = manager.temporaryDirectory.appendingPathComponent(
            UUID().uuidString,
            isDirectory: true
        )
        try manager.createDirectory(
            at: directory,
            withIntermediateDirectories: true
        )
        defer { try? manager.removeItem(at: directory) }

        #expect(
            manager.createFile(
                atPath: directory.appendingPathComponent("phosh.erofs").path,
                contents: Data()
            )
        )
        #expect(
            manager.createFile(
                atPath: directory.appendingPathComponent("rootfs.erofs").path,
                contents: Data()
            )
        )
        #expect(
            manager.createFile(
                atPath: directory.appendingPathComponent("ignored.img").path,
                contents: Data()
            )
        )
        try manager.createDirectory(
            at: directory.appendingPathComponent("directory.erofs"),
            withIntermediateDirectories: false
        )

        #expect(
            RootfsImage.availableFilenames(in: directory) == [
                "phosh.erofs",
                "rootfs.erofs",
            ]
        )
    }

    private func isolatedDefaults() -> (UserDefaults, String) {
        let suiteName = "KernariumTests.\(UUID().uuidString)"
        return (UserDefaults(suiteName: suiteName)!, suiteName)
    }
}
