import AndariumCore
import SwiftUI

// MARK: - Stats

struct Readout {
    var cpu = 34
    var memoryMiB = 1420
    var fps = 60
    var history: [Int] = [28, 31, 26, 40, 35, 30, 33, 29, 36, 34]
    private var phase = 0

    mutating func tick(memoryMiB limit: Int) {
        phase += 1
        let wave = [0, 6, -4, 12, 3, -7, 9, -2]
        cpu = max(4, min(97, 31 + wave[phase % wave.count]))
        memoryMiB = min(limit - 120, 1380 + (phase * 7) % 160)
        fps = phase % 5 == 0 ? 58 : 60
        history = Array((history + [cpu]).suffix(10))
    }
}

/// Live guest CPU, memory and frame rate in one glass capsule.
struct StatsPill: View {
    let readout: Readout
    let memoryMiB: Int
    var recovery = false

    var body: some View {
        HStack(spacing: 14) {
            HStack(spacing: 6) {
                Sparkline(values: readout.history)
                    .frame(width: 30, height: 16)
                metric("CPU", "\(readout.cpu)%")
            }
            metric("Memory", "\(Formats.size(mib: readout.memoryMiB)) of \(Formats.size(mib: memoryMiB))")
            if !recovery {
                metric("Frame rate", "\(readout.fps) fps")
            }
        }
        .fixedSize()
        .padding(.horizontal, 14)
        .frame(height: 40)
        .glassEffect(.regular, in: .capsule)
        .accessibilityElement(children: .combine)
    }

    private func metric(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(title)
                .font(.system(size: 9, weight: .semibold))
                .foregroundStyle(.secondary)
                .textCase(.uppercase)
            Text(value)
                .font(.system(size: 13, weight: .semibold, design: .rounded))
                .monospacedDigit()
                .lineLimit(1)
        }
    }
}

struct Sparkline: View {
    let values: [Int]

    var body: some View {
        GeometryReader { geo in
            HStack(alignment: .bottom, spacing: 1.5) {
                ForEach(Array(values.enumerated()), id: \.offset) { _, v in
                    Capsule()
                        .fill(Color.accentColor)
                        .frame(height: max(2, geo.size.height * CGFloat(v) / 100 * 2))
                }
            }
            .frame(maxHeight: .infinity, alignment: .bottom)
        }
    }
}
