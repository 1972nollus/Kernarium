import Foundation

/// What a bundle's payload boots. One runtime framework serves one flavour,
/// because its kernel is fixed in a signed carrier, so the flavour also selects
/// which framework the loader may open.
enum GuestFlavor: UInt32 {
    case linux = 0
    case android = 1
}

/// A guest bundle is a directory in Documents holding everything one guest
/// needs, including the writable disk it accumulates. The extension is neutral
/// about the operating system inside; the payload names the flavour.
struct GuestBundle: Equatable {
    static let pathExtension = "kern"

    /// Android payload layout, as published by the accepted release.
    enum Android {
        static let manifest = "release-manifest.json"
        static let ramdisk = "boot/ramdisk.img"
        static let commandLine = "boot/kernel-command-line.txt"
        static let guestDirectory = "guest"
        static let diskExtension = "img"
        /// UBD creates this copy-on-write layer over the immutable base image
        /// on first boot and every later boot reopens it, so guest state
        /// accumulates inside the bundle it belongs to.
        static let instance = "userdata.cow"
    }

    let url: URL
    let flavor: GuestFlavor
    let baseDisk: URL

    var name: String { url.lastPathComponent }
    var manifest: URL { url.appendingPathComponent(Android.manifest) }
    var ramdisk: URL { url.appendingPathComponent(Android.ramdisk) }
    var commandLine: URL { url.appendingPathComponent(Android.commandLine) }
    var instance: URL { url.appendingPathComponent(Android.instance) }

    /// Android guests boot the ACK kernel that only the 16 KiB runtime carries.
    var requiredPageSize: KernelPageSize? {
        switch flavor {
        case .android:
            .sixteenKiB
        case .linux:
            nil
        }
    }

    /// Recognises a bundle from its payload rather than its name. A directory
    /// that does not carry a complete payload is not offered at all, so a
    /// half-copied bundle cannot be selected and then fail at boot.
    static func load(
        at url: URL,
        fileManager: FileManager = .default
    ) -> GuestBundle? {
        guard url.pathExtension.lowercased() == pathExtension,
              isDirectory(url, fileManager: fileManager)
        else {
            return nil
        }
        for relative in [Android.manifest, Android.ramdisk, Android.commandLine] {
            guard isRegularFile(
                url.appendingPathComponent(relative),
                fileManager: fileManager
            ) else {
                return nil
            }
        }
        guard let baseDisk = androidBaseDisk(in: url, fileManager: fileManager)
        else {
            return nil
        }
        return GuestBundle(url: url, flavor: .android, baseDisk: baseDisk)
    }

    /// The base image name carries the release it was published for, so it is
    /// discovered rather than assumed. Exactly one candidate keeps the choice
    /// unambiguous; the runtime still checks its size against the release the
    /// framework was built from.
    private static func androidBaseDisk(
        in url: URL,
        fileManager: FileManager
    ) -> URL? {
        let directory = url.appendingPathComponent(Android.guestDirectory)
        guard let entries = try? fileManager.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: [.isRegularFileKey],
            options: [.skipsHiddenFiles]
        ) else {
            return nil
        }
        let disks = entries.filter { entry in
            entry.pathExtension.lowercased() == Android.diskExtension &&
                isRegularFile(entry, fileManager: fileManager)
        }
        return disks.count == 1 ? disks[0] : nil
    }

    private static func isDirectory(
        _ url: URL,
        fileManager: FileManager
    ) -> Bool {
        var directory: ObjCBool = false
        let exists = fileManager.fileExists(
            atPath: url.path,
            isDirectory: &directory
        )
        return exists && directory.boolValue
    }

    private static func isRegularFile(
        _ url: URL,
        fileManager: FileManager
    ) -> Bool {
        var directory: ObjCBool = false
        let exists = fileManager.fileExists(
            atPath: url.path,
            isDirectory: &directory
        )
        return exists && !directory.boolValue
    }

    static func availableNames(
        in directory: URL,
        fileManager: FileManager = .default
    ) -> [String] {
        guard let entries = try? fileManager.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }
        return entries.compactMap { entry in
            load(at: entry, fileManager: fileManager)?.name
        }.sorted {
            $0.localizedStandardCompare($1) == .orderedAscending
        }
    }
}
