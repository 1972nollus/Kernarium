# Kernarium iOS Phosh SIGTRAP Handover

This document captures the unfinished debugging state for the Kernarium iOS
Phosh boot/display issue as of 2026-08-15. It is meant for a new chat to resume
without replaying the entire investigation.

## User goal still in flight

The emulator should stably boot `phosh.erofs` into the Phosh home screen with
the correct full-screen dimensions on iOS, while macOS continues to work in the
same architecture and launch flow.

The newest user instructions to preserve are:

- Do not worry about host thread count. Revert any deliberate checks or
  workarounds whose only purpose is enforcing/counting the iOS 64-thread
  limit.
- iOS needs more time loading the Phosh UI after login. Verification should not
  fail merely because the post-login UI takes several minutes to settle.
- Keep the normal userspace startup flow. The rootfs image now owns
  `serial-getty@ttyS0`, and the `debian` user's password is `1234`.
- Avoid adding more one-off kernel or service workarounds. Keep changes
  organized and reversible.

## Current repository state

The tree was dirty before this handover was written. Treat these existing
changes as active debugging state, not accepted implementation:

```text
 M .gitignore
 M Kernarium/Kernarium/ContentView.swift
 M Kernarium/KernariumUITests/KernariumUITests.swift
 M Kernarium/Runtime/KernariumRuntime.mm
 M tests/tooling/rootfs/test_boot_overlay_systemd.py
```

Important details:

- `.gitignore` was changed from explicit rootfs filenames to `*.erofs`. This
  appears user-owned or at least unrelated to the SIGTRAP debugging. Do not
  stage or revert it unless the user asks.
- `ContentView.swift` currently contains an experimental UIKit
  `LiquidGlassControls` implementation for iOS 26. This was added while trying
  to understand the crash trigger. It should be reverted back to the simpler
  SwiftUI circular button implementation once the real memory issue is fixed.
- `KernariumUITests.swift` contains a temporary
  `testAttachAndLoginToRunningGuest` test that attaches to an already-running
  app, opens the keyboard, types `1234\n`, and hides the keyboard. Remove this
  before committing production changes.
- `KernariumRuntime.mm` currently adds `plymouth.enable=0` for non-Geekbench
  systemd boots. This conflicts with the accepted contract that Plymouth
  follows the image's normal handoff. It was an experiment and should be
  removed unless a fresh, narrow proof shows it is actually required.
- `tests/tooling/rootfs/test_boot_overlay_systemd.py` was inverted to require
  `plymouth.enable=0`; restore the previous assertion that forbids this
  workaround.

## Accepted baseline to protect

The authoritative docs currently say the accepted Kernarium checkpoint has
these properties:

- Device and Simulator use the production Virtio-GPU, virglrenderer, ANGLE, and
  Metal presentation path. No fake GPU path exists.
- iOS uses native pixels from the safe-area `MTKView` dimensions and passes
  effective DPI through EDID.
- The Phosh rootfs uses stock `display-manager -> greetd -> Phrog`, with the
  image-owned getties and credentials preserved.
- Kernarium removes only the stale development
  `graphical.target.wants/phosh.service` enablement from the disposable
  overlay.
- Empty disposable `/etc/phrog/phoc.ini` and `/etc/phosh/phoc.ini` files force
  both the greeter and logged-in session to use the Virtio-GPU EDID mode,
  avoiding the rootfs's 720x1440 Phoc config.
- macOS native Metal, VNC, and CLI behavior must remain fully functional.
- The accepted iPad validation previously booted Phosh repeatedly at 1.5 GiB
  without OOM or crash, but the latest investigation found a new crash path
  while driving the iPhone 17 Pro Max Simulator and UI automation.

## Most important finding

The current `SIGTRAP` is not primarily a SwiftUI state bug, a Phosh service
timeout, or guest kernel execution failure. The best evidence points to host
heap corruption in the iOS/Simulator display stack.

An LLDB run launched the Simulator app with checked allocations and stopped at:

