import AndariumCore
import SwiftUI

/// The only screen before a run: the guest's processor, memory and storage,
/// its options and Start. Without a guest it offers to set one up.
struct HomeView: View {
    @Environment(AppModel.self) private var model
    let hints: ScreenHints

    var body: some View {
        @Bindable var model = model
        NavigationStack {
            GeometryReader { geo in
                let wide = geo.size.width >= 700
                Group {
                    if model.guest != nil {
                        ScrollViewReader { proxy in
                            ScrollView {
                                if wide && geo.size.width > geo.size.height {
                                    wideLayout
                                } else if wide {
                                    tallLayout
                                } else {
                                    narrowLayout
                                }
                            }
                            .onAppear {
                                if let target = hints.scrollTarget {
                                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                                        proxy.scrollTo(target, anchor: .bottom)
                                    }
                                }
                            }
                        }
                        .safeAreaBar(edge: .bottom) { startBar(wide: wide) }
                    } else {
                        WelcomeView()
                    }
                }
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("Andarium")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        model.sheet = .info
                    } label: {
                        Label("Info", systemImage: "info")
                    }
                }
            }
        }
        .sheet(item: $model.sheet) { kind in
            switch kind {
            case .create: NewGuestSheet()
            case .resize: ResizeSheet()
            case .erase:
                EraseSheet(confirmed: hints.eraseConfirmed)
                    .presentationSizing(EraseSheetSizing())
            case .info: InfoSheet(acknowledgements: hints.acknowledgements)
            }
        }
        .confirmationDialog(
            "Android's Update Didn't Finish", isPresented: $model.updateChoice, titleVisibility: .visible
        ) {
            UpdateChoiceButtons()
        } message: {
            Text("The start that updates Android did not reach its end. Choose how to go on; Erase Guest starts over instead.")
        }
        .alert("Not Enough Free Space", isPresented: $model.startBlocked) {
            Button("OK", role: .cancel) {}
        } message: {
            Text("Starting needs \(Formats.hostGB(bytes: Limits.startReserveBytes)) free for Android's own writes. This \(model.host.name) has \(Formats.gb(model.host.freeStorageGB)) free.")
        }
    }

    // MARK: Layouts

    private var narrowLayout: some View {
        VStack(spacing: 22) {
            notices
            panel(large: false)
            OptionsPanel()
            EraseRow()
                .id("erase")
        }
        .padding(.horizontal, 16)
        .padding(.top, 4)
        .padding(.bottom, 20)
    }

    /// iPad in portrait or a tall window: the controls across the top, the
    /// options and Erase beneath.
    private var tallLayout: some View {
        VStack(spacing: 28) {
            notices
            panel(large: true)
            OptionsPanel()
            EraseRow()
                .id("erase")
        }
        .frame(maxWidth: 680)
        .padding(.horizontal, 28)
        .padding(.top, 12)
        .padding(.bottom, 20)
        .frame(maxWidth: .infinity)
    }

    private var wideLayout: some View {
        HStack(alignment: .top, spacing: 24) {
            VStack(spacing: 22) {
                notices
                panel(large: true)
            }
            .frame(maxWidth: 520)
            VStack(spacing: 22) {
                OptionsPanel()
                EraseRow()
                    .id("erase")
            }
            .frame(maxWidth: 400)
        }
        .padding(.horizontal, 28)
        .padding(.top, 8)
        .padding(.bottom, 20)
        .frame(maxWidth: .infinity)
    }

    @ViewBuilder
    private func panel(large: Bool) -> some View {
        @Bindable var model = model
        if model.guest != nil {
            ControlsPanel(
                guest: Binding(get: { model.guest! }, set: { model.guest = $0 }),
                host: model.host,
                resize: { model.sheet = .resize }
            )
        }
    }

    @ViewBuilder
    private var notices: some View {
        @Bindable var model = model
        if model.lowSpace {
            NoticeCard(
                symbol: "externaldrive.badge.exclamationmark", color: .orange,
                title: "\(Formats.gb(model.host.freeStorageGB)) free on this \(model.host.name)",
                text: "Android needs \(Formats.hostGB(bytes: Limits.startReserveBytes)) free to start. Free up space in Settings › General › \(model.host.name) Storage."
            )
        }
        switch model.plan {
        case .update(_, let release):
            NoticeCard(
                symbol: "arrow.down.circle.fill", color: .accentColor,
                title: "Android updated to \(release)",
                text: "The next start finishes the update and takes a little longer. Your data is saved first, so a start that does not finish can be undone."
            )
        case .unfinishedUpdate(let release, let canRetry, let canRestore):
            UnfinishedUpdateCard(release: release, canRetry: canRetry, canRestore: canRestore)
        case .blocked(let reason):
            NoticeCard(
                symbol: "exclamationmark.triangle.fill", color: .orange,
                title: reason.title, text: reason.explanation
            )
        case .firstStart, .normal:
            EmptyView()
        }
        if let notice = model.notice {
            switch notice {
            case .erased:
                NoticeCard(
                    symbol: "sparkles", color: .accentColor,
                    title: "Guest erased",
                    text: "The next start runs Android's setup.",
                    onClose: { withAnimation { model.notice = nil } }
                )
            case .closedInBackground:
                NoticeCard(
                    symbol: "moon.zzz.fill", color: .secondary,
                    title: "iOS closed Andarium",
                    text: "The guest was paused in the background. Start it again when you're ready.",
                    onClose: { withAnimation { model.notice = nil } }
                )
            }
        }
    }

    // MARK: Start

    /// One split capsule: Start starts normally, and its arrow opens the two
    /// ways to start.
    private func startBar(wide: Bool) -> some View {
        StartButton()
            .frame(maxWidth: wide ? 520 : .infinity)
            .padding(.horizontal, 16)
            .padding(.top, 14)
            .padding(.bottom, 6)
            .frame(maxWidth: .infinity)
            .background {
                // Content fades out under the button instead of showing through.
                LinearGradient(
                    stops: [
                        .init(color: Color(.systemGroupedBackground).opacity(0), location: 0),
                        .init(color: Color(.systemGroupedBackground).opacity(0.92), location: 0.45),
                        .init(color: Color(.systemGroupedBackground), location: 1),
                    ],
                    startPoint: .top, endPoint: .bottom
                )
                .ignoresSafeArea(edges: .bottom)
                .allowsHitTesting(false)
            }
    }
}

