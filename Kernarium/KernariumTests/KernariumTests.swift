import Foundation
import Testing
@testable import Kernarium

struct KernariumTests {
    @Test
    @MainActor
    func overlayDiskIsRecreatedAtEightGiB() throws {
        let manager = FileManager.default
        let directory = manager.temporaryDirectory.appendingPathComponent(
            UUID().uuidString,
            isDirectory: true
        )
        try manager.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? manager.removeItem(at: directory) }

        let first = try OverlayDisk.recreate(in: directory)
        let writer = try FileHandle(forWritingTo: first)
        try writer.write(contentsOf: Data([0xa5]))
        try writer.close()

        let second = try OverlayDisk.recreate(in: directory)
        let attributes = try manager.attributesOfItem(atPath: second.path)
        let size = attributes[.size] as? NSNumber
        #expect(first == second)
        #expect(size?.uint64Value == OverlayDisk.byteCount)

        let reader = try FileHandle(forReadingFrom: second)
        let prefix = try reader.read(upToCount: 1)
        try reader.close()
        #expect(prefix == Data([0]))
    }
}
