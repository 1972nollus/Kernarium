import Foundation
import Observation
import SwiftUI
import UIKit

/// The one system Andarium carries. It is read-only, ships inside the app and
/// changes only when Andarium itself is updated from the App Store.
enum SystemImage {
    static let distribution = "LineageOS 24"
    static let android = "17"
    static let build = "20261024"
    static let previousBuild = "20261002"
    static let securityPatch = "5 October 2026"
    static let kernel = "Linux 6.18.54"
    static let architecture = "arm64, 4 KB pages"
    static let sizeGB = 2.8
    static let appVersion = "1.0 (1)"
}

/// What the iPhone or iPad running Andarium offers. Mock values per idiom.
struct HostDevice {
    var name: String
    var processorCount: Int
    var recommendedMemoryMiB: Int
    var maximumMemoryMiB: Int
    var freeStorageGB: Double

    static func current(lowSpace: Bool) -> HostDevice {
        if UIDevice.current.userInterfaceIdiom == .pad {
            return HostDevice(
                name: "iPad", processorCount: 10,
                recommendedMemoryMiB: 6144, maximumMemoryMiB: 12288,
                freeStorageGB: lowSpace ? 1.2 : 118.3
            )
        }
        return HostDevice(
            name: "iPhone", processorCount: 6,
            recommendedMemoryMiB: 4096, maximumMemoryMiB: 8192,
            freeStorageGB: lowSpace ? 1.2 : 41.6
        )
    }
}

enum Limits {
    /// Storage capacity, set in whole gigabytes.
    static let capacityRangeGiB = 4...64
    static let defaultCapacityGiB = 8
    static let defaultMemoryMiB = 2048
    static let minimumMemoryMiB = 1024
    static let memoryStepMiB = 512
    /// Free space a new guest needs to finish Android's first-run setup.
    static let createReserveGB = 3.0
    /// Free space any start needs for Android's own writes.
    static let startReserveGB = 1.5

    static func memoryStops(maximum: Int) -> [Int] {
        Array(stride(from: minimumMemoryMiB, through: maximum, by: memoryStepMiB))
    }
}

enum StartMode: Hashable {
    case normal, recovery
}

/// Why a start takes as long as it does.
enum BootKind: Hashable {
    case normal, firstStart, upgrade, recovery, restart
}

/// The one guest Andarium keeps: Android's data partition (apps, accounts,
/// settings and files) with the processor, memory and capacity it runs with.
struct Guest: Hashable {
    var cores: Int
    var memoryMiB: Int
    var capacityGiB: Int
    /// A larger capacity that applies when the guest next starts.
    var pendingCapacityGiB: Int?
    /// Data Android has written; the sparse image occupies about this much.
    var usedGB: Double
    var apps: Int
    /// The system build this guest last ran; nil until its first start.
    var systemBuild: String?
    var microphone = false

    var neverStarted: Bool { systemBuild == nil }

    /// The capacity the next start runs with.
    var effectiveCapacityGiB: Int { pendingCapacityGiB ?? capacityGiB }

    /// Storage only grows, up to the largest capacity there is.
    var canGrow: Bool { effectiveCapacityGiB < Limits.capacityRangeGiB.upperBound }

    var needsUpgrade: Bool {
        guard let systemBuild else { return false }
        return systemBuild != SystemImage.build
    }

    func bootKind(_ mode: StartMode) -> BootKind {
        if mode == .recovery { return .recovery }
        if neverStarted { return .firstStart }
        if needsUpgrade { return .upgrade }
        return .normal
    }

    /// Resize: storage only grows, and the growth waits for the next start.
    /// Choosing the current capacity again cancels a growth not yet applied.
    mutating func setCapacity(_ value: Int) {
        let value = min(max(value, capacityGiB), Limits.capacityRangeGiB.upperBound)
        pendingCapacityGiB = value > capacityGiB ? value : nil
    }

