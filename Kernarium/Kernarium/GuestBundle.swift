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

    /// Android rootfs payload layout. The signed runtime carries its kernel
    /// independently, so a kernel-only app update does not rewrite the bundle.
    enum Android {
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

    enum StorageError: Error, LocalizedError {
        case capacityLocked

        var errorDescription: String? {
            switch self {
            case .capacityLocked:
                "Android disk capacity cannot change after the instance " +
                    "was created"
            }
        }
    }

    var name: String { url.lastPathComponent }
    var commandLine: URL { url.appendingPathComponent(Android.commandLine) }
    var instance: URL { url.appendingPathComponent(Android.instance) }

    /// Removes only this bundle's writable instance. This is intentionally a
    /// narrow primitive: callers cannot replace the bundle directory or touch
    /// a sibling while preparing a fresh-boot diagnostic.
    @discardableResult
    func removeInstanceIfPresent(
        fileManager: FileManager = .default
    ) throws -> Bool {
        guard fileManager.fileExists(atPath: instance.path) else {
            return false
        }
        try fileManager.removeItem(at: instance)
        return true
    }

    /// The host grows the raw bundle image and its userdata GPT entry. Android
    /// grows ext4 during boot only after observing the larger partition. Once
    /// UBD has created a COW, its recorded base extent locks the capacity until
    /// the instance is explicitly removed.
    @discardableResult
    func prepareDiskCapacity(
        _ capacityGiB: Int,
        diskUtil: any DiskCapacityManaging = DiskUtil(),
        fileManager: FileManager = .default
    ) throws -> Bool {
        let requested = try DiskUtil.byteCount(forGiB: capacityGiB)
        let current = try diskUtil.imageSize(at: baseDisk)
        if fileManager.fileExists(atPath: instance.path) {
            guard current == requested,
                  try diskUtil.androidImageIsPrepared(
                    at: baseDisk,
                    forGiB: capacityGiB
                  )
            else {
                throw StorageError.capacityLocked
            }
            return false
        }
        return try diskUtil.resizeAndroidImage(
            at: baseDisk,
            toGiB: capacityGiB
        )
    }

    /// Android guests boot the ACK kernel that only the 16 KiB runtime carries.
    var requiredPageSize: KernelPageSize? {
        switch flavor {
        case .android:
            .sixteenKiB
        case .linux:
            nil
        }
    }

    /// Recognises a bundle from the inputs needed to launch it. The release
    /// manifest is provenance for people and tools, not a runtime identity.
    static func load(
        at url: URL,
        fileManager: FileManager = .default
    ) -> GuestBundle? {
        guard url.pathExtension.lowercased() == pathExtension,
              isDirectory(url, fileManager: fileManager)
        else {
            return nil
        }
        guard let baseDisk = androidBaseDisk(in: url, fileManager: fileManager)
        else {
            return nil
        }
        return GuestBundle(url: url, flavor: .android, baseDisk: baseDisk)
    }

    /// The base image is discovered rather than named by a release. Exactly one
    /// candidate keeps the launch input unambiguous.
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
