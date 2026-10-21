import Foundation
import SwiftUI

struct EmulatorSettingsView: View {
    @Environment(\.dismiss) private var dismiss

    @State private var vCPUCount: Int
    @State private var guestMemoryMiB: Int
    @State private var overlaySizeGiB: Int
    @State private var androidDiskSizeGiB: Int
    @State private var rootfsFilename: String
    @State private var retinaMode: Bool
    @State private var autoResizeGuest: Bool
    @State private var androidBootMode: AndroidBootMode
    @State private var backgroundMode: BackgroundMode
    @State private var backgroundLimitMinutes: Int

    private let maximumVCPUCount: Int
    private let defaults: UserDefaults
    private let rootfsFilenames: [String]
    private let documentsDirectory: URL

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
        _retinaMode = State(initialValue: configuration.retinaMode)
        _autoResizeGuest = State(initialValue: configuration.autoResizeGuest)
        _androidBootMode = State(initialValue: configuration.androidBootMode)
        let background = BackgroundSettings(defaults: defaults)
        _backgroundMode = State(
            initialValue: background.mode(forGuest: configuration.rootfsFilename)
        )
        _backgroundLimitMinutes = State(
            initialValue: background.limitMinutes(
                forGuest: configuration.rootfsFilename
            )
        )
    }

    /// The window is offered only where iOS can show it; a guest that already
    /// chose it keeps showing as chosen.
    private var offeredBackgroundModes: [BackgroundMode] {
        BackgroundMode.allCases.filter {
            $0 != .pictureInPicture || $0 == backgroundMode ||
                GuestPictureInPicture.isSupported
        }
    }

    /// The times on offer, with the one a guest already has even if it is not
    /// among them.
    private var offeredLimitMinutes: [Int] {
        Set(BackgroundSettings.limitChoicesMinutes + [backgroundLimitMinutes])
            .sorted()
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

                    Text(
                        "EROFS images and .\(GuestBundle.pathExtension) " +
                        "guest bundles are loaded from Documents."
                    )
                }

                Section("Background") {
                    Picker(
                        "When Kernarium is in the background",
                        selection: $backgroundMode
                    ) {
                        ForEach(offeredBackgroundModes) { mode in
                            Text(mode.title).tag(mode)
                        }
                    }
                    .accessibilityIdentifier("backgroundModePicker")

                    if backgroundMode == .keepRunning {
                        Picker(
                            "Keep running for",
                            selection: $backgroundLimitMinutes
                        ) {
                            ForEach(offeredLimitMinutes, id: \.self) { minutes in
                                Text(BackgroundSettings.limitTitle(
                                    minutes: minutes
                                ))
                                .tag(minutes)
                            }
                        }
                        .accessibilityIdentifier("backgroundLimitPicker")
                    }

                    Text(backgroundMode.explanation)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
                .onChange(of: rootfsFilename) { _, filename in
                    let background = BackgroundSettings(defaults: defaults)
                    backgroundMode = background.mode(forGuest: filename)
                    backgroundLimitMinutes = background.limitMinutes(
                        forGuest: filename
                    )
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
            retinaMode: retinaMode,
            autoResizeGuest: autoResizeGuest,
            androidBootMode: androidBootMode
        ).save(defaults: defaults)
        let background = BackgroundSettings(defaults: defaults)
        background.setMode(backgroundMode, forGuest: rootfsFilename)
        background.setLimitMinutes(
            backgroundLimitMinutes,
            forGuest: rootfsFilename
        )
        dismiss()
    }
}
