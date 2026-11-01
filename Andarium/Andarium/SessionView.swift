import AndariumCore
import SwiftUI
import TipKit
import UIKit

/// A run: Android's display fills the screen, and the app keeps only a pair of
/// side keys on the edge. Tapped, the keys swell in place into Power and a
/// volume rocker, with Settings and a Back and Home pair right beside them
/// as one small group.
struct SessionView: View {
    @Environment(AppModel.self) private var model
    let mode: StartMode

    @State private var open = false
    @State private var settings = false
    @State private var powerMenu = false
    @State private var screen: GuestScreen = .home
    @State private var leading = false
    @State private var fraction: CGFloat = 0.32
    @State private var dragStart: CGPoint?
    @State private var volume = 9
    /// Android's own volume panel, shown after a volume key press.
    @State private var volumePanel = false
    @State private var volumeToken = 0
    /// Counts key presses for the click felt on each one.
    @State private var keyPresses = 0
    @State private var recovery = RecoveryState()
    @State private var prepared = false
    @Namespace private var keysSpace
    private let tip = SideKeysTip()

    var body: some View {
        let guestModel = model.guest ?? Guest.fresh(on: model.host)
        ZStack {
            Color.black.ignoresSafeArea()
            guest
                .clipped()
        }
        .overlay(alignment: .top) {
            if model.session.showStats {
                StatsPill(readout: model.session.readout, memoryMiB: guestModel.memoryMiB, recovery: mode == .recovery)
                    .transition(.move(edge: .top).combined(with: .opacity))
                    .padding(.top, 36)
                    .allowsHitTesting(false)
            }
        }
        .overlay {
            GeometryReader { geo in
                sideKeys(in: geo.size)
            }
            .ignoresSafeArea(.keyboard)
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .sensoryFeedback(.impact(weight: .light), trigger: keyPresses)
        .sheet(isPresented: $settings) {
            SessionSettingsSheet(large: model.sessionPreset.settingsLarge)
        }
        .onAppear(perform: prepare)
        .task {
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(1))
                model.session.readout.tick(memoryMiB: guestModel.memoryMiB)
            }
        }
    }

    // MARK: Guest

    @ViewBuilder
    private var guest: some View {
        ZStack {
            if mode == .recovery {
                GuestRecoveryScreen(state: $recovery, choose: chooseRecovery)
            } else {
                switch screen {
                case .home:
                    GuestHomeScreen(onSearch: { withAnimation(.easeOut(duration: 0.2)) { screen = .search } })
                case .search:
                    GuestSearchScreen(onBack: { pressBack() }, onHome: { pressHome() })
                case .setup:
                    GuestSetupScreen { withAnimation { screen = .home } }
                case .off:
                    Color.black
                }
                if volumePanel && screen != .off {
                    GuestVolumePanel(level: volume, maximum: 15)
                        .transition(.move(edge: .trailing).combined(with: .opacity))
                }
                if powerMenu {
                    GuestPowerMenu(
                        onPowerOff: { model.powerOff() },
                        onRestart: { model.restart() },
                        onDismiss: { withAnimation { powerMenu = false } }
                    )
                    .transition(.opacity)
                }
            }
        }
    }

    // MARK: Side keys

    @ViewBuilder
    private func sideKeys(in size: CGSize) -> some View {
        let edgeX: CGFloat = leading ? 0 : size.width
        let closedY = min(max(70, size.height * fraction), size.height - 70)
        let openY = min(max(118, size.height * fraction), max(118, size.height - 118))
        let inward: CGFloat = leading ? 1 : -1
        let recoveryMode = mode == .recovery
        // The group's width: Power's column, and in Android the column beside it.
        let groupWidth: CGFloat = recoveryMode ? KeyMetrics.width : KeyMetrics.width * 2 + KeyMetrics.columnGap

        ZStack {
            if open {
                // A soft shade behind the group keeps it legible over any
                // screen; tapping it closes the keys.
                RadialGradient(
                    colors: [.black.opacity(0.5), .black.opacity(0.16), .black.opacity(0)],
                    center: UnitPoint(x: edgeX / max(size.width, 1), y: openY / max(size.height, 1)),
                    startRadius: 20, endRadius: 300
                )
                .contentShape(Rectangle())
                .onTapGesture { setOpen(false) }
                .transition(.opacity)

                GlassEffectContainer(spacing: 4) {
                    HStack(alignment: .top, spacing: KeyMetrics.columnGap) {
                        if leading {
                            powerColumn
                            if !recoveryMode { guestColumn }
                        } else {
                            if !recoveryMode { guestColumn }
                            powerColumn
                        }
                    }
                }
                .position(x: edgeX + inward * (KeyMetrics.edgeGap + groupWidth / 2), y: openY)
            } else {
                ClosedKeys(space: keysSpace, leading: leading)
                    .popoverTip(tip, arrowEdge: leading ? .leading : .trailing)
                    .position(x: edgeX + inward * 18, y: closedY)
                    .gesture(dragKeys(size: size))
                    .onTapGesture { setOpen(true) }
            }
        }
    }

    private var powerColumn: some View {
        OpenKeys(
            space: keysSpace, recovery: mode == .recovery,
            lit: screen != .off,
            power: pressPower, powerHold: holdPower,
            volumeUp: { pressVolume(up: true) }, volumeDown: { pressVolume(up: false) }
        )
    }

    private var guestColumn: some View {
        GuestKeys(
            settings: openSettings,
            back: pressBack,
            home: pressHome
        )
        .transition(
            .asymmetric(
                insertion: .scale(scale: 0.6, anchor: leading ? .leading : .trailing)
                    .combined(with: .opacity)
                    .animation(.spring(duration: 0.34, bounce: 0.25).delay(0.04)),
                removal: .opacity.animation(.easeOut(duration: 0.12))
            )
        )
    }

    private func dragKeys(size: CGSize) -> some Gesture {
        DragGesture(minimumDistance: 6, coordinateSpace: .local)
            .onChanged { value in
                if dragStart == nil {
                    dragStart = CGPoint(x: leading ? 0 : size.width, y: size.height * fraction)
                }
                guard let start = dragStart else { return }
                let y = start.y + value.translation.height
                fraction = min(max(0.1, y / size.height), 0.9)
            }
            .onEnded { value in
                // Dragged across the middle: the keys move to the other edge.
                let x = value.location.x
                if leading && x > size.width * 0.6 {
                    withAnimation(.snappy) { leading = false }
                } else if !leading && x < size.width * 0.4 {
                    withAnimation(.snappy) { leading = true }
                }
                dragStart = nil
            }
    }

    private func setOpen(_ value: Bool) {
        withAnimation(.spring(duration: 0.36, bounce: 0.22)) { open = value }
        if value { tip.invalidate(reason: .actionPerformed) }
    }

    // MARK: Keys

    private func pressPower() {
        keyPresses += 1
        if mode == .recovery {
            chooseRecovery(recovery.highlighted)
            return
        }
        withAnimation(.easeInOut(duration: 0.25)) {
            powerMenu = false
            volumePanel = false
            screen = screen == .off ? .home : .off
        }
    }

    private func holdPower() {
        guard mode == .normal else { return }
        if screen == .off { screen = .home }
        withAnimation(.snappy) {
            powerMenu = true
            open = false
        }
    }

    private func pressVolume(up: Bool) {
        keyPresses += 1
        if mode == .recovery {
            let count = recovery.items.count
            recovery.highlighted = (recovery.highlighted + (up ? count - 1 : 1)) % count
            return
        }
        // Android answers with its own volume panel, which it hides again
        // after a moment; while its screen is off, it changes the volume
        // without one.
        withAnimation(.snappy) {
            volume = min(15, max(0, volume + (up ? 1 : -1)))
            if screen != .off { volumePanel = true }
        }
        volumeToken += 1
        let token = volumeToken
        Task {
            try? await Task.sleep(for: .seconds(2.5))
            if token == volumeToken { withAnimation(.easeOut(duration: 0.25)) { volumePanel = false } }
        }
    }

    /// Android's Back: closes what is on top, as Android would. On the home
    /// screen Android does nothing, and neither does Andarium.
    private func pressBack() {
        keyPresses += 1
        guard screen != .off else { return }
        withAnimation(.easeOut(duration: 0.2)) {
            if volumePanel {
                volumePanel = false
            } else if powerMenu {
                powerMenu = false
            } else if screen == .search {
                screen = .home
            }
        }
    }

    /// Android's Home: back to the home screen from anywhere but setup.
    private func pressHome() {
        keyPresses += 1
        guard screen != .off else { return }
        withAnimation(.easeOut(duration: 0.2)) {
            volumePanel = false
            powerMenu = false
            if screen == .search { screen = .home }
        }
    }

    private func openSettings() {
        setOpen(false)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { settings = true }
    }

    private func chooseRecovery(_ index: Int) {
        let items = recovery.items
        guard items.indices.contains(index) else { return }
        recovery.highlighted = index
        switch items[index].0 {
        case "Reboot system now": model.restart()
        case "Power off": model.powerOff()
        case "Factory reset": go(.factoryReset)
        case "Apply update": go(.update)
        case "Advanced": go(.advanced)
        case "Format data/factory reset": go(.confirm)
        case "Format data":
            model.factoryReset()
            go(.wiped)
        case "Back", "Cancel": go(.main)
        default: break
        }
    }

    private func go(_ page: RecoveryState.Page) {
        recovery.page = page
        recovery.highlighted = 0
    }

    private func prepare() {
        guard !prepared else { return }
        prepared = true
        let preset = model.sessionPreset
        screen = preset.guestScreen
        leading = preset.leftEdge
        open = preset.keysOpen
        volumePanel = preset.volumePanel
        powerMenu = preset.powerMenu
        if preset.settings {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.45) { settings = true }
        }
    }
}

