// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import SwiftUI
import TipKit
import UIKit

/// The Simulator preview's entry. `-screen <name>` opens any screen or state
/// directly with mock data, so a simulator driven only by `simctl` reaches
/// every flow. The list is in the README.
struct AppEntry {
    var screen: String
    /// Boot screens animate and continue into the session instead of holding.
    var play: Bool
    var lowSpace: Bool
    var resetTips: Bool
    /// The boot step a frozen boot screen shows, from 0; the screen's own
    /// when absent.
    var bootStage: Int?
    /// The Microphone switch asks the Simulator's own permission instead of
    /// taking the access as given, so `simctl privacy` and the system's
    /// alert drive it as they drive the device's.
    var systemMicrophone: Bool

    static func fromArguments() -> AppEntry {
        let defaults = UserDefaults.standard
        return AppEntry(
            screen: defaults.string(forKey: "screen") ?? "home",
            play: defaults.bool(forKey: "play"),
            lowSpace: defaults.bool(forKey: "lowSpace"),
            resetTips: defaults.bool(forKey: "resetTips"),
            bootStage: defaults.string(forKey: "bootStage").flatMap { Int($0) },
            systemMicrophone: defaults.bool(forKey: "systemMicrophone")
        )
    }

    /// Where Home scrolls to for a screen that shows a lower section.
    private static let scrollTargets: [String: String] = [
        "home-options": "erase", "home-recovery-erase": "erase", "start-recovery": "erase",
        "home-display": "display",
        "home-capabilities": "capabilities", "home-mic-on": "capabilities", "home-mic-denied": "capabilities",
        "home-mic-alert": "capabilities", "home-gyroscope-unavailable": "capabilities",
        "home-backup": "backup",
    ]

    /// Where Settings scrolls to for a screen that shows a lower section.
    private static let settingsScrolls: [String: String] = [
        "settings-stats": "stats", "settings-display": "display",
        "settings-capabilities": "capabilities", "settings-mic-denied": "capabilities", "settings-mic-on": "capabilities",
    ]

    /// The page of Info a screen opens on.
    private static let infoPages: [String: InfoPage] = [
        "info-acknowledgements": .acknowledgements,
        "info-license": .license("mesa"),
        "info-license-gpl": .license("linux"),
    ]

