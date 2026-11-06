import AndariumCore

#if !APP_STORE

/// The names a harness launch gives `LaunchOverrides` settings in the
/// environment (`Andarium/README.md#automation`). The `KERNARIUM_` names mean
/// what they mean in Kernarium; the `ANDARIUM_` ones are Andarium's own. An
/// App Store build compiles this file out, so it reads no launch setting.
extension LaunchOverrides {
    static func environmentName(_ setting: Setting) -> String {
        switch setting {
        case .vCPUCount: "KERNARIUM_VCPU_COUNT"
        case .memoryMiB: "KERNARIUM_GUEST_MEMORY_MIB"
        case .diskGiB: "KERNARIUM_ANDROID_DISK_SIZE_GIB"
        case .retina: "KERNARIUM_RETINA_MODE"
        case .followWindow: "KERNARIUM_GPU_AUTO_RESIZE"
        case .start: "ANDARIUM_START"
        case .bootMode: "KERNARIUM_ANDROID_BOOT_MODE"
        case .erase: "ANDARIUM_ERASE"
        case .stats: "ANDARIUM_STATS"
        case .instance: "ANDARIUM_GUEST_INSTANCE"
        }
    }

    init(environment: [String: String], processorCount: Int) {
        self.init(processorCount: processorCount) { environment[Self.environmentName($0)] }
    }
}

#endif
