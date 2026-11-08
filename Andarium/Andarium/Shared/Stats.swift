// SPDX-License-Identifier: GPL-2.0-or-later

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
    /// The guest's graphics memory in use and the budget the host gives it
    /// for this launch, in bytes. A budget of zero is no budget, and leaves
    /// both figures unknown.
    var vramUsedBytes: UInt64?
    var vramBudgetBytes: UInt64?
    var history: [Int] = []
    private var phase = 0
    /// The preview's capture holds the history as it was given.
    private var holds = false

    init() {}

    /// What the runtime's readings give.
    init(_ meter: StatsMeter) {
        cpu = meter.cpuPercent
        memoryUsedBytes = meter.memoryInUseBytes
        fps = meter.framesPerSecond
        history = meter.history
    }

    /// The mock readings a seeded history starts from, oldest first.
    private static let seedHistory = [28, 31, 26, 40, 35, 30, 33, 29, 36, 34]
    /// The mock graphics budget, as Andarium gives it on an iPhone.
    private static let previewVramBudgetMiB: UInt64 = 534

    /// A session that has `count` readings so far, for the preview's stats
    /// captures: 0 is a session before its first reading, every figure
    /// still a dash. The readout then holds still, so a capture shows
    /// exactly that history.
    init(previewReadings count: Int, memoryMiB limit: Int) {
        history = Array(Self.seedHistory.suffix(count))
        holds = true
        guard count > 0 else { return }
        cpu = history.last
        memoryUsedBytes = UInt64(min(limit - 120, 1380)) * Limits.bytesPerMiB
        fps = 60
        vramUsedBytes = 412 * Limits.bytesPerMiB
        vramBudgetBytes = Self.previewVramBudgetMiB * Limits.bytesPerMiB
    }

    /// The preview's mock figures.
    mutating func tick(memoryMiB limit: Int) {
        guard !holds else { return }
        phase += 1
        let wave = [0, 6, -4, 12, 3, -7, 9, -2]
        let load = max(4, min(97, 31 + wave[phase % wave.count]))
        cpu = load
        memoryUsedBytes = UInt64(min(limit - 120, 1380 + (phase * 7) % 160)) * Limits.bytesPerMiB
        fps = phase % 5 == 0 ? 58 : 60
        vramUsedBytes = UInt64(412 + wave[(phase + 3) % wave.count]) * Limits.bytesPerMiB
        vramBudgetBytes = Self.previewVramBudgetMiB * Limits.bytesPerMiB
        history = Array(((history.isEmpty ? Self.seedHistory : history) + [load]).suffix(StatsMeter.historyLength))
    }

    /// What a figure shows before the first reading.
    static let unknown = "\u{2013}"
    /// What VoiceOver says for it.
    static let unknownSpoken = "no reading yet"

    var cpuText: String { cpu.map { "\($0)%" } ?? Self.unknown }
    var fpsText: String { fps.map { "\($0) fps" } ?? Self.unknown }
    /// The frame rate under a title that states the unit.
    var fpsNumber: String { fps.map { "\($0)" } ?? Self.unknown }

    /// "818 MiB of 2 GiB", with the guest's size. Every size goes through
    /// the one formatter.
    func memoryText(of memoryMiB: Int) -> String {
        memoryUsedBytes.map { "\(Formats.size(bytes: $0)) of \(Formats.size(mib: memoryMiB))" } ?? Self.unknown
    }

    /// "1.35/2.5 GiB": the memory figure where room is short.
    func memoryPair(of memoryMiB: Int) -> String {
        memoryUsedBytes.map { Formats.size(bytes: $0, ofBytes: Self.bytes(mib: memoryMiB)) } ?? Self.unknown
    }

    /// The memory in use alone, for a view that draws the share.
    var memoryUsedText: String { memoryUsedBytes.map { Formats.size(bytes: $0) } ?? Self.unknown }

    /// "1.15 GiB free"; nil before the first reading.
    func memoryFreeText(of memoryMiB: Int) -> String? {
        memoryUsedBytes.map { Self.free($0, of: Self.bytes(mib: memoryMiB)) }
    }

    /// The share of the guest's memory in use, from 0 to 1.
    func memoryFraction(of memoryMiB: Int) -> Double? {
        memoryUsedBytes.map { Self.fraction($0, of: Self.bytes(mib: memoryMiB)) }
    }

    /// The graphics figures, or nil while either is unknown or there is no
    /// budget.
    private var vram: (used: UInt64, budget: UInt64)? {
        guard let used = vramUsedBytes, let budget = vramBudgetBytes, budget > 0 else { return nil }
        return (used, budget)
    }

    /// "412 MiB of 534 MiB".
    var vramText: String {
        vram.map { "\(Formats.size(bytes: $0.used)) of \(Formats.size(bytes: $0.budget))" } ?? Self.unknown
    }
    /// "412/534 MiB".
    var vramPair: String { vram.map { Formats.size(bytes: $0.used, ofBytes: $0.budget) } ?? Self.unknown }
    var vramUsedText: String { vram.map { Formats.size(bytes: $0.used) } ?? Self.unknown }
    var vramFreeText: String? { vram.map { Self.free($0.used, of: $0.budget) } }
    var vramFraction: Double? { vram.map { Self.fraction($0.used, of: $0.budget) } }

    /// What VoiceOver reads for the whole capsule: every figure in words,
    /// whatever the design abbreviates.
    func spoken(memoryMiB: Int, recovery: Bool) -> String {
        func part(_ name: String, _ value: String?) -> String { "\(name) \(value ?? Self.unknownSpoken)" }
        var parts = [
            part("CPU", cpu.map { "\($0)%" }),
            part("memory", memoryUsedBytes.map { _ in memoryText(of: memoryMiB) }),
            part("graphics memory", vram.map { _ in vramText }),
        ]
        if !recovery { parts.append(part("frame rate", fps.map { "\($0) fps" })) }
        return parts.joined(separator: ", ")
    }

    private static func bytes(mib: Int) -> UInt64 { UInt64(max(mib, 0)) * Limits.bytesPerMiB }

    private static func free(_ used: UInt64, of whole: UInt64) -> String {
        "\(Formats.size(bytes: whole > used ? whole - used : 0)) free"
    }

    private static func fraction(_ used: UInt64, of whole: UInt64) -> Double {
        whole == 0 ? 0 : min(1, Double(used) / Double(whole))
    }
}