    /// The same choices with nothing in it: what Erase leaves behind.
    var erased: Guest {
        Guest(
            cores: cores, memoryMiB: memoryMiB, capacityGiB: effectiveCapacityGiB,
            pendingCapacityGiB: nil, usedGB: 0, apps: 0, systemBuild: nil,
            microphone: microphone
        )
    }

    static func fresh(host: HostDevice) -> Guest {
        Guest(
            cores: host.processorCount, memoryMiB: Limits.defaultMemoryMiB,
            capacityGiB: Limits.defaultCapacityGiB, usedGB: 0, apps: 0, systemBuild: nil
        )
    }

    /// A guest in everyday use.
    static func sample(host: HostDevice) -> Guest {
        Guest(
            cores: host.processorCount, memoryMiB: 3072, capacityGiB: 16,
            usedGB: 5.4, apps: 23, systemBuild: SystemImage.build
        )
    }
}

enum Phase: Equatable {
    case home
    case booting(BootKind)
    case running(StartMode)
    case poweredOff
    case failed
}

enum SheetKind: String, Identifiable, Hashable {
    case create, resize, erase, info

    var id: String { rawValue }
}

enum HomeNotice: Equatable {
    /// The guest was just erased.
    case erased
    /// This version of Andarium carries a new system.
    case systemUpdated
    /// iOS ended Andarium while the guest was paused in the background.
    case closedInBackground
}

/// What a launch argument asks the session to show first.
struct SessionPreset: Equatable {
    var keysOpen = false
    /// Android's volume panel, as after a volume key press.
    var volumePanel = false
    var settings = false
    /// Settings opens at full height instead of half.
    var settingsLarge = false
    var powerMenu = false
    var leftEdge = false
    var guestScreen: GuestScreen = .home
}

enum GuestScreen: Equatable {
    /// Android's home screen.
    case home
    /// The launcher's search with Android's own keyboard up.
    case search
    /// Android's first-run setup.
    case setup
    /// Android's screen asleep.
    case off
}

/// One APK handed to Android from the session's Settings.
struct ApkInstall: Identifiable, Equatable {
    enum State: Equatable {
        case installing(Double)
        case installed
        /// The file is not an Android package.
        case notApk
        /// Android's package installer refused it.
        case failed(String)
    }

    let id = UUID()
    var fileName: String
    var sizeMB: Double
    var state: State

    /// "Signal-7.21.4.apk" installs as Signal, version 7.21.4.
    var appName: String {
        let base = (fileName as NSString).deletingPathExtension
        let name = base.split(whereSeparator: { $0 == "-" || $0 == "_" }).first.map(String.init) ?? base
        return name.prefix(1).uppercased() + name.dropFirst()
    }

    var version: String? {
        let base = (fileName as NSString).deletingPathExtension
        let parts = base.split(whereSeparator: { $0 == "-" || $0 == "_" })
        return parts.dropFirst().first { $0.first?.isNumber == true }.map(String.init)
    }

    static let sampleFile = "Signal-7.21.4.apk"
    static let sampleSizeMB = 38.2
}

/// What the running guest offers through Settings: installs, ADB and the
/// stats overlay. It lasts for one run.
@Observable
final class SessionState {
    var installs: [ApkInstall] = []
    var adb = false
    var showStats = false
    var readout = Readout()

    /// The address a computer on the same network connects to.
    static let adbAddress = "198.51.100.23:5555"

    /// Mock install: anything that is not an .apk is refused at once;
    /// an .apk installs over about three seconds.
    func install(fileName: String, sizeMB: Double, play: Bool = true) {
        guard fileName.lowercased().hasSuffix(".apk") else {
            withAnimation(.snappy) {
                installs.insert(ApkInstall(fileName: fileName, sizeMB: sizeMB, state: .notApk), at: 0)
            }
            return
        }
        let item = ApkInstall(fileName: fileName, sizeMB: sizeMB, state: .installing(0))
        withAnimation(.snappy) { installs.insert(item, at: 0) }
        guard play else { return }
        Task {
            for step in 1...20 {
                try? await Task.sleep(for: .milliseconds(150))
                update(item.id, .installing(Double(step) / 20))
            }
            try? await Task.sleep(for: .milliseconds(250))
            withAnimation(.snappy) { update(item.id, .installed) }
        }
    }