// MARK: - Closed and open keys

enum KeyMetrics {
    static let width: CGFloat = 52
    static let keyHeight: CGFloat = 62
    static let rowGap: CGFloat = 10
    static let columnGap: CGFloat = 8
    static let edgeGap: CGFloat = 8
}

/// The closed keys: Power and a longer volume rocker, each a plain glass key
/// on the screen edge. They show nothing about Android's state.
struct ClosedKeys: View {
    let space: Namespace.ID
    let leading: Bool

    var body: some View {
        VStack(spacing: 6) {
            key(height: 30)
                .matchedGeometryEffect(id: "power", in: space)
            key(height: 58)
                .matchedGeometryEffect(id: "rocker", in: space)
        }
        .padding(leading ? .leading : .trailing, 3)
        .frame(width: 36, height: 124, alignment: leading ? .leading : .trailing)
        .contentShape(Rectangle())
        .accessibilityElement()
        .accessibilityLabel("Side keys")
        .accessibilityHint("Opens Power, volume, Back, Home and Settings")
        .accessibilityAddTraits(.isButton)
    }

    private func key(height: CGFloat) -> some View {
        Capsule()
            .fill(.white.opacity(0.9))
            .frame(width: 9, height: height)
            .glassEffect(.regular, in: .capsule)
            .shadow(color: .black.opacity(0.35), radius: 3, x: leading ? 1 : -1)
    }
}