/// Start, split: the main part starts normally; the arrow opens Start and
/// Start in Recovery, each with its one line.
struct StartButton: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        HStack(spacing: 0) {
            Button {
                model.start(.normal)
            } label: {
                Label(title, systemImage: "play.fill")
                    .frame(maxWidth: .infinity)
                    .frame(height: 52)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            Rectangle()
                .fill(.white.opacity(0.4))
                .frame(width: 1, height: 24)
            Button {
                model.startMenu = true
            } label: {
                Image(systemName: "chevron.up")
                    .font(.system(size: 15, weight: .bold))
                    .frame(width: 62, height: 52)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Start Options")
            .popover(isPresented: $model.startMenu, arrowEdge: .bottom) {
                StartMenu { mode in
                    model.startMenu = false
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { model.start(mode) }
                }
                .presentationCompactAdaptation(.popover)
            }
        }
        .font(.body.weight(.semibold))
        .foregroundStyle(.white)
        .background(Capsule().fill(Color.accentColor))
        .glassEffect(.regular.tint(Color.accentColor).interactive(), in: .capsule)
        // A guest that cannot start with this Android starts again only after
        // it is erased.
        .opacity(blocked ? 0.4 : 1)
        .disabled(blocked)
    }

    private var blocked: Bool {
        if case .blocked = model.plan { return true }
        return false
    }

    private var title: String {
        guard model.guest != nil else { return "Start" }
        if case .unfinishedUpdate = model.plan { return "Start" }
        switch model.bootKind(.normal) {
        case .firstStart: return "Start and Set Up"
        case .upgrade: return "Update and Start"
        default: return "Start"
        }
    }
}

