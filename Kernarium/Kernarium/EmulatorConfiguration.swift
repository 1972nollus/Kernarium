import Foundation

enum AndroidBootMode: UInt32, CaseIterable, Identifiable {
    static let environmentKey = "KERNARIUM_ANDROID_BOOT_MODE"

    case automatic = 0
    case normal = 1
    case recovery = 2

    var id: UInt32 { rawValue }

    var title: String {
        switch self {
        case .automatic:
            "Automatic"
        case .normal:
            "Normal"
        case .recovery:
            "Recovery"
        }
    }

    init?(environmentValue: String) {
        switch environmentValue {
        case "auto": self = .automatic
        case "normal": self = .normal
        case "recovery": self = .recovery
        default: return nil
        }
    }
}

struct EmulatorConfiguration: Equatable {
    static let vCPUEnvironmentKey = "KERNARIUM_VCPU_COUNT"
    static let guestMemoryEnvironmentKey = "KERNARIUM_GUEST_MEMORY_MIB"
    static let overlaySizeEnvironmentKey = "KERNARIUM_OVERLAY_SIZE_GIB"
    static let androidDiskSizeEnvironmentKey = "KERNARIUM_ANDROID_DISK_SIZE_GIB"
    static let retinaEnvironmentKey = "KERNARIUM_RETINA_MODE"
    static let autoResizeGuestEnvironmentKey = "KERNARIUM_GPU_AUTO_RESIZE"
    static let guestMemoryRangeMiB = 512...4096
    static let guestMemoryStepMiB = 64
    static let overlaySizeRangeGiB = 1...32
    static let androidDiskSizeRangeGiB = 4...64
    // A real iPhone or iPad enforces a jetsam limit that a 4 GiB guest would
    // cross, so the device default stays at half the range. The same slice
    // running on an Apple silicon Mac in Designed for iPad mode is given the
    // host's memory instead and keeps the full guest.
    static var defaultGuestMemoryMiB: Int {
        ProcessInfo.processInfo.isiOSAppOnMac ? 4096 : 2048
    }
    static let defaultOverlaySizeGiB = 8
    static let defaultAndroidDiskSizeGiB = 8
    static let defaultRetinaMode = false
    static let defaultAutoResizeGuest = true
    static let defaultAndroidBootMode = AndroidBootMode.automatic

    let vCPUCount: Int
    let guestMemoryMiB: Int
    let overlaySizeGiB: Int
    let androidDiskSizeGiB: Int
    let rootfsFilename: String
    let retinaMode: Bool
    let autoResizeGuest: Bool
    let androidBootMode: AndroidBootMode

    static var physicalProcessorCount: Int {
        max(ProcessInfo.processInfo.processorCount, 1)
    }

    static func load(defaults: UserDefaults = .standard) -> Self {
        load(
            defaults: defaults,
            physicalProcessorCount: physicalProcessorCount,
            environment: ProcessInfo.processInfo.environment
        )
    }

