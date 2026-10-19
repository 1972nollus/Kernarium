import Foundation
import UIKit

/// iOS can reclaim a suspended application without running any more of its
/// code, and a guest's writable images are ordinary host files that nothing
/// else will flush. Every lifecycle edge that can precede a reclaim therefore
/// pushes what the guest has already written through to storage while the app
/// is still scheduled.
///
/// This is the host half of durability. Pages still dirty inside the guest are
/// written back by the guest kernel on its own schedule; nothing outside it can
/// force that.
@MainActor
final class GuestPersistence {
    static let shared = GuestPersistence()

    /// Long enough for a flush of a few images to finish, short enough that the
    /// app never looks wedged to the watchdog if storage is busy.
    private static let terminationBudget = DispatchTimeInterval.seconds(4)

    private let queue = DispatchQueue(
        label: "com.imxieyi.Kernarium.guest-sync",
        qos: .userInitiated
    )
    private var observers: [NSObjectProtocol] = []

    private init() {}

    /// What the application does about the background: the policy and the
    /// facilities it acts through. Created by `startObserving`.
    private(set) var background: BackgroundController?

    /// What the guest is asked to do at a lifecycle edge.
    enum Departure {
        /// Write back, but keep running. Losing focus is not losing the
        /// process, and freezing the guest for it would stall a user who is
        /// about to come straight back.
        case flush
        /// Write back and enter full system suspend. Backgrounding is the
        /// point after which iOS may never schedule this process again, and a
        /// guest left running would dirty what was just written back.
        case quiesce
    }

    /// - Parameter guest: the name the running guest's background mode is
    ///   stored under.
    func startObserving(
        guest: @escaping () -> String?,
        pictureInPicture: (any PictureInPictureWindow)? = nil
    ) {
        guard observers.isEmpty else { return }
        let settings = BackgroundSettings()
        let effects = SystemBackgroundEffects(
            persistence: self,
            pictureInPicture: pictureInPicture
        )
        let controller = BackgroundController(effects: effects) {
            guest().map(settings.mode(forGuest:)) ?? .off
        }
        background = controller
        pictureInPicture?.controller = controller
        let center = NotificationCenter.default
        observers.append(
            center.addObserver(
                forName: UIApplication.willResignActiveNotification,
                object: nil,
                queue: .main
            ) { _ in
                MainActor.assumeIsolated {
                    GuestPersistence.shared.flush(
                        reason: "inactive",
                        departure: .flush,
                        waitingUntil: nil
                    )
                }
            }
        )
        observers.append(
            center.addObserver(
                forName: UIApplication.didEnterBackgroundNotification,
                object: nil,
                queue: .main
            ) { _ in
                MainActor.assumeIsolated {
                    BackgroundSession.shared.didEnterBackground()
                    GuestPersistence.shared.background?.didEnterBackground()
                }
            }
        )
        observers.append(
            center.addObserver(
                forName: UIApplication.didBecomeActiveNotification,
                object: nil,
                queue: .main
            ) { _ in
                MainActor.assumeIsolated {
                    GuestPersistence.shared.background?.didBecomeActive()
                }
            }
        )
        observers.append(
            center.addObserver(
                forName: UIApplication.willTerminateNotification,
                object: nil,
                queue: .main
            ) { _ in
                MainActor.assumeIsolated {
                    BackgroundSession.shared.willTerminate()
                    GuestPersistence.shared.flush(
                        reason: "terminating",
                        departure: .quiesce,
                        waitingUntil: .now() + Self.terminationBudget
                    )
                }
            }
        )
        observers.append(
            center.addObserver(
                forName: UIApplication.willEnterForegroundNotification,
                object: nil,
                queue: .main
            ) { _ in
                MainActor.assumeIsolated {
                    BackgroundSession.shared.willEnterForeground()
                    GuestPersistence.shared.background?.willEnterForeground()
                }
            }
        )
    }

    /// Writes the guest's storage back and leaves it running.
    func syncGuest(reason: String, until deadline: DispatchTime?) {
        flush(reason: reason, departure: .flush, waitingUntil: deadline)
    }

    /// Writes the guest's storage back and suspends it.
    func quiesceGuest(reason: String, until deadline: DispatchTime?) {
        flush(reason: reason, departure: .quiesce, waitingUntil: deadline)
    }

    /// Resumes on the flush queue, behind any quiesce still in flight. A quick
    /// return to the app would otherwise hold the main thread until that
    /// quiesce finished, and resuming first would leave the guest suspended.
    func resumeGuest() {
        queue.async {
            kernarium_runtime_loader_resume()
        }
    }

    /// Switches the guest's periodic page-cache trimming through `set` on the
    /// flush queue, behind any write-back or quiesce already asked for. The
    /// switch may wait for the guest's console, and one made while the guest
    /// is quiesced reaches it on resume.
    func setPageCacheTrimming(
        _ enabled: Bool,
        through set: @escaping @Sendable (Int32) -> Int32
    ) {
        queue.async {
            let result = set(enabled ? 1 : 0)
            print(
                "Kernarium: guest page-cache trimming " +
                    "\(enabled ? "on" : "off") (\(result))"
            )
        }
    }

    func stopObserving() {
        for observer in observers {
            NotificationCenter.default.removeObserver(observer)
        }
        observers.removeAll()
    }

