import AndariumCore
import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// Settings while the guest runs, opened from the side keys. It slides up over
/// the guest, which keeps running behind it: install an app from Files,
/// connect a computer with adb, make a bug report, watch the stats, and
/// change the options. An operation on the guest dismisses Settings and
/// shows the full-screen progress.
struct SessionSettingsSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var importing = false
    @State private var copied = false
    @State private var detent: PresentationDetent

    init(large: Bool) {
        _detent = State(initialValue: large ? .large : .medium)
    }

    var body: some View {
        let session = model.session
        NavigationStack {
            List {
                if model.capabilities.apkInstall { apps(session) }
                if model.capabilities.adbOverNetwork { adb(session) }
                if model.capabilities.statistics { stats(session) }
                options
            }
            .listSectionSpacing(.compact)
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done", systemImage: "checkmark") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium, .large], selection: $detent)
        .presentationBackground(Color(.systemGroupedBackground))
        .presentationBackgroundInteraction(.enabled(upThrough: .medium))
        .fileImporter(isPresented: $importing, allowedContentTypes: [.item]) { result in
            guard case .success(let url) = result else { return }
            let scoped = url.startAccessingSecurityScopedResource()
            let bytes = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
            if scoped { url.stopAccessingSecurityScopedResource() }
            model.session.install(fileName: url.lastPathComponent, sizeMB: Double(bytes) / 1_000_000)
        }
    }

    // MARK: Apps

    private func apps(_ session: SessionState) -> some View {
        Section("Apps") {
            Button {
                importing = true
            } label: {
                HStack(spacing: 12) {
                    IconTile(symbol: "square.and.arrow.down.fill")
                    Text("Install APK\u{2026}")
                        .foregroundStyle(Color.accentColor)
                    Spacer()
                    Text("From Files")
                        .font(.subheadline)
                        .foregroundStyle(Color(.secondaryLabel))
                }
            }
            ForEach(session.installs) { item in
                InstallRow(item: item)
                    .swipeActions {
                        Button("Remove", systemImage: "xmark", role: .destructive) {
                            withAnimation { session.remove(item.id) }
                        }
                    }
            }
        }
    }

    // MARK: ADB

    private func adb(_ session: SessionState) -> some View {
        @Bindable var session = session
        let command = "adb connect \(SessionState.adbAddress)"
        return Section {
            Toggle(isOn: $session.adb.animation(.snappy)) {
                HStack(spacing: 12) {
                    IconTile(symbol: "terminal.fill", color: session.adb ? .accentColor : .gray)
                    Text("ADB over Network")
                }
            }
            if session.adb {
                VStack(alignment: .leading, spacing: 8) {
                    Text("On a computer on the same network, run:")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                    HStack(spacing: 10) {
                        Text(command)
                            .font(.system(.callout, design: .monospaced).weight(.medium))
                            .lineLimit(1)
                            .minimumScaleFactor(0.8)
                            .textSelection(.enabled)
                        Spacer(minLength: 0)
                        Button {
                            UIPasteboard.general.string = command
                            withAnimation(.snappy) { copied = true }
                            Task {
                                try? await Task.sleep(for: .seconds(1.6))
                                withAnimation(.snappy) { copied = false }
                            }
                        } label: {
                            Image(systemName: copied ? "checkmark" : "doc.on.doc")
                                .font(.system(size: 15, weight: .semibold))
                                .contentTransition(.symbolEffect(.replace))
                                .frame(width: 36, height: 36)
                        }
                        .buttonStyle(.borderless)
                        .accessibilityLabel(copied ? "Copied" : "Copy command")
                    }
                    .padding(.leading, 14)
                    .padding(.trailing, 4)
                    .frame(minHeight: 46)
                    .background(Color(.tertiarySystemFill), in: .rect(cornerRadius: 12, style: .continuous))
                }
                .padding(.vertical, 4)
            }
        } header: {
            Text("ADB")
        } footer: {
            if session.adb {
                Text("Android asks to allow the computer the first time it connects. ADB turns off when Android stops.")
            } else {
                Text("Lets a computer on the same network connect to Android with adb.")
            }
        }
    }

    // MARK: Stats

    private func stats(_ session: SessionState) -> some View {
        @Bindable var session = session
        let memoryMiB = model.guest?.memoryMiB ?? Limits.defaultMemoryMiB
        return Section("Stats") {
            LabeledContent {
                HStack(spacing: 10) {
                    Sparkline(values: session.readout.history)
                        .frame(width: 44, height: 18)
                    value("\(session.readout.cpu)%")
                        .frame(minWidth: 44, alignment: .trailing)
                }
            } label: {
                rowLabel("CPU", symbol: "cpu")
            }
            LabeledContent {
                value(String(format: "%.1f of %@", Double(session.readout.memoryMiB) / 1024, Formats.memory(memoryMiB)))
            } label: {
                rowLabel("Memory", symbol: "memorychip")
            }
            LabeledContent {
                value("\(session.readout.fps) fps")
            } label: {
                rowLabel("Frame Rate", symbol: "speedometer")
            }
            Toggle(isOn: $session.showStats.animation(.snappy)) {
                rowLabel("Show on Screen", symbol: "rectangle.topthird.inset.filled")
            }
        }
    }

    // MARK: Options

    @ViewBuilder
    private var options: some View {
        @Bindable var model = model
        if let guest = model.guest {
            Section {
                if model.capabilities.liveDisplayOptions {
                    LabeledContent {
                        Picker("Resolution", selection: $model.retina) {
                            Text("Points").tag(false)
                            Text("Retina").tag(true)
                        }
                        .pickerStyle(.segmented)
                        .frame(width: 150)
                    } label: {
                        rowLabel("Resolution", symbol: "rectangle.inset.filled")
                    }
                    Toggle(isOn: $model.followWindow) {
                        rowLabel("Follow Window Size", symbol: "arrow.up.left.and.arrow.down.right")
                    }
                }
                Toggle(isOn: Binding(
                    get: { guest.microphone },
                    set: { model.setMicrophone($0) }
                )) {
                    rowLabel("Microphone", symbol: "mic.fill")
                }
            } header: {
                Text("Options")
            } footer: {
                if guest.microphone && model.microphoneState == .denied {
                    Text("iOS has not allowed Andarium to use the microphone, so Android records silence. Allow it for Andarium in Settings.")
                }
            }
        }
    }

    private func rowLabel(_ title: String, symbol: String) -> some View {
        HStack(spacing: 12) {
            IconTile(symbol: symbol)
            Text(title)
                .lineLimit(1)
                .minimumScaleFactor(0.85)
        }
    }

    private func value(_ text: String) -> some View {
        Text(text)
            .font(.system(.body, design: .rounded).weight(.medium))
            .monospacedDigit()
            .foregroundStyle(.secondary)
            .contentTransition(.numericText())
    }
}