/// The open keys, swollen in place: Power (tap to press, hold for Android's
/// power menu) above the volume rocker.
struct OpenKeys: View {
    let space: Namespace.ID
    let recovery: Bool
    let lit: Bool
    let power: () -> Void
    let powerHold: () -> Void
    let volumeUp: () -> Void
    let volumeDown: () -> Void

    var body: some View {
        VStack(spacing: KeyMetrics.rowGap) {
            ZStack {
                Capsule().fill(.clear)
                VStack(spacing: 3) {
                    Image(systemName: "power")
                        .font(.system(size: 20, weight: .semibold))
                    Circle()
                        .fill(lit ? Color.accentColor : Color.gray.opacity(0.5))
                        .frame(width: 5, height: 5)
                        .shadow(color: lit ? Color.accentColor : .clear, radius: 3)
                }
            }
            .frame(width: KeyMetrics.width, height: KeyMetrics.keyHeight)
            .glassEffect(.regular.interactive(), in: .capsule)
            .matchedGeometryEffect(id: "power", in: space)
            .contentShape(.capsule)
            .onTapGesture(perform: power)
            .onLongPressGesture(minimumDuration: 0.6, perform: powerHold)
            .accessibilityElement()
            .accessibilityLabel("Power")
            .accessibilityHint(recovery ? "Chooses the highlighted item" : "Hold for Android's power menu")
            .accessibilityAddTraits(.isButton)
            .accessibilityAction(named: "Power menu", powerHold)

            KeyPair(
                top: (recovery ? "chevron.up" : "plus", recovery ? "Up" : "Volume up", volumeUp),
                bottom: (recovery ? "chevron.down" : "minus", recovery ? "Down" : "Volume down", volumeDown)
            )
            .matchedGeometryEffect(id: "rocker", in: space)
        }
        .foregroundStyle(.primary)
    }
}