    static func load(
        defaults: UserDefaults,
        physicalProcessorCount: Int,
        environment: [String: String] = ProcessInfo.processInfo.environment,
        documentsDirectory: URL = RootfsImage.documentsDirectory
    ) -> Self {
        let maximumVCPUCount = max(physicalProcessorCount, 1)
        func stored(_ key: String, _ fallback: Int) -> Int {
            defaults.object(forKey: key) == nil
                ? fallback
                : defaults.integer(forKey: key)
        }
        func stored(_ key: String, _ fallback: Bool) -> Bool {
            defaults.object(forKey: key) == nil
                ? fallback
                : defaults.bool(forKey: key)
        }
        let storedVCPUCount = stored(Key.vCPUCount, maximumVCPUCount)
        let storedGuestMemoryMiB = stored(
            Key.guestMemoryMiB, defaultGuestMemoryMiB
        )
        let storedOverlaySizeGiB = stored(
            Key.overlaySizeGiB, defaultOverlaySizeGiB
        )
        let storedAndroidDiskSizeGiB = stored(
            Key.androidDiskSizeGiB, defaultAndroidDiskSizeGiB
        )
        let storedRootfsFilename = defaults.string(
            forKey: Key.rootfsFilename
        ) ?? RootfsImage.defaultFilename
        let storedRetinaMode = stored(Key.retinaMode, defaultRetinaMode)
        let storedAutoResizeGuest = stored(
            Key.autoResizeGuest, defaultAutoResizeGuest
        )
        let storedAndroidBootMode = AndroidBootMode(
            rawValue: UInt32(clamping: stored(
                Key.androidBootMode, Int(defaultAndroidBootMode.rawValue)
            ))
        ) ?? defaultAndroidBootMode
        let environmentVCPUCount = environmentInteger(
            environment[vCPUEnvironmentKey], in: 1...maximumVCPUCount
        )
        let environmentGuestMemoryMiB = environmentInteger(
            environment[guestMemoryEnvironmentKey], in: guestMemoryRangeMiB
        ).flatMap { value in
            (value - guestMemoryRangeMiB.lowerBound) % guestMemoryStepMiB == 0
                ? value
                : nil
        }
        let environmentOverlaySizeGiB = environmentInteger(
            environment[overlaySizeEnvironmentKey], in: overlaySizeRangeGiB
        )
        let environmentAndroidDiskSizeGiB = environmentInteger(
            environment[androidDiskSizeEnvironmentKey],
            in: androidDiskSizeRangeGiB
        )
        func environmentFlag(_ key: String) -> Bool? {
            switch environment[key] {
            case "0"?: false
            case "1"?: true
            default: nil
            }
        }
        let environmentRetinaMode = environmentFlag(retinaEnvironmentKey)
        let environmentAutoResizeGuest = environmentFlag(
            autoResizeGuestEnvironmentKey
        )
        let environmentAndroidBootMode = environment[
            AndroidBootMode.environmentKey
        ].flatMap(AndroidBootMode.init(environmentValue:))

        let rootfsFilename = RootfsImage.validatedFilename(
            storedRootfsFilename
        ) ?? RootfsImage.defaultFilename

        return Self(
            vCPUCount: environmentVCPUCount ?? min(
                max(storedVCPUCount, 1), maximumVCPUCount
            ),
            guestMemoryMiB: environmentGuestMemoryMiB ??
                normalizedGuestMemoryMiB(storedGuestMemoryMiB),
            overlaySizeGiB: environmentOverlaySizeGiB ?? min(
                max(storedOverlaySizeGiB, overlaySizeRangeGiB.lowerBound),
                overlaySizeRangeGiB.upperBound
            ),
            androidDiskSizeGiB: environmentAndroidDiskSizeGiB ?? min(
                max(
                    storedAndroidDiskSizeGiB,
                    androidDiskSizeRangeGiB.lowerBound
                ),
                androidDiskSizeRangeGiB.upperBound
            ),
            rootfsFilename: rootfsFilename,
            retinaMode: environmentRetinaMode ?? storedRetinaMode,
            autoResizeGuest: environmentAutoResizeGuest ??
                storedAutoResizeGuest,
            androidBootMode: environmentAndroidBootMode ?? storedAndroidBootMode
        )
    }

    func save(defaults: UserDefaults = .standard) {
        defaults.set(vCPUCount, forKey: Key.vCPUCount)
        defaults.set(guestMemoryMiB, forKey: Key.guestMemoryMiB)
        defaults.set(overlaySizeGiB, forKey: Key.overlaySizeGiB)
        defaults.set(androidDiskSizeGiB, forKey: Key.androidDiskSizeGiB)
        defaults.set(
            RootfsImage.validatedFilename(rootfsFilename) ??
                RootfsImage.defaultFilename,
            forKey: Key.rootfsFilename
        )
        defaults.set(retinaMode, forKey: Key.retinaMode)
        defaults.set(autoResizeGuest, forKey: Key.autoResizeGuest)
        defaults.set(androidBootMode.rawValue, forKey: Key.androidBootMode)
    }

    private static func normalizedGuestMemoryMiB(_ value: Int) -> Int {
        let clamped = min(
            max(value, guestMemoryRangeMiB.lowerBound),
            guestMemoryRangeMiB.upperBound
        )
        let offset = clamped - guestMemoryRangeMiB.lowerBound
        let roundedSteps = (offset + guestMemoryStepMiB / 2) /
            guestMemoryStepMiB
        return guestMemoryRangeMiB.lowerBound +
            roundedSteps * guestMemoryStepMiB
    }

    private static func environmentInteger(
        _ value: String?,
        in range: ClosedRange<Int>
    ) -> Int? {
        guard let value, let parsed = Int(value), range.contains(parsed) else {
            return nil
        }
        return parsed
    }

