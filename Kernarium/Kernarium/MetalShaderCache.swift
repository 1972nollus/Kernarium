// SPDX-License-Identifier: GPL-2.0-or-later

import Darwin
import Foundation

/// Nothing Metal compiles for one guest boot is kept for the next.
///
/// Metal keeps one shader cache per application and reads that cache's whole
/// index into the process the first time anything in it compiles a pipeline.
/// Kernarium's display and terminal compile theirs as the application starts,
/// so every entry an earlier boot left behind is paid for in memory from the
/// first frame on, whether this boot uses it or not; the guest's GPU
/// conformance campaigns alone leave about 2 GB. Metal has no call that
/// clears, bounds or bypasses the cache and does not say which entries came
/// from a guest shader, so the application removes the whole cache before
/// anything in the process can open it, and each boot compiles what it uses
/// again.
nonisolated enum MetalShaderCache {
    /// Metal keeps the cache in `com.apple.metal` below a directory named
    /// after the bundle identifier in the Darwin user cache directory. That
    /// layout is Metal's own and undocumented: if it changes, nothing is
    /// removed and the cache simply stays.
    static func directory(
        userCacheDirectory: URL,
        bundleIdentifier: String
    ) -> URL {
        userCacheDirectory
            .appendingPathComponent(bundleIdentifier, isDirectory: true)
            .appendingPathComponent("com.apple.metal", isDirectory: true)
    }

    /// The Darwin user cache directory. An iOS application on a Mac has one
    /// outside its container, which only `confstr(_CS_DARWIN_USER_CACHE_DIR)`
    /// names. On a device that query fails with `EIO`, and the directory is
    /// the container's `Library/Caches`.
    static var userCacheDirectory: URL? {
        darwinUserCacheDirectory ?? FileManager.default.urls(
            for: .cachesDirectory,
            in: .userDomainMask
        ).first
    }

    private static var darwinUserCacheDirectory: URL? {
        let length = confstr(_CS_DARWIN_USER_CACHE_DIR, nil, 0)
        guard length > 1 else { return nil }
        var path = [CChar](repeating: 0, count: length)
        guard confstr(_CS_DARWIN_USER_CACHE_DIR, &path, length) == length else {
            return nil
        }
        return URL(fileURLWithPath: String(cString: path), isDirectory: true)
    }

    /// Removes the cache at `directory` and returns the bytes its files held,
    /// or nil when there is no cache.
    static func remove(
        at directory: URL,
        fileManager: FileManager = .default
    ) throws -> UInt64? {
        guard fileManager.fileExists(atPath: directory.path) else { return nil }
        var bytes: UInt64 = 0
        let keys: [URLResourceKey] = [.isRegularFileKey, .fileSizeKey]
        if let files = fileManager.enumerator(
            at: directory,
            includingPropertiesForKeys: keys
        ) {
            for case let file as URL in files {
                let values = try? file.resourceValues(forKeys: Set(keys))
                if values?.isRegularFile == true {
                    bytes += UInt64(values?.fileSize ?? 0)
                }
            }
        }
        try fileManager.removeItem(at: directory)
        return bytes
    }

    /// Removes this application's cache. The application calls it once, from
    /// its initializer, before any view or runtime exists: once Metal has read
    /// the index, removing the files no longer returns its memory.
    static func removeBeforeFirstUse() {
        guard let root = userCacheDirectory,
              let bundleIdentifier = Bundle.main.bundleIdentifier
        else { return }
        let cache = directory(
            userCacheDirectory: root,
            bundleIdentifier: bundleIdentifier
        )
        let clock = ContinuousClock()
        let start = clock.now
        do {
            guard let bytes = try remove(at: cache) else {
                // Metal recreates the cache on every launch, so its absence
                // after the first one means the layout above has moved.
                print("Kernarium: no Metal shader cache at \(cache.path)")
                return
            }
            let elapsed = start.duration(to: clock.now)
            let milliseconds = Double(elapsed.components.seconds) * 1000 +
                Double(elapsed.components.attoseconds) / 1e15
            print(String(
                format: "Kernarium: removed the Metal shader cache, %.1f MB in %.1f ms",
                Double(bytes) / 1e6,
                milliseconds
            ))
        } catch {
            print("Kernarium: cannot remove the Metal shader cache: \(error)")
        }
    }
}
