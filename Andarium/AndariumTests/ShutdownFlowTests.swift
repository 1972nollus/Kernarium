// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Foundation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Real guest sessions through the production Home and boot views. Runs alone
/// with ANDARIUM_SHUTDOWN_TESTS=1 and ANDARIUM_GUEST_INSTANCE=shutdown-flow,
/// without ANDARIUM_START. The ordinary guest directory is never opened.
@MainActor
struct ShutdownFlowTests {
    @Test(.timeLimit(.minutes(20)))
    func cleanPowerOffReturnsHomeAndAnUncleanEndRequiresRelaunch() async throws {
        guard ProcessInfo.processInfo.environment["ANDARIUM_SHUTDOWN_TESTS"] == "1" else { return }
        #expect(ProcessInfo.processInfo.environment["ANDARIUM_START"] == nil)
        try #require(ProcessInfo.processInfo.environment["ANDARIUM_GUEST_INSTANCE"] == "shutdown-flow")
        let directory = AppEntry.guestDirectory(instance: "shutdown-flow")
        if FileManager.default.fileExists(atPath: directory.path) {
            try FileManager.default.removeItem(at: directory)
        }
        defer { try? FileManager.default.removeItem(at: directory) }
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        try #require(model.alert == nil, "\(String(describing: model.alert))")
        let scene = try #require(UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first)
        let keyWindow = scene.windows.first { $0.isKeyWindow }
        let window = try #require(keyWindow)
        let original = window.rootViewController
        defer { window.rootViewController = original }
        window.rootViewController = UIHostingController(rootView: RootView(entry: entry).environment(model))
        model.create(Guest(cores: 2, memoryMiB: 1536, capacityGiB: 8))
        let createDeadline = ContinuousClock.now + .seconds(30)
        while model.busy != nil && ContinuousClock.now < createDeadline {
            try await Task.sleep(for: .milliseconds(100))
        }
        try #require(model.guest != nil && model.busy == nil && model.alert == nil)

        let identity = try AdbKeyStore().loadOrCreate()
        let operations = AdbGuestOperations(identity: identity, clock: GuestRunClock(), device: "device")
        for boot in 1...3 {
            model.start(.normal)
            let bootDeadline = ContinuousClock.now + .seconds(5 * 60)
            var ready = false
            while ContinuousClock.now < bootDeadline {
                if case .failed = model.phase { break }
                if let result = try? await operations.shell("getprop sys.boot_completed", limit: .seconds(10)),
                    result.text.hasPrefix("1"), model.phase == .running(.normal) {
                    ready = true
                    break
                }
                try await Task.sleep(for: .seconds(2))
            }
            try #require(ready, "Boot \(boot) did not reach Android: \(model.runReport)")
            print("ANDROID_SHUTDOWN: boot \(boot) reached Android in the same app process")
            let expected: Phase
            if boot < 3 {
                expected = .home
                _ = try? await operations.shell("reboot -p", limit: .seconds(30))
            } else {
                expected = .failed
                kernarium_runtime_loader_request_stop()
            }
            let endDeadline = ContinuousClock.now + .seconds(120)
            while model.phase != expected && ContinuousClock.now < endDeadline {
                try await Task.sleep(for: .milliseconds(200))
            }
            try #require(model.phase == expected, "Boot \(boot) ended at \(model.phase): \(model.runReport)")
            print("ANDROID_SHUTDOWN: boot \(boot) ended at \(model.phase)")
            if boot < 3 {
                #expect(model.runEnd.poweredOff)
                #expect(model.plan == .normal)
            } else {
                #expect(!model.runEnd.poweredOff)
                model.start(.normal)
                #expect(model.phase == .failed)
            }
        }
    }
}