    private enum Key {
        static let vCPUCount = "emulator.vCPUCount"
        static let guestMemoryMiB = "emulator.guestMemoryMiB"
        static let overlaySizeGiB = "emulator.overlaySizeGiB"
        static let androidDiskSizeGiB = "emulator.androidDiskSizeGiB"
        static let rootfsFilename = "emulator.rootfsFilename"
        static let retinaMode = "emulator.retinaMode"
        static let autoResizeGuest = "emulator.autoResizeGuest"
        static let androidBootMode = "emulator.androidBootMode"
    }
}

/// What the launcher was asked to boot, once the stored name has been resolved
/// against Documents.
enum GuestSelection {
    /// A single root filesystem image and the runtime profile it requires.
    case image(URL, flavor: GuestFlavor)
    /// A self-contained bundle carrying its own payload and writable disk.
    case bundle(GuestBundle)

    var flavor: GuestFlavor {
        switch self {
        case let .image(_, flavor):
            flavor
        case let .bundle(bundle):
            bundle.flavor
        }
    }
}

enum RootfsImage {
    /// The accepted KDE desktop image (tools/rootfs/desktop/images.json),
    /// which carries its own session setup. It applies only while the
    /// settings store no name: a stored name, `rootfs.erofs` included, is kept.
    static let defaultFilename = "kde-20260929-r2.erofs"
    static let environmentKey = "KERNARIUM_ROOTFS"

    static var documentsDirectory: URL {
        FileManager.default.urls(
            for: .documentDirectory,
            in: .userDomainMask
        )[0]
    }

    /// The runtime names the image in a UBD kernel argument, which cannot
    /// carry kernel whitespace (including byte 0xa0, which the kernel's
    /// Latin-1 table counts), a double quote, a comma or a colon. Such a name
    /// is not offered, rather than failing to boot (darwin/device/block/
    /// ubd_path.hpp holds the same rule).
    static func namesOnKernelCommandLine(_ filename: String) -> Bool {
        !filename.utf8.contains { byte in
            (0x09...0x0d).contains(byte) || byte == 0x20 || byte == 0xa0 ||
                byte == 0x22 || byte == 0x2c || byte == 0x3a
        }
    }

    static func validatedFilename(_ filename: String) -> String? {
        let pathExtension = (filename as NSString).pathExtension.lowercased()
        guard !filename.isEmpty,
              (filename as NSString).lastPathComponent == filename,
              namesOnKernelCommandLine(filename),
              pathExtension == "erofs" || pathExtension == "ext4" ||
                  pathExtension == GuestBundle.pathExtension
        else {
            return nil
        }
        return filename
    }

    /// Resolves a stored name to what it actually is on disk. A bundle name is
    /// only accepted when its payload is complete, so the picker and the
    /// launcher agree on what can be booted.
    static func selection(
        named filename: String,
        in directory: URL = documentsDirectory,
        fileManager: FileManager = .default
    ) -> GuestSelection? {
        guard let validated = validatedFilename(filename) else { return nil }
        let url = directory.appendingPathComponent(validated)
        if (validated as NSString).pathExtension.lowercased() ==
            GuestBundle.pathExtension {
            return GuestBundle.load(at: url, fileManager: fileManager)
                .map(GuestSelection.bundle)
        }
        guard fileManager.fileExists(atPath: url.path) else { return nil }
        return .image(url, flavor: .linux)
    }

    static func selectedFilename(
        configuredFilename: String,
        environment: [String: String] = ProcessInfo.processInfo.environment
    ) -> String? {
        validatedFilename(environment[environmentKey] ?? configuredFilename)
    }

    static func availableFilenames(
        in directory: URL = documentsDirectory,
        fileManager: FileManager = .default
    ) -> [String] {
        let resourceKeys: [URLResourceKey] = [.isRegularFileKey]
        guard let urls = try? fileManager.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: resourceKeys,
            options: [.skipsHiddenFiles]
        ) else {
            return []
        }
        let images: [String] = urls.compactMap { url -> String? in
            guard let filename = validatedFilename(url.lastPathComponent),
                  let values = try? url.resourceValues(
                      forKeys: Set(resourceKeys)
                  ),
                  values.isRegularFile == true else {
                return nil
            }
            return filename
        }
        let bundles = GuestBundle.availableNames(
            in: directory, fileManager: fileManager
        )
        return (images + bundles).sorted {
            $0.localizedStandardCompare($1) == .orderedAscending
        }
    }
}
