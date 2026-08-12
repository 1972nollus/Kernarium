import KernariumRuntime
import MetalKit
import SwiftUI

struct ContentView: View {
    var body: some View {
        GuestDisplayView { view, size in
            EmulatorLauncher.shared.start(view: view, size: size)
        }
        .onDisappear {
            kernarium_runtime_request_stop()
        }
    }
}

private struct GuestDisplayView: UIViewRepresentable {
    let viewDidLayout: @MainActor (MTKView, CGSize) -> Void

    func makeUIView(context: Context) -> GuestMetalView {
        let view = GuestMetalView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.viewDidLayout = viewDidLayout
        view.isPaused = true
        view.enableSetNeedsDisplay = true
        view.autoResizeDrawable = false
        return view
    }

    func updateUIView(_ view: GuestMetalView, context: Context) {
        view.reportLayout()
    }
}

private final class GuestMetalView: MTKView {
    var viewDidLayout: (@MainActor (MTKView, CGSize) -> Void)?

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
}

@MainActor
private final class EmulatorLauncher {
    static let shared = EmulatorLauncher()

    private var started = false

    func start(view: MTKView, size: CGSize) {
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
        print("Kernarium: runtime returned \(result)")
    }
}
