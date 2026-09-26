// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import Kernarium

struct MetalShaderCacheTests {
    private static let bundleIdentifier = "com.example.Guest"

    private static func makeRoot() throws -> URL {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(
            "MetalShaderCacheTests-\(UUID().uuidString)",
            isDirectory: true
        )
        try FileManager.default.createDirectory(
            at: root,
            withIntermediateDirectories: true
        )
        return root
    }

    private static func write(_ count: Int, to file: URL) throws {
        try FileManager.default.createDirectory(
            at: file.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        try Data(count: count).write(to: file)
    }

    @Test
    func cacheLivesBelowTheBundleIdentifier() {
        let root = URL(fileURLWithPath: "/var/mobile/Caches", isDirectory: true)
        let cache = MetalShaderCache.directory(
            userCacheDirectory: root,
            bundleIdentifier: Self.bundleIdentifier
        )
        #expect(cache.path == "/var/mobile/Caches/com.example.Guest/com.apple.metal")
    }

    @Test
    func userCacheDirectoryExists() throws {
        let root = try #require(MetalShaderCache.userCacheDirectory)
        var isDirectory: ObjCBool = false
        #expect(FileManager.default.fileExists(
            atPath: root.path,
            isDirectory: &isDirectory
        ))
        #expect(isDirectory.boolValue)
    }

    @Test
    func removalTakesTheWholeCacheAndLeavesItsSiblings() throws {
        let root = try Self.makeRoot()
        defer { try? FileManager.default.removeItem(at: root) }
        let cache = MetalShaderCache.directory(
            userCacheDirectory: root,
            bundleIdentifier: Self.bundleIdentifier
        )
        try Self.write(3000, to: cache.appendingPathComponent("functions.data"))
        try Self.write(200, to: cache.appendingPathComponent("functions.list"))
        try Self.write(1000, to: cache.appendingPathComponent("32024/libraries.data"))
        let sibling = root.appendingPathComponent(
            "\(Self.bundleIdentifier)/com.apple.metalfe/modules.timestamp"
        )
        try Self.write(10, to: sibling)

        #expect(try MetalShaderCache.remove(at: cache) == 4200)
        #expect(!FileManager.default.fileExists(atPath: cache.path))
        #expect(FileManager.default.fileExists(atPath: sibling.path))
    }

    @Test
    func anAbsentCacheIsNotAnError() throws {
        let root = try Self.makeRoot()
        defer { try? FileManager.default.removeItem(at: root) }
        let cache = MetalShaderCache.directory(
            userCacheDirectory: root,
            bundleIdentifier: Self.bundleIdentifier
        )
        #expect(try MetalShaderCache.remove(at: cache) == nil)
    }
}