/// The stats designs on trial; the user picks one and the others go.
enum StatsDesign: Int, CaseIterable, Identifiable {
    /// CPU, memory and frame rate, as shipped.
    case today
    /// Today's columns with compact figures: "1.35/2.5 GiB", "412/534 MiB".
    case figures
    /// The figure in use over a thin bar that fills with its share.
    case meters
    /// Memory and graphics memory as one block: two nested rings and a
    /// two-line legend.
    case rings
    /// Two lines: CPU and frame rate over memory and graphics memory.
    case twoLines

    var id: Int { rawValue }

    var name: String {
        switch self {
        case .today: "Today"
        case .figures: "Compact figures"
        case .meters: "Meters"
        case .rings: "Rings"
        case .twoLines: "Two lines"
        }
    }

    /// Whether the design shows graphics memory.
    var showsVram: Bool { self != .today }
}

/// The colours of the stats' meters.
enum StatsTint {
    /// Graphics memory where it sits beside memory in one drawing.
    static let vram = Color.indigo
    /// A share at or above this is drawn in `warning`.
    static let warningShare = 0.9
    static let warning = Color.orange

    static func fill(_ share: Double?, _ tint: Color = .accentColor) -> Color {
        (share ?? 0) >= warningShare ? warning : tint
    }
}

