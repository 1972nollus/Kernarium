import AndariumCore
import Observation
import SwiftUI

enum SheetKind: String, Identifiable, Hashable {
    case create, resize, erase, info

    var id: String { rawValue }
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

/// What the screens read and change. The guest's life (create, start, boot,
/// erase, how a run ends) is `Lifecycle` in AndariumCore; this adds what only
/// a screen holds: the open sheet, display options and the session's state.
@Observable
final class AppModel {
    var lifecycle: Lifecycle
    var host: HostDevice
    /// The system this build of Andarium carries.
    var system: SystemInfo
    /// What the runtime behind the screens can do; a screen shows only the
    /// controls whose capability exists.
    var capabilities: Capabilities
    /// Free space a new guest needs, which the storage behind the screens
    /// decides.
    var createRequirementBytes: UInt64
    /// What the system takes on this device.
    var systemBytes: UInt64
    var sheet: SheetKind?
    var startMenu = false
    var startBlocked = false

    // App-wide display options, set on the home screen.
    var retina = false
    var followWindow = true

    var sessionPreset = SessionPreset()
    var session = SessionState()
    /// How the last run ended, for the screen that says so.
    var runEnd = RunEnd(result: 0)
    var runUptimeSeconds: Int?
    /// Where Resize opens; nil opens at the current capacity.
    var resizePreset: Int?
    /// Holds the boot screen at one stage instead of animating (screenshots).
    var frozenBootStage: Int?
    /// Seconds the boot screen shows when it is held.
    var frozenElapsed = 9

    init(
        guest: Guest?, host: HostDevice, system: SystemInfo, capabilities: Capabilities,
        createRequirementBytes: UInt64, systemBytes: UInt64
    ) {
        lifecycle = Lifecycle(guest: guest)
        self.host = host
        self.system = system
        self.capabilities = capabilities
        self.createRequirementBytes = createRequirementBytes
        self.systemBytes = systemBytes
    }

    var guest: Guest? {
        get { lifecycle.guest }
        set { lifecycle.guest = newValue }
    }

    var phase: Phase {
        get { lifecycle.phase }
        set { lifecycle.phase = newValue }
    }

    var notice: HomeNotice? {
        get { lifecycle.notice }
        set { lifecycle.notice = newValue }
    }

    var canCreate: Bool { host.hasRoom(for: createRequirementBytes) }
    /// Less free space than Android's first start wants: Home says so before
    /// a start would fail.
    var lowSpace: Bool { !host.hasRoom(for: Limits.firstStartReserveBytes) }

    /// The release a guest is compared with, where this build can move a
    /// guest to a newer system.
    private var updateTarget: String? { capabilities.systemUpdate ? system.release : nil }

    /// What a start of the guest would be, for the words on Start.
    func bootKind(_ mode: StartMode) -> BootKind {
        guest?.bootKind(mode, currentRelease: updateTarget) ?? .normal
    }

    func create(_ guest: Guest) {
        lifecycle.create(guest)
    }

    func erase() {
        lifecycle.erase()
    }

    func start(_ mode: StartMode) {
        startMenu = false
        switch lifecycle.start(mode, host: host, currentRelease: updateTarget) {
        case .allowed: frozenBootStage = nil
        case .lowSpace: startBlocked = true
        case .noGuest: break
        }
    }

    func finishBoot(_ kind: BootKind) {
        let firstStart = guest?.neverStarted ?? false
        lifecycle.finishBoot(kind, release: system.release)
        if firstStart, kind != .recovery {
            sessionPreset.guestScreen = .setup
            guest?.usedBytes = 2_100_000_000
        }
    }

    func powerOff() {
        runEnd = RunEnd(result: 0)
        lifecycle.powerOff()
    }

    func fail(_ end: RunEnd = RunEnd(result: -11), uptimeSeconds: Int? = 2530) {
        runEnd = end
        runUptimeSeconds = uptimeSeconds
        lifecycle.fail()
    }

    /// Android's own Restart boots the guest again in place. ADB and the
    /// install list belong to the run that ended.
    func restart() {
        frozenBootStage = nil
        sessionPreset = SessionPreset()
        let showStats = session.showStats
        session = SessionState()
        session.showStats = showStats
        lifecycle.restart()
    }

    func factoryReset() {
        lifecycle.factoryReset()
    }

    /// The failure screen's copyable report.
    var runReport: String {
        runEnd.report(
            appVersion: system.appVersion, system: system, guest: guest, uptimeSeconds: runUptimeSeconds
        )
    }
}
