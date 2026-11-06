// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Setting up the one guest: its storage and backups. A new guest takes every
/// vCPU the device offers and the default memory (`Guest.fresh`), which the
/// System section of Home can change afterwards.
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
                            text: "Setting up needs \(Formats.size(bytes: model.createRequirementBytes)) free for the guest's setup. This \(model.host.name) has \(Formats.size(bytes: model.host.freeStorageBytes)). Free up space in Settings › General › \(model.host.name) Storage."
                        )
                    }
                    StoragePanel(guest: draftBinding, host: model.host, isNew: true)
                    HomeSection(title: "Backup") {
                        BackupOption(isOn: Binding(
                            get: { draftBinding.wrappedValue.includeInBackups },
                            set: { draftBinding.wrappedValue.includeInBackups = $0 }
                        ))
                    } footer: {
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

/// Edit on Home's System section: the guest's vCPUs and memory on their
/// rulers, with the limits of the device. Save applies them; they take effect
/// when the guest next starts.
struct SystemSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var draft: Guest?

    private var current: Guest { model.guest ?? Guest.fresh(on: model.host) }

    private var draftBinding: Binding<Guest> {
        Binding(get: { draft ?? current }, set: { draft = $0 })
    }

    private var changed: Bool {
        guard let draft else { return false }
        return draft.cores != current.cores || draft.memoryMiB != current.memoryMiB
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 14) {
                    SystemControls(guest: draftBinding, host: model.host)
                    SectionNote("Changes apply the next time the guest starts.")
                }
                .padding(.horizontal, 16)
                .padding(.top, 8)
                .padding(.bottom, 24)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("System")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", role: .cancel) { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        if let draft {
                            model.guest?.cores = draft.cores
                            model.guest?.memoryMiB = draft.memoryMiB
                        }
                        dismiss()
                    }
                    .fontWeight(.semibold)
                    .disabled(!changed)
                }
            }
        }
        .presentationDetents([.height(450)])
        .presentationSizing(FixedSheetSizing(width: 540, height: 460))
        .onAppear { draft = model.guest }
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
                            Text("Deletes everything the guest has stored. The next start runs Andarium's setup.")
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
                    item("internaldrive", "Data on this \(model.host.name)", Formats.size(bytes: guest.usedBytes))
                    if case .unfinishedUpdate(_, _, true) = model.plan {
                        item("clock.arrow.circlepath", "The data saved before the update", nil)
                    }
                }
                Section("Kept") {
                    item("checkmark.seal", "Andarium, built in", nil, kept: true)
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

/// The app, the system it carries and the notices for what is in it. Read-only.
struct InfoSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var path: [InfoPage]

    init(page: InfoPage?) {
        _path = State(initialValue: page?.trail ?? [])
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
                Section("System Info") {
                    LabeledContent("Android Version", value: model.system.version)
                    LabeledContent("Build", value: model.system.release)
                    if let patch = model.system.securityPatch {
                        LabeledContent("Security Patch", value: patch)
                    }
                    LabeledContent("Kernel", value: model.system.kernel)
                    LabeledContent("Architecture", value: model.system.architecture)
                }
                Section {
                    NavigationLink("Acknowledgements", value: InfoPage.acknowledgements)
                }
            }
            .navigationTitle("Info")
            .navigationBarTitleDisplayMode(.inline)
            .navigationDestination(for: InfoPage.self) { page in
                switch page {
                case .acknowledgements: AcknowledgementsView()
                case .license(let id): LicenseView(projectID: id)
                }
            }
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}
