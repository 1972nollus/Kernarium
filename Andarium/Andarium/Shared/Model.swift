import AndariumCore
import Observation
import SwiftUI

/// A view that shows the guest's statistics.
enum StatisticsViewer: Hashable {
    /// The capsule over the session.
    case pill
    /// The Stats rows of the session's Settings.
    case settings
}

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
    var sizeBytes: UInt64
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
    static let sampleSizeBytes = UInt64(38.2 * Double(Limits.bytesPerMiB))
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
    func install(fileName: String, sizeBytes: UInt64, play: Bool = true) {
        guard fileName.lowercased().hasSuffix(".apk") else {
            withAnimation(.snappy) {
                installs.insert(ApkInstall(fileName: fileName, sizeBytes: sizeBytes, state: .notApk), at: 0)
            }
            return
        }
        let item = ApkInstall(fileName: fileName, sizeBytes: sizeBytes, state: .installing(0))
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
    var lifecycle: Lifecycle {
        // The readings run while the guest does.
        didSet { if lifecycle.phase != oldValue.phase { refreshStatistics() } }
    }
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
    /// What a start would be, from the guest's files: Home words Start and
    /// shows the update notices by it.
    var plan: StartPlan = .normal
    /// The release the running boot updates from, where it is known.
    var bootFrom: String?
    /// The preview's plan after Restore; a device reads it from the files.
    var planAfterRestore: StartPlan?

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

    /// The runtime and storage behind the screens; nil plays mock data.
    var driver: (any RuntimeDriver)?
    /// A long task the screens wait for.
    var busy: BusyState?
    var alert: AppAlert?
    /// How far the current boot has got, from the guest's console.
    var bootTracker = BootTracker()
    /// The guest's disk is being written before the run can begin.
    var preparingDisk = false
    var diskProgress: Double?
    var microphoneState = MicrophoneState.notAsked
    private var persistTask: Task<Void, Never>?

    // MARK: Statistics state

    /// The views that show the statistics. Nothing is read while there are
    /// none.
    private(set) var statisticsViewers: Set<StatisticsViewer> = []
    /// Whether the application is the active one. The readings stop when it
    /// is not, and start again with a new baseline.
    var applicationActive = true {
        didSet { if applicationActive != oldValue { refreshStatistics() } }
    }
    /// The figures behind the screens, from the driver's readings.
    private(set) var statisticsMeter = StatsMeter()
    /// A launch for automation prints each reading.
    var printsStatistics = false
    /// Waits between readings; a test paces them by hand.
    @ObservationIgnored var statisticsSleep: @MainActor (Duration) async -> Void = { duration in
        try? await Task.sleep(for: duration)
    }
    @ObservationIgnored private var statisticsTask: Task<Void, Never>?
    /// Tells a loop that ended from the one that replaced it.
    @ObservationIgnored private var statisticsGeneration = 0

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
        set {
            lifecycle.guest = newValue
            persistSoon()
        }
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

    /// What a start of the guest would be, for the words on Start.
    func bootKind(_ mode: StartMode) -> BootKind {
        guest == nil ? .normal : plan.bootKind(mode)
    }

    /// Reads the plan from the guest's files again: after the guest is made,
    /// erased or has run, which change what a start is.
    func refreshPlan() {
        guard let driver else { return }
        // A record that cannot be read has no plan; the start then fails
        // with the reason.
        plan = driver.startPlan() ?? .normal
    }

    /// Records the guest's choices a moment after the last change, so a
    /// drag on a ruler writes once.
    private func persistSoon() {
        guard driver != nil else { return }
        persistTask?.cancel()
        persistTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(300))
            guard !Task.isCancelled else { return }
            self?.persistNow()
        }
    }

    func persistNow() {
        persistTask?.cancel()
        guard let driver, let guest else { return }
        do {
            try driver.save(guest)
        } catch {
            alert = AppAlert(title: "Settings Not Saved", message: error.localizedDescription)
        }
    }

    func create(_ guest: Guest) {
        guard let driver else {
            lifecycle.create(guest)
            plan = .firstStart
            return
        }
        runBusy(
            BusyState(
                title: "Creating Guest",
                detail: "Setting up the system with \(Formats.size(gib: guest.effectiveCapacityGiB)).", progress: 0
            ),
            failure: "The Guest Could Not Be Created"
        ) { progress in
            let made = try await driver.create(guest, progress: progress)
            self.lifecycle.create(made)
            self.refreshPlan()
        }
    }

    /// Restore: the data goes back to what the update that did not finish
    /// started from, and the guest does not start. Home then shows the
    /// ordinary pending update.
    func restoreUpdate() {
        guard let driver else {
            if let planAfterRestore { plan = planAfterRestore }
            return
        }
        do {
            try driver.restoreUpdate()
        } catch {
            alert = AppAlert(title: "The Guest Could Not Be Restored", message: error.localizedDescription)
        }
        refreshPlan()
    }

    func erase() {
        guard let driver, let current = guest else {
            lifecycle.erase()
            plan = .firstStart
            return
        }
        runBusy(
            BusyState(title: "Erasing Guest", detail: "Deleting everything the guest stored.", progress: 0),
            failure: "The Guest Could Not Be Erased"
        ) { progress in
            _ = try await driver.erase(current, progress: progress)
            self.lifecycle.erase()
            self.persistNow()
            self.refreshPlan()
        }
    }

    /// Runs `work` behind the busy overlay, with a failure alert if it throws.
    private func runBusy(
        _ state: BusyState, failure: String,
        _ work: @escaping @MainActor (@escaping @Sendable (Double) -> Void) async throws -> Void
    ) {
        guard busy == nil else { return }
        busy = state
        Task { [weak self] in
            guard let self else { return }
            do {
                try await work { fraction in
                    Task { @MainActor in self.busy?.progress = fraction }
                }
            } catch {
                alert = AppAlert(title: failure, message: error.localizedDescription)
            }
            busy = nil
        }
    }

    /// Starts the guest in `mode`. Start on an update that did not finish
    /// tries the update again.
    func start(_ mode: StartMode) {
        startMenu = false
        switch lifecycle.start(mode, host: host, plan: plan) {
        case let .allowed(kind):
            frozenBootStage = nil
            bootTracker = BootTracker()
            preparingDisk = false
            diskProgress = nil
            if case let .update(from, _) = plan { bootFrom = from } else { bootFrom = nil }
            if let driver, let guest {
                persistNow()
                driver.begin(guest: guest, mode: mode, kind: kind)
            }
        case .lowSpace: startBlocked = true
        case .unfinishedUpdate:
            alert = AppAlert(
                title: "The System's Update Didn't Finish",
                message: plan.allowsStart
                    ? "Recovery starts only once the update has finished. Start tries the update again; Restore or Erase Guest goes back."
                    : "The guest starts only after its data is restored or the guest is erased."
            )
        case let .blocked(reason):
            alert = AppAlert(title: reason.title, message: reason.explanation)
        case .needsNewerChip:
            alert = AppAlert(title: DeviceRequirement.title, message: DeviceRequirement.explanation(deviceName: host.name))
        case .noGuest: break
        }
    }

    func finishBoot(_ kind: BootKind) {
        let firstStart = plan == .firstStart
        lifecycle.finishBoot(kind)
        if driver != nil {
            persistNow()
        } else if kind != .recovery {
            // The preview plays the instance too: the first start sets
            // Android up, and an update or a retry ends.
            plan = .normal
            if firstStart {
                sessionPreset.guestScreen = .setup
                guest?.usedBytes = UInt64(2.1 * Double(Limits.bytesPerGiB))
            }
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

    /// The runtime's run returned: the guest powered off, or stopped for a
    /// reason the failure screen reports.
    func runEnded(_ end: RunEnd, uptimeSeconds: Int?) {
        runEnd = end
        runUptimeSeconds = uptimeSeconds
        lifecycle.phase = end.phase
        persistNow()
        refreshPlan()
    }

    /// Android's own Restart boots the guest again in place. ADB and the
    /// install list belong to the run that ended.
    func restart() {
        frozenBootStage = nil
        sessionPreset = SessionPreset()
        let showStats = session.showStats
        session = SessionState()
        session.showStats = showStats
        statisticsMeter = StatsMeter()
        bootTracker = BootTracker()
        lifecycle.restart()
    }

    func factoryReset() {
        lifecycle.factoryReset()
        // Only the preview knows the data is empty; on a device the data
        // disk stays and Android sets itself up again by itself.
        if driver == nil { plan = .firstStart }
        persistNow()
    }

    /// The microphone switch: the guest's choice, then the system's answer.
    func setMicrophone(_ on: Bool) {
        guard guest != nil else { return }
        guest?.microphone = on
        persistNow()
        driver?.setMicrophone(on)
    }

    // MARK: Statistics

    /// A view that shows the statistics appears or goes. The readings run
    /// while some view shows them and everything else allows
    /// (`StatsSampling.isWanted`), and one loop serves every view.
    func statistics(_ viewer: StatisticsViewer, visible: Bool) {
        if visible {
            statisticsViewers.insert(viewer)
        } else {
            statisticsViewers.remove(viewer)
        }
        refreshStatistics()
    }

    private var statisticsWanted: Bool {
        var running = false
        if case .running = phase { running = true }
        return StatsSampling.isWanted(
            viewers: statisticsViewers.count, applicationActive: applicationActive,
            capable: capabilities.statistics, hasDriver: driver != nil, running: running
        )
    }

    /// Starts the loop when the readings are wanted and none runs, and stops
    /// it when they are not.
    func refreshStatistics() {
        if statisticsWanted {
            guard statisticsTask == nil else { return }
            statisticsGeneration += 1
            let generation = statisticsGeneration
            statisticsTask = Task { [weak self] in
                await self?.runStatisticsLoop()
                if let self, self.statisticsGeneration == generation { self.statisticsTask = nil }
            }
        } else if let task = statisticsTask {
            task.cancel()
            statisticsTask = nil
            statisticsGeneration += 1
        }
    }

    /// Two readings a moment apart, so figures show within about a third of
    /// a second, and the guest's memory after the second. Then a reading
    /// every second and the memory with every other one. A result that
    /// arrives after the loop was stopped is dropped.
    private func runStatisticsLoop() async {
        guard let driver else { return }
        statisticsMeter.pause()
        var readings = 0
        while !Task.isCancelled, statisticsWanted {
            if let sample = await driver.readStatistics(), !Task.isCancelled {
                statisticsMeter.record(sample)
                readings += 1
                if readings.isMultiple(of: StatsSampling.memoryEvery) {
                    if let memory = await driver.readGuestMemory(), !Task.isCancelled {
                        statisticsMeter.record(memory: memory)
                    }
                }
                publishStatistics()
            }
            guard !Task.isCancelled else { return }
            await statisticsSleep(readings == 1 ? StatsSampling.firstGap : StatsSampling.interval)
        }
    }

    private func publishStatistics() {
        session.readout = Readout(statisticsMeter)
        #if !APP_STORE
        if printsStatistics {
            print(ConsoleReport.statistics(statisticsMeter, guestMemoryMiB: guest?.memoryMiB ?? 0))
        }
        #endif
    }

    /// The step of the boot under way, from the runtime's milestones.
    var liveBootStage: Int {
        BootSteps.current(preparingDisk: preparingDisk, milestone: bootTracker.milestone)
    }

    /// The failure screen's copyable report.
    var runReport: String {
        runEnd.report(
            appVersion: system.appVersion, system: system, guest: guest, uptimeSeconds: runUptimeSeconds
        )
    }
}