/// The two ways to start. Choosing one starts at once: the menu is the
/// only step on the way into Recovery.
struct StartMenu: View {
    @Environment(AppModel.self) private var model
    let start: (StartMode) -> Void

    var body: some View {
        VStack(spacing: 0) {
            row("Start", detail: detail, symbol: "play.fill", tint: .accentColor) { start(.normal) }
            Divider().padding(.leading, 60)
            row("Start in Recovery", detail: "Factory reset, reboot or power off", symbol: "wrench.adjustable.fill", tint: .gray) { start(.recovery) }
        }
        .padding(.vertical, 6)
        .frame(width: 310)
        .foregroundStyle(Color.primary)
        .font(.body)
    }

    private var detail: String {
        guard model.guest != nil else { return "" }
        if case .unfinishedUpdate = model.plan { return "Choose how to finish the update" }
        switch model.bootKind(.normal) {
        case .firstStart: return "First start: Android sets up"
        case .upgrade: return "Android finishes its update"
        default: return "\(model.system.distribution) · Android \(model.system.android)"
        }
    }

    private func row(_ title: String, detail: String, symbol: String, tint: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 14) {
                IconTile(symbol: symbol, color: tint)
                VStack(alignment: .leading, spacing: 1) {
                    Text(title).font(.body.weight(.semibold))
                    Text(detail).font(.footnote).foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 16)
            .frame(height: 62)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

/// The rounded square that marks every row: a white glyph on the accent.
struct IconTile: View {
    let symbol: String
    var color: Color = .accentColor

    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: 14, weight: .semibold))
            .foregroundStyle(.white)
            .frame(width: 30, height: 30)
            .background(color.gradient, in: .rect(cornerRadius: 8, style: .continuous))
    }
}

/// First launch: there is no guest yet.
struct WelcomeView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        VStack(spacing: 0) {
            Spacer(minLength: 20)
            StackMark(lit: false, width: 150)
                .padding(.bottom, 34)
            Text("No Guest Yet")
                .font(.title2.weight(.bold))
            Text("\(model.system.distribution) (Android \(model.system.android)) is built into Andarium. Set up a guest to run it.")
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 340)
                .padding(.top, 8)
            Button {
                model.sheet = .create
            } label: {
                Text("Set Up Guest")
                    .font(.body.weight(.semibold))
                    .frame(width: 220, height: 50)
            }
            .buttonStyle(.glassProminent)
            .padding(.top, 28)
            Spacer(minLength: 20)
            Label("\(Formats.gb(model.host.freeStorageGB)) free on this \(model.host.name)", systemImage: "internaldrive")
                .font(.footnote)
                .foregroundStyle(model.canCreate ? Color.secondary : Color.orange)
                .padding(.bottom, 12)
        }
        .padding(.horizontal, 24)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

/// A dismissible message above the controls.
struct NoticeCard: View {
    let symbol: String
    let color: Color
    let title: String
    let text: String
    var onClose: (() -> Void)?

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: symbol)
                .font(.title3)
                .foregroundStyle(color)
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.subheadline.weight(.semibold))
                Text(text)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
            if let onClose {
                Button(action: onClose) {
                    Image(systemName: "xmark")
                        .font(.footnote.weight(.semibold))
                        .foregroundStyle(.secondary)
                        .frame(width: 28, height: 28)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Dismiss")
            }
        }
        .padding(14)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
    }
}