```text
thread #4 queue=com.apple.frontboardservices.workspace
EXC_BREAKPOINT
libsystem_malloc.dylib`_xzm_xzone_malloc_freelist_outlined + 848
```

Backtrace shape:

```text
#0 _xzm_xzone_malloc_freelist_outlined
#1 _xzm_xzone_malloc_tiny
#2 objc cache_t::insert
#5 FrontBoardServices _queue_updateScene...
```

The allocator trap examined a corrupted tiny-zone freelist block:

```text
x4 = 0x0000000104156c00
x12 expected token = 0xe64c502ab37d8788
malloc_size(0x104156c00) = 512
```

Memory around the corrupted address:

```text
0x104156bc0: 0x0000000000000002 0x000000003f800000
0x104156bd0: 0x0000000119400000 0x000000011a0fdc80
0x104156be0: 0x000000011a0fdc80 0x0000000104134be0
0x104156bf0: 0x0000000000000007 0x0000052800000000
0x104156c00: 0xe64c502a00000a14 0x0000000000018080
```

The values are decisive:

- `0x528` is `1320`, the current guest/display width.
- `0x0a14` is `2580`, the current guest/display height.
- The width landed at the last word of a 512-byte allocation, and the height
  landed in the next 512-byte allocator slot's freelist metadata.

That is a display-dimension overwrite across a host allocation boundary.
Native Liquid Glass and XCTest accessibility activity make the allocator check
easy to trip, but they are probably crash amplifiers rather than the underlying
writer.

## Reproduction environment and commands

Simulator used during the detailed diagnosis:

```text
iPhone 17 Pro Max, iOS 26.5
UDID: 4E6609BA-F458-4741-84EE-FEC9C5C1B230
Bundle id: com.imxieyi.Kernarium
```

Useful launch/debug flow:

```sh
xcrun simctl launch --checked-allocations --wait-for-debugger \
  4E6609BA-F458-4741-84EE-FEC9C5C1B230 \
  com.imxieyi.Kernarium

lldb -p <pid>
```

In LLDB, let UML's normal host signals pass and stop on allocator traps:

```text
process handle SIGTRAP -s true -n true -p false
process handle SIGUSR1 -s false -n false -p true
process handle SIGUSR2 -s false -n false -p true
process handle SIGALRM -s false -n false -p true
process handle SIGCHLD -s false -n false -p true
process handle SIGIO -s false -n false -p true
continue
```

The temporary UI-test trigger that was used:

```sh
xcodebuild test-without-building \
  -project Kernarium/Kernarium.xcodeproj \
  -scheme Kernarium \
  -derivedDataPath .build/kernarium-xcode-derived \
  -destination 'platform=iOS Simulator,id=4E6609BA-F458-4741-84EE-FEC9C5C1B230' \
  -only-testing:KernariumUITests/KernariumUITests/testAttachAndLoginToRunningGuest
```

This test should not be kept. It exists only to drive the running app through
the keyboard/login path while LLDB is attached.

For allocation provenance, rerun with stack logging from process launch. The
previous checked-allocation run that captured the corrupted block did not have
enough stack history to identify the allocating call site:

```sh
SIMCTL_CHILD_MallocStackLogging=1 \
SIMCTL_CHILD_MallocStackLoggingNoCompact=1 \
xcrun simctl launch --checked-allocations --wait-for-debugger \
  4E6609BA-F458-4741-84EE-FEC9C5C1B230 \
  com.imxieyi.Kernarium

