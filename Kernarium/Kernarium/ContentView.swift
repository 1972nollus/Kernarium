import Combine
import Foundation
import MetalKit
import SwiftUI

struct ContentView: View {
    @StateObject private var serialConsole = SerialConsoleController()
    @ObservedObject private var launcher = EmulatorLauncher.shared
    @ObservedObject private var backgroundSession = BackgroundSession.shared
    @ObservedObject private var backgroundJob = BackgroundJob.shared
    /// Whether the running guest chose the keep-running mode, which is what
    /// offers the button that starts its job.
    @State private var keepRunningChosen = false
    @State private var guestViewMode = GuestViewMode.defaultMode
    /// Whether the framebuffer holds the keyboard. Only the framebuffer
    /// mode sets it; the serial console's keyboard is its terminal's focus.
    @State private var framebufferKeyboardVisible = false
    @State private var specialKeysExpanded = false
    @State private var presentedSheet: PresentedSheet?
    /// Whether the window scene fills the screen. Until the view is in a
    /// scene, it is taken to be a window, which is the system's own default.
    @State private var sceneCoversScreen = false
    @Namespace private var floatingButtonNamespace

    var body: some View {
        ZStack(alignment: .trailing) {
            ZStack {
                GuestPictureInPictureAnchor()
                    .allowsHitTesting(false)
                    .accessibilityHidden(true)

                GuestDisplayView(
                    keyboardVisible: $framebufferKeyboardVisible,
                    retinaMode: EmulatorLauncher.shared.retinaMode
                ) { view, geometry in
                    EmulatorLauncher.shared.start(view: view, geometry: geometry)
                }
                .opacity(guestViewMode == .framebuffer ? 1 : 0)
                .allowsHitTesting(guestViewMode == .framebuffer)
                .accessibilityHidden(guestViewMode != .framebuffer)
                .accessibilityIdentifier("framebufferView")

                SerialConsoleView(
                    controller: serialConsole,
                    isVisible: guestViewMode == .serialConsole
                )
                    .opacity(guestViewMode == .serialConsole ? 1 : 0)
                    .allowsHitTesting(guestViewMode == .serialConsole)
                    .accessibilityHidden(guestViewMode != .serialConsole)

                if case let .ended(result) = launcher.phase {
                    GuestEndedNotice(end: GuestRunEnd(result: result))
                        .padding()
                } else if let restart = backgroundSession.restart {
                    GuestRestartedNotice(restart: restart) {
                        backgroundSession.dismissRestartNotice()
                    }
                    .padding()
                    .frame(maxHeight: .infinity, alignment: .top)
                }
            }
            // In a window, iPadOS draws its window controls over the top-left
            // corner of the content, which would cover the guest's own top-left
            // (Android's status bar). Pushing the guest below them is the
            // system's job: it knows where the controls are for the window's
            // placement. A scene that covers the whole screen has none over
            // its content, and the system's corner offset can outlive the
            // window it described, so the offset applies only to a scene
            // smaller than the screen.
            .containerCornerOffset(
                SceneCoverage.cornerOffsetEdges(coversScreen: sceneCoversScreen),
                sizeToFit: true
            )
            floatingButtons
        }
        .ignoresSafeArea(.keyboard)
        .background {
            SceneCoverageReader { sceneCoversScreen = $0 }
        }
        .sheet(item: $presentedSheet) { destination in
            switch destination {
            case .settings:
                EmulatorSettingsView()
            }
        }
        .onAppear {
            serialConsole.startObserving()
            let window = GuestPictureInPicture.shared
            window.wanted = {
                EmulatorLauncher.shared.runningGuestName.map {
                    BackgroundSettings().mode(forGuest: $0)
                } == .pictureInPicture
            }
            GuestPersistence.shared.startObserving(
                guest: { EmulatorLauncher.shared.runningGuestName },
                pictureInPicture: window
            )
        }
        .onDisappear {
            serialConsole.stopObserving()
            GuestPersistence.shared.stopObserving()
            GuestPictureInPicture.shared.guestRunningChanged(false)
            EmulatorLauncher.shared.stop()
        }
        .onReceive(
            NotificationCenter.default.publisher(
                for: UserDefaults.didChangeNotification
            )
        ) { _ in
            GuestPictureInPicture.shared.refresh()
            refreshKeepRunning()
        }
        .onChange(of: launcher.phase) { _, phase in
            GuestPictureInPicture.shared.guestRunningChanged(phase == .running)
            refreshKeepRunning()
            // Nothing is left to type into, and the keyboard would cover
            // the notice.
            guard case .ended = phase else { return }
            framebufferKeyboardVisible = false
            serialConsole.resignFirstResponder()
        }
    }

    @ViewBuilder
    private var floatingButtons: some View {
        GlassEffectContainer(spacing: FloatingControlMetrics.spacing) {
            floatingButtonsContent
        }
    }

    private var floatingButtonsContent: some View {
        VStack(alignment: .trailing, spacing: FloatingControlMetrics.spacing) {
            settingsButton
            if keepRunningChosen && launcher.phase == .running {
                keepRunningButton
            }
            displayModeButton
            keyboardButton
            specialKeysRow
        }
    }

    /// Starts, or stops, the job that keeps the guest running once the user
    /// leaves Kernarium. iOS wants such a job to follow the user's own action,
    /// so it never starts by itself.
    private var keepRunningButton: some View {
        floatingIconButton(
            systemName: backgroundJob.isRunning ? "stop.circle" : "timer",
            accessibilityLabel: keepRunningLabel,
            identifier: "backgroundJobButton"
        ) {
            if backgroundJob.state == .idle {
                guard let guest = launcher.runningGuestName else { return }
                backgroundJob.start(
                    limitMinutes: BackgroundSettings().limitMinutes(
                        forGuest: guest
                    ),
                    guestName: (guest as NSString).deletingPathExtension
                )
            } else {
                backgroundJob.cancel()
            }
        }
    }

    private var keepRunningLabel: String {
        switch backgroundJob.state {
        case .idle: "Keep the Guest Running in the Background"
        case .submitted: "Starting to Keep the Guest Running"
        case .running: "Stop Keeping the Guest Running"
        }
    }

    private func refreshKeepRunning() {
        let mode = launcher.runningGuestName.map {
            BackgroundSettings().mode(forGuest: $0)
        } ?? .off
        keepRunningChosen = mode == .keepRunning
        backgroundJob.reconcile(mode: mode)
    }

