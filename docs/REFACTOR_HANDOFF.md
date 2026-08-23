# Refactor handoff

## Common Apple ANGLE/Metal graphics transition (2026-08-21)

The Phase 1 transition in `MAX_GL_GLES_SUPPORT_PLAN.md` is complete as one
atomic product change. macOS, iOS Simulator, and iOS device now consume the
same central `apple-gpu` source matrix and offline recipe: the pinned UTM
WebKit/ANGLE, libepoxy, and virglrenderer commits plus their ordered generated
topics. The ordinary macOS prefix contains only non-graphics dependencies;
the three format-3 Apple-GPU prefixes contain static libepoxy/virglrenderer
and flat EGL/GLESv2 frameworks for their exact Mach-O platform. The former
`ios-gpu` entry point, desktop libepoxy/virglrenderer family, CGL provider,
OpenGL texture/IOSurface bridge, and OpenGL.framework link are removed.

`HostGlProvider` is now a common device-owned, fixed-layout ANGLE/Metal facade.
It requests the hardware Metal display and one ES 3 shared-context topology;
provider choice no longer changes public renderer/device layout or controls
VNC availability. `MetalPresentation` owns the common newest-frame-wins
`MTLTexture` timeline, while AppKit and UIKit retain their platform shells.
Native scanout accepts only a same-device, full-frame Metal texture; bounded
VNC/diagnostic readback remains separately counted and cannot qualify native
performance. Lifetime tests pin provider context sharing, same-thread
teardown, direct presentation, unsupported handles, readback fallback, and
the existing renderer/transport reset ordering.

The real prepared `apple-gpu` and `ios-network` repositories and all three
installed Apple-GPU prefixes verify cleanly. Dependency preparation,
regeneration, build-manifest, prefix, Apple-GPU negative-fixture, and runtime
closure tooling passed. Both canonical GPU workflows passed seven GPU unit
tests and the VNC smoke, including ASan/UBSan. The macOS integration executable
rebuilt with an exact authenticated EGL/GLES runtime closure; all four current
Kernarium runtime slices rebuilt, and packaging produced the two versioned
runtime XCFrameworks plus EGL and GLESv2 after revalidating both mobile
prefixes.

This is an implementation and packaging checkpoint, not a GL/GLES capability
promotion. No bridge ABI, rootfs, Mesa, libdrm, or CTS payload changed. A
focused 16 KiB common-path macOS diagnostic reached both DRM nodes through real
Xwayland and virgl, naturally reported desktop GL 2.1 and GLES 3.0, passed one
frozen GLES 3 shader-declaration case, exported a complete QPA, and powered off
cleanly. The same exact allow-listed case then passed 1/1 through the signed 16
KiB app on the iOS 26.5 iPhone 17 Pro Max Simulator and, only after that QPA
gate, on the connected iPad mini 6. Both reported desktop GL 2.1 and GLES 3.0
through real DRM/Xwayland/virgl and returned guest/runtime status zero. A
normal Simulator boot separately selected the common hardware provider, two
Virgl capsets, and direct Metal presentation and rendered KDE at 440x860/160
DPI. Both runtime page profiles embed the exact sentinel launcher generated
from the common driver. The thin Simulator wrapper captures and validates
framed artifacts; the physical-iPad wrapper requires its matching passing
Simulator evidence.

The current adapters additionally enforce a native macOS gadgets predecessor
before Simulator and preserve a full capability schema on every product. An
opt-in provider probe requests ES 3.0, 3.1, and 3.2 separately, captures the
successful context identity and sorted extension set, and records explicit EGL
failure codes. Virgl capsets 1 and 2 are framed with their complete raw bytes,
strictly decoded through every known v1/v2 field, and semantically diffed at
each macOS-to-Simulator-to-iPad boundary. The final accepted 16 KiB chain is
`.build/vk-gl-cts/20260821-042014-33014-16k-gles3`,
`.build/vk-gl-cts-ios-simulator/20260821-042130-33165-16k-gles3-declaration-sentinel-v1`,
and
`.build/vk-gl-cts-ios-device/20260821-042215-33330-16k-gles3-declaration-sentinel-v1`.
All three create ES 3.0, reject ES 3.1/3.2 with `EGL_BAD_MATCH`, pass the exact
case 1/1, and report zero final CPU-fallback frames. Extension counts and
format masks differ by Apple GPU as expected; their exact decoded differences
are evidence, not a requirement for false byte parity. iOS capability records
use the ordered stdout/guest-console stream because device capture may merge
separate stdout/stderr transports between bytes.

The campaign authority is now schema 2 and freezes ordered multi-case sets.
Under both 16 KiB and 4 KiB, `gles2-identity-sentinel-v1` passed 5/5 on macOS,
Simulator, and the connected iPad; `gles3-identity-sentinel-v1` passed 12/12
on the same ordered chains. Every accepted run had a matching selected-list
identity, clean shutdown, and zero CPU fallback. None of the three diagnostic
findings discovered while constructing those bounded sets was waived. The
complete macOS matrix now classifies the depth-buffer case as 15 `Pass` plus
five permitted `NotSupported` results on no-depth configurations, and the
repaired transform-feedback query passes all 20 configurations. The former
Simulator-only GLES 2 texture image-comparison failures were later repaired by
truthful Simulator swizzle and 32-bit-float-filter support. The complete
Simulator GLES 2 matrix passed under the earlier memory policy; fresh
4,096-MiB-policy mobile matrices remain required. Exact paths and hashes are
indexed in `docs/refactor-evidence.md`. Predecessor validation now
reparses the QPA and rechecks campaign, leaf, seed, selected-list, renderer,
capability, and frame evidence before launching the next product.

The first 2,048 MiB connected-iPad GLES 3 `cfg00` fraction `2/4` then exposed
one real failure at
`KHR-GLES3.core.internalformat.texture2d.rgb_float_rgb32f_linear`. The guest
and emulator completed normally, so this was not an OOM and did not authorize
a RAM decrement. The A15/Apple8 Metal query withheld 32-bit-float filtering
although the operation executes correctly. Managed UTM WebKit topic
`66766da048` retains the native query and Simulator route while adding only
the Apple8 R/RG/RGBA32F filtering path; the rebuilt,
normally signed app passed the exact same 1,624-case slice at 2,048 MiB with
1,563 `Pass`, 61 permitted `NotSupported`, clean shutdown, and zero CPU
fallback. This focused result does not replace either complete device matrix.

The first iPad shutdown emitted `Memory manager not clean during takedown`
from `drm_managed_release`. Maintained Linux topic 0020 now calls the atomic
DRM shutdown helper before unplugging Virtio-GPU, producing final replay tree
`b7ebf6944c6ed87956d27992bbef32c84c3692f7` with a 161/8 delta. Both canonical
page profiles rebuilt with LLVM 22, all four runtime slices rebuilt and
repackaged, and three consecutive Simulator plus three connected-iPad exact
sentinels passed and powered off without the warning. A focused tooling test
pins the kernel ordering, while adapter validation rejects the old signature
and any missing clean-stop marker.

The first fixed 4 KiB macOS Aquarium run then reached its measurement window
but hit two RCU stalls. The captured stacks showed Chromium's nested
`mprotect` path entering `tlb_finish_mmu`, where generic `mmu_gather` promoted
the precise range to `fullmm`; Darwin consequently repeated whole-mm walks and
executor quiesces. Maintained Linux topic 0021 adds a default-conservative
architecture hook and opts Darwin into retaining the precise range because its
synchronous invalidation already serializes every executor. Its source commit
is `28bfc7baf`, the 21-topic replay tree is
`436f25b3c4cad9cbbead1d37bad19e219ce4075d`, and the audited delta is 163/9.
A fresh LLVM 22 replay and both page-profile builds passed; accepted artifacts
`mainline-6.18-{16k,4k}-20260821-28bfc7ba-1` keep bridge ABI v8. Clean revision
`9f30073a` canaries then passed on macOS at 2.533/2.867 FPS and Simulator at
5.767/5.933 FPS for 16 KiB/4 KiB, with WebGL 2/GLES 3.0, 30 samples, clean
shutdown, and zero dropped or CPU-fallback frames. The four runs are
implementation canaries, not a controlled performance qualification or A/B.

The broader lifecycle gate remains open alongside the Simulator/iPad matrices,
seven interleaved performance pairs, connected-iPad thermal, and presentation
campaigns.
A fail-closed global display-backed diagnostic exercised the rebuilt common
emulator and exact installed `glcts`: eight module hierarchies exported before
the desktop-GL-dependent `dEQP-GL45-ES3` package requested an unsupported GL
4.5 context. Native arm64 Debian reproduced its status, 148,476 rows, and
partial digest, but that VM is comparison evidence only and is neither ground
truth nor a conformance oracle. Released-source inspection establishes why the
package is outside the GLES-only inventory; only complete official matrices in
this emulator can establish implementation compliance. The qualification
inventory now filters unmodified
`glcts` separately to `KHR-GLES2.*` and `KHR-GLES3.*`. A JIT/ten-vCPU emulator
run against the sealed derivative completed exactly those two modules with
6,972 cases, frozen SHA-256
`3ec390ed2d36f0fd8e32a02718e19c5750cf9381eab002aac9b71f848ebc6162`,
and clean shutdown. The isolated official GLES 2 matrix then passed all 25
then-advertised EGL configurations and 11,825 executions with 10,950 `Pass`,
875 `NotSupported`, no disallowed result, and clean shutdown throughout. A
later qualification defect proved that inventory included false normalized
32-bit depth configurations, so the historical GLES 2 result required a rerun
against the current 20-config product.

That current rerun at
`.build/vk-gl-cts/matrix/gles2-16k-e741cbad-z32-truthful` passed all 9,460
executions: 8,760 `Pass`, 700 released-policy `NotSupported`, no disallowed
result, and 20/20 immutable shards with acknowledged artifacts and clean
shutdown. Independent verify-only reconstruction accepted the exact 20 x 473
product.

The first GLES 3 matrix exposed a common ANGLE/Metal correctness defect at
`KHR-GLES3.shaders.loops.for_constant_iterations.conditional_continue_vertex`.
Virgl's lowered shader used `uintBitsToFloat` to carry integer loop state, but
ANGLE implemented boolean `mix` through arithmetic Metal interpolation and
flushed the selected subnormal value. UTM WebKit topic `38fc4c80bc` now emits
`metal::select` and adds a translator regression. The exact official case
changed from a 60-second fence-timeout failure to `Pass` in 465,116
microseconds under the same VNC/JIT/ten-vCPU/8-GiB immutable-root conditions.
Later shards required Virgl topics `42b8c00`/`a32f188` for ES 3.0 multisample
renderbuffers and separate transform-feedback buffers, plus WebKit topics
`a25e51c`/`45a2a7b` for per-draw transform-feedback query accounting and
strict non-finite Metal shader semantics. Focused emulator reruns passed the
implicated official cases and the complete matrix supplies campaign-wide
closure.
The regenerated product then exposed depth-blit and scissor-blit failures only
on the falsely advertised `VIRGL_FORMAT_Z32_UNORM` configurations. Guest Mesa
rejected their 24-bit-versus-32-bit depth mismatch before a host blit, while
direct ANGLE and instrumented Virgl probes proved genuine 32-bit floating-point
MSAA depth resolve. Managed Virgl topic `3a287ef` and outer checkpoint
`e741cbad` remove only the emulated GLES normalized-depth advertisement.

The resulting Release inventory has 20 window/pixmap/pbuffer configurations,
IDs 1-4, 13-16, 25-28, 37-40, and 49-52, with SHA-256
`843544a7aef987426f74851a8802447a2428bde9ff9a5d893e1c2073ffbb2607`.
The clean detached 16 KiB campaign
`.build/vk-gl-cts/matrix/gles3-16k-e741cbad-z32-truthful` passed its exact
129,960 GLES 3 executions: 125,270 `Pass`, 4,690 released-policy
`NotSupported`, zero disallowed results, 20/20 immutable shards, acknowledged
artifacts, and clean shutdown throughout. Independent verify-only
reconstruction passed, and the sealed derivative remained mode 0444 at its
authenticated hash. This completes current 16 KiB macOS GLES 2 and GLES 3.
The rebuilt signed iOS 26.5 Simulator app then ran the fixed generated
`gles3-config-inventory-v1` and `gles2-config-inventory-v1` campaigns at
`.build/vk-gl-cts-ios-simulator/20260822-093844-4028-16k-gles3-config-inventory-v1`
and
`.build/vk-gl-cts-ios-simulator/20260822-093938-4195-16k-gles2-config-inventory-v1`.
Each returned zero through Gadgets, direct ANGLE/Metal presentation, and the
sealed ext4 lower below OverlayFS, and each matched its VNC/JIT/ten-vCPU macOS
predecessor exactly: the same 20 config tuples and canonical digest
`843544a7aef987426f74851a8802447a2428bde9ff9a5d893e1c2073ffbb2607`.
Both recorded zero dropped and CPU-fallback frames. The adapters expose no
arbitrary selector and fail closed on any predecessor config mismatch; both
records also pass the connected-device predecessor validator. Complete
Simulator/iPad case matrices and Phase 2 performance, thermal, and lifecycle
gates remain open. The original image also lacks a build receipt, so future
qualification must use the checked derivative. Phase 3 and later profile work
must not start until the lower shared baseline is measured and qualified.

The signed Simulator GLES 2 matrix subsequently completed all 9,460 current
executions at
`.build/vk-gl-cts-ios-simulator-matrix/gles2-16k-2275c9b4-full`: 8,760
`Pass`, 700 permitted `NotSupported`, no disallowed status, and exact agreement
with the macOS aggregate. Before that full run, UTM WebKit topics `b2710939ab`
and `cab62532b4` enabled arm64-Simulator texture swizzles and truthful 32-bit
float filtering after focused native-Metal and CTS diagnostics proved both
features work despite the Simulator's missing GPU-family/property reports.
Those changes repaired all 25 config-ID-1 GLES 2 texture failures without a
guest change or waiver.

The original signed GLES 3 layout used one complete 6,498-case CTS process per
config. Its first two Simulator configs were reproducibly terminated by the
guest OOM killer after 2,255 passing results, with `glcts` at 894 and 874 MiB
RSS and total anonymous memory exhausting the fixed 1.5 GiB guest. This was a
qualification-process lifetime defect, not a CTS result or permission to add
swap or raise the product default; both incomplete attempts remain preserved
under `.build/vk-gl-cts-ios-simulator-matrix/gles3-16k-2275c9b4-full`.

Checked schema-2 `vk_gl_cts_apple_matrix.json` therefore expands to 100
compiled IDs: one full GLES 2 leaf per config and four deterministic GLES 3
slices per config. Each GLES 3 slice has 1,625, 1,625, 1,624, or 1,624 cases
and a signed selected-list digest derived from the authenticated complete
macOS leaf. The mobile aggregate reconstructs the original case order for
every config and requires the exact frozen leaf hash, so a missing, duplicate,
or reordered case fails closed. Simulator qualification accepts the existing
complete passing macOS config run as the stronger predecessor for all four
slices; connected-device qualification still requires the exact matching
passing Simulator slice. Both mobile adapters retain the authenticated
derivative, signed inventory, fixed Gadgets engine, immutable-attempt, and
clean-shutdown requirements. Current source now makes the memory policy
explicit: 4,096 MiB on Simulator and 2,048 MiB initially on a physical device.
The device adapter accepts only 64 MiB decrements after a confirmed emulator
OOM, and every run records the exact value. Existing 1.5 GiB inventories are
therefore historical inputs. Fresh 4,096 MiB inventories now pass at
`.build/vk-gl-cts-ios-simulator/20260822-131649-63237-16k-gles3-config-inventory-v1`
and
`.build/vk-gl-cts-ios-simulator/20260822-131735-63365-16k-gles2-config-inventory-v1`,
with the same 20 truthful configurations and canonical digest as macOS.

The first sliced GLES 3 diagnostic had eight depth/stencil shadow
texture-repeat failures because arm64 Simulator omits Metal GPU-family reports
and ANGLE consequently replaced every sampler comparison with
`MTLCompareFunctionNever`. Managed UTM WebKit topic `fff809d63b` enables the
native sampler comparison supported by the modern arm64 Simulator while
leaving macOS, physical-device, and other Simulator architectures on their
existing predicates. After both mobile prefixes, all four runtimes, the
XCFrameworks, and the clean Debug app were rebuilt, exact `cfg00` fraction
`0/4` passed all 1,625 executions with 1,563 `Pass`, 62 permitted
`NotSupported`, zero failure, and zero CPU fallback at
`.build/vk-gl-cts-ios-simulator/focused/gles3-shadow-sampler-fff809d/20260822-131903-63508-16k-gles3-matrix-cfg00-000of004-v1`.
All eight implicated cases now pass. Complete fresh Simulator and iPad
campaigns remain the next gate.

## Versioned kernel ownership preparation (2026-08-20)

The former singleton kernel layout is split into
`kernel/mainline-6.18` and `kernel/common`. The mainline family owns its
source identity, generated patches, complete configuration, compiler policy,
and standalone scripts. Common ownership is limited to the unchanged bridge
ABI, shared 16 KiB page/task-stack geometry, parameterized series verifier, and
prebuilt-artifact contract. Root `kernel/CMakeLists.txt` is now import-only: it
validates accepted local bundles and exposes their files to the unchanged
current product targets without running Git, replay, toolchain selection, or
Kbuild. The Android ACK series, AOSP product, static disk/initramfs producer,
and accepted release are separately owned by `kernel/android-6.18` and
`android/aosp-17`. A separate 16 KiB-only macOS executable now consumes that
accepted release; the signed iOS Android runtime remains later work.
The accepted ACK source now has 26 generated topics. Topic 24 services
throttled cooperative timers from IRQ-enabled kernel spin checkpoints. Topics
25 and 26 add safe non-atomic uaccess and completed host-TLB quiescent
boundaries, polling the cooperative timer before `cond_resched()` so a missed
deadline can raise `need_resched()` and awaken RCU. The accepted built-in-only
release `20260821-5735514a-rcu-timer1` ran a four-vCPU gadgets cold boot to
guest second 982 and a same-COW warm boot to second 284 without an RCU-stall,
timer-wakeup, oops, or panic signature; the previous late failure occurred at
second 816. `ttyS0` retained kernel/init output while independent `hvc0`
sessions returned the Android shell and main logcat buffer. SurfaceFlinger
still aborts repeatedly and `sys.boot_completed` remains unset, so graphical
Android application readiness remains open.

The default JIT then exposed a separate host-side SMP deadlock before that ACK
timer gate: a native LSE RMW/CAS block published its direct write before a cold
fallback entered the serialized exclusive helper. One vCPU could own the
exclusive turn while waiting for another vCPU's publication, as that second
vCPU waited for the turn with its own publication still active. Commit
`2a28713a` defers that fallback until `run_block()` has unwound publication;
the scalar-atomic code-generation regression rejects a direct serialized-helper
call from every native atomic block while retaining a semantic-memory positive
control. A fresh-COW, four-vCPU default-JIT Android run using the unchanged
accepted release kept the `hvc0` shell and main logcat buffer responsive through
guest second 319.98, powered off normally at second 368.49, and emitted no
observed RCU-stall, oops, or panic report on `ttyS0`. Both host page-profile
workflows passed all unit tests (576/576 at 16 KiB and 578/578 at 4 KiB); each
tooling matrix passed apart from the sandbox's Unix-socket `EPERM`, whose exact
`gpu-guest-rootfs-safety` rerun passed outside that restriction.

## Android 17 guest reaches a usable desktop (2026-08-23)

The Android 17 guest now boots to `sys.boot_completed=1` with SystemUI, the
Launcher3 taskbar, and third-party applications drawing through
Mesa/virgl/`hwc3-drm`. Accepted release `20260823-bb3cf26e-napi1` carries ACK
topics 29 and 30; `android/aosp-17/metadata/accepted-release.env` records its
identity.

Four defects had to close before the guest was usable, and each is worth
keeping visible because none of them announced itself as what it was:

- The screen showed only wallpaper. The guest was at the lock screen, and its
  window never drew: `dumpsys SurfaceFlinger --list` had no `VRI-` layer for
  `QuickstepLauncher`. Nothing was broken in composition.
- Clicks and taps did nothing because Android never opened the pointer at all.
  `EventHub` classifies an absolute device only when it also reports
  `BTN_TOUCH`, and a device that matches no class is dropped before
  `InputReader` sees it, so `dumpsys input` listed only the keyboard. ACK topic
  29 reports `BTN_TOUCH` and marks the device `INPUT_PROP_DIRECT`; the guest
  now lists `Darwin UML VNC Tablet, Classes: TOUCH, Sources: TOUCHSCREEN`.
  Topic 10 and the mainline series are untouched, so the Linux desktop guests
  keep their absolute mouse.
- The keyboard looked dead because `KEY_LEFTSHIFT` and `KEY_LEFTMETA` were
  stuck down: AppKit stops delivering `keyUp` and `flagsChanged` once its
  window is no longer key, so Command-Tab stranded them for the session. The
  native shell now releases everything it believes is held at that boundary and
  reads the device-dependent modifier bits, which distinguish the two sides of
  a pair.
- `adb push` of a large file wedged the guest every time. The Darwin
  user-network driver drained frames from the interrupt handler on a
  level-triggered descriptor, which is a receive livelock; the block stack was
  never implicated, taking a 3 GiB guest-local write in 10 s at 293 MB/s on the
  same boot. The wedged guest still took ticks - RCU reported "2515 ticks this
  GP" - and its carriers spun in `darwin_cpu_relax` and `_raw_spin_lock_bh`.
  ACK topic 30 moves the receive path onto NAPI, following
  `arch/um/drivers/vector_kern.c`. The same 512 MiB push then completed in
  35.8 s at 14.3 MB/s with matching SHA-256 and no stall of any kind.

The macOS launcher gained `--net-forward`, so the guest's adbd is reachable
without special-casing: `--net-forward=5555-5555` plus
`adb connect 127.0.0.1:5555` gives the ordinary `adb install`, `adb shell`, and
`adb push`. The product already sets `persist.adb.tcp.port=5555`. The boot
command line no longer disables the boot animation, which now runs for the
length of the cold boot.

A much longer transfer still ends badly, and for a different reason. A 2 GiB
push on the NAPI release transferred completely, but degraded from 14.3 MB/s to
2.7 MB/s over 750 s and then stopped responding. Every one of the eleven RCU
stalls on that boot reports `0 ticks this GP` on CPU 0 sitting in
`default_idle_call`, which is the Darwin cooperative-timer starvation signature
that topics 24 through 28 already address, not the receive livelock topic 30
removed - that signature (ticks present, carriers spinning) did not occur once.
An idle carrier losing its timer under sustained load is therefore the next
thing to chase, and it is pre-existing rather than introduced here.

Two limitations stay open. A booted guest holds roughly 1400 order-3
(128 KiB) task stacks, which pins about 176 MiB of the zone in unmovable blocks
and fragments the buddy allocator past what compaction recovers: an observed
boot held 147 MiB free and 2.6 GiB `MemAvailable` with no free block at order 3
or above, 188 of 230 compaction attempts failing, and six global OOM kills of
background applications. `CONFIG_VMAP_STACK` is the usual remedy and was
measured to be much worse here - this port's vmalloc window is only about
375 MiB, and vmapped stacks exhausted it at guest second 175 and panicked in
`exit_mmap` at second 203. Widening that window is address-profile work.
`kernel/android-6.18/README.md` owns the detail.

## Required case-sensitive kernel source configuration (2026-08-20)

The maintained Linux checkout now comes only from the Git-ignored
`kernel/mainline-6.18/source-location.conf`. Copy the tracked example and set
one absolute `UML_KERNEL_SOURCE_DIR`;
`kernel/mainline-6.18/scripts/source-location.sh` parses it as data,
canonicalizes the location, and proves case sensitivity with a case-collision
probe. Preparation, Kbuild, delta audit, and patch regeneration consume that
single value. Apple CMake consumes accepted artifacts only and never reads the
source configuration. A missing/malformed config, source mismatch, missing
checkout during standalone build, or case-insensitive location remains a hard
producer failure. Fresh replay verification writes an isolated disposable
config beside its temporary checkout.

