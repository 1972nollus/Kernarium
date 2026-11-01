import AndariumCore
import Foundation
import Observation
import UIKit

/// The guest console, read on the runtime's thread for the boot's milestones.
nonisolated final class ConsoleSink: @unchecked Sendable {
    private let lock = NSLock()
    private var scanner = ConsoleScanner()
    private let report: @Sendable ([BootMilestone]) -> Void

    init(report: @escaping @Sendable ([BootMilestone]) -> Void) {
        self.report = report
    }

    func feed(_ data: Data) {
        let found = lock.withLock { scanner.feed(data) }
        if !found.isEmpty { report(found) }
    }
}

/// A run the model asked for, waiting for the disk and the display.
private struct PendingRun {
    var guest: Guest
    var mode: StartMode
    var kind: BootKind
}

/// The device application behind the screens: the guest's storage, and the
/// runtime that runs it. One run is made per launch, on a thread of its own,
/// into the display view the screens keep alive.
@MainActor
final class LiveRuntime: RuntimeDriver {
    /// The loader's flavour for the Android runtime.
    private static let androidFlavor: UInt32 = 1
    /// Android's power key held this long raises its power menu.
    nonisolated private static let powerMenuHold = 0.8

    private weak var model: AppModel?
    private let storage: SingleImageStorage
    private let commandLine: URL
    private let pause: PauseController
    private let microphone = SystemMicrophonePermission()
    private var observers: [NSObjectProtocol] = []

    private var pending: PendingRun?
    private var launching = false
    private var preparing = false
    private var view: GuestMetalView?
    private var geometry: GuestDisplayGeometry?
    private var bootStartedAt = Date()
    private var runStartedAt: Date?
    private var thermalObserver: NSObjectProtocol?
    private var console: ConsoleSink?

    init(model: AppModel, storage: SingleImageStorage, commandLine: URL, marker: PauseMarker) {
        self.model = model
        self.storage = storage
        self.commandLine = commandLine
        pause = PauseController(effects: LivePauseEffects(), marker: marker)
        model.microphoneState = Self.state(microphone.access)
        observeLifecycle()
        observeDisplayOptions()
    }

    // MARK: Storage