    private var displayModeButton: some View {
        floatingIconButton(
            systemName: guestViewMode == .framebuffer ? "terminal" : "display",
            accessibilityLabel: guestViewMode == .framebuffer
                ? "Show Serial Console"
                : "Show Framebuffer",
            identifier: "displayModeButton",
            action: toggleGuestViewMode
        )
    }

    private var settingsButton: some View {
        floatingIconButton(
            systemName: "gearshape",
            accessibilityLabel: "Emulator Settings",
            identifier: "settingsButton"
        ) {
            presentedSheet = .settings
        }
    }

    /// Whether the view on screen holds the keyboard.
    private var keyboardVisible: Bool {
        switch guestViewMode {
        case .framebuffer: framebufferKeyboardVisible
        case .serialConsole: serialConsole.isFocused
        }
    }

    private var keyboardButton: some View {
        floatingIconButton(
            systemName: keyboardVisible
                ? "keyboard.chevron.compact.down"
                : "keyboard",
            accessibilityLabel: keyboardVisible
                ? "Hide Keyboard"
                : "Show Keyboard",
            identifier: "keyboardButton",
            action: toggleKeyboard
        )
    }

    private var specialKeysRow: some View {
        HStack(spacing: FloatingControlMetrics.spacing) {
            if specialKeysExpanded {
                ForEach(GuestSpecialKey.allCases) { key in
                    specialKeyButton(key)
                }
                .transition(.move(edge: .trailing).combined(with: .opacity))
            }
            floatingIconButton(
                systemName: specialKeysExpanded
                    ? "chevron.right"
                    : "chevron.left",
                accessibilityLabel: specialKeysExpanded
                    ? "Hide Special Keys"
                    : "Show Special Keys",
                identifier: "specialKeysButton"
            ) {
                withAnimation {
                    specialKeysExpanded.toggle()
                }
            }
        }
    }

    private func specialKeyButton(_ key: GuestSpecialKey) -> some View {
        floatingIconButton(
            systemName: key.systemName,
            accessibilityLabel: key.accessibilityLabel,
            identifier: key.accessibilityIdentifier
        ) {
            sendGuestKey(key.code)
        }
    }

    @ViewBuilder
    private func floatingIconButton(
        systemName: String,
        accessibilityLabel: String,
        identifier: String,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            floatingIcon(systemName: systemName)
        }
        .buttonStyle(.plain)
        .glassEffect(.regular.interactive(), in: .circle)
        .glassEffectID(identifier, in: floatingButtonNamespace)
        .accessibilityLabel(accessibilityLabel)
        .accessibilityIdentifier(identifier)
    }

    private func floatingIcon(systemName: String) -> some View {
        Image(systemName: systemName)
            .frame(
                width: FloatingControlMetrics.buttonSide,
                height: FloatingControlMetrics.buttonSide
            )
            .contentShape(Circle())
    }

    /// Raises or puts away the keyboard of the view on screen, so typed
    /// text reaches what the user is looking at.
    private func toggleKeyboard() {
        switch guestViewMode {
        case .framebuffer:
            framebufferKeyboardVisible.toggle()
        case .serialConsole:
            if serialConsole.isFocused {
                serialConsole.resignFirstResponder()
            } else {
                serialConsole.becomeFirstResponder()
            }
        }
    }

    /// A mode switch puts the keyboard away: the view that held it is
    /// hidden, and no hidden view keeps keyboard focus.
    private func toggleGuestViewMode() {
        switch guestViewMode {
        case .framebuffer:
            framebufferKeyboardVisible = false
            guestViewMode = .serialConsole
        case .serialConsole:
            serialConsole.resignFirstResponder()
            guestViewMode = .framebuffer
        }
    }

    private func sendGuestKey(_ code: UInt16) {
        kernarium_runtime_loader_send_key(code, 1)
        kernarium_runtime_loader_send_key(code, 0)
    }
}

enum GuestViewMode {
    case framebuffer
    case serialConsole

    static let defaultMode: Self = .framebuffer
}

/// How this launch's guest run ended, as the application reports it.
struct GuestRunEnd: Equatable {
    let result: Int32

    var title: String {
        result == 0 ? "Guest Shut Down" : "Guest Stopped with an Error"
    }

    var message: String {
        let relaunch = "Relaunch Kernarium to start it again."
        return result == 0
            ? relaunch
            : "The runtime returned \(result). \(relaunch)"
    }

    var systemName: String {
        result == 0 ? "power" : "exclamationmark.triangle"
    }
}

/// Shown over the guest views once the run has ended. The guest's last frame
/// and console stay readable behind it, and the floating controls still
/// switch between them.
private struct GuestEndedNotice: View {
    let end: GuestRunEnd

    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: end.systemName)
                .font(.title)
            Text(end.title)
                .font(.headline)
            Text(end.message)
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
        }
        .padding(20)
        .frame(maxWidth: 320)
        .glassEffect(.regular, in: .rect(cornerRadius: 20))
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("guestEndedNotice")
    }
}

/// Shown over the guest views when the previous launch ended in the
/// background and this launch's guest booted again. It stays until dismissed:
/// the guest's boot is under way behind it, and what was running before is
/// gone.
private struct GuestRestartedNotice: View {
    let restart: GuestRestart
    let dismiss: () -> Void

