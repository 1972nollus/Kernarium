// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics
import Foundation
import GhosttyTerminal
import Testing
import UIKit
@testable import Kernarium

/// The serial console inside the application: guest output reaches the
/// Ghostty terminal whole and in order, and keyboard input leaves it as the
/// bytes a serial terminal sends. Input is captured through the controller's
/// sink, so no guest runtime is involved.
@MainActor
struct SerialConsoleTests {
    /// Collects what the terminal sends on Ghostty's I/O thread.
    private final class InputRecorder: @unchecked Sendable {
        private let lock = NSLock()
        private var received = Data()

        var count: Int {
            lock.withLock { received.count }
        }

        func append(_ data: Data) {
            lock.withLock { received.append(data) }
        }

        func take() -> [UInt8] {
            lock.withLock {
                defer { received = Data() }
                return Array(received)
            }
        }
    }

    @MainActor
    private struct HostedConsole {
        let controller: SerialConsoleController
        let window: UIWindow
        let view: TerminalView

        func remove() {
            controller.detach(view)
            view.removeFromSuperview()
            window.isHidden = true
        }
    }

    /// Installs the controller's terminal view in a window of the
    /// application's scene, which is what gives it a surface. A hidden window
    /// parses and encodes; only a visible one renders.
    private func host(
        _ controller: SerialConsoleController,
        visible: Bool = false
    ) throws -> HostedConsole {
        let scene = try #require(
            UIApplication.shared.connectedScenes
                .compactMap { $0 as? UIWindowScene }
                .first,
            "the application has no window scene"
        )
        let window = UIWindow(windowScene: scene)
        window.frame = CGRect(x: 0, y: 0, width: 800, height: 480)
        window.windowLevel = .alert
        window.isHidden = !visible
        let view = controller.makeTerminalView()
        view.frame = window.bounds
        window.addSubview(view)
        window.layoutIfNeeded()
        return HostedConsole(controller: controller, window: window, view: view)
    }

    /// Polls `condition` on the main actor for at most ten seconds.
    private func eventually(_ condition: () -> Bool) async throws -> Bool {
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: .seconds(10))
        while !condition() {
            guard clock.now < deadline else { return false }
            try await Task.sleep(for: .milliseconds(10))
        }
        return true
    }

    private func viewportLines(
        of controller: SerialConsoleController,
        once marker: String
    ) async throws -> [String] {
        var text = ""
        let found = try await eventually {
            text = controller.session.readViewportText() ?? ""
            return text.contains(marker)
        }
        #expect(found, "the terminal never showed \(marker): \(text)")
        return text.components(separatedBy: "\n")
    }

    private func input(
        from recorder: InputRecorder,
        expecting count: Int
    ) async throws -> [UInt8] {
        _ = try await eventually { recorder.count >= count }
        // Anything an action produced beyond the expectation comes from the
        // same I/O-thread write; give a stray byte time to land.
        try await Task.sleep(for: .milliseconds(50))
        return recorder.take()
    }

    private func accessoryButton(
        _ label: String,
        of view: UIView
    ) throws -> UIButton {
        let bar = try #require(
            view.inputAccessoryView,
            "the console has no keyboard accessory bar"
        )
        func search(_ node: UIView) -> UIButton? {
            if let button = node as? UIButton,
               button.accessibilityLabel == label {
                return button
            }
            for child in node.subviews {
                if let found = search(child) { return found }
            }
            return nil
        }
        return try #require(search(bar), "no \(label) key on the accessory bar")
    }

    /// The number of pixels within `tolerance` of an sRGB colour.
    private func pixelCount(
        in image: UIImage,
        red: UInt8,
        green: UInt8,
        blue: UInt8,
        tolerance: Int = 24
    ) -> Int {
        guard let cgImage = image.cgImage,
              let space = CGColorSpace(name: CGColorSpace.sRGB)
        else { return 0 }
        let width = cgImage.width
        let height = cgImage.height
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        let drawn = pixels.withUnsafeMutableBytes { buffer -> Bool in
            guard let context = CGContext(
                data: buffer.baseAddress,
                width: width,
                height: height,
                bitsPerComponent: 8,
                bytesPerRow: width * 4,
                space: space,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
            ) else { return false }
            context.draw(
                cgImage,
                in: CGRect(x: 0, y: 0, width: width, height: height)
            )
            return true
        }
        guard drawn else { return 0 }
        var count = 0
        for offset in stride(from: 0, to: pixels.count, by: 4)
        where abs(Int(pixels[offset]) - Int(red)) <= tolerance &&
            abs(Int(pixels[offset + 1]) - Int(green)) <= tolerance &&
            abs(Int(pixels[offset + 2]) - Int(blue)) <= tolerance {
            count += 1
        }
        return count
    }

    @Test
    func configurationIsTheApplicationsOwn() {
        let controller = SerialConsoleController(input: { _ in })
        let terminal = controller.terminal
        // A rejected configuration silently falls back to Ghostty's defaults,
        // which let a program write the host clipboard.
        #expect(terminal.lastConfigurationIssue == nil)
        guard case .generated = terminal.currentConfigSource else {
            Issue.record("unexpected source \(terminal.currentConfigSource)")
            return
        }
        #expect(terminal.renderedConfig.contains("clipboard-read = deny"))
        #expect(terminal.renderedConfig.contains("clipboard-write = deny"))
    }

    @Test(.timeLimit(.minutes(1)))
    func guestOutputReachesTheTerminalWholeAndInOrder() async throws {
        let controller = SerialConsoleController(input: { _ in })

        // Output that arrives before the view exists is kept in full, even
        // past the 1 MiB the session would keep; the first line would be the
        // first thing that limit drops.
        let first = Data(
            "FIRST \u{1b}[1;31mred\u{1b}[0m \u{2713} \u{fc}n\u{ef}c\u{f8}d\u{e9} ".utf8
        )
        let wide = Data("\u{65e5}\u{672c}\u{8a9e}\r\n".utf8)
        controller.receive(first)
        // A multi-byte character split across two chunks.
        controller.receive(wide.prefix(2))
        controller.receive(wide.dropFirst(2))
        let overwrite = Data(("\r" + String(repeating: "=", count: 40)).utf8)
        var filler = Data()
        while filler.count <= 3 << 19 {
            filler.append(overwrite)
        }
        controller.receive(filler)
        controller.receive(Data("\r\u{1b}[KBEFORE-ATTACH\r\n".utf8))

        let console = try host(controller)
        defer { console.remove() }
        controller.receive(Data("\u{1b}[10;5HPOSITIONED\r\n".utf8))

        let lines = try await viewportLines(of: controller, once: "POSITIONED")
        try #require(lines.count >= 10)
        #expect(
            lines[0].trimmingCharacters(in: .whitespaces) ==
                "FIRST red \u{2713} \u{fc}n\u{ef}c\u{f8}d\u{e9} " +
                "\u{65e5}\u{672c}\u{8a9e}"
        )
        #expect(lines[1].trimmingCharacters(in: .whitespaces) == "BEFORE-ATTACH")
        #expect(lines[9].hasPrefix("    POSITIONED"))
    }

    @Test(.timeLimit(.minutes(1)))
    func keyboardInputReachesTheGuestAsTerminalBytes() async throws {
        let recorder = InputRecorder()
        let controller = SerialConsoleController(input: { recorder.append($0) })
        let console = try host(controller)
        defer { console.remove() }
        let view = console.view
        #expect(try await eventually { view.surface != nil })
        let keyboard: any UIKeyInput = view

        // The software keyboard types text, Return, and Backspace.
        keyboard.insertText("ls -l")
        #expect(try await input(from: recorder, expecting: 5) == Array("ls -l".utf8))
        keyboard.deleteBackward()
        #expect(try await input(from: recorder, expecting: 1) == [0x7f])
        keyboard.insertText("\n")
        #expect(try await input(from: recorder, expecting: 1) == [0x0d])
        keyboard.insertText("\u{65e5}\u{672c}")
        #expect(
            try await input(from: recorder, expecting: 6) ==
                Array("\u{65e5}\u{672c}".utf8)
        )

        // Its accessory bar supplies the keys the software keyboard lacks.
        try accessoryButton("Tab", of: view).sendActions(for: .touchUpInside)
        #expect(try await input(from: recorder, expecting: 1) == [0x09])
        try accessoryButton("Escape", of: view).sendActions(for: .touchUpInside)
        #expect(try await input(from: recorder, expecting: 1) == [0x1b])
        for (label, final) in [
            ("Up Arrow", "A"), ("Down Arrow", "B"),
            ("Right Arrow", "C"), ("Left Arrow", "D"),
        ] {
            try accessoryButton(label, of: view).sendActions(for: .touchUpInside)
            #expect(
                try await input(from: recorder, expecting: 3) ==
                    Array("\u{1b}[\(final)".utf8),
                "\(label)"
            )
        }
        try accessoryButton("Control", of: view).sendActions(for: .touchUpInside)
        keyboard.insertText("c")
        #expect(try await input(from: recorder, expecting: 1) == [0x03])

        // A hardware keyboard's Control chord arrives as a key command.
        let interrupt = try #require(
            view.keyCommands?.first {
                $0.input == "c" && $0.modifierFlags == .control
            }
        )
        let action = try #require(interrupt.action)
        #expect(
            UIApplication.shared.sendAction(
                action, to: view, from: interrupt, for: nil
            )
        )
        #expect(try await input(from: recorder, expecting: 1) == [0x03])

        // Multi-line text is a paste: each line ends in a carriage return,
        // which is what a serial getty accepts as Enter.
        keyboard.insertText("echo one\necho two\n")
        #expect(
            try await input(from: recorder, expecting: 18) ==
                Array("echo one\recho two\r".utf8)
        )

        // The guest's terminal modes decide the encoding: application
        // cursor keys and bracketed paste.
        controller.receive(Data("\u{1b}[?1h\u{1b}[?2004hMODES".utf8))
        _ = try await viewportLines(of: controller, once: "MODES")
        try accessoryButton("Up Arrow", of: view).sendActions(for: .touchUpInside)
        #expect(try await input(from: recorder, expecting: 3) == Array("\u{1b}OA".utf8))
        keyboard.insertText("a\nb")
        #expect(
            try await input(from: recorder, expecting: 15) ==
                Array("\u{1b}[200~a\nb\u{1b}[201~".utf8)
        )
    }

    /// Colours are not part of the text the terminal exposes, so they are
    /// checked in what the console actually draws; the frame is attached to
    /// the result.
    @Test(.timeLimit(.minutes(1)))
    func colouredOutputIsDrawn() async throws {
        #expect(
            try await eventually {
                UIApplication.shared.applicationState == .active
            },
            "an inactive application does not draw its terminal"
        )
        let controller = SerialConsoleController(input: { _ in })
        let console = try host(controller, visible: true)
        defer { console.remove() }
        let block = String(repeating: " ", count: 24)
        let foreground = (31...37).map {
            "\u{1b}[\($0)mcolour \($0)\u{1b}[0m"
        }.joined(separator: " ")
        let background = (41...47).map {
            "\u{1b}[\($0)m \($0) \u{1b}[0m"
        }.joined()
        controller.receive(Data((
            "\u{1b}[1mKernarium serial console\u{1b}[0m\r\n" +
                foreground + "\r\n" + background + "\r\n" +
                "\u{1b}[48;2;255;0;0m\(block)\u{1b}[0m 24-bit red\r\n" +
                "\u{1b}[48;2;0;0;255m\(block)\u{1b}[0m 24-bit blue\r\n" +
                "UTF-8 \u{2713} \u{65e5}\u{672c}\u{8a9e} " +
                "\u{fc}n\u{ef}c\u{f8}d\u{e9}\r\n" +
                "kernarium login: "
        ).utf8))

        var frame: UIImage?
        let drawn = try await eventually {
            guard let image = console.view.snapshotImage() else { return false }
            frame = image
            return pixelCount(in: image, red: 255, green: 0, blue: 0) > 100 &&
                pixelCount(in: image, red: 0, green: 0, blue: 255) > 100
        }
        if let png = frame?.pngData() {
            Attachment.record(png, named: "serial-console.png")
        }
        #expect(drawn, "the 24-bit colour blocks were never drawn")
        let image = try #require(frame)
        // The theme's own background and its SGR 41 red.
        #expect(pixelCount(in: image, red: 0x21, green: 0x21, blue: 0x21) > 1000)
        #expect(pixelCount(in: image, red: 0xac, green: 0x41, blue: 0x42) > 100)
    }
}