/// Display and microphone: plain rows in one glass panel.
struct OptionsPanel: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        VStack(alignment: .leading, spacing: 8) {
            Text("Options")
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(.secondary)
                .padding(.leading, 18)
            VStack(spacing: 0) {
                row("Resolution", symbol: "rectangle.inset.filled") {
                    Picker("Resolution", selection: $model.retina) {
                        Text("Points").tag(false)
                        Text("Retina").tag(true)
                    }
                    .pickerStyle(.segmented)
                    .frame(width: 150)
                }
                divider
                row("Follow Window Size", symbol: "arrow.up.left.and.arrow.down.right") {
                    Toggle("Follow Window Size", isOn: $model.followWindow).labelsHidden()
                }
                if let guest = model.guest {
                    divider
                    row("Microphone", symbol: "mic.fill") {
                        Toggle("Microphone", isOn: Binding(
                            get: { guest.microphone },
                            set: { model.setMicrophone($0) }
                        ))
                        .labelsHidden()
                    }
                    divider
                    row("Include in Backups", symbol: "icloud.fill") {
                        Toggle("Include in Backups", isOn: Binding(
                            get: { guest.includeInBackups },
                            set: { model.guest?.includeInBackups = $0 }
                        ))
                        .labelsHidden()
                    }
                }
            }
            .glassEffect(.regular, in: .rect(cornerRadius: 26))
            if model.guest != nil {
                BackupNote(host: model.host)
            }
        }
        .id("options")
    }

    private var divider: some View {
        Divider().padding(.leading, 58)
    }

    private func row<C: View>(_ title: String, symbol: String, @ViewBuilder control: () -> C) -> some View {
        HStack(spacing: 12) {
            IconTile(symbol: symbol)
            Text(title)
                .lineLimit(1)
                .minimumScaleFactor(0.85)
            Spacer(minLength: 8)
            control()
        }
        .padding(.horizontal, 16)
        .frame(minHeight: 54)
    }
}

/// What Include in Backups does to iCloud and computer backups.
struct BackupNote: View {
    let host: HostDevice

    var body: some View {
        Text("iCloud and computer backups of this \(host.name) leave the guest out, because Android can set itself up again and its files take several gigabytes. Turn on Include in Backups to keep them.")
            .font(.footnote)
            .foregroundStyle(.secondary)
            .padding(.horizontal, 18)
    }
}

/// An update that did not reach its end: the ways on, beside the notice.
struct UnfinishedUpdateCard: View {
    @Environment(AppModel.self) private var model
    let release: String
    let canRetry: Bool
    let canRestore: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: "exclamationmark.arrow.triangle.2.circlepath")
                    .font(.title3)
                    .foregroundStyle(Color.orange)
                    .frame(width: 28)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Android's update didn't finish").font(.subheadline.weight(.semibold))
                    Text(text)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            if canRetry || canRestore {
                HStack(spacing: 10) {
                    if canRetry {
                        Button("Try Again") { model.start(.normal, resolution: .retry) }
                    }
                    if canRestore {
                        Button("Restore and Update") { model.start(.normal, resolution: .restoreSnapshot) }
                    }
                }
                .buttonStyle(.glass)
                .controlSize(.regular)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(14)
        .glassEffect(.regular, in: .rect(cornerRadius: 22))
    }

    private var text: String {
        switch (canRetry, canRestore) {
        case (true, true):
            "The start that updates to \(release) did not reach its end. Try it again, or put back your data from before the update and update afresh."
        case (true, false):
            "The start that updates to \(release) did not reach its end. Try it again, or erase the guest to start over."
        case (false, true):
            "This Andarium carries a different Android than the update that did not finish. Put back your data from before it and update afresh, or erase the guest to start over."
        case (false, false):
            "The start that updates to \(release) did not reach its end, and nothing from before it is left to put back. Erase the guest to start over."
        }
    }
}

/// The choices of the dialog Start opens for an update that did not finish.
struct UpdateChoiceButtons: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if case .unfinishedUpdate(_, let canRetry, let canRestore) = model.plan {
            if canRetry {
                Button("Try Again") { model.start(.normal, resolution: .retry) }
            }
            if canRestore {
                Button("Restore and Update") { model.start(.normal, resolution: .restoreSnapshot) }
            }
        }
        Button("Cancel", role: .cancel) {}
    }
}

/// Erase lives apart from everything else, in red.
struct EraseRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Button {
            model.sheet = .erase
        } label: {
            HStack(spacing: 12) {
                IconTile(symbol: "trash.fill", color: .red)
                Text("Erase Guest…")
                    .foregroundStyle(.red)
                Spacer()
                if let guest = model.guest {
                    Text(Formats.gb(guest.usedGB))
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.horizontal, 16)
            .frame(minHeight: 54)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .glassEffect(.regular.interactive(), in: .rect(cornerRadius: 26))
    }
}
