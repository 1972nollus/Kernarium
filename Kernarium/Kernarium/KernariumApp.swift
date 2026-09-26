import SwiftUI

@main
struct KernariumApp: App {
    init() {
        // Before any view: the display and the terminal compile Metal
        // pipelines as they are created, and that reads the shader cache.
        MetalShaderCache.removeBeforeFirstUse()
        // Mesa would otherwise keep a disk cache of its own below
        // $HOME/.cache. A device container does not let it create that
        // directory, so Mesa reported the failure on every launch and ran
        // without one; say so instead, here and on a Mac alike.
        setenv("MESA_SHADER_CACHE_DISABLE", "true", 1)
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
