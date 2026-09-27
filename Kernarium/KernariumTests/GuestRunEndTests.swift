// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Testing
import UIKit
@testable import Kernarium

/// A launch runs its guest once. The runtime cannot boot a second guest in the
/// same process, and an attempt ends the application, so after the run returns
/// the application reports how the guest ended and asks to be relaunched.
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
            await settles(within: 90) { launcher.phase == .running },
            "the application's guest did not start: \(launcher.phase)"
        )
        try await Task.sleep(for: .seconds(5))
        let ended = await settles(within: 90) {
            if launcher.phase == .running { launcher.stop() }
            return launcher.phase == .ended(result: 0)
        }
        try #require(ended, "the guest did not end cleanly: \(launcher.phase)")

        // Each layout of the guest display asks the launcher to start a run;
        // once the run has ended, none may start.
        let window = try #require(Self.applicationWindow())
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
            await settles(within: 10) {
                Self.element("guestEndedNotice", in: window, depth: 0) != nil
            },
            "the application does not say that the guest has ended"
        )
        #expect(launcher.phase == .ended(result: 0))
    }

    private func settles(
        within seconds: Double,
        _ condition: () -> Bool
    ) async -> Bool {
        let deadline = Date().addingTimeInterval(seconds)
        while Date() < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(250))
        }
        return condition()
    }

    private static func applicationWindow() -> UIWindow? {
        UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .flatMap(\.windows)
            .first { $0.rootViewController != nil && !$0.isHidden }
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

    private static func element(
        _ identifier: String,
        in node: NSObject,
        depth: Int
    ) -> NSObject? {
        // SwiftUI's accessibility nodes answer the identifier without
        // declaring UIAccessibilityIdentification.
        let getter = NSSelectorFromString("accessibilityIdentifier")
        if node.responds(to: getter),
           node.perform(getter)?.takeUnretainedValue() as? String == identifier {
            return node
        }
        guard depth < 64 else { return nil }
        var children: [NSObject] = []
        if let elements = node.accessibilityElements {
            children += elements.compactMap { $0 as? NSObject }
        }
        if let view = node as? UIView {
            children += view.subviews
        }
        for child in children {
            if let found = element(identifier, in: child, depth: depth + 1) {
                return found
            }
        }
        return nil
    }
}
