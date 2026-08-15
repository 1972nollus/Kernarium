import MetalKit
import SwiftUI

struct ContentView: View {
    @State private var keyboardVisible = false
    @State private var presentedSheet: PresentedSheet?

    var body: some View {
        ZStack(alignment: .trailing) {
            GuestDisplayView(keyboardVisible: $keyboardVisible) { view, geometry in
                EmulatorLauncher.shared.start(view: view, geometry: geometry)
            }
            floatingButtons
        }
        .ignoresSafeArea(.keyboard)
        .sheet(item: $presentedSheet) { destination in
            switch destination {
            case .settings:
                EmulatorSettingsView()
            }
        }
        .onDisappear {
            EmulatorLauncher.shared.stop()
        }
    }

    private var floatingButtons: some View {
        VStack {
            settingsButton
            keyboardButton
        }
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

    @ViewBuilder
    private func floatingIconButton(
        systemName: String,
        accessibilityLabel: String,
        identifier: String,
        action: @escaping () -> Void
    ) -> some View {
        if #available(iOS 26.0, *) {
            Button(action: action) {
                floatingIcon(systemName: systemName)
            }
            .buttonStyle(.glass)
            .buttonBorderShape(.circle)
            .accessibilityLabel(accessibilityLabel)
            .accessibilityIdentifier(identifier)
        } else {
            Button(action: action) {
                floatingIcon(systemName: systemName)
                    .background(.regularMaterial, in: Circle())
                    .overlay {
                        Circle().strokeBorder(.primary, lineWidth: 1)
                    }
            }
            .buttonStyle(.plain)
            .accessibilityLabel(accessibilityLabel)
            .accessibilityIdentifier(identifier)
        }
    }

    private func floatingIcon(systemName: String) -> some View {
        Image(systemName: systemName)
            .frame(width: 44, height: 44)
            .contentShape(Circle())
    }

    private func toggleKeyboard() {
        keyboardVisible.toggle()
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

private struct GuestDisplayView: UIViewRepresentable {
    @Binding var keyboardVisible: Bool
    let viewDidLayout: @MainActor (
        GuestMetalView,
        GuestDisplayGeometry
    ) -> Void

    func makeUIView(context: Context) -> GuestMetalView {
        let view = GuestMetalView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.viewDidLayout = viewDidLayout
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.autoResizeDrawable = false
        view.isMultipleTouchEnabled = false
        return view
    }

    func updateUIView(_ view: GuestMetalView, context: Context) {
        view.setKeyboardVisible(keyboardVisible)
        view.reportLayout()
    }

    static func dismantleUIView(_ view: GuestMetalView, coordinator: ()) {
        view.resignFirstResponder()
    }
}

private final class GuestMetalView: MTKView, UIKeyInput {
    var viewDidLayout: (@MainActor (
        GuestMetalView,
        GuestDisplayGeometry
    ) -> Void)?
    var guestPixelSize: CGSize?
    private var trackedTouch: UITouch?

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
        let scale = screen.nativeScale
        guard bounds.width > 0, bounds.height > 0,
              scale.isFinite, scale > 0 else { return }
        // SwiftUI supplies safe-area-excluded point bounds. Convert those
        // bounds to the screen's physical backing pixels, including scaled
        // iPhone modes where nativeScale is not an integer.
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
        sendTouch(touch, pressed: true)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesMoved(touches, with: event)
        guard let touch = trackedTouch, touches.contains(touch) else { return }
        sendTouch(touch, pressed: true)
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
        finishTouch(in: touches)
    }

    private func finishTouch(in touches: Set<UITouch>) {
        guard let touch = trackedTouch, touches.contains(touch) else { return }
        sendTouch(touch, pressed: false)
        trackedTouch = nil
    }

    private func sendTouch(_ touch: UITouch, pressed: Bool) {
        let point = touch.location(in: self)
        let size = guestPixelSize ?? bounds.size
        guard size.width >= 1, size.height >= 1,
              bounds.width > 0, bounds.height > 0 else { return }
        let x = min(
            max((point.x * size.width / bounds.width).rounded(.down), 0),
            size.width - 1
        )
        let y = min(
            max((point.y * size.height / bounds.height).rounded(.down), 0),
            size.height - 1
        )
        kernarium_runtime_loader_send_touch(
            UInt32(x),
            UInt32(y),
            pressed ? 1 : 0
        )
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

@MainActor
private final class EmulatorLauncher {
    static let shared = EmulatorLauncher(configuration: .load())

    private let configuration: EmulatorConfiguration
    private var started = false

    private init(configuration: EmulatorConfiguration) {
        self.configuration = configuration
    }

    func start(view: GuestMetalView, geometry: GuestDisplayGeometry) {
        guard !started else { return }
        guard let rootfsFilename = RootfsImage.selectedFilename(
            configuredFilename: configuration.rootfsFilename
        ) else {
            print("Kernarium: invalid rootfs filename")
            return
        }
        let rootfs = RootfsImage.documentsDirectory.appendingPathComponent(
            rootfsFilename
        )
        guard FileManager.default.fileExists(atPath: rootfs.path) else {
            print("Kernarium: Documents/\(rootfsFilename) is missing")
            return
        }
        guard kernarium_runtime_loader_select(
            UInt32(configuration.kernelPageSize.rawValue)
        ) == 0 else {
            print(
                "Kernarium: cannot load the " +
                "\(configuration.kernelPageSize.title) runtime"
            )
            return
        }
        let overlay: URL
        do {
            overlay = try OverlayDisk.recreate(
                sizeGiB: configuration.overlaySizeGiB
            )
        } catch {
            print(
                "Kernarium: cannot create " +
                "\(configuration.overlaySizeGiB) GiB overlay disk: \(error)"
            )
            return
        }
        started = true
        let size = geometry.pixelSize
        view.guestPixelSize = size
        UIApplication.shared.isIdleTimerDisabled = true
        let width = UInt32(size.width.rounded(.down))
        let height = UInt32(size.height.rounded(.down))
        let guestMemoryMiB = UInt32(configuration.guestMemoryMiB)
        let vCPUCount = UInt32(configuration.vCPUCount)
        Task.detached {
            let result = rootfs.path.withCString { rootfsPath in
                overlay.path.withCString { overlayPath in
                    kernarium_runtime_loader_run(
                        view,
                        rootfsPath,
                        overlayPath,
                        width,
                        height,
                        geometry.dotsPerInch,
                        guestMemoryMiB,
                        vCPUCount
                    )
                }
            }
            await EmulatorLauncher.shared.finished(result: result)
        }
    }

    private func finished(result: Int32) {
        started = false
        UIApplication.shared.isIdleTimerDisabled = false
        print("Kernarium: runtime returned \(result)")
    }

    func stop() {
        kernarium_runtime_loader_request_stop()
    }
}
