# Kernarium iOS Phosh SIGTRAP handover

This document recorded the unfinished debugging state for the Kernarium iOS
Phosh boot/display issue on 2026-08-15. The `SIGTRAP` itself is now understood
and fixed; the sections below keep the original evidence as history and record
what the fix was.

## Resolution (2026-08-15)

The trap was host heap corruption caused by a preprocessor ABI mismatch, not by
SwiftUI, Plymouth, guest services, or thread count.

`UML_GPU_ANGLE_METAL` declares `VirglRenderer`'s `eglDisplay_` and `eglConfig_`
members, so it changes `sizeof(VirglRenderer)` by sixteen bytes and shifts every
member of every object that embeds it. It was a **PRIVATE** compile definition
on `uml_gpu_renderer`, `uml_gpu_display`, and `uml_gpu_protocol_device`, and
`uml_gpu_transport` never received it at all. On iOS device and Simulator that
produced two incompatible views of the same class:

- `virtio_gpu_device.cpp` (macro 1) — `sizeof(VirtioGpuDevice)` 512,
  `scanoutResource_` at offset 488, `scanoutRectangle_` at 492.
- `vhost_user_server.cpp` (macro undefined, so 0) — `sizeof(VirtioGpuDevice)`
  496, and therefore `sizeof(GpuVhostUserDevice)` 512 instead of 528.

`VhostUserServer`'s `std::make_unique<GpuVhostUserDevice>` runs in the second
translation unit, so every iOS run allocated 512 bytes for a 528-byte object.
The constructor and every later `SET_SCANOUT` then wrote sixteen bytes past the
allocation. macOS was never affected: the macro is 0 in all of its translation
units, so both views agree.

This exactly reproduces the captured LLDB evidence below. With the device at
base `0x104156a00`:

```text
0x104156bf0  scanoutResource_        = 7
0x104156bf4  scanoutRectangle_.x     = 0
0x104156bf8  scanoutRectangle_.y     = 0
0x104156bfc  scanoutRectangle_.width = 1320   <- last four in-bounds bytes
0x104156c00  scanoutRectangle_.height= 2580   <- next block's freelist token
```

The neighbouring words in that dump are the same object's earlier members: the
`contexts_` hash table's live count `2` and its `1.0f` max load factor, and
`transferStaging_`'s vector pointers spanning `0x11a0fdc80 - 0x119400000` =
13,622,400 bytes = 1320 x 2580 x 4.

The fix has two parts:

- `uml_configure_gpu_component` publishes `UML_GPU_ANGLE_METAL` as a PUBLIC
  definition for the whole GPU component set, so every consumer of the headers
  observes one value. `tests/unit/gpu/CMakeLists.txt` publishes the same value
  for `uml_gpu_protocol_device_test_variant`, which compiles the device source
  without linking the renderer.
- `virgl_renderer.hpp` now fails to compile when the macro is undefined, and
  `virtio_gpu_device.cpp`'s silent `#define UML_GPU_ANGLE_METAL 0` fallback is
  gone. A future PRIVATE definition breaks the build instead of the heap.

Verified: `uml_gpu_transport`'s object now emits `mov w0, #0x210` (528) before
`operator new` in `VhostUserServer::VhostUserServer`, `cmake --workflow
--preset check-gpu` passes on macOS, and both Kernarium iOS presets build
clean. A `--checked-allocations` Simulator launch runs the SET_SCANOUT path
continuously without an allocator trap.

The temporary debugging changes listed in the original cleanup checklist have
been reverted: `ContentView.swift`, `KernariumUITests.swift`,
`KernariumRuntime.mm` (`plymouth.enable=0`), and
`tests/tooling/rootfs/test_boot_overlay_systemd.py`. The user-owned
`.gitignore` change was left untouched.

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

## Historical investigation record (2026-08-15)

The remainder of this document is the state as captured before the root cause
was found. It is kept for provenance; where it speculates about likely code
areas, the answer turned out to be the build-graph ABI mismatch above.

### Most important finding

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

### Reproduction environment and commands

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

### Strong false leads already ruled down

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

### Kernel SIGTRAP handling cleanup to consider

There is a separate cleanup worth doing: host Darwin `SIGTRAP` handling can
misclassify allocator breakpoints as UML kernel-mode traps. This did not cause
the corruption, but it made the allocator stop harder to read.

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

### Notes for physical-device testing

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

### Local dependency-prefix note

`.build/kernarium-network-dependencies/ios-simulator-arm64` and
`ios-device-arm64` are older than the current `dependencies/manifest.tsv`, so
the checked presets fail their pinned-prefix verification. The matching
prefixes on this host are the `-20260815` directories; pass
`-DUML_IOS_NETWORK_DEPENDENCY_PREFIX=...` at configure time rather than
overwriting the user's prefix directories.
