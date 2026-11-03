import AndariumCore
import SwiftUI

/// Setting up the one guest: the same controls, already at the defaults.
struct NewGuestSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var draft: Guest?

    private var draftBinding: Binding<Guest> {
        Binding(
            get: { draft ?? Guest.fresh(on: model.host) },
            set: { draft = $0 }
        )
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 22) {
                    if !model.canCreate {
                        NoticeCard(
                            symbol: "externaldrive.badge.exclamationmark", color: .orange,
                            title: "Not enough free space",
                            text: "Setting up needs \(Formats.hostGB(bytes: model.createRequirementBytes)) free for the guest's setup. This \(model.host.name) has \(Formats.gb(model.host.freeStorageGB)). Free up space in Settings › General › \(model.host.name) Storage."
                        )
                    }
                    ControlsPanel(guest: draftBinding, host: model.host, isNew: true)
                    VStack(alignment: .leading, spacing: 8) {
                        HStack(spacing: 12) {
                            IconTile(symbol: "icloud.fill")
                            Text("Include in Backups")
                            Spacer(minLength: 8)
                            Toggle("Include in Backups", isOn: Binding(
                                get: { draftBinding.wrappedValue.includeInBackups },
                                set: { draftBinding.wrappedValue.includeInBackups = $0 }
                            ))
                            .labelsHidden()
                        }
                        .padding(.horizontal, 16)
                        .frame(minHeight: 54)
                        .glassEffect(.regular, in: .rect(cornerRadius: 26))
                        BackupNote(host: model.host)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.top, 8)
                .padding(.bottom, 24)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("New Guest")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", role: .cancel) { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Create") {
                        model.create(draft ?? Guest.fresh(on: model.host))
                        dismiss()
                    }
                    .fontWeight(.semibold)
                    .disabled(!model.canCreate)
                }
            }
        }
    }
}

/// Erasing lists exactly what goes and what stays, and the red button stays
/// locked until the loss is acknowledged.
struct EraseSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var understood: Bool

    init(confirmed: Bool) {
        _understood = State(initialValue: confirmed)
    }

    var body: some View {
        let guest = model.guest ?? Guest.fresh(on: model.host)
        NavigationStack {
            List {
                Section {
                    HStack(alignment: .top, spacing: 14) {
                        Image(systemName: "trash.fill")
                            .font(.system(size: 22, weight: .semibold))
                            .foregroundStyle(.white)
                            .frame(width: 48, height: 48)
                            .background(Color.red.gradient, in: .circle)
                        VStack(alignment: .leading, spacing: 3) {
                            Text("Erase Guest")
                                .font(.title2.weight(.bold))
                            Text("Deletes everything the guest has stored. The next start runs the system's setup.")
                                .font(.subheadline)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .listRowInsets(EdgeInsets(top: 0, leading: 4, bottom: 4, trailing: 4))
                    .listRowBackground(Color.clear)
                }
                Section("Erased") {
                    item("person.crop.circle", "Accounts and settings", nil)
                    item("photo.on.rectangle", "Photos, downloads and other files", nil)
                    item("internaldrive", "Data on this \(model.host.name)", Formats.gb(guest.usedGB))
                    if case .unfinishedUpdate(_, _, true) = model.plan {
                        item("clock.arrow.circlepath", "The data saved before the update", nil)
                    }
                }
                Section("Kept") {
                    item("checkmark.seal", "System \(model.system.version), built in", nil, kept: true)
                    item("slider.horizontal.3", "vCPUs, memory, storage size and options", nil, kept: true)
                }
            }
            .contentMargins(.top, 0, for: .scrollContent)
            .listSectionSpacing(.compact)
            .safeAreaInset(edge: .bottom) {
                VStack(spacing: 12) {
                    Button {
                        withAnimation(.snappy) { understood.toggle() }
                    } label: {
                        HStack(spacing: 12) {
                            Image(systemName: understood ? "checkmark.circle.fill" : "circle")
                                .font(.title3)
                                .foregroundStyle(understood ? Color.red : Color(.tertiaryLabel))
                                .contentTransition(.symbolEffect(.replace))
                            Text("I understand the erased data can't be recovered.")
                                .font(.subheadline)
                                .foregroundStyle(Color.primary)
                                .multilineTextAlignment(.leading)
                            Spacer(minLength: 0)
                        }
                        .padding(.horizontal, 16)
                        .frame(minHeight: 52)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .glassEffect(.regular.interactive(), in: .rect(cornerRadius: 20))
                    .sensoryFeedback(.selection, trigger: understood)

                    Button(role: .destructive) {
                        model.erase()
                        dismiss()
                    } label: {
                        Text("Erase Guest")
                            .font(.body.weight(.semibold))
                            .frame(maxWidth: .infinity)
                            .frame(height: 50)
                    }
                    .buttonStyle(.glassProminent)
                    .tint(.red)
                    .disabled(!understood)
                }
                .padding(.horizontal, 20)
                .padding(.top, 12)
                .padding(.bottom, 8)
                .background(Color(.systemGroupedBackground).opacity(0.94))
            }
            .navigationTitle("")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", role: .cancel) { dismiss() }
                }
            }
        }
    }

    private func item(_ symbol: String, _ title: String, _ value: String?, kept: Bool = false) -> some View {
        HStack(spacing: 12) {
            Image(systemName: symbol)
                .foregroundStyle(kept ? Color.secondary : Color.red)
                .frame(width: 24)
            Text(title)
            Spacer()
            if let value {
                Text(value).foregroundStyle(.secondary)
            }
        }
    }
}

