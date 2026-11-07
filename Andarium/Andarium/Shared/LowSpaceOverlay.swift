// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Covers the session while the guest is paused because the device is nearly
/// out of space: what is the matter, what to do, and the two ways on. Resume
/// reads the space again; Shut down has Android write what it has and power
/// off.
struct LowSpaceOverlay: View {
    let pause: SpacePause
    let resume: () -> Void
    let shutDown: () -> Void

    var body: some View {
        ZStack {
            Color.black.opacity(0.32).ignoresSafeArea()
            VStack(alignment: .leading, spacing: 14) {
                HStack(alignment: .firstTextBaseline, spacing: 10) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .font(.title3)
                        .foregroundStyle(Color.orange)
                        .accessibilityHidden(true)
                    Text(SpacePause.title)
                        .font(.title3.weight(.bold))
                }
                Text(pause.detail)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("space-pause-detail")
                buttons
            }
            .padding(20)
            .frame(maxWidth: 380)
            .glassEffect(.regular, in: .rect(cornerRadius: 26))
            .padding(24)
        }
        .transition(.opacity)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("space-pause")
    }

    @ViewBuilder
    private var buttons: some View {
        if let working = pause.working {
            HStack(spacing: 10) {
                ProgressView()
                Text(working == .resuming ? "Resuming\u{2026}" : "Shutting down\u{2026}")
                    .font(.subheadline.weight(.semibold))
            }
            .frame(maxWidth: .infinity, alignment: .trailing)
        } else {
            HStack(spacing: 10) {
                Spacer()
                Button("Shut down", action: shutDown)
                    .buttonStyle(.bordered)
                    .accessibilityIdentifier("space-pause-shutdown")
                Button("Resume", action: resume)
                    .buttonStyle(.borderedProminent)
                    .accessibilityIdentifier("space-pause-resume")
            }
        }
    }
}
