import Foundation
import SwiftUI

struct EmulatorSettingsView: View {
    @Environment(\.dismiss) private var dismiss

    @State private var vCPUCount: Int
    @State private var guestMemoryMiB: Int
    @State private var overlaySizeGiB: Int
    @State private var rootfsFilename: String

    private let maximumVCPUCount: Int
    private let defaults: UserDefaults
    private let rootfsFilenames: [String]

    init(
        defaults: UserDefaults = .standard,
        maximumVCPUCount: Int = EmulatorConfiguration.physicalProcessorCount,
        documentsDirectory: URL = RootfsImage.documentsDirectory
    ) {
        let maximum = max(maximumVCPUCount, 1)
        let configuration = EmulatorConfiguration.load(
            defaults: defaults,
            physicalProcessorCount: maximum
        )
        self.maximumVCPUCount = maximum
        self.defaults = defaults
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
        _rootfsFilename = State(initialValue: configuration.rootfsFilename)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Hardware") {
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

                    Text("EROFS images are loaded from Documents.")
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
            rootfsFilename: rootfsFilename
        ).save(defaults: defaults)
        dismiss()
    }
}
