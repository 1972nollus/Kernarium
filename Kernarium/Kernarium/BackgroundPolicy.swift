import Foundation

/// What a guest does when Kernarium leaves the foreground. A guest runs only
/// in the foreground unless its owner chose otherwise: the background modes
/// keep the guest's CPUs busy on the battery, so each is opt-in per guest.
enum BackgroundMode: String, CaseIterable, Identifiable {
    /// Write the guest's storage back and pause it, as every app does.
    case off
    /// Keep the guest running while its display shows in a Picture in Picture
    /// window, which iOS keeps alive for as long as the user leaves it open.
    case pictureInPicture
    /// Keep the guest running for a time the user chose, which they start with
    /// a button: iOS runs the app as a continued task and shows its progress.
    /// The guest's graphics wait meanwhile, because iOS withholds the GPU.
    case keepRunning

    var id: String { rawValue }

    var title: String {
        switch self {
        case .off: "Pause the guest"
        case .pictureInPicture: "Keep running in Picture in Picture"
        case .keepRunning: "Keep running for a set time"
        }
    }

    var explanation: String {
        switch self {
        case .off:
            "The guest is paused while Kernarium is in the background and " +
                "continues when you return."
        case .pictureInPicture:
            "The guest keeps running while its display shows in a small " +
                "floating window. Close the window to pause the guest. This " +
                "uses the battery as the guest does in the foreground."
        case .keepRunning:
            "Tap the keep running button, then leave Kernarium: the guest " +
                "keeps working, with its graphics paused, until the time " +
                "is up. iOS shows the progress and lets you stop it at any " +
                "time."
        }
    }
}

/// The mode each guest chose, stored under the guest's name. A guest that
/// never chose one pauses.
struct BackgroundSettings {
    static let keyPrefix = "background.mode."
    /// Automation's way to run one launch in a mode without changing what the
    /// user chose, as the other `KERNARIUM_` knobs do.
    static let environmentKey = "KERNARIUM_BACKGROUND_MODE"
    /// "0" keeps the vCPUs at full priority while the guest is kept running,
    /// so a measurement can compare the two.
    static let throttleEnvironmentKey = "KERNARIUM_BACKGROUND_THROTTLE"
    static let limitKeyPrefix = "background.limitMinutes."
    /// Automation's way to run one launch with another time limit, in whole
    /// minutes, so a test does not wait out a user's choice.
    static let limitEnvironmentKey = "KERNARIUM_BACKGROUND_LIMIT_MINUTES"
    /// The times offered for a kept-running job, and the one a guest has until
    /// it chooses.
    static let limitChoicesMinutes = [15, 30, 60, 120]
    static let defaultLimitMinutes = 30
    static let limitRangeMinutes = 1...(24 * 60)

    private let defaults: UserDefaults
    private let environment: [String: String]

    init(
        defaults: UserDefaults = .standard,
        environment: [String: String] = ProcessInfo.processInfo.environment
    ) {
        self.defaults = defaults
        self.environment = environment
    }

    func mode(forGuest guest: String) -> BackgroundMode {
        if let forced = environment[Self.environmentKey]
            .flatMap(BackgroundMode.init(rawValue:)) {
            return forced
        }
        return defaults.string(forKey: Self.keyPrefix + guest)
            .flatMap(BackgroundMode.init(rawValue:)) ?? .off
    }

    /// Whether a guest kept running in the background runs at the system's
    /// background priority. It does unless automation says otherwise.
    var throttlesKeptGuest: Bool {
        environment[Self.throttleEnvironmentKey] != "0"
    }

    /// How long a kept-running job lasts for this guest, in minutes.
    func limitMinutes(forGuest guest: String) -> Int {
        if let forced = environment[Self.limitEnvironmentKey].flatMap(Int.init),
           Self.limitRangeMinutes.contains(forced) {
            return forced
        }
        let stored = defaults.integer(forKey: Self.limitKeyPrefix + guest)
        return Self.limitRangeMinutes.contains(stored) ?
            stored : Self.defaultLimitMinutes
    }

