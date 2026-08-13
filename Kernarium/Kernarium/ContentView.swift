import KernariumRuntime
import MetalKit
import SwiftUI

struct ContentView: View {
    @State private var keyboardVisible = false

    var body: some View {
        ZStack(alignment: .trailing) {
            GuestDisplayView(keyboardVisible: $keyboardVisible) { view, size in
                EmulatorLauncher.shared.start(view: view, size: size)
            }
            keyboardButton
        }
        .ignoresSafeArea(.keyboard)
        .onDisappear {
            EmulatorLauncher.shared.stop()
        }
    }

    @ViewBuilder
    private var keyboardButton: some View {
        if #available(iOS 26.0, *) {
            Button(action: toggleKeyboard) {
                keyboardIcon
            }
            .buttonStyle(.glass)
            .buttonBorderShape(.circle)
            .accessibilityIdentifier("keyboardButton")
        } else {
            Button(action: toggleKeyboard) {
                keyboardIcon
                    .background(.regularMaterial, in: Circle())
                    .overlay {
                        Circle().strokeBorder(.primary, lineWidth: 1)
                    }
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("keyboardButton")
        }
    }

    private var keyboardIcon: some View {
        Image(systemName: keyboardVisible
              ? "keyboard.chevron.compact.down"
              : "keyboard")
            .frame(width: 44, height: 44)
            .contentShape(Circle())
            .accessibilityLabel(keyboardVisible
                                ? "Hide Keyboard"
                                : "Show Keyboard")
    }

    private func toggleKeyboard() {
        keyboardVisible.toggle()
    }
}

private struct GuestDisplayView: UIViewRepresentable {
    @Binding var keyboardVisible: Bool
    let viewDidLayout: @MainActor (GuestMetalView, CGSize) -> Void

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
    var viewDidLayout: (@MainActor (GuestMetalView, CGSize) -> Void)?
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
        let size = bounds.size
        guard size.width > 0, size.height > 0 else { return }
        // SwiftUI lays this view out inside the safe area. Deliberately do not
        // apply the screen scale: one host point is one guest pixel.
        drawableSize = size
        viewDidLayout?(self, size)
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
        guard size.width >= 1, size.height >= 1 else { return }
        let x = min(max(point.x.rounded(.down), 0), size.width - 1)
        let y = min(max(point.y.rounded(.down), 0), size.height - 1)
        kernarium_runtime_send_touch(UInt32(x), UInt32(y), pressed ? 1 : 0)
    }

    private func sendKey(code: UInt16, pressed: Bool) {
        kernarium_runtime_send_key(code, pressed ? 1 : 0)
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

@MainActor
private final class EmulatorLauncher {
    static let shared = EmulatorLauncher()

    private var started = false

    func start(view: GuestMetalView, size: CGSize) {
        guard !started else { return }
        let documents = FileManager.default.urls(
            for: .documentDirectory,
            in: .userDomainMask
        )[0]
        let rootfs = documents.appendingPathComponent("rootfs.erofs")
        guard FileManager.default.fileExists(atPath: rootfs.path) else {
            print("Kernarium: Documents/rootfs.erofs is missing")
            return
        }
        started = true
        view.guestPixelSize = size
        UIApplication.shared.isIdleTimerDisabled = true
        let width = UInt32(size.width.rounded(.down))
        let height = UInt32(size.height.rounded(.down))
        Task.detached {
            let result = rootfs.path.withCString { path in
                kernarium_runtime_run(view, path, width, height)
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
        kernarium_runtime_request_stop()
    }
}
