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

struct EmulatorConfiguration: Equatable {
    static let vCPUEnvironmentKey = "KERNARIUM_VCPU_COUNT"
    static let guestMemoryEnvironmentKey = "KERNARIUM_GUEST_MEMORY_MIB"
    static let overlaySizeEnvironmentKey = "KERNARIUM_OVERLAY_SIZE_GIB"
    static let retinaEnvironmentKey = "KERNARIUM_RETINA_MODE"
    static let guestMemoryRangeMiB = 512...4096
    static let guestMemoryStepMiB = 64
    static let overlaySizeRangeGiB = 1...32
#if targetEnvironment(simulator)
    static let defaultGuestMemoryMiB = 4096
#else
    static let defaultGuestMemoryMiB = 2048
#endif
    static let defaultOverlaySizeGiB = 8
    static let defaultKernelPageSize = KernelPageSize.sixteenKiB
    static let defaultRetinaMode = false

    let vCPUCount: Int
    let guestMemoryMiB: Int
    let overlaySizeGiB: Int
    let rootfsFilename: String
    let kernelPageSize: KernelPageSize
    let retinaMode: Bool

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
        environment: [String: String] = ProcessInfo.processInfo.environment
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
        let environmentRetinaMode = environment[retinaEnvironmentKey].flatMap {
            value in
            switch value {
            case "0": false
            case "1": true
            default: nil
            }
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
            rootfsFilename: RootfsImage.validatedFilename(
                storedRootfsFilename
            ) ?? RootfsImage.defaultFilename,
            kernelPageSize: environmentKernelPageSize ??
                KernelPageSize(rawValue: storedKernelPageSize) ??
                defaultKernelPageSize,
            retinaMode: environmentRetinaMode ?? storedRetinaMode
        )
    }

    func save(defaults: UserDefaults = .standard) {
        defaults.set(vCPUCount, forKey: Key.vCPUCount)
        defaults.set(guestMemoryMiB, forKey: Key.guestMemoryMiB)
        defaults.set(overlaySizeGiB, forKey: Key.overlaySizeGiB)
        defaults.set(
            RootfsImage.validatedFilename(rootfsFilename) ??
                RootfsImage.defaultFilename,
            forKey: Key.rootfsFilename
        )
        defaults.set(kernelPageSize.rawValue, forKey: Key.kernelPageSize)
        defaults.set(retinaMode, forKey: Key.retinaMode)
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
        static let rootfsFilename = "emulator.rootfsFilename"
        static let kernelPageSize = "emulator.kernelPageSize"
        static let retinaMode = "emulator.retinaMode"
    }
}

enum RootfsImage {
    static let defaultFilename = "rootfs.erofs"
    static let environmentKey = "KERNARIUM_ROOTFS"

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
              pathExtension == "erofs" || pathExtension == "ext4"
        else {
            return nil
        }
        return filename
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
        return urls.compactMap { url in
            guard let filename = validatedFilename(url.lastPathComponent),
                  let values = try? url.resourceValues(
                      forKeys: Set(resourceKeys)
                  ),
                  values.isRegularFile == true else {
                return nil
            }
            return filename
        }.sorted {
            $0.localizedStandardCompare($1) == .orderedAscending
        }
    }
}
