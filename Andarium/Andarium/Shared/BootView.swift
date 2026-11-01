import AndariumCore
import SwiftUI

/// The boot, from the built-in system to Android's first frame: a status
/// line with a live dot, what is starting and the elapsed time; the title
/// with overall progress; and the stages in one glass panel. First start,
/// update, recovery and restart are variants of the same screen. A launch
/// has one run, so there is no Cancel.
struct BootView: View {
    @Environment(AppModel.self) private var model
    let kind: BootKind

    @State private var stage = 0
    @State private var startedAt = Date()

    struct Step {
        let title: String
        let detail: String
        let seconds: Double
    }

    var body: some View {
        let guest = model.guest ?? Guest.fresh(on: model.host)
        let steps = shownSteps(guest: guest)
        let frozen = model.frozenBootStage.map { min($0, steps.count - 1) }
        let current = frozen ?? stage
        let progress = min(1, (Double(current) + (frozen != nil ? 0.45 : 0.5)) / Double(steps.count))
        VStack(spacing: 0) {
            statusLine(frozen: frozen != nil)
            ScrollView {
                content(guest: guest, system: model.system, steps: steps, current: current, progress: progress)
                    .containerRelativeFrame(.vertical, alignment: .center) { length, _ in length }
            }
            .scrollBounceBehavior(.basedOnSize)
        }
        .background(Color(.systemGroupedBackground))
        .statusBarHidden(true)
        .task(id: kind) {
            guard frozen == nil else { return }
            if model.driver != nil {
                await followRuntime(stepCount: steps.count)
                return
            }
            for (index, step) in steps.enumerated() {
                withAnimation(.snappy) { stage = index }
                try? await Task.sleep(for: .seconds(step.seconds))
                if Task.isCancelled { return }
            }
            withAnimation(.snappy) { stage = steps.count }
            try? await Task.sleep(for: .seconds(0.4))
            model.finishBoot(kind)
        }
    }

    /// The steps, with the first saying what the disk write is doing while
    /// one is under way.
    private func shownSteps(guest: Guest) -> [Step] {
        var steps = Self.steps(kind, guest: guest, system: model.system)
        if model.preparingDisk {
            let detail = model.diskProgress.map { "Preparing the system \u{00B7} \(Int(($0 * 100).rounded()))%" }
                ?? "Preparing the system"
            steps[0] = Step(title: steps[0].title, detail: detail, seconds: 0)
        }
        return steps
    }

    /// The device application: the boot is as far as the guest's console
    /// says, and ends when the guest has something to show.
    private func followRuntime(stepCount: Int) async {
        while !Task.isCancelled && !model.bootTracker.isReady {
            let live = min(model.liveBootStage, stepCount - 1)
            if live != stage { withAnimation(.snappy) { stage = live } }
            try? await Task.sleep(for: .milliseconds(200))
        }
        if Task.isCancelled { return }
        withAnimation(.snappy) { stage = stepCount }
        try? await Task.sleep(for: .seconds(0.4))
        if Task.isCancelled { return }
        model.finishBoot(kind)
    }

    // MARK: Parts

    private func statusLine(frozen: Bool) -> some View {
        HStack(spacing: 8) {
            PulsingDot()
            Text(Self.status(kind))
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(Color.accentColor)
            Spacer()
            TimelineView(.periodic(from: startedAt, by: 1)) { timeline in
                let seconds = frozen ? model.frozenElapsed : Int(timeline.date.timeIntervalSince(startedAt))
                Text(Formats.elapsed(seconds))
                    .font(.system(.subheadline, design: .rounded).weight(.medium))
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                    .contentTransition(.numericText())
            }
        }
        .padding(.horizontal, 24)
        .frame(height: 44)
        .frame(maxWidth: 640)
        .frame(maxWidth: .infinity)
    }