/// On iPad, a form-width sheet tall enough to show what is erased and what is
/// kept without scrolling. On iPhone the sheet is full height anyway.
nonisolated struct EraseSheetSizing: PresentationSizing {
    func proposedSize(for root: PresentationSizingRoot, context: PresentationSizingContext) -> ProposedViewSize {
        ProposedViewSize(width: 560, height: 740)
    }
}

/// What is installed and where the space goes. Read-only.
struct InfoSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var path: [String]

    init(acknowledgements: Bool) {
        _path = State(initialValue: acknowledgements ? ["ack"] : [])
    }

    /// The most memory a guest has at its vCPU count; a new guest takes every
    /// vCPU the host offers.
    private var memoryForAGuest: String {
        let count = model.guest?.cores ?? model.host.processorCount
        return Formats.memoryBound(model.host.memory.maximumMiB(vCPUs: count), vCPUs: count)
    }

    var body: some View {
        NavigationStack(path: $path) {
            List {
                Section {
                    HStack(spacing: 16) {
                        StackMark(lit: true, width: 62)
                            .frame(width: 80)
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Andarium").font(.title3.weight(.semibold))
                            Text("Version \(model.system.appVersion)")
                                .font(.subheadline)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .padding(.vertical, 6)
                }
                Section {
                    LabeledContent("Version", value: model.system.version)
                    LabeledContent("Build", value: model.system.release)
                    if let patch = model.system.securityPatch {
                        LabeledContent("Security Patch", value: patch)
                    }
                    LabeledContent("Kernel", value: model.system.kernel)
                    LabeledContent("Architecture", value: model.system.architecture)
                } header: {
                    Text("Built-in System")
                } footer: {
                    if model.capabilities.systemUpdate {
                        Text("The system is read-only and updates with Andarium from the App Store.")
                    } else {
                        Text("A guest keeps the system it was created with. Erase the guest to start again with the system in this version of Andarium.")
                    }
                }
                Section("Storage") {
                    // Every size on this page is in decimal gigabytes, as iOS's
                    // own storage screens state them.
                    StorageBar(
                        guestGB: Formats.decimalGB(bytes: model.guest?.usedBytes ?? 0),
                        systemGB: Formats.decimalGB(bytes: model.systemBytes),
                        freeGB: model.host.freeStorageGB
                    )
                    .padding(.vertical, 6)
                    if let guest = model.guest {
                        LabeledContent(
                            "Guest Data",
                            value: "\(Formats.hostGB(bytes: guest.usedBytes)) of \(Formats.hostGB(bytes: guest.effectiveCapacityBytes))"
                        )
                    }
                    LabeledContent("System", value: Formats.hostGB(bytes: model.systemBytes))
                    LabeledContent("Free on This \(model.host.name)", value: Formats.gb(model.host.freeStorageGB))
                }
                Section("This \(model.host.name)") {
                    LabeledContent("Processor", value: "\(model.host.physicalProcessorCount) cores")
                    LabeledContent("Memory for a Guest", value: memoryForAGuest)
                }
                Section {
                    NavigationLink("Acknowledgements", value: "ack")
                } footer: {
                    Text("The guest hears the microphone only while Microphone is on.")
                }
            }
            .navigationTitle("Info")
            .navigationBarTitleDisplayMode(.inline)
            .navigationDestination(for: String.self) { _ in AcknowledgementsView() }
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}

struct StorageBar: View {
    let guestGB: Double
    let systemGB: Double
    let freeGB: Double

    var body: some View {
        let total = guestGB + systemGB + freeGB
        VStack(alignment: .leading, spacing: 8) {
            GeometryReader { geo in
                HStack(spacing: 2) {
                    Rectangle().fill(Color.accentColor)
                        .frame(width: max(4, geo.size.width * guestGB / total))
                    Rectangle().fill(Color.accentColor.opacity(0.45))
                        .frame(width: max(4, geo.size.width * systemGB / total))
                    Rectangle().fill(Color(.systemFill))
                }
                .clipShape(.capsule)
            }
            .frame(height: 10)
            HStack(spacing: 14) {
                key(Color.accentColor, "Guest")
                key(Color.accentColor.opacity(0.45), "System")
                key(Color(.systemFill), "Free")
            }
            .font(.caption)
            .foregroundStyle(.secondary)
        }
    }

    private func key(_ color: Color, _ title: String) -> some View {
        HStack(spacing: 5) {
            Circle().fill(color).frame(width: 8, height: 8)
            Text(title)
        }
    }
}

struct AcknowledgementsView: View {
    private let items: [(String, String)] = [
        ("LineageOS", "Apache License 2.0 and others"),
        ("Android Open Source Project", "Apache License 2.0"),
        ("Linux", "GNU General Public License v2"),
        ("User-Mode Linux", "GNU General Public License v2"),
        ("virglrenderer", "MIT License"),
        ("Mesa", "MIT License"),
    ]

    var body: some View {
        List {
            Section {
                ForEach(items, id: \.0) { item in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(item.0)
                        Text(item.1).font(.footnote).foregroundStyle(.secondary)
                    }
                    .padding(.vertical, 2)
                }
            } footer: {
                Text("Source code for the built-in system is available as its licences require.\n\nAndroid, LineageOS, Linux and the other names and marks here belong to their owners. They identify the software built into Andarium and imply no endorsement.")
            }
        }
        .navigationTitle("Acknowledgements")
    }
}
