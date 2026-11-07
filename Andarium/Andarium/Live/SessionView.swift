// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// A run: Android's own display fills the screen, and Andarium keeps only
/// the side keys on its edge. The keys are Android's: Power and volume, Back
/// and Home, as the keyboard events a phone's hardware keys would raise.
struct SessionView: View {
    @Environment(AppModel.self) private var model
    let mode: StartMode
    @State private var settings = false

    var body: some View {
        Color.clear
            .overlay(alignment: .top) {
                if model.session.showStats {
                    StatsOverlay(memoryMiB: model.guest?.memoryMiB ?? Limits.defaultMemoryRangeMiB.lowerBound, recovery: mode == .recovery)
                        .transition(.move(edge: .top).combined(with: .opacity))
                        .padding(.top, 36)
                        .allowsHitTesting(false)
                }
            }
            .overlay {
                SideKeysOverlay(mode: mode, lit: true, actions: actions)
            }
            .statusBarHidden(true)
            // The system honours the bottom edge's deferral only while the
            // home indicator is visible: with it hidden, one swipe goes Home.
            .persistentSystemOverlays(.visible)
            .sheet(isPresented: $settings) {
                SessionSettingsSheet(large: false)
                    .environment(model)
            }
    }

    private var runtime: LiveRuntime? { model.driver as? LiveRuntime }

    private var actions: SideKeyActions {
        SideKeyActions(
            power: { runtime?.press(.power) },
            powerHold: { runtime?.hold(.power) },
            volumeUp: { runtime?.press(.volumeUp) },
            volumeDown: { runtime?.press(.volumeDown) },
            back: { runtime?.press(.back) },
            home: { runtime?.press(.home) },
            settings: { settings = true }
        )
    }
}