    var hints: ScreenHints {
        ScreenHints(
            scrollTarget: Self.scrollTargets[screen],
            eraseConfirmed: screen == "erase-confirm",
            infoPage: Self.infoPages[screen]
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
        CaptureState.begin(screen: screen)
        let lowSpace = lowSpace || ["create-low-space", "start-low-space", "home-low-space"].contains(screen)
        let host = PreviewData.host(lowSpace: lowSpace)
        var guest: Guest? = PreviewData.sampleGuest(on: host)
        if ["welcome", "create", "create-low-space"].contains(screen) { guest = nil }
        let model = AppModel(
            guest: guest, host: host, system: PreviewData.system, capabilities: .preview,
            createRequirementBytes: PreviewData.createRequirementBytes
        )
        let freeze = !play

        func boot(_ kind: BootKind, stage: Int, seconds: Int) {
            model.frozenBootStage = freeze ? (bootStage ?? stage) : nil
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
        case "home-options", "home-display", "home-capabilities", "home-backup", "system-edit":
            break
        case "home-mic-on":
            model.guest?.microphone = true
            model.microphoneState = .granted
        case "home-mic-denied", "home-mic-alert":
            model.microphoneState = .denied
        case "home-gyroscope-unavailable":
            model.motionSensorsAvailable = false
        case "home-erased":
            model.erase()
        case "busy-create":
            model.busy = BusyState(title: "Creating Guest", detail: "Setting up Andarium with 16 GiB.", progress: 0.42)
        case "busy-erase":
            model.busy = BusyState(title: "Erasing Guest", detail: "Deleting everything the guest stored.", progress: 0.65)
        case "alert-update-unfinished":
            model.plan = .unfinishedUpdate(to: PreviewData.release, canRetry: false, canRestore: true)
        case "alert-blocked":
            model.plan = .blocked(.olderAndroid)
        case "boot":
            model.guest?.pendingCapacityGiB = 24
            boot(.normal, stage: 3, seconds: 9)
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
             "settings-adb-off", "settings-adb-no-address",
             "settings-install-failed", "settings-adb",
             "settings-stats", "settings-display", "settings-capabilities", "settings-mic-denied", "settings-mic-on":
            let session = model.session
            let file = ApkInstall.sampleFile
            let size = ApkInstall.sampleSizeBytes
            switch screen {
            case "settings-installing":
                session.installs = [ApkInstall(fileName: file, sizeBytes: size, state: play ? .installing(0) : .installing(0.62))]
                if play {
                    session.installs = []
                    session.install(fileName: file, sizeBytes: size)
                }
            case "settings-installed":
                session.installs = [ApkInstall(fileName: file, sizeBytes: size, state: .installed)]
            case "settings-not-apk":
                session.installs = [ApkInstall(fileName: "Trip photos.zip", sizeBytes: UInt64(212.4 * Double(Limits.bytesPerMiB)), state: .notApk)]
            case "settings-bundle":
                session.installs = [ApkInstall(fileName: "Trip Planner.xapk", sizeBytes: UInt64(84.6 * Double(Limits.bytesPerMiB)), state: .bundle)]
            case "settings-install-failed":
                session.installs = [ApkInstall(
                    fileName: "Retro Racer 2.3 x86.apk", sizeBytes: 96 * Limits.bytesPerMiB,
                    state: .failed("The guest couldn\u{2019}t install it: this app has no version for arm64 devices.")
                )]
            case "settings-adb":
                session.adb = true
                session.adbAddress = SessionState.previewAddress
            case "settings-adb-no-address":
                session.adb = true
            case "settings-adb-off":
                session.usbDebugging = false
            case "settings-mic-denied":
                model.microphoneState = .denied
            case "settings-mic-on":
                model.guest?.microphone = true
                model.microphoneState = .granted
            default:
                break
            }
            // The two guest operations play on the full-screen overlay with
            // Settings already gone.
            let operation = screen == "bug-report" || screen == "install-apk"
            model.sessionPreset = SessionPreset(
                settings: !operation,
                settingsLarge: Self.settingsScrolls.keys.contains(screen) || screen == "settings",
                settingsScroll: Self.settingsScrolls[screen]
            )
            model.phase = .running(.normal)
            // The real operation lasts a few seconds, too short for a
            // screenshot to catch: hold its overlay part way unless `-play`.
            if operation && freeze { model.busy = Self.operationOverlay(for: screen) }
        case "recovery", "recovery-open":
            model.sessionPreset = SessionPreset(keysOpen: screen == "recovery-open")
            model.phase = .running(.recovery)
        case "failed":
            // The failure path a real run takes: a crash after 42 minutes.
            model.fail()
        default:
            break
        }
        if systemMicrophone { useSystemMicrophone(in: model) }
        return model
    }

    /// A guest operation's overlay as it looks part way: what `createBugReport`
    /// and `installPicked` show, held still.
    private static func operationOverlay(for screen: String) -> BusyState {
        let cancel = BusyCancel {}
        if screen == "bug-report" {
            return BusyState(
                title: "Creating Bug Report",
                detail: "The guest is collecting its logs and state. This can take several minutes.",
                progress: 0.6, cancel: cancel
            )
        }
        return BusyState(
            title: "Installing App",
            detail: "\(ApkInstall.sampleFile) \u{00B7} \(Formats.size(bytes: ApkInstall.sampleSizeBytes))",
            progress: 0.45, cancel: cancel
        )
    }

    /// The device's microphone conversation against the Simulator's
    /// permission, which is read again whenever the app becomes active.
    private func useSystemMicrophone(in model: AppModel) {
        model.microphone = MicrophoneController(model: model, source: SimulatorMicrophoneSource()) { _ in }
        NotificationCenter.default.addObserver(
            forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { _ in
            Task { @MainActor in model.microphone?.refresh() }
        }
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
        case "system-edit":
            model.sheet = .system
        case "home-mic-alert":
            model.setMicrophone(true)
        case "erase", "erase-confirm":
            model.sheet = .erase
        case "info", "info-acknowledgements", "info-license", "info-license-gpl":
            model.sheet = .info
        case "start-low-space", "alert-update-unfinished", "alert-blocked":
            model.start(.normal)
        case "alert-error":
            model.alert = AppAlert(
                title: "Settings Not Saved",
                message: "The file couldn\u{2019}t be saved because there is no space left on the device."
            )
        case "bug-report" where play:
            try? await Task.sleep(for: .milliseconds(450))
            model.createBugReport()
        case "install-apk" where play:
            try? await Task.sleep(for: .milliseconds(450))
            _ = model.installPicked(PreviewData.sampleApk())
        default:
            break
        }
    }
}