    private func content(guest: Guest, system: SystemInfo, steps: [Step], current: Int, progress: Double) -> some View {
        VStack(alignment: .leading, spacing: 18) {
            VStack(alignment: .leading, spacing: 14) {
                HStack(alignment: .firstTextBaseline) {
                    Text(kind == .recovery ? "Recovery" : "Android")
                        .font(.largeTitle.weight(.bold))
                    Spacer()
                    Text("\(Int((progress * 100).rounded()))%")
                        .font(.system(.title3, design: .rounded).weight(.semibold))
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                        .contentTransition(.numericText())
                }
                ProgressBar(value: progress)
            }
            .padding(.horizontal, 4)

            VStack(spacing: 0) {
                ForEach(Array(steps.enumerated()), id: \.offset) { index, step in
                    if index > 0 { Divider().padding(.leading, 54) }
                    stepRow(step, state: index < current ? .done : (index == current ? .active : .pending))
                }
            }
            .padding(.vertical, 4)
            .glassEffect(.regular, in: .rect(cornerRadius: 26))

            if let note = Self.note(kind, guest: guest, system: system) {
                Label {
                    Text(note)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                } icon: {
                    Image(systemName: Self.noteSymbol(kind))
                        .foregroundStyle(Color.accentColor)
                }
                .padding(16)
                .frame(maxWidth: .infinity, alignment: .leading)
                .glassEffect(.regular, in: .rect(cornerRadius: 22))
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 20)
        .frame(maxWidth: 520)
        .frame(maxWidth: .infinity)
    }

    private enum StepState { case done, active, pending }

    private func stepRow(_ step: Step, state: StepState) -> some View {
        HStack(alignment: .top, spacing: 14) {
            ZStack {
                switch state {
                case .done:
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(Color.accentColor)
                case .active:
                    ProgressView()
                        .controlSize(.small)
                case .pending:
                    Image(systemName: "circle")
                        .foregroundStyle(.tertiary)
                }
            }
            .font(.title3)
            .frame(width: 24, height: 24)
            VStack(alignment: .leading, spacing: 2) {
                Text(step.title)
                    .font(.body.weight(.semibold))
                    .foregroundStyle(state == .pending ? AnyShapeStyle(.tertiary) : AnyShapeStyle(.primary))
                Text(step.detail)
                    .font(.subheadline)
                    .monospacedDigit()
                    .foregroundStyle(state == .active ? AnyShapeStyle(Color.accentColor) : (state == .done ? AnyShapeStyle(.secondary) : AnyShapeStyle(.tertiary)))
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }

    // MARK: Content per kind

    static func status(_ kind: BootKind) -> String {
        switch kind {
        case .normal: "Starting"
        case .firstStart: "First Start"
        case .upgrade: "Updating"
        case .recovery: "Starting Recovery"
        case .restart: "Restarting"
        }
    }

    static func noteSymbol(_ kind: BootKind) -> String {
        switch kind {
        case .firstStart: "sparkles"
        case .upgrade: "arrow.triangle.2.circlepath"
        default: "info.circle"
        }
    }

    static func note(_ kind: BootKind, guest: Guest, system: SystemInfo) -> String? {
        switch kind {
        case .firstStart:
            "Android\u{2019}s first-run setup takes a few minutes."
        case .upgrade:
            "Updating from \(guest.systemRelease ?? "the previous release") to \(system.release). Apps and files are kept."
        case .recovery:
            "Recovery appears on Android\u{2019}s screen. Use touch, or the side keys: volume moves, Power chooses."
        case .normal, .restart:
            nil
        }
    }

    static func steps(_ kind: BootKind, guest: Guest, system info: SystemInfo) -> [Step] {
        let system = Step(title: "System", detail: "\(info.distribution) · \(info.release) · read-only", seconds: 0.7)
        let kernel = Step(title: "Linux Kernel", detail: "\(info.kernel.replacingOccurrences(of: "Linux ", with: "")) · \(guest.cores) vCPUs · \(Formats.memory(guest.memoryMiB)) RAM", seconds: 0.9)
        let screen = Step(title: "Screen", detail: "Waiting for Android\u{2019}s first frame", seconds: 0.7)
        let used = "\(Formats.gb(guest.usedGB)) of \(guest.capacityGiB) GB"
        switch kind {
        case .recovery:
            return [system, kernel,
                    Step(title: "Storage", detail: "\(used) · unchanged", seconds: 0.6),
                    Step(title: "Recovery", detail: "Loading \(info.distribution) Recovery", seconds: 1.4),
                    screen]
        case .firstStart:
            return [system, kernel,
                    Step(title: "Storage", detail: "Preparing \(guest.effectiveCapacityGiB) GB for first use", seconds: 0.9),
                    Step(title: "Android", detail: "Setting up for the first time", seconds: 4.0),
                    screen]
        case .upgrade:
            return [system, kernel,
                    Step(title: "Storage", detail: used, seconds: 0.7),
                    Step(title: "Android", detail: "Updating apps for \(info.release)", seconds: 3.6),
                    screen]
        case .normal, .restart:
            let storage = guest.pendingCapacityGiB.map { "Growing from \(guest.capacityGiB) GB to \($0) GB" } ?? used
            return [system, kernel,
                    Step(title: "Storage", detail: storage, seconds: guest.pendingCapacityGiB == nil ? 0.6 : 1.2),
                    Step(title: "Android", detail: kind == .restart ? "Starting again" : "Starting services", seconds: 1.8),
                    screen]
        }
    }
}

/// Overall boot progress: a cyan capsule on a quiet track.
struct ProgressBar: View {
    let value: Double
    var height: CGFloat = 8

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(Color(.systemFill))
                Capsule()
                    .fill(Color.accentColor)
                    .frame(width: max(height, geo.size.width * value))
            }
        }
        .frame(height: height)
        .animation(.snappy, value: value)
        .accessibilityElement()
        .accessibilityLabel("Progress")
        .accessibilityValue("\(Int((value * 100).rounded())) percent")
    }
}

/// The live dot on the status line.
struct PulsingDot: View {
    @State private var dim = false

    var body: some View {
        Circle()
            .fill(Color.accentColor)
            .frame(width: 8, height: 8)
            .opacity(dim ? 0.35 : 1)
            .animation(.easeInOut(duration: 0.9).repeatForever(autoreverses: true), value: dim)
            .onAppear { dim = true }
            .accessibilityHidden(true)
    }
}
