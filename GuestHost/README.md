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
Config/Signing.xcconfig                the signing settings both Xcode projects use as
                                       their base configuration
```

## Signing

Both projects take their signing from `Config/Signing.xcconfig`, which names no
team. The signer's own Apple Developer team goes in `Config/Signing.local.xcconfig`,
which the base file includes when it exists and Git ignores:

```sh
cp GuestHost/Config/Signing.local.xcconfig.example GuestHost/Config/Signing.local.xcconfig
# then set DEVELOPMENT_TEAM in it
```

Without that file a build for the iOS Simulator works unsigned and a build for
a device stops with Xcode's "requires a development team" error. Name the team
in that file, not in Xcode's Signing & Capabilities editor, which would write it
into the tracked project. A worktree is
a separate checkout and needs its own copy to build for a device.
`Kernarium/scripts/audio-interrupter.py` reads the same file (or
`UML_DEVELOPMENT_TEAM`). The team is never tracked: the
`repository-environment-portability` test fails on a literal one.

## Rules

The display forwards up to ten simultaneous UIKit contacts through the runtime
API. Each finger keeps an independent path and slot until it ends or is
cancelled. Clipping a path to the visible guest rectangle releases and restarts
only that contact: a finger that leaves the display lifts, as it did. A contact
that UIKit cancels (`touchesCancelled`, as when the system takes a touch over
for a gesture), and every contact when the display mode changes or the view
leaves its window, is cancelled in the guest instead of released
(`GuestTouchPath.cancel()` and `GuestTouchContacts.cancelAll()`, which call
the runtime's `cancel_touch`): the guest sees the contact turn into a palm and
then lift, so no tap or click follows. A contact with nothing in the guest,
because it began or has been clipped outside the display, has nothing to
cancel. The Virtio device uses Linux type-B slots and tracking IDs, sets a
finger tool type before every new tracking ID, and its legacy `ABS_X/Y`
coordinates follow the primary finger until that finger lifts.

- A file here depends only on the system frameworks, the runtime header, and
  the prebuilt `LibZstd` XCFramework. It names neither application's views,
  settings or product text; an application supplies those.
- The applications build with the same Swift settings (Swift 5 language mode,
  main-actor default isolation, approachable concurrency), because these
  sources rely on them.
- Background running, picture in picture and the console are Kernarium's, and
  stay in `Kernarium/Kernarium/`.
- An application that defines `UML_APP_STORE_APPLICATION` (Andarium's AppStore
  configuration) gets a loader without `set_frame_sink`, `send_console_input`
  and the two `automation_*` entries and their lookups; the other entries and
  the runtime API's validation are the same.
- A change here is a change to both applications: build and test both.
