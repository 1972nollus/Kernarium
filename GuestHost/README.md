# GuestHost

GuestHost is the iOS source that [Kernarium](../Kernarium/README.md) and
[Andarium](../Andarium/README.md) share: what it takes to host one guest on a
Kernarium runtime framework and show it. Each application compiles
`GuestHost/Sources/` into its own module as a synchronized folder, so the types
are `internal` and `@testable import Kernarium` reaches them. It is not a
Swift package: the packages it would need are prebuilt XCFrameworks under
`.build/` and the runtime header outside any package root.

## Contents

```text
Sources/KernariumRuntimeLoader.h/.mm   loads the embedded runtime framework and
                                       wraps its C API (Runtime/KernariumRuntime.h)
Sources/GuestDisplay.swift             the Metal view that shows the guest's display,
                                       its view controller, and the SwiftUI wrapper
Sources/GuestInput.swift               touch and pointer mapping onto the guest's
                                       touchscreen range, display viewport geometry
Sources/GuestResizeHold.swift          when the guest keeps its display mode while the
                                       host view changes size
Sources/SceneCoverage.swift            whether a window scene fills its screen
Sources/GuestRestartEvent.swift        a restart the guest asked for, as the runtime
                                       reports it
Sources/LinuxKeyboard.swift            characters to Linux key codes
Sources/ZstdUtil.swift                 streamed, sparse zstd decode
Sources/MicrophonePermission.swift     the system microphone permission, read and requested
Sources/MetalShaderCache.swift         the bound on Metal's per-application shader cache
Sources/MesaShaderCache.swift          the place and size limit of Mesa's disk shader cache
Sources/GuestProcessorLimit.swift      the vCPU count the applications default to and offer
                                       at most, as the loader answers it
```

## Rules

The display forwards up to ten simultaneous UIKit contacts through the runtime
API. Each finger keeps an independent path and slot until it ends or is
cancelled. Clipping a path to the visible guest rectangle releases and restarts
only that contact. Display mode changes and removal of the view release every
contact. The Virtio device uses Linux type-B slots and tracking IDs; its legacy
`ABS_X/Y` coordinates follow the primary finger until that finger lifts.

- A file here depends only on the system frameworks, the runtime header, and
  the prebuilt `LibZstd` XCFramework. It names neither application's views,
  settings or product text; an application supplies those.
- The applications build with the same Swift settings (Swift 5 language mode,
  main-actor default isolation, approachable concurrency), because these
  sources rely on them.
- Background running, picture in picture and the console are Kernarium's, and
  stay in `Kernarium/Kernarium/`.
- A change here is a change to both applications: build and test both.
