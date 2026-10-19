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

    /// What the guest is asked to do at a lifecycle edge.
    private enum Departure {
        /// Write back, but keep running. Losing focus is not losing the
        /// process, and freezing the guest for it would stall a user who is
        /// about to come straight back.
        case flush
        /// Write back and enter full system suspend. Backgrounding is the
        /// point after which iOS may never schedule this process again, and a
        /// guest left running would dirty what was just written back.
        case quiesce
    }

    func startObserving() {
        guard observers.isEmpty else { return }
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
                    GuestPersistence.shared.enterBackground()
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
                    GuestPersistence.shared.returnToForeground()
                }
            }
        )
    }

    /// The application has entered the background, and iOS revokes the host
    /// GPU within seconds of this handler returning. The guest's GPU requests
    /// are held first, so none reaches Metal in the time the guest needs to
    /// suspend, or later should the suspend miss its budget and iOS freeze the
    /// guest running; a Metal error there would make the guest's Vulkan device
    /// lost for good. Then the guest is quiesced. The handler as a whole
    /// answers to iOS's watchdog, so the hold and the quiesce share the one
    /// budget.
    private func enterBackground() {
        let deadline = DispatchTime.now() + Self.terminationBudget
        BackgroundSession.shared.didEnterBackground()
        let held = kernarium_runtime_loader_set_gpu_hold(1)
        print("Kernarium: guest GPU requests held on background (\(held))")
        flush(reason: "background", departure: .quiesce, waitingUntil: deadline)
    }

    /// Lets the held GPU requests through, then resumes on the flush queue,
    /// behind any quiesce still in flight. A quick return to the app would
    /// otherwise hold the main thread until that quiesce finished, and
    /// resuming first would leave the guest suspended. The guest's rings stay
    /// disabled until the resume, and the requests it queued wait in them
    /// until then.
    private func returnToForeground() {
        BackgroundSession.shared.willEnterForeground()
        let released = kernarium_runtime_loader_set_gpu_hold(0)
        print("Kernarium: guest GPU requests released on foreground (\(released))")
        queue.async {
            kernarium_runtime_loader_resume()
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
