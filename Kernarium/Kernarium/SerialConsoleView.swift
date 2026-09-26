// SPDX-License-Identifier: GPL-2.0-or-later

import Combine
import Foundation
import GhosttyTerminal
import SwiftUI
import UIKit

/// The guest serial console, rendered by Ghostty over a host-managed byte
/// stream: guest output is fed to the terminal, and the bytes the terminal
/// produces (encoded keys, pastes, and replies to the guest's queries) are
/// written back to the guest. No process is spawned, and the console has no
/// window size of its own, so terminal resizes are not forwarded.
@MainActor
final class SerialConsoleController: ObservableObject {
    /// Receives the bytes the terminal sends to the guest. Ghostty calls it
    /// on its own I/O thread, in the order the input was produced.
    typealias InputSink = @Sendable (Data) -> Void

    /// The terminal configuration belongs to the application. No Ghostty
    /// configuration file of the user's is read, and the guest can neither
    /// read nor write the host clipboard through OSC 52.
    static let terminalConfiguration = TerminalConfiguration { builder in
        builder.withFontSize(12)
        builder.withCursorStyle(.block)
        builder.withCursorStyleBlink(true)
        builder.withCustom("clipboard-read", "deny")
        builder.withCustom("clipboard-write", "deny")
    }

    /// One colour scheme regardless of the system appearance.
    static let terminalTheme = TerminalTheme(light: .afterglow, dark: .afterglow)

    let session: InMemoryTerminalSession
    let terminal: TerminalController
    private let surfaceOptions: TerminalSurfaceOptions
    private weak var terminalView: TerminalView?
    /// Output that arrived while no terminal surface existed. The session
    /// itself would keep only the newest 1 MiB of it.
    private var pendingOutput = Data()
    private var isObserving = false

    convenience init() {
        self.init(input: { SerialConsoleController.sendToGuest($0) })
        startObserving()
    }

    /// Creates a console whose input goes to `input` and which does not
    /// observe the runtime until `startObserving()` is called.
    init(input: @escaping InputSink) {
        session = InMemoryTerminalSession(write: input, resize: { _ in })
        terminal = TerminalController(
            configuration: Self.terminalConfiguration,
            theme: Self.terminalTheme
        )
        surfaceOptions = TerminalSurfaceOptions(backend: .inMemory(session))
    }

    func startObserving() {
        guard !isObserving else { return }
        isObserving = true
        // The loader delivers every chunk on the main queue, in the order the
        // guest wrote it; anything else is a broken contract, not a race.
        kernarium_runtime_loader_set_console_output_handler {
            @Sendable [weak self] data in
            MainActor.assumeIsolated {
                self?.receive(data)
            }
        }
    }

    func stopObserving() {
        guard isObserving else { return }
        isObserving = false
        kernarium_runtime_loader_set_console_output_handler(nil)
    }

    func makeTerminalView() -> TerminalView {
        let view = TerminalView(frame: .zero)
        attach(view)
        return view
    }

    func attach(_ view: TerminalView) {
        terminalView = view
        view.delegate = self
        // The options come first: a controller installed on a view that
        // still carries the default options would build a surface that
        // executes a shell.
        view.configuration = surfaceOptions
        view.controller = terminal
    }

    func detach(_ view: TerminalView) {
        _ = view.resignFirstResponder()
        view.delegate = nil
        if terminalView === view {
            terminalView = nil
        }
    }

    func resignFirstResponder() {
        _ = terminalView?.resignFirstResponder()
    }

    func receive(_ data: Data) {
        guard !data.isEmpty else { return }
        guard terminalView?.surface != nil else {
            pendingOutput.append(data)
            return
        }
        session.receive(data)
    }

    nonisolated static func sendToGuest(_ data: Data) {
        let result = data.withUnsafeBytes { buffer -> Int32 in
            guard let baseAddress = buffer.baseAddress else { return 0 }
            return kernarium_runtime_loader_send_console_input(
                baseAddress,
                buffer.count
            )
        }
        if result != 0 {
            print("Kernarium: serial console input failed: \(result)")
        }
    }
}

extension SerialConsoleController: TerminalSurfaceLifecycleDelegate {
    func terminalDidAttachSurface(_: TerminalSurface) {
        guard !pendingOutput.isEmpty else { return }
        session.receive(pendingOutput)
        pendingOutput = Data()
    }

    func terminalDidDetachSurface() {}
}

extension SerialConsoleController: TerminalSurfaceOpenURLDelegate {
    /// Links in guest output are not opened on the host.
    func terminalDidRequestOpenURL(_: String, kind _: TerminalOpenURLKind) {}
}

struct SerialConsoleView: UIViewRepresentable {
    let controller: SerialConsoleController
    /// A hidden console keeps parsing guest output but stops drawing.
    let isVisible: Bool

    func makeUIView(context: Context) -> TerminalView {
        let view = controller.makeTerminalView()
        view.setSurfaceVisible(isVisible)
        return view
    }

    func updateUIView(_ view: TerminalView, context: Context) {
        controller.attach(view)
        view.setSurfaceVisible(isVisible)
    }

    static func dismantleUIView(
        _ view: TerminalView,
        coordinator: ()
    ) {
        (view.delegate as? SerialConsoleController)?.detach(view)
    }
}