/// The keys Android has and an iPhone does not, beside Power and volume:
/// Settings across from Power, and Back above Home across from the rocker.
struct GuestKeys: View {
    let settings: () -> Void
    let back: () -> Void
    let home: () -> Void

    var body: some View {
        VStack(spacing: KeyMetrics.rowGap) {
            Button(action: settings) {
                Image(systemName: "gearshape")
                    .font(.system(size: 20, weight: .semibold))
                    .frame(width: KeyMetrics.width, height: KeyMetrics.keyHeight)
                    .contentShape(.capsule)
            }
            .buttonStyle(.plain)
            .glassEffect(.regular.interactive(), in: .capsule)
            .accessibilityLabel("Settings")
            .accessibilityHint("Install apps, ADB, stats and options")

            KeyPair(
                top: ("arrowtriangle.backward.fill", "Back", back),
                bottom: ("circle.fill", "Home", home),
                glyphSize: 16
            )
        }
        .foregroundStyle(.primary)
    }
}

/// Two keys in one glass capsule, like a volume rocker.
struct KeyPair: View {
    let top: (symbol: String, label: String, action: () -> Void)
    let bottom: (symbol: String, label: String, action: () -> Void)
    var glyphSize: CGFloat = 19

    var body: some View {
        VStack(spacing: 0) {
            key(top)
            Capsule()
                .fill(.primary.opacity(0.18))
                .frame(width: 22, height: 1.5)
            key(bottom)
        }
        .buttonStyle(.plain)
        .glassEffect(.regular.interactive(), in: .capsule)
    }

    private func key(_ key: (symbol: String, label: String, action: () -> Void)) -> some View {
        Button(action: key.action) {
            Image(systemName: key.symbol)
                .font(.system(size: glyphSize, weight: .semibold))
                .frame(width: KeyMetrics.width, height: KeyMetrics.keyHeight - 0.75)
                .contentShape(Rectangle())
        }
        .accessibilityLabel(key.label)
    }
}

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
            metric("Memory", String(format: "%.1f of %@", Double(readout.memoryMiB) / 1024, Formats.memory(memoryMiB)))
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

// MARK: - Tip

/// Shown once Android has booted, pointing at the side keys. It stops once the
/// keys have been opened or the tip closed, and appears at most three times.
nonisolated struct SideKeysTip: Tip {
    var title: Text { Text("Side Keys") }
    var message: Text? { Text("Tap for Back, Home and Settings. Hold Power for Android's power menu. Drag to move.") }
    var image: Image? { Image(systemName: "hand.tap") }
    var options: [any TipOption] { [MaxDisplayCount(3)] }
}
