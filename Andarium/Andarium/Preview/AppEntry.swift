import AndariumCore
import Foundation
import SwiftUI
import TipKit

/// The Simulator preview's entry. `-screen <name>` opens any screen or state
/// directly with mock data, so a simulator driven only by `simctl` reaches
/// every flow. The list is in the README.
struct AppEntry {
    var screen: String
    /// Boot screens animate and continue into the session instead of holding.
    var play: Bool
    var lowSpace: Bool
    var resetTips: Bool

    static func fromArguments() -> AppEntry {
        let defaults = UserDefaults.standard
        return AppEntry(
            screen: defaults.string(forKey: "screen") ?? "home",
            play: defaults.bool(forKey: "play"),
            lowSpace: defaults.bool(forKey: "lowSpace"),
            resetTips: defaults.bool(forKey: "resetTips")
        )
    }

    var hints: ScreenHints {
        ScreenHints(
            scrollTarget: screen == "home-options" ? "erase" : nil,
            eraseConfirmed: screen == "erase-confirm",
            acknowledgements: screen == "info-acknowledgements"
        )
    }

    /// The side-keys tip. A screenshot of the tip shows it whatever the
    /// datastore says; other session screenshots leave it out.
    func configureTips() {
        if resetTips { try? Tips.resetDatastore() }
        if screen == "session-tip" {
            Tips.showAllTipsForTesting()
        } else if screen.hasPrefix("session") || screen.hasPrefix("settings") || screen.hasPrefix("recovery") || screen.hasPrefix("boot")
            || screen == "bug-report" || screen == "install-apk" {
            Tips.hideAllTipsForTesting()
        }
        try? Tips.configure([.displayFrequency(.immediate)])
    }

    func makeModel() -> AppModel {
        let lowSpace = lowSpace || ["create-low-space", "start-low-space", "home-low-space"].contains(screen)
        let host = PreviewData.host(lowSpace: lowSpace)
        var guest: Guest? = PreviewData.sampleGuest(on: host)
        if ["welcome", "create", "create-low-space"].contains(screen) { guest = nil }
        let model = AppModel(
            guest: guest, host: host, system: PreviewData.system, capabilities: .preview,
            createRequirementBytes: PreviewData.createRequirementBytes, systemBytes: PreviewData.systemBytes
        )
        let freeze = !play

        func boot(_ kind: BootKind, stage: Int, seconds: Int) {
            model.frozenBootStage = freeze ? stage : nil
            model.frozenElapsed = seconds
            model.phase = .booting(kind)
        }

        switch screen {
        case "home-new":
            model.guest = Guest.fresh(on: host)
            model.plan = .firstStart
        case "home-grow":
            model.guest?.pendingCapacityGiB = 24
        case "resize":
            model.resizePreset = 24
        case "home-max":
            model.guest?.capacityGiB = Limits.capacityRangeGiB.upperBound
        case "home-update":
            model.plan = .update(from: PreviewData.previousRelease, to: PreviewData.release)
        case "home-update-failed":
            model.plan = .unfinishedUpdate(to: PreviewData.release, canRetry: true, canRestore: true)
            model.planAfterRestore = .update(from: PreviewData.previousRelease, to: PreviewData.release)
        case "home-update-failed-restore":
            model.plan = .unfinishedUpdate(to: PreviewData.release, canRetry: false, canRestore: true)
            model.planAfterRestore = .update(from: PreviewData.previousRelease, to: PreviewData.release)
        case "home-blocked":
            model.plan = .blocked(.olderAndroid)
        case "home-closed":
            model.notice = .closedInBackground
        case "home-options":
            break
        case "home-erased":
            model.erase()
        case "boot":
            model.guest?.pendingCapacityGiB = 24
            boot(.normal, stage: 2, seconds: 9)
        case "boot-first":
            model.erase()
            boot(.firstStart, stage: 3, seconds: 74)
        case "boot-upgrade":
            model.bootFrom = PreviewData.previousRelease
            boot(.upgrade, stage: 3, seconds: 51)
        case "boot-recovery":
            boot(.recovery, stage: 3, seconds: 7)
        case "boot-restart":
            boot(.restart, stage: 3, seconds: 8)
        case "session", "session-tip", "session-open", "session-keyboard", "session-stats",
             "session-power", "session-setup", "session-off", "session-left", "session-volume":
            model.session.showStats = screen == "session-stats"
            model.sessionPreset = SessionPreset(
                keysOpen: ["session-open", "session-off", "session-left", "session-volume"].contains(screen),
                volumePanel: screen == "session-volume",
                powerMenu: screen == "session-power",
                leftEdge: ["session-left", "session-volume"].contains(screen),
                guestScreen: {
                    switch screen {
                    case "session-setup": .setup
                    case "session-off": .off
                    case "session-keyboard": .search
                    default: .home
                    }
                }()
            )
            model.phase = .running(.normal)
        case "settings", "settings-installing", "settings-installed", "settings-not-apk",
             "settings-bundle", "install-apk",
             "bug-report",
             "settings-install-failed", "settings-adb":
            let session = model.session
            let file = ApkInstall.sampleFile
            let size = ApkInstall.sampleSizeMB
            switch screen {
            case "settings-installing":
                session.installs = [ApkInstall(fileName: file, sizeMB: size, state: play ? .installing(0) : .installing(0.62))]
                if play {
                    session.installs = []
                    session.install(fileName: file, sizeMB: size)
                }
            case "settings-installed":
                session.installs = [ApkInstall(fileName: file, sizeMB: size, state: .installed)]
            case "settings-not-apk":
                session.installs = [ApkInstall(fileName: "Trip photos.zip", sizeMB: 212.4, state: .notApk)]
            case "settings-bundle":
                session.installs = [ApkInstall(fileName: "Trip Planner.xapk", sizeMB: 84.6, state: .bundle)]
            case "settings-install-failed":
                session.installs = [ApkInstall(
                    fileName: "Retro Racer 2.3 x86.apk", sizeMB: 96.0,
                    state: .failed("The guest couldn\u{2019}t install it: this app has no version for arm64 devices.")
                )]
            case "settings-adb":
                session.adb = true
            default:
                break
            }
            // The two guest operations play on the full-screen overlay with
            // Settings already gone.
            let operation = screen == "bug-report" || screen == "install-apk"
            model.sessionPreset = SessionPreset(settings: !operation, settingsLarge: screen == "settings")
            model.phase = .running(.normal)
        case "recovery", "recovery-open":
            model.sessionPreset = SessionPreset(keysOpen: screen == "recovery-open")
            model.phase = .running(.recovery)
        case "off":
            model.phase = .poweredOff
        case "failed":
            model.phase = .failed
        default:
            break
        }
        return model
    }

    /// Sheets and dialogs open after the first frame so SwiftUI presents them
    /// the way a tap would.
    @MainActor
    func applyDeferred(to model: AppModel) async {
        try? await Task.sleep(for: .milliseconds(450))
        switch screen {
        case "create", "create-low-space":
            model.sheet = .create
        case "resize":
            model.sheet = .resize
        case "erase", "erase-confirm":
            model.sheet = .erase
        case "info", "info-acknowledgements":
            model.sheet = .info
        case "start-menu", "start-recovery":
            try? await Task.sleep(for: .milliseconds(450))
            model.startMenu = true
        case "start-low-space":
            model.start(.normal)
        case "bug-report":
            try? await Task.sleep(for: .milliseconds(450))
            model.createBugReport()
        case "install-apk":
            try? await Task.sleep(for: .milliseconds(450))
            _ = model.installPicked(PreviewData.sampleApk())
        default:
            break
        }
    }
}