/usr/bin/malloc_history <pid> <corrupted-address>
/usr/bin/malloc_history <pid> <previous-512-byte-slot-address>
```

## Visual evidence

These local screenshots were produced during the diagnosis and may still exist:

```text
/private/tmp/kernarium-current-diagnostic.png
/private/tmp/kernarium-phosh-after-login.png
```

Observed states:

- `kernarium-current-diagnostic.png` showed the Phrog greeter rendered at the
  correct full guest size through native Metal.
- `kernarium-phosh-after-login.png` previously showed the logged-in Phosh home
  path at full geometry after waiting long enough.
- The absence of a quick home-screen transition on iOS should be treated as
  "still loading" for a few minutes, not an immediate failure.

## Strong false leads already ruled down

Do not spend first-pass time on these unless new evidence appears:

- "SwiftUI glass caused the bug." Native glass increases allocator churn and
  makes the overwrite trip visibly, but the overwritten data are display
  dimensions from the emulator/display path.
- "The guest screen is still quarter-sized because of host scaling." macOS
  controls showed that empty disposable Phrog and Phosh Phoc configs retain
  `1320x2580`; the rootfs's own `/etc/phosh/phoc.ini` was the source of the old
  720x1440 post-login session.
- "Phosh failed because it was too slow." iOS Phosh can take longer after login;
  wait longer before classifying it as failed.
- "The solution is to disable Plymouth." The accepted rootfs contract says
  Plymouth should follow the image's normal handoff, and the current
  `plymouth.enable=0` change is only an experiment.
- "Need to enforce thread count again." The user explicitly said not to worry
  about thread count and to revert deliberate checks around it.

## Likely code areas to inspect next

Start with display paths that allocate or pass width/height pairs around 512
byte host objects:

- `darwin/device/gpu/native_display_ios.mm`
- `darwin/device/gpu/virtio_gpu_device.cpp`
- `darwin/device/gpu/virgl_renderer.cpp`
- `darwin/device/gpu/in_process_service.cpp`
- `Kernarium/Kernarium/ContentView.swift`
- `Kernarium/Runtime/KernariumRuntime.mm`

The most suspicious boundary is not the SwiftUI overlay itself, but some
host-side structure or ABI path where the guest display dimensions are written
into a too-small allocation. Pay special attention to:

- ANGLE/Metal objects created after `MTKView.drawableSize` changes.
- virglrenderer or ANGLE structure layout mismatches between headers and
  prebuilt libraries.
- iOS-specific native resource export/import structures.
- Any Objective-C/C++ bridge object that copies `{width, height}` into a
  fixed-size buffer or descriptor.
- Any code that assumes point dimensions but receives native pixel dimensions.

The corruption pattern looked like a 512-byte object with width at offset 508
and height at offset 512. If stack logging identifies the allocation as an
ANGLE/Metal/private framework object, prefer fixing the host-side advertised
capabilities or descriptor construction over guest workarounds.

## Kernel SIGTRAP handling cleanup to consider

There is a separate cleanup worth doing once the heap overwrite is understood:
host Darwin `SIGTRAP` handling can misclassify allocator breakpoints as UML
kernel-mode traps.

The guest breakpoint path already exists through the Darwin UML execution
result path, so host process `SIGTRAP` probably should not be installed as an
ordinary UML signal handler. Relevant Linux patch-series locations:

```text
kernel/patches/series/0004-uml-add-Darwin-host-service-implementation.patch
linux/arch/um/os-Darwin/process.c
linux/arch/um/os-Darwin/signal.c
linux/arch/um/os-Darwin/skas/process.c
```

If this is changed, follow the kernel patch-series policy, not direct `linux/`
edits:

- Update patch bytes under `kernel/patches/series/`.
- Update `kernel/patches/series/manifest.tsv`, stable patch IDs and hashes,
  expected final tree metadata, and relevant kernel docs/tests.
- Verify replay with the repository's kernel tooling.

This cleanup makes host allocator traps report honestly, but it does not fix
the underlying width/height overwrite.

## Immediate cleanup checklist

Before committing a final fix, remove or revert temporary debugging changes:

- Remove `LiquidGlassControls` UIKit bridge and restore the simpler SwiftUI
  implementation. Keep the iOS 26 circular Liquid Glass button only if it is
  stable after the allocator overwrite is fixed.
- Delete `testAttachAndLoginToRunningGuest`.
- Remove `plymouth.enable=0` from `KernariumRuntime.mm`.
- Restore `tests/tooling/rootfs/test_boot_overlay_systemd.py` to forbid
  `plymouth.enable=0`.
- Do not stage unrelated `.gitignore` changes unless the user confirms them.
- Remove any deliberate thread-count assertions or diagnostics added only for
  the iOS thread-limit concern.

## Validation plan for the next chat

Use the narrowest checks first, then the owning workflow:

1. Reproduce the allocator trap with checked allocations and stack logging.
2. Identify the allocation provenance with `malloc_history` or an LLDB
   watchpoint, then fix the writer or ABI mismatch.
3. Rebuild the iOS Simulator app.
4. Boot `phosh.erofs` on the iPhone 17 Pro Max Simulator and log in as:

   ```text
   user: debian
   password: 1234
   ```

5. Wait several minutes after login before deciding the home screen failed.
6. Confirm the greeter and logged-in Phosh session both use the full native
   guest resolution, not the old 720x1440 quarter-screen mode.
7. Rebuild/run the relevant macOS path to ensure the shared display/runtime
   change did not regress macOS native Metal or VNC.
8. Run focused tests for any changed tooling contracts.
9. Run the single canonical workflow that owns the touched subsystem before
   committing.

For repository validation, the likely relevant commands are:

```sh
cmake --preset kernarium-ios-simulator
cmake --build --preset kernarium-ios-simulator

cmake --preset kernarium-ios-device
cmake --build --preset kernarium-ios-device

cmake --workflow --preset check-gpu
```

If kernel patch-series changes are made, also run the kernel replay/audit
commands documented in `kernel/README.md` and `AGENTS.md`.

## Notes for physical-device testing

The user asked to use the connected iPad as the physical device for iOS
testing. Do not run WebGL Aquarium on physical iOS right now; the user
explicitly paused that because it is too heavy for the current guest memory.

If physical boot is rechecked:

- Use `phosh.erofs` from the app Documents folder.
- Use the configured guest RAM requested by the user; the recent stable
  accepted iPad run used 1.5 GiB, but the user later reduced some flows to
  1 GiB and then asked not to trim the Phosh service set.
- Let the user handle rootfs copying when they say physical installation is
  ready.
- Do not reintroduce remote-install workarounds.

## Handoff summary

The next useful move is to catch the writer of the `1320`/`2580` overwrite.
The emulator can reach a correct full-size Phrog greeter, and prior evidence
shows a full-size logged-in Phosh screen after enough wait time. The blocking
remaining problem is host heap corruption in the iOS/Simulator display stack,
not a need for more guest service surgery.
