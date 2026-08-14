import Foundation

struct EmulatorConfiguration: Equatable {
    static let guestMemoryRangeMiB = 512...4096
    static let guestMemoryStepMiB = 128
    static let overlaySizeRangeGiB = 1...32
    static let defaultGuestMemoryMiB = 1536
    static let defaultOverlaySizeGiB = 8

    let vCPUCount: Int
    let guestMemoryMiB: Int
    let overlaySizeGiB: Int
    let rootfsFilename: String

    static var physicalProcessorCount: Int {
        max(ProcessInfo.processInfo.processorCount, 1)
    }

    static func load(defaults: UserDefaults = .standard) -> Self {
        load(
            defaults: defaults,
            physicalProcessorCount: physicalProcessorCount
        )
    }

    static func load(
        defaults: UserDefaults,
        physicalProcessorCount: Int
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

        return Self(
            vCPUCount: min(max(storedVCPUCount, 1), maximumVCPUCount),
            guestMemoryMiB: normalizedGuestMemoryMiB(
                storedGuestMemoryMiB
            ),
            overlaySizeGiB: min(
                max(storedOverlaySizeGiB, overlaySizeRangeGiB.lowerBound),
                overlaySizeRangeGiB.upperBound
            ),
            rootfsFilename: RootfsImage.validatedFilename(
                storedRootfsFilename
            ) ?? RootfsImage.defaultFilename
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

    private enum Key {
        static let vCPUCount = "emulator.vCPUCount"
        static let guestMemoryMiB = "emulator.guestMemoryMiB"
        static let overlaySizeGiB = "emulator.overlaySizeGiB"
        static let rootfsFilename = "emulator.rootfsFilename"
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
        guard !filename.isEmpty,
              (filename as NSString).lastPathComponent == filename,
              (filename as NSString).pathExtension.lowercased() == "erofs"
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