    func create(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest {
        defer { model?.host.freeStorageBytes = AppEntry.freeStorageBytes() }
        return try await storage.create(guest, progress: progress)
    }

    func erase(_ guest: Guest, progress: @escaping @Sendable (Double) -> Void) async throws -> Guest {
        defer { model?.host.freeStorageBytes = AppEntry.freeStorageBytes() }
        return try await storage.erase(guest, progress: progress)
    }

    func save(_ guest: Guest) throws {
        try storage.save(guest)
    }

    // MARK: Starting

    func begin(guest: Guest, mode: StartMode, kind: BootKind) {
        pending = PendingRun(guest: guest, mode: mode, kind: kind)
        launching = false
        startIfReady()
    }

    /// The display view has a size. The first layout after a run is wanted
    /// starts it.
    func viewDidLayout(_ view: GuestMetalView, _ geometry: GuestDisplayGeometry) {
        self.view = view
        self.geometry = geometry
        startIfReady()
    }

    private func startIfReady() {
        guard let run = pending, !launching, view != nil, geometry != nil else { return }
        pending = nil
        launching = true
        Task { await prepareAndLaunch(run) }
    }

    private func prepareAndLaunch(_ run: PendingRun) async {
        preparing = true
        do {
            try await storage.prepareStart(run.guest) { fraction in
                Task { @MainActor in self.preparingDisk(fraction) }
            }
        } catch {
            preparing = false
            end(startFailure: "the disk could not be prepared: \(error.localizedDescription)")
            return
        }
        preparing = false
        model?.preparingDisk = false
        guard let view, let geometry else {
            end(startFailure: "the display is not ready")
            return
        }
        launch(run, view: view, geometry: geometry)
    }

    private func preparingDisk(_ fraction: Double) {
        guard preparing, let model else { return }
        model.preparingDisk = true
        model.diskProgress = fraction
    }

    private func end(startFailure: String) {
        print("Andarium: cannot start the guest: \(startFailure)")
        model?.runEnded(RunEnd(result: -1, startFailure: startFailure), uptimeSeconds: nil)
    }

    private func launch(_ run: PendingRun, view: GuestMetalView, geometry: GuestDisplayGeometry) {
        guard kernarium_runtime_loader_select(Self.androidFlavor) == 0 else {
            end(startFailure: "the runtime could not be loaded")
            return
        }
        // The runtime boots a guest that restarts itself again inside the
        // run, and says when. The loader delivers each event on the main
        // queue.
        kernarium_runtime_loader_set_restart_handler { @Sendable [weak self] event, restartCount in
            guard let restart = GuestRestartEvent(event: event, restartCount: restartCount) else { return }
            MainActor.assumeIsolated { self?.guestRestarted(restart) }
        }
        let sink = ConsoleSink { milestones in
            Task { @MainActor in self.reached(milestones) }
        }
        console = sink
        kernarium_runtime_loader_set_console_output_handler { data in sink.feed(data) }
        // A Microphone setting the person turned on earlier follows the
        // guest; one that is off leaves the microphone alone.
        if run.guest.microphone { tellMicrophone(true) }

        pause.guestDidStart()
        bootStartedAt = Date()
        runStartedAt = Date()
        startThermalObservation()
        let size = GuestInputCoordinateSpace.boundedDisplaySize(geometry.pixelSize)
        let width = UInt32(size.width.rounded(.down))
        let height = UInt32(size.height.rounded(.down))
        view.setGuestDisplay(width: width, height: height)
        UIApplication.shared.isIdleTimerDisabled = true

        let diskPath = storage.diskPath
        let commandLinePath = commandLine.path
        let bootMode: UInt32 = run.mode == .recovery ? 2 : 1
        let memoryMiB = UInt32(run.guest.memoryMiB)
        let processorCount = UInt32(run.guest.cores)
        let dotsPerInch = geometry.dotsPerInch
        let autoResize = model?.followWindow ?? true
        // The run occupies its thread for the guest's whole life and the GPU
        // service reaches the main queue while it starts, so it must not be
        // the main thread. It owns a thread outright rather than a place on
        // the cooperative pool it would block for hours.
        let worker = Thread {
            let result = Self.runGuest(
                view: view, diskPath: diskPath, commandLinePath: commandLinePath, bootMode: bootMode,
                width: width, height: height, dotsPerInch: dotsPerInch, memoryMiB: memoryMiB,
                processorCount: processorCount, autoResize: autoResize
            )
            Task { @MainActor in self.finished(result: result) }
        }
        worker.name = "andarium-runtime"
        worker.start()
    }

    private nonisolated static func runGuest(
        view: GuestMetalView, diskPath: String, commandLinePath: String, bootMode: UInt32,
        width: UInt32, height: UInt32, dotsPerInch: UInt32, memoryMiB: UInt32,
        processorCount: UInt32, autoResize: Bool
    ) -> Int32 {
        diskPath.withCString { disk in
            commandLinePath.withCString { commandLine in
                var request = kernarium_runtime_boot()
                request.struct_size = UInt32(MemoryLayout<kernarium_runtime_boot>.size)
                request.width = width
                request.height = height
                request.dots_per_inch = dotsPerInch
                request.guest_memory_mib = memoryMiB
                request.guest_processor_count = processorCount
                request.android_boot_mode = bootMode
                request.auto_resize_guest = autoResize ? 1 : 0
                request.android_disk_path = disk
                request.android_command_line_path = commandLine
                return withUnsafePointer(to: &request) { kernarium_runtime_loader_run(view, $0) }
            }
        }
    }

    // MARK: Events

    private func reached(_ milestones: [BootMilestone]) {
        guard let model else { return }
        let seconds = Int(Date().timeIntervalSince(bootStartedAt))
        for milestone in milestones where model.bootTracker.reach(milestone, atSeconds: seconds) {
            print("Andarium: boot reached \(milestone) at \(seconds) s")
        }
    }

    private func guestRestarted(_ event: GuestRestartEvent) {
        print("Andarium: \(event)")
        guard case .began = event, let model else { return }
        switch model.phase {
        case .running, .booting:
            bootStartedAt = Date()
            model.restart()
        default:
            break
        }
    }

    private func finished(result: Int32) {
        stopThermalObservation()
        kernarium_runtime_loader_set_console_output_handler(nil)
        console = nil
        pause.guestDidEnd()
        UIApplication.shared.isIdleTimerDisabled = false
        print(ConsoleReport.runtimeReturned(result))
        let uptime = runStartedAt.map { Int(Date().timeIntervalSince($0)) }
        let loop = result == Int32(KERNARIUM_RUNTIME_RESULT_RESTART_LOOP)
        model?.runEnded(RunEnd(result: result, restartedInALoop: loop), uptimeSeconds: uptime)
    }

    // MARK: Keys

    func press(_ key: GuestKey) {
        kernarium_runtime_loader_send_key(key.rawValue, 1)
        kernarium_runtime_loader_send_key(key.rawValue, 0)
    }

    /// Holds the key down for `seconds`, as Power is for Android's power menu.
    func hold(_ key: GuestKey, seconds: Double = LiveRuntime.powerMenuHold) {
        kernarium_runtime_loader_send_key(key.rawValue, 1)
        Task {
            try? await Task.sleep(for: .seconds(seconds))
            kernarium_runtime_loader_send_key(key.rawValue, 0)
        }
    }

    // MARK: Microphone

    private static func state(_ access: MicrophoneAccess) -> MicrophoneState {
        switch access {
        case .notAsked: .notAsked
        case .denied: .denied
        case .granted: .granted
        }
    }

    func setMicrophone(_ on: Bool) {
        model?.microphoneState = Self.state(microphone.access)
        if on, microphone.access == .notAsked {
            microphone.request { answer in
                Task { @MainActor in
                    self.model?.microphoneState = Self.state(answer)
                    if self.model?.guest?.microphone == true { self.tellMicrophone(true) }
                }
            }
        }
        tellMicrophone(on)
    }

    /// The runtime is told the setting and never asks the system itself; a
    /// guest records silence while access is out of reach.
    private func tellMicrophone(_ on: Bool) {
        _ = kernarium_runtime_loader_set_microphone(on ? 1 : 0)
    }

    // MARK: Thermal state

    private func reportThermalState(_ phase: ConsoleReport.ThermalPhase) {
        print(ConsoleReport.thermal(
            phase: phase, state: ProcessInfo.processInfo.thermalState,
            uptime: ProcessInfo.processInfo.systemUptime
        ))
    }

    private func startThermalObservation() {
        reportThermalState(.start)
        thermalObserver = NotificationCenter.default.addObserver(
            forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.reportThermalState(.change) }
        }
    }