    func setLimitMinutes(_ minutes: Int, forGuest guest: String) {
        if minutes == Self.defaultLimitMinutes ||
            !Self.limitRangeMinutes.contains(minutes) {
            defaults.removeObject(forKey: Self.limitKeyPrefix + guest)
        } else {
            defaults.set(minutes, forKey: Self.limitKeyPrefix + guest)
        }
    }

    func setMode(_ mode: BackgroundMode, forGuest guest: String) {
        if mode == .off {
            defaults.removeObject(forKey: Self.keyPrefix + guest)
        } else {
            defaults.set(mode.rawValue, forKey: Self.keyPrefix + guest)
        }
    }
}

/// A scheduled action that can be withdrawn.
@MainActor
protocol BackgroundCancellable: AnyObject {
    func cancel()
}

/// Everything the background policy does to the guest, the display and the
/// system, behind one seam so the policy can be tested without any of them.
@MainActor
protocol BackgroundEffects: AnyObject {
    /// Holds every guest GPU request, so none reaches the host GPU while it
    /// is revoked.
    func holdGuestGPU()
    func releaseGuestGPU()
    /// Writes the guest's storage back and leaves it running. Waits for the
    /// write-back until `deadline`, or not at all when it is nil.
    func flushGuest(until deadline: DispatchTime?)
    /// Writes the guest's storage back and suspends it.
    func quiesceGuest(reason: String, until deadline: DispatchTime?)
    func resumeGuest()
    /// Asks the guest to trim its page cache while it runs unseen.
    func setPageCacheTrimming(_ enabled: Bool)
    /// Runs the guest's vCPUs at the system's background priority while it
    /// runs unseen, so it keeps going on a fraction of the power.
    func setBackgroundThrottle(_ enabled: Bool)

    /// Whether the user started a job that iOS is running, so the app may keep
    /// the guest running in the background without a window.
    var jobActive: Bool { get }

    /// Whether iOS will start the Picture in Picture window as the app leaves.
    var pictureInPictureCanStart: Bool { get }
    var pictureInPictureActive: Bool { get }
    func stopPictureInPicture()

    /// Keeps the process scheduled while the policy waits on the system;
    /// the returned value ends the wait.
    func keepScheduled(named name: String) -> BackgroundCancellable
    func schedule(
        after seconds: TimeInterval,
        _ action: @escaping @MainActor () -> Void
    ) -> BackgroundCancellable
}

/// Decides what happens to the guest at each lifecycle edge. It owns no
/// system facility: it asks its effects, in an order that keeps the GPU safe.
///
/// iOS revokes the host GPU a few seconds after the app enters the
/// background unless something keeps it running, and a guest request that
/// reaches the GPU then makes the guest's Vulkan device lost for good. So the
/// GPU is held at the edge and released only once the guest is safe: quiesced
/// (it issues nothing), or kept alive by a Picture in Picture window that
/// iOS has started.
@MainActor
final class BackgroundController {
    enum Phase: Equatable {
        case foreground
        /// In the background with the GPU held, waiting for the Picture in
        /// Picture window the system starts on its own.
        case awaitingPictureInPicture
        /// In the background, the guest running behind its window.
        case keptRunning
        /// In the background, the guest running for a job the user started,
        /// with the GPU held: iOS gives a continued task no GPU.
        case jobRunning
        /// In the background with the guest paused.
        case quiesced
    }

    /// How long iOS gets to show the window before the guest is paused: it
    /// starts it about a second and a half after the app leaves.
    static let pictureInPictureStartBudget: TimeInterval = 3
    /// What the edge handler's own work may take: the watchdog answers to the
    /// handler as a whole.
    static let edgeBudget = DispatchTimeInterval.seconds(4)

    private(set) var phase = Phase.foreground {
        didSet {
            guard phase != oldValue else { return }
            print("Kernarium: background policy \(oldValue) -> \(phase)")
        }
    }
    private let effects: any BackgroundEffects
    private let mode: () -> BackgroundMode
    private var startDeadline: (any BackgroundCancellable)?
    private var keepScheduled: (any BackgroundCancellable)?
    private var gpuHeld = false
    private var guestQuiesced = false

    init(
        effects: any BackgroundEffects,
        mode: @escaping () -> BackgroundMode
    ) {
        self.effects = effects
        self.mode = mode
    }