/// Live guest CPU, memory and frame rate in one glass capsule.
struct StatsPill: View {
    let readout: Readout
    let memoryMiB: Int
    var recovery = false
    var design = StatsDesign.today

    var body: some View {
        let pill = content
            .fixedSize()
            .padding(.horizontal, design == .figures ? 12 : design == .twoLines ? 16 : 14)
            .frame(height: design == .twoLines ? 44 : 40)
            .glassEffect(.regular, in: .capsule)
        if design == .today {
            pill.accessibilityElement(children: .combine)
        } else {
            pill
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(readout.spoken(memoryMiB: memoryMiB, recovery: recovery))
        }
    }

    @ViewBuilder private var content: some View {
        switch design {
        case .today:
            HStack(spacing: 14) {
                cpu
                metric("Memory", readout.memoryText(of: memoryMiB))
                if !recovery {
                    metric("Frame rate", readout.fpsText)
                }
            }
        case .figures:
            HStack(spacing: 10) {
                cpu
                metric("RAM", readout.memoryPair(of: memoryMiB))
                metric("VRAM", readout.vramPair)
                if !recovery { metric("FPS", readout.fpsNumber) }
            }
        case .meters:
            // The titles and figures line up; the bars hang below them.
            HStack(alignment: .top, spacing: 12) {
                cpu
                meter("RAM", readout.memoryUsedText, share: readout.memoryFraction(of: memoryMiB))
                meter("VRAM", readout.vramUsedText, share: readout.vramFraction)
                if !recovery { metric("FPS", readout.fpsNumber) }
            }
        case .rings:
            HStack(spacing: 12) {
                cpu
                HStack(spacing: 7) {
                    NestedRings(outer: readout.memoryFraction(of: memoryMiB), inner: readout.vramFraction)
                        .frame(width: 26, height: 26)
                    Grid(alignment: .leading, horizontalSpacing: 4, verticalSpacing: 0) {
                        legend("RAM", readout.memoryPair(of: memoryMiB), tint: .accentColor)
                        legend("VRAM", readout.vramPair, tint: StatsTint.vram)
                    }
                }
                if !recovery { metric("FPS", readout.fpsNumber) }
            }
        case .twoLines:
            HStack(spacing: 8) {
                Sparkline(values: readout.history)
                    .frame(width: 26, height: 24)
                Grid(alignment: .leading, horizontalSpacing: 4, verticalSpacing: 1) {
                    GridRow(alignment: .firstTextBaseline) {
                        inlineTitle("CPU")
                        inlineValue(readout.cpuText)
                        if !recovery {
                            inlineTitle("FPS").padding(.leading, 8)
                            inlineValue(readout.fpsNumber)
                        }
                    }
                    GridRow(alignment: .firstTextBaseline) {
                        inlineTitle("RAM")
                        inlineValue(readout.memoryPair(of: memoryMiB))
                        inlineTitle("VRAM").padding(.leading, 8)
                        inlineValue(readout.vramPair)
                    }
                }
            }
        }
    }

    private var cpu: some View {
        HStack(spacing: 6) {
            Sparkline(values: readout.history)
                .frame(width: design == .figures ? 26 : 30, height: 16)
            metric("CPU", readout.cpuText)
        }
    }

    private func metric(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(title)
                .font(.system(size: 9, weight: .semibold))
                .foregroundStyle(.secondary)
                .textCase(.uppercase)
            Text(value)
                // Compact figures fit four columns in today's width.
                .font(.system(size: design == .figures ? 12.5 : 13, weight: .semibold, design: .rounded))
                .monospacedDigit()
                .lineLimit(1)
        }
    }

    /// A figure over a bar as wide as the figure's column.
    private func meter(_ title: String, _ value: String, share: Double?) -> some View {
        metric(title, value)
            .frame(minWidth: 40, alignment: .leading)
            .padding(.bottom, 5)
            .overlay(alignment: .bottom) {
                ShareBar(share: share)
                    .frame(height: 3)
            }
    }

    private func legend(_ title: String, _ value: String, tint: Color) -> some View {
        GridRow(alignment: .firstTextBaseline) {
            Text(title)
                .font(.system(size: 9, weight: .bold))
                .foregroundStyle(tint)
            Text(value)
                .font(.system(size: 12, weight: .semibold, design: .rounded))
                .monospacedDigit()
                .lineLimit(1)
        }
    }

    private func inlineTitle(_ title: String) -> some View {
        Text(title)
            .font(.system(size: 9, weight: .semibold))
            .foregroundStyle(.secondary)
    }

    private func inlineValue(_ value: String) -> some View {
        Text(value)
            .font(.system(size: 12, weight: .semibold, design: .rounded))
            .monospacedDigit()
            .lineLimit(1)
    }
}