    private func stopThermalObservation() {
        reportThermalState(.finish)
        if let thermalObserver {
            NotificationCenter.default.removeObserver(thermalObserver)
            self.thermalObserver = nil
        }
    }

    // MARK: Lifecycle

    private func observeLifecycle() {
        let edges: [(Notification.Name, @MainActor (LiveRuntime) -> Void)] = [
            (UIApplication.willResignActiveNotification, {
                print(ConsoleReport.application(active: false))
                $0.pause.willResignActive()
            }),
            (UIApplication.didEnterBackgroundNotification, { $0.pause.didEnterBackground() }),
            (UIApplication.willEnterForegroundNotification, { $0.pause.willEnterForeground() }),
            (UIApplication.willTerminateNotification, { $0.pause.willTerminate() }),
            (UIApplication.didBecomeActiveNotification, { $0.didBecomeActive() }),
        ]
        observers = edges.map { name, edge in
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated {
                    if let self { edge(self) }
                }
            }
        }
    }

    /// The person may have changed the permissions or the storage while
    /// Andarium was away.
    private func didBecomeActive() {
        print(ConsoleReport.application(active: true))
        guard let model else { return }
        model.host.freeStorageBytes = AppEntry.freeStorageBytes()
        model.microphoneState = Self.state(microphone.access)
        if model.guest?.microphone == true { tellMicrophone(true) }
    }

    private func observeDisplayOptions() {
        guard let model else { return }
        withObservationTracking {
            _ = (model.retina, model.followWindow)
        } onChange: {
            Task { @MainActor in
                guard let model = self.model else { return }
                DisplayOptionsStore.save(DisplayOptions(retina: model.retina, followWindow: model.followWindow))
                self.observeDisplayOptions()
            }
        }
    }
}

/// Pausing the guest on the real runtime. The runtime calls run on one serial
/// queue, in the order asked, so a quick return to the app resumes behind a
/// quiesce still in flight.
@MainActor
final class LivePauseEffects: PauseEffects {
    private let queue = DispatchQueue(label: "com.imxieyi.andarium.guest-sync", qos: .userInitiated)

    func holdGuestGPU() {
        let result = kernarium_runtime_loader_set_gpu_hold(1)
        print("Andarium: guest GPU requests held on background (\(result))")
    }

    func releaseGuestGPU() {
        let result = kernarium_runtime_loader_set_gpu_hold(0)
        print("Andarium: guest GPU requests released (\(result))")
    }

    func flushGuest(until deadline: DispatchTime?) {
        settle(reason: "inactive", quiesce: false, until: deadline)
    }

    func quiesceGuest(reason: String, until deadline: DispatchTime?) {
        settle(reason: reason, quiesce: true, until: deadline)
    }

    func resumeGuest(finished: @escaping @MainActor () -> Void) {
        queue.async {
            kernarium_runtime_loader_resume()
            Task { @MainActor in finished() }
        }
    }

    /// Writes the guest's storage back, and suspends the guest when asked,
    /// under an assertion that keeps the process scheduled until it is done.
    private func settle(reason: String, quiesce: Bool, until deadline: DispatchTime?) {
        let assertion = PauseTaskAssertion(named: "guest-sync")
        let finished = DispatchSemaphore(value: 0)
        queue.async {
            let unflushed = quiesce ? kernarium_runtime_loader_quiesce() : kernarium_runtime_loader_sync()
            finished.signal()
            Task { @MainActor in
                if unflushed > 0 {
                    print("Andarium: \(reason) sync left \(unflushed) guest file(s) unflushed")
                } else if unflushed == 0 {
                    print("Andarium: guest storage settled on \(reason)")
                }
                assertion.end()
            }
        }
        if let deadline {
            _ = finished.wait(timeout: deadline)
        }
    }
}

/// A background-task assertion that keeps the process scheduled until it ends:
/// the caller ends it, or iOS expires it. Ending it twice is harmless.
@MainActor
final class PauseTaskAssertion {
    private var identifier = UIBackgroundTaskIdentifier.invalid

    init(named name: String) {
        identifier = UIApplication.shared.beginBackgroundTask(withName: name) { [weak self] in
            MainActor.assumeIsolated { self?.end() }
        }
    }

    func end() {
        guard identifier != .invalid else { return }
        UIApplication.shared.endBackgroundTask(identifier)
        identifier = .invalid
    }
}