## Git-only dependency and kernel provenance (2026-08-20)

All project-owned external-library manifests, platform receipts, patches,
builders, and CMake prefix verifiers now live under `dependencies/`. Every
source row names an upstream HTTPS Git repository and full commit; optional
changes replay as an ordered Git mail series. Source archives, extraction
identities, prepared-tree hashes, `Kernarium/dependencies/`, and Kernarium’s
dependency build scripts/CMake modules are retired. The kernel keeps the same
model under its versioned `kernel/mainline-6.18` ownership boundary: stable
repository branch `linux-6.18.y`, commit
`2fe596715f840d053aed5cee5455f701bdcd2b50`, and final replay tree
`f78217b082f6becb8af1656bcc69521307eb729b`.

Patch files are generated artifacts in both domains. Modify topic commits with
`git rebase -i` and `git commit --amend`, then run
`dependencies/rebase-patches.sh` or
`kernel/mainline-6.18/scripts/rebase-patches.sh`; never edit mail patch hunks
or headers manually. Focused dependency preparation/generation/
prefix tests, kernel Git materialization, canonical replay verification, delta
audit, and a fresh pinned-commit kernel replay without compilation passed.
Earlier archive-based evidence below is retained only as dated history and no
longer describes the preparation or patch-maintenance workflow.

## Desktop PipeWire userspace ownership (2026-08-19)

The unified desktop image registers the VirtIO ALSA card but contains
`pipewire` and `pipewire-pulse` without WirePlumber or
`pipewire-media-session`. Plasma consequently advertised only its dummy output
after the host and kernel audio paths were already working. The standard
macOS KDE launcher passes `--audio`, while Kernarium attaches the same host
device. ALSA discovery and application routing remain guest-userspace policy.
The accepted resolution is a whole-image update that adds a packaged session
manager such as WirePlumber to the authenticated EROFS rootfs. The macOS KDE
and Kernarium overlays do not install a static sink, link router, or substitute
session policy.

An escalated macOS guest probe using the production integration binary proved
the kernel card and `/dev/snd/pcmC0D0p`. A full KDE boot using the production
overlay experiment then created an automatically linked PulseAudio stream,
wrote and drained a 440 Hz tone through `uml-virtio-sound`, and shut down
cleanly. That experiment was not retained because a packaged session manager
owns the behavior. It also exposed Linux's valid repeated PREPARE sequence,
which the host had incorrectly rejected. The retained host fix implements the
repeatable SET_PARAMS and PREPARE transitions plus RELEASE-to-PREPARE re-entry
required by the VirtIO PCM lifecycle. The protocol and KDE launcher tests pin
the retained behavior. The image still lacks `aplay`, so documentation and
probes must not claim that command as an image prerequisite.

## Apple Virtio Sound playback (2026-08-18)

Both Apple application compositions now include a standard Virtio Sound
backend. Kernarium attaches it automatically on iOS Device and Simulator; the
macOS CLI attaches it with the host-only `--audio` option. The card advertises
one output-only S16_LE stereo stream at 48 kHz and one FL/FR channel map. It
uses the upstream built-in `snd_virtio` ALSA driver rather than adding a public
or host/kernel ABI. `CONFIG_SOUND`, `CONFIG_SND`, and `CONFIG_SND_VIRTIO` are
built in, while `CONFIG_UML_SOUND` stays off.

AVAudioPlayerNode owns a bounded set of preallocated period buffers. TX
descriptors complete only from the data-consumed callback; STOP/RELEASE flush
outstanding periods without leaving the Linux driver waiting. iOS selects the
playback AVAudioSession category and observes interruptions plus route/engine
configuration changes. The generalized vhost-user server now derives its
queue count from the protocol device and retains deferred descriptor chains
until an event-source completion publishes them to the used ring.

The normal and ASan/UBSan GPU/audio workflows, four-queue deferred transport
fixture, both macOS page-profile product links, both iOS Simulator runtime
links, and macOS/iOS Device/iOS Simulator audio compiles passed for both page
profiles. Real 16 KiB and 4 KiB guests both discovered the card; libasound
writes played and drained a
4,800-frame 440 Hz period with all four hardware periods completed in order.
The focused `snd_timer01` race then passed 1/1 on both profiles. The
physical-iOS full runtime link is currently blocked before linking by its
pre-existing stale iOS network dependency-prefix
manifest, although both the physical audio archive and modified runtime source
compile for the iPhoneOS SDK.

## macOS WebGL instancing shader fix (2026-08-17)

The macOS virglrenderer no longer rejects an otherwise valid instanced vertex
shader by requiring `GL_ARB_draw_instanced` from Apple's core-only OpenGL 4.1
driver. The generated desktop shader already selects GLSL 1.40, where
`gl_InstanceID` is core, but the generic requirement table still emitted the
legacy extension directive. Its compile failure put virglrenderer into an
illegal-shader context error; later Virtio-GPU 3D submissions then received
`VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID` (`0x1204`), explaining both the error
flood and the apparent Chromium wedge.

The pinned virglrenderer patch now omits only that extension directive for
desktop GLSL 1.40 or newer. Older desktop GLSL and GLES retain the upstream
extension path. The patch's existing no-video helper insertion also gained
real source context: the former zero-context hunk inserted the helpers inside
the following function when applied to a fresh authenticated 1.3.0 archive,
even though the previously prepared tree had the intended layout. The updated
patch applies cleanly from the archive and the manifest records its new patch
and prepared-tree hashes.

A network-independent WebGL2 workload using `gl_InstanceID` reproduced the
reported signature with the prior macOS binary: the illegal extension error
was immediately followed by four `0x1204` replies before the bounded run
stopped. The identical workload on the rebuilt binary rendered 512 colored
instances for 30 verified frames with ANGLE/Mesa/virgl and emitted neither
signature. The current public three.js instancing-performance page was also
run in the rebuilt guest: it built 1,000 instanced meshes, rendered 38 verified
frames, produced a nonblank VNC capture, and emitted no shader or invalid-
context error. The current page did not trigger the negative signature on the
prior binary in the matched run, so the smaller `gl_InstanceID` workload is the
accepted negative control rather than a claim that every page load fails.
Both browser A/Bs used diagnostic `--single-process --in-process-gpu` and
`--no-sandbox` flags because this checkout's ordinary Chromium child processes
timed out before navigation. They still traverse the production guest Mesa,
Virtio-GPU, and host virglrenderer path; the flags are not a launcher change or
product recommendation.

The authenticated dependency source verification and full private-prefix
rebuild passed. Both the normal and ASan/UBSan GPU gates passed five ownership
tests plus the VNC smoke test; the five dependency tooling tests and the
architecture audit also passed. Local logs and captures are indexed in
[refactor-evidence.md](refactor-evidence.md).

## Guest namespaces restored, and a config guard (2026-08-17)

The shipped kernel had no namespaces at all. `CONFIG_EXPERT=y` makes
`NAMESPACES` a visible prompt whose default becomes `n` (`default !EXPERT` in
`init/Kconfig`), and `common.fragment` never named it, so `olddefconfig`
silently discarded the `CONFIG_USER_NS` and `CONFIG_TIME_NS` the fragment did
ask for and took PID, network, UTS and IPC with them. The guest had only
`/proc/<pid>/ns/{mnt,cgroup}`, no `uid_map`, and `unshare` returned `EINVAL`.

`guest-overlay-systemd-smoke` failed on both page profiles because of it:
dockerd stats `/proc/<pid>/ns/pid` while initializing buildkit, exits 1, and
`docker.service` never reaches active. The fragment now names
`CONFIG_NAMESPACES=y`, both profiles rebuilt, and that test passes in 33 s.

`verify_config_fragment` in
`kernel/mainline-6.18/build/build-kernel.sh` now compares both
fragments against the resolved `.config` after `olddefconfig` and fails the
build on any divergence, so this class cannot recur silently. Applying it
found two more: `CONFIG_HZ_100` and its three companion choice symbols never
existed for this port, because UML declares its own integer `CONFIG_HZ` and
never sources `kernel/Kconfig.hz`; and `# CONFIG_DEBUG_KERNEL is not set` was
unachievable because `EXPERT` selects it. Both are corrected in the fragment,
and `darwin-vcpu-carrier-policy` now pins the real single `CONFIG_HZ=100`
setting and rejects the choice symbols.

`DEBUG_KERNEL` itself stays on because `EXPERT` selects it, but the two symbols
it defaulted on that nothing requested are now off: `CONFIG_DEBUG_MISC` and
`CONFIG_RCU_TRACE`. The second is the one with a hot path — it ungated 116
`trace_rcu_*` sites, and this port has no `CONFIG_JUMP_LABEL` to patch them to
NOPs — so 19 of the 21 `TRACE_EVENT_RCU` tracepoints now compile out and
`vmlinux` is 11,104 bytes smaller. `rcu_utilization` and `rcu_stall_warning`
use plain `TRACE_EVENT` and survive by design. No A/B measured the cost, and
none is claimed: these were removed because a selected menu gate turned them
on, not because a measurement asked for it.

## EL0 identification-register emulation (2026-08-16)

Guest `AT_HWCAP` now advertises `HWCAP_CPUID`, and all three engines emulate
EL0 `MRS` reads of the AArch64 identification registers over the same encoding
space arm64 Linux emulates from its undefined-instruction hook (Op0 3, Op1 0,
CRn 0, CRm 0 or 2 through 7). These reads are UNDEFINED in hardware and were
previously reported to the guest as illegal instructions.

`darwin/cpu/arm64/id_registers.hpp` is the single source of the returned
values. It builds them from the same host predicates `virtual_hwcap()` uses,
so both ways a guest can ask about a feature give the same answer and neither
can name an extension no engine executes; fields Linux hides from EL0 read as
zero. Unallocated members of the group remain illegal, as they are on arm64.

Found by running Wine under the 4 KiB profile. Wine's unix-side `ntdll.so`
reads `ID_AA64PFR0_EL1` unconditionally during process start; the SIGILL
arrived on Wine's unix stack, where its exception dispatcher refuses to run, so
every Windows program died with `could not load kernel32.dll`. The accepted
proof is a `xfce-wine.erofs` guest under gadgets with `ncpus=4`, `mem=4G`, and
a 1280x720 VNC display, running `winecfg` from a fresh prefix to a stable Wine
configuration window captured through `uml-vnc-smoke-client`, followed by a
clean `result=0 exitcode=0` shutdown. The missing `wine32:i386` package and the
`syswow64\rundll32.exe` failure that follows from it are unrelated to the
emulator and remain expected in this image.

## Kernarium iOS application prototype checkpoint (2026-08-15)

`Kernarium/Kernarium.xcodeproj` boots the maintained UML kernel from a basic
SwiftUI `ContentView` into the production Virtio-GPU/virglrenderer command path.
UTM's pinned libepoxy and virglrenderer drive its ANGLE Metal backend, and the
resulting native `MTLTexture` is sampled by the app's `MTKView`. The macOS
AppKit/CGL/IOSurface/Metal and VNC providers remain platform-exclusive. The
desktop virgl path now restores context-zero bookkeeping after creating a
guest context and tolerates only the no-codec video-buffer lifecycle emitted by
Mesa's shader-only `VAProfileNone` probe; actual codec capabilities and frame
commands remain disabled.

The app starts one 1.5 GiB guest by default after its safe-area view has a real
size. Retina mode is off by default: safe-area points map directly to Metal and
guest pixels, and Virtio-GPU EDID advertises 160 DPI. The optional Retina mode
multiplies the same point geometry by `UIScreen.nativeScale` and advertises an
effective `160 * nativeScale` density. Touch coordinates are scaled into the
selected pixel space. Device and Simulator share both policies and default to
all processors reported by iOS. Kernarium selects the built-in VGA8x16 fbcon
font at point density and TER16x32 at Retina density so early-console glyphs
retain approximately the same physical size.
A settings sheet can persist 1 through that count, 512 MiB through 4 GiB of
RAM in 128 MiB steps, a 1--32 GiB overlay, and the selected Documents `.erofs`
filename for the next launch. It also persists Retina mode and a 4 KiB or
16 KiB kernel page profile; Retina defaults off and the kernel defaults to
16 KiB. A 32-point display button between settings and keyboard switches
between the framebuffer and a SwiftTerm serial terminal; framebuffer is the
initial surface. Both views remain mounted so the terminal retains all boot
output and the Metal/Virtio-GPU path continues presenting while hidden. The
same ordered `ttyS0` stream still reaches app stdout, while input typed into
SwiftTerm is returned to that serial endpoint. The app recreates the sparse overlay disk on
every run; the guest formats it as ext4 and uses separate directories from its
shared free-space pool for OverlayFS, Docker, and containerd before executing
the image's normal systemd or OpenRC init. No disk-backed swap is
created or activated. Documents file sharing and Extended Virtual Addressing
remain project-owned metadata.

`pmos-sxmo.erofs` is now a supported Documents selection. Its disposable
overlay preserves the image's UID-10000 account, tinydm default session, and
Sxmo/Sway entry point, while enabling the image's udev coldplug under UML,
supplying minimal no-swap deviceinfo, classifying Virtio input, and granting
the `video` and `input` groups device access after udev settles. The rebuilt
common kernel includes postmarketOS's mixed-family nftables firewall path. An
exact production-bootstrap macOS run completed DHCP, OpenRC udev/coldplug and
firewall startup, started tinydm, and produced a 440x860 Sxmo/Sway frame over
VNC; the image's account, runlevel, and session were not replaced.

The default macOS integration target and both 16 KiB iOS runtime slices consume
the same accepted 16 KiB mainline bundle. The macOS 4 KiB compatibility target
and both 4 KiB iOS slices likewise consume the accepted 4 KiB bundle. CMake
derives the ELF only after manifest, source, page, ABI, configuration, import,
and inventory checks. Kernarium packages them as
`KernariumRuntime_6_18_41_16K` and `KernariumRuntime_6_18_41_4K`, embeds both
without linking either at process launch, and dynamically loads exactly the
saved profile for the process lifetime. The selected framework places its ELF
in an aligned `__TEXT` section covered by Xcode's standard code signature,
while macOS retains its established unsigned CLI embedding. Every Apple
product exposes the same 128 TiB sparse software-MMU task range and derives a
native window from guest RAM plus a page-rounded one eighth for auxiliary
kernel mappings, with a 32 MiB floor.
Device and Simulator top-pack that window below 62 GiB.
The always-linked application loader keeps `SIGALRM` blocked before main,
validates the selected framework's release, page size, and immutable C API,
then delegates to that isolated runtime. The iOS ELF loader aliases immutable
executable pages from signed storage and copies only data/BSS. Darwin's linker
keeps executable boundaries aligned to 16 KiB host pages for both guest
profiles, and packaging rejects an unsafe carrier layout. With the 1.5 GiB
default, the auxiliary allowance is
exactly 192 MiB and the active window is 60.3125--62 GiB. It uses no anonymous
executable publication, RW-to-RX transition, LLDB, or JIT entitlement. macOS
exercises the same range, window derivation, relocation, and exclusion-hole
handling while retaining its 8 GiB and ten-vCPU defaults; its 1 GiB auxiliary
allowance produces a 9 GiB active window starting at 96 TiB. The separately
tested 4 KiB compatibility profile retains its necessarily distinct
compile-time page-profile artifact on macOS and iOS.

Current dual-runtime acceptance built all four Device/Simulator framework
slices and packaged both versioned XCFrameworks. The iOS 26.5 iPhone 17 Pro Max
Simulator Xcode campaign passed five app tests and six UI tests; separate app
launches selected each saved profile, logged the matching 4 KiB or 16 KiB
runtime, completed DHCP and the EROFS/ext4 pivot, and reached
`graphical.target` plus greetd. A later 16 KiB focused display-mode check on
that Simulator booted point mode at 440x860/160 DPI with VGA8x16 fbcon
(55x53 cells), then Retina mode at 1320x2580/480 DPI with TER16x32 fbcon
(82x80 cells); both reached the full-geometry Phrog keypad. Console samples
such as `[    0.004448]` also proved that printk timestamps are no longer
quantized to the 10 ms tick. A generic physical-iOS Release build succeeded
with standard Xcode signing and embedded both runtimes without linking either
into the app executable. Both current macOS page profiles pass 53/53 tooling
checks and the interpreter, gadgets, JIT, and SLiRP guest cases. Their common
external `rootfs.erofs` currently stalls only the Docker overlay smoke after
containerd starts, so that matched 4/5 result is recorded as a rootfs campaign
limitation rather than attributed to kernel selection. Exact current evidence
is indexed in [refactor-evidence.md](refactor-evidence.md).
Fresh post-clock-change gadgets shell boots also passed on both rebuilt macOS
profiles against `phosh.erofs`: 16 KiB emitted `[    0.004062]`, 4 KiB emitted
`[    0.005343]`, both completed DHCP and the disposable overlay, and both
returned their profile marker before clean poweroff.
For the postmarketOS addition, both page-profile kernels and all four iOS
runtime slices rebuilt, the four XCFrameworks repackaged, and generic iOS
Simulator and physical-iOS Debug app builds succeeded. No Simulator was booted
for this check; the rendered guest qualification used the macOS host.

The Darwin port now owns exactly one persistent host carrier for each
configured vCPU, including vCPU0. Linux tasks keep stackful context on 128 KiB
kernel stacks and migrate between carriers without creating host pthreads.
Carriers are named `uml-vcpu-N-carrier`; host-stop wakes them cooperatively,
and IRQ-enabled spin checkpoints service pending IPIs so synchronous
cross-calls cannot deadlock. Shipped kernels use a 100 Hz tick and
`NO_HZ_IDLE`. Kernarium leaves the launcher on its existing Swift worker and
blocks the process-directed early `ITIMER_REAL` signal on application and
launcher workers; only vCPU0's carrier unmasks `SIGALRM` during calibration.
The common kernel also enables `CONFIG_PRINTK_TIME` for timestamped VT and
host-serial printk records. Normal UML mode now supplies those prefixes from a
boot-relative `CLOCK_MONOTONIC` nanosecond scheduler clock instead of 100 Hz
jiffies; the 100 Hz event/tick policy remains unchanged, and deterministic
time-travel modes retain their simulated-jiffies clock.

The common architecture was requalified with a complete no-systemd gadgets
Geekbench 6 run: the direct OverlayFS shell scored **96/565** single/multi,
versus the final pre-change result at **96/567**. Diagnostic single/multi Clang
and Object Detection captures put carrier activation plus synchronization
below 1.2% of accumulated guest+syscall time and found no material spin/IPI
leaf. The remaining Clang link churn and Object Detection SDOT/vector-load
hotspots match the pre-change gadget profile. Two new SDOT stream-load arms
were removed after only 0.00% and 0.39% focused-probe gains. The result does not
justify a macOS-only memory, address-space, or thread-model shortcut.

A separate clean full run of the current 4 KiB compatibility product scored
**92/417** single/multi. Against the current 16 KiB result above, that is
**-4.17%/-26.19%**; against the older G35 4 KiB result at 79/416, it is
**+16.46%/+0.24%**, though the historical comparison crosses substantial
source and harness changes. A separate unscored `vmmap` pass proved that this
4 KiB product relocated its executable kernel to `0x600000000000` (96 TiB)
and mapped the 8 GiB physical backing in the same high native window rather
than directly above 64 GiB. The scored run was untouched by inspection and
powered off cleanly. The current page-profile comparison has one observation
per profile; its wider multi-core gap is therefore evidence for the current
performance boundary, not a causal attribution. The pre-dual-runtime canonical
4 KiB workflow passed 53/53 tooling checks and 5/5 fast guest integrations;
the focused loader ASan/UBSan gate and the 16 KiB five-guest set also passed.

A requested four-vCPU comparison then used Geekbench's working
`--multi-core --no-upload` path, so neither profile executed the single-core
suite. The current 16 KiB product scored **309** multi-core (263 integer / 415
floating point), while the current 4 KiB product scored **269** (226 / 373):
**-12.94%** aggregate, **-14.07%** integer, and **-10.12%** floating point.
Both completed all 16 workloads and powered off cleanly. The gap ranged from
-1.23% in Background Blur to -35.36% in PDF Renderer; 4 KiB elapsed time was
17.53% longer while peak resident size changed only +0.35%. This ordered
single observation per profile ran on an actively used host, so it is a
directional current comparison rather than a low-noise causal A/B. G148 owns
the complete table, exact product and console hashes, and measurement limits.

The common kernel also builds zswap in, enables it by default, and selects zstd
while retaining zram with zstd. Zswap requires backing swap and allocates no
pool in the normal macOS or Kernarium desktop flows, which deliberately do not
activate a swap device or file.

Two real Virtio-input devices carry UIKit touch coordinates and keyboard text
to the guest. The 32-by-32-point trailing-center settings, keyboard, and
special-key tray controls are circular and bordered on older systems and share a native
Liquid Glass container on iOS 26. The tray expands left and emits
Virtio-keyboard press/release pairs for Power, Volume Up, Volume Down, Escape,
Tab, and Home. A private iOS GLib/libslirp prefix supplies normal SLiRP
networking, and the
UIKit idle timer is disabled only for the managed guest lifetime.

The common kernel now gives Virtio-GPU to standard VT/fbcon instead of the DRM
log client. It omits the conflicting UML stdio-console driver and legacy BSD
PTY bank, retains dynamic Unix98 PTYs, and exposes only four UML serial lines.
Kernarium attaches no Virtio-console device, so `ttyS0` remains its host
console; the app fans that serial endpoint out to both its stdout relay and the
SwiftTerm view while `tty0` remains visible on the framebuffer. SwiftTerm input
uses the same bidirectional serial socket. Both page profiles build the
selectable VGA8x16 and TER16x32 fonts used by the two iOS density modes.

The repeated physical reliability campaign preceded the selectable display
mode change. Its standard-signed Release app and `phosh.erofs` on an iPad mini 6
completed five cold launches at 1.5 GiB and six vCPUs under the former
`96 * nativeScale` rule, rendering at 2266x1390 and 192 DPI. Every run brought
all CPUs online, emitted timestamped printk records, reported 1,520,944 KiB
available from 1,572,864 KiB, completed DHCP and the EROFS/ext4 pivot, started
stock greetd, reached `graphical.target`, opened its `_greetd` session, and
entered direct virgl/ANGLE/Metal presentation. The captured consoles contain
no Darwin panic, POSIX-signal failure, allocator corruption, or OOM marker,
and no new device crash report appeared. The host stopped each run after
stable presentation, so clean guest shutdown is not claimed; no memory
reduction was needed.

A focused standard-signed Debug check of the now-optional
`160 * nativeScale` Retina rule booted the selected `rootfs.erofs` on the same
iPad at 2266x1390 and 320 DPI.
Virtio-GPU accepted EDID, fbcon switched to its framebuffer, DHCP and the fresh
ext4 pivot completed, and SDDM entered direct virgl/ANGLE/Metal presentation.
The matching iPhone 17 Pro Max Simulator run booted at 1320x2580 and 480 DPI,
initialized EDID/fbcon, reached graphical target and greetd, and entered the
same direct Metal path. These are focused density checks, not replacements for
the five-run physical reliability campaign. The earlier 1 GiB KDE
device/Simulator boots remain dated predecessor evidence. Current macOS Phosh
validation runs the full
service set, completes the GStreamer VA
plugin scan without the former virgl context error, captures the rendered
Phosh lock screen through VNC, and reaches direct CGL/IOSurface/Metal native
presentation. A separate 1290x2796, 288-DPI cold boot proved fbcon, EDID, four
serial devices, no legacy PTYs, and stock Phrog on greetd's first invocation.
That earlier-image investigation found the former first-start failure was a
`SIGHUP` from its tty0 autologin's `vhangup`, not a Phosh or virgl crash. The
updated image replaces that getty with its own `serial-getty@ttyS0` login, and
Kernarium now preserves every image-owned getty, credential, and login policy.
It removes only the mutually exclusive development `phosh.service` enablement
so the stock display-manager alias selects greetd/Phrog. The units and EROFS
remain unchanged, and no wait helper or greetd ordering drop-in remains.
Kernarium systemd boots append `plymouth.enable=0`; this skips the iOS-blocking
boot splash without changing the macOS launchers or direct Geekbench shell. A
matched macOS native-Metal
post-login control at 1320x2580 and 288 DPI proved that the image's
`/etc/phosh/phoc.ini` selects a 720x1440 framebuffer, plane, and CRTC at origin;
empty disposable overrides for both `/etc/phrog/phoc.ini` and
`/etc/phosh/phoc.ini` retain 1320x2580 throughout the greeter-to-session
handoff. A 1320x2580, 288-DPI cold
boot on the iPhone 17 Pro Max Simulator reached graphical target, started
stock greetd and its Phrog session, and presented Phrog through the native
Metal display path. No physical Aquarium result or swap-based workaround is
part of the current checkpoint.

