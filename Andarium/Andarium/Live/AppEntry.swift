import AndariumCore
import Foundation
import os
import TipKit
import UIKit

/// The device application's entry: the guest on disk, this device and the
/// system Andarium carries, and what a harness launch asks for in the
/// environment (`Andarium/README.md#automation`).
struct AppEntry {
    var overrides: LaunchOverrides

    /// The live application opens no screen directly.
    var hints: ScreenHints { ScreenHints() }

    static func fromArguments() -> AppEntry {
        let info = ProcessInfo.processInfo
        return AppEntry(
            overrides: LaunchOverrides(environment: info.environment, processorCount: info.activeProcessorCount)
        )
    }

    func configureTips() {
        try? Tips.configure([.displayFrequency(.immediate)])
    }

    /// The model for this launch. A copy of Andarium without its system, or
    /// with one it cannot read, still opens, and says so.
    @MainActor
    func makeModel() -> AppModel {
        let launch = overrides
        let bundle = Bundle.main
        let version = Self.appVersion(bundle)
        let systemFolder = bundle.resourceURL?.appendingPathComponent(SystemFolder.name)
        let host = Self.hostDevice()

        var problem: String?
        var system = SystemInfo(
            distribution: "LineageOS", android: "", release: "unknown", securityPatch: nil,
            kernel: "", architecture: "", appVersion: version
        )
        var seed: SeedImage?
        var commandLine: URL?
        if let systemFolder {
            do {
                let manifest = try Data(contentsOf: systemFolder.appendingPathComponent(SystemFolder.manifest))
                system = try SystemInfo(manifest: manifest, appVersion: version)
                commandLine = systemFolder.appendingPathComponent(SystemFolder.commandLine)
                seed = try SystemFolder.seed(in: systemFolder, release: system.release, stagedName: launch.systemImageName)
            } catch {
                problem = "This copy of Andarium does not carry a usable system: \(error.localizedDescription)"
            }
        } else {
            problem = "This copy of Andarium does not carry a system."
        }

        let locations = GuestLocations(directory: Self.guestDirectory())
        let storage = SingleImageStorage(
            locations: locations,
            seed: seed ?? SeedImage(url: locations.directory, release: system.release, expandedBytes: 0),
            appVersion: version, writer: LiveSeedWriter()
        )
        var guest: Guest?
        do {
            guest = try storage.load()
        } catch {
            problem = problem ?? "Android\u{2019}s settings could not be read: \(error.localizedDescription)"
        }
        guest = guest.map { launch.applied(to: $0, processorCount: host.processorCount) }

        let model = AppModel(
            guest: guest, host: host, system: system, capabilities: .firstRelease,
            createRequirementBytes: storage.createRequirementBytes,
            systemBytes: seed?.expandedBytes ?? 0
        )
        let options = DisplayOptionsStore.load()
        model.retina = launch.retina ?? options.retina
        model.followWindow = launch.followWindow ?? options.followWindow

        let marker = PauseMarker(url: Self.supportDirectory().appendingPathComponent("pause.json"))
        if marker.consume(), guest != nil {
            model.notice = .closedInBackground
            print("Andarium: the previous process ended out of the foreground; the guest starts again from boot")
        }
        if let commandLine {
            let runtime = LiveRuntime(
                model: model, storage: storage, commandLine: commandLine, marker: marker
            )
            model.driver = runtime
        }
        model.alert = problem.map { AppAlert(title: "Android Is Not Available", message: $0) }
        return model
    }

    /// An automation launch creates the guest if there is none, erases it
    /// when asked, and starts it; a normal launch stops at Home.
    @MainActor
    func applyDeferred(to model: AppModel) async {
        let launch = overrides
        guard launch.start != nil || launch.erase else { return }
        if model.guest == nil {
            model.create(launch.applied(to: Guest.fresh(on: model.host), processorCount: model.host.processorCount))
            guard await Self.idle(model) else { return }
        }
        if launch.erase {
            model.erase()
            guard await Self.idle(model) else { return }
        }
        if let mode = launch.start, model.alert == nil {
            model.start(mode)
        }
    }