/// One app on its way into the guest, or the result.
struct InstallRow: View {
    let item: ApkInstall

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            tile
            VStack(alignment: .leading, spacing: 4) {
                Text(title)
                    .font(.body.weight(.medium))
                    .lineLimit(1)
                    .truncationMode(.middle)
                detail
                    .font(.footnote)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
            if item.state == .installed {
                Image(systemName: "checkmark.circle.fill")
                    .font(.title3)
                    .foregroundStyle(Color.accentColor)
                    .accessibilityLabel("Installed")
            }
        }
        .padding(.vertical, 4)
    }

    private var title: String {
        item.state == .installed ? item.appName : item.fileName
    }

    @ViewBuilder
    private var tile: some View {
        switch item.state {
        case .installing, .installed:
            // The app's own icon is not known until the guest has it; its
            // initial stands in.
            Text(String(item.appName.prefix(1)))
                .font(.system(size: 19, weight: .semibold, design: .rounded))
                .foregroundStyle(.white)
                .frame(width: 40, height: 40)
                .background(Color.accentColor.gradient, in: .rect(cornerRadius: 11, style: .continuous))
                .opacity(item.state == .installed ? 1 : 0.55)
        case .notApk:
            Image(systemName: "doc.fill")
                .font(.system(size: 18, weight: .semibold))
                .foregroundStyle(.white)
                .frame(width: 40, height: 40)
                .background(Color.gray.gradient, in: .rect(cornerRadius: 11, style: .continuous))
        case .failed:
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(.white)
                .frame(width: 40, height: 40)
                .background(Color.red.gradient, in: .rect(cornerRadius: 11, style: .continuous))
        }
    }

    @ViewBuilder
    private var detail: some View {
        switch item.state {
        case .installing(let progress):
            VStack(alignment: .leading, spacing: 5) {
                ProgressBar(value: max(0.03, progress), height: 5)
                Text("Installing \u{00B7} \(Int((progress * 100).rounded()))% of \(String(format: "%.1f", item.sizeMB)) MB")
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
            }
        case .installed:
            Text(item.version.map { "Installed \u{00B7} version \($0)" } ?? "Installed")
                .foregroundStyle(.secondary)
        case .notApk:
            Text("Not an APK. Choose a file that ends in .apk.")
                .foregroundStyle(.red)
        case .failed(let reason):
            Text(reason)
                .foregroundStyle(.red)
        }
    }
}
