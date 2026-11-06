// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// A titled section of Home: rows in one glass panel under a small heading,
/// and a note beneath.
struct HomeSection<Content: View, Footer: View>: View {
    let title: String
    @ViewBuilder var content: Content
    @ViewBuilder var footer: Footer

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(.secondary)
                .padding(.leading, 18)
            VStack(spacing: 0) { content }
                .glassEffect(.regular, in: .rect(cornerRadius: 26))
            footer
        }
    }
}

extension HomeSection where Footer == EmptyView {
    init(title: String, @ViewBuilder content: () -> Content) {
        self.init(title: title, content: content, footer: { EmptyView() })
    }
}

/// A note beneath a section.
struct SectionNote: View {
    let text: String

    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(.footnote)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
            .padding(.horizontal, 18)
    }
}

/// One row of a section: the icon tile, the title (and a line under it) and
/// the control on the right.
struct OptionRow<Control: View>: View {
    let title: String
    let symbol: String
    var detail: String?
    /// The capability is off and unreachable: the tile and the words dim.
    var dimmed = false
    @ViewBuilder var control: Control

    var body: some View {
        HStack(spacing: 12) {
            IconTile(symbol: symbol, color: dimmed ? .gray : .accentColor)
                .opacity(dimmed ? 0.55 : 1)
            VStack(alignment: .leading, spacing: 1) {
                Text(title)
                    .lineLimit(1)
                    .minimumScaleFactor(0.85)
                    .opacity(dimmed ? 0.55 : 1)
                if let detail {
                    Text(detail)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 8)
            control
        }
        .padding(.horizontal, 16)
        .padding(.vertical, detail == nil ? 0 : 6)
        .frame(minHeight: 54)
    }
}

/// The rule between two rows of a section.
struct RowDivider: View {
    var body: some View { Divider().padding(.leading, 58) }
}

// MARK: System

/// The guest's processors and memory as a summary, with Edit opening the sheet
/// that changes them.
struct SystemSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if let guest = model.guest {
            HomeSection(title: "Guest System") {
                HStack(spacing: 12) {
                    stat(symbol: "cpu", value: "\(guest.cores)", unit: guest.cores == 1 ? "vCPU" : "vCPUs")
                    stat(symbol: "memorychip", value: memory(guest).value, unit: "\(memory(guest).unit) RAM")
                    Spacer(minLength: 4)
                    Button {
                        model.sheet = .system
                    } label: {
                        Text("Edit")
                            .font(.subheadline.weight(.semibold))
                            .foregroundStyle(Color.accentColor)
                            .padding(.horizontal, 18)
                            .frame(height: 34)
                            .background(Color.accentColor.opacity(0.13), in: .capsule)
                            .contentShape(.capsule)
                    }
                    .buttonStyle(.plain)
                    .accessibilityIdentifier("home-system-edit")
                    .accessibilityLabel("Edit System")
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 14)
                .accessibilityElement(children: .contain)
            }
        }
    }

    /// "2.75 GiB" as the number and its unit.
    private func memory(_ guest: Guest) -> (value: String, unit: String) {
        let parts = Formats.size(mib: guest.memoryMiB).split(separator: " ", maxSplits: 1).map(String.init)
        return (parts.first ?? "", parts.count > 1 ? parts[1] : "")
    }

    private func stat(symbol: String, value: String, unit: String) -> some View {
        HStack(spacing: 10) {
            IconTile(symbol: symbol)
            VStack(alignment: .leading, spacing: 0) {
                Text(value)
                    .font(.system(size: 24, weight: .semibold, design: .rounded))
                    .monospacedDigit()
                    .lineLimit(1)
                Text(unit)
                    .font(.footnote.weight(.medium))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(value) \(unit)")
    }
}

// MARK: Display

/// Retina mode: the guest draws at the screen's own pixel density instead of
/// at points. It applies when the guest next starts.
struct DisplaySection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        HomeSection(title: "Display") {
            OptionRow(title: "Retina mode", symbol: "rectangle.inset.filled") {
                Toggle("Retina mode", isOn: Binding(
                    get: { model.retina },
                    set: { model.chooseRetina($0) }
                ))
                .labelsHidden()
            }
        }
        .id("display")
    }
}

