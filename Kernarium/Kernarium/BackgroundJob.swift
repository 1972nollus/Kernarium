import BackgroundTasks
import Combine
import Foundation

/// Where a kept-running job stands against its time limit. The system wants
/// progress from a continued task, and the job's only measurable progress is
/// the time it has used.
struct BackgroundJobTimeline: Equatable {
    let started: Date
    let limit: TimeInterval

    var end: Date { started.addingTimeInterval(limit) }

    func elapsed(at date: Date) -> TimeInterval {
        min(max(date.timeIntervalSince(started), 0), limit)
    }

    func remaining(at date: Date) -> TimeInterval {
        limit - elapsed(at: date)
    }

    func isOver(at date: Date) -> Bool { remaining(at: date) <= 0 }

    /// Whole seconds used, held below the total until the job is over so the
    /// system never sees it finished before it is.
    func completedUnits(at date: Date) -> Int64 {
        let seconds = Int64(elapsed(at: date))
        return isOver(at: date) ? Int64(limit) : min(seconds, Int64(limit) - 1)
    }

    var totalUnits: Int64 { Int64(limit) }

    /// What the system's progress card says about the time left.
    func subtitle(at date: Date) -> String {
        let minutes = Int((remaining(at: date) / 60).rounded(.up))
        switch minutes {
        case ...0: return "Finishing"
        case 1: return "1 minute left"
        default: return "\(minutes) minutes left"
        }
    }
}

/// The job a user starts to keep their guest working while Kernarium is in the
/// background: one iOS continued processing task, submitted on the user's tap,
/// for a time they chose. It ends when the time is up, when the user stops it
/// or iOS does, and then reports to the policy that decides what the guest
/// does next. It asks for no GPU: iOS offers none to this task on every
/// device, and the guest's GPU requests wait meanwhile.
@MainActor
final class BackgroundJob: ObservableObject {
    static let shared = BackgroundJob()

    enum State: Equatable {
        case idle
        /// Submitted; iOS has not yet started it.
        case submitted
        case running(BackgroundJobTimeline)
    }

    static let identifier = "com.imxieyi.Kernarium.guest.job"

    @Published private(set) var state = State.idle
    weak var controller: BackgroundController?

    private var registered = false
    private var task: BGContinuedProcessingTask?
    private var ticker: Timer?
    private var lastSubtitleMinutes = Int.max
    private var pendingLimitMinutes = BackgroundSettings.defaultLimitMinutes

    var isRunning: Bool {
        if case .running = state { return true }
        return false
    }

    var timeline: BackgroundJobTimeline? {
        if case let .running(timeline) = state { return timeline }
        return nil
    }

    /// Starts a job on the user's action. A guest needs no job while it is in
    /// front, so this only submits the request; the job runs from the moment
    /// iOS starts it.
    func start(limitMinutes: Int, guestName: String) {
        guard state == .idle else { return }
        pendingLimitMinutes = limitMinutes
        registerHandler()
        let request = BGContinuedProcessingTaskRequest(
            identifier: Self.identifier,
            title: "\(guestName) is running",
            subtitle: "Stops after " +
                BackgroundSettings.limitTitle(minutes: limitMinutes)
        )
        request.strategy = .fail
        do {
            try BGTaskScheduler.shared.submit(request)
            state = .submitted
            print("Kernarium: background job submitted (\(limitMinutes) min)")
        } catch {
            print("Kernarium: background job refused: \(error)")
        }
    }

    /// The guest's settings may have changed: a job belongs to the keep-running
    /// mode, so leaving the mode ends it.
    func reconcile(mode: BackgroundMode) {
        if mode != .keepRunning { cancel() }
    }

    /// The user stopped the job, or the guest or its mode went away.
    func cancel() {
        guard state != .idle else { return }
        if state == .submitted {
            BGTaskScheduler.shared.cancel(taskRequestWithIdentifier: Self.identifier)
        }
        finish(success: false, reason: "stopped")
    }

    private func registerHandler() {
        guard !registered else { return }
        registered = BGTaskScheduler.shared.register(
            forTaskWithIdentifier: Self.identifier,
            using: nil
        ) { started in
            guard let continued = started as? BGContinuedProcessingTask else {
                started.setTaskCompleted(success: false)
                return
            }
            // The handler runs on a system queue; the job lives on the main
            // actor. The task is a reference the system gives for this use.
            nonisolated(unsafe) let handed = continued
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    BackgroundJob.shared.begin(task: handed)
                }
            }
        }
        print("Kernarium: background job handler registered=\(registered)")
    }

    private func begin(task: BGContinuedProcessingTask) {
        guard state == .submitted else {
            // The user stopped it, or the guest ended, before iOS started it.
            task.setTaskCompleted(success: false)
            return
        }
        let limitMinutes = pendingLimitMinutes
        let timeline = BackgroundJobTimeline(
            started: Date(),
            limit: TimeInterval(limitMinutes * 60)
        )
        self.task = task
        task.progress.totalUnitCount = timeline.totalUnits
        task.progress.completedUnitCount = 0
        task.expirationHandler = { [weak self] in
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    self?.finish(success: false, reason: "expired")
                }
            }
        }
        lastSubtitleMinutes = Int.max
        state = .running(timeline)
        ticker = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { _ in
            MainActor.assumeIsolated { BackgroundJob.shared.tick() }
        }
        print("Kernarium: background job running for \(limitMinutes) min")
    }

    private func tick() {
        guard case let .running(timeline) = state else { return }
        let now = Date()
        if timeline.isOver(at: now) {
            finish(success: true, reason: "time is up")
            return
        }
        task?.progress.completedUnitCount = timeline.completedUnits(at: now)
        let minutes = Int((timeline.remaining(at: now) / 60).rounded(.up))
        if minutes != lastSubtitleMinutes {
            lastSubtitleMinutes = minutes
            task?.updateTitle(
                task?.title ?? "",
                subtitle: timeline.subtitle(at: now)
            )
        }
    }

    private func finish(success: Bool, reason: String) {
        ticker?.invalidate()
        ticker = nil
        let finished = task
        task = nil
        state = .idle
        finished?.setTaskCompleted(success: success)
        print("Kernarium: background job ended (\(reason))")
        controller?.jobDidEnd()
    }
}
