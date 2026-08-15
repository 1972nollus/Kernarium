// SPDX-License-Identifier: GPL-2.0-or-later

import Combine
import Foundation
import SwiftTerm
import SwiftUI

@MainActor
final class SerialConsoleController: NSObject, ObservableObject,
    TerminalViewDelegate
{
    private weak var terminalView: TerminalView?
    private var pendingOutput = Data()
    private var isObserving = false

    override init() {
        super.init()
        startObserving()
    }

    func startObserving() {
        guard !isObserving else { return }
        isObserving = true
        kernarium_runtime_loader_set_console_output_handler {
            [weak self] data in
            self?.receive(data)
        }
    }

    func stopObserving() {
        guard isObserving else { return }
        isObserving = false
        kernarium_runtime_loader_set_console_output_handler(nil)
    }

    func attach(_ view: TerminalView) {
        terminalView = view
        guard !pendingOutput.isEmpty else { return }
        feed(pendingOutput, to: view)
        pendingOutput.removeAll(keepingCapacity: true)
    }

    func detach(_ view: TerminalView) {
        if terminalView === view {
            terminalView = nil
        }
    }

    func resignFirstResponder() {
        _ = terminalView?.resignFirstResponder()
    }

    private func receive(_ data: Data) {
        guard !data.isEmpty else { return }
        guard let terminalView else {
            pendingOutput.append(data)
            return
        }
        feed(data, to: terminalView)
    }

    private func feed(_ data: Data, to view: TerminalView) {
        let bytes = [UInt8](data)
        view.feed(byteArray: bytes[...])
    }

    func send(source: TerminalView, data: ArraySlice<UInt8>) {
        let bytes = Array(data)
        bytes.withUnsafeBytes { buffer in
            guard let baseAddress = buffer.baseAddress else { return }
            let result = kernarium_runtime_loader_send_console_input(
                baseAddress,
                buffer.count
            )
            if result != 0 {
                print("Kernarium: serial console input failed: \(result)")
            }
        }
    }

    func sizeChanged(source: TerminalView, newCols: Int, newRows: Int) {}

    func setTerminalTitle(source: TerminalView, title: String) {}

    func hostCurrentDirectoryUpdate(
        source: TerminalView,
        directory: String?
    ) {}

    func scrolled(source: TerminalView, position: Double) {}

    func requestOpenLink(
        source: TerminalView,
        link: String,
        params: [String: String]
    ) {}

    func rangeChanged(source: TerminalView, startY: Int, endY: Int) {}
}

struct SerialConsoleView: UIViewRepresentable {
    let controller: SerialConsoleController

    func makeUIView(context: Context) -> TerminalView {
        let view = TerminalView(frame: .zero)
        view.terminalDelegate = controller
        controller.attach(view)
        return view
    }

    func updateUIView(_ view: TerminalView, context: Context) {
        if view.terminalDelegate !== controller {
            view.terminalDelegate = controller
        }
        controller.attach(view)
    }

    static func dismantleUIView(
        _ view: TerminalView,
        coordinator: ()
    ) {
        _ = view.resignFirstResponder()
        view.terminalDelegate = nil
    }
}