That Simulator path also carried an iOS-only host heap defect until
2026-08-15. `UML_GPU_ANGLE_METAL` declares `VirglRenderer`'s two ANGLE EGL
members, but it was a PRIVATE compile definition, so `vhost_user_server.cpp`
saw the macOS layout while `virtio_gpu_device.cpp` saw the mobile one. On both
iOS products `make_unique<GpuVhostUserDevice>` therefore allocated 512 bytes
for a 528-byte object, and the first SET_SCANOUT wrote `scanoutResource_` and
`scanoutRectangle_` sixteen bytes past the end. A checked-allocation LLDB stop
had captured the resulting tiny-zone freelist corruption: resource id 7 then
`{0, 0, 1320, 2580}` straddling the allocation boundary, with the guest height
overwriting the next block's token. macOS was never affected because the macro
is 0 in every macOS translation unit. `uml_configure_gpu_component` now
publishes one value to the whole GPU component set, and `virgl_renderer.hpp`
fails to compile when the macro is undefined, so the mismatch cannot recur
silently.

That fix did not make the Plymouth handoff boot. A same-build
Simulator A/B on 2026-08-15 left the guest idle with no overlay writes for
eleven minutes and no greeter at 16.5 minutes with Plymouth enabled, while one
`plymouth.enable=0` argument reached
`graphical.target`, `greetd.service`, the image's `ttyS0` login prompt, and a
full-geometry Phrog keypad on the direct virgl/ANGLE/Metal path within about
two minutes. The exact Plymouth-internal wait remains unidentified, but the
user chose the proven disable arm as Kernarium's product policy. The argument
now applies to Kernarium systemd boots only; it does not replace guest units or
change macOS and direct-shell launches. In the resulting checked-allocation
run, entering the image-owned `debian` password through Kernarium's virtual
keyboard handed off to the logged-in session; the Phosh app screen filled the
same 1320x2580 guest surface about 45 seconds later without another host
allocator trap. The iOS GStreamer capability probe still logs rejected
no-codec video-buffer commands, but it completes and does not prevent the
responsive Phosh session; no iOS host codec backend is claimed.

The same physical app also ran the image-local Geekbench 6.7.1 payload
from an opt-in no-systemd ext4 OverlayFS shell while short Time Profiler windows
covered eight completed single-core workloads. The user stopped the run during
the unfinished memory-pressure-heavy Photo Filter phase, so no aggregate score
or multi-core result is claimed and its three traces are excluded. Retained
captures put non-carrier CPU at or below 0.040%, scheduled more than 99.6% of
active work on A15 performance cores, and reported Nominal thermal state at the
first and last retained windows. File Compression, Asset Compression, Text
Processing, and Object Remover were predominantly gadget assembly; HTML5 and
Clang instead expose the existing code-change link reset and cache/link churn.
Object Detection's only large non-gadget pocket was native EROFS zstd demand
decompression. The next experiments are bounded active-link invalidation,
folded scalar register-offset address generation, and a profile-keyed
MTE-disabled vector-pair gadget; none is accepted from unmatched diagnostic
sampling alone.

Targeted clean reruns then rechecked every G147 workload more than 10% below
its current macOS reference. Asset Compression improved to 84 and 83 versus
macOS 89, so its gap did not reproduce. Background Blur remained 96/93 versus
112 and HDR 79/75 versus 106. Horizon Detection produced 51, 143, then 51
versus 120; its repeated slow mode and one fast mode agree with G42's recorded
Geekbench adaptive-duration split rather than defining one stable throughput
delta. Separate 15-second physical profiles remained thermally Nominal, put at
least 99.398% of carrier samples on performance cores, and held non-carrier
CPU below 0.03%. They rank a profile-keyed MTE-disabled vector-memory path,
HDR boundary/link instrumentation, and a Horizon byte-store locality probe.
Horizon's 8.775% guest-kernel compaction/migration share remains explicitly
outside the optimization target. G149 records the complete scores and stacks;
no speculative hot-path change is accepted.

The earlier 2 GiB Simulator run remains dated graphics evidence: it emitted
`UML_AQUARIUM_OK` with WebGL 2 over OpenGL ES 3.0 and ANGLE/virgl. That result
uses an exact upstream ANGLE Simulator-capability backport and a focused
virglrenderer ES 3 multisample-renderbuffer patch; it does not alter Chromium's
library loading or advertise a false guest GL version.

## Post-baseline JIT checkpoint (2026-08-12)

This repository checkpoint records the completed
foundational JIT mechanisms and the focused evidence actually collected so far;
it does not replace the broad
accepted baseline below or attribute that baseline's full host, sanitizer,
guest, Linux, or package matrices to this working tree.

| Boundary | Current checkpoint and evidence |
| --- | --- |
| Whole-process budget | The host parses `ncpus=` and `mem=` with the maintained kernel's policy, then publishes total budget, executor count, and private ceiling as one first-valid configuration. Complete private geometry is capped at 64 MiB per executor; once funded, only the shared tier grows with guest memory. The private charge includes CPU/TLB state, block cache, fallback words, fixed indirect links, direct reverse index, relocation scratch, and rounded arenas. One compact 64-byte arena-adjacent activation anchor is retained per direct target PC and reused by later sources; capacity refusal stays bounded and takes the semantic unlinked path. |
| Shared translations | Production sharing is process-byte-bounded and allocates each shard's backing on first insertion. Its key includes PC, exact consumed guest words, and the complete code-generation profile. Canonical code has zero in executor-pointer cells and retains neutral relocation keys plus immutable validation words; each installer reuses or creates its executor-local direct target anchors and resolves indirect state. `matched=` is only an exact lookup and `reused=` advances after successful private installation. Mutable activation state is never shared. |
| Executor lifetime | A managed run retains at most the configured guest CPU count of executors. Lightweight task carriers borrow them exclusively with FIFO waiter priority; the pool is session, memory-domain, and code-generation-profile bound and drains before domain retirement. Pooling exposed a same-sequence migration bug: a new carrier's distinct `StaticGuestMemory` could skip `memory.prepare()`. Prepared-memory identity is now reset at every begin/end boundary and is separate from conditional executor-TLB preparation. Focused pool tests pass in both page profiles and under ASan/UBSan and TSan. |
| Final reporting | The private app-to-emulator-to-runtime-to-CPU facade explicitly reports compile/overlap, executor pool, then shared after task carriers join but before pool teardown; the app may repeat it idempotently. Compile/overlap and shared retain normal-exit callbacks, while pool reporting is intentionally explicit-only because its snapshot requires the quiescent pre-drain boundary. Records use bounded private formatting and direct descriptor writes, beginning with an LF in the same write so unrelated partial PTY text cannot prefix the schema. Short writes and `EINTR` resume the exact suffix; `EAGAIN`/`EWOULDBLOCK` waits with `poll(POLLOUT)` and then resumes. No task carrier takes or flushes stderr's shared `FILE` lock. |
| Focused host gate | `cmake --workflow --preset check-jitprof` passed 2/2 after the backpressure and same-write leading-LF framing fixes. Its fixture holds the stderr `FILE` lock, fills a nonblocking pipe with an unterminated prefix to `EAGAIN`, proves the writer blocks until a reader drains it, checks leading-LF framing through both writer APIs, exercises concurrent long continuation records, and checks unchanged/changed final phases. The associated architecture audit covered 56 targets, 50 layered targets, and 57 registrations with no forbidden edge or cycle. |
| Rejected framing diagnostic | `.build/results/jit-cache-handoff/jit-profile-pool-shared-final-current-source-20260812/README.md` preserves one current-source attempt and no retry. The guest command, benchmark pipeline, emulator, workloads, three finals, and terminal order passed, but physical line 188 placed exactly 39 shell continuation prompts before the otherwise complete first compile record. Because stdout/stderr already shared the emulator PTY, this was rejected rather than repaired in parsing or by separating only the outer streams. |
| Accepted current-source diagnostic | `.build/results/jit-cache-handoff/jit-profile-line-framing-current-source-20260812/README.md` records the post-framing run. Guest command, `tee`, and emulator statuses were zero; Clang 758 and Object Detection 1257 are diagnostic/unscored. All 1,450 physical profile records start exactly with `Darwin JIT` and end in LF: 1,445 periodic compile, two periodic shared, and exactly one compile, pool, and shared final, with zero overlap/expansion/GPR/malformed records. The 39 prompts remain intact on their own line, finals follow stop in compile→pool→shared order, and nothing follows shared. Direct/indirect link occupancy is 262,144/262,144 and 21,469/262,144, with 60,380 direct, zero indirect, and 46,381 target refusals. Shared final: 5,131,797 matches, 5,127,800 reuses, 5,672,507 admission rejections, zero insertion failures/retained failure bytes, 3,986 direct-capacity plus 11 arena failures, and 268,434,608/268,435,456 accounted bytes. |
| Pool mechanism trace | `.build/results/jit-cache-handoff/executor-pool-mechanism-trace-final-setup-corrected-20260812/README.md` records the accepted unscored mechanism counters. P recorded 18,002,647 borrows, 99.9971% remembered-slot return, 0.0029% fallback, 1.000081 cell probes per borrow, 1,461 failed CASes, no waiters, and ten one-time constructions; E recorded 6,002,142 borrows, 99.9998% remembered-slot return, 0.0004% fallback, 1.000005 probes per borrow, 35 failed CASes, no waiters, and four one-time constructions. Thus frequent acquisition scanning, CAS retry, waiter traffic, and reconstruction are excluded as explanations for the rejected A/B. The trace saw exactly 10/4 nonnull sequence identities with only 0/2 wrapper sequence changes, yet all 10/4 identities used multiple slots with maximum fanout 9/4. That fanout is a lifetime ORed set rather than borrow-weighted churn; Linux's sequence pointer identifies a vCPU interrupt source, not a JIT-cache key, while wrapper preference missed on only 0.002883%/0.000183% of P/E borrows. Sequence affinity is therefore closed without code. |
| Fixed x8/x9 carry closure | The default-off prototype closed its missing 4 KiB ON focused/architecture gate and 16 KiB ON ASan/UBSan focused gate, then ran a matched OFF/ON 2x2 control campaign at P10 and E4 with one worker. Results are in `.build/results/jit-cache-handoff/carry-x8-x9-matched-perf-final-20260812/README.md`. P raw/normalized wall improved 4.443%/5.091% and normalized CPU 5.614%; E raw improved 2.503%, control improved 3.100%, normalized wall regressed 0.616%, and normalized CPU improved 2.025%. The strict result is P PASS/E FAIL/overall FAIL. The user separately accepts the prototype under a tolerance allowing E regression below twice the measured P gain. Default remains OFF because the microprobe is not representative-workload evidence. |
| G107 successor-hoist closure | The bounded 32-entry add/sub-immediate optimization passed focused Release 16 KiB and 4 KiB OFF/ON tests plus 16 KiB ON ASan/UBSan. The matched P10/E4 campaign retained 96 scored samples: P/E polymorphic wall improved 13.723512%/7.258795% and host CPU 13.888889%/9.683605%. A current representative check then improved full Geekbench Single-Core 94→97 and Multi-Core 564→567; targeted Clang/Object Detection improved 438→441, while rerun File Compression/HTML5 outliers stayed within 1.3% of OFF. The bounded selector is now ON by default. Results are in `.build/results/jit-cache-handoff/g107-successor-hoist-matched-perf-setup-20260812/` and `.build/results/gadgets-optimization-20260813/`. |
| G140 SIMD destination residency | The bounded integer-MLA implementation keeps one nonaliasing v0-v7 destination in q24 only within runs of four or more operations and flushes before the translated boundary. Its sensitive probe improved 69.6% P and 43.5% E; the paired out-of-bank control moved -1.1%/-5.7%. Targeted SIMD Geekbench changed 91→92. Against the G107 production predecessor, a clean full run changed Single-Core 97→96 and Multi-Core 567→575. One earlier full attempt was stopped after an RCU stall in Multi-Core Text Processing; isolated and clean-full reruns scored 62/63 there. The bounded selector is ON by default. |
| G141 exact gadget-cache validation | Same-generation code changes now reset local link activations and retain a decoded program only after exact page-bounded comparison of all consumed words. The P/E invalidation probe improved 62.4%/50.6%; targeted multi-core Clang/Object Detection moved 394/461→383/464, and full Geekbench moved from G140's 96/575 to 96/567 (original baseline 94/564). Lazy grow-on-demand saved-word buffers replaced the first fixed-array prototype. The selector is ON by default. A live sample put decode at 2.36% of configured ten-vCPU capacity, too little to justify global decoded templates that still require private copy/link relocation. |
| Residual gadget experiments | The historical G8/G17/G36 E-core remeasurement item is closed without a new run: none clears the prospective `P > 2%` eligibility gate, and G106 later showed the same noncritical instruction-count class flat on P and E. G140 closes the bounded G130 destination-only route; no evidence authorizes a whole-bank or cross-block residency protocol. |
| Measured boundary | The saved-word `A B B A A B B A` medians remain -0.34% Clang, -2.16% Object Detection, and +0.62% maximum RSS; the score deltas are inside the 2.8% band and RSS does not show a win. Both final-profile artifacts are diagnostics without matched controls. The global-mask and exact-token pool A/Bs are rejected; the pool remains required for the guest-CPU cap. The shortened sharing ON/OFF/OFF/ON sequence also rejects the memory exception (+2.87% median peak tree RSS). The fixed-private-cache diagnosis progressed from Clang/Object Detection/summary 460/856/628 to 542/1139/786 with compact records and to 613/1238/871 after per-target anchor deduplication. A focused two-observation sweep found 62 MiB neutral at 620.5/1255.5/883 versus 613/1242.5/872.5 at 64 MiB, while 60 MiB failed the Object Detection and combined-score bands. The user retained 64 MiB because the smaller arm saves only 2 MiB per executor and does not change the total budget. Shared installation averaged 2,424.81 ns versus 16,322.84 ns for local compilation; the final sample peaked at 6.932 GiB RSS without activation/deactivation in its collapsed top-stack list, while precursor counters show 3,285,847,540 bytes of real lazy-tier population. These are focused observations, not a broad benchmark. The final 16 KiB workflow reached 572/573 before one stale code-shape expectation; that corrected exact test passes at both 16 KiB and 4 KiB. Full sanitizer and complete integration workflows are not credited to this checkpoint. |

## Current authoritative status (2026-08-10)

The refactor campaign and forward 4 KiB Darwin mapping repair remain accepted
at implementation baseline
`783bfcf7e2447f8f981830a158bec3f402b08d97`. The later unified-EROFS migration
supersedes that baseline's rootfs workflow without reattributing its broader
host, sanitizer, package, or performance evidence to the migration checkpoint.
This section supersedes every older "current", "open", or completeness
statement below. Those sections remain dated audit and experiment history, not
current blockers or current rootfs instructions.

| Boundary | Accepted state |
| --- | --- |
| Linux source | The signed Linux 6.18.41 package now replays 15 ordered topics to canonical tree `ac91de43ee9c8eedd208103791dec4ed76810716`. Fresh replay, topic, overlap, ABI, and delta policy checks passed at 155 changed files and 6 files outside the Darwin port. Topic 0015 adds the compact physical-iOS address profile while leaving `standard` as the macOS/Simulator default; the earlier accepted campaign's two standard page-profile builds remain dated evidence. |
| Interfaces | The independent host/kernel bridge is ABI v6, fingerprint `0x6524b65f20239ea1`. The installed public API remains v4, fingerprint `0xd9a69804d3303044`. |
| Preserved repairs | G99 gadget ThinLTO, G100's exactly five audited decode-family `.ipp` fragments, G121's published per-thread address-space lifecycle slots, and G123's serialized Darwin `init_mm`/4 KiB host-slot repair are retained. The pre-refactor rollback used to diagnose G123 is not retained. |
| Refactor host acceptance | At baseline `783bfcf7`, ordinary unit suites passed 572/572 at 16 KiB and 573/573 at 4 KiB; tooling passed 51/51 in each profile. ASan+UBSan passed 572/572 in 710.45 seconds, TSan passed 8/8 in 5.14 seconds, and the malformed-loader ASan gate passed 1/1. Normal and ASan GPU graphs each passed 5/5 unit tests plus the 1/1 VNC tooling test. Architecture/source graphs covered 575 targets and 623 registrations at 16 KiB, 576/624 at 4 KiB, and 595/628 in the GPU graph. Input materialization was off and produced zero sidecars. |
| Refactor guest acceptance | At the same baseline, `integration-fast` passed 5/5 at 16 KiB and 5/5 at 4 KiB. The standalone 10-vCPU SMP workload passed at 8.19 and 8.03 effective host cores, respectively. Its retired partitioned-image observations remain historical evidence only. |
| Unified EROFS migration | Final host workflows passed 572/572 units plus 49/49 tooling at 16 KiB and 573/573 plus 49/49 at 4 KiB. The latest complete integration workflows passed 52/52 tooling checks and the regenerated five-test `integration-fast` suite in each profile using raw `rootfs.erofs` at `/dev/ubda`: 16 KiB 5/5 in 65.42 seconds and 4 KiB 5/5 in 69.33 seconds. The overlay/systemd test passed in 34.06 and 38.19 seconds, respectively, with separate tmpfs filesystems for Docker and containerd plus an offline source-built hello-world container run with pulling and networking disabled. Provisioning, launcher, runner, emulator-option, and API contracts pass. EROFS owns the read-only lower-filesystem property; host mode, per-run hashes, sidecar locks, `ubd0r`, and `ro` are not acceptance mechanisms. The 16 KiB profile emits the upstream experimental subpage-compression warning for the image's 4 KiB filesystem blocks; the 4 KiB profile does not. |
| LTP and benchmarks | The unified image supplies LTP at `/opt/ltp`, Node at `/opt/node/bin/node`, and benchmarks under `/opt`. The manifests now contain 2,428 verified and 135 disabled LTP cases with no overlap; 88 require three deliberately absent kernel configurations. The credited final-image boundary is 44 focused gadgets-engine cases, all accepted; `statx05` passed 4/0 in both page profiles and the exact live `zram01` row passed 12/1 with rc 0 in 167.6 seconds. Full LTP was not rerun. Geekbench 6 uses the local payload with `--no-upload`; the user-directed final scoring run remains skipped and no new score is claimed. |
| Apple package | The macOS, iOS-device, and iOS-simulator slices passed platform, API, staging, package-shape, and consumer checks and formed the three-slice XCFramework. Raw and installed-tree consumers passed, and the real Xcode consumer reported `BUILD SUCCEEDED`; its locked-iPhone discovery warning was non-fatal. |
| Post-baseline native display | The macOS CLI now accepts `--gpu-display=native|vnc`; the KDE launcher selects native by default and retains VNC through `--display vnc`. Supported Virgl 2D scanouts take a GPU-only CGL-to-IOSurface-to-Metal path with two hazard slots, latest-frame replacement, and no additional FIFO. VNC and unsupported resources retain CPU fallback. Normal and ASan+UBSan GPU workflows each passed 5/5 units plus the 1/1 VNC smoke. The canonical production CLI rebuilt with its supported dependencies. A focused 800x500, four-vCPU native launch reached `graphical.target`, passed the overlay guard, and logged direct CGL/IOSurface/Metal presentation before Ctrl-C. Keyboard/pointer interaction and a timed latency measurement were not claimed. This source change does not retroactively turn the dated KDE/VNC evidence into native-window evidence. |

Current guest runs use EROFS directly without per-run hashing or a sidecar
lock. Only explicit provisioning reads `tools/rootfs/manifest.json` to
authenticate an input image.

Acceptance is deliberately bounded. Input materialization remains explicit
opt-in because a complete profile is about 6.6 GiB; ordinary acceptance ran
with it off. The instruction ledger still reports **0/983 variant-complete**
entries, so historical 983-entry checkmarks and representative semantic
contracts are not an exhaustive architectural-semantics claim. Full LTP remains
an explicit external campaign even though its payload is now present. Source-
size thresholds remain advisory ownership signals, not gates.

All verification above was local; the repository has no hosted CI workflow.
After every other acceptance task completed, the final Geekbench rerun was
intentionally skipped at the user's direction. The preserved historical
measurements and focused hot-path controls remain the performance evidence.
The EROFS migration removes the upload/network gate and uses `--no-upload`, but
does not create a new end-to-end Geekbench result.

[`docs/refactor-evidence.md`](refactor-evidence.md) is the detailed evidence
index. [`REFACTOR_PROGRESS.md`](REFACTOR_PROGRESS.md) is the compact summary.

## Seventh-audit handoff (historical; superseded)

This was the authoritative status as of the seventh independent audit on
2026-08-04. Repository-owned work remained open. The seventh audit superseded
the sixth-audit completeness claim: it found two instruction-evidence closure
gaps, multiple runtime/device correctness gaps, unfinished engine and Linux
modularization, and structural-test gaps. The sixth-audit implementation and
host evidence are retained below as historical results, not current closure.

Work needing credentials, special images, physical devices, public services,
interactive graphics, or controlled performance hardware remains external
acceptance and is not represented as a local pass. Rootfs file management is
still deferred as described below; none of the seventh-audit findings reopens
it.

