import Foundation

enum KernelPageSize: Int, CaseIterable, Identifiable {
    static let environmentKey = "KERNARIUM_KERNEL_PAGE_SIZE"

    case fourKiB = 4096
    case sixteenKiB = 16384

    var id: Int { rawValue }

    var title: String {
        switch self {
        case .fourKiB:
            "4 KB"
        case .sixteenKiB:
            "16 KB"
        }
    }
}

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
#if targetEnvironment(simulator)
    static let defaultGuestMemoryMiB = 4096
#else
    static let defaultGuestMemoryMiB = 2048
#endif
    static let defaultOverlaySizeGiB = 8
    static let defaultAndroidDiskSizeGiB = 8
    static let defaultKernelPageSize = KernelPageSize.sixteenKiB
    static let defaultRetinaMode = false
    static let defaultAutoResizeGuest = true
    static let defaultAndroidBootMode = AndroidBootMode.automatic

    let vCPUCount: Int
    let guestMemoryMiB: Int
    let overlaySizeGiB: Int
    let androidDiskSizeGiB: Int
    let rootfsFilename: String
    let kernelPageSize: KernelPageSize
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
        let storedVCPUCount = defaults.object(forKey: Key.vCPUCount) == nil
            ? maximumVCPUCount
            : defaults.integer(forKey: Key.vCPUCount)
        let storedGuestMemoryMiB = defaults.object(
            forKey: Key.guestMemoryMiB
        ) == nil
            ? defaultGuestMemoryMiB
            : defaults.integer(forKey: Key.guestMemoryMiB)
        let storedOverlaySizeGiB = defaults.object(
            forKey: Key.overlaySizeGiB
        ) == nil
            ? defaultOverlaySizeGiB
            : defaults.integer(forKey: Key.overlaySizeGiB)
        let storedAndroidDiskSizeGiB = defaults.object(
            forKey: Key.androidDiskSizeGiB
        ) == nil
            ? defaultAndroidDiskSizeGiB
            : defaults.integer(forKey: Key.androidDiskSizeGiB)
        let storedRootfsFilename = defaults.string(
            forKey: Key.rootfsFilename
        ) ?? RootfsImage.defaultFilename
        let storedKernelPageSize = defaults.object(
            forKey: Key.kernelPageSize
        ) == nil
            ? defaultKernelPageSize.rawValue
            : defaults.integer(forKey: Key.kernelPageSize)
        let environmentKernelPageSize = environment[
            KernelPageSize.environmentKey
        ].flatMap { value in
            Int(value).flatMap(KernelPageSize.init(rawValue:))
        }
        let storedRetinaMode = defaults.object(forKey: Key.retinaMode) == nil
            ? defaultRetinaMode
            : defaults.bool(forKey: Key.retinaMode)
        let storedAutoResizeGuest = defaults.object(
            forKey: Key.autoResizeGuest
        ) == nil
            ? defaultAutoResizeGuest
            : defaults.bool(forKey: Key.autoResizeGuest)
        let storedAndroidBootMode = defaults.object(
            forKey: Key.androidBootMode
        ) == nil
            ? defaultAndroidBootMode
            : AndroidBootMode(
                rawValue: UInt32(clamping: defaults.integer(
                    forKey: Key.androidBootMode
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
        let environmentRetinaMode = environment[retinaEnvironmentKey].flatMap {
            value in
            switch value {
            case "0": false
            case "1": true
            default: nil
            }
        }
        let environmentAutoResizeGuest = environment[
            autoResizeGuestEnvironmentKey
        ].flatMap { value in
            switch value {
            case "0": false
            case "1": true
            default: nil
            }
        }
        let environmentAndroidBootMode = environment[
            AndroidBootMode.environmentKey
        ].flatMap(AndroidBootMode.init(environmentValue:))

        let rootfsFilename = RootfsImage.validatedFilename(
            storedRootfsFilename
        ) ?? RootfsImage.defaultFilename
        let requestedKernelPageSize = environmentKernelPageSize ??
            KernelPageSize(rawValue: storedKernelPageSize) ??
            defaultKernelPageSize
        // A bundle whose kernel exists in only one page profile decides the
        // profile itself. Honouring a stale stored or environment setting here
        // would only select a runtime that has no kernel for its payload. The
        // environment override names what will actually boot, so it is what
        // the profile has to follow.
        let requiredKernelPageSize = RootfsImage.selectedFilename(
            configuredFilename: rootfsFilename,
            environment: environment
        ).flatMap { effective in
            RootfsImage.requiredPageSize(
                forSelectionNamed: effective,
                in: documentsDirectory
            )
        }

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
            kernelPageSize: requiredKernelPageSize ?? requestedKernelPageSize,
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
        defaults.set(kernelPageSize.rawValue, forKey: Key.kernelPageSize)
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
        static let kernelPageSize = "emulator.kernelPageSize"
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

    /// Specialized guests pin the page profile carried by their runtime;
    /// ordinary Linux images leave the choice to the setting.
    var requiredPageSize: KernelPageSize? {
        switch self {
        case let .image(_, flavor):
            switch flavor {
            case .wineARM64EC:
                .fourKiB
            case .linux, .android:
                nil
            }
        case let .bundle(bundle):
            bundle.requiredPageSize
        }
    }
}

enum RootfsImage {
    static let defaultFilename = "rootfs.erofs"
    static let environmentKey = "KERNARIUM_ROOTFS"
    static let wineARM64ECFilenamePrefix = "wine-arm64ec-"

    static var documentsDirectory: URL {
        FileManager.default.urls(
            for: .documentDirectory,
            in: .userDomainMask
        )[0]
    }

    static func validatedFilename(_ filename: String) -> String? {
        let pathExtension = (filename as NSString).pathExtension.lowercased()
        guard !filename.isEmpty,
              (filename as NSString).lastPathComponent == filename,
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
        let flavor: GuestFlavor = validated.hasPrefix(
            wineARM64ECFilenamePrefix
        ) ? .wineARM64EC : .linux
        return .image(url, flavor: flavor)
    }

    /// The page profile a stored selection forces, if any. Settings and the
    /// launcher both consult this so a stale stored profile cannot reach a
    /// runtime that has no kernel for it.
    static func requiredPageSize(
        forSelectionNamed filename: String,
        in directory: URL = documentsDirectory,
        fileManager: FileManager = .default
    ) -> KernelPageSize? {
        selection(
            named: filename, in: directory, fileManager: fileManager
        )?.requiredPageSize
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
