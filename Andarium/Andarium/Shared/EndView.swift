import SwiftUI
import UIKit

/// The end of a run. Android starts once per launch of Andarium, so all this
/// screen can do is say what happened and how to start again.
struct EndView: View {
    @Environment(AppModel.self) private var model
    let failure: Bool
    @State private var copied = false

    var body: some View {
        VStack(spacing: 0) {
            Spacer(minLength: 24)
            StackMark(lit: false, width: 140, fault: failure)
                .padding(.bottom, 36)
            Text(failure ? "The Guest Stopped Unexpectedly" : "The Guest Is Off")
                .font(.title2.weight(.bold))
                .multilineTextAlignment(.center)
            Text(failure
                 ? "Nothing in the guest was lost. To start it again, close Andarium and open it again."
                 : "To start it again, close Andarium and open it again.")
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 340)
                .padding(.top, 8)

            if failure {
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
            }

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