    /// - Parameter waitingUntil: when to stop holding the main thread for the
    ///   flush to finish, or nil to return at once. Backgrounding and
    ///   termination wait, because the process may not be scheduled again;
    ///   merely losing focus does not, since the app keeps running and the
    ///   flush would only stall the UI.
    private func flush(
        reason: String,
        departure: Departure,
        waitingUntil deadline: DispatchTime?
    ) {
        let application = UIApplication.shared
        var assertion = UIBackgroundTaskIdentifier.invalid
        assertion = application.beginBackgroundTask(withName: "guest-sync") {
            application.endBackgroundTask(assertion)
            assertion = .invalid
        }
        let finished = DispatchSemaphore(value: 0)
        queue.async {
            let unflushed = switch departure {
            case .flush: kernarium_runtime_loader_sync()
            case .quiesce: kernarium_runtime_loader_quiesce()
            }
            finished.signal()
            Task { @MainActor in
                GuestPersistence.report(reason: reason, unflushed: unflushed)
                if assertion != .invalid {
                    application.endBackgroundTask(assertion)
                    assertion = .invalid
                }
            }
        }
        if let deadline {
            _ = finished.wait(timeout: deadline)
        }
    }

    private static func report(reason: String, unflushed: Int32) {
        if unflushed > 0 {
            print(
                "Kernarium: \(reason) sync left \(unflushed) guest " +
                "file(s) unflushed"
            )
        } else if unflushed == 0 {
            print("Kernarium: guest storage settled on \(reason)")
        }
    }
}

/// The window that keeps the guest alive in the background, as the policy
/// sees it. The window reports its own edges to `controller`.
@MainActor
protocol PictureInPictureWindow: AnyObject {
    var controller: BackgroundController? { get set }
    /// Whether iOS will show the window as the app leaves.
    var canStart: Bool { get }
    var isActive: Bool { get }
    func stop()
}

/// The background policy's effects on the real guest, display and system.
@MainActor
final class SystemBackgroundEffects: BackgroundEffects {
    private final class Token: BackgroundCancellable {
        private var action: (@MainActor () -> Void)?

        init(_ action: @escaping @MainActor () -> Void) {
            self.action = action
        }

        func cancel() {
            action?()
            action = nil
        }
    }

    private let persistence: GuestPersistence
    private let window: (any PictureInPictureWindow)?
    private let pageCacheTrimming: @Sendable (Int32) -> Int32

    /// - Parameter pageCacheTrimming: the runtime's trimming switch; the
    ///   unit tests replace it.
    init(
        persistence: GuestPersistence,
        pictureInPicture: (any PictureInPictureWindow)?,
        pageCacheTrimming: @escaping @Sendable (Int32) -> Int32 = {
            kernarium_runtime_loader_set_page_cache_trimming($0)
        }
    ) {
        self.persistence = persistence
        window = pictureInPicture
        self.pageCacheTrimming = pageCacheTrimming
    }

    func holdGuestGPU() {
        let result = kernarium_runtime_loader_set_gpu_hold(1)
        print("Kernarium: guest GPU requests held on background (\(result))")
    }

    func releaseGuestGPU() {
        let result = kernarium_runtime_loader_set_gpu_hold(0)
        print("Kernarium: guest GPU requests released (\(result))")
    }

    func flushGuest(until deadline: DispatchTime?) {
        persistence.syncGuest(reason: "background", until: deadline)
    }

    func quiesceGuest(reason: String, until deadline: DispatchTime?) {
        persistence.quiesceGuest(reason: reason, until: deadline)
    }

    func resumeGuest() {
        persistence.resumeGuest()
    }

    func setPageCacheTrimming(_ enabled: Bool) {
        persistence.setPageCacheTrimming(enabled, through: pageCacheTrimming)
    }

    func setBackgroundThrottle(_ enabled: Bool) {
        guard enabled == false || BackgroundSettings().throttlesKeptGuest else {
            return
        }
        let changed = kernarium_runtime_loader_set_background_throttle(
            enabled ? 1 : 0
        )
        print(
            "Kernarium: guest vCPUs \(enabled ? "throttled" : "restored") " +
                "(\(changed) thread(s))"
        )
    }

    var pictureInPictureCanStart: Bool { window?.canStart ?? false }
    var pictureInPictureActive: Bool { window?.isActive ?? false }

    func stopPictureInPicture() {
        window?.stop()
    }

    func keepScheduled(named name: String) -> any BackgroundCancellable {
        let application = UIApplication.shared
        var assertion = UIBackgroundTaskIdentifier.invalid
        assertion = application.beginBackgroundTask(withName: name) {
            application.endBackgroundTask(assertion)
            assertion = .invalid
        }
        return Token {
            if assertion != .invalid {
                application.endBackgroundTask(assertion)
                assertion = .invalid
            }
        }
    }

    func schedule(
        after seconds: TimeInterval,
        _ action: @escaping @MainActor () -> Void
    ) -> any BackgroundCancellable {
        let timer = Timer(timeInterval: seconds, repeats: false) { _ in
            MainActor.assumeIsolated { action() }
        }
        RunLoop.main.add(timer, forMode: .common)
        return Token { timer.invalidate() }
    }
}
