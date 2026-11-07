// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The open side keys take touches only on the keys. A touch that begins
/// anywhere else reaches the guest's display, which reports it to the session,
/// and the session closes the keys.
@MainActor
@Suite(.serialized)
struct SideKeysTouchTests {
    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    private func descendants(of view: UIView) -> [UIView] {
        view.subviews + view.subviews.flatMap { descendants(of: $0) }
    }

    // MARK: The session's rule

    @Test func closingTheKeysChangesOnlyAnOpenSet() {
        let session = SessionState()
        #expect(!session.sideKeysOpen)
        session.closeSideKeys()
        #expect(!session.sideKeysOpen)
        session.sideKeysOpen = true
        session.closeSideKeys()
        #expect(!session.sideKeysOpen)
        // Every run starts with the keys closed.
        session.sideKeysOpen = true
        #expect(!SessionState().sideKeysOpen)
    }

    // MARK: The display's report

    @Test func aTouchThatBeginsOnTheDisplayIsReportedOnce() {
        let view = GuestMetalView(frame: CGRect(x: 0, y: 0, width: 200, height: 100), device: nil)
        var reports = 0
        view.touchDidBegin = { reports += 1 }
        view.touchesBegan([UITouch()], with: nil)
        #expect(reports == 1)
        // Moving and lifting are not beginnings.
        view.touchesMoved([UITouch()], with: nil)
        view.touchesEnded([UITouch()], with: nil)
        view.touchesCancelled([UITouch()], with: nil)
        #expect(reports == 1)
    }

    // MARK: The keys over the real display

    /// The production root with a run on screen: the guest's display under the
    /// session's overlay. A point beside the open keys must reach the display
    /// (UIKit hands the touch to the display's view), and a point on a key must
    /// stay with the keys.
    @Test func theOpenKeysTakeNoTouchesBesideThem() async throws {
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        model.launchApplied = true
        model.driver = StubDriver(plan: nil)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(rootView: RootView(entry: entry).environment(model), model: model)
        lease.window.rootViewController = controller
        model.phase = .running(.normal)

        let root = controller.view!
        var display: GuestMetalView?
        #expect(await eventually {
            root.layoutIfNeeded()
            display = self.descendants(of: root).compactMap { $0 as? GuestMetalView }.first
            return display != nil && display!.bounds.width > 0
        })
        let guest = try #require(display)
        // The overlay lays out in the safe area, from its top-left corner.
        let safe = root.safeAreaInsets
        let width = root.bounds.width - safe.left - safe.right
        let height = root.bounds.height - safe.top - safe.bottom
        func window(_ x: CGFloat, _ y: CGFloat) -> CGPoint {
            root.convert(CGPoint(x: safe.left + x, y: safe.top + y), to: lease.window)
        }
        func hit(_ point: CGPoint) -> UIView? { lease.window.hitTest(point, with: nil) }
        func reachesGuest(_ point: CGPoint) -> Bool {
            guard let view = hit(point) else { return false }
            return view === guest || view.isDescendant(of: guest)
        }

        // The open group: Power's column at the edge and the guest keys beside
        // it, centred on the edge's fraction of the height, from `SideKeys`.
        let groupWidth = KeyMetrics.width * 2 + KeyMetrics.columnGap
        let centreY = min(max(118, height * 0.32), max(118, height - 118))
        let powerX = width - KeyMetrics.edgeGap - KeyMetrics.width / 2
        let settingsX = width - KeyMetrics.edgeGap - groupWidth + KeyMetrics.width / 2
        let groupTop = centreY - (KeyMetrics.keyHeight + KeyMetrics.rowGap + 2 * (KeyMetrics.keyHeight - 0.75) + 1.5) / 2
        let powerY = groupTop + KeyMetrics.keyHeight / 2
        let homeY = groupTop + KeyMetrics.keyHeight + KeyMetrics.rowGap + KeyMetrics.keyHeight * 1.5

        let beside = [
            window(width * 0.3, height * 0.5),
            window(width * 0.5, centreY),
            window(width - KeyMetrics.edgeGap - groupWidth - 40, centreY),
            window(width - 20, height * 0.9),
            window(width * 0.3, height * 0.1),
        ]

        // Closed, as the baseline, beside the keys reaches the guest.
        model.session.sideKeysOpen = false
        try await Task.sleep(for: .milliseconds(600))
        root.layoutIfNeeded()
        for point in beside { #expect(reachesGuest(point), "closed: \(point)") }

        model.session.sideKeysOpen = true
        try await Task.sleep(for: .milliseconds(800))
        root.layoutIfNeeded()
        for point in beside { #expect(reachesGuest(point), "open: \(point)") }
        // On a key the touch stays with the keys.
        for (name, point) in [("Power", window(powerX, powerY)), ("Settings", window(settingsX, powerY)),
                              ("Home", window(settingsX, homeY)), ("Volume", window(powerX, homeY))] {
            #expect(!reachesGuest(point), "\(name) at \(point) reached the guest")
        }

        // A touch beginning on the display closes the keys, as the display's
        // report does.
        guest.touchesBegan([UITouch()], with: nil)
        #expect(await eventually { !model.session.sideKeysOpen })
        try await Task.sleep(for: .milliseconds(600))
        root.layoutIfNeeded()
        for point in beside { #expect(reachesGuest(point), "closed again: \(point)") }
    }
}