    /// Waits for the busy overlay to go, within a bound; false if it did not.
    @MainActor
    private static func idle(_ model: AppModel) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(900)
        while model.busy != nil {
            if ContinuousClock.now > deadline { return false }
            try? await Task.sleep(for: .milliseconds(200))
        }
        return model.alert == nil
    }

    // MARK: Device

    private static func appVersion(_ bundle: Bundle) -> String {
        let short = bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "1.0"
        let build = bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "1"
        return "\(short) (\(build))"
    }

    /// `Library/Guest/`. The disk's path goes to Linux in a kernel argument
    /// that cannot carry a space, so the guest does not live in `Application
    /// Support`; the library directory's name is safe on a device and on a Mac.
    static func guestDirectory() -> URL {
        let library = (try? FileManager.default.url(
            for: .libraryDirectory, in: .userDomainMask, appropriateFor: nil, create: true
        )) ?? FileManager.default.temporaryDirectory
        return library.appendingPathComponent("Guest", isDirectory: true)
    }

    /// `Library/Application Support/`, for the application's own small files.
    static func supportDirectory() -> URL {
        (try? FileManager.default.url(
            for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true
        )) ?? FileManager.default.temporaryDirectory
    }

    @MainActor
    static func hostDevice() -> HostDevice {
        let info = ProcessInfo.processInfo
        let name: String
        let limitMiB: Int
        if info.isiOSAppOnMac {
            // A Mac has no per-application memory limit to read; half its
            // memory, up to the largest iPad's, is what the policy plans for.
            name = "Mac"
            limitMiB = min(Int(info.physicalMemory / 1_048_576) / 2, 12_288)
        } else {
            name = UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone"
            limitMiB = launchLimitMiB()
        }
        return HostDevice(
            name: name, processorCount: info.activeProcessorCount,
            memory: MemoryPolicy(limitMiB: limitMiB), freeStorageBytes: freeStorageBytes()
        )
    }

    /// L: what iOS lets the application use, which is what it has left plus
    /// what it already holds, read at launch before the guest exists.
    private static func launchLimitMiB() -> Int {
        let available = Int(os_proc_available_memory())
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
        let status = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        let footprint = status == KERN_SUCCESS ? Int(info.phys_footprint) : 0
        return max((available + footprint) / 1_048_576, 1024)
    }

    static func freeStorageBytes() -> UInt64 {
        let values = try? URL(fileURLWithPath: NSHomeDirectory())
            .resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return UInt64(max(values?.volumeAvailableCapacityForImportantUsage ?? 0, 0))
    }
}

/// The two display options, kept across launches.
enum DisplayOptionsStore {
    private static let key = "andarium.display"

    static func load() -> DisplayOptions {
        guard let data = UserDefaults.standard.data(forKey: key),
              let options = try? JSONDecoder().decode(DisplayOptions.self, from: data)
        else { return DisplayOptions() }
        return options
    }

    static func save(_ options: DisplayOptions) {
        UserDefaults.standard.set(try? JSONEncoder().encode(options), forKey: key)
    }
}

/// The system Andarium carries: the staged folder `andarium-system` in the
/// application bundle (`Andarium/scripts/stage-system-image.py`).
enum SystemFolder {
    static let name = "andarium-system"
    static let manifest = "release-manifest.json"
    static let commandLine = "kernel-command-line.txt"
    static let layout = "partition-layout.json"

    enum Problem: LocalizedError {
        case noArchive
        case severalArchives
        case noSize
        case badStagedImage(String)

        var errorDescription: String? {
            switch self {
            case .noArchive: "the disk archive is missing"
            case .severalArchives: "there is more than one disk archive"
            case .noSize: "the disk size is not declared"
            case .badStagedImage(let name): "the staged image \(name) cannot be used"
            }
        }
    }

    /// The seed: the archive in the folder, or an image staged in Documents
    /// by a harness launch.
    static func seed(in folder: URL, release: String, stagedName: String?) throws -> SeedImage {
        if let stagedName {
            let documents = try FileManager.default.url(
                for: .documentDirectory, in: .userDomainMask, appropriateFor: nil, create: false
            )
            let url = documents.appendingPathComponent(stagedName)
            guard FileManager.default.fileExists(atPath: url.path) else { throw Problem.badStagedImage(stagedName) }
            if stagedName.hasSuffix(".zst") {
                guard let handle = try? FileHandle(forReadingFrom: url),
                      let size = ZstdFrame.contentSize(header: handle.readData(ofLength: 18))
                else { throw Problem.badStagedImage(stagedName) }
                return SeedImage(url: url, release: "staged \(stagedName)", expandedBytes: size)
            }
            let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
            guard size > 0 else { throw Problem.badStagedImage(stagedName) }
            return SeedImage(url: url, release: "staged \(stagedName)", expandedBytes: UInt64(size))
        }
        let archives = try FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)
            .filter { $0.lastPathComponent.hasSuffix(".img.zst") }
        guard let archive = archives.first else { throw Problem.noArchive }
        guard archives.count == 1 else { throw Problem.severalArchives }
        let layoutData = try Data(contentsOf: folder.appendingPathComponent(layout))
        guard let object = try JSONSerialization.jsonObject(with: layoutData) as? [String: Any],
              let size = object["disk_size_bytes"] as? Int, size > 0
        else { throw Problem.noSize }
        return SeedImage(url: archive, release: release, expandedBytes: UInt64(size))
    }
}
