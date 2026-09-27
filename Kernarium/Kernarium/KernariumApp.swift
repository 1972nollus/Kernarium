import SwiftUI

@main
struct KernariumApp: App {
    init() {
        // Before any view: the display and the terminal compile Metal
        // pipelines as they are created, and that reads the shader cache.
        MetalShaderCache.removeIfBloatedBeforeFirstUse()
        // Mesa would otherwise keep a disk cache of its own below
        // $HOME/.cache. A device container does not let it create that
        // directory, so Mesa reported the failure on every launch and ran
        // without one; say so instead, here and on a Mac alike.
        setenv("MESA_SHADER_CACHE_DISABLE", "true", 1)
        // The host GL provider selects Zink through the environment when
        // it creates its display, on a runtime thread during boot. The
        // terminal's libghostty copied the environment at its first
        // initialization, so a variable added after that leaves its copy
        // pointing at freed memory and the next terminal configuration
        // crashes. Every process variable is set here, before any view.
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1)
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