    func didEnterBackground() {
        guard phase == .foreground else { return }
        let deadline = DispatchTime.now() + Self.edgeBudget
        holdGPU()
        switch mode() {
        case .pictureInPicture
        where effects.pictureInPictureActive ||
            effects.pictureInPictureCanStart:
            effects.flushGuest(until: deadline)
            if effects.pictureInPictureActive {
                keepGuestRunning()
            } else {
                awaitPictureInPicture()
            }
        case .keepRunning where effects.jobActive:
            effects.flushGuest(until: deadline)
            effects.setPageCacheTrimming(true)
            phase = .jobRunning
        case .off, .pictureInPicture, .keepRunning:
            quiesce(reason: "background", until: deadline)
        }
    }

    /// The job ended: its time ran out, the user stopped it, or iOS
    /// expired it. In the background nothing keeps the process running any
    /// more, so the guest pauses; in the foreground it carries on.
    func jobDidEnd() {
        guard phase == .jobRunning else { return }
        effects.setPageCacheTrimming(false)
        quiesce(
            reason: "job ended",
            until: DispatchTime.now() + Self.edgeBudget
        )
    }

    /// The Picture in Picture window is on screen.
    func pictureInPictureDidStart() {
        guard phase == .awaitingPictureInPicture else { return }
        endWait()
        keepGuestRunning()
    }

    /// iOS would not show the window.
    func pictureInPictureDidFailToStart() {
        guard phase == .awaitingPictureInPicture else { return }
        endWait()
        quiesce(
            reason: "picture in picture did not start",
            until: DispatchTime.now() + Self.edgeBudget
        )
    }

    /// The user closed the window. In the foreground that is the app's own
    /// doing and changes nothing; in the background nothing keeps the process
    /// running any more.
    func pictureInPictureDidStop() {
        switch phase {
        case .awaitingPictureInPicture:
            pictureInPictureDidFailToStart()
        case .keptRunning:
            endKeptRunning()
            holdGPU()
            quiesce(
                reason: "picture in picture closed",
                until: DispatchTime.now() + Self.edgeBudget
            )
        case .foreground, .jobRunning, .quiesced:
            break
        }
    }

    /// The app is on screen again: the window has done its work.
    func didBecomeActive() {
        if effects.pictureInPictureActive {
            effects.stopPictureInPicture()
        }
    }

    func willEnterForeground() {
        endWait()
        if phase == .keptRunning {
            endKeptRunning()
        }
        if phase == .jobRunning {
            effects.setPageCacheTrimming(false)
        }
        if gpuHeld {
            effects.releaseGuestGPU()
            gpuHeld = false
        }
        if guestQuiesced {
            effects.resumeGuest()
            guestQuiesced = false
        }
        phase = .foreground
    }

    private func holdGPU() {
        effects.holdGuestGPU()
        gpuHeld = true
    }

    private func awaitPictureInPicture() {
        phase = .awaitingPictureInPicture
        keepScheduled = effects.keepScheduled(named: "picture-in-picture")
        startDeadline = effects.schedule(
            after: Self.pictureInPictureStartBudget
        ) { [weak self] in
            self?.pictureInPictureDidFailToStart()
        }
    }

    /// The window is up, so the host GPU stays available: the guest may use
    /// it again.
    private func keepGuestRunning() {
        effects.releaseGuestGPU()
        gpuHeld = false
        effects.setPageCacheTrimming(true)
        effects.setBackgroundThrottle(true)
        phase = .keptRunning
    }

    /// The guest is no longer to run unseen: it returns to full speed before
    /// anything else is asked of it, so a quiesce is not throttled.
    private func endKeptRunning() {
        effects.setBackgroundThrottle(false)
        effects.setPageCacheTrimming(false)
    }

    private func quiesce(reason: String, until deadline: DispatchTime) {
        effects.quiesceGuest(reason: reason, until: deadline)
        guestQuiesced = true
        phase = .quiesced
    }

    private func endWait() {
        startDeadline?.cancel()
        startDeadline = nil
        keepScheduled?.cancel()
        keepScheduled = nil
    }
}