/// The pill over the session, and the reason the readings run while it
/// shows: the model reads the statistics only while some view shows them.
struct StatsOverlay: View {
    @Environment(AppModel.self) private var model
    let memoryMiB: Int
    var recovery = false

    var body: some View {
        StatsPill(readout: model.session.readout, memoryMiB: memoryMiB, recovery: recovery, design: model.statsDesign)
            .onAppear { model.statistics(.pill, visible: true) }
            .onDisappear { model.statistics(.pill, visible: false) }
    }
}

/// The last CPU values as bars, 100% filling the frame. It always draws one
/// bar per history slot, newest on the right; a slot with no reading yet is
/// a dimmed bar at the floor, so the bars keep their width as values arrive.
struct Sparkline: View {
    let values: [Int]
    var slots = StatsMeter.historyLength

    /// One entry per slot, oldest first; `nil` where no reading exists yet.
    static func slotValues(_ values: [Int], slots: Int) -> [Int?] {
        let recent = values.suffix(slots).map { Optional($0) }
        return Array(repeating: nil, count: max(0, slots - recent.count)) + recent
    }

    var body: some View {
        GeometryReader { geo in
            HStack(alignment: .bottom, spacing: 1.5) {
                ForEach(Array(Self.slotValues(values, slots: slots).enumerated()), id: \.offset) { _, v in
                    Capsule()
                        .fill(v == nil ? Color.accentColor.opacity(0.25) : Color.accentColor)
                        .frame(height: max(2, geo.size.height * CGFloat(v ?? 0) / 100))
                }
            }
            .frame(maxHeight: .infinity, alignment: .bottom)
        }
    }
}

/// A share as a bar on a dimmed track, as the sparkline dims its empty
/// slots; from `StatsTint.warningShare` it is orange. Unknown is the track
/// alone.
struct ShareBar: View {
    let share: Double?
    var tint = Color.accentColor

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(tint.opacity(0.25))
                if let share {
                    Capsule()
                        .fill(StatsTint.fill(share, tint))
                        .frame(width: max(geo.size.height, geo.size.width * share))
                }
            }
        }
        .accessibilityHidden(true)
    }
}

/// A share as a ring that fills clockwise from the top.
struct ShareRing: View {
    let share: Double?
    var tint = Color.accentColor
    var lineWidth: CGFloat = 3

    var body: some View {
        ZStack {
            Circle().stroke(tint.opacity(0.25), lineWidth: lineWidth)
            if let share {
                Circle()
                    .trim(from: 0, to: share)
                    .stroke(StatsTint.fill(share, tint), style: StrokeStyle(lineWidth: lineWidth, lineCap: .round))
                    .rotationEffect(.degrees(-90))
            }
        }
        .padding(lineWidth / 2)
        .accessibilityHidden(true)
    }
}

/// Memory on the outer ring, graphics memory on the inner.
struct NestedRings: View {
    let outer: Double?
    let inner: Double?

    var body: some View {
        ZStack {
            ShareRing(share: outer, lineWidth: 3.5)
            ShareRing(share: inner, tint: StatsTint.vram, lineWidth: 3.5)
                .padding(5)
        }
    }
}
