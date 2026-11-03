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
        /// The file is not an app package.
        case notApk
        /// The file is an app in several parts (.apks, .xapk, .apkm), which
        /// the install does not take.
        case bundle
        /// The guest's package installer refused it.
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

    /// The file name's kind for the install: an app in one file, one in
    /// several, or not an app.
    static func installKind(of fileName: String) -> ApkInstall.State? {
        let name = fileName.lowercased()
        if name.hasSuffix(".apk") { return nil }
        if [".apks", ".xapk", ".apkm"].contains(where: { name.hasSuffix($0) }) { return .bundle }
        return .notApk
    }

    /// Mock install: anything that is not an .apk is refused at once;
    /// an .apk installs over about three seconds.
    func install(fileName: String, sizeMB: Double, play: Bool = true) {
        if let refused = Self.installKind(of: fileName) {
            withAnimation(.snappy) {
                installs.insert(ApkInstall(fileName: fileName, sizeMB: sizeMB, state: refused), at: 0)
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

    /// Notes an install's end in the Apps list.
    func record(_ state: ApkInstall.State, fileName: String, sizeMB: Double) {
        withAnimation(.snappy) {
            installs.insert(ApkInstall(fileName: fileName, sizeMB: sizeMB, state: state), at: 0)
        }
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
    /// The Android operation under way (a bug report, an install), which the
    /// overlay's Cancel stops, and the run it belongs to.
    private var operation: Task<Void, Never>?
    private var operationEpoch = 0
    private var operationCancelled = false

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
                title: "Creating Android",
                detail: "Setting up \(system.distribution) with \(guest.effectiveCapacityGiB) GB.", progress: 0
            ),
            failure: "Android Could Not Be Created"
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
            alert = AppAlert(title: "Android Could Not Be Restored", message: error.localizedDescription)
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
            BusyState(title: "Erasing Android", detail: "Deleting everything Android stored.", progress: 0),
            failure: "Android Could Not Be Erased"
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

    // MARK: Android operations

    /// Runs one of Android's operations behind the busy overlay: Settings
    /// has dismissed, the overlay covers the screens, and a Cancel button
    /// stops it. `work` reports through the update closure and throws
    /// `CancellationError` when the person cancelled; any other error is an
    /// alert. `finished` runs once the overlay has gone.
    private func runOperation<Output: Sendable>(
        _ state: BusyState, failure: String,
        _ work: @escaping @MainActor (@escaping @Sendable (BusyUpdate) -> Void) async throws -> Output,
        finished: @escaping @MainActor (Output) -> Void = { _ in }
    ) {
        guard busy == nil else { return }
        operationEpoch += 1
        let epoch = operationEpoch
        operationCancelled = false
        var state = state
        state.cancel = BusyCancel { [weak self] in self?.cancelOperation() }
        busy = state
        operation = Task { [weak self] in
            guard let self else { return }
            var result: Output?
            do {
                result = try await work { update in
                    Task { @MainActor in self.apply(update, epoch: epoch) }
                }
            } catch is CancellationError {
            } catch {
                if operationEpoch == epoch {
                    alert = AppAlert(title: failure, message: error.localizedDescription)
                }
            }
            // The run ending, or a restart, took the operation over already.
            guard operationEpoch == epoch else { return }
            busy = nil
            operation = nil
            if let result { finished(result) }
        }
    }

    private func apply(_ update: BusyUpdate, epoch: Int) {
        guard epoch == operationEpoch, !operationCancelled, busy != nil else { return }
        // The new value is worked out before the write: reading `busy` while
        // writing through it is an overlapping access.
        if let progress = update.progress, let current = busy {
            busy?.progress = max(current.progress ?? 0, progress)
        }
        if let detail = update.detail { busy?.detail = detail }
        if update.cancellable == false { busy?.cancel = nil }
    }

    /// The overlay's Cancel: the operation stops and cleans up while the
    /// overlay stays, so the session returns with nothing left behind.
    private func cancelOperation() {
        guard operation != nil, !operationCancelled else { return }
        operationCancelled = true
        busy?.cancel = nil
        busy?.detail = "Cancelling\u{2026}"
        operation?.cancel()
    }

    /// The guest ended or restarted under an operation: its stream is gone
    /// with the boot, so the overlay goes and no alert is raised over the
    /// screen that says the guest stopped.
    private func abandonOperation() {
        guard operation != nil else { return }
        operationEpoch += 1
        operation?.cancel()
        operation = nil
        busy = nil
    }

    /// Create Bug Report: Settings has dismissed. Android collects its report
    /// (minutes, under emulation) and the finished zip goes to the share
    /// sheet, where Save to Files is one of the actions.
    func createBugReport() {
        guard capabilities.bugReport, case .running(.normal) = phase else { return }
        runOperation(
            BusyState(
                title: "Creating Bug Report",
                detail: "The guest is collecting its logs and state. This can take several minutes.", progress: 0
            ),
            failure: "Bug Report Not Created",
            { [driver] update in
                if let driver { return try await driver.createBugReport(update: update) }
                return try await Self.playBugReport(update)
            },
            finished: { [weak self] file in self?.share(file) }
        )
    }

    /// Opens the share sheet on a finished report; false when there is no
    /// window to show it. A test puts its own here.
    var presentShare: (URL, @escaping @MainActor () -> Void) -> Bool = { file, finished in
        BugReportSharing.present(file, finished: finished)
    }

    /// The report is deleted on every outcome of the sheet.
    private func share(_ file: URL) {
        let shown = presentShare(file) { try? FileManager.default.removeItem(at: file) }
        if !shown { try? FileManager.default.removeItem(at: file) }
    }

    /// The preview's report: the same steps on the same overlay, ending in
    /// the share sheet with a small file.
    private static func playBugReport(_ update: @Sendable (BusyUpdate) -> Void) async throws -> URL {
        for step in 1...36 {
            try await Task.sleep(for: .milliseconds(100))
            update(BusyUpdate(progress: Double(step) / 36 * 0.9))
        }
        update(BusyUpdate(progress: 0.9, detail: "Copying the report\u{2026}"))
        for step in 1...8 {
            try await Task.sleep(for: .milliseconds(100))
            update(BusyUpdate(progress: 0.9 + 0.1 * Double(step) / 8))
        }
        try FileManager.default.createDirectory(at: BugReportStaging.directory, withIntermediateDirectories: true)
        let file = BugReportStaging.directory.appendingPathComponent("bugreport-preview.zip")
        // An empty zip: its end-of-central-directory record.
        try Data([0x50, 0x4B, 0x05, 0x06] + [UInt8](repeating: 0, count: 18)).write(to: file)
        return file
    }

    /// Install App: `url` is what the Files picker returned. A file that does
    /// not end in .apk (a split bundle too) is answered at once with a row in
    /// Settings, which stays open: the result is false. For an .apk the
    /// caller dismisses Settings and the install runs behind the overlay:
    /// the result is true. The file's security scope is held to the end.
    func installPicked(_ url: URL) -> Bool {
        let name = url.lastPathComponent
        let scoped = url.startAccessingSecurityScopedResource()
        let bytes = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        let megabytes = Double(bytes) / 1_000_000
        func release() {
            if scoped { url.stopAccessingSecurityScopedResource() }
        }
        guard SessionState.installKind(of: name) == nil else {
            release()
            session.install(fileName: name, sizeMB: megabytes)
            return false
        }
        guard busy == nil else {
            release()
            return false
        }
        runOperation(
            BusyState(
                title: "Installing App",
                detail: "\(name) \u{00B7} \(String(format: "%.1f", megabytes)) MB", progress: 0
            ),
            failure: "App Not Installed",
            { [driver, weak self] update in
                defer { release() }
                do {
                    if let driver {
                        try await driver.installApk(at: url, sizeBytes: bytes, update: update)
                    } else {
                        try await Self.playInstall(update)
                    }
                } catch is CancellationError {
                    throw CancellationError()
                } catch {
                    self?.session.record(.failed(error.localizedDescription), fileName: name, sizeMB: megabytes)
                    throw error
                }
                // 100% for a moment, then the overlay goes.
                update(BusyUpdate(progress: 1))
                try? await Task.sleep(for: .milliseconds(500))
                self?.session.record(.installed, fileName: name, sizeMB: megabytes)
            }
        )
        return true
    }

    /// The preview's install: the file goes over, the guest takes its time.
    private static func playInstall(_ update: @Sendable (BusyUpdate) -> Void) async throws {
        for step in 1...20 {
            try await Task.sleep(for: .milliseconds(100))
            update(BusyUpdate(progress: Double(step) / 20 * 0.8))
        }
        update(BusyUpdate(detail: "The guest is installing the app\u{2026}", cancellable: false))
        try await Task.sleep(for: .milliseconds(1200))
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
                title: "Android's Update Didn't Finish",
                message: plan.allowsStart
                    ? "Recovery starts only once the update has finished. Start tries the update again; Restore or Erase Guest goes back."
                    : "The guest starts only after its data is restored or the guest is erased."
            )
        case let .blocked(reason):
            alert = AppAlert(title: reason.title, message: reason.explanation)
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
                guest?.usedBytes = 2_100_000_000
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
        abandonOperation()
        runEnd = end
        runUptimeSeconds = uptimeSeconds
        lifecycle.phase = end.phase
        persistNow()
        refreshPlan()
    }

    /// Android's own Restart boots the guest again in place. ADB and the
    /// install list belong to the run that ended.
    func restart() {
        abandonOperation()
        frozenBootStage = nil
        sessionPreset = SessionPreset()
        let showStats = session.showStats
        session = SessionState()
        session.showStats = showStats
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
