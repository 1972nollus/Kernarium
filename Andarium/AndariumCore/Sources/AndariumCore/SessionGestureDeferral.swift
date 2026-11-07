// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Screen edges, with UIKit's `UIRectEdge` raw values so the application
/// converts one into the other with `init(rawValue:)`.
public struct ScreenEdges: OptionSet, Hashable, Sendable {
    public let rawValue: UInt

    public init(rawValue: UInt) { self.rawValue = rawValue }

    public static let top = ScreenEdges(rawValue: 1 << 0)
    public static let left = ScreenEdges(rawValue: 1 << 1)
    public static let bottom = ScreenEdges(rawValue: 1 << 2)
    public static let right = ScreenEdges(rawValue: 1 << 3)
    public static let all: ScreenEdges = [.top, .left, .bottom, .right]
}

/// The screen edges whose system gestures a running guest defers.
///
/// On a deferred edge the system gives the first swipe to the application and
/// shows its indicator; a second swipe while the indicator shows reaches the
/// system. A swipe can reach Android only where the guest's display meets the
/// screen edge, which is where the scene fills the screen and the safe area
/// leaves no inset, so only those edges are deferred. The bottom edge stays
/// the system's, so one swipe still leaves Andarium: Android's 3-button bar is
/// tapped, not swiped. Outside a normal run the session takes no part and the
/// screen keeps its own preference.
public struct SessionGestureDeferral: Equatable, Sendable {
    /// The edges to defer, or nil where the session does not decide.
    public let deferredEdges: ScreenEdges?

    /// - Parameter insetEdges: the edges where the safe area insets the
    ///   guest's display from the screen edge.
    public init(phase: Phase, sceneSize: CGSize, screenSize: CGSize, insetEdges: ScreenEdges) {
        let fullScreen = abs(sceneSize.width - screenSize.width) < 0.5
            && abs(sceneSize.height - screenSize.height) < 0.5
        guard case .running(.normal) = phase, fullScreen else {
            deferredEdges = nil
            return
        }
        deferredEdges = ScreenEdges.all.subtracting(insetEdges).intersection([.top, .left, .right])
    }
}
