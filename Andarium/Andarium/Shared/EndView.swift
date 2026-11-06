// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI
import UIKit

/// An unclean end requires a fresh process before the guest can start again.
struct EndView: View {
    @Environment(AppModel.self) private var model
    @State private var copied = false

    var body: some View {
        VStack(spacing: 0) {
            Spacer(minLength: 24)
            StackMark(lit: false, width: 140, fault: true)
                .padding(.bottom, 36)
            Text("The Guest Stopped Unexpectedly")
                .font(.title2.weight(.bold))
                .multilineTextAlignment(.center)
            Text("To start it again, close Andarium and open it again.")
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 340)
                .padding(.top, 8)

            VStack(alignment: .leading, spacing: 6) {
                Text(report)
                    .font(.system(size: 12, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(14)
            .background(Color(.secondarySystemGroupedBackground), in: .rect(cornerRadius: 16))
            .frame(maxWidth: 380)
            .padding(.top, 24)
            Button {
                UIPasteboard.general.string = report
                withAnimation { copied = true }
            } label: {
                Label(copied ? "Copied" : "Copy Report", systemImage: copied ? "checkmark" : "doc.on.doc")
                    .font(.body.weight(.semibold))
                    .frame(height: 44)
                    .padding(.horizontal, 10)
            }
            .buttonStyle(.glass)
            .padding(.top, 14)

            Spacer(minLength: 24)
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: "rectangle.stack")
                    .font(.title3)
                    .foregroundStyle(.tint)
                    .frame(width: 28)
                Text("Swipe up from the bottom edge and pause to see your apps, then swipe Andarium up to close it.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(16)
            .frame(maxWidth: 380)
            .glassEffect(.regular, in: .rect(cornerRadius: 22))
            .padding(.bottom, 12)
        }
        .padding(.horizontal, 24)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(.systemGroupedBackground))
    }

    private var report: String { model.runReport }
}