// MARK: Capabilities

/// What the guest may use of this device: the microphone, which iOS asks
/// about when it is turned on, and the gyroscope.
struct CapabilitiesSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if model.guest != nil {
            HomeSection(title: "Capabilities") {
                MicrophoneRow()
                RowDivider()
                GyroscopeRow()
            } footer: {
                if let reason = model.motionSensorsProblem ?? Self.gyroscopeUnavailable(available: model.motionSensorsAvailable) {
                    SectionNote(reason)
                }
            }
            .id("capabilities")
        }
    }

    /// Why Gyroscope cannot be turned on; nil when it can, which needs no note.
    static func gyroscopeUnavailable(available: Bool) -> String? {
        available ? nil : "This device has no gyroscope or accelerometer to share."
    }
}

/// The words under Microphone while iOS refuses access.
enum CapabilityWords {
    static let microphoneDenied = "Not allowed in Settings"
}

struct MicrophoneRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let denied = model.microphoneState == .denied
        OptionRow(
            title: "Microphone", symbol: "mic.fill",
            detail: denied ? CapabilityWords.microphoneDenied : nil, dimmed: denied
        ) {
            Toggle("Microphone", isOn: Binding(
                get: { model.microphoneShownOn },
                set: { model.setMicrophone($0) }
            ))
            .labelsHidden()
        }
        .accessibilityHint(denied ? "Andarium is not allowed to use the microphone. Turning this on offers to open Settings." : "")
    }
}

struct GyroscopeRow: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let on = model.guest?.motionSensors == true
        OptionRow(title: "Gyroscope", symbol: "gyroscope") {
            Toggle("Gyroscope", isOn: Binding(
                get: { on },
                set: { model.setMotionSensors($0) }
            ))
            .labelsHidden()
            .disabled(!model.motionSensorsAvailable && !on)
        }
    }
}

/// When iOS has refused the microphone there is nothing to ask again: say so,
/// and offer Andarium's page in Settings.
struct MicrophoneAccessAlert: ViewModifier {
    @Environment(AppModel.self) private var model

    func body(content: Content) -> some View {
        @Bindable var model = model
        content.alert(
            "Allow Microphone Access", isPresented: Binding(
                get: { model.permissionPrompt == .microphone },
                set: { if !$0 { model.permissionPrompt = nil } }
            )
        ) {
            Button("Open Settings") { model.openMicrophoneSettings() }
            Button("Not Now", role: .cancel) {}
        } message: {
            Text("iOS is not allowing Andarium to use the microphone. Turn on Microphone for Andarium in Settings, then come back to use it.")
        }
    }
}

extension View {
    func microphoneAccessAlert() -> some View { modifier(MicrophoneAccessAlert()) }
}

// MARK: Backup

/// Whether iCloud and computer backups of this device take the guest's data.
struct BackupOption: View {
    @Binding var isOn: Bool

    var body: some View {
        OptionRow(title: "Back Up Guest Data", symbol: "icloud.fill") {
            Toggle("Back Up Guest Data", isOn: $isOn).labelsHidden()
        }
    }
}

/// What Back Up Guest Data does to iCloud and computer backups.
struct BackupNote: View {
    let host: HostDevice

    var body: some View {
        SectionNote("Puts the guest's apps and files in iCloud and computer backups of this \(host.name).")
    }
}

struct BackupSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if let guest = model.guest {
            HomeSection(title: "Backup") {
                BackupOption(isOn: Binding(
                    get: { guest.includeInBackups },
                    set: { model.guest?.includeInBackups = $0 }
                ))
            } footer: {
                BackupNote(host: model.host)
            }
            .id("backup")
        }
    }
}
