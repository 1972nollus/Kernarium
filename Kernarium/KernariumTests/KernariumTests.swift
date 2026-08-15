import Foundation
import Testing
import UIKit
@testable import Kernarium

struct KernariumTests {
    @Test
    @MainActor
    func floatingControlsAndSpecialKeysHaveExpectedGeometryAndCodes() {
        #expect(FloatingControlMetrics.buttonSide == 32)
        #expect(GuestViewMode.defaultMode == .framebuffer)
        #expect(UIImage(systemName: "terminal") != nil)
        #expect(UIImage(systemName: "display") != nil)
        #expect(GuestSpecialKey.power.code == 116)
        #expect(GuestSpecialKey.volumeUp.code == 115)
        #expect(GuestSpecialKey.volumeDown.code == 114)
        #expect(GuestSpecialKey.escape.code == 1)
        #expect(GuestSpecialKey.tab.code == 15)
        #expect(GuestSpecialKey.home.code == 102)
        for key in GuestSpecialKey.allCases {
            #expect(UIImage(systemName: key.systemName) != nil)
        }
    }

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
        #expect(configuration.guestMemoryMiB == 1536)
        #expect(configuration.overlaySizeGiB == 8)
        #expect(configuration.rootfsFilename == "rootfs.erofs")
        #expect(configuration.kernelPageSize == .sixteenKiB)
        #expect(configuration.retinaMode == false)
    }

    @Test
    func configurationPersistsAndNormalizesStoredValues() {
        let (defaults, suiteName) = isolatedDefaults()
        defer { defaults.removePersistentDomain(forName: suiteName) }

        EmulatorConfiguration(
            vCPUCount: 4,
            guestMemoryMiB: 2048,
            overlaySizeGiB: 16,
            rootfsFilename: "phosh.erofs",
            kernelPageSize: .fourKiB,
            retinaMode: true
        ).save(defaults: defaults)
        #expect(
            EmulatorConfiguration.load(
                defaults: defaults,
                physicalProcessorCount: 6
            ) == EmulatorConfiguration(
                vCPUCount: 4,
                guestMemoryMiB: 2048,
                overlaySizeGiB: 16,
                rootfsFilename: "phosh.erofs",
                kernelPageSize: .fourKiB,
                retinaMode: true
            )
        )

        defaults.set(99, forKey: "emulator.vCPUCount")
        defaults.set(1000, forKey: "emulator.guestMemoryMiB")
        defaults.set(0, forKey: "emulator.overlaySizeGiB")
        defaults.set("../phosh.erofs", forKey: "emulator.rootfsFilename")
        defaults.set(8192, forKey: "emulator.kernelPageSize")
        let normalized = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: 6
        )
        #expect(normalized.vCPUCount == 6)
        #expect(normalized.guestMemoryMiB == 1024)
        #expect(normalized.overlaySizeGiB == 1)
        #expect(normalized.rootfsFilename == "rootfs.erofs")
        #expect(normalized.kernelPageSize == .sixteenKiB)
        #expect(normalized.retinaMode == true)
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
                configuredFilename: "pmos-sxmo.erofs",
                environment: [:]
            ) == "pmos-sxmo.erofs"
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
                atPath: directory
                    .appendingPathComponent("pmos-sxmo.erofs").path,
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
                "pmos-sxmo.erofs",
                "rootfs.erofs",
            ]
        )
    }

    private func isolatedDefaults() -> (UserDefaults, String) {
        let suiteName = "KernariumTests.\(UUID().uuidString)"
        return (UserDefaults(suiteName: suiteName)!, suiteName)
    }
}
