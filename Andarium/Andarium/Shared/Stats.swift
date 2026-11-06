import AndariumCore
import SwiftUI

// MARK: - Stats

/// The figures a screen shows. A figure not yet known is `nil` and shows a
/// dash.
struct Readout {
    var cpu: Int?
    /// The guest's own memory in use, in bytes.
    var memoryUsedBytes: UInt64?
    var fps: Int?
    var history: [Int] = []
    private var phase = 0

    init() {}

    /// What the runtime's readings give.
    init(_ meter: StatsMeter) {
        cpu = meter.cpuPercent
        memoryUsedBytes = meter.memoryInUseBytes
        fps = meter.framesPerSecond
        history = meter.history
    }

    /// The preview's mock figures.
    mutating func tick(memoryMiB limit: Int) {
        phase += 1
        let wave = [0, 6, -4, 12, 3, -7, 9, -2]
        let load = max(4, min(97, 31 + wave[phase % wave.count]))
        cpu = load
        memoryUsedBytes = UInt64(min(limit - 120, 1380 + (phase * 7) % 160)) * Limits.bytesPerMiB
        fps = phase % 5 == 0 ? 58 : 60
        history = Array(((history.isEmpty ? [28, 31, 26, 40, 35, 30, 33, 29, 36, 34] : history) + [load]).suffix(10))
    }

    /// What a figure shows before the first reading.
    static let unknown = "\u{2013}"

    var cpuText: String { cpu.map { "\($0)%" } ?? Self.unknown }
    var fpsText: String { fps.map { "\($0) fps" } ?? Self.unknown }

    /// "818 MiB of 2 GiB", with the guest's size. Every size goes through
    /// the one formatter.
    func memoryText(of memoryMiB: Int) -> String {
        memoryUsedBytes.map { "\(Formats.size(bytes: $0)) of \(Formats.size(mib: memoryMiB))" } ?? Self.unknown
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
                metric("CPU", readout.cpuText)
            }
            metric("Memory", readout.memoryText(of: memoryMiB))
            if !recovery {
                metric("Frame rate", readout.fpsText)
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

/// The pill over the session, and the reason the readings run while it
/// shows: the model reads the statistics only while some view shows them.
struct StatsOverlay: View {
    @Environment(AppModel.self) private var model
    let memoryMiB: Int
    var recovery = false

    var body: some View {
        StatsPill(readout: model.session.readout, memoryMiB: memoryMiB, recovery: recovery)
            .onAppear { model.statistics(.pill, visible: true) }
            .onDisappear { model.statistics(.pill, visible: false) }
    }
}

/// The last CPU values as bars, 100% filling the frame.
struct Sparkline: View {
    let values: [Int]

    var body: some View {
        GeometryReader { geo in
            HStack(alignment: .bottom, spacing: 1.5) {
                ForEach(Array(values.enumerated()), id: \.offset) { _, v in
                    Capsule()
                        .fill(Color.accentColor)
                        .frame(height: max(2, geo.size.height * CGFloat(v) / 100))
                }
            }
            .frame(maxHeight: .infinity, alignment: .bottom)
        }
    }
}
