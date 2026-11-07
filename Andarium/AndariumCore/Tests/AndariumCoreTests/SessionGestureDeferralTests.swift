// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import Testing
@testable import AndariumCore

struct SessionGestureDeferralTests {
    private let screen = CGSize(width: 834, height: 1210)

    private func deferral(
        _ phase: Phase = .running(.normal), size: CGSize? = nil, insets: ScreenEdges = [.bottom]
    ) -> ScreenEdges? {
        SessionGestureDeferral(phase: phase, sceneSize: size ?? screen, screenSize: screen, insetEdges: insets)
            .deferredEdges
    }

    @Test func aFullScreenIPadRunDefersTheEdgesTheGuestMeets() {
        // Status bar hidden: the guest meets the top and both sides; the home
        // indicator's inset keeps it off the bottom.
        #expect(deferral() == [.top, .left, .right])
    }

    @Test func theBottomStaysTheSystemsEvenWhereTheGuestMeetsIt() {
        #expect(deferral(insets: []) == [.top, .left, .right])
    }

    @Test func anInsetEdgeIsNotDeferred() {
        // iPhone: the sensor housing and the home indicator inset the guest.
        #expect(deferral(insets: [.top, .bottom]) == [.left, .right])
    }

    @Test func screensOtherThanANormalRunKeepTheirOwnPreference() {
        for phase in [Phase.home, .booting(.normal), .booting(.recovery), .running(.recovery), .failed] {
            #expect(deferral(phase) == nil)
        }
    }

    @Test func aWindowKeepsItsOwnPreference() {
        #expect(deferral(size: CGSize(width: 600, height: 900)) == nil)
    }

    @Test func edgesShareUIRectEdgeRawValues() {
        // UIRectEdge: top 1, left 2, bottom 4, right 8.
        #expect(ScreenEdges.top.rawValue == 1)
        #expect(ScreenEdges.left.rawValue == 2)
        #expect(ScreenEdges.bottom.rawValue == 4)
        #expect(ScreenEdges.right.rawValue == 8)
    }
}
