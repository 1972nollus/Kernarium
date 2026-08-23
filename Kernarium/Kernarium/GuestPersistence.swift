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

    func startObserving() {
        guard observers.isEmpty else { return }
        let center = NotificationCenter.default
        let edges: [(Notification.Name, String, Bool)] = [
            (UIApplication.willResignActiveNotification, "inactive", false),
            (UIApplication.didEnterBackgroundNotification, "background", true),
            (UIApplication.willTerminateNotification, "terminating", true),
        ]
        for (name, reason, waits) in edges {
            observers.append(
                center.addObserver(
                    forName: name,
                    object: nil,
                    queue: .main
                ) { _ in
                    MainActor.assumeIsolated {
                        GuestPersistence.shared.flush(
                            reason: reason,
                            waitingForCompletion: waits
                        )
                    }
                }
            )
        }
    }

    func stopObserving() {
        for observer in observers {
            NotificationCenter.default.removeObserver(observer)
        }
        observers.removeAll()
    }

    /// - Parameter waitingForCompletion: whether to hold the main thread until
    ///   the flush finishes. Backgrounding and termination do, because the
    ///   process may not be scheduled again; merely losing focus does not,
    ///   since the app keeps running and the flush would only stall the UI.
    func flush(reason: String, waitingForCompletion: Bool) {
        let application = UIApplication.shared
        var assertion = UIBackgroundTaskIdentifier.invalid
        assertion = application.beginBackgroundTask(withName: "guest-sync") {
            application.endBackgroundTask(assertion)
            assertion = .invalid
        }
        let finished = DispatchSemaphore(value: 0)
        queue.async {
            let unflushed = kernarium_runtime_loader_sync()
            finished.signal()
            Task { @MainActor in
                GuestPersistence.report(reason: reason, unflushed: unflushed)
                if assertion != .invalid {
                    application.endBackgroundTask(assertion)
                    assertion = .invalid
                }
            }
        }
        if waitingForCompletion {
            _ = finished.wait(timeout: .now() + Self.terminationBudget)
        }
    }

    private static func report(reason: String, unflushed: Int32) {
        if unflushed > 0 {
            print(
                "Kernarium: \(reason) sync left \(unflushed) guest " +
                "file(s) unflushed"
            )
        } else if unflushed == 0 {
            print("Kernarium: guest storage synced on \(reason)")
        }
    }
}