    var body: some View {
        Button(action: dismiss) {
            VStack(spacing: 8) {
                Image(systemName: restart.systemName)
                    .font(.title)
                Text(restart.title)
                    .font(.headline)
                Text(restart.message)
                    .font(.subheadline)
                    .foregroundStyle(.primary)
                    .multilineTextAlignment(.center)
                Text("Tap to dismiss")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
            .padding(20)
            .frame(maxWidth: 320)
            .glassEffect(.regular, in: .rect(cornerRadius: 20))
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("guestRestartedNotice")
    }
}

enum FloatingControlMetrics {
    static let buttonSide: CGFloat = 32
    static let spacing: CGFloat = 8
}

enum GuestSpecialKey: UInt16, CaseIterable, Identifiable {
    case power = 116
    case volumeUp = 115
    case volumeDown = 114
    case escape = 1
    case tab = 15
    case home = 102

    var id: Self { self }
    var code: UInt16 { rawValue }

    var systemName: String {
        switch self {
        case .power: "power"
        case .volumeUp: "speaker.wave.3"
        case .volumeDown: "speaker.wave.1"
        case .escape: "escape"
        case .tab: "arrow.right.to.line"
        case .home: "house"
        }
    }

    var accessibilityLabel: String {
        switch self {
        case .power: "Power"
        case .volumeUp: "Volume Up"
        case .volumeDown: "Volume Down"
        case .escape: "Escape"
        case .tab: "Tab"
        case .home: "Home"
        }
    }

    var accessibilityIdentifier: String {
        switch self {
        case .power: "guestPowerButton"
        case .volumeUp: "guestVolumeUpButton"
        case .volumeDown: "guestVolumeDownButton"
        case .escape: "guestEscapeButton"
        case .tab: "guestTabButton"
        case .home: "guestHomeButton"
        }
    }
}

private enum PresentedSheet: String, Identifiable {
    case settings

    var id: String { rawValue }
}

private struct GuestDisplayGeometry {
    let pixelSize: CGSize
    let dotsPerInch: UInt32
}

/// The coordinates the guest's touchscreen takes, in a space of `size`
/// positions on each axis.
struct GuestInputCoordinateSpace {
    static let maximumDisplayDimension = CGFloat(4095)

    /// The Virtio touchscreen's fixed absolute range, 0 through
    /// `KERNARIUM_RUNTIME_TOUCH_MAXIMUM` on both axes. It does not depend on
    /// the guest's display, so a launch at a small size loses no resolution
    /// at a larger one, and the guest never has to learn the range again
    /// after a mode change.
    static let touchscreen = GuestInputCoordinateSpace(
        size: CGSize(
            width: CGFloat(KERNARIUM_RUNTIME_TOUCH_MAXIMUM) + 1,
            height: CGFloat(KERNARIUM_RUNTIME_TOUCH_MAXIMUM) + 1
        )
    )

    let size: CGSize

    static func boundedDisplaySize(_ size: CGSize) -> CGSize {
        CGSize(
            width: min(size.width, maximumDisplayDimension),
            height: min(size.height, maximumDisplayDimension)
        )
    }

    func location(
        _ point: CGPoint,
        in bounds: CGSize,
        drawableSize: CGSize? = nil,
        contentSize: CGSize? = nil,
        clampToContent: Bool = false
    ) -> (x: UInt32, y: UInt32)? {
        let drawableSize = drawableSize ?? bounds
        let contentSize = contentSize ?? drawableSize
        guard size.width >= 1, size.height >= 1,
              bounds.width > 0, bounds.height > 0,
              point.x.isFinite, point.y.isFinite,
              size.width.isFinite, size.height.isFinite,
              bounds.width.isFinite, bounds.height.isFinite,
              drawableSize.width.isFinite, drawableSize.height.isFinite,
              drawableSize.width > 0, drawableSize.height > 0,
              size.width <= CGFloat(UInt32.max),
              size.height <= CGFloat(UInt32.max) else { return nil }
        guard let viewport = GuestDisplayViewport.aspectFit(
            contentSize: contentSize,
            in: drawableSize
        ) else { return nil }
        var drawablePoint = CGPoint(
            x: point.x * drawableSize.width / bounds.width,
            y: point.y * drawableSize.height / bounds.height
        )
        if clampToContent {
            drawablePoint.x = min(
                max(drawablePoint.x, viewport.minX), viewport.maxX
            )
            drawablePoint.y = min(
                max(drawablePoint.y, viewport.minY), viewport.maxY
            )
        } else if drawablePoint.x < viewport.minX ||
                    drawablePoint.x > viewport.maxX ||
                    drawablePoint.y < viewport.minY ||
                    drawablePoint.y > viewport.maxY {
            return nil
        }
        let x = min(
            max(
                (
                    (drawablePoint.x - viewport.minX) * size.width /
                        viewport.width
                ).rounded(.down),
                0
            ),
            size.width - 1
        )
        let y = min(
            max(
                (
                    (drawablePoint.y - viewport.minY) * size.height /
                        viewport.height
                ).rounded(.down),
                0
            ),
            size.height - 1
        )
        return (UInt32(x), UInt32(y))
    }
}

enum GuestDisplayViewport {
    static func aspectFit(
        contentSize: CGSize,
        in containerSize: CGSize
    ) -> CGRect? {
        guard contentSize.width.isFinite, contentSize.height.isFinite,
              containerSize.width.isFinite, containerSize.height.isFinite,
              contentSize.width > 0, contentSize.height > 0,
              containerSize.width > 0, containerSize.height > 0 else {
            return nil
        }
        let scale = min(
            containerSize.width / contentSize.width,
            containerSize.height / contentSize.height
        )
        let width = contentSize.width * scale
        let height = contentSize.height * scale
        return CGRect(
            x: (containerSize.width - width) / 2,
            y: (containerSize.height - height) / 2,
            width: width,
            height: height
        )
    }
}

struct GuestTouchEvent: Equatable {
    let x: UInt32
    let y: UInt32
    let pressed: Bool
}

enum GuestTouchSamples {
    static func ordered(
        coalesced: [CGPoint],
        current: CGPoint
    ) -> [CGPoint] {
        guard coalesced.last != current else { return coalesced }
        return coalesced + [current]
    }
}

struct GuestTouchGeometry {
    private let coordinateSpace: GuestInputCoordinateSpace
    private let boundsSize: CGSize
    private let drawableSize: CGSize
    private let contentSize: CGSize
    private let viewport: CGRect

    init?(
        coordinateSpace: GuestInputCoordinateSpace,
        boundsSize: CGSize,
        drawableSize: CGSize,
        contentSize: CGSize
    ) {
        guard boundsSize.width.isFinite, boundsSize.height.isFinite,
              boundsSize.width > 0, boundsSize.height > 0,
              drawableSize.width.isFinite, drawableSize.height.isFinite,
              drawableSize.width > 0, drawableSize.height > 0,
              let viewport = GuestDisplayViewport.aspectFit(
                  contentSize: contentSize,
                  in: drawableSize
              ) else { return nil }
        self.coordinateSpace = coordinateSpace
        self.boundsSize = boundsSize
        self.drawableSize = drawableSize
        self.contentSize = contentSize
        self.viewport = viewport
    }

    fileprivate func drawablePoint(for point: CGPoint) -> CGPoint? {
        guard point.x.isFinite, point.y.isFinite else { return nil }
        let drawablePoint = CGPoint(
            x: point.x * drawableSize.width / boundsSize.width,
            y: point.y * drawableSize.height / boundsSize.height
        )
        guard drawablePoint.x.isFinite, drawablePoint.y.isFinite else {
            return nil
        }
        return drawablePoint
    }

    fileprivate func contains(_ point: CGPoint) -> Bool {
        point.x >= viewport.minX && point.x <= viewport.maxX &&
            point.y >= viewport.minY && point.y <= viewport.maxY
    }

    fileprivate func touchEvent(
        at drawablePoint: CGPoint,
        pressed: Bool
    ) -> GuestTouchEvent? {
        let boundsPoint = CGPoint(
            x: drawablePoint.x * boundsSize.width / drawableSize.width,
            y: drawablePoint.y * boundsSize.height / drawableSize.height
        )
        guard let location = coordinateSpace.location(
            boundsPoint,
            in: boundsSize,
            drawableSize: drawableSize,
            contentSize: contentSize,
            clampToContent: true
        ) else { return nil }
        return GuestTouchEvent(
            x: location.x,
            y: location.y,
            pressed: pressed
        )
    }

    fileprivate func clippedSegment(
        from start: CGPoint,
        to end: CGPoint
    ) -> (
        entry: CGPoint,
        exit: CGPoint,
        entryParameter: CGFloat,
        exitParameter: CGFloat
    )? {
        let delta = CGPoint(x: end.x - start.x, y: end.y - start.y)
        var entryParameter = CGFloat.zero
        var exitParameter = CGFloat(1)

        func clipped(
            _ direction: CGFloat,
            _ distance: CGFloat,
            entry: inout CGFloat,
            exit: inout CGFloat
        ) -> Bool {
            if direction == 0 {
                return distance >= 0
            }
            let parameter = distance / direction
            if direction < 0 {
                guard parameter <= exit else { return false }
                entry = max(entry, parameter)
            } else {
                guard parameter >= entry else { return false }
                exit = min(exit, parameter)
            }
            return true
        }

        guard clipped(
            -delta.x,
            start.x - viewport.minX,
            entry: &entryParameter,
            exit: &exitParameter
        ), clipped(
            delta.x,
            viewport.maxX - start.x,
            entry: &entryParameter,
            exit: &exitParameter
        ), clipped(
            -delta.y,
            start.y - viewport.minY,
            entry: &entryParameter,
            exit: &exitParameter
        ), clipped(
            delta.y,
            viewport.maxY - start.y,
            entry: &entryParameter,
            exit: &exitParameter
        ), entryParameter <= exitParameter else { return nil }

        return (
            CGPoint(
                x: start.x + delta.x * entryParameter,
                y: start.y + delta.y * entryParameter
            ),
            CGPoint(
                x: start.x + delta.x * exitParameter,
                y: start.y + delta.y * exitParameter
            ),
            entryParameter,
            exitParameter
        )
    }
}

struct GuestTouchPath {
    private var previousPoint: CGPoint?
    private var guestContactActive = false
    private var lastGuestEvent: GuestTouchEvent?

    mutating func begin(
        at point: CGPoint,
        geometry: GuestTouchGeometry?
    ) -> [GuestTouchEvent] {
        previousPoint = point
        guestContactActive = false
        lastGuestEvent = nil
        guard let geometry,
              let drawablePoint = geometry.drawablePoint(for: point),
              geometry.contains(drawablePoint),
              let event = geometry.touchEvent(
                  at: drawablePoint,
                  pressed: true
              ) else { return [] }
        guestContactActive = true
        lastGuestEvent = event
        return [event]
    }

    mutating func move(
        to point: CGPoint,
        geometry: GuestTouchGeometry?
    ) -> [GuestTouchEvent] {
        guard let previousPoint else {
            return begin(at: point, geometry: geometry)
        }
        self.previousPoint = point
        guard let geometry,
              let start = geometry.drawablePoint(for: previousPoint),
              let end = geometry.drawablePoint(for: point) else { return [] }

        var events: [GuestTouchEvent] = []
        let startInside = geometry.contains(start)
        let endInside = geometry.contains(end)

        if guestContactActive && !startInside {
            appendLastRelease(to: &events)
        }

        if guestContactActive {
            if endInside {
                append(
                    geometry.touchEvent(at: end, pressed: true),
                    to: &events
                )
            } else if let segment = geometry.clippedSegment(
                from: start,
                to: end
            ) {
                append(
                    geometry.touchEvent(at: segment.exit, pressed: false),
                    to: &events
                )
            } else {
                appendLastRelease(to: &events)
            }
            return events
        }

        guard let segment = geometry.clippedSegment(from: start, to: end),
              startInside || endInside ||
                segment.exitParameter > segment.entryParameter else {
            return events
        }
        append(
            geometry.touchEvent(at: segment.entry, pressed: true),
            to: &events
        )
        if endInside {
            if segment.entryParameter < 1 {
                append(
                    geometry.touchEvent(at: end, pressed: true),
                    to: &events
                )
            }
        } else {
            append(
                geometry.touchEvent(at: segment.exit, pressed: false),
                to: &events
            )
        }
        return events
    }

    mutating func move(
        through points: [CGPoint],
        geometry: GuestTouchGeometry?
    ) -> [GuestTouchEvent] {
        var events: [GuestTouchEvent] = []
        for point in points {
            events.append(contentsOf: move(to: point, geometry: geometry))
        }
        return events
    }

    mutating func end(
        at point: CGPoint,
        geometry: GuestTouchGeometry?
    ) -> [GuestTouchEvent] {
        var events = move(to: point, geometry: geometry)
        appendLastRelease(to: &events)
        previousPoint = nil
        return events
    }

    mutating func cancel(
        at point: CGPoint,
        geometry: GuestTouchGeometry?
    ) -> [GuestTouchEvent] {
        end(at: point, geometry: geometry)
    }

    mutating func release() -> [GuestTouchEvent] {
        var events: [GuestTouchEvent] = []
        appendLastRelease(to: &events)
        previousPoint = nil
        return events
    }

    private mutating func append(
        _ event: GuestTouchEvent?,
        to events: inout [GuestTouchEvent]
    ) {
        guard let event else { return }
        events.append(event)
        guestContactActive = event.pressed
        lastGuestEvent = event.pressed ? event : nil
    }

    private mutating func appendLastRelease(
        to events: inout [GuestTouchEvent]
    ) {
        guard guestContactActive, let lastGuestEvent else { return }
        events.append(GuestTouchEvent(
            x: lastGuestEvent.x,
            y: lastGuestEvent.y,
            pressed: false
        ))
        guestContactActive = false
        self.lastGuestEvent = nil
    }
}

private extension Notification.Name {
    static let guestDisplayResizeWillBegin = Notification.Name(
        "org.linux-um-darwin.guest-display-resize-will-begin"
    )
    static let guestDisplayResizeDidEnd = Notification.Name(
        "org.linux-um-darwin.guest-display-resize-did-end"
    )
}

/// When the guest keeps its display mode while the host view changes size.
///
/// A view-controller transition holds the mode from its start to its
/// completion. An iPadOS window resize is one interactive gesture that runs
/// many such transitions, one per size the window passes through, and it holds
/// the mode until the gesture ends, as an AppKit live resize does. The guest
/// then sees only the size the gesture settles at, not each pause on the way.
struct GuestResizeHold {
    enum Action: Equatable {
        case none
        case begin
        case finish
    }

    private var generation: UInt64 = 0
    private var transitionInFlight = false
    private var interactive = false

    /// A transition starts; returns the token its completion presents.
    mutating func transitionWillBegin() -> (token: UInt64, action: Action) {
        generation &+= 1
        transitionInFlight = true
        return (generation, .begin)
    }

    /// A transition completed. A superseded transition never finishes the
    /// hold, and neither does any transition during an interactive resize.
    mutating func transitionDidComplete(token: UInt64) -> Action {
        guard token == generation else { return .none }
        transitionInFlight = false
        return interactive ? .none : .finish
    }

    /// The window scene's interactive-resize state changed. The end of a
    /// gesture finishes the hold unless the gesture's last transition is
    /// still running, which finishes it on completion.
    mutating func interactiveResizeChanged(_ resizing: Bool) -> Action {
        guard resizing != interactive else { return .none }
        interactive = resizing
        if resizing { return .begin }
        return transitionInFlight ? .none : .finish
    }
}

/// Whether a window scene fills its screen, and so has no window controls
/// over its content: Full Screen Apps, and a window zoomed to the screen,
/// whose controls move to the menu bar. A window, tile or Slide Over scene
/// is smaller than the screen.
enum SceneCoverage {
    static func covers(sceneSize: CGSize, screenSize: CGSize) -> Bool {
        // A scene is a few hundredths of a point off the screen at most,
        // where it fills it.
        abs(sceneSize.width - screenSize.width) < 0.5
            && abs(sceneSize.height - screenSize.height) < 0.5
    }

    static func covers(_ scene: UIWindowScene) -> Bool {
        covers(
            sceneSize: scene.effectiveGeometry.coordinateSpace.bounds.size,
            screenSize: scene.screen.bounds.size
        )
    }

    /// The edges whose corner offset the guest view takes: the top, which
    /// the window controls overlay, unless the scene covers the screen.
    static func cornerOffsetEdges(coversScreen: Bool) -> Edge.Set {
        coversScreen ? [] : .top
    }
}

/// Reports, now and whenever the scene's geometry changes, whether the
/// scene the view is in covers its screen. The system's corner offset keeps
/// describing a window after Multitasking switches the scene to Full Screen
/// Apps, so the offset cannot be the judge of whether there are controls.
private struct SceneCoverageReader: UIViewRepresentable {
    let report: @MainActor (Bool) -> Void

    func makeUIView(context: Context) -> SceneCoverageView {
        let view = SceneCoverageView()
        view.isUserInteractionEnabled = false
        view.report = report
        return view
    }

    func updateUIView(_ view: SceneCoverageView, context: Context) {
        view.report = report
    }
}

final class SceneCoverageView: UIView {
    var report: (@MainActor (Bool) -> Void)?
    private var observation: NSKeyValueObservation?
    private var reported: Bool?

    override func didMoveToWindow() {
        super.didMoveToWindow()
        observation = nil
        guard let scene = window?.windowScene else { return }
        observation = scene.observe(
            \.effectiveGeometry, options: [.initial, .new]
        ) { [weak self] scene, _ in
            MainActor.assumeIsolated { self?.update(from: scene) }
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        if let scene = window?.windowScene {
            update(from: scene)
        }
    }

    private func update(from scene: UIWindowScene) {
        let covers = SceneCoverage.covers(scene)
        guard covers != reported else { return }
        reported = covers
        // SwiftUI state is not written from inside its own layout pass.
        DispatchQueue.main.async { [weak self] in
            self?.report?(covers)
        }
    }
}

private struct GuestDisplayView: UIViewControllerRepresentable {
    @Binding var keyboardVisible: Bool
    let retinaMode: Bool
    let viewDidLayout: @MainActor (
        GuestMetalView,
        GuestDisplayGeometry
    ) -> Void

    func makeUIViewController(context: Context) -> GuestDisplayViewController {
        let view = GuestMetalView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.viewDidLayout = viewDidLayout
        view.retinaMode = retinaMode
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.autoResizeDrawable = false
        view.isMultipleTouchEnabled = false
        return GuestDisplayViewController(metalView: view)
    }

    func updateUIViewController(
        _ controller: GuestDisplayViewController,
        context: Context
    ) {
        let view = controller.metalView
        view.retinaMode = retinaMode
        view.setKeyboardVisible(keyboardVisible)
        view.reportLayout()
    }

    static func dismantleUIViewController(
        _ controller: GuestDisplayViewController,
        coordinator: ()
    ) {
        controller.metalView.releaseGuestInput()
        controller.metalView.finishHostResize()
        controller.metalView.resignFirstResponder()
    }
}

private final class GuestDisplayViewController: UIViewController {
    let metalView: GuestMetalView
    private var resizeHold = GuestResizeHold()
    private var sceneGeometryObservation: NSKeyValueObservation?

    init(metalView: GuestMetalView) {
        self.metalView = metalView
        super.init(nibName: nil, bundle: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func loadView() {
        view = metalView
    }

    override func viewWillTransition(
        to size: CGSize,
        with coordinator: UIViewControllerTransitionCoordinator
    ) {
        let (token, action) = resizeHold.transitionWillBegin()
        perform(action)
        super.viewWillTransition(to: size, with: coordinator)
        coordinator.animate(alongsideTransition: nil) { [weak self] _ in
            guard let self else { return }
            self.perform(self.resizeHold.transitionDidComplete(token: token))
        }
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        guard sceneGeometryObservation == nil,
              let scene = view.window?.windowScene else { return }
        // An interactive window resize reports no transition of its own to
        // hold across, so the scene's geometry says when the gesture runs.
        sceneGeometryObservation = scene.observe(
            \.effectiveGeometry, options: [.initial, .new]
        ) { [weak self] scene, _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.perform(self.resizeHold.interactiveResizeChanged(
                    scene.effectiveGeometry.isInteractivelyResizing
                ))
            }
        }
    }

    private func perform(_ action: GuestResizeHold.Action) {
        switch action {
        case .none: break
        case .begin: metalView.beginHostResize()
        case .finish: metalView.finishHostResize()
        }
    }
}

private final class GuestMetalView: MTKView, UIKeyInput {
    var viewDidLayout: (@MainActor (
        GuestMetalView,
        GuestDisplayGeometry
    ) -> Void)?
    var retinaMode = false
    @objc private(set) var umlGuestResizeInProgress = false
    private var trackedTouch: UITouch?
    private var touchPath = GuestTouchPath()
    private var guestContentSize: CGSize?

    override var canBecomeFirstResponder: Bool { true }
    var hasText: Bool { true }
    var keyboardType: UIKeyboardType = .asciiCapable
    var autocorrectionType: UITextAutocorrectionType = .no
    var autocapitalizationType: UITextAutocapitalizationType = .none
    var smartDashesType: UITextSmartDashesType = .no
    var smartQuotesType: UITextSmartQuotesType = .no
    var smartInsertDeleteType: UITextSmartInsertDeleteType = .no

    override func layoutSubviews() {
        super.layoutSubviews()
        reportLayout()
    }

    func reportLayout() {
        guard let screen = window?.screen else { return }
        let scale = retinaMode ? screen.nativeScale : 1
        guard bounds.width > 0, bounds.height > 0,
              scale.isFinite, scale > 0 else { return }
        // SwiftUI supplies safe-area-excluded point bounds. Point-resolution
        // mode maps them directly to guest pixels; Retina mode converts them
        // to physical backing pixels, including fractional native scales.
        let pixelSize = CGSize(
            width: (bounds.width * scale).rounded(.down),
            height: (bounds.height * scale).rounded(.down)
        )
        guard pixelSize.width >= 1, pixelSize.height >= 1 else { return }
        drawableSize = pixelSize
        let dotsPerInch = UInt32(
            (CGFloat(160) * scale).rounded()
        )
        viewDidLayout?(
            self,
            GuestDisplayGeometry(
                pixelSize: pixelSize,
                dotsPerInch: dotsPerInch
            )
        )
    }

    func beginHostResize() {
        guard !umlGuestResizeInProgress else { return }
        umlGuestResizeInProgress = true
        NotificationCenter.default.post(
            name: .guestDisplayResizeWillBegin,
            object: self
        )
    }

    func finishHostResize() {
        guard umlGuestResizeInProgress else { return }
        reportLayout()
        umlGuestResizeInProgress = false
        NotificationCenter.default.post(
            name: .guestDisplayResizeDidEnd,
            object: self
        )
    }

    @objc(umlSetGuestDisplayWidth:height:)
    func setGuestDisplay(width: UInt32, height: UInt32) {
        releaseTrackedTouch()
        guard width > 0, height > 0 else {
            guestContentSize = nil
            return
        }
        guestContentSize = CGSize(
            width: CGFloat(width),
            height: CGFloat(height)
        )
    }

    func releaseGuestInput() {
        releaseTrackedTouch()
    }

    func setKeyboardVisible(_ visible: Bool) {
        if visible {
            becomeFirstResponder()
        } else {
            resignFirstResponder()
        }
    }

    func insertText(_ text: String) {
        for character in text {
            guard let stroke = LinuxKeyboard.stroke(for: character) else {
                print("Kernarium: unsupported keyboard character \(character)")
                continue
            }
            if stroke.shifted {
                sendKey(code: LinuxKeyboard.leftShift, pressed: true)
            }
            sendKey(code: stroke.code, pressed: true)
            sendKey(code: stroke.code, pressed: false)
            if stroke.shifted {
                sendKey(code: LinuxKeyboard.leftShift, pressed: false)
            }
        }
    }

    func deleteBackward() {
        sendKey(code: LinuxKeyboard.backspace, pressed: true)
        sendKey(code: LinuxKeyboard.backspace, pressed: false)
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesBegan(touches, with: event)
        guard trackedTouch == nil, let touch = touches.first else { return }
        trackedTouch = touch
        sendTouchEvents(touchPath.begin(
            at: touch.location(in: self),
            geometry: touchGeometry()
        ))
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesMoved(touches, with: event)
        guard let touch = trackedTouch, touches.contains(touch) else { return }
        let current = touch.location(in: self)
        let coalescedTouches = event?.coalescedTouches(for: touch) ?? []
        let coalesced = coalescedTouches.map {
            $0.location(in: self)
        }
        sendTouchEvents(touchPath.move(
            through: GuestTouchSamples.ordered(
                coalesced: coalesced,
                current: current
            ),
            geometry: touchGeometry()
        ))
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesEnded(touches, with: event)
        finishTouch(in: touches)
    }

    override func touchesCancelled(
        _ touches: Set<UITouch>,
        with event: UIEvent?
    ) {
        super.touchesCancelled(touches, with: event)
        finishTouch(in: touches, cancelled: true)
    }

    private func finishTouch(
        in touches: Set<UITouch>,
        cancelled: Bool = false
    ) {
        guard let touch = trackedTouch, touches.contains(touch) else { return }
        let point = touch.location(in: self)
        let events = cancelled ?
            touchPath.cancel(at: point, geometry: touchGeometry()) :
            touchPath.end(at: point, geometry: touchGeometry())
        sendTouchEvents(events)
        trackedTouch = nil
    }

    private func touchGeometry() -> GuestTouchGeometry? {
        GuestTouchGeometry(
            coordinateSpace: .touchscreen,
            boundsSize: bounds.size,
            drawableSize: drawableSize,
            contentSize: guestContentSize ?? drawableSize
        )
    }

    private func sendTouchEvents(_ events: [GuestTouchEvent]) {
        for event in events {
            kernarium_runtime_loader_send_touch(
                event.x,
                event.y,
                event.pressed ? 1 : 0
            )
        }
    }

    private func releaseTrackedTouch() {
        guard trackedTouch != nil else { return }
        sendTouchEvents(touchPath.release())
        trackedTouch = nil
    }

    private func sendKey(code: UInt16, pressed: Bool) {
        kernarium_runtime_loader_send_key(code, pressed ? 1 : 0)
    }
}

private enum LinuxKeyboard {
    struct Stroke {
        let code: UInt16
        let shifted: Bool
    }

    static let backspace: UInt16 = 14
    static let leftShift: UInt16 = 42

    static func stroke(for character: Character) -> Stroke? {
        if character == "\n" || character == "\r" {
            return Stroke(code: 28, shifted: false)
        }
        if character == "\t" {
            return Stroke(code: 15, shifted: false)
        }
        if character == " " {
            return Stroke(code: 57, shifted: false)
        }

        let rows: [(String, [UInt16])] = [
            ("1234567890-=", Array(2...13)),
            ("qwertyuiop[]", Array(16...27)),
            ("asdfghjkl;'`", Array(30...41)),
            ("\\zxcvbnm,./", Array(43...53)),
        ]
        let shiftedRows = [
            "!@#$%^&*()_+",
            "QWERTYUIOP{}",
            "ASDFGHJKL:\"~",
            "|ZXCVBNM<>?",
        ]
        for (rowIndex, row) in rows.enumerated() {
            if let index = row.0.firstIndex(of: character) {
                let offset = row.0.distance(from: row.0.startIndex, to: index)
                return Stroke(code: row.1[offset], shifted: false)
            }
            let shifted = shiftedRows[rowIndex]
            if let index = shifted.firstIndex(of: character) {
                let offset = shifted.distance(from: shifted.startIndex, to: index)
                return Stroke(code: row.1[offset], shifted: true)
            }
        }
        return nil
    }
}

enum OverlayDisk {
    static let filename = "kernarium-overlay.ext4"

    static func byteCount(forSizeGiB sizeGiB: Int) throws -> UInt64 {
        guard EmulatorConfiguration.overlaySizeRangeGiB.contains(sizeGiB) else {
            throw CocoaError(.fileWriteInvalidFileName)
        }
        return UInt64(sizeGiB) * 1024 * 1024 * 1024
    }

    static func recreate(
        sizeGiB: Int,
        in directory: URL = FileManager.default.temporaryDirectory
    ) throws -> URL {
        let manager = FileManager.default
        let disk = directory.appendingPathComponent(filename)
        let requestedByteCount = try byteCount(forSizeGiB: sizeGiB)
        if manager.fileExists(atPath: disk.path) {
            try manager.removeItem(at: disk)
        }
        guard manager.createFile(atPath: disk.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        do {
            let handle = try FileHandle(forWritingTo: disk)
            defer { try? handle.close() }
            try handle.truncate(atOffset: requestedByteCount)
        } catch {
            try? manager.removeItem(at: disk)
            throw error
        }
        return disk
    }
}

enum ThermalSamplePhase: String {
    case start
    case change
    case finish
}

enum ThermalStateReport {
    static func name(for state: ProcessInfo.ThermalState) -> String {
        switch state {
        case .nominal:
            return "nominal"
        case .fair:
            return "fair"
        case .serious:
            return "serious"
        case .critical:
            return "critical"
        @unknown default:
            return "unknown"
        }
    }

    static func line(
        phase: ThermalSamplePhase,
        state: ProcessInfo.ThermalState,
        uptime: TimeInterval
    ) -> String {
        let uptimeText = String(
            format: "%.3f",
            locale: Locale(identifier: "en_US_POSIX"),
            uptime
        )
        return "Kernarium thermal state: phase=\(phase.rawValue) " +
            "state=\(name(for: state)) uptime=\(uptimeText)"
    }

    static func probeLine(
        state: ProcessInfo.ThermalState,
        uptime: TimeInterval
    ) -> String {
        let uptimeText = String(
            format: "%.3f",
            locale: Locale(identifier: "en_US_POSIX"),
            uptime
        )
        return "Kernarium thermal probe: state=\(name(for: state)) " +
            "uptime=\(uptimeText)"
    }
}

/// The paths one boot needs, in the order the boot request declares them.
/// They are held as Swift strings and only become C pointers for the duration
/// of the run, which spans the entire guest lifetime.
private struct PreparedBoot: Sendable {
    private let paths: [String?]
    let androidBootMode: AndroidBootMode

    init(rootfs: URL, overlay: URL) {
        paths = [rootfs.path, overlay.path, nil, nil]
        androidBootMode = .automatic
    }

    init(
        bundle: GuestBundle,
        androidBootMode: AndroidBootMode
    ) {
        paths = [
            nil,
            nil,
            bundle.disk.path,
            bundle.commandLine.path,
        ]
        self.androidBootMode = androidBootMode
    }

    func withCPaths<Result>(
        _ body: ([UnsafePointer<CChar>?]) -> Result
    ) -> Result {
        let buffers = paths.map { path in
            path.map { ContiguousArray($0.utf8CString) }
        }
        var pointers = [UnsafePointer<CChar>?](
            repeating: nil, count: buffers.count
        )
        func bind(_ index: Int) -> Result {
            guard index < buffers.count else { return body(pointers) }
            guard let buffer = buffers[index] else { return bind(index + 1) }
            return buffer.withUnsafeBufferPointer { raw in
                pointers[index] = raw.baseAddress
                defer { pointers[index] = nil }
                return bind(index + 1)
            }
        }
        return bind(0)
    }
}

private func runGuest(
    view: GuestMetalView,
    boot: PreparedBoot,
    width: UInt32,
    height: UInt32,
    dotsPerInch: UInt32,
    guestMemoryMiB: UInt32,
    vCPUCount: UInt32,
    autoResizeGuest: Bool
) -> Int32 {
    boot.withCPaths { paths in
        var request = kernarium_runtime_boot()
        request.struct_size = UInt32(MemoryLayout<kernarium_runtime_boot>.size)
        request.width = width
        request.height = height
        request.dots_per_inch = dotsPerInch
        request.guest_memory_mib = guestMemoryMiB
        request.guest_processor_count = vCPUCount
        request.android_boot_mode = boot.androidBootMode.rawValue
        request.auto_resize_guest = autoResizeGuest ? 1 : 0
        request.rootfs_path = paths[0]
        request.overlay_path = paths[1]
        request.android_disk_path = paths[2]
        request.android_command_line_path = paths[3]
        return withUnsafePointer(to: &request) { pointer in
            kernarium_runtime_loader_run(view, pointer)
        }
    }
}

/// Where this launch's guest is. A launch runs its guest at most once: the
/// runtime cannot boot a second guest in the same process, so once the run
/// returns the application can only ask to be relaunched.
enum GuestRunPhase: Equatable {
    /// No run has started. A failure before the run begins, such as a
    /// missing rootfs, leaves the launch here, and a later layout retries.
    case idle
    case running
    /// The run returned `result`; nothing starts another in this process.
    case ended(result: Int32)
}

@MainActor
final class EmulatorLauncher: ObservableObject {
    static let shared = EmulatorLauncher(configuration: .load())

    private let configuration: EmulatorConfiguration
    @Published private(set) var phase = GuestRunPhase.idle
    /// The name the running guest's own settings are stored under.
    private(set) var runningGuestName: String?
    private(set) var runningGuestIsAndroid = false
    private var thermalObserver: NSObjectProtocol?

    var retinaMode: Bool { configuration.retinaMode }

    private init(configuration: EmulatorConfiguration) {
        self.configuration = configuration
    }

    fileprivate func start(
        view: GuestMetalView,
        geometry: GuestDisplayGeometry
    ) {
        guard phase == .idle else { return }
        if ProcessInfo.processInfo.environment[
            "KERNARIUM_THERMAL_PROBE_ONLY"
        ] == "1" {
            phase = .running
            print(
                ThermalStateReport.probeLine(
                    state: ProcessInfo.processInfo.thermalState,
                    uptime: ProcessInfo.processInfo.systemUptime
                )
            )
            return
        }
        guard let rootfsFilename = RootfsImage.selectedFilename(
            configuredFilename: configuration.rootfsFilename
        ) else {
            print("Kernarium: invalid rootfs filename")
            return
        }
        guard let selection = RootfsImage.selection(named: rootfsFilename) else {
            print("Kernarium: Documents/\(rootfsFilename) is missing")
            return
        }
        runningGuestName = rootfsFilename
        runningGuestIsAndroid = selection.flavor == .android
        // Every runtime carries a 4 KiB kernel.
        guard kernarium_runtime_loader_select(
            selection.flavor.rawValue,
            4096
        ) == 0 else {
            print("Kernarium: cannot load the runtime")
            return
        }
        let boot: PreparedBoot
        switch selection {
        case let .image(rootfs, _):
            guard let prepared = prepareImageBoot(rootfs: rootfs) else { return }
            boot = prepared
        case let .bundle(bundle):
            guard let prepared = prepareBundleBoot(bundle: bundle) else { return }
            boot = prepared
        }
        phase = .running
        startThermalObservation()
        let size = GuestInputCoordinateSpace.boundedDisplaySize(
            geometry.pixelSize
        )
        view.setGuestDisplay(
            width: UInt32(size.width.rounded(.down)),
            height: UInt32(size.height.rounded(.down))
        )
        UIApplication.shared.isIdleTimerDisabled = true
        let width = UInt32(size.width.rounded(.down))
        let height = UInt32(size.height.rounded(.down))
        let guestMemoryMiB = UInt32(configuration.guestMemoryMiB)
        let vCPUCount = UInt32(configuration.vCPUCount)
        let dotsPerInch = geometry.dotsPerInch
        let autoResizeGuest = configuration.autoResizeGuest
        // The run occupies its thread for the guest's whole life and the GPU
        // service reaches the main queue while it starts, so it must not be the
        // main thread. Own the thread outright rather than depending on where a
        // task happens to be scheduled, and keep it off the cooperative pool it
        // would otherwise block for hours. It inherits the main thread's
        // SIGALRM block, which the runtime then reasserts for itself.
        let worker = Thread {
            let result = runGuest(
                view: view,
                boot: boot,
                width: width,
                height: height,
                dotsPerInch: dotsPerInch,
                guestMemoryMiB: guestMemoryMiB,
                vCPUCount: vCPUCount,
                autoResizeGuest: autoResizeGuest
            )
            Task { @MainActor in
                EmulatorLauncher.shared.finished(result: result)
            }
        }
        worker.name = "uml-launcher"
        worker.start()
    }

    /// A plain image boots read-only under a disposable overlay disk, which is
    /// recreated every launch and holds no state worth keeping.
    private func prepareImageBoot(rootfs: URL) -> PreparedBoot? {
        if rootfs.pathExtension.lowercased() == "ext4" {
            do {
                try FileManager.default.setAttributes(
                    [.posixPermissions: 0o444],
                    ofItemAtPath: rootfs.path
                )
            } catch {
                print("Kernarium: cannot seal the ext4 rootfs read-only")
                return nil
            }
        }
        do {
            let overlay = try OverlayDisk.recreate(
                sizeGiB: configuration.overlaySizeGiB
            )
            return PreparedBoot(rootfs: rootfs, overlay: overlay)
        } catch {
            print(
                "Kernarium: cannot create " +
                "\(configuration.overlaySizeGiB) GiB overlay disk: \(error)"
            )
            return nil
        }
    }

    /// A bundle keeps its directly writable disk beside its payload, so guest
    /// state survives the app being closed and reopened.
    private func prepareBundleBoot(bundle: GuestBundle) -> PreparedBoot? {
        guard FileManager.default.isWritableFile(atPath: bundle.url.path) else {
            print("Kernarium: \(bundle.name) is not writable")
            return nil
        }
        do {
            let resized = try bundle.prepareDiskCapacity(
                configuration.androidDiskSizeGiB
            )
            if resized {
                print(
                    "Kernarium: expanded \(bundle.disk.lastPathComponent) " +
                        "to \(configuration.androidDiskSizeGiB) GiB"
                )
            }
        } catch {
            print("Kernarium: cannot resize Android disk: \(error)")
            return nil
        }
        return PreparedBoot(
            bundle: bundle,
            androidBootMode: configuration.androidBootMode
        )
    }

    fileprivate func finished(result: Int32) {
        stopThermalObservation()
        phase = .ended(result: result)
        UIApplication.shared.isIdleTimerDisabled = false
        print("Kernarium: runtime returned \(result)")
    }

    func stop() {
        kernarium_runtime_loader_request_stop()
    }

    private func reportThermalState(phase: ThermalSamplePhase) {
        print(
            ThermalStateReport.line(
                phase: phase,
                state: ProcessInfo.processInfo.thermalState,
                uptime: ProcessInfo.processInfo.systemUptime
            )
        )
    }

    private func startThermalObservation() {
        reportThermalState(phase: .start)
        thermalObserver = NotificationCenter.default.addObserver(
            forName: ProcessInfo.thermalStateDidChangeNotification,
            object: nil,
            queue: .main
        ) { _ in
            Task { @MainActor in
                EmulatorLauncher.shared.reportThermalState(phase: .change)
            }
        }
    }

    private func stopThermalObservation() {
        reportThermalState(phase: .finish)
        if let thermalObserver {
            NotificationCenter.default.removeObserver(thermalObserver)
            self.thermalObserver = nil
        }
    }
}
