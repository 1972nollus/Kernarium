import Foundation
import SwiftUI

struct EmulatorSettingsView: View {
    @Environment(\.dismiss) private var dismiss

    @State private var vCPUCount: Int
    @State private var guestMemoryMiB: Int
    @State private var overlaySizeGiB: Int
    @State private var androidDiskSizeGiB: Int
    @State private var rootfsFilename: String
    @State private var kernelPageSize: KernelPageSize
    @State private var retinaMode: Bool
    @State private var autoResizeGuest: Bool
    @State private var androidBootMode: AndroidBootMode

    private let maximumVCPUCount: Int
    private let defaults: UserDefaults
    private let rootfsFilenames: [String]
    private let documentsDirectory: URL

    /// A bundle whose kernel exists in only one page profile decides the
    /// profile; the picker then shows that profile and stops offering the other
    /// rather than letting a selection be saved that cannot boot.
    private var requiredPageSize: KernelPageSize? {
        RootfsImage.requiredPageSize(
            forSelectionNamed: rootfsFilename,
            in: documentsDirectory
        )
    }

    /// Shows the profile the selection forces while remembering what the user
    /// last chose, so switching back to an image restores their own setting
    /// instead of the one a bundle imposed.
    private var shownPageSize: Binding<KernelPageSize> {
        Binding(
            get: { requiredPageSize ?? kernelPageSize },
            set: { kernelPageSize = $0 }
        )
    }

    private var androidBundleSelected: Bool {
        RootfsImage.selection(
            named: rootfsFilename,
            in: documentsDirectory
        )?.flavor == .android
    }

    init(
        defaults: UserDefaults = .standard,
        maximumVCPUCount: Int = EmulatorConfiguration.physicalProcessorCount,
        documentsDirectory: URL = RootfsImage.documentsDirectory
    ) {
        let maximum = max(maximumVCPUCount, 1)
        let configuration = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: maximum,
            documentsDirectory: documentsDirectory
        )
        self.maximumVCPUCount = maximum
        self.defaults = defaults
        self.documentsDirectory = documentsDirectory
        let availableRootfsFilenames = RootfsImage.availableFilenames(
            in: documentsDirectory
        )
        self.rootfsFilenames = availableRootfsFilenames.contains(
            configuration.rootfsFilename
        ) ? availableRootfsFilenames : [configuration.rootfsFilename] +
            availableRootfsFilenames
        _vCPUCount = State(initialValue: configuration.vCPUCount)
        _guestMemoryMiB = State(initialValue: configuration.guestMemoryMiB)
        _overlaySizeGiB = State(initialValue: configuration.overlaySizeGiB)
        _androidDiskSizeGiB = State(
            initialValue: configuration.androidDiskSizeGiB
        )
        _rootfsFilename = State(initialValue: configuration.rootfsFilename)
        _kernelPageSize = State(initialValue: configuration.kernelPageSize)
        _retinaMode = State(initialValue: configuration.retinaMode)
        _autoResizeGuest = State(initialValue: configuration.autoResizeGuest)
        _androidBootMode = State(initialValue: configuration.androidBootMode)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Hardware") {
                    Toggle("Retina mode", isOn: $retinaMode)
                        .accessibilityIdentifier("retinaModeToggle")

                    Toggle(
                        "Automatically resize guest display",
                        isOn: $autoResizeGuest
                    )
                    .accessibilityIdentifier("autoResizeGuestToggle")

                    Text(
                        "Controls whether settled view-size changes resize " +
                            "the guest. When off, the guest keeps its " +
                            "resolution and is fitted to the view. A " +
                            "guest-selected resolution pauses automatic " +
                            "resizing until the guest restarts."
                    )
                    .font(.footnote)
                    .foregroundStyle(.secondary)

                    Picker("Kernel page size", selection: shownPageSize) {
                        ForEach(KernelPageSize.allCases) { pageSize in
                            Text(pageSize.title).tag(pageSize)
                        }
                    }
                    .disabled(requiredPageSize != nil)
                    .accessibilityIdentifier("kernelPageSizePicker")

                    Stepper(
                        "vCPUs: \(vCPUCount)",
                        value: $vCPUCount,
                        in: 1...maximumVCPUCount
                    )
                    .accessibilityIdentifier("vCPUCountStepper")

                    Stepper(
                        "Guest RAM: \(guestMemoryMiB) MB",
                        value: $guestMemoryMiB,
                        in: EmulatorConfiguration.guestMemoryRangeMiB,
                        step: EmulatorConfiguration.guestMemoryStepMiB
                    )
                    .accessibilityIdentifier("guestMemoryStepper")

                    Stepper(
                        "Overlay FS: \(overlaySizeGiB) GB",
                        value: $overlaySizeGiB,
                        in: EmulatorConfiguration.overlaySizeRangeGiB
                    )
                    .accessibilityIdentifier("overlaySizeStepper")
                }

                Section("Storage") {
                    Picker("Root filesystem", selection: $rootfsFilename) {
                        ForEach(rootfsFilenames, id: \.self) { filename in
                            Text(filename).tag(filename)
                        }
                    }
                    .accessibilityIdentifier("rootfsPicker")

                    if androidBundleSelected {
                        Stepper(
                            "Android disk: \(androidDiskSizeGiB) GiB",
                            value: $androidDiskSizeGiB,
                            in: EmulatorConfiguration.androidDiskSizeRangeGiB
                        )
                        .accessibilityIdentifier("androidDiskSizeStepper")

                        Picker("Android boot", selection: $androidBootMode) {
                            ForEach(AndroidBootMode.allCases) { mode in
                                Text(mode.title).tag(mode)
                            }
                        }
                        .accessibilityIdentifier("androidBootModePicker")
                    }

                    if let required = requiredPageSize {
                        Text(
                            "\(rootfsFilename) carries its own kernel and " +
                            "boots with \(required.title) pages."
                        )
                        .accessibilityIdentifier("guestBundleNote")
                    } else {
                        Text(
                            "EROFS images and .\(GuestBundle.pathExtension) " +
                            "guest bundles are loaded from Documents."
                        )
                    }
                }

                Section {
                    Text("Changes take effect the next time the app launches.")
                }
            }
            .navigationTitle("Emulator Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        dismiss()
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        save()
                    }
                    .accessibilityIdentifier("settingsSaveButton")
                }
            }
        }
    }

    private func save() {
        EmulatorConfiguration(
            vCPUCount: vCPUCount,
            guestMemoryMiB: guestMemoryMiB,
            overlaySizeGiB: overlaySizeGiB,
            androidDiskSizeGiB: androidDiskSizeGiB,
            rootfsFilename: rootfsFilename,
            kernelPageSize: requiredPageSize ?? kernelPageSize,
            retinaMode: retinaMode,
            autoResizeGuest: autoResizeGuest,
            androidBootMode: androidBootMode
        ).save(defaults: defaults)
        dismiss()
    }
}