    func update(_ id: UUID, _ state: ApkInstall.State) {
        guard let index = installs.firstIndex(where: { $0.id == id }) else { return }
        installs[index].state = state
    }

    func remove(_ id: UUID) {
        installs.removeAll { $0.id == id }
    }
}

@Observable
final class AppModel {
    /// Andarium holds at most one guest.
    var guest: Guest?
    var host: HostDevice
    var phase: Phase = .home
    var sheet: SheetKind?
    var startMenu = false
    var startBlocked = false
    var notice: HomeNotice?

    // App-wide display options, set on the home screen.
    var retina = false
    var followWindow = true

    var sessionPreset = SessionPreset()
    var session = SessionState()
    /// Where Resize opens; nil opens at the current capacity.
    var resizePreset: Int?
    /// Holds the boot screen at one stage instead of animating (screenshots).
    var frozenBootStage: Int?
    /// Seconds the boot screen shows when it is held.
    var frozenElapsed = 9

    init(guest: Guest?, host: HostDevice) {
        self.guest = guest
        self.host = host
    }

    var canCreate: Bool { host.freeStorageGB >= Limits.createReserveGB }
    var canStart: Bool { host.freeStorageGB >= Limits.startReserveGB }
    var lowSpace: Bool { host.freeStorageGB < Limits.createReserveGB }

    func create(_ guest: Guest) {
        self.guest = guest
        notice = nil
    }

    /// Erases everything in the guest and leaves a fresh one with the same
    /// choices; its next start runs Android's first-run setup.
    func erase() {
        guard let guest else { return }
        self.guest = guest.erased
        notice = .erased
    }

    func start(_ mode: StartMode) {
        startMenu = false
        guard canStart else {
            startBlocked = true
            return
        }
        guard let guest else { return }
        frozenBootStage = nil
        phase = .booting(guest.bootKind(mode))
    }

    func finishBoot(_ kind: BootKind) {
        if var guest, kind != .recovery {
            if let pending = guest.pendingCapacityGiB {
                guest.capacityGiB = pending
                guest.pendingCapacityGiB = nil
            }
            if guest.neverStarted {
                sessionPreset.guestScreen = .setup
                guest.usedGB = 2.1
            }
            guest.systemBuild = SystemImage.build
            self.guest = guest
        }
        phase = .running(kind == .recovery ? .recovery : .normal)
    }

    func powerOff() {
        phase = .poweredOff
    }

    func fail() {
        phase = .failed
    }

    /// Android's own Restart boots the guest again in place. ADB and the
    /// install list belong to the run that ended.
    func restart() {
        frozenBootStage = nil
        sessionPreset = SessionPreset()
        let showStats = session.showStats
        session = SessionState()
        session.showStats = showStats
        phase = .booting(.restart)
    }

    /// Android's factory reset from Recovery: the same result as Erase, done
    /// from inside.
    func factoryReset() {
        guard let guest else { return }
        self.guest = guest.erased
    }
}

enum Formats {
    static func gb(_ value: Double) -> String {
        if value <= 0 { return "0 GB" }
        if value < 0.95 { return String(format: "%.0f MB", value * 1000) }
        if value >= 10 || value.rounded() == value { return String(format: "%.0f GB", value) }
        return String(format: "%.1f GB", value)
    }

    static func memoryNumber(_ mib: Int) -> String {
        let gb = Double(mib) / 1024
        if gb.rounded() == gb { return String(format: "%.0f", gb) }
        return String(format: "%.1f", gb)
    }

    static func memory(_ mib: Int) -> String { memoryNumber(mib) + " GB" }

    static func duration(_ minutes: Int) -> String {
        if minutes % 60 == 0 { return minutes == 60 ? "1 hour" : "\(minutes / 60) hours" }
        return "\(minutes) min"
    }

    static func elapsed(_ seconds: Int) -> String {
        String(format: "%d:%02d", seconds / 60, seconds % 60)
    }
}
