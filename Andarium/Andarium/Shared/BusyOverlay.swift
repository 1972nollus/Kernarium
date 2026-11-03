import SwiftUI

/// Covers the screens while a long task runs, so nothing else is asked of the
/// guest's files meanwhile: a glass card with what is happening and how far.
struct BusyOverlay: View {
    let state: BusyState

    var body: some View {
        ZStack {
            Color.black.opacity(0.32).ignoresSafeArea()
            VStack(alignment: .leading, spacing: 14) {
                HStack(alignment: .firstTextBaseline) {
                    Text(state.title)
                        .font(.title3.weight(.bold))
                    Spacer()
                    if let progress = state.progress {
                        Text("\(Int((progress * 100).rounded()))%")
                            .font(.system(.body, design: .rounded).weight(.semibold))
                            .monospacedDigit()
                            .foregroundStyle(.secondary)
                            .contentTransition(.numericText())
                    }
                }
                ProgressBar(value: state.progress ?? 0)
                Text(state.detail)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if let cancel = state.cancel {
                    Button("Cancel", role: .cancel, action: cancel.action)
                        .buttonStyle(.bordered)
                        .frame(maxWidth: .infinity, alignment: .trailing)
                        .transition(.opacity)
                }
            }
            .padding(20)
            .frame(maxWidth: 360)
            .glassEffect(.regular, in: .rect(cornerRadius: 26))
            .padding(24)
        }
        .transition(.opacity)
        .animation(.easeInOut(duration: 0.2), value: state.cancel != nil)
        .accessibilityElement(children: .contain)
    }
}