The historical status changed on 2026-08-06: a gadget-engine performance
campaign began;
input-materialization recording became opt-in, removing 6.6 GiB of build-tree
cost from ordinary runs; `arm64-input-materialization-join`, which is now
registered only with that option, exposed an undersized contribution-spool
capacity constant and a checked aggregate manifest that predates the current
test sources. The capacity defect is fixed on the current tree; the stale
two-profile publication remains open. An **intermittent vCPU stall under long
Geekbench runs is open and blocking guest benchmarking**. See
[Status change 2026-08-06](#status-change-2026-08-06-performance-campaign-and-a-newly-diagnosed-blocker)
below; that failure is an artifact-staleness blocker, not a behavioral defect,
and its repair is intentionally deferred into the existing instruction-evidence
P1 finding.

## Seventh-audit working-tree snapshot (historical; superseded)

The historical refactor base was
`618b68658013b83394efef367570fa2c36113eb0`. The then-current outer HEAD was
`b931e9b682e7b4bde4a4824c2e2142676052e1e1` (`Major refactor`). At the seventh-
audit baseline, the implementation and documentation existed as 155 modified
or deleted tracked status entries plus 56 untracked status entries (58
individual untracked files), with nothing staged. `git diff --stat` reported
155 files, 6,904 insertions, and 27,459 deletions. These are user-owned changes.
Do not commit, stage, reset, clean, broadly check out, or otherwise rewrite the
working tree without separate authorization. Preserve unrelated pre-existing
working-tree changes.

Rootfs file management is explicitly deferred to a later testing redesign. Do
not implement or expand image hashes, sidecar locking, maintenance
serialization, or rootfs cleanup as part of this refactor. Existing wrappers
may remain in use, and incidental destructive image changes still require
separate authorization.

## Seventh independent audit: historical open checklist (superseded)

The following items were current for that audit. They are retained to explain
the subsequent campaign, not as unfinished requirements. At that time, they
had to be fixed before using any historical
"complete" statement below. Source-size thresholds remain advisory; the
modularity findings are about ownership and dependency shape, not line-count
enforcement.

### P1 correctness and acceptance blockers

| Finding | Current evidence | Acceptance requirement |
| --- | --- | --- |
| Independent instruction allocation and native-feature authority is implemented; current-host regeneration remains an acceptance replay | The pinned Arm XML oracle now publishes 2,396 binding-independent form/decode intersections across all 983 entries plus 6,225 allocated/unallocated decode partitions, including dependent/reserved predicates, word-selected features, fallback rules, semantic feature rules, operation digests, and alias digests. Selected bindings must resolve through that model and a pinned LLVM decoder. Removal, duplication, form/feature swap, reserved-field, regenerated-catalog, and official-index negatives pass. | With the pinned 2026-06 instruction and system-register archives plus LLVM 22.1.8 available, regenerate the required/allocation/decode artifacts and require byte parity before the next two-profile publication. This is verification of the implemented independent authority, not permission to infer behavioral completeness. |
| Semantic evidence is still representative, not variant-complete | The schema and validator now permit multiple independently identified execution records, and removal, duplication, cross-variant-result-swap, semantic-removal, and semantic-swap negatives pass. The checked artifact nevertheless contains only 4,744 harness-declared semantic contracts (2,372 per page profile), all explicitly `entry_complete=0`; current validation reports **0/983 variant-complete entries**. The much larger raw sweep and 2,396-row allocation model enumerate execution obligations but do not independently supply expected state/event/memory results for every operand, state, valid/reserved, and feature partition. | Complete fresh opt-in materialization joins and the exact two-profile official-semantics campaign, then independently review/bind every required semantic partition to expected architectural results for all eligible engines. Keep `--require-variant-complete` failing until those reviewed totals exist; do not mark an entry complete from representative or aggregate counts alone. |
| Instruction-fetch fault direction is erased before Linux receives it | The public engine result distinguishes read, write, and execute, but interpreter results, translated TLB metadata, gadget/JIT fetch failures, the shared translated adapter, and the C bridge reduce the direction to read/write. The bridge exports only a write bit. Linux then guesses execute when the fault address equals the instruction pointer, so a data read at that address or a true execute-permission fault can be retried with the wrong permission. The common harness repeats the same collapse and cannot detect it. | Preserve read/write/execute through all three engines, translated fault metadata, the mirrored host/kernel ABI, and the Linux consumer. Test an unmapped/non-executable entry fetch, a data read whose address equals the current PC, ordinary read/write faults, exact address/PC, zero retirement, and retry behavior; advance ABI identity, refresh the Linux topic, and replay both page modes. |
| Gadget exception cleanup can violate the common engine contract | Gadget blocks intentionally leave the guest floating-point environment active between translated blocks. Ordinary exits restore/materialize it, but if a later allocation or translation throws, the `noexcept` adapter returns fatal after `GadgetExecutor::end_execution()`, which ends only MMU execution. Direct `CpuEngine` callers can receive a fatal result with guest host-state still active and lazy architectural state not materialized. The native-code executor already restores this state in its cleanup. | Inject a second-block allocation/translation failure after the first block activates native floating-point state. Require a documented retirement count, materialized `CpuState`, restored host environment, balanced begin/end, and successful engine reuse. Run the same exception-contract test against the native-code engine. |
| Retired memory-domain guards are unbounded and allocation-fallible | Every run receives a monotonic domain. Both software and initialized 4 KiB native registries permanently insert that domain into `retired_domains_`, even when no address space existed. N runs retain N software keys and another N native keys in 4 KiB mode. Insertion failure is swallowed; a delayed resolver can then recreate state for a retired domain after the live entry is erased. | Make retirement allocation-free and use bounded lifetime/identity state that prevents an in-flight old resolver from publishing after retirement. Add 100,000-run bounded-metadata tests for both page modes, deterministic delayed-resolver/retire interleavings, allocation-failure coverage or a structural no-allocation proof, and immediate next-run retry. |
| GPU shutdown can block forever on an incomplete live stream frame | The connection loop polls only before entering `recvmsg(MSG_WAITALL)` and blocking completion reads. `stop()` merely sets a flag and joins. A peer that sends part of a header or payload and stays open prevents destruction from returning. Current truncation tests explicitly shut down the sender first. | Make framed reads/writes stop-aware with nonblocking polling plus a wake path, or interrupt them through a separately and correctly owned endpoint. Add bounded subprocess tests for partial header and payload, descriptor balance, restart, and normal plus ASan/UBSan GPU gates. |
| Published GPU and input descriptors have overlapping owners | The service retains both peer endpoints in RAII objects while publishing their raw numbers. The same-process guest connector returns those exact descriptors rather than duplicates, and guest cleanup closes them. Service destruction can later close a recycled descriptor number; using the retained number to fix shutdown could also target an unrelated reopened descriptor. | Define transfer/duplication ownership. The guest must own a distinct transferred/duplicated endpoint and the service must retain only a stable, separately owned wake/shutdown capability. Test guest-close followed by descriptor-number reuse and host stop/destruction, plus startup failure and retry. |
| Current production/package evidence predates the final source split | The newest macOS CLI and both kernels predate the current gadget/JIT split; topic 0011 and metadata also postdate both kernels. The current XCFramework macOS archive still contains obsolete `program.cpp.o`, whereas current iOS build archives contain `program_builder.cpp.o` and `program_execution.cpp.o`. Historical package consumers therefore cannot close the current tree. | After source findings are fixed, replay/build both kernel page modes, final-link the current macOS CLI, rebuild all three package slices, rerun architecture/final-link and executable-publication audits, and repeat raw plus installed-tree C/C++ consumers. Record only current-tree artifacts as seventh-audit acceptance. |
| Suite build targets omit registered GPU fixture binaries | `unit-test-binaries` is assembled from arm64-local targets plus runtime and omits the registered `uml_gpu_service_fd_tests`. `tooling-test-binaries` scans only the tooling subdirectory, while the registered VNC smoke commands use two targets owned by the parent tests directory; in a fresh GPU graph the tooling aggregate is empty and those executables are absent. A build preset can therefore finish before CTest attempts a registered command. | Derive suite build dependencies from every registered target command, or explicitly attach every cross-directory binary. Add a fresh-graph audit proving each enabled suite's `$<TARGET_FILE:...>` or target command is reachable from its aggregate, then run fresh GPU tooling, integration, 4 KiB integration, and all workflows. |

### P2 architecture, device, and workflow work

| Finding | Current evidence | Acceptance requirement |
| --- | --- | --- |
| Engine splitting is implementation-header fragmentation | Production `jit_executor.cpp` is a four-line textual composition of roughly 10,095 implementation-header lines into one object. Gadget translation remains one roughly 4,274-line builder body plus about 8,543 transitive selection-header lines. These are not the normal responsibility-owned translation units required by the plan; size-review rationales do not create module boundaries. | Give cache, links, publication, diagnostics, semantic helpers, lifecycle, test hooks, and instruction-family translation normal `.cpp` owners with declaration-only private headers. Make the gadget builder orchestration rather than the complete decode walk. Preserve hot object/disassembly shape and extend the source audit to reject implementation-fragment headers without prohibiting genuine templates or small inline code. |
| Cross-engine include enforcement is bypassable | The real architecture manifest serializes link, object, and aggregate edges, but never emits include-directory ownership. Negative tests inject synthetic `include@` edges. Text scans catch only include strings containing another engine directory, so another engine's include directory plus `#include "basename.hpp"` escapes both gates. | Add explicit engine metadata and serialize/inspect include directories or compile-command/header dependencies. Resolve included files to their owning engine. Add real configure negatives for direct, basename, and transitive includes in every engine direction. |
| Common conformance does not use every exact production engine object | Ordinary instruction conformance constructs a test-local gadget adapter. Its native-code path resolves the production adapter against `uml_arm64_jit_test_support`, which recompiles the whole executor with test macros instead of using the production executor object. Basic production-adapter contract tests pass, but the full instruction sweep does not prove those exact production compositions. | Run ordinary conformance through production factories, adapters, and executor objects. Put forced-path controls and instrumentation behind neutral injected hooks or separate focused tests; do not credit a recompiled implementation as production-engine evidence. |
| Interrupt-token registration silently drops allocation failure | `darwin_uml_static_guest_interrupt_sequence()` catches every failure from thread execution-state acquisition and returns success-like `void`; it does not latch an error. Linux installs the sequence immediately before guest entry, so a one-shot allocation failure can let that entry run with a null/default token and delay cooperative interrupt observation. The comment claiming the next entry will fail is not implemented. | Make registration allocation-free, latch a failure that the next entry consumes before retirement, or change the checked kernel ABI to return an error. Inject the first registration allocation failure, require deterministic no-retirement failure, then prove explicit retry/re-registration and no stale token; update both ABI copies and replay if the signature changes. |
| Linux topic 0002 remains deliberately mixed | The plan explicitly requires SMP/timer/IRQ to be separated from MMU/HugeTLB and unrelated execution/build changes. Topic 0002 is still named for all of them and spans 19 files at about +1,723/-117, including the largest SMP, TLB, HugeTLB, uaccess, timing, execution, and build-wiring areas. | Split it into reviewable final-state topics, at minimum MMU/HugeTLB, SMP/timer/IRQ, execution/capability, and build portability, without changing the final tree. Refresh stable IDs, hashes, order, overlap ownership, extraction decisions, and next-version actions; replay and build both page modes. |
| Checked-in integration presets cannot select the required case-sensitive kernel checkout | `macos-integration` enables the kernel runner but leaves `UML_KERNEL_SOURCE_DIR` at repository `linux`; source verification rejects its normal case-insensitive parent. The manual documentation supplies a case-sensitive-volume path, while the dependency README incorrectly presents the preset alone as sufficient. | Make integration/all presets consume a documented environment or user-preset path and fail early with a clear missing-path diagnostic. From fresh caches, configure `check-integration`, 4 KiB integration, and `check-all`. Correct the dependency README. |
| GPU connection reset and command mutations do not fully own device state | Transport reset clears mappings, queues, and negotiated features but retains contexts, resources, and scanout state; `RESET_OWNER` is a no-op. At least context creation mutates the renderer before allocating bookkeeping and a response, without the rollback used by another resource path. Existing GPU tests do not directly exercise `VirtioGpuDevice::process`. | Add explicit device reset on owner reset and disconnect, destroying renderer state exactly once. Make every mutating command exception-atomic. Add split host-only command suites with fake renderer/display, malformed request coverage, deterministic allocation failures, same-ID retry, and reconnect-to-pristine-state tests under normal and sanitizers. |
| Optional diagnostics impose production cost and retain thread records | Every guest entry reads `steady_clock` even when dispatch profiling is compiled off. The MMU writer path also reads a clock while gate profiling is disabled. When gate profiling is enabled, each participating host thread permanently appends a heap record and writer diagnostics scan the ever-growing vector. | Compile diagnostics into an optional tool/test target or a compile-time zero-cost policy. Prove the ordinary production object has no clock/profile call in these paths. If the gate profiler remains, give per-thread registration RAII retirement and add a thread-churn subprocess proving the registry returns to baseline and contains no dead IDs. |
| Structural audit CTest is too slow for its timeout | `symmetric-engine-edge-audit` performs one positive plus 24 sequential full File API scans. It failed at 60.01 seconds; direct runs passed at 59.71-65.69 seconds. The source audit also recursively traverses large ignored build/Linux/external trees before filtering them, taking about 25 seconds on this workspace. | Parse the architecture graph once or use a minimal synthetic graph, and enumerate only first-party source roots. Target a sub-10-second edge fixture, give realistic timeout margin, and repeat the registered tooling suite under ordinary parallel load. |

### P3 bounded cleanup and hardening

| Finding | Acceptance requirement |
| --- | --- |
| One fatal gadget/helper has no source or relocation reachability | Remove its wrapper, declaration, and assembly body unless it is an intentional documented ABI surface; rerun symbol reachability, units, and layout-sensitive checks. |
| Network restart test controls execute in the production per-frame path | Move pause/counter state into a test-only provider variant or compile-time no-op policy. Prove the production object has no test counter/pause symbols, branch, or per-frame atomic, while the deterministic restart fixture still passes. |
| Runtime lifecycle/failure hooks live in the production `Context` object | Move admission/retirement hooks, state counters, and forced-publication failure fields/branches behind a test-only facade or narrow test object variant. Prove the production object has no `_for_testing` symbols, fields, or conditional branches while keeping deterministic lifecycle coverage. |
| Legacy-context and software-registry static destruction order is implicit | Make process-lifetime ownership/destruction order explicit and add a subprocess exit-order test so context teardown cannot call a destroyed memory registry. |
| Raw CTest registrations could bypass suite-label verification | Reject first-party `add_test` outside `UmlTesting.cmake`, or validate actual labels from CTest JSON against `tests/test-manifest.tsv`; add a wrong/missing-label negative. |
| The Python-free unit contract is ambiguous | `tests/unit` contains no Python or shell files and the selected unit tests execute without Python, satisfying the user's physical-test requirement. ADR 0001 makes the stronger claim that units are buildable without Python, but top-level configure always runs the Python ABI declaration verifier. Either revise the ADR to distinguish Python-free test implementation/execution from Python-used-as-a-build-tool configuration, or preserve the stronger claim by adding a no-Python configure/build smoke and a native verifier. |

### Recommended continuation order

1. Repair the independent allocation/feature oracle and introduce explicit
   variant identities first. Until those gates pass, the 983-row report is
   useful regression evidence but cannot close instruction completeness.
2. Preserve instruction-fetch fault direction through the engine/kernel ABI and
   fix translated-engine exception cleanup and interrupt registration. Then
   replace retired-domain tombstones before building further lifecycle behavior
   on those foundations.
3. Route the common conformance matrix through exact production objects. Split
   implementation fragments into normal translation units and strengthen real
   include ownership while checking generated hot-path shape after each small
   behavior-neutral move.
4. Fix GPU cancellation, descriptor ownership, reset, and command atomicity;
   remove production test/profiling overhead. Split Linux topic 0002 without
   changing the final tree, repair fresh-cache integration inputs and suite
   aggregate completeness, shorten the structural audits, and run both page-
   mode host/sanitizer matrices.
5. Only after source closure, replay/build both kernels and rebuild the current
   CLI plus all package slices/consumers. Then run the separately authorized
   guest and controlled performance acceptance. Continue to defer rootfs file
   management, and do not commit or stage refactor changes without explicit
   authorization.

### Seventh-audit verification

| Check | Fresh result |
| --- | --- |
| 16 KiB host graph | clean configure/build succeeded; strict unit selection passed 562/562 in 468.78 seconds |
| 4 KiB host graph | clean configure/build succeeded; strict unit selection passed 563/563 in 367.62 seconds |
| Architecture/source audit | 4 KiB graph passed at 553 targets / 586 registrations; GPU graphs passed at 562 targets / 587 registrations; source report covers 1,060 files, 60 above advisory guidance, 42 without optional review context, zero changed since review |
| Tooling | 22 of 23 tests passed; only `symmetric-engine-edge-audit` hit its 60-second timeout. The exact command passed directly in 59.71 seconds, confirming semantics but no usable margin |
| Sanitizer/focused gates | full ASan+UBSan 562/562; TSan 4/4; malformed-loader ASan+UBSan 1/1; GPU normal 1/1 and GPU ASan+UBSan 1/1 after granting loopback-port permission |
| Allocation-catalog regeneration | regenerated 983-row file is byte-identical to the checked-in file at SHA-256 `0e383651df7e3d743ab7d7d4ffe2007b6ce41bd6bb09f1ffc8bdb9f0ca77d574`; this proves deterministic reproduction of the current circular method, not independent architectural validity |
| Dependency and policy checks | current pinned release-source manifest, pristine LibVNCServer 0.9.15, private prefix, ABI identities, Linux manifest/topology/next-version fixtures, and isolated topic 0011 checks pass |
| Guest/performance/product acceptance | no guest boot, rootfs mutation, LTP, browser, benchmark, current kernel replay/build, current CLI final link, or current package rebuild was performed; the seventh audit remains open |

### Confirmed seventh-audit invariants (historical)

- All three CPU engines implement the common virtual interface, direct factory
  selection is in place, the native-code engine has no gadget link/object
  fallback, and a valid implementation gap produces a guest-scoped illegal-
  instruction event with zero retirement rather than terminating the emulator.
- Engine selection is frozen per run; same-handle admission, queued stop
  synchronization, per-thread execution-state retirement, bound concurrent-
  handle routing, and public input deep-copy contracts remain covered.
- The unit, tooling, and integration source trees are physically separated.
  `tests/unit` contains no Python or shell scripts. Full LTP remains a custom
  target outside CTest; SMP is standalone or explicitly opt-in integration and
  is absent from the default unit configuration. The manifest has 603 rows:
  567 unit, 26 tooling, and 10 integration. Current registrations use the
  centralized helpers, subject to the future raw-registration bypass above.
- CMake/CTest with Ninja remains the appropriate canonical first-party system
  for the mixed Apple/Linux/upstream-source graph; no parallel Bazel or Meson
  first-party source manifest exists.
- Dependencies are pinned official release-source packages. LibVNCServer is the
  pristine upstream 0.9.15 tree with no project patch. Active source paths no
  longer use the retired layout vocabulary, device modules live under
  `darwin/device`, and no empty first-party source directory remains.
- Source-size thresholds remain advisory. Fresh reporting covers 1,060 files;
  size alone is not an error or an open item.

## Status change 2026-08-06: performance campaign and a newly diagnosed blocker

**Historical and superseded by the current status at the top of this file.**
This section records work started after the seventh audit. At that time it did
not close any seventh-audit finding.

### Changed: input materialization is now opt-in, and its join has two blockers

**Disk cost removed from ordinary runs.** Every unit test previously wrote one
compressed input-materialization sidecar whenever tooling tests were built,
even though only `arm64-input-materialization-join` reads them. A complete
16 KiB profile cost **6.6 GiB** against 3.4 MiB of ordinary evidence. Producers
and the join now share one new option,
`UML_INSTRUCTION_INPUT_MATERIALIZATION`, default `OFF`, so the join can never be
selected without its inputs. Measured effect: the default suite registers **618
tests and leaves a 12 KiB evidence directory**; with the option on it registers
619 and regenerates the full profile. The `A64_INSTRUCTION_PROGRESS.md`
publication workflow records how to enable it.

Two further reductions are measured but not taken, because both require a
vendored compressor that the macOS SDK does not provide: on a real 120 MB
sample `xz -1` is 1.98x smaller than the current zlib level 6 at comparable
write cost, and `zstd -3` is 2.13x smaller and 9x faster. Raising zlib to level
9 recovers only 2.4% and is not worth doubling write time.

**The historical complete join failed when enabled.** With the option on, it
was the only failure in the 16 KiB selection: **618 of 619 tests passed** in
549.89 seconds at `ctest -j 16` with a complete 450-file evidence profile.
Neither finding was caused by the performance changes below; both were
reproduced before any engine source was edited. The first is now fixed; the
second still requires a fresh complete two-profile publication.

There were two, and the first masked the second.

**Resolved: the contribution-spool capacity policy was too small.** The join
previously sized each producer's SQLite spool as
`expected_contributions * spool_bytes_per_contribution + 4 MiB` and enforces it
with `PRAGMA max_page_count`. For `arm64-atomic-floating-allocation` the 16 KiB
profile declares 157,080 contributions, giving a 165,044,224-byte cap at the
default 1,024 bytes per contribution. The spool reached 164,941,824 bytes and
SQLite raised `database or disk is full`, which a broad `except
sqlite3.DatabaseError` reports as `contribution spool exceeded its hard byte
cap`. Free disk at the time was 38.8 GB, so this is a capacity-policy shortfall,
not exhausted storage: rows carrying long native-feature alternative lists
average more than 1,024 bytes. The current bounded default is 2,048 bytes per
contribution plus 4 MiB; the affected producer completed at that setting and
the join advanced to the separately stale aggregate check. The failed run's
size divided by its declared total is a 1,050.05-byte lower bound; the
successful replay, rather than that incomplete-run estimate alone, establishes
that 2,048 is sufficient. The preflight calculation uses the same corrected
bound. Error handling distinguishes a bounded
`max_page_count` hit, backing-filesystem exhaustion, and unrelated SQLite
failures, with end-to-end initialization/insertion/commit cleanup fixtures.

**Remaining blocker: the checked aggregate manifest predates the working
tree.** With the capacity defect fixed, the join proceeds and fails on the
underlying problem.
It maps each raw producer contract to an aggregate identity and rejects any
identity absent from the checked manifest, reporting `<raw-contract> maps to
unknown/out-of-profile <aggregate>`. `aggregate_id` is a SHA-256 over 24
columns that include `source_file` and `source_line`, so any movement of test
source lines invalidates every identity derived from the moved code.

Measured drift, comparing the checked-in manifest against evidence regenerated
from the current tree:

| Quantity | Value |
| --- | --- |
| Producers in the checked manifest | 449 |
| Producers compared | 47 |
| Identical | 11 |
| Drifted | 36 |

The drift is systematic line movement in shared test-support headers, not a
semantic change. For example `arm64_base_add_sub_test_support.hpp` records
evidence at lines 286 and 310, while the manifest expects 62, 284, and 298. The
manifest was last committed at `d15e98c`, before the current uncommitted test
sources, so roughly three quarters of producers are expected to be stale.

The remaining stale publication is therefore an artifact-staleness blocker,
not a behavioral defect:
it validates instruction *evidence* bookkeeping, and the behavioral unit tests
that gate engine changes are unaffected.

Repair requires the documented complete two-profile publication in
`A64_INSTRUCTION_PROGRESS.md`: all 449 `*.tsv.raw` producers present in both a
4 KiB and a 16 KiB evidence directory, both full profile TSVs, then direct
semantic validation of the combined replacement TSV. Partial producer
replacement and validator-side writes are explicitly unsupported.

The independent-allocation prerequisite is now implemented by the pinned Arm
XML required-variant/decode models and end-to-end catalog mutation negatives.
That does not make a stale partial replacement valid: replacement still requires
fresh raw and input-materialization evidence for all 449 producers in both page
profiles. This multi-gigabyte operation remains an
explicit opt-in acceptance workflow, never a standard CTest side effect. The
separate behavioral-variant completeness gate remains open until every
required semantic partition is independently specified and executed.

### New: vCPU stall regression introduced by this refactor, root cause open

**The regression is confirmed against the pre-refactor build.** Three matched
10-vCPU gadget-engine Geekbench runs, same rootfs, same host:

| Build | Workloads | RCU stalls | Watchdog kills | Outcome |
| --- | ---: | ---: | ---: | --- |
| pre-refactor (`.build/cmake-macos/bin/uml-darwin`, 2026-08-01) | 32/32 | **0** | 0 | completed, 52 m |
| post-refactor run 1 | 32/32 | **2** | 0 | completed, 57 m |
| post-refactor run 2 | 23/32 | **1** | 1 | **wedged** |
| post-refactor run 3 (stall-catch) | 23/32 | **2** | 0 | stopped after capture |

The decisive signal is not pass/fail but the stall count: the post-refactor run
that *passed* still recorded two RCU stalls where the pre-refactor run recorded
none over the identical 32 workloads. Pass/fail alone reads as flakiness.

**The trigger is deterministic: Multi-Core Text Processing.** All three
post-refactor runs stalled immediately after the 23rd workload marker, which is
multi-core workload 7. That is the one Geekbench workload that scales to roughly
1.2 host cores, so it is dominated by inter-thread synchronization rather than
parallel compute.

**Live host state at the stall** (sampled while wedged, not after it settled):

- Five vCPUs stalled at once: `1-...0`, `3-...0`, `4-...0`, `8-...0`, `9-...0`,
  all `(1 GPs behind)` with dynticks nesting 1, each carrying a runnable
  `geekbench_aarch` task.
- **All 208 host carrier threads were named `uml-vcpu0`; none were named
  `uml-vcpu1` through `uml-vcpu9`.** `darwin_set_current_cpu()` is called by
  every carrier as it acquires a CPU, so this means no carrier held any CPU
  other than 0 while the guest believed five other CPUs had running tasks.
- 222 threads were blocked in `pthread_cond_wait`, the task-switch wait.
- The host process held one core at ~108% with the host 80% idle.

**Mechanism, and what is still unproven.** A vCPU can only be acquired by
handoff from its current carrier: `switch_threads_dead()` sets `from->cpu = -1`,
calls `release_cpu_execution(cpu)`, then assigns `to->cpu = cpu` and signals it.
That path was read in full and transfers the CPU before any exit, including both
`pthread_exit()` branches, so it does not strand a CPU on its own. The one path
that releases `cpu_execution_owner[cpu]` *without* handing the CPU to another
task is `retire_task_carrier()`, which this refactor added. If it ever fires on
a live carrier, that vCPU has no owner and no way to acquire one, which matches
the observed state exactly. This remains a suspect: it has not been shown to
fire, and five synthetic payloads failed to reproduce the stall.

Reproduction by synthetic payload was not achieved. Geekbench itself is
currently the only reliable reproducer, reaching the trigger in 35-47 minutes.
The payloads below all completed cleanly on the post-refactor build at 10 vCPUs,
which rules out the failure being reachable by ordinary carrier, thread, or
mapping churn:

| Payload | Subsystem targeted | Result |
| --- | --- | --- |
| `/bin/true` fork/exec storm, 10 workers, 5,000 execs | task-carrier create/retire | 5.4 s, 7.85 host cores |
| pthread create/join churn, 16 threads x 300 rounds | `retire_task_carrier` orphaning a vCPU | 1.5 s, clean |
| mmap/mprotect/munmap churn, 16 threads, 4 GiB | `ExecutionQuiesce` gate never reopening | 16.3 s, clean |
| the same churn at 40 threads x 3,000 rounds, 8 GiB | sustained mapping and thread pressure | 715 s, clean |
| contended queue, 64 threads, mutex/condvar/barrier | inter-thread synchronization | 487 s, clean |

**`retire_task_carrier()` is eliminated.** It was instrumented to log every
invocation with `cpu >= 0` and the kernel rebuilt through the patch series. It
fired **zero** times under a 10-worker fork/exec storm (5,000 execs, 7.86 host
cores) and **zero** times at the stall itself, captured live at 49 minutes. The
carrier-retirement path does not release a live vCPU, so it is not the defect.

**Sharper characterization from the instrumented capture.** The fourth run's
stall is a scheduling-starvation report rather than a lock hang:

```
rcu: rcu_sched kthread starved for 6296 jiffies! g21977 f0x2
     RCU_GP_WAIT_FQS(5) ->state=0x0 ->cpu=7
rcu: All QSes seen, ... root ->qsmask 0x0
task:rcu_sched  state:R  running task  stack:0  pid:15
```

The host sample taken at that instant shows the whole emulator idle at the host
level while the guest has runnable work:

- 222 host threads: **216 parked in `pthread_cond_wait`** (guest tasks awaiting a
  scheduler handoff), three vCPU carriers in `pselect` inside `os_idle_sleep()`,
  one kevent IRQ thread, one SLiRP `event_loop` poll, one workqueue thread.
- **No thread is executing guest code**, yet `rcu_sched` is runnable on CPU 7.
- All named carriers are `uml-vcpu0`; none is `uml-vcpu1`..`9`.

So idle vCPU carriers are asleep in `pselect` while runnable guest tasks wait to
be handed a CPU. That is a **missed idle wakeup**, not a stranded CPU: the
carriers exist and are waiting, but nothing wakes them.

**Root cause located: `walker_mutex` is held across a guest task switch.** A
fifth run, with a guard added to `os_idle_sleep()`, stalled again at the same
point, and its live sample is decisive. Of 225 host threads:

- **four vCPU carriers blocked in `darwin_uml_static_vm_walker_lock` ->
  `std::mutex::lock` -> `_pthread_mutex_firstfit_lock_wait`** for the entire
  ten-second sample;
- four carriers idle in `pselect`;
- one kevent thread, one SLiRP `event_loop`, one workqueue thread;
- 214 guest tasks parked in `pthread_cond_wait`;
- **no thread anywhere is inside the walker doing work.**

Since no running thread holds the lock, its owner is one of the parked guest
tasks. `lock_walker()`/`unlock_walker()` take and release a host `std::mutex`
owned by the *pthread*, while the guest may switch tasks inside that region;
the parked owner's CPU is then handed to another task which cannot release it.
Every later walker acquisition blocks forever, which starves the guest's
memory-management path and produces exactly the observed RCU stalls on the
CPUs that still have runnable work.

This is the same lock G3 identified as the dominant multi-core serialization
("eight carriers spent roughly 58%-91% of their samples waiting in
`darwin_uml_static_vm_walker_lock`"), and `address_space.cpp` changed by 1,274
lines in this commit. `prune_retired_thread_state()` already contains an
out-of-band `state->walker_mutex.unlock()` for a retired domain, which is
evidence that cross-context ownership of this mutex was known to be possible.

**Fix applied: take the guest turnstile mutex outside the host walker lock.**
The Linux-host backend serializes this region with `mutex_lock(&ctx->turnstile)`,
a task-owned sleeping lock. The Darwin backend had replaced that with the host
pthread mutex alone. `arch/um/kernel/skas/mmu_darwin.c` now acquires the guest
mutex first and releases it last:

```c
void enter_turnstile(struct mm_id *id) __acquires(turnstile)
{
	struct mm_context *ctx = container_of(id, struct mm_context, id);

	mutex_lock(&ctx->turnstile);
	if (darwin_uml_vm_walker_lock(id->darwin_generation) < 0)
		panic(...);
}
```

A waiter now blocks as a Linux task and releases its virtual CPU, so the holder
can always be scheduled; the host walker lock behind it is uncontended, which
also removes the serialization G3 measured at 58-91% of carrier samples.
`enter`/`exit` are strictly nested, so the guest-then-host order cannot invert.
`mm->context.turnstile` already existed and was already `mutex_init`'d.

**Measured effect.** Walker waiters at the stall fell from **4 to 0**, the
stalled-CPU count fell from **five or six to one**, completed workloads rose
from 23 to 25, and the journald watchdog kill disappeared. A residual stall
remains with a different signature: seven carriers idle in `pselect`, 211 tasks
parked, and **no thread blocked on any host lock**, while one CPU holds runnable
work. That is a missed idle wakeup, addressed by the second fix below.

**Measured outcome across eight matched 10-vCPU Geekbench runs.** Both the wedge
and the transient stalls are fixed. A broad performance regression remains.

| Build | SC | MC | Text Processing MC | Workloads | RCU stalls | Watchdog |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| pre-refactor (2026-08-01) | **81** | **514** | 61 | 32 | **0** | 0 |
| post-refactor baseline run 1 | 79 | 471 | 33 | 32 | 2 | 0 |
| post-refactor baseline run 2 | - | - | - | 23 | 1 | 1 (**wedged**) |
| turnstile fix, run 1 | 78 | 464 | - | 32 | 1 | 0 |
| turnstile fix, run 2 | 78 | 458 | - | 32 | 3 | 0 |
| turnstile fix, run 4 | 79 | 435 | 20 | 32 | 4 | 0 |
| turnstile fix + idle guard | 78 | 458 | - | 32 | **6** | 0 |
| + rejected batch-scan trial | 78 | 456 | - | 32 | **6** | 0 |
| **+ G97 page hash** | **78** | **478** | **60** | 32 | **0** | 0 |

Conclusions supported by these numbers:

- The turnstile fix removes the **hard wedge**. No run with it has failed to
  complete all 32 workloads, and the journald watchdog kill has not recurred.
- The turnstile fix alone does **not** remove transient stalls: one, three and
  four across three runs against zero pre-refactor.
- **The G97 page hash does.** Zero stalls, all 32 workloads, 3,285 s wall
  against 3,375 s and 3,520 s for the two runs it replaces.
  (result)
- Multi-Core Text Processing is the single workload the stall damages, and its
  score tracks the stall count exactly: 20 at four stalls, 33 at two, **60 at
  zero**, against 61 pre-refactor. Full recovery of that section.
- **A broad regression against pre-refactor survives and is a separate cause.**
  Single-core 78 vs 81 and multi-core 478 vs 514, spread across nearly every
  section - single-core Clang -11.8%, multi-core Photo Filter -14.2%, most
  others -3% to -9% - rather than concentrated anywhere. Uniform cost on every
  workload points at the translation, dispatch or syscall boundary, not at the
  mapping batch path. This is the next thing to investigate, and it is
  unrelated to everything above.

### Residual regression: leading candidate is lost cross-TU inlining

Structural evidence only so far; no measurement yet. Recorded because the
elimination half is already conclusive and saves the next investigator the
same work.

**Ruled out.** These were the obvious suspects and none of them survives:

| Suspect | Why it is not the cause |
| --- | --- |
| `CpuState` layout / guest-state locality | Every `UML_GADGET_CPU_*` and `UML_GADGET_TLB_*` offset is byte-identical between the pre-refactor `gadgets/offsets.inc` and the post-refactor `layout/assembly_offsets.h` |
| Gadget bodies | The five `engine/*.inc` files differ by 2-16 lines each, all cosmetic |
| Gadget bank layout | `engine.S` lost only the two `restore_native_fp_environment` routines to `native_environment.S`, and they sat *after* every gadget include, so no gadget entry moved |
| Software TLB fast path | Unaffected by the split: the gadget probe is inline assembly, and `memory/tlb.hpp` defines the entry layout and constants `constexpr`/`inline`. Most of the old 1,258-line `tlb.cpp` became test files |

**The candidate.** Gadget translation was one 14,160-line translation unit
(`darwin/backend/cpu/arm64/gadgets/program.cpp`) and is now **15**, the largest
2,926 lines. `Builder::build()` calls `decode_branch_system`,
`decode_data_processing`, `decode_floating_simd` and `decode_load_store` **once
per guest instruction translated**; all four are bare declarations in
`program_builder_decode.hpp` and are defined in separate `program_builder_*.cpp`
files, which in turn call into the `gadget_*_selection.cpp` TUs.
`gadget_selection_api.hpp` is types plus 182 declarations with no inline bodies.

The build is `Release`, `-O3 -DNDEBUG`, with **no LTO and no unity build** -
confirmed from `CMakeCache.txt` and by grepping every `CMakeLists.txt`,
`cmake/*.cmake`, toolchain file and preset. The only `-flto` in the tree
belongs to vendored virglrenderer. So the entire decode -> select -> emit chain
that the compiler used to inline is now behind hard call boundaries.

### Corrected by the noise band: there are two causes, not one

The three-run band recorded in `HOT_PATH_EXPERIMENTS.md` under G96 makes the
per-workload comparison against pre-refactor (18926314) readable, and it
overturns two things stated above.

**The regression is not a uniform tax.** Single-core section noise is at most
one point, so every single-core delta below is real. There is a floor of about
-2.5% to -3.5% across the settled workloads, and then a tail on exactly the
translation-heavy ones:

| Workload | SC pre | SC band | SC delta | MC pre | MC band | MC delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Clang | 51 | 45 | **-11.8%** | 389 | 359 | -7.7% |
| Object Remover | 125 | 113 | **-9.6%** | 708 | 659 | -6.9% |
| File Compression | 102 | 93.5 | -8.3% | 635 | 558.5 | **-12.0%** |
| Navigation | 170 | 162.5 | -4.4% | 1210 | 1063 | **-12.2%** |
| PDF Renderer | 87 | 84 | -3.4% | 535 | 486.5 | -9.1% |
| HTML5 Browser | 73 | 70 | -4.1% | 589 | 538 | -8.7% |
| Background Blur | 71 | 68.5 | -3.5% | 587 | 539 | -8.2% |
| Photo Library | 80 | 78 | -2.5% | 636 | 585.5 | -7.9% |
| Horizon Detection | 108 | 105 | -2.8% | 835 | 787.5 | -5.7% |
| HDR | 102 | 99.5 | -2.5% | 787 | 751 | -4.6% |
| Photo Filter | 103 | 100 | -2.9% | 424 | 380.5 | -10.3% (noise 8.1%) |

Clang has **zero** spread across all three band runs, so its -11.8% is the
single most reliable number here, and Object Remover is 26.8% control-heavy -
more blocks, so more translation churn. That tail is what the inlining
candidate explains.

**Multi-core is led by something else.** Navigation -12.2% and File Compression
-12.0% lead the multi-core loss while Clang multi-core is only -7.7%, so
translation is not the multi-core story. Navigation is the memory-first
workload where the G3-era profile already recorded "TLB misses then contended
on `mappings_mutex`", and the refactor rewrote exactly that:
`address_space.cpp` 1,406 -> 1,923 lines and `native_address_space.cpp`
355 -> 2,033. The single-core versions of those two workloads are far milder,
so the cost scales with concurrency - lock contention or cross-thread traffic
rather than per-instruction cost. That needs its own investigation, narrower
than plan item B-1: find what the refactor *added* to the translate and miss
path rather than redesigning it.

**Retract the Photo Filter data point.** Multi-core Photo Filter was cited
above as a -14.2% outlier. That section alone swings **8.1%** run to run, the
noisiest in the whole suite, so it cannot carry a conclusion and should not
have been treated as evidence.

> **Outcome, for anyone reading this section later.** The candidate was right
> and it has been fixed, in two accepted steps recorded as G99 and G100 in
> `HOT_PATH_EXPERIMENTS.md`. Interprocedural optimization took multi-core
> 477 -> 497 but left single-core Clang untouched, because ThinLTO declines to
> import functions as large as the decode families. Composing those five
> families back into one translation unit took single-core 78 -> **80** against
> 81 pre-refactor, with Clang 44 -> **50** and File Compression, Asset
> Compression, HDR and Text Processing back to their exact pre-refactor scores.
> Single-core is now 75% recovered and multi-core 49%. The architecture gate
> below was relaxed deliberately, with its audit rewritten and mutation-tested
> rather than removed. The multi-core remainder is the separate MMU-path cause.

**The obvious fixes are forbidden by a registered architecture gate, and that
narrows the answer to one.** `verify_gadget_builder_ownership()` in
`tests/tooling/cmake/test_engine_object_boundaries.py` - *"keep each decode
family in a normal, independently compiled owner"* - requires all of:

- `program_builder.cpp.o` **and** each of the five `program_builder_*.cpp.o`
  present as distinct object files;
- each `decode_*` declared but **not** defined in `program_builder_decode.hpp`;
- each family `.cpp` owning its definition, with `program_builder.cpp` staying
  caller-only;
- no textual `#include` of a `.cpp`, `.cc`, `.cxx`, `.inc`, `.inl` or `.ipp`
  in any builder source;
- no `program_builder*.{inc,inl,ipp}` and no `*_impl*` fragment files.

So `UNITY_BUILD` fails the first rule - CMake would emit `unity_N_cxx.cxx.o` -
and every header-inlining route is blocked by the last three. The separation is
deliberate, not an oversight.

That leaves **link-time optimization**, which is the only route that changes no
source organization and no object file name, so the ownership rule still
passes. What has to be checked when trying it: the same audit runs `nm` over
these objects and there is a stable-engine-entry disassembly check, and under
full LTO the builder objects become bitcode - so prefer enabling IPO on the
gadget translation library alone rather than the assembly bank, and confirm the
production static-link-closure and ABI fingerprint gates still pass. Confirm
with the G96 bank-layout comparator that no gadget entry moved.

**Rejected: the idle-pending guard.** Forcing a zero `pselect` timeout whenever
a vector was already pending made stalls **six times more frequent** (6 versus
1) and cost multi-core score. The condition fires roughly 9,600 times per run,
so the guard converted a large number of ordinary idle waits into busy-polling
that burned host CPU and starved the sibling vCPUs it was meant to protect. It
has been removed; only the turnstile fix is retained. Its rationale is recorded
below because the reasoning looked sound and was wrong, which is the useful
part.

**The residual stall is deterministic, not intermittent.** Counting workload
markers before each stall report shows every stall in every run lands at the
same position:

| Run | Stalls | Position |
| --- | ---: | --- |
| pre-refactor | 0 | - |
| post-refactor baseline run 1 | 2 | both after workload 23 |
| turnstile fix run 1 | 1 | workload 23 |
| turnstile fix run 2 | 3 | workload 23 |
| turnstile fix run 4 | 4 | workload 23 |
| + rejected batch-scan trial | 6 | workload 23 |
| **+ G97 page hash** | **0** | - |

Workload 23 is **Multi-Core Text Processing**, the only Geekbench workload that
scales to roughly 1.2 host cores. No stall has ever been observed at any other
workload.

This explains why five synthetic reproducers failed: every one of them kept the
vCPUs *busy*. Text Processing has the opposite shape, one thread working while
nine vCPUs idle and repeatedly enter and leave `os_idle_sleep()`. A reproducer
must therefore combine **heavy serialization with low parallelism**, not high
contention across many runnable threads.

The RCU reports agree with that reading: the stalled CPU is always
`idle=XXXX/1/0x4000...`, dyntick nesting **1**, so the guest believes the CPU is
in kernel context rather than idle and RCU can never harvest a quiescent state
from it. A vCPU blocked in `execution_reader_begin()` while a sibling holds the
execution gate closed is exactly such a state: stopped, in kernel context, and
invisible to RCU as a quiescent CPU. `ExecutionQuiesce` is the only mechanism
that closes that gate, and its destructor waits for every reader to drain.

**Residual root cause identified: `apply_batch_locked()` plans under the closed
execution gate.** A burst-sampling capture taken *at* four live workload-23
stalls (CPUs 2, 5, 1 and 7, one per report) shows a vCPU carrier inside

```
darwin_uml_static_vm_batch_end
  uml::memory::AddressSpace::apply_batch_locked
```

for the whole of every snapshot, while 215-217 guest tasks are parked and six or
seven carriers idle. `end_batch()` reaches `apply_batch_locked()` on the
destructive path *after* constructing `ExecutionQuiesce`, so the execution gate
is already closed and every sibling vCPU is stopped in
`execution_reader_begin()`. Those stopped siblings are in kernel context, which
is exactly the dyntick nesting 1 that every RCU report shows.

**Root cause, confirmed: the planning index hashes page-aligned addresses onto
four buckets.** Resolving the sample against the UML image's symbol table -
`sample` cannot symbolize the kernel, which loads at its link address
`0x6000_0000_0000`, so every kernel frame printed as `??? (in <unknown
binary>)` - shows one carrier spending **1,255 of 1,255 samples**, the entire
ten-second capture, in a single call:

```
__um_tlb_sync+0x4c4
  darwin_uml_static_vm_batch_end
    apply_batch_locked
      apply_batch_locked::'lambda'(unsigned long long)   /* page_for */
        std::__hash_table::__emplace_unique_key_args     /* page_indices */
```

Every sample is in the two hash operations of `page_for()`, and nothing else in
the emulator is doing work. The cause is `page_indices.reserve(plan_reserve)`:

- `maximum_plan_reserve` is `64 * 1024`, **a power of two**, and it is the value
  `plan_reserve` saturates to for exactly the large batches that matter.
- libc++ keeps a power-of-two bucket count when `reserve()` asks for one, and
  then constrains a hash by masking (`hash & (bucket_count - 1)`) instead of by
  a modulus.
- `std::hash<std::uint64_t>` is the identity, and a page address has
  `log2(page_size)` low zero bits.

At 16 KiB pages a 65,536-bucket table therefore reaches
`65536 / 16384 = 4` buckets. Measured directly against the host toolchain,
inserting 65,536 page-aligned keys:

| `reserve()` | buckets | occupied | longest chain | time |
| --- | --- | --- | --- | --- |
| none | 102,877 | 65,536 | 1 | **0.002 s** |
| 1,024 | 65,536 | 4 | 16,384 | **3.274 s** |
| 65,536 | 65,536 | 4 | 16,384 | **2.590 s** |
| 65,537 | 65,537 | 65,536 | 1 | **0.001 s** |

Filling the index is quadratic, costing seconds per batch, and it runs with
`mappings_mutex` held and - on the destructive path - the execution gate closed
and every sibling vCPU stopped. That is precisely the >= 10 s
`apply_batch_locked()` in the capture, and it reaches the 21-second RCU stall
threshold across consecutive batches.

`git log -S` confirms both `page_indices` and `maximum_plan_reserve` were
introduced in **d15e98c "Refactor half-way"** - the commit at which the
benchmark first misbehaved.

**Fix applied.** `GuestPageHash` in
[address_space_types.hpp](../darwin/memory/mmu/address_space_types.hpp) folds the
page number through a multiply-shift finalizer, so the distribution no longer
depends on the bucket count the container happens to choose. The reservation and
its allocation checkpoint are unchanged. `GenerationState::mappings` is left on
`std::hash` deliberately: it is never `reserve()`d, so libc++ grows it through
primes (102,877 buckets, longest chain 1 above), and changing it would perturb
the hot `translate()` path in the same trial.

`static-mmu-batch` gained
`test_page_index_hash_spreads_page_aligned_addresses()`, which asserts the
bucket distribution rather than a wall-clock bound so the guarantee does not
depend on host speed.

**Verified.** 23/23 MMU, memory and batch tests including both TSan variants;
full 16 KiB unit suite 618/618; and a stall-catch 10-vCPU gadget-engine
Geekbench with **zero RCU stalls** over all 32 workloads. See the outcome table
above and G97 in [HOT_PATH_EXPERIMENTS.md](HOT_PATH_EXPERIMENTS.md).

**Tested and rejected: the quadratic broad-unmap scan is not the cause.** The
per-operation `state.mappings` scan was replaced with one sorted key snapshot
per batch plus `lower_bound` range lookups, guarded to build the index only when
a batch contains more than one broad unmap so the single-unmap case is
unchanged. It compiled, passed 23/23 MMU/memory/batch tests including TSan, and
made **no difference**: 6 stalls, still every one at workload 23, and
`apply_batch_locked()` still occupies the carrier for whole samples. The
expensive path is therefore the *general* per-page branch, not the broad-unmap
branch. The change was reverted because the project's acceptance rule requires
measured improvement. Result: (result).

**Superseded magnitude argument.** An earlier reading blamed the two full scans
`apply_batch_locked()` runs per broad-unmap operation - one over all of
`state.mappings`, one over all planned pages - and estimated hundreds of
millions of iterations. That reading was wrong on both counts. The scans are
real but were measured to cost nothing here (see the rejected sorted-snapshot
trial above), and the quadratic term was in the hash table, not the scans. It is
recorded because the correct objection it raised - that 65,536 *well-behaved*
hash operations is milliseconds, not seconds - is what forced the search for a
quadratic term and led to the actual cause.

**Options considered and not needed.** Splitting `apply_batch_locked()` into a
planning phase before `ExecutionQuiesce` and a commit phase under the closed
gate, validated by the existing `invalidation_epoch` re-check, remains available
if gate-closed time is still material after the hash fix. It is strictly more
invasive - it touches the TLB-coherence commit path - so it is not attempted
while a one-line hash change removes the same cost.

Two further planning refinements are available and independent of the stall:
`page_for()` materializes a planned page for every address in an unmap range,
including holes, where `clear_page()` is then a no-op; and the broad-unmap fast
path is gated on `!operation.map`, so a large protection change (a map with new
protection bits) always walks its whole range. Both are ordinary performance
work, to be measured on their own.

**Corrected analysis of the residual, and one candidate fix struck as unsafe.**

- **Do not bound the `ExecutionQuiesce` wait.** That quiesce guarantees no vCPU
  observes a stale positive TLB entry across a destructive mapping change.
  Timing it out would exchange an RCU stall for silent guest memory corruption.
- The gate-inversion theory is **eliminated**: `map()`, `unmap()`, `rebind()`,
  `destroy()` and `end_batch()` all construct `ExecutionQuiesce` before taking
  `mappings_mutex`, so no path holds the mapping lock while waiting for readers.
- The blocked-reader theory is **not** what the samples show, and it turned out
  not to be needed. No thread in any capture is in `execution_reader_begin()`;
  the idle carriers sit in `darwin_ipi_idle_drain()` and `os_idle_sleep()`,
  which is ordinary idling. The stalled CPU is the one **doing** the work: its
  own carrier is inside `__um_tlb_sync` -> `apply_batch_locked` for the whole
  capture, so it reports no quiescent state and no timer ticks. Sibling
  blocking, if any, is secondary.

A missing RCU-idle annotation was considered and **rejected**: `arch_cpu_idle()`
calls `um_idle_sleep()` without annotating, but the generic `default_idle_call()`
already wraps it in `ct_cpuidle_enter()`/`ct_cpuidle_exit()`, so the idle path is
annotated. Dyntick nesting **1** in every stall report therefore does not mean a
sleeping CPU is miscounted; it means the named CPU is genuinely **in kernel
context** and not progressing - which is answered above: its carrier is inside
`__um_tlb_sync` -> `apply_batch_locked`, not blocked on anything.

A seventh reproducer, one worker holding a rotating token while nine vCPUs idle
and cycle through `os_idle_sleep()` plus mapping churn, also completed cleanly
in 190 s. Reproduction outside Geekbench remains unachieved, and is no longer
needed for this cause: the degenerate hash is reproducible directly against the
host toolchain in a few lines, and `static-mmu-batch` now asserts against it.

**Method note that unblocked this.** `sample` cannot symbolize the UML kernel,
which the emulator loads at its link address near `0x6000_0000_0000`, so nearly
every frame in a 32 MB capture printed as `??? (in <unknown binary>)` and the
captures read as "no emulator thread is doing anything". Resolving those
addresses against `nm` output from `vmlinux.unstripped` for the matching build
turns the same captures into a per-thread answer immediately. Do this first for
any future host-side stall investigation.

**Note for future A/B work.** Rebuilding identical sources did not reproduce the
frozen turnstile artifact byte-for-byte, so the emulator embeds something
non-reproducible, most likely a kernel build identifier. Compare frozen
artifacts, never rebuilds.

**Rejected second fix, retained for its reasoning: never enter an unbounded idle
wait with an IPI already pending.**
`os_send_ipi()` omits its pipe byte when `previous_pending != 0` and returns
early without signalling when `ipi_idle_waiting[cpu]` is set, so a carrier that
enters `os_idle_sleep()` with a vector already pending has no remaining wake
source, and its `pselect` timeout is `NULL` whenever the cooperative timer is
inactive. `darwin_ipi_pending()` is now checked before the wait and forces a
zero timeout. Measured in isolation this condition occurs about 9,600 times per
run, so it is common; on its own it did not clear the stall because the walker
deadlock dominated, and it is only meaningful combined with the turnstile fix.

**Performance regression from the refactor, independently confirmed.** Matched
10-vCPU gadget-engine Geekbench, same rootfs and host:

| Build | Single-core | Multi-core |
| --- | ---: | ---: |
| pre-refactor (2026-08-01) | **81** | **514** |
| post-refactor baseline | 79 | 471 |
| change | -2.5% | **-8.4%** |

The multi-core loss is consistent with carriers waiting on the walker lock
rather than running guest work.

**Hypotheses eliminated on the way, with evidence:** the interrupt-registration
ABI change (kernel checks the result), an `execution_lifecycle_mutex_`
reader/writer convoy (the writer is only taken at lifecycle boundaries),
`retire_task_carrier()` orphaning a vCPU (instrumented; fired zero times under
fork/exec churn and zero times at the stall), and an unbounded idle `pselect`
missing an IPI wakeup (guarded; the condition fired 9,643 times, so it is
routine and benign, and the stall persisted).

**Superseded note.** `os_idle_sleep()` waits on `pselect(ipi_fd, ..., timeout)`
where the timeout comes from the per-CPU cooperative timer deadline and is
`NULL` when that timer is inactive, making the wait unbounded. The only wakeups
are an IPI pipe byte and `DARWIN_IPI_SIGNAL`. `os_send_ipi()` writes the pipe
byte only when `previous_pending == 0 && !target_in_guest`, and returns early
without signalling when `ipi_idle_waiting[cpu]` is set. The interaction between
that early return, the pipe-then-mask drain order in `darwin_ipi_poll()`, and an
unbounded `pselect` is the most likely place for a lost wakeup, and it is the
next thing to instrument: log every `os_send_ipi()` decision path together with
`ipi_idle_waiting`, `guest_executing`, and `previous_pending`, then reproduce.

All kernel instrumentation has been reverted and the managed checkout verified
back to canonical tree `640b3d1404118f336d70496483f87f244076e574`.

**This historical manual patch-editing procedure is superseded.** Changing the
kernel now means editing/reordering the topic commits with `git rebase -i` and
`git commit --amend`, updating the reviewed final tree in metadata, and running
`kernel/rebase-patches.sh`. That command alone regenerates patch bytes,
SHA-256 values, stable patch IDs, subjects, and ordering with Git before a
fresh replay.

Historical detail retained below.

Observed state, from the guest console and a host `sample` of the wedged
process:

- `rcu: INFO: rcu_sched detected stalls on CPUs/tasks: 4-...!: (0 ticks this
  GP) ... 9-...!: (1 ticks this GP) ... (detected by 0, t=21002 jiffies,
  ncpus=10)`. Both stalled CPUs carried a `geekbench_aarch` task in state `R`
  with dynticks nesting 1, so the vCPU had runnable work and recorded no ticks.
- `systemd-journald.service: Main process exited, code=killed, status=6/ABRT`,
  `Failed with result 'watchdog'`.
- The host process held steady at ~109% CPU, one core, for over an hour with
  zero console output. Its busiest thread was CPU 0's idle carrier in
  `os_idle_sleep`, sampling 2474/2561 in `pselect` and 78 in `poll`.
- 206 host threads were named `uml-vcpu0` and none `uml-vcpu1`..`9`.
- The host was 80% idle throughout, so the guest was not starved by host load.

Reproduction is not yet achieved. Four targeted payloads, each aimed at a
subsystem the refactor changed, all completed cleanly on the post-refactor
build at 10 vCPUs:

| Payload | Subsystem targeted | Result |
| --- | --- | --- |
| `/bin/true` fork/exec storm, 10 workers, 5,000 execs | task-carrier create/retire | 5.4 s, 7.85 host cores |
| pthread create/join churn, 16 threads x 300 rounds | `retire_task_carrier` orphaning a vCPU | 1.5 s, clean |
| mmap/mprotect/munmap churn, 16 threads, 4 GiB | `ExecutionQuiesce` gate never reopening | 16.3 s, clean |
| the same churn scaled to 40 threads x 3,000 rounds, 8 GiB | sustained mapping and thread pressure | 715 s, clean |

Two host-side hypotheses were examined against the source and rejected:

- The interrupt-sequence registration ABI changed from `void` to `int` in this
  commit, but the kernel call site checks the result and calls
  `fatal_userspace` on failure, so a silent unregistered sequence is not
  possible.
- `Context::thread_execution_state()` holds a `shared_lock` on
  `execution_lifecycle_mutex_` for the whole guest run, but the matching
  `unique_lock` is taken only in the constructor, `begin_run()`, and
  `end_run()`, so no writer contends during steady-state execution.

The remaining untested suspect is the carrier-retirement code this commit added
to `kernel/patches/series/0004-uml-add-Darwin-host-service-implementation.patch`
(`retire_task_carrier`, `release_cpu_bootstrap`, and their
`pthread_cleanup_push` handlers). `retire_task_carrier` releases
`cpu_execution_owner[cpu]` and clears `task->cpu`/`task->runnable` on thread
exit without handing the CPU to another carrier; if it ever fires on a live
carrier, that vCPU is stranded in exactly the observed way. This is a suspect,
not a diagnosis.

Note for future investigation: the kernel is patched through `kernel/patches/`,
not by editing `linux/` directly, so `git show <commit> -- linux/` reports no
kernel change even when the kernel changed substantially.

The reproduction harness and payload are retained under the session scratchpad
rather than the test tree, because they do not yet reproduce the failure. They
do encode two real guest-toolchain workarounds: `cc1` is at
`/usr/libexec/gcc/aarch64-linux-gnu/14` and is not found by a plain `gcc`
invocation, which fails `posix_spawnp`.

### New measurement controls for gadget-engine work

Recorded in full as G96 in `HOT_PATH_EXPERIMENTS.md`. Neither control changes
runtime behavior.

- `tests/tooling/cmake/test_gadget_bank_layout.py`, registered as
  `gadget-bank-layout-snapshot`, records every `uml_arm64_gadget_*` entry's
  offset, 64-byte cache-line phase, and size, and compares two builds. It exists
  because G17 and G36 were rejected for a bank-layout shift that G35 later
  learned to pin, so those results are not trustworthy as stated. Calibration
  measured that one gadget growing 16 bytes relocates 5,925 later entries.
- The four `tests/perf` runners accept
  `--core-class={performance,efficiency}`. Efficiency runs launch through
  `taskpolicy -c background`, whose process-wide QoS clamp places the task on the
  efficiency cores that dominate iOS. Verified by measurement on an Apple M1
  Ultra (16 performance, 4 efficiency cores): a four-thread spinner takes
  0.213 s unclamped and 0.907 s clamped. Efficiency runs use `--cpus 4` to match
  the four efficiency cores; performance runs keep `--cpus 10`.

### In flight

Guest `YIELD` now lowers to host `ISB` in both translated engines, because Arm
defines `YIELD`'s only architectural effect on symmetric multi-threading
hardware, which Apple Silicon does not have. `ISB` is a strictly stronger
barrier than the hint it replaces, so no guest-visible behavior changes. The
gadget bank snapshot is byte-identical across the change, and the guest rootfs
contains 378 `YIELD` sites, so the substitution is reachable. Its measured
acceptance is pending and it is not yet accepted.

## Sixth independent audit: historical closure claim

The sixth pass reported all twelve of its then-known findings closed. The
seventh checklist above supersedes that completeness claim. Rootfs file
management remains deliberately deferred to the later testing-image redesign.
No guest boot, LTP, browser, Node.js, Go, benchmark, or controlled performance
campaign was used for the historical claim.

| Area | Resolution |
| --- | --- |
| Independent ARM64 oracle | `generate_a64_instruction_allocations.py` derives the checked-in 983-row allocation catalog from LLVM MC 22.1.8 rather than copying test bindings. The catalog contains 79 distinct masks and only 21 exact-word masks. Validation joins section plus architectural form, opcode provenance, mask, and value. Seven negative fixtures include the formerly accepted self-consistent wrong-form mutation. |
| Exact engine evidence | Fresh 16 KiB and 4 KiB reports each bind all 983 stable IDs. JIT records 670 retired and 40 executed/non-retired cases with 273 exact feature skips; interpreter and gadgets record 893 retired and 90 executed/non-retired cases. The two reports are byte-identical, SHA-256 `d4bf8d90db910807e6adbf2bf19e06f1d32ccd98fd60cfa7b2d26a583d3ed139`. |
| Native 4 KiB ownership and failure safety | Native-shadow state is domain-owned and retired on every normal or abnormal run exit. Since `MAP_FIXED` is process-global, 4 KiB native callbacks now share the process address-space admission gate; a same-VA two-handle test covers serialization, queued cancellation, flush-before-handoff, isolation, and retry. Public `darwin_uml_vm_*` tests retain sequential reuse, abnormal return, and allocation failures in map, protect, unmap, and refresh. C boundaries contain exceptions, preserve sibling state, roll back mappings/protections/bookkeeping, and permit immediate retry. Native shadow storage is data-only and strips execute permission. |
| GPU/VNC ownership | Queue notification descriptors use RAII, memory-table replacement is transactional, and VNC initialization guards the live screen until event-thread ownership succeeds. Production dispatch tests cover repeated replacement, reset-stage failures, retry, descriptor balance, post-listen thread failure, and exact port reuse. Canonical rootfs-free normal and ASan+UBSan GPU workflows require the ownership test whenever the target exists. |
| Engine modularity | Gadget catalog/selection/encoding, build, and execution responsibilities are separated. JIT cache, linking, publication, diagnostics, semantic memory, emission, compilation, verification, lifecycle, and test-only support have distinct internal owners; production and test-only lifecycle compositions are separate. The production JIT object remained byte-for-byte identical across the split. Advisory size reviews were refreshed with zero changed-since-review rows. |
| Engine boundary enforcement | The File API audit classifies every concrete engine and rejects cross-engine link, object, aggregate, include, and source-include edges in both directions. Positive isolated ownership and a 24-case negative matrix exercise the rule. Interpreter and gadget paths cannot reach JIT publication. |
| Linux and dependency policy | The 64-bit vhost-user DMA-mask correction is independent topic 11. Kernel and dependency validators compare the complete patch directory with their manifests and reject unlisted files. Topic-path ownership is singular except for the exact declared `virtio_uml.c` owners 0007/0011; undeclared, stale, and mismatched overlaps fail. A fresh authenticated eleven-topic replay verifies final tree `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d` and the 162/13 delta policy. |
| Bounded cleanup and coverage | Network restart now deterministically pauses with a queued transmit and proves that no prior-session frame executes after reopen. The NEON inventory input lives under `tests/unit/arm64`. The source audit covers 1,060 files: 60 above an advisory guideline, 42 without optional review context, and zero changed since review. |

### Sixth-audit closure verification

| Check | Fresh result |
| --- | --- |
| 16 KiB host graph | build 1130/1130; unit 562/562; tooling 23/23 |
| 4 KiB host graph | build 1070/1070; unit 563/563; exact-evidence validator 1/1 |
| Sanitizers | full ASan+UBSan 562/562; focused native 4 KiB ASan+UBSan 2/2; TSan 4/4; malformed-loader ASan+UBSan 1/1 |
| GPU gates | normal 1/1 and ASan+UBSan 1/1; both audited graphs contain 562 targets / 587 registrations |
| Network gate | rootfs-free deterministic SLiRP ARP/restart test 1/1 |
| Apple product graphs | iOS device and simulator builds/audits passed at 21 targets each; archive symbol scans found no executable-publication reference outside `jit_arena.cpp` |
| Linux/dependencies | fresh authenticated eleven-topic replay, exact final tree, 162/13 delta, topology/overlap policy, exact patch inventories, dependency manifest/negative tests, and ABI identities passed |
| Source organization | 1,060 files reported; 60 above advisory guidance; 42 without optional review context; zero changed since review |

A fresh cached-archive replay reverified the pinned signature, packaged tree,
all eleven patch identities/order, canonical final tree, and 162/13 delta. Its
temporary checkout was removed after verification. Generated timestamps,
commit/build identities, compiler notes, package layouts, and binary hashes
remain dynamic observations rather than byte-reproducibility requirements.

## Sixth independent audit: baseline findings (historically reported closed)

This was the reproduction baseline that reopened the work after the fifth
audit: four correctness/completion blockers, five enforcement or exception-
safety gaps, and three bounded cleanup items. The sixth pass reported every
acceptance requirement in these tables satisfied. The seventh audit reopened
some underlying guarantees, so the original wording is retained only as dated
implementation history.

### Baseline P1 findings (historically reported closed)

| Finding | Historical baseline evidence | Acceptance requirement |
| --- | --- | --- |
| Instruction allocation evidence is not independent | All 983 rows in `tests/unit/arm64/a64_instruction_allocations.tsv` use `ffffffff` masks, and every allocation value is byte-for-byte the encoding already present in `a64_instruction_evidence_bindings.tsv`; form and feature fields also mirror the other catalogs. `validate_a64_instruction_evidence.py` therefore checks one copy against another. A fixture that put the same word from a different architectural form into both sources printed `accepted self-consistent nonmatching architectural encoding`. The existing encoding negative mutates only one side. | Derive or import allocation masks/values from an independent architectural source, not the test bindings. Validate the selected word against that allocation/form metadata and add a negative that changes the same word consistently in every test-owned catalog. Regenerate both page-size evidence reports only after that negative fails for the intended reason. |
| The production 4 KiB native shadow is process-global and survives run retirement | Non-user `darwin_uml_vm_map_generation()` calls discard the generation and enter `darwin_uml_vm_map()` in `darwin/memory/mmu/static_api.cpp`. The 4 KiB shadow uses the process-static `native_address_space()` in `native_address_space.cpp`; `Context::end_run()` retires only the domain-owned software address space. The fresh-run loader fixture calls `darwin_uml_static_vm_*` directly and therefore bypasses the native wrapper whose state it purports to isolate. | Give native-shadow state explicit run/domain ownership or retire it completely on every normal and abnormal run exit. Add 4 KiB sequential-run, abnormal-return, and concurrent-handle tests through `darwin_uml_vm_*`; mappings, materialized host slots, protections, and bookkeeping must neither survive nor cross domains. |
| Guest-driven GPU memory-table replacement leaks queue notification descriptors | `VhostUserServer::configureMemory()` closes old memory mappings and then assigns `queues_ = {}` without closing each `Queue::callDescriptor`; queue setup transfers those raw descriptors into the array. A later valid memory-table replacement, or a failure after the reset point, loses them permanently. The current dispatch-exception test throws after receive and never invokes production reconfiguration. | Make queue notification ownership RAII or explicitly close it before every reset. Exercise real dispatch for repeated successful replacements and for each failure after reset begins; live-descriptor counts must remain constant and retry must work. |
| The two largest engine translation units still combine unrelated responsibilities | `darwin/cpu/arm64/gadgets/program.cpp` is 12,824 lines and combines the symbol catalog, selection tables, decoding, `Builder::build`, and execution entry points. `darwin/cpu/arm64/jit/jit_executor.cpp` is 11,047 lines and combines linking, publication, diagnostics, semantic helpers, emission, compilation, caches, test hooks, and lifecycle. This is the logical split required by `REFACTORING_PLAN.md`; relaxed numeric guidelines do not close mixed ownership. Both source-review rows are now reported changed since review. | Split by the stable responsibility boundaries already listed in the plan, using normal `.cpp`/`.hpp` files. Preserve decoder priority, emitted streams, hot-path layout, and benchmark behavior; keep test hooks out of production ownership. Refresh advisory review records after the logical split, not instead of it. |

### Baseline P2 findings (historically reported closed)

| Finding | Historical baseline evidence | Acceptance requirement |
| --- | --- | --- |
| Cross-engine dependency enforcement is asymmetric | `scripts/audit-cmake-file-api.cmake` rejects native-code-engine to gadget-engine edges only. A copied manifest with the reverse gadget-engine to native-code-engine edge still passed the 551-target audit. The current graph is clean, but the plan forbids every engine-to-engine edge and isolated adapter tests do not inspect dependency closure. | Classify every concrete engine target and reject every cross-engine link, object, aggregate, and include edge in both directions. Add positive isolated-link tests and negative manifest/configuration fixtures for each direction. |
| Native-shadow failure coverage proves only mapping rollback | The 4 KiB fault-injection cases call `darwin_uml_vm_map()` only. Production protect, unmap, and refresh paths have distinct allocation checkpoints and separate C wrappers, but no injected test reaches them. This is a coverage gap rather than a reproduced ordinary-input failure. | Inject every reachable occurrence through each production C wrapper. Require contained exceptions, unchanged mapping/slot/protection and sibling bytes, balanced state, and immediate successful retry. |
| `VncDisplay` can leak a live server when event-thread construction throws | `vnc_display.cpp` initializes and validates the listening server before assigning `eventThread_`. If the `std::thread` constructor throws, the enclosing object destructor is not run and raw `screen_` plus its listening socket remain active. Existing startup injection occurs before display construction. | Hold the initialized screen in an initialization guard until construction completes. Inject after listen and before thread ownership, then prove cleanup, unchanged descriptor count, port reuse, and successful retry. |
| GPU ownership coverage has no canonical rootfs-free gate | Canonical unit and unit-sanitizer presets disable the GPU. Even a GPU-enabled graph does not list `gpu-service-fd-ownership` as a required conditional, so removing its registration while leaving the manifest row conditional would pass the architecture audit. | Add a canonical host-only GPU unit workflow, including an appropriate sanitizer variant, and require `gpu-service-fd-ownership` whenever its target exists. Add the new production reconfiguration and display-construction cases to that gate. |
| One small generic Linux correction remains hidden in the broad device patch | The plan requires three generic corrections to be independent. Topics 9 and 10 are separate, but the 64-bit virtual-transport address-width correction remains an unguarded hunk in broad topic 7 (`kernel/patches/series/0007-...patch`, around lines 886-902). | Extract it into its own tiny, hash-checked, upstreamable topic; update topology, extraction decisions, next-version actions, replay identity, and exact-tree evidence. Drop it independently when a future base contains it. |

### Baseline P3 findings (historically reported closed)

- Kernel and dependency patch validators verify referenced rows but do not
  reject extra unlisted `.patch` files. Current real directories match their
  manifests, but negative fixtures with one extra patch were accepted. Compare
  the complete directory inventory with each manifest and add an extra-file
  negative.
- Network restart coverage proves receive/notification cleanup but never pauses
  the worker with a transmit queued. Add a deterministic queued-transmit
  restart case and prove no prior-session frame executes after reopen.
- `tests/arm64-neon-inventory.s` is a unit-test input referenced from
  `tests/unit/arm64/fp-simd-a.cmake` but still lives outside `tests/unit`. Move
  it under the owning unit directory and update its manifest/reference.

### Historical recommended order (superseded)

1. Repair the independent instruction oracle and its self-consistent negative;
   broad engine evidence is not an acceptance gate until this is sound.
2. Domain-own or retire the 4 KiB native shadow, then complete all native C-
   boundary failure cases and rerun both page sizes plus sanitizers.
3. Repair GPU queue/display ownership and make the rootfs-free GPU gate
   mandatory before relying on descriptor evidence.
4. Make cross-engine enforcement symmetric, then perform the planned logical
   engine-file split in behavior-neutral stages with focused code-generation
   and performance comparisons.
5. Isolate the small Linux correction, enforce complete patch inventories,
   close the transmit-restart and unit-file-placement items, and replay from a
   pristine authenticated release source.
6. Repeat host, Apple-package, guest, and controlled performance acceptance.
   Rootfs management remains explicitly out of scope.

### Sixth-audit baseline verification (historical)

| Check | Fresh result |
| --- | --- |
| 16 KiB host unit selection | 562/562 passed in `.build/audit-round6-main-unit16` |
| 4 KiB host unit selection | 563/563 passed in `.build/audit-round6-main-unit4k` |
| Raw tooling selection | 22/22 passed after evidence production; the self-consistent oracle reproduction above still passed unexpectedly |
| Architecture/source audits | The then-current graphs passed (551-target base and 562-target GPU graph); the advisory source report covered 1,034 files, 53 above guideline, 42 without optional review context, and six changed since review |
| GPU-enabled host fixture | A fresh default-prefix graph built; the then-current `gpu-service-fd-ownership` passed 1/1 but lacked the replacement and post-listen cases above |
| Sanitizers | Fresh canonical loader ASan+UBSan passed 1/1; fresh canonical TSan selection passed exactly 4/4 |
| Linux, ABI, and dependencies | Ten-patch exact replay still matches tree `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`; 162/13 policy, both page-size imports, topology/action fixtures, ABI identities, pinned-source dependency tests, pristine LibVNCServer 0.9.15, verified default prefix, and final-link allowlist passed |
| Guest/performance campaigns | No guest boot, LTP, browser, or benchmark was run in this audit; these green host results are not performance evidence |

## Fifth independent audit: historical closure claim (superseded)

The following repository-owned P1, P2, and P3 rows were reported closed at the
end of the fifth pass. The sixth audit above found missing semantic independence,
production-path isolation, ownership, enforcement, and logical-split coverage,
so this section is a dated implementation record rather than current closure.
Its detailed tables retain the baseline wording and the acceptance requirements
that were believed satisfied at that time.

| Area | Closure |
| --- | --- |
| Exact ARM64 evidence and engine boundaries | A checked-in 983-row independent allocation catalog validates encoding masks/values separately from binding labels. Evidence records exact required and observed host features; six negatives cover encoding-only and unrelated-feature mutations. Shared native helpers moved to `uml_arm64_execution_helpers`; native-code targets contain no gadget objects, links, symbols, or headers, and the architecture audit now rejects that edge. |
| Runtime publication and admission | Context and provider entries use deferred, allocation-free commit with rollback of every uncommitted mutation. A provider-B failure leaves A unchanged, B retryable, and only a committed overlap fail-closed. Stop mutation and queued-run admission share one mutex, with deterministic post-admission/wait-boundary coverage proving a queued stop returns before the active run is released. |
| MMU correctness | Software mappings, history, and invalidation updates are exception-atomic under deterministic Nth-page, Nth-operation, and history-publication failures. Batches retain exact generation-state identity and reject retired/replaced state with `-EAGAIN`. Native 4 KiB entry points contain C++ exceptions and roll back mappings, containers, and sibling state after every injected allocation site. |
| GPU ownership | Received `SCM_RIGHTS` descriptors acquire fixed-capacity RAII ownership immediately after `recvmsg` and release only after successful transfer. Repeated malformed-version, oversized, truncated, dispatch-exception, allocation-failure, and valid-transfer cases pass without leaks or double-close. Startup failure injection is compiled only into a test-owned service variant. |
| Reproducible gates and policy | Reset, evidence producers, and validation form standalone deterministic presets; raw tooling is non-destructive. A canonical loader ASan+UBSan preset passes. Next-version policy detects applied, already-contained, and genuine-conflict states and emits distinct carry/drop/retest/upstream actions, with fixtures for each outcome. ABI/provider/TSan documentation and ADRs match the implementation. |
| Dependencies and Apple products | The default private prefix was rebuilt from pinned release inputs and passes receipt, payload, architecture, platform, deployment-target, and static-only verification. macOS, iOS-device, and iOS-simulator archives pass architecture and raw/install-tree C/C++ consumer checks. Device/simulator archives expose executable publication only through the explicit JIT arena; interpreter and gadget paths contain neither W+X nor RW/RX publication. |

### Audit-baseline P1 findings (closed)

| Baseline finding | Baseline reproduction | Acceptance requirement (satisfied) |
| --- | --- | --- |
| Exact instruction evidence does not prove encoding-to-form identity or exact native-feature skips | `tests/tooling/arm64/validate_a64_instruction_evidence.py` compares two copies of the same form label and checks only that an encoding has eight hexadecimal characters. It never validates the word against independent allocation/form metadata. Its evidence schema accepts `skipped-host-feature` without the required or missing feature. The negative fixtures change the label rather than keeping both labels correct and changing only the encoding. The real validator accepted `0b220027` self-labelled as `ADD (shifted register)` and also accepted an intentionally unrelated inventory feature requirement. | Validate each encoding with canonical decoder masks/allocation metadata independent of the checked-in binding label. Add a negative fixture that changes only the encoding. Record the exact required feature and observed host capability in binding/evidence rows and compare them with `native_feature_requirements`; continue interpreter and gadget execution when native execution is skipped. Regenerate both page-size reports. |
| The native-code engine still depends on the gadget engine | The plan forbids engine-to-engine edges, but `darwin/cpu/CMakeLists.txt` links both native-code targets to `uml_arm64_gadgets`, and `uml_cpu_jit_component` embeds gadget objects. `jit_executor.cpp` calls a helper defined in `gadgets/memory_transfer.cpp`. The File API audit passes because `scripts/audit-cmake-file-api.cmake` permits every same-layer edge before applying any target-specific rule. | Move the shared transfer primitive to a neutral CPU/memory contract target. Remove gadget links, objects, symbols, headers, and source ownership from every native-code target. Add engine-level metadata or an explicit intra-CPU prohibition so the architecture audit rejects this edge, then pass isolated-link and product graphs. |
| Context and network-provider publication are not one transaction | `Emulator::run()` creates `ActiveContextScope` before `NetworkSession`. A second context publication permanently clears the first context's fallback bit; if the second provider publication then throws, context destruction removes only the new entry and does not restore the first. The focused reproducer printed `failed=1 before=3 after=0`, proving that handle A changed from interpreter routing to fail-closed after handle B failed before entry. Existing tests fail each registry independently and miss this sequence. | Publish context/provider state jointly or roll back every uncommitted cross-registry mutation. Test context-B success followed by provider-B failure while A's unbound CPU and network callbacks work, require A to remain unchanged, and prove B can retry. Preserve fail-closed behavior only for an overlap that actually commits. |
| Software-MMU mutations are not exception-atomic | `map_locked()` inserts a multi-page mapping page by page and publishes its one change record only after the loop. A later allocation failure leaves earlier pages installed with stale sequences. `changed_locked()` increments the mapping sequence, then performs a potentially allocating history insertion, and only afterward publishes the code change. Batch operations have the same partial-application risk. Current injection fails only the first one-page/single-operation allocation. | Preallocate or roll back mapping and history mutations, and make code invalidation fail-safe once executable state changes. Add deterministic Nth-page, Nth-operation, and history-publication failures. Require no partial mappings, coherent mapping/code sequences, no stale translated code, balanced batch state, and successful retry. |
| A mapping batch can commit into a replacement generation | Batch begin stores only a numeric generation and its current invalidation epoch. Another thread can destroy and recreate that generation; batch end resolves the number again, obtains the new state, and accepts the same initial epoch, allowing operations queued for the retired state to populate its replacement. | Retain and validate the exact `GenerationState` identity, and reject a retired or replaced state with `-EAGAIN`. Queue an old mapping, destroy/recreate the generation on another thread, and prove the old batch cannot resurrect its backing. |
| The 4 KiB native-shadow path can throw through C and leave orphaned host mappings | `darwin/memory/mmu/static_api.cpp` directly calls allocating `NativeAddressSpace` methods from `extern "C"` entry points. `materialize_slot_locked()` creates a host mapping before inserting into allocating containers, and map/protect/unmap/refresh mutate incrementally. The 4 KiB native-MMU tests contain no allocation-failure coverage. | Contain every native-shadow C boundary and make host mapping/bookkeeping changes rollback-safe. Inject after host mapping and at each container-growth/multi-page point; no exception may escape, no orphan slot may remain, live sibling pages must remain intact, and retry must succeed. |
| A stop notification for a queued embedded run can be lost | Embedded admission waits on `embedded_kernel_run_changed` under `embedded_kernel_run_mutex`, but the stop predicate changes under the context mutex and notification occurs without the wait mutex. A stop can land after a false predicate check but before the wait begins. The current test signals `second_started` before calling `uml_emulator_run`, so it does not prove the second thread reached the wait boundary. | Synchronize predicate mutation with the admission mutex or use an equivalent monotonic wait primitive. Add deterministic hooks after handle admission and at the wait boundary; a stopped queued run must return before the active run is released. |
| Malformed GPU service messages leak received descriptors | `VhostUserServer::ReceivedMessage` owns raw descriptor integers, and the only close loop is after successful dispatch. Twelve valid-version unsupported requests carrying one descriptor each increased the live service descriptor count from 9 to 21. Invalid-version, oversized, truncated, allocation-failure, and dispatch-exception paths can leak before or after ancillary-data extraction. | Give every received descriptor RAII ownership immediately after `recvmsg`, with explicit release only on successful ownership transfer. Repeated malformed-version, oversized, truncated, unsupported-request, and injected-allocation cases must leave the descriptor count unchanged and valid transfers must neither leak nor double-close. |

### Audit-baseline P2 findings (closed)

| Baseline finding | Acceptance requirement (satisfied) |
| --- | --- |
| Instruction-evidence test orchestration is order-dependent | A raw `tooling` label run selects both reset and validation but not unit producers; it failed 21/22 after reset erased the evidence. The supported reset-excluding selection passes only when a complete unit run has already populated the directory. Give reset, producers, and validation an explicit deterministic pipeline so every advertised preset has clear standalone/precondition semantics, stale producer files are removed before production, strict unit selection stays Python-free, and tooling cannot erase evidence it is about to validate. |
| Malformed-loader sanitizer coverage has no canonical preset | The normal ASan preset disables the kernel runner and selects only `unit`, while the malformed-loader fixture is conditional on the kernel runner and labelled `tooling`. The previously created targeted ASan artifact still passes 1/1, but the documented canonical ASan commands register no loader test. Add a checked-in targeted loader ASan configure/build/test preset or an equivalent locally reproducible gate. |
| Next-version policy is classified but not acted upon | `verify-series-policy.py` validates that policies such as `drop-when-base-contains-fix` are spelled and assigned consistently, but `check-next-version.sh` unconditionally applies every patch and reports only a generic conflict. Make the checker identify already-contained fixes, distinguish the recorded policies, and emit the required drop/retest/upstream action per affected path. Add fixtures for an already-contained fix and a genuine conflict. |

### Audit-baseline P3 findings (closed)

- Move `uml::gpu::testing::StartupFailure`, its global atomic, and failure
  checkpoints out of the production GPU library or compile them only into a
  module-owned test variant.
- Correct `kernel/abi/README.md` to describe the independently versioned public
  API and kernel bridge, their separate histories, and both update procedures.
- Amend or supersede ADRs 0002/0003: the application owns and stops the injected
  network provider, while the installed callback context is caller-owned and
  has no stop callback.
- Correct `docs/testing.md`: the then-current TSan label selected four tests, not five,
  and its evidence-reset fixture is not selected.

### Implemented order

1. Preserve the dirty-tree baseline and split work by invariant, without
   committing or staging.
2. Repair the independent instruction form/feature joins and the forbidden
   native-code-to-gadget edge first; these determine whether the broad engine
   matrix is meaningful.
3. Fix joint registry publication and queued-stop admission, then add the exact
   interleaving tests before changing other lifecycle code.
4. Address software-MMU transaction/history behavior, stale batch identity, and
   4 KiB native-shadow containment as one memory-correctness campaign. Run both
   page sizes and sanitizer variants after every sub-step.
5. Give received GPU descriptors RAII ownership and move the startup-failure
   seam out of production.
6. Make evidence and loader-sanitizer workflows reproducible, execute the
   next-version policy actions, and correct the bounded documentation drift.
7. Rebuild the default private dependency prefix, repeat all host/product and
   guest acceptance matrices, and only then rerun controlled benchmarks.
   Rootfs-management redesign remains deferred.

### Fifth-audit reported verification (historical)

| Check | Dated result |
| --- | --- |
| Deterministic 16 KiB evidence pipeline | architecture/build targets passed; reset, 561 producers, and validation passed 563/563 in 286.37 seconds |
| Deterministic 4 KiB evidence pipeline | architecture passed with 553 targets / 585 registrations; reset, native 4 KiB coverage, all producers, and validation passed 564/564 in 279.60 seconds |
| Exact instruction reports | both page sizes contain 983 stable IDs, 670 retired native executions, 40 executed/non-retired cases, and 273 exact host-feature skips; interpreter and gadgets continued for every native skip |
| Raw tooling-label selection | non-destructive selection passed 22/22 after evidence generation; it does not select reset or erase producer files |
| Focused concurrency and memory checks | joint registry rollback/retry, queued-stop wait-boundary, Nth-failure MMU/history, stale-generation batch, and native 4 KiB rollback cases all passed in the page-size pipelines |
| GPU descriptor ownership | the default-prefix GPU graph passed 562 targets / 586 registrations; 64 repetitions of each malformed/allocation/dispatch case plus valid ownership transfer passed 1/1 |
| Sanitizers | canonical malformed-loader ASan+UBSan passed 1/1; canonical TSan selection passed exactly 4/4 in 4.44 seconds |
| Linux, ABI, dependencies, and packages | authenticated ten-patch replay matches final tree `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`; 162/13 delta policy, independent public/kernel ABI v4 identities, exact 16/4 KiB active-import checks, then-current manifest/action fixtures, pristine LibVNCServer 0.9.15, rebuilt default-prefix verification, the then-current 16 KiB production kernel/app final-link allowlist, and all three Apple C/C++ package-consumer graphs passed |
| Source organization | the advisory report covers 1,034 files: 53 above guideline, 42 without optional review context, and six changed since review; these are review signals, not a strict line-count gate |
| Guest/performance campaigns | no Node.js, Go, browser, benchmark, or other heavy interpreter workload was run; rootfs redesign remains explicitly deferred and external/device campaigns remain external acceptance |

## Fourth independent audit: historical closure record

These requirements were reported satisfied at the end of the fourth audit.
They are retained as a closure record; the fifth-audit section above records
the stronger joins, state transitions, and reproducible gates now in force.

### P1 requirements (reported closed)

| Original finding | Audit evidence | Closure requirement (satisfied) |
| --- | --- | --- |
| Instruction closure is not mechanically sound | `tests/tooling/arm64/validate_a64_instruction_evidence.py` explicitly bound only 183 of 983 inventory IDs; 800 rows used decoded-mnemonic/form heuristics. The then-generated report had only 902 unique evidence selections. `BASE-0009` and `BASE-0011` both bound extended-register encoding `0x2b220027`; `FPSIMD-0234` and literal-form `FPSIMD-0235` both bound immediate encoding `0x3c403c5d`. Program evidence also credited every stored word with one aggregate engine result, including words not proved retired. | Give every evidence-producing variant explicit stable-ID and architectural-form metadata. Record the exact executed/retired words and per-variant engine result; bind JIT feature skips to that exact variant. Reject undeclared duplicate bindings except true declared aliases. Add negative validator fixtures for shifted versus extended register, immediate versus literal, and unreachable program words. Regenerate both page-size reports with all 983 IDs bound without mnemonic fallback. |
| Engines disagree on guest-scoped handling of unimplemented instructions | At block start, JIT mapped `Action::unsupported` to `illegal_instruction`, while interpreter and gadgets returned a fatal emulator event for a valid but unimplemented encoding. The shared contract tested only the interpreter's then-current behavior. The required policy became that an unsupported instruction fails the guest program without stopping the emulator. | Run `0xd5000000` through every available engine. Each must return the normal guest illegal-instruction event with zero retirement and the faulting PC preserved, without falling back to another engine. Then execute known-good guest work in the same emulator instance to prove it remains usable. Keep internal diagnostics capable of distinguishing an invalid encoding, unavailable virtual feature, and implementation gap without changing the guest-scoped outcome. |
| Same-handle embedded runs violate the public fail-fast contract | `darwin/emulator/src/emulator.cpp` takes the process-wide embedded-run mutex before `RunScope`. A second call on the same handle waits, then runs, although `uml/emulator.h` says concurrent same-handle runs fail. A queued different-handle embedded run also has no defined stop-aware admission behavior. | Deterministically block one embedded run and require a second call on the same handle to return `-UML_EMULATOR_RUNTIME_ERROR` before the first is released. Add a separate queued-handle stop test and define/correct cancellation semantics without weakening process-wide embedded serialization. |
| Software-MMU state is neither handle- nor run-isolated | `darwin/memory/mmu/address_space.cpp` owns a process singleton keyed only by numeric generation; kernel mappings collapse to generation zero, and run/handle retirement does not clear it. Concurrent native-backed handles and sequential embedded kernels can therefore collide or inherit mappings. | Two concurrent handles must map the same generation/address to distinct backings and read their own data. A new embedded run must start without previous user or generation-zero mappings. Cross-thread generation destroy/recreate must resolve only the new canonical state. Route the required context through the bridge without restoring dependency cycles. |
| Embedded ELF loading can overwrite host mappings and trusts unbounded metadata | `darwin/emulator/loader/elf_loader.cpp` uses destructive `MAP_FIXED`, omits `munmap()` on success and error paths, and does not fully bound dynamic entries, relocation tables, symbol/string indexes, destinations, or address arithmetic to loaded segments. | Pre-map a sentinel at the linked window and prove loading refuses without modifying it. Unmap every successful and failed load. Exercise malformed dynamic/relocation images under sanitizers and return status 126 without out-of-range access or arithmetic overflow. |
| Network-enabled installed static library is incomplete | `uml_emulator` references `uml_slirp_net_*` when networking is enabled, but `darwin/emulator/CMakeLists.txt` installs only `libuml_emulator.a`; static linking to `uml_network_provider` does not fold its objects into that archive. A fresh raw consumer fails with seven undefined provider symbols. Packaging currently masks this by building slices with networking off. | Either move default-provider ownership to the app and inject neutral callbacks, or export/package the provider and complete third-party closure. Add network-enabled raw-archive and install-tree consumers. Include a genuine C consumer and encode or document the C++ runtime link requirement. |
| Active context/provider registries are not strongly exception-safe | `ActiveContextScope` and `ActiveProviderScope` clear existing fallback flags before a potentially throwing `vector::push_back()`. Publication also precedes engine validation and argument/environment allocations, contrary to ADR 0002's stated guarantee. | Insert the new registry entry before mutating prior entries, or use a rollback-safe representation. Inject allocation and pre-entry failures while handle A has working unbound CPU/network callbacks; A must remain usable and prior registry state must be unchanged when publication fails. |
| Public and kernel-internal ABI identities are coupled and the bridge remains wider than Linux uses | Declaration fingerprinting combines installed `uml/emulator.h` with the internal host/kernel bridge. `darwin_uml_set_execution_engine`, `darwin_uml_get_execution_engine`, and `darwin_uml_jit_available` remain in the canonical bridge although current Linux imports none of them. | Split public API and kernel-bridge identities/histories. Remove unused engine controls from the synchronized bridge and use internal test seams. Verify the bridge against the kernel's actual undefined-symbol set; prove internal-only changes do not churn the public identity and public-only changes do not change the Linux replay tree. |

### P2 requirements (reported closed)

| Original finding | Closure requirement (satisfied) |
| --- | --- |
| An undersized SLiRP read removes the frame but leaves its readiness byte | Retain the frame for retry or consume frame and signal atomically. After the queue is empty, polling the IRQ fd must not remain readable. Add a short-buffer/retry/readiness unit test. |
| MMU C entry points can unwind C++ allocation exceptions through Linux C frames | Fault-inject backing, batch, generation, and mapping allocations. Every C entry point must return a defined error, balance batch/walker state, leak no VM mapping, and permit immediate retry. |
| Linux replay accepts topology not represented by the ten-patch manifest | Require the exact commit count, one nonempty patch ID per commit, expected subjects, and no merge commits. Add negative tests for empty, extra, reordered, and merge commits. Preserve exact final-tree verification as a separate invariant. |
| Next-version checking bypasses canonical replay policy | Make `check-next-version.sh` share manifest hashes/subjects/patch IDs, isolated Git identity, delta ownership, extraction decisions, and per-path next-version policy. Mechanically validate `extraction-decisions.tsv`; it is currently descriptive only. |
| Runtime aggregation still crosses directory scope as raw objects | Replace the `UML_RUNTIME_CORE_OBJECTS` `PARENT_SCOPE` export with target-owned composition. Either make `uml_runtime_core` a real consumed/installed boundary or remove the unused archive. Keep the narrow JIT test variant rather than restoring monolithic recompilation. |
| Test ownership still has conditional coupling | Make integration with tooling disabled and external-network tests enabled a valid configure: the VNC client is currently tooling-owned but integration-referenced. Move/guard the standalone GPU fixture according to its actual test owner. Keep the evidence reset fixture from being pulled into a strict unit-only selection. |
| Public/internal API documentation is inconsistent | Amend or supersede ADRs 0002/0003 to distinguish concurrent native handles from serialized embedded runs. Move the conditional JIT profiling declaration out of the installed public header or deliberately stabilize it. Correct the manifest's “read-only” description for the evidence validator, which writes `stable-id-bindings.tsv`. |
| GPU service file-descriptor ownership is not exception-safe | Replace raw socket-pair integers with RAII ownership and explicit release at transfer. Repeated forced startup failures must leave the process FD count unchanged. Keep host-only GPU fixtures out of production ownership. |
| Dependency/link provenance enforcement is denylist-based at the final link | Change the final-link audit to allow only build-tree objects, Apple SDK/toolchain inputs, and the verified private source-built prefix. Add an arbitrary ambient-path negative fixture. Keep the existing release-package, receipt, archive-member, and private-prefix checks. |
| LibVNCServer currently has a project-maintained post-release patch layer | Return the manifest to the pristine pinned upstream release, currently 0.9.15: use version `0.9.15`, patch fields `-`, and the canonical unmodified release-tree hash. Remove `dependencies/patches/libvncserver/0001-release-backports.patch` and its patch README, update the build description, rebuild the private prefix, and rerun VNC plus package consumers. Do not replace it with a moving branch snapshot. |

### P3 requirements (reported closed)

- Prune expired weak entries from the per-thread registration list and prove
  thousands of handle create/run/destroy cycles on one thread remain bounded.
- Make the network provider's implementation include directory private; expose
  only the neutral callback contract.
- Remove or explicitly reject the obsolete `DARWIN_LIBELF_INCLUDE` hook from
  the final Linux series and replace the stale package-manager-specific comment
  in the Darwin ELF compatibility header. The canonical build currently
  supplies the repository-owned header directly.
- Audit `.DS_Store` and other unconsumed non-rootfs artifacts. Rootfs payloads,
  images, maintenance tools, hashes, and locks are deferred and are not closure
  items in this refactor.

### Fourth-audit completion evidence

| Area | Final resolution and acceptance |
| --- | --- |
| Exact ARM64 evidence and engine policy | A checked-in 983-row exact catalog replaces mnemonic/form fallback. Evidence records exact entry instruction and per-engine executed/retired state; duplicates require a declared alias. Four negative fixtures reject shifted/extended confusion, immediate/literal confusion, unreachable program words, and undeclared aliases. Fresh 16 KiB and 4 KiB reports each validate 983/983 IDs with 713 JIT executions and 270 exact host-feature skips. All engines map `0xd5000000` to guest illegal-instruction with zero retirement and preserved PC, without fallback, then run known-good work in the same emulator. |
| Handles, MMU, loader, and registries | Same-handle embedded runs fail fast before process-wide admission; queued different-handle runs are stop-aware. Memory domains isolate concurrent handles and run retirement clears user and generation-zero state. The ELF loader refuses occupied fixed windows, validates all dynamic/relocation bounds, and unmaps every path; its combined fixture passes at the production link address and under targeted ASan+UBSan. Registry publication and all MMU C allocation boundaries have deterministic failure injection, rollback, leak checks, and immediate retry coverage. |
| Network and installed API | The default SLiRP provider moved to macOS app ownership and is injected through neutral callbacks, leaving `libuml_emulator.a` provider-complete in a networking-enabled configuration. Three platform slices link raw and installed archives from genuine C and C++ consumers. Short-buffer frame retry/readiness and provider publication failures are covered. Public API v4 (`0xd9a69804d3303044`) and kernel bridge v4 (`0xfe2d71a33e3d446e`) have independent histories and negative identity tests; Linux imports are checked against the narrowed bridge. |
| Composition and test ownership | Runtime closure is target-owned through `uml_runtime_core`; no raw object list crosses directory scope. Integration with tooling disabled and external workflows enabled configures and audits as 39 targets / 10 registrations, with GPU/VNC fixtures owned by integration. A strict unit-label selection does not select the tooling evidence reset. ADRs distinguish 16 KiB native concurrency from 4 KiB/embedded process-address-space serialization, and JIT profiling is internal-only. |
| GPU, link provenance, and dependencies | Socket pairs and server connections use RAII with explicit release; 64 repetitions at each of three forced startup failures leave `/dev/fd` unchanged. Final-link verification canonicalizes every object and permits only the build tree, Apple SDK/toolchain, or verified private prefix; an arbitrary ambient archive is rejected. LibVNCServer is pristine 0.9.15 with canonical tree `3b0f5deb99f726cc0cfb963b783fcc396017fde8e646240a9ed3b01c18077a35`; a fresh transactional prefix, loopback VNC smoke, and package consumers pass. |
| Linux replay and bounded cleanup | Manifest topology enforces ten exact nonempty stable patch IDs, subjects/order, and no merges, with empty/extra/reordered/merge negatives and independent exact-tree verification. The authenticated 6.18.41 release replays to `834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d` across the bounded 162-path delta. Next-version and extraction policy share the canonical verifier. `DARWIN_LIBELF_INCLUDE` and its stale package-manager comment are gone. Weak TLS registrations stay bounded through 3,000 cycles, network implementation includes are private, and root `.DS_Store` is removed and now rejected by the source audit. |
| iOS executable memory | Interpreter, gadgets, and MMU storage are data-only and never W+X or RW↔RX. Gadgets execute precompiled signed RX text. Device and simulator archive audits reject executable-memory publication primitives outside `jit_arena.cpp`; dynamic publication remains available only when the JIT engine is explicitly selected for entitled debugger/jailbreak use. |

At the end of the fourth audit this record stated that no repository-owned item
remained. The fifth and sixth audits successively superseded that status, and
the seventh-audit checklist now supersedes all three closure claims. Rootfs
management remains the explicitly
excluded testing redesign; the external campaigns named at the top remain
evidence prerequisites rather than source-refactor tasks.

## Third independent audit: historical closure record

This table records what the third audit accepted at the time. It is retained so
a new chat can understand the work already performed, but it is no longer the
authoritative closure checklist; the seventh-audit section above supersedes any
conflicting statement.

| Audit finding | Final resolution and acceptance |
| --- | --- |
| Same-handle run retirement | Run admission stays closed through execution-state retirement. Generation-scoped registrations and a deterministic retirement/admission hook prevent the previous run from erasing state acquired by the next. The lifecycle test passes normally, under targeted TSan, and in repeated execution. |
| Public multi-handle contract | Embedded-kernel runs are explicitly process-serialized because the fixed linked window and software MMU are process-wide. The installed header documents that narrower contract; a test runs two real embedded-kernel handles through the loader and proves serialization. Native callback-only handles retain documented independent concurrency. |
| Fresh integration builds | Guest helper compilation uses an explicit compiler selected independently of the CXX-only host graph. A generated-file verifier makes a missing helper a hard build failure. Fresh 16 KiB and 4 KiB integration configurations build their apps and audit their graphs. |
| Optional profiling configurations | Option-conditioned test-manifest rows and required registrations cover both JIT GPR-link modes. Fresh link-profile and pressure-profile configurations pass 2/2 and 1/1 respectively. |
| Complete architecture enforcement | The File API audit now rejects undeclared first-party targets, checks the reverse target map and object composition, traverses pure-interface cycles, validates semantic test owners, and runs in product-only builds. Unit/tooling, both integration page sizes, macOS product, iOS device, iOS simulator, profile, and TSan graphs pass. |
| Dependency verification invariant | Manifest, policy, receipt, payload, and verifier inputs trigger build-time/pre-link revalidation. Every archive member is checked for arm64, macOS platform, and minimum OS. Direct device dependencies and the final production link map are audited; the fresh private prefix and both integration apps pass. |
| Linux final-state series | The signed Linux 6.18.41 package now replays 10 final-state topic patches to exact tree `8adb106de7c19ed9e6c83f622a48e54629053699`. The two pending-upstream fixes are isolated topics. Policy owner/disposition, origin-patch membership, allowed next version, 162 changed files, and 13 outside-port files are mechanically checked. Authenticated replay, both kernel builds, a 16 KiB bounded boot set, and a 4 KiB lightweight boot pass. |
| Network queue reuse | SLiRP receive/transmit rings reset under their locks at stop. A two-run stale-frame/IRQ test passes. Active fallback is context-scoped and remains disabled after overlapping-run ambiguity, eliminating the provider-retirement regression found by repeated API testing. |
| Public error codes | The installed API states that functions return zero or the negation of the positive enum value. An external consumer checks the negative result, and the declaration change is bound by ABI v3. |
| Target-owned tests and suites | Only the JIT executor is rebuilt for its test macros; runtime guest selection is reused through module-owned objects. Tooling-only, unit-only, integration-only, sanitizer, and composed presets are explicit. Manifest-owner validation resolves the registered command/source rather than accepting any existing path. |
| Slim ABI and extraction decisions | Host-internal concrete engine entry points and the dead VM queries were removed. ABI v3 binds the synchronized declarations. The remaining `arch/um/drivers` and `scripts` paths have explicit keep/upstream ownership and extraction rationale in the hardened delta policy. |
| Direct device/dependency ownership | GPU renderer/display frameworks and the network provider's concrete library are linked by their direct owners. Imported targets match actual production use; retained auxiliary archives are enumerated and rejected if undocumented. |
| Audit, debris, and documentation | Cache/debris checks cover kernel, dependencies, Darwin, CMake, scripts, tests, and tools. Obsolete artifacts/clones/backups and dead CMake branches were removed. Documentation records that instruction validation rewrites only `stable-id-bindings.tsv`, and the architecture graph includes the app-to-device edge. |

The third-audit checks verified exact Linux tree
`8adb106de7c19ed9e6c83f622a48e54629053699`, ABI v3, 10 patch identities,
162 changed and 13 outside-port files, the schema-2 dependency prefix, all
then-selected Apple product graphs and network-disabled package consumers, and
the immutable rootfs contract. The fourth audit's network-enabled consumer
failure and replay-topology gaps qualify those historical results.

## Decisions applied during closure

- The 800/600/300-line guidelines are advisory, not hard limits. Every
  project-owned production, test/support, and CMake source is reported, while
  cohesive or order-sensitive files may remain larger without an exception.
  Optional context records an owner, factual size at review time, rationale,
  and date in
  [`docs/source-size-reviews.tsv`](source-size-reviews.tsv).
- Interpreter integration is deliberately lightweight: shell arithmetic and
  CPU/HWCAP inspection. Heavy Node.js, Go, browser, and benchmark workloads are
  not run through the interpreter.
- Interpreter state, gadget programs, and software-MMU storage are data-only.
  They never use W+X memory and never perform RW-to-RX or RX-to-RW transitions.
  Gadget code is precompiled signed RX text. Dynamic executable publication is
  confined to the explicitly selected JIT path for entitled debugger or
  jailbreak use; a non-JIT engine cannot reach it.
- Reproducibility means pinned official release archives and signatures,
  patch content and order, ABI identity, and verified prepared source trees.
  Timestamps, committer dates, build user/host, compiler notes, generated build
  identities, package layouts, and output hashes are dynamic observations and
  need not be byte-for-byte deterministic.
- When behavior changed unexpectedly, the focused working-tree diff was
  compared with the pre-refactor implementation. That recovered the systemd
  overlay contract, WFET/WFIT instruction behavior, SLiRP context ownership,
  and the correct atomic-alias test semantics.

## Earlier closure record

This section records the broad implementation state established before the
third audit. It is historical; the seventh-audit checklist is authoritative where
details differ.

The following table records the earlier closure snapshot from the
2026-08-03 tree.

| # | Resolution |
| ---: | --- |
| 1 | Translated execution now owns `begin_execution()` with an RAII guard; throwing-executor tests prove exactly one cleanup and successful re-entry. |
| 2 | Interpreter address-space exceptions are caught at the `noexcept` boundary and translated to `ExitReason::fatal`; throwing-memory tests cover it. |
| 3 | Architectural-state materialization and engine availability live behind neutral CPU contracts; runtime no longer includes concrete JIT, gadget, or CPU-detail helpers. |
| 4 | Host-only parsers, validators, protocols, launchers, fixtures, and inventories now live under tooling; unit owns native behavior and the instruction inventory; integration registrations require a guest/rootfs. |
| 5 | All 983 stable instruction IDs bind to registered CTests and executable encodings, with interpreter/gadget evidence and either JIT execution or an explicit host-feature skip for every row. |
| 6 | External-network, VNC, rendered-WebGL, and native-4K conditional rows are mandatory whenever their owning options/suites are enabled; the architecture audit checks the manifest. |
| 7 | CPU/MMU sources were split by responsibility. The order-sensitive gadget decoder, JIT coordinator, assembly banks, and other cohesive logical units remain intact where splitting would harm clarity, generated-code stability, or measured layout. |
| 8 | The File API audit normalizes include paths, verifies actual source ownership and object composition, covers root targets, and rejects forbidden edges/cycles/publication primitives. The former umbrella bridge is split by public contract. |
| 9 | Gadget, interpreter, JIT, and factory targets declare their concrete direct dependencies. Isolated-link tests prevent aggregation from masking missing edges. |
| 10 | Host capabilities use a neutral platform contract; Apple platform code no longer imports CPU-owned types, and the audit rejects that reverse dependency. |
| 11 | Canonical build/check targets run architecture, durable test-manifest, rename/removal-ledger, advisory source-size reporting, and executable-memory audits. Removing both a test and its current row is detected against the preserved inventory. |
| 12 | Engine choice, stop state, network provider/callbacks, and translated caches are owned by each public emulator handle. Run admission/stop is synchronized, host-thread state retires safely, and deterministic race/churn/reuse tests pass. Lifetime and stop semantics are recorded in ADRs 0002 and 0003. |
| 13 | Fresh 16/4 KiB normal, sanitizer, integration-fast, architecture, and Apple matrices were regenerated under `.build/results/refactor/final-20260803`; no stale JUnit is closure evidence. |
| 14 | Configure and build verification require a clean canonical Linux source tree with the expected exact tree and audited delta, and have no-fetch operation for already prepared sources. |
| 15 | A fresh authenticated `linux-6.18.41.tar.xz` release package replays 10 hash-checked final-state topic patches and verifies exact final tree `8adb106de7c19ed9e6c83f622a48e54629053699`. The 13 outside-port changes are classified and kept within audited ceilings. Dynamic local replay commit/build metadata is recorded only as observation. |
| 16 | Production dependencies are prepared from pinned official release packages with authenticated archive, patch, and final-tree identities. The private-prefix receipt binds that manifest and the build policy; verification rejects an altered or extra payload, non-archive or shared-library content, the wrong architecture or deployment target, and ambient/staging paths. |
| 17 | `UMLEmulator.xcframework` was the sole supported Apple aggregate name in this snapshot. Its macOS, iOS-device, and iOS-simulator arm64 archives and external consumers were rebuilt together; those artifacts are dated and the older local aggregate is not evidence. |
| 18 | One generated ABI identity is checked across normalized canonical/public declarations, declaration history, Linux patches/replay, package headers, configure, and external-consumer builds; declaration drift requires a derived fingerprint and explicit version/history update. |
| 19 | Empty residual `darwin/loader` and `darwin/probes` directories are gone, and the source audit now rejects empty residual architecture directories. |
| 20 | Standalone Python entry points suppress source-adjacent bytecode caches; the source audit rejects regenerated cache/debris directories. |
| 21 | Overlay, maintenance, GPU, VNC, and rendered-workflow launchers use the canonical `.build/presets/macos-integration` defaults. |
| 22 | Architecture documentation names `assembly_offsets.h`; documentation link/path validation prevents stale moved-file references. |
| 23 | Production, generated/declarative, test-support, and CMake sources are all reported under advisory guidelines. Optional review records retain context for cohesive units without turning reviewed size into a future ceiling. |

## Previous audit implementation record

The third audit extended the lifecycle/isolation, target-composition, and
audit-coverage work in this table. It remains a record of implemented fixes;
the fourth audit identifies where their acceptance was incomplete.

| # | Final resolution |
| ---: | --- |
| 1 | `Context` now serializes run admission, stop requests, and run retirement. A deterministic admission-boundary hook proves that a stop issued after admission cannot be erased and that an idle stop is not inherited by a later run. |
| 2 | Per-thread execution state lives in a shared store with generation-tagged weak TLS registrations. Host-thread exit and `end_run()` retire state without allowing stale cleanup to erase a reused registration; churn tests enter the engines and verify bounded cleanup and same-thread reuse. |
| 3 | Official, versioned dependency release packages are authenticated by archive/patch/final-tree identity and built into the verified private arm64 static prefix. CMake and Darwin kernel host tools no longer consume ambient third-party libraries. Real tool versions, timestamps, package layouts, and binary hashes remain dynamic observations. |
| 4 | `kernel/abi/verify-declarations.py` comment-strips and token-normalizes every canonical host/kernel header plus installed `uml/emulator.h`. Its SHA-256-derived fingerprint and consecutive history make a declaration/layout change fail unless the ABI version/history is explicitly advanced; negative tests cover comment-only and declaration changes. |
| 5 | `architecture-audit` consumes the File API reply from ordinary configuration and never reconfigures its own build tree. The stale CTest-registration comment is corrected. |
| 6 | The public implementation copies environment strings, arguments, the configuration, callback table, and callback values before use. The installed header documents ownership plus run/stop/destroy concurrency; mutation/lifetime tests exercise the contract. |
| 7 | CPU/memory composition is target-owned, including a narrow JIT test-support object variant. Raw exported source lists and monolithic recompilation are gone. Unreachable raw stop/static-dispatch exports, host-internal engine entries, dead VM queries, and the unused `clear_stop` seam were removed through ABI v3. |
| 8 | Physical ownership now matches the test taxonomy: native behavior and the inventory are under unit, host parsers/validators/protocols/fixtures are under tooling, and only guest/rootfs execution is under integration. Unit remains Python-free and guest tests retain the immutable-rootfs wrapper. |
| 9 | The five confirmed unreferenced test artifacts were removed; the expanded source audit inventories the supported production, test/support, CMake, and declarative locations. |
| 10 | `arm64-instruction-evidence` is non-destructive to producer evidence when selected alone. Its reset/producer ordering remains in the canonical aggregate, while a standalone selection lists and runs exactly the validator without erasing producer evidence; it still rewrites `stable-id-bindings.tsv`. |

Source-size policy remains advisory. The reporter applies the documented
800/600/300 guidelines, emits a stable TSV, and never rejects a file for line
count; only malformed, duplicate, or stale optional review metadata is an
architecture error. Cohesive files may remain over a guideline.

## Initial fourth-audit reproductions and preserved earlier evidence

The following table is the historical reproduction record that opened the
fourth-audit findings. Its false-form binding and network-link failures are
intentionally retained so the fixes can be audited against the original
symptoms; they are not descriptions of the current tree. Their dated acceptance
results are in the fourth-audit completion table above; the current checklist
is the seventh-audit section at the top of this file.

These initial checks completed without booting a guest or rerunning Geekbench:

| Check | Result | Evidence |
| --- | --- | --- |
| Fresh unit-only configure and architecture audit | passed; 550 targets / 561 registrations | `.build/audit-round4-unit-only` |
| Dated unit selection with required evidence reset fixture | 562/562 passed | `.build/audit-round3-finish-unit` |
| Standalone instruction-evidence validator | 1/1 passed, but produced the false-form bindings documented above | `.build/audit-round3-finish-unit/instruction-evidence/stable-id-bindings.tsv` |
| Inventory structure | 983 verified rows: 516 Base / 467 FP&SIMD | `validate_a64_instruction_inventory.py` |
| Dependency manifest and ABI synchronization | passed | `dependencies/prepare.sh --validate-manifest-only`; `kernel/abi/sync.sh --verify` |
| Network-enabled raw package consumer | failed as expected with seven unresolved `uml_slirp_net_*` symbols | `.build/audit-round4-macos-network-consumer`; `/private/tmp/uml-audit-round4-macos-network-consumer.log` |
| Network-disabled package consumers and fresh iOS graphs | passed | fourth-audit package/build configurations |

That audit's source-size report covered 1,010 files: 53 above an advisory
guideline, 42 without optional review context, and two changed since review.
Those counts remain informational.

Preserved third-audit evidence follows:

| Gate | Result | Evidence |
| --- | --- | --- |
| Dated unit selection | 562/562 passed | `.build/audit-round3-finish-unit` |
| Repeated API/context race coverage | 50/50 each passed | that audit's unit binaries |
| Tooling/provenance after evidence production | 18/18 passed | reset fixture deliberately excluded |
| Targeted TSan | 1/1 context lifecycle passed | `.build/audit-round3-final-tsan` |
| Optional JIT link profile | 2/2 passed | `.build/audit-round3-final-profile-link` |
| Optional JIT pressure profile | 1/1 passed | `.build/audit-round3-final-profile-pressure` |
| Bounded 16 KiB guest acceptance | 5/5 passed | systemd, interpreter, gadgets, JIT, SLiRP |
| Bounded 4 KiB guest acceptance | 1/1 passed | lightweight interpreter boot |
| Architecture, unit/tooling | 551 targets / 580 registrations | fresh File API graph |
| Architecture, integration 16 KiB | 43 targets / 27 registrations | fresh File API graph |
| Architecture, integration 4 KiB | 44 targets / 27 registrations | fresh File API graph |
| Architecture, macOS/iOS products | 21 targets in each of 3 graphs | macOS, iOS device, iOS simulator |
| Signed Linux replay | archive/signature, 10 topics, final tree, and 162/13 policy passed | retained authenticated replay |
| Linux builds | 16 KiB and 4 KiB passed | Homebrew LLVM 22; dynamic build metadata accepted |
| Dependency prefix and production links | passed | fresh schema-2 prefix plus both integration apps |
| Apple aggregate/package | 3 arm64 slices, 59 members each, all external consumers linked | `.build/audit-round3-final-package` |

The audited architecture graphs had no forbidden dependency edge or cycle. The
instruction validator lists all 983 stable rows (516 Base and 467 FP&SIMD) and
passes without erasing producer TSVs; it refreshes `stable-id-bindings.tsv`.
The fourth audit proved that this passing result was not evidence that each row
or variant executed the correct form on every applicable engine. Later audits
reported exact evidence, but the seventh audit found it still records only one
selected case per stable entry and therefore does not supply per-variant proof.

The preserved 16 KiB integration selection used only lightweight shell arithmetic
and CPU/HWCAP inspection in the interpreter. The 4 KiB acceptance repeated that
bounded interpreter smoke. Heavy Node.js, Go, browser, and benchmark workloads
were not run through the interpreter, and the pressure registrations were not
selected. Gadget and JIT smokes remain separate engine tests.

The earlier broad matrices remain preserved: normal unit/tooling 580/580 at
16 KiB and 581/581 at 4 KiB, 562/562 ASan+UBSan, 5/5 targeted TSan, and fast
guest integration 24/24 at each page size. They are dated regression evidence,
not substitutes for the fourth-audit acceptance tests above.

Apple archive and package hashes are intentionally not reproducibility
identities. The verified slice platforms, architectures, public headers, member
composition, executable-memory policy, and external links are the acceptance
invariants.

Historically, the rootfs finished mode `0444`, size 8,589,934,592 bytes, SHA-256
`501770dcce8223a3b7b41c475413ccb5f733291e190dcb23964890b50dafc0e5`.
This is retained as evidence, not as a current refactor gate.

## Linux and dependency provenance (historical)

The Linux series starts from the signed official `linux-6.18.41.tar.xz`
release package (SHA-256
`17fc72f0f8d4a8a8633a5d20085f5d9c5a5ec51ee896a0b7ae1ec25da31273ea`),
authenticated with the detached signature and pinned full signer fingerprint
`647F28654894E3BD457199BE38DBBDC86092693E`. Its packaged tree is
`3098cc6dba43578488a8f7f486797af8046ce8b8`; the official archive intentionally
export-ignores `Documentation/.renames.txt`, the sole difference from upstream
tag tree `9b4299714b8020ae57ca8c6310d747c2299aea0f` at tag commit
`2fe596715f840d053aed5cee5455f701bdcd2b50`. The 11 final-state topics in
`kernel/patches/series/manifest.tsv` reproduce final tree
`834e5c5faa8fc4638c9fdcbd40a1f29f5c7c2d5d`. Local import/replay commit IDs and
their real creation times are observations, not canonical identities. The
Darwin host-service topic has stable ID
`14c6b921eac5574938073b9aca3006fc5f9cac14` and SHA-256
`589f1c85669095f43f4a4bf9d6e1d71f6b6923ec0011b65e0711cbf1745b22f0`.
The independent DMA-mask topic has stable ID
`4293e280ee408165ae454f97819e652e00906581` and SHA-256
`b9f1494ad577cbc2bbe62f34daaac27e53f86b9d03a196fbed928be0872aebce`.

Fresh replay downloads only the pinned release archive and signature from
kernel.org, verifies their SHA-256 identities and exact OpenPGP signer,
verifies the packaged tree, applies the manifest, then verifies the exact
final tree and delta. Its isolated GnuPG home uses a short securely-created
temporary path so deep case-sensitive replay roots cannot exceed the agent
socket limit. Prepared-source verification is offline. The opt-in next-version
conflict audit alone uses Git because arbitrary development refs do not have
release packages. Kernel and external dependency records keep dynamically
generated build metadata as observations; verification is anchored to signed
source/patch content and the generated ABI identity.

The installed API and kernel bridge have independent declaration identities.
Public API v4 is fingerprint `0xd9a69804d3303044`, declaration SHA-256
`d9a69804d33030448b780d58c3f553c0e12e4f1e19ab37f89d2719b8b93115f1`;
kernel bridge v4 is fingerprint `0xfe2d71a33e3d446e`, declaration SHA-256
`fe2d71a33e3d446ec929bad4ef91d8f7b59fa2c1718bbd3d137cb24627ba175b`.
Their separate consecutive histories and negative tests require an explicit
identity advance for a declaration or layout change in the affected boundary.

## Dated Apple executable-memory policy evidence

The device and simulator archives built for the sixth audit contained executable-publication
references only in `cpu/arm64/jit/jit_arena.cpp`. Neither archive has
`mprotect`, VM-protect, JIT write-protect, or instruction-cache publication
references outside the JIT arena; production archives contain no
`jit::testing` support. This preserves JIT for explicitly entitled use without
making RW/RX transitions part of interpreter, gadgets, the software MMU, or
the normal sandbox-compatible iOS path. These archives predate the final source
split and must be rebuilt before they can be described as current artifacts.

## Preserved performance result

The fresh source-built-dependency 16 KiB gadget Geekbench result is
**80 single / 506 multi**
(result). It used the
Release executable with SHA-256
`949a6ef31838e87b3d894dda96c43592a4878a2ac98f79c63dba8b6b7b700af6`, 10
guest cores, 8 GiB, systemd, network access, and the immutable lower image
under tmpfs OverlayFS. Relative to the immediately preceding 77/508 run, the
aggregate deltas are **+3.90% / -0.39%**. Relative to the earlier G79 79/506
point they are **+1.27% / 0.00%**. This is the best preserved single-core
aggregate, with multi-core effectively flat; there is no broad gadget
regression.

The earlier single-core PDF anomaly resolved on the clean repeat: 63 rose to
86 (+36.51%), while multi-core PDF rose from 535 to 547. Single/multi compiler
scores changed from 48/392 to 51/385. The largest individual multi-core decline
was HTML5 Browser at -4.58%; it is outweighed at aggregate level and is not a
reason to reject the result under the workload-sensitive comparison policy.

The console is
`.build/results/refactor/audit-2026-08-03-release-deps/geekbench-gadgets-16k-console.log`,
SHA-256
`31df1439d98fe6b4773b9a6c05c00f85078e92e2cea701a44b844fe91bf7b86c`.
Repeated matched gadget/JIT campaigns remain external acceptance, as below.

## External acceptance not claimed (historical)

These require inputs outside the repository and are intentionally not local
pass claims:

- full per-engine LTP with a suitable guest image containing the LTP payload;
- physical sandboxed and entitled iOS runtime campaigns;
- opt-in public-network, interactive KDE/VNC, rendered WebGL, and browser
  campaigns; and
- repeated controlled gadget/JIT performance medians.

Their prerequisites and commands are documented in
[`docs/testing.md`](testing.md). Missing external inputs do not leave a
repository item open by themselves, but they also do not close the repository-
owned seventh-audit checklist above. Rootfs provisioning, identity, locking,
and maintenance are intentionally excluded until the testing-image redesign.
