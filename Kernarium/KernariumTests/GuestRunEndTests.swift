// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
import UIKit
@testable import Kernarium

/// A launch runs its guest once. The runtime runs one guest per process and
/// refuses a second run, so after the run returns the application reports how
/// the guest ended and asks to be relaunched.
@MainActor
struct GuestRunEndTests {
    @Test
    func theNoticeTellsAShutdownFromAnError() {
        let shutDown = GuestRunEnd(result: 0)
        #expect(shutDown.title == "Guest Shut Down")
        #expect(shutDown.message == "Relaunch Kernarium to start it again.")

        let failed = GuestRunEnd(result: -5)
        #expect(failed.title == "Guest Stopped with an Error")
        #expect(failed.message.contains("-5"))
        #expect(
            failed.message.hasSuffix("Relaunch Kernarium to start it again.")
        )
        #expect(failed.systemName != shutDown.systemName)
    }

    /// Stops the application's own guest, so it runs once per test process.
    /// The application boots its guest as it launches. The kernel retires a
    /// stop cleanly only once its boot is under way, so the test gives it a
    /// few seconds before asking, and repeats the request until the run
    /// returns.
    @Test(.timeLimit(.minutes(3)))
    func anEndedRunIsNeverStartedAgain() async throws {
        let launcher = EmulatorLauncher.shared
        try #require(
            try await eventually(within: .seconds(90)) {
                launcher.phase == .running
            },
            "the application's guest did not start: \(launcher.phase)"
        )
        try await Task.sleep(for: .seconds(5))
        // Each poll repeats the stop request, so it polls four times a second.
        let ended = try await eventually(
            within: .seconds(90),
            every: .milliseconds(250)
        ) {
            if launcher.phase == .running { launcher.stop() }
            return launcher.phase == .ended(result: 0)
        }
        try #require(ended, "the guest did not end cleanly: \(launcher.phase)")

        // Each layout of the guest display asks the launcher to start a run;
        // once the run has ended, none may start.
        let window = try #require(
            try windowScene().windows.first {
                $0.rootViewController != nil && !$0.isHidden
            }
        )
        let display = try #require(Self.guestDisplay(in: window))
        for _ in 0..<3 {
            display.setNeedsLayout()
            display.layoutIfNeeded()
            window.rootViewController?.view.setNeedsLayout()
            window.rootViewController?.view.layoutIfNeeded()
            try await Task.sleep(for: .milliseconds(500))
            #expect(launcher.phase == .ended(result: 0))
        }
        #expect(
            try await eventually {
                accessibilityElement("guestEndedNotice", in: window) != nil
            },
            "the application does not say that the guest has ended"
        )
        #expect(launcher.phase == .ended(result: 0))
    }

    /// The guest display is private to the application; it is the Metal view
    /// whose layout reports the guest's geometry.
    private static func guestDisplay(in view: UIView) -> UIView? {
        if NSStringFromClass(type(of: view)).hasSuffix("GuestMetalView") {
            return view
        }
        for child in view.subviews {
            if let found = guestDisplay(in: child) { return found }
        }
        return nil
    }
}
