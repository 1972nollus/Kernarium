import SwiftUI

@main
struct KernariumApp: App {
    init() {
        // Before any view: the display and the terminal compile Metal
        // pipelines as they are created, and that reads the shader cache.
        MetalShaderCache.removeBeforeFirstUse()
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
