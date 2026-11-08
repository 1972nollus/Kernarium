// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI

/// Every stats design on trial, one under another over the game, each drawn
/// twice: with the session's figures and with the widest the formatter
/// writes. A caption states both widths in points, and the widths also go to
/// `stats-widths.json` in the caches directory for the capture to read.
struct StatsDesignGallery: View {
    let memoryMiB: Int
    let maximumMemoryMiB: Int
    @State private var widths: [String: Double] = [:]

    var body: some View {
        let typical = Readout(previewReadings: StatsMeter.historyLength, memoryMiB: memoryMiB)
        VStack(spacing: 9) {
            ForEach(StatsDesign.allCases) { design in
                VStack(spacing: 4) {
                    measured(design, typical, memoryMiB: memoryMiB, key: "\(design.rawValue).typical")
                    measured(design, Self.widest(memoryMiB: maximumMemoryMiB), memoryMiB: maximumMemoryMiB, key: "\(design.rawValue).widest")
                    caption(design)
                }
            }
            Text("VoiceOver: \u{201C}\(typical.spoken(memoryMiB: memoryMiB, recovery: false))\u{201D}")
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(.white)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background(.black.opacity(0.6), in: .rect(cornerRadius: 10))
                .padding(.horizontal, 20)
        }
        .padding(.top, 8)
        .onChange(of: widths) { _, widths in write(widths) }
    }

    /// The widest figures a guest of the device's largest memory can show:
    /// four-digit MiB figures, units that differ, a three-digit frame rate.
    static func widest(memoryMiB: Int) -> Readout {
        var readout = Readout(previewReadings: StatsMeter.historyLength, memoryMiB: memoryMiB)
        readout.cpu = 100
        readout.memoryUsedBytes = 1004 * Limits.bytesPerMiB
        readout.vramUsedBytes = 1004 * Limits.bytesPerMiB
        readout.vramBudgetBytes = 1925 * Limits.bytesPerMiB
        readout.fps = 120
        return readout
    }

    private func measured(_ design: StatsDesign, _ readout: Readout, memoryMiB: Int, key: String) -> some View {
        StatsPill(readout: readout, memoryMiB: memoryMiB, design: design)
            .onGeometryChange(for: Double.self) { $0.size.width } action: { widths[key] = $0 }
    }

    private func caption(_ design: StatsDesign) -> some View {
        let typical = widths["\(design.rawValue).typical"].map { "\(Int($0.rounded())) pt" } ?? "\u{2013}"
        let widest = widths["\(design.rawValue).widest"].map { "\(Int($0.rounded())) pt" } ?? "\u{2013}"
        return Text("\(design.rawValue) \u{00B7} \(design.name) \u{00B7} \(typical), widest \(widest)")
            .font(.system(size: 11, weight: .semibold))
            .monospacedDigit()
            .foregroundStyle(.white)
            .padding(.horizontal, 8)
            .padding(.vertical, 2)
            .background(.black.opacity(0.6), in: .capsule)
    }

    private func write(_ widths: [String: Double]) {
        guard let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first,
              let data = try? JSONSerialization.data(withJSONObject: widths, options: [.sortedKeys]) else { return }
        try? data.write(to: caches.appendingPathComponent("stats-widths.json"), options: .atomic)
    }
}
