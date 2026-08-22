# Maximum guest GL and GLES support through Virgl on Apple hosts

Written 2026-08-20. This is an implementation plan, not a capability or
conformance claim. It incorporates the measured results in
[GPU_BACKEND_FEASIBILITY.md](GPU_BACKEND_FEASIBILITY.md), the current Virgl
implementation in
[VIRTIO_GPU_VIRGL_IMPLEMENTATION_PLAN.md](VIRTIO_GPU_VIRGL_IMPLEMENTATION_PLAN.md),
and the additional requirement that the exact guest userspace graphics
libraries in the current authenticated `rootfs.erofs` remain unchanged. That
restriction does not apply to the guest kernel: this project owns its Linux
build and maintained patch series, including the `virtio_gpu` driver and UML
transport, and may change them when required.

Dependency-management assumptions were revised after the 2026-08-20
centralization under `dependencies/`; paths and workflows in this document
refer to that post-refactor ownership model.

## Implementation checkpoint (2026-08-22)

Phase 1 is implemented and verified as one atomic transition. All three Apple
products now use the central `apple-gpu` UTM ANGLE/Metal matrix, fixed common
provider, native Metal-texture contract, common presentation owner, and exact
platform prefix. CGL, OpenGL.framework, the desktop graphics dependency
family, layout-changing provider macro, and independently maintained
`ios-gpu` recipe are absent. The normal and ASan/UBSan GPU workflows, real
dependency verifiers, macOS integration link/closure, all four Kernarium
runtime builds, and XCFramework packaging pass.

After this plan was written, the user explicitly authorized one narrow
derivative-rootfs exception to its no-image-change rules. The original
`rootfs.erofs` and provisioning manifest remain byte-for-byte unchanged. An
8 GiB ext4 clone at `.build/rootfs/rootfs-gl-gles.ext4` instead excludes the
checked unrelated benchmark, language-runtime, Vulkan runtime, and Vulkan CTS
paths listed in
`tests/integration/gpu/vk_gl_cts_derivative_rootfs.json`. The one restored
Vulkan-named subtree is the 12,895,859-byte shared build framework that
upstream `tcutil-platform` unconditionally needs through static `vkutil`; no
Vulkan module, data, must-pass, or runtime payload was restored. During its
bounded maintenance epoch the emulator built only `deqp-gles2`, `deqp-gles3`,
and `glcts` in Release mode using JIT, ten guest vCPUs, and build parallelism
ten, with the derivative root block read-only and a separate writable scratch
disk. The clean build and link audit found no selected Vulkan, Vulkan SC, or
Shaderc runtime input. Installation replaced only the three audited
executables, preserved all authenticated base resource trees verbatim, and
wrote the in-guest build/install receipt. After final shutdown and read-only
filesystem checking, the image was sealed mode 0444 at SHA-256
`f3ebadb579988892f4e33524b3101e82437199f876a7463aae092251fccec57a`;
every subsequent boot attaches it block-read-only and supplies writable state
through OverlayFS. The checked receipt authenticates that exact lifecycle and
is not permission to mutate or reprovision either image.

Phase 0 remains intentionally incomplete at its conformance-evidence boundary.
The original EROFS binaries still have no recovered authoritative source/build
receipt; the new receipt proves only the checked derivative build. The initial
all-package display-backed export aborts at `dEQP-GL45-ES3` with
`EGL_BAD_MATCH` after eight modules and 148,476 rows. A native arm64 Debian 13
VM running the byte-identical payload over direct virgl reproduced the exact
status, row count, and partial digest. That VM is diagnostic evidence only: it
is not a conformance oracle and cannot excuse an emulator failure. Released
source inspection independently shows that the
package deliberately requests a real core GL 4.5 context even outside execute
mode. It is an ES-on-desktop-GL compatibility package, not a GLES 2/GLES 3
profile requirement; rebuilding or modifying CTS is neither necessary nor
valid. Compliance must be established by the complete official matrices in
this emulator.

The qualification inventory is now explicitly scoped to the lower shared GLES
baseline. Unmodified `glcts` exports `KHR-GLES2.*` and `KHR-GLES3.*` in two
separate status-zero runs, and the runner requires exactly those modules with
no cross-module rows. The native VM diagnostic produced 473 and 6,499
hierarchy cases, but those counts are not acceptance evidence. The real 16 KiB
common ANGLE/Metal emulator independently passed the scoped capture
with JIT, ten vCPUs, the sealed derivative mounted read-only below OverlayFS,
status zero, clean shutdown, 6,972 total cases, and SHA-256
`3ec390ed2d36f0fd8e32a02718e19c5750cf9381eab002aac9b71f848ebc6162`.
The official GLES 2/GLES 3 must-pass leaves independently remain frozen at 473
and 6,498 cases. The scoped caselist gate is complete; Phase 0 stays open for
the complete official matrices and remaining qualification gates, and the
original EROFS receipt gap remains recorded.

Official GLES matrix expansion is now implemented fail-closed. The repository
owns the byte-identical released `mustpass.xml` at SHA-256
`9f466f19a26120149bab06e70596c9fff57b61d767cfdbadc7ba13934ce2ea96`
and validates every typed field before use. A disposable Release build of the
same 1.4.5.5 `cts-runner` on arm64 macOS served only as an argument-generation
oracle. Because macOS cannot enumerate EGL configs, its build-only fixture
replaced platform config discovery with three EGL configs exercising the
runner's window, pbuffer, and pixmap priority; the released XML generator and
expansion code were unchanged. The checked oracle records the original and
fixture source identities, generated header, binary, summary identities,
first/other-config roles, and exact emitted argument order. Its expected
status 1 reflects summary mode's zero executed sessions and is not a CTS
result. Qualification shards re-evaluate the repository expansion against
that evidence before booting. The released runner also explicitly treats a
complete session with zero failed cases as successful when individual results
are `Pass`, `NotSupported`, `QualityWarning`, or `CompatibilityWarning`. The
driver reproduces that exact unwaived policy, while continuing to reject
`Waiver`, `Fail`, resource/internal errors, device loss, crashes, timeouts,
missing cases, and truncated sessions.

At the user's direction, repeated macOS CTS boots now use headless VNC so they
do not open an AppKit window; native direct presentation remains required for
the separate performance qualification. On revision `79cf9184`, two 16 KiB
VNC boots with JIT, ten vCPUs, the sealed derivative lower, and disposable
OverlayFS state passed exactly `CTS-Configs.es2` and `CTS-Configs.es3` and shut
down cleanly. Both APIs initially reported 25 ordered non-MSAA EGL configs:
IDs 1-5, 16-20, 31-35, 46-50, and 61-65, each supporting window, pixmap, and
pbuffer surfaces, with canonical digest
`e7347c0a04099e4b45d777c6a141c90a7b2c1ca8047817fb280cc89af60ced56`.
Later qualification proved that this inventory was not truthful: the GLES
Virgl format table advertised normalized 32-bit depth while actually backing
it with 24-bit depth. The 25-config inventory, its QPA digests, and its
174,275-case cartesian product remain diagnostic history and are not current
qualification evidence. The eight-case GLES 2 config-ID-1 preflight likewise
remains valid only as an end-to-end automation check.

After removing that false format, the Release product exposes 20 truthful
non-MSAA configurations: IDs 1-4, 13-16, 25-28, 37-40, and 49-52. Every one
supports window, pixmap, and pbuffer surfaces, and the canonical inventory
digest is
`843544a7aef987426f74851a8802447a2428bde9ff9a5d893e1c2073ffbb2607`.
The passing inventory run is
`.build/vk-gl-cts/qualification/gles3-z32-release-config-inventory/20260822-060342-60650-16k-config-inventory-gles3`.

The macOS matrix now has a resumable fail-closed orchestrator. It fixes every
child to VNC, JIT, ten vCPUs, 8 GiB, and the sealed ext4 lower; freezes the Git,
emulator, CTS, XML, oracle, config-inventory, receipt, timeout, and shard
identities; and permits no retry to erase a failed or incomplete qualification
attempt. Final verification reparses every retained QPA and console, checks
artifact hashes and the released unwaived status policy, reconstructs the
ordered leaf across fractions for every EGL config, and requires the complete
cartesian product. Tooling and architecture registration pass. No complete
matrix result is claimed by this automation checkpoint.

The first complete GLES 2 attempt verified nine configurations before the
unacknowledged `journal+console` artifact stream stopped during the config-ID
18 QPA export. The CTS process itself had completed all 473 cases for that
configuration with 438 `Pass`, 35 `NotSupported`, and no failure, but the
interrupted campaign directory is permanently disqualified. The image-owned
serial getty on `/dev/hvc0` also made that device unsuitable for a private
protocol, and the launcher does not mask or replace it. Instead, both mainline
page profiles now build `CONFIG_VIRTIO_CONSOLE=y`, and the outer runtime adds a
standards-based multiport Virtio serial attachment while retaining the existing
single-port Android console. `--virtio-serial-socket` publishes the named port
`org.uml.host-serial.0` at
`/dev/virtio-ports/org.uml.host-serial.0`; the host implements the device-ready,
port-add, port-ready, port-name, port-open, control-queue, and asynchronous
wakeup contracts. CTS sends each bounded frame through that raw port and waits
for an exact host acknowledgement before sending the next. Full LLVM 22 builds
of the 16 KiB and 4 KiB profiles passed and published local accepted artifacts
`mainline-6.18-{16k,4k}-20260821-virtio-console-1` with unchanged release,
source tree, patch manifest, and bridge ABI v8. A fresh VNC/JIT/ten-vCPU config
ID 18 preflight at
`.build/vk-gl-cts-virtio-serial-preflight-2/20260821-231607-15653-16k-matrix-gles2-cfg07-000of001`
then passed the same 473 cases, transferred the complete 545,587-byte QPA in 27
acknowledged frames, and shut down cleanly. Its QPA SHA-256 is
`b81e1be7fc1365350b834e7f786030afbb98befdc85c323b9d761d91ca5be534`
and its transport transcript SHA-256 is
`9456708701ffd63a929c07f17d79f7696f762c7beec71a5967cfcdcc81c9cfc5`.
This closes the macOS matrix artifact-transport defect, not either complete
matrix or any conformance claim.

A clean isolated GLES 2 campaign at
`.build/vk-gl-cts/matrix/gles2-16k-ece975c5-isolated` subsequently completed
all 25 EGL configurations and all 11,825 required executions: 10,950 `Pass`,
875 `NotSupported`, and no disallowed result. Every configuration completed
the exact 473-case leaf through VNC with JIT, ten vCPUs, 8 GiB, acknowledged
Virtio serial artifacts, clean shutdown, and the sealed ext4 lower unchanged.
This completed the matrix for the then-advertised inventory. Because the
normalized-32-bit depth advertisement was later removed, it does not qualify
the current 20-config product; a current-revision GLES 2 matrix was required.

That current-revision rerun is now complete at
`.build/vk-gl-cts/matrix/gles2-16k-e741cbad-z32-truthful`. Its matching Release
inventory exposes the same 20 configuration IDs and canonical digest as GLES
3. The exact 20 x 473 product contains 9,460 executions: 8,760 `Pass`, 700
Khronos-permitted `NotSupported`, and no failure, warning, waiver, missing
case, incomplete shard, dirty shutdown, or retry. Independent verify-only
reconstruction reported
`Verified 9460 gles2 case executions in 20 immutable shards`. Every guest used
VNC, JIT, ten vCPUs, 8 GiB, acknowledged Virtio serial artifacts, and the
sealed ext4 lower below disposable OverlayFS state.

The first GLES 3 campaign stopped at exact official fraction `359/6498`, case
`KHR-GLES3.shaders.loops.for_constant_iterations.conditional_continue_vertex`.
The generated Virgl shader carried the integer loop bound 3 through
`uintBitsToFloat`; ANGLE's Metal prelude implemented boolean `mix` with
arithmetic interpolation, which flushed that selected subnormal bit pattern to
zero and made the lowered loop nonterminating until the 60-second Virgl fence
timeout. An isolated rebuild changed that helper to component-wise
`metal::select`; the same case then passed in 465,116 microseconds at
`.build/vk-gl-cts/focused/gles3-angle-select-fix/20260822-010050-53009-16k-matrix-gles3-cfg00-359of6498`.
UTM WebKit topic `38fc4c80bc` adds the correction and a focused MSL translator
regression. The temporary shader instrumentation was removed, and the sealed
root remained mode 0444 at its exact authenticated hash.

Subsequent immutable shards exposed four more common-path defects. Managed
Virgl topics `42b8c00` and `a32f188` use ES 3.0 multisample renderbuffers when
multisample textures are unavailable and map representable Gallium stream-out
layouts to GLES separate transform-feedback attributes. Managed WebKit topics
`a25e51c` and `45a2a7b` count transform-feedback query primitives per draw and
disable Metal fast math only for shaders that emit or classify non-finite
values. Focused VNC/JIT/ten-vCPU emulator reruns passed the implicated
official cases; the complete matrix below supplies the required full-campaign
closure.

The regenerated common dependency exposed a second qualification defect. The
then-advertised normalized 32-bit depth configurations failed
`multisampled_to_singlesampled_blit_depth_config_test` and `scissor_blit`.
Direct ANGLE and instrumented Virgl probes showed complete source and
destination FBOs and a successful host `glBlitFramebuffer`; guest Mesa rejected
the operation first with `GL_INVALID_OPERATION` for a depth-format mismatch.
Virgl's GLES table had advertised `VIRGL_FORMAT_Z32_UNORM` while mapping it to
`GL_DEPTH_COMPONENT24`, whereas CTS correctly treats a 32-bit default depth
buffer as floating point. Managed Virgl topic `3a287ef` and outer checkpoint
`e741cbad` stop advertising only that emulated GLES format. Genuine
`Z32_FLOAT` remains available, and the native desktop `Z32_UNORM` path is
unchanged.

The clean detached 16 KiB campaign at
`.build/vk-gl-cts/matrix/gles3-16k-e741cbad-z32-truthful` then completed the
current 20-configuration matrix through VNC with JIT, ten vCPUs, 8 GiB, the
sealed read-only ext4 lower, disposable OverlayFS state, and acknowledged
Virtio serial artifacts. Its exact 20 x 6,498 product contains 129,960
executions: 125,270 `Pass`, 4,690 Khronos-permitted `NotSupported`, and no
failure, warning, waiver, missing case, incomplete shard, dirty shutdown, or
retry. A separate verify-only pass reparsed every QPA and console, rechecked
all artifact and frozen-input hashes, reconstructed every ordered leaf, and
reported `Verified 129960 gles3 case executions in 20 immutable shards`. The
sealed image remained mode 0444 at SHA-256
`f3ebadb579988892f4e33524b3101e82437199f876a7463aae092251fccec57a`.
The current macOS Apple-GPU prefix was then rebuilt transactionally at its
canonical path and passed its read-only verifier. `check-gpu` and
`check-gpu-asan-ubsan` each passed the architecture audit, all eight selected
tests, and the VNC smoke test; the matrix-orchestrator unit test also passed.
Together with the current GLES 2 result above, this completes both current
macOS 16 KiB official GLES matrices. It does not substitute for the Simulator
and connected-iPad matrices.

The fixed signed-app authority now splits each 6,498-case GLES 3 configuration
into immutable 1,625/1,625/1,624/1,624-case processes, fixes Simulator memory
at 4,096 MiB, and starts physical-device campaigns at 2,048 MiB. Fresh
Simulator inventories at
`.build/vk-gl-cts-ios-simulator/20260822-131649-63237-16k-gles3-config-inventory-v1`
and
`.build/vk-gl-cts-ios-simulator/20260822-131735-63365-16k-gles2-config-inventory-v1`
match the macOS 20-config digest under that policy. The first repaired
Simulator GLES 3 slice passed 1,625/1,625 after managed topic `fff809d63b`
restored native sampler comparison. The first physical-device run then found
one real failure in `rgb_float_rgb32f_linear` on the A15/Apple8 query path.
It was not an OOM, so memory remained 2,048 MiB. Managed UTM WebKit topic
`66766da048` retains the native query and Simulator route while adding only
the Apple8 R/RG/RGBA32F filtering path;
the rebuilt normally signed app passed the exact same 1,624-case device slice
with 1,563 `Pass`, 61 permitted `NotSupported`, zero failure, clean shutdown,
and zero CPU fallback at
`.build/vk-gl-cts-ios-device-probe/float32-force-56b25e98/20260822-154345-73520-16k-gles3-matrix-cfg00-002of004-v1`.
These are focused closures, not complete mobile matrix results.

The final 4,096 MiB Simulator GLES 2 campaign at
`.build/vk-gl-cts-ios-simulator-matrix/gles2-16k-ce8cfa0f-apple8-restored-4g`
then completed the current 20-configuration product. Its exact 20 x 473
matrix contains 9,460 executions: 8,760 `Pass`, 700 Khronos-permitted
`NotSupported`, and no failure, warning, waiver, missing case, incomplete
shard, dirty shutdown, or retry. A separate verify-only pass reparsed every
retained QPA and console, rechecked the signed inputs, reconstructed every
ordered leaf, and reported
`Verified 9460 gles2 case executions in 20 signed simulator shards`. Every
fresh guest used Gadgets, the common direct ANGLE/Metal presentation path,
4,096 MiB, and the sealed ext4 lower below disposable OverlayFS state. The
complete Simulator GLES 3 and both connected-iPad matrices remain open.

The rebuilt normally signed physical-device app then produced final GLES 2
and GLES 3 configuration inventories at
`.build/vk-gl-cts-ios-device-restored-final/20260822-180049-69193-16k-gles2-config-inventory-v1`
and
`.build/vk-gl-cts-ios-device-restored-final/20260822-180246-70898-16k-gles3-config-inventory-v1`.
Both match their 4,096 MiB Simulator predecessors at all 20 tuples and the
canonical digest, run the guest at 2,048 MiB, return zero, shut down cleanly,
and report zero CPU fallback. The new public marker also recorded `serious`
thermal state at both ends because these inventory boots immediately followed
the long diagnostic device campaign. They are valid configuration inputs but
not thermal acceptance evidence; the final immutable device matrices are
deliberately deferred until the iPad cools. There was no OOM, so no RAM
reduction is authorized.

Phase 2 has completed the common host implementation, dependency, unit, macOS
product, mobile product, and packaging portions. A focused 16 KiB macOS guest
diagnostic proves real DRM/Xwayland/virgl, natural desktop GL 2.1 and GLES 3.0,
one passing frozen GLES 3 case, complete artifact egress, and clean shutdown. A
separate normal 16 KiB iPhone 17 Pro Max Simulator boot selected the common
hardware ANGLE/Metal provider, direct Metal presentation, both Virgl capsets,
and a rendered KDE desktop. The signed app embeds page-specific launchers
generated from common allow-listed CTS campaign definitions; thin Simulator and
physical-iPad wrappers enforce exact IDs, framed artifacts, `Pass`, and
Simulator-before-device ordering. That exact 16 KiB sentinel now passes 1/1 on
both the iOS 26.5 Simulator and the connected iPad mini 6, measuring guest
desktop GL 2.1 and GLES 3.0 through the same DRM/Xwayland/virgl path on each.
The campaign schema now also freezes five-case GLES 2 and twelve-case GLES 3
identity sentinels. Both pass their exact ordered sets on 16 KiB and 4 KiB
macOS, Simulator, and iPad with clean shutdown and zero CPU fallback. Three
non-passes encountered while choosing that bounded identity corpus were not
waived: current macOS qualification classified or repaired both host findings,
and the Simulator GLES 2 texture findings were repaired in the common
ANGLE/Metal dependency. Connected-iPad qualification, seven-pair performance
campaigns, sustained lifecycle acceptance, and the fresh-policy
Simulator/connected-iPad Khronos matrices remain open. The
original exact
allow-listed sentinel also captures
explicit ES 3.0/3.1/3.2 host-context attempts, sorted extensions, every raw and
decoded Virgl v1/v2 capset field, cross-product semantic diffs, and final
native-display counters. The ordered macOS, Simulator, and iPad runs all create
ES 3.0, naturally reject ES 3.1/3.2 with `EGL_BAD_MATCH`, retain the measured
guest GL 2.1/GLES 3.0 result, pass 1/1, and report zero CPU-fallback frames.
Hardware-specific capset differences are recorded rather than incorrectly
requiring byte equality. This completes the plan's host-context/full-capset
probe checkpoint and the bounded fixed identity sentinels under both page
profiles, but not performance qualification. The first iPad shutdown exposed
an intermittent `drm_mm`
managed-release warning. Maintained kernel topic 0020 now retires atomic
Virtio-GPU scanout state before device unplug; both LLVM 22 page-profile builds
passed, and three consecutive 16 KiB Simulator plus three connected-iPad exact
CTS runs then powered off cleanly. The adapters now reject that warning and any
missing clean-stop marker. This is a bounded lifecycle repair, not complete
lifecycle qualification or a capability promotion. Those lifecycle results
used the unchanged original EROFS image; the separately authenticated
derivative CTS workflow is the only guest-userspace exception described by
this checkpoint.

The first fixed 4 KiB macOS Aquarium run exposed a separate kernel
scalability defect: it reached the measurement window, then stalled RCU twice
while Chromium's nested `mprotect` teardown forced generic `mmu_gather`
promotion to `fullmm`. That made Darwin repeat whole-address-space walks and
executor quiesces instead of invalidating the already precise range.
Maintained topic 0021 adds a conservative architecture hook and lets only
Darwin retain precise nested ranges, whose synchronous invalidation already
serializes all executors. A fresh LLVM 22 replay and both canonical builds
passed at final tree `436f25b3c4cad9cbbead1d37bad19e219ce4075d` (163 changed
paths, 9 outside the port), and accepted artifacts
`mainline-6.18-{16k,4k}-20260821-28bfc7ba-1` retain bridge ABI v8. Clean
accepted-artifact Aquarium runs then passed on macOS at 2.533 FPS for 16 KiB
and 2.867 FPS for 4 KiB, and on Simulator at 5.767 FPS and 5.933 FPS,
respectively. Every run captured 30 samples, WebGL 2/GLES 3.0, clean shutdown,
and zero dropped or CPU-fallback frames. These are one-run implementation
canaries, not the frozen seven-pair performance qualification, and Aquarium
remains forbidden on the physical iPad. Phases 3 through 7 have not started;
they remain gated on a truthful, qualified lower shared baseline and the
explicit stop conditions below.

The current 16 KiB macOS product also completes the rendered functional-page
gate. `.build/webgl-samples-accepted/20260822-194715` booted release
`6.18.41-geebc662cf6b6` through VNC with Gadgets, ten vCPUs, 8 GiB, and the
sealed derivative read-only below disposable OverlayFS state. All 33 frozen
pages created a live WebGL context through
`ANGLE (Mesa, virgl, OpenGL ES 3.0)` and emitted the ordered success markers,
ending with `UML_WEBGL_ALL_OK|COUNT=33`. The transcript contains no Virgl
illegal-resource, invalid-command-buffer, context-error, or `0x1204` reply.
This is functional rendering evidence; the runner deliberately stops the VNC
guest after capture, so it is not native-presentation, performance, or clean
shutdown evidence.

In this plan, “maximum” means the highest truthful profile that preserves the
shared high-performance ANGLE/Metal path. A higher version is rejected when it
materially worsens throughput, latency, frame pacing, CPU/GPU time, energy,
memory, thermals, or presentation efficiency.

## Decision summary

Make the iOS ANGLE renderer architecture the only compiled GL path on macOS,
iOS Simulator, and iOS device. iOS remains the primary product, but use macOS
first for the edit/build/debug/sanitizer loop because it is faster to launch
and inspect. Every macOS change must exercise the exact source identity, build
recipe, renderer, capset, shader, and native-resource contract that will next
be validated on iOS; a macOS-only graphics provider is forbidden.

- The current iOS ANGLE/Metal stack would deliberately lower macOS from guest
  desktop GL 4.1 to the iOS-class desktop-GL ceiling, expected to be GL 2.1,
  while retaining GLES 3.0. It will not by itself raise iOS above GLES 3.0.
- Accept that initial macOS ceiling reduction and remove CGL from the planned
  product, build graph, runtime options, dependency prefixes, and regression
  matrix. The previously measured CGL result may remain in archived evidence;
  it is not a runnable lane or a comparison gate.
- Use macOS as the normal first development, guest-test, debugger, sanitizer,
  VNC, and capability-iteration target. Validate the same change next on the
  iPhone 17 Pro Max Simulator. macOS evidence never substitutes for Simulator
  validation or determines an iOS capability claim.
- Require the complete agreed physical-device campaign on the connected iPad
  before qualifying any profile. Simulator and macOS evidence cannot replace
  that gate.
- Treat guest-kernel graphics support as an owned implementation layer, not an
  external ceiling. Backport or implement required `virtio_gpu` DRM/KMS,
  blob-resource, mapping, fence, and UML transport behavior in the maintained
  common kernel patch series while keeping the ABI consumed by the exact Mesa
  and libdrm installed in the current image compatible with their standard
  userspace contract.
- Make **GLES 3.0 and desktop GL 2.1 measured but unqualified** the committed
  direct-hardware shared baseline candidate. Phase 2 must still qualify its
  absolute performance on macOS, Simulator, and iPad. Treat GLES 3.1 as a
  stretch target only through direct ANGLE/Metal implementation of compute,
  storage, image, atomic, and related behavior on the GPU. The GL/GLES
  compatibility path must never select
  ANGLE/Vulkan, MoltenVK, Zink-over-Vulkan, or another Vulkan translation
  layer.
- Consider desktop GL 3.0/3.1 only when every missing gate can remain
  GPU-resident and the complete profile passes the performance budget. Do not
  implement conditional rendering, geometry expansion, draw rewriting, or
  another advertised feature with synchronous readback, per-draw CPU work,
  shader interpretation, or substantial CPU command replay.
- Stop at the last fast profile. GLES/GL 3.2 and GL 4.x are not targets in this
  plan because the chosen ANGLE/Metal provider lacks the required direct
  geometry/desktop feature path and the known alternatives add heavy
  translation or multi-pass emulation. Efficient hardware-backed support may
  be reconsidered only after a new measured Metal capability or provider makes
  those statements false on the connected iPad.

The recommended development order is therefore:

```text
retain the already-recorded CGL result as archived evidence only
    -> replace every Apple product with one ANGLE provider/build product
    -> backport or implement any required common virtio_gpu kernel support
    -> develop and test the exact shared path on macOS first
    -> validate the same change on the iPhone 17 Pro Max Simulator
    -> qualify the milestone on the connected iPad
    -> ship the measured GLES 3.0 / desktop GL 2.1 baseline after qualification
    -> attempt direct ANGLE/Metal GLES 3.1 mappings
    -> attempt only GPU-resident desktop-GL 3.0/3.1 gates
    -> stop when the next profile requires CPU-heavy or materially slower emulation
```

## Scope and invariants

This plan keeps the guest API path:

```text
guest application
    -> released upstream Mesa Virgl Gallium
    -> project-maintained Linux virtio_gpu DRM/KMS
    -> project-maintained UML Virtio/vhost-user transport
    -> VirtioGpuDevice
    -> virglrenderer
    -> ANGLE EGL/GLES frontend
    -> ANGLE Metal renderer
    -> Metal rendering and presentation
```

The following are non-negotiable:

1. iOS is the primary product. Renderer and protocol choices must satisfy iOS
   public-API, signing, sandbox, lifecycle, and current memory constraints
   first: 4 GiB on Simulator and 2 GiB initially on physical devices. macOS is
   the fast development host for that design and may not add desktop-only
   graphics assumptions.
2. Only guest userspace is externally constrained. `rootfs.erofs` and its
   installed guest userspace are fixed inputs to this plan. Use the exact Mesa,
   libdrm, CTS executables/resources, WSI support, and desktop packages already
   installed there. Do not replace, preload, overlay-substitute, rebuild,
   repackage, or provision any of them, even with a newer unmodified release.
   If a milestone requires different guest userspace, stop that phase, retain
   the lower truthful ceiling, record the exact blocker, and ask the user before
   any rootfs or guest-userspace change. The implementation checkpoint records
   the one subsequently requested derivative exception; it does not change the
   original image or relax this rule for later work.
3. The guest kernel is controlled by this project. Required `virtio_gpu`, DRM,
   UAPI implementation, Kconfig, or `arch/um` transport changes belong in the
   maintained common Linux patch series and one kernel image per page profile.
   Prefer standard Virtio-GPU and DRM interfaces; a project-specific
   kernel-to-emulator extension is acceptable only when negotiated, bounded,
   invisible to guest userspace, and presented through a standard-compatible
   ABI that the exact unmodified Mesa/libdrm in `rootfs.erofs` already
   understands. Do not
   make a private userspace ioctl or library a production dependency. Any new
   graphics Kconfig support belongs built-in in `kernel/mainline-6.18/config/common.fragment`
   because loadable modules remain disabled; page fragments vary only genuine
   page-derived values.
4. Never use `MESA_GL_VERSION_OVERRIDE`, `MESA_GLES_VERSION_OVERRIDE`, forced
   Virgl capability bits, or extension-string injection as evidence. A version
   rises only when natural capset discovery and functional tests agree.
5. Preserve the real Virtio-GPU/Virgl path. Do not substitute an app-owned
   framebuffer or Chromium-specific library preload.
6. Preserve one common maintained Linux source and the existing
   platform-neutral device ownership. Build exactly the canonical 16 KiB and
   4 KiB artifacts; graphics changes must not create a platform-specific Linux
   image or public emulator ABI.
7. ANGLE/Metal is the only GL/GLES host provider. Do not route GL/GLES through
   ANGLE/Vulkan, MoltenVK, Zink, Venus, or another Vulkan implementation, even
   as an experimental profile-promotion lane. Do not build, link, or package
   CGL or a desktop-only virglrenderer/libepoxy graphics family, and do not
   fall back to Homebrew or ambient Vulkan, EGL, GL, or `pkg-config`
   installations. If Vulkan support is also built, its Venus-to-MoltenVK
   targets and loader remain separate from the ANGLE/Metal GL facade.
8. Keep iOS App Store compatibility: public APIs only, ordinary signing, no
   JIT entitlement, no private Metal API, and no helper process required by
   the graphics backend.
9. Report macOS, Simulator, and connected iPad separately. macOS is the first
   development stage, Simulator is the required iOS validation stage, and the
   connected iPad is the mandatory physical-device qualification stage.
10. Do not run WebGL Aquarium on the connected iPad. Start its desktop and
   qualification workloads at 2 GiB; only a confirmed emulator OOM permits a
   fresh run 64 MiB lower, repeated one step at a time until stable.
11. Profile version is subordinate to performance. Software emulation is
   forbidden when it adds heavy CPU work, synchronous GPU-to-CPU feedback,
   per-draw CPU expansion, or sustained command serialization. GPU-accelerated
   lowering is allowed only when it stays GPU-resident and passes the explicit
   regression, CPU, frame-pacing, memory, and thermal budgets in this plan.
   A lower, faster profile is a successful final outcome.
12. External-project access is fetch-only. Never push a branch, open or update
   a pull request, merge request, or issue, send a patch or email, or upload an
   artifact to an external project. First-party emulator and test-orchestration
   changes live directly in this repository; third-party
   ANGLE or virglrenderer source changes live only in
   `dependencies/patches`; Linux changes live only in `kernel/mainline-6.18/patches`.
   Qualification CTS source remains released and unmodified. Prepared
   dependency trees remain remote-less and builders stay offline after
   `dependencies/prepare.sh`.
13. The current authenticated `rootfs.erofs` and its provisioning manifest are
   read-only evidence, not work products. No step in this plan may rebuild,
   replace, reprovision, or modify the image or substitute guest graphics
   libraries, CTS executables/resources, runners, or WSI components through an
   overlay or preload. A disposable overlay remains permitted only for
   first-party campaign scripts, transient configuration, and bounded output.
   If an image change becomes unavoidable, pause the plan and ask the user
   before running any rootfs build or provisioning command.

Hardware video encode/decode, a new guest display server, and a Vulkan guest
API are outside this file. The Vulkan transport and memory plan is in
[MAX_VULKAN_SUPPORT_PLAN.md](MAX_VULKAN_SUPPORT_PLAN.md); the two plans may
share coherent-blob infrastructure but retain separate acceptance claims.

## Baseline and target ladder

The values below distinguish measured state, the committed direct-hardware
baseline candidate, and conditional stretch work. “Expected” means the capset
predicates imply the value but a current guest probe has not yet captured it.
A stretch row is discarded if it violates the performance gates; it is not
replaced by a slower emulation path.

| Host renderer | macOS guest | Simulator guest | Connected-iPad guest | Status |
| --- | --- | --- | --- | --- |
| Apple CGL 4.1 | GL 4.1, GLES 3.0 | unavailable | unavailable | archived pre-migration measurement only; no planned build, runtime, or test lane |
| Current UTM ANGLE/Metal | GL 2.1 and GLES 3.0 measured in a focused 16 KiB guest diagnostic | GL 2.1 and GLES 3.0 measured by the exact 16 KiB signed-app sentinel; the normal boot also proved both Virgl capsets, direct Metal presentation, and rendered KDE | GL 2.1 and GLES 3.0 measured by the identical 16 KiB signed-app sentinel | all three host slices build from the common product and the exact allow-listed case passes on macOS, Simulator, and iPad; complete identity/capset, matrices, performance, thermal, and lifecycle qualification remain pending |
| Direct ANGLE/Metal ES 3.1 implementation | GL 2.1 expected, GLES 3.1 stretch ceiling | same conditional ceiling | same conditional ceiling | acceptable only with native Metal compute/storage/image/atomic mappings and the full performance gate |
| Previous row plus GPU-resident desktop gates | at most GL 3.1, GLES 3.1 | same conditional ceiling | same conditional ceiling | optional stretch; retain the immediately lower qualified desktop tier, GL 2.1 or GL 3.0, when a required next-tier gate needs CPU-heavy or materially slower emulation |
| GLES/GL 3.2 or GL 4.x | not targeted | not targeted | not targeted | geometry/desktop translation does not meet the selected direct ANGLE/Metal, performance-first architecture |

Important limits behind the table:

- ANGLE's Metal backend currently has
  [`kMaxSupportedGLVersion = 3.0`](https://chromium.googlesource.com/angle/angle/+/1da7e3afb21d4f1f3e5a9615b8673c15288f8727/src/libANGLE/renderer/metal/mtl_common.h),
  and [ANGLE's own platform support table](https://chromium.googlesource.com/angle/angle/+/main/README.md#platform-support)
  marks Metal complete through ES 3.0, not ES 3.1.
- The shipped Simulator ANGLE build refuses explicit ES 3.1 and ES 3.2
  contexts. Changing macOS to that exact path aligns the version ceiling; it
  does not increase it.
- Metal exposes hardware compute, storage, image, and atomic primitives that
  may support a direct ANGLE/Metal GLES 3.1 implementation. That possibility
  must be proved feature by feature on macOS, Simulator, and the connected
  iPad; a Vulkan detour is not an allowed proof or fallback.
- Unchanged virglrenderer does not advertise conditional rendering on a GLES
  host lacking `GL_NV_conditional_render`. The current image's Mesa consequently cannot
  derive desktop GL 3.0 even when the same host reaches GLES 3.1.
- GLES 3.2 requires geometry behavior that the selected ANGLE/Metal backend
  does not expose directly. Compute/mesh expansion, multi-pass rewriting, or a
  second translation provider is outside the target unless a new probe proves
  a fully GPU-resident implementation meets the same performance budget on the
  connected iPad. GL 4.x is likewise outside this provider's target surface.
- Missing Linux `virtio_gpu` behavior does not lower the architectural ceiling:
  it is implementation work owned by this repository. The external ceiling is
  the intersection of the immutable guest userspace currently installed in
  `rootfs.erofs` and the truthful
  host/backend behavior after the maintained kernel supplies the standard
  userspace-facing ABI.
- The pinned Linux source already contains the standard blob wire definitions,
  DRM blob ioctls, and `RESOURCE_BLOB`/`HOST_VISIBLE` getparams. Its
  `virtgpu_kms.c` exposes host-visible memory only when the transport supplies a
  Virtio shared-memory region, while the maintained UML transport does not yet
  implement `.get_shm_region` and the host device does not yet implement blob
  commands. The current image's Mesa naturally derives Virgl `supports_coherent` from
  exactly those two standard getparams. Separately, its persistent-coherent
  buffer gate requires `VIRGL_CAP_ARB_BUFFER_STORAGE` and host feature-check
  version 4 or later. This owned host/kernel work is optional transport
  optimization under the selected lower profile; it is not justification for
  pursuing GL 4.4-4.6 or accepting a slower provider.

The current rootfs Mesa and installed CTS case universe cover the planned GL
2.1/GLES 3.0 baseline and the GL 3.0/3.1/GLES 3.1 attempts, so an image update
is not inherently required for this plan. Qualification still depends on
reconstructing the existing CTS provenance and reproducing its exact
`mustpass.xml` matrix. If either cannot be established read-only, development
evidence may continue but promotion pauses and asks the user.

## Architecture changes

### 1. Separate the common product contract from presentation shells

`UML_GPU_ANGLE_METAL` currently means all of the following at once:

- select the EGL/GLES provider;
- use ANGLE's Metal backend;
- compile the iOS-sized `VirglRenderer` object;
- export `MTLTexture` rather than an OpenGL texture name;
- disable VNC; and
- select the mobile product.

That conflation caused a real object-size mismatch and cannot safely be reused
for macOS. Replace it with explicit fields, while fixing the provider field in
the Apple-GPU product contract for all three platform slices:

```text
Apple platform slice:       iOS Simulator | iOS device | macOS
product-fixed GL provider:  ANGLE Metal
thin UI shell:              UIKit (Simulator/device) | AppKit (macOS)
optional output consumer:   native view | VNC view (macOS only)
resource result:            Metal texture | diagnostic/VNC readback | unsupported
```

Prefer a fixed-layout `VirglRenderer` with an opaque provider object rather
than another public layout-changing macro. Candidate internal types are:

```text
HostGlProvider
    AngleMetalHostGlProvider  (Metal EGL display attributes only)

NativeResource
    MetalTextureResource
```

The provider owns context creation, sharing, current-context state, EGL
cleanup, and native-resource discovery. `VirglRenderer` retains protocol
submission, capsets, resource lifetime, and fences. All callers see one class
layout, and the provider remains in the device layer.

If the Vulkan plan is implemented too, its renderer session becomes the sole
owner of virglrenderer's process-global initialization, but this provider
remains an ANGLE/Metal GL facade and Venus remains a separate MoltenVK Vulkan
facade. Do not create independent global owners, and do not let the GL facade
resolve MoltenVK entry points or select ANGLE's Vulkan display type.

Each Apple-GPU product contract selects ANGLE/Metal for macOS, Simulator, and
device. macOS has no runtime selector or private provider choice that can
diverge from iOS. CGL, ANGLE/Vulkan, and Zink have no GL-provider enum, option,
preset, imported target, or compiled fallback. An incomplete platform slice
fails dependency verification before the guest starts.

### 2. Share Metal-texture presentation without sharing UIKit/AppKit

The iOS native display already consumes an ANGLE-exported `MTLTexture`
directly. Replace the macOS OpenGL-texture/IOSurface bridge with the same
ANGLE-exported Metal-texture contract; only the AppKit/UIKit presentation
shells remain platform-specific.

Extract only the Metal presentation timeline and texture sampling into common
device-owned code:

- one newest-frame-wins publication state;
- at most one Metal command buffer in flight;
- accepted/dropped/unsupported outcomes;
- bounded diagnostic/VNC CPU readback after an unsupported direct resource,
  never a steady-state native-display fallback;
- existing `UML_GPU_FRAME_STATS` accounting; and
- no Objective-C types in platform-neutral headers.

Keep thin platform shells:

- AppKit retains window creation, main-thread event loop, keyboard, pointer,
  wheel input, window title, and VNC availability;
- UIKit retains the application-provided `MTKView`, touch/text integration,
  safe-area sizing, and lifecycle;
- the ANGLE/Metal provider delivers the native Metal texture when its resource-export
  contract supports it.

If retained, VNC is only a presentation consumer of the common renderer's CPU
readback/damage stream. It cannot create a context, select a provider, change a
capset, or fall back to desktop GL. Optional VNC readback must be damage-bounded
and satisfy a separately frozen host/guest CPU budget. Disable VNC for a
format/mode that requires sustained full-frame readback or exceeds that budget;
diagnostic status does not waive the no-heavy-CPU rule.

Use bounded CPU readback only as an early diagnostic and VNC correctness tool.
Sustained native-display qualification requires the same-device ANGLE/Metal
texture path with no per-frame CPU readback. If an accepted format cannot use
that path without heavy CPU copies, mark it unsupported or retain the lower
profile instead of shipping the slow fallback as normal presentation.

### 3. Authenticate one source matrix for all Apple products

The dependency-management refactor is the baseline for this plan. All
project-owned external-library identity and policy now lives under
`dependencies/`: one Git-based `manifest.tsv`, checked-in platform contracts
and generated build receipts where applicable, ordered mail-patch series,
preparation, builders, CMake imports, and prefix verifiers. The deleted
Kernarium-local manifests, build scripts, CMake modules, and source-archive
pipeline must not be recreated.

The central manifest records an HTTPS Git repository, full immutable commit,
product membership, optional sparse source paths, and an optional ordered
`series.tsv`. `dependencies/prepare.sh` is the only network-capable step. It
publishes clean detached prepared repositories with no remotes and replays
declared changes with `git am --3way`; its offline `--verify` mode is the
required input gate for every builder. A maintained third-party change is
edited as a commit on a clean topic branch and regenerated with
`dependencies/rebase-patches.sh`. Never hand-edit a generated `.patch` or
invent an archive/tree hash alongside the Git identity.

The refactored current state still has two different graphics source families,
which this plan treats as migration input rather than a supported end state:

- `macos` currently selects desktop libepoxy/virglrenderer rows that must be
  removed from the macOS product when the common Apple-GPU product lands; and
- `ios-gpu` selects pinned UTM WebKit/ANGLE, UTM libepoxy, and UTM
  virglrenderer for the Device and Simulator prefixes. Its builder emits
  static libepoxy/virglrenderer plus `EGL.framework` and
  `GLESv2.framework`, while format-2 checked-in platform contracts describe
  platform, architecture, deployment target, components, linkage, graphics,
  presentation, and source-manifest ownership. `VerifyIosGpuPrefix.cmake`
  combines that contract with byte-for-byte manifest/series/patch provenance
  and binary-shape checks; the platform file is not itself a generated build
  receipt.

Make the replacement atomically and visibly: introduce one reviewed
Apple-ANGLE product membership in the central manifest, remove the obsolete
desktop graphics rows from `macos`, and generalize the refactored GPU
builder/CMake verifier to produce three platform contracts from
the exact same prepared repositories, ordered patch series, feature flags, and
renderer configuration:

```text
ios-simulator-arm64
ios-device-arm64
macos-arm64
```

This plan selects an `apple-gpu` product and
`dependencies/build-apple-gpu.sh`; there remains one central manifest, one
common GPU recipe, and exactly one graphics prefix per platform. The old
`build-ios-gpu.sh` entry point is removed after consumers move atomically; it
must not remain as a divergent recipe. The ordinary macOS dependency prefix
may continue to contain non-graphics libraries, but no desktop GL provider,
libepoxy, or virglrenderer. The common recipe must publish through an
exclusive staging/rename operation, verify a
concurrent winner before reuse, never overwrite a mismatched prefix, and
install the central manifest, platform contract, exact series files, and patch
bytes below `share/uml-dependencies/`.

Generalize `dependencies/cmake/IosGpuDependencies.cmake` and
`VerifyIosGpuPrefix.cmake` into Apple-GPU consumers/verifiers rather than
adding a macOS-only importer. Every checked-in platform contract must record
the deployment target, architecture, linkage/payload kind, graphics and
presentation APIs, components, source-manifest path, and renderer feature
policy. Bump the current format-2 contract only when those new fields land,
and update all three contracts, the builder, verifier, CMake imports, packaging,
and tooling fixtures in the same change. Record the real Xcode/SDK/compiler
and exact build command as run evidence (or in a generated per-prefix build
receipt if the common builder adopts one); do not put normalized timestamps,
binary hashes, or mutable tool
observations in the checked-in platform contract. Verification must compare
installed provenance to the checked-in manifest/series bytes and
check architecture, Mach-O platform, minimum OS, install names, symlink
absence, allowed payload, and runtime closure. All products select the same
libepoxy/virglrenderer imported targets from their Apple-GPU platform prefix.
Delete the desktop graphics imports and reject CGL, OpenGL.framework, or a
second virglrenderer family in CMake File API and final-link checks.

The macOS executable currently rejects non-system dynamic dependencies. The
common Apple-GPU product must replace that desktop-only assumption with one
framework contract: it emits authenticated `EGL.framework` and
`GLESv2.framework` slices for macOS, Simulator, and device from the same source,
patch, feature, and renderer configuration. macOS stages its slices in its
verified app/CLI runtime in the same way Kernarium packages the iOS slices,
with only unavoidable Mach-O platform, signing, and lifecycle differences.
A static macOS-only ANGLE build cannot satisfy a phase exit because it would no
longer exercise the iOS dependency and loader path.

Do not relax the runtime-dependency verifier globally. Teach it the exact
authenticated framework locations, fixed `@rpath` install names, allowed
closure, and one-copy rule for each Apple-GPU platform product.

## In-image VK-GL-CTS development and qualification strategy

The current authenticated `rootfs.erofs` contains useful VK-GL-CTS
1.4.5.5-labeled binaries, resources, and co-located source. Use them as the
main guest-side test instrument instead of adding a private test library or
relying only on browser workloads. The whole-image signature authenticates the
bytes but the current provisioning metadata does not prove that the co-located
source produced the ELF files. Phase 0 may recover an existing authoritative
build receipt through read-only inspection, but it must not reconstruct or
invent historical provenance. Without that receipt, keep the artifacts
diagnostic-only, record the provenance gap in repository-local evidence, pause
qualification, and ask the user before proposing any image change. No such
inspection may change `rootfs.erofs`, its provisioning manifest, or an
installed file. The separately authenticated derivative in the implementation
checkpoint was built only after that explicit direction and is not historical
provenance for these original binaries.

### Installed artifacts and their roles

| Guest path | Identity | Intended use |
| --- | --- | --- |
| `/opt/vk-gl-cts/gles2/deqp-gles2` | AArch64/Linux PIE, 11,118,432 bytes, build ID `d2f695f3ac1ed9e7d4b85b2ff19326ce5cce3f1a` | fast GLES 2 functional and regression groups |
| `/opt/vk-gl-cts/gles3/deqp-gles3` | AArch64/Linux PIE, 17,923,136 bytes, build ID `54febf8ac17f594ca10ec7523a3e91af5d6f1e69` | fast GLES 3.0 functional and regression groups |
| `/opt/vk-gl-cts/openglcts/glcts` | AArch64/Linux PIE, 65,486,984 bytes, build ID `884796d1a220663afb5b79d1e96f622b3c1e7e14` | Khronos GLES 2/3/3.1/3.2 and desktop GL 3.0 through 4.6 promotion gates |
| `/home/uml/vk-gl-cts-1.4.5.5` | co-located 1.4.5.5-labeled source; no `.git` directory | read-only test-logic and expected-result derivation plus failure localization; never a source for external submissions or altered qualification expectations |

The resource trees beside each executable are part of the artifact. Run from
the executable's directory or pass that directory through
`--deqp-archive-dir`; never copy only the executable or mix a new executable
with old resources and caselists. The image has no `deqp-runner` or
`cts-runner`, so the repository must own the campaign orchestration rather than
silently depending on an ambient package. The source does define upstream
`cts-runner`, but do not provision it into the image or a guest overlay. Use
the installed `glcts` plus repository-owned exact `mustpass.xml` expansion for
GL/GLES qualification. Differential-test that expansion against a disposable
macOS build of the same released runner used solely as an argument-generation
oracle; never copy the oracle into the guest. If the exact configuration matrix
cannot be reproduced with the installed artifacts, pause qualification and ask
the user rather than changing the rootfs.

For release promotion, prefer `glcts` and the installed Khronos material:

- GLES lists are under
  `/opt/vk-gl-cts/openglcts/gl_cts/data/mustpass/gles/khronos_mustpass/main/`:
  `gles2-khr-main.txt` (473 cases), `gles3-khr-main.txt` (6,498),
  `gles31-khr-main.txt` (4,101), `gles32-khr-main.txt` (1,405), and
  `gles32-khr-glesext.txt` (1,097);
- desktop lists are under
  `/opt/vk-gl-cts/openglcts/gl_cts/data/mustpass/gl/khronos_mustpass/main/`:
  `gl30-main.txt` through `gl46-main.txt`, plus the matching `-gtf-main.txt`
  lists and the compatibility list where the selected profile requires it;
  representative sizes are 888 cases for `gl31-main.txt`, 10,816 for
  `gl41-main.txt`, and 18,647 for `gl46-main.txt`; and
- the adjacent `mustpass.xml` files own the required context, surface size,
  color/depth/stencil configuration, and list combinations. A single leaf
  list run with a convenient pbuffer is a development slice, not execution of
  the official must-pass configuration matrix.

At Phase 0, export the complete text hierarchy for each profile in the lower
shared baseline and record its ordered digest, case count, module, executable
build ID, and selected must-pass-leaf digest. The initial scope is exactly
`KHR-GLES2` and `KHR-GLES3`. Do not use an unfiltered `glcts` traversal as the
GLES identity: that binary also registers future-profile, ES-on-desktop-GL,
and desktop GL packages whose context requirements deliberately exceed the
lower renderer. Record those registrations as structural diagnostics. A later
phase adds `KHR-GLES31`, `KHR-GLESEXT`, `KHR-GLES32`, or the applicable
`KHR-GL*` package to its own scoped identity only after that candidate context
exists naturally. Fail closed if a scoped hierarchy and its installed list
disagree. Do not compare raw pass counts across different CTS releases or
caselist digests.

### Repository-owned campaign driver

Add `tests/integration/gpu/run_vk_gl_cts.py`, modeled on the existing rendered
guest workflows rather than on the pre-display arbitrary-command runner. It
must:

1. boot the ordinary Virtio-GPU guest with a disposable OverlayFS upper layer
   and use the gadget or JIT engine, never the interpreter for a long CTS run.
   The upper layer may hold only first-party campaign scripts, transient
   configuration, and output; it must not replace or add Mesa, libdrm, CTS
   executables/resources, Xwayland, loaders, or other guest components;
2. wait for the real display manager and DRM nodes, then use the actual CTS WSI
   and selected image account. The provisioned binaries currently require the
   real X11/Xwayland socket and xauth: use `uml` for KDE, or the image's
   UID-1000 Phosh session user under greetd/Phrog, inheriting that session's
   `DISPLAY`, `XAUTHORITY`, `XDG_RUNTIME_DIR`, and DRI permissions. Prove
   Xwayland exists on the selected iOS desktop before qualification. If the
   installed image lacks a usable CTS WSI, mark qualification blocked, record
   the exact missing component, and ask the user; do not build, inject,
   preload, overlay, or provision a replacement target;
3. preserve the actual Virgl path and reject llvmpipe, software rasterizers,
   unexpected guest Zink, Xvfb, and a missing or mismatched renderer before
   counting any result;
4. accept the API/context, module or leaf list, exact-case selector, must-pass
   configuration, fraction, page profile, engine, seed, and timeout as typed
   arguments rather than an unreviewed shell string;
5. serialize one CTS process per guest, enforce an outer per-run timeout, emit
   progress/heartbeats, retain the first failing case, and always request a
   clean guest shutdown. Preserve the watchdog setting declared by the exact
   `mustpass.xml`; enable dEQP watchdog/crash handlers only in developer slices
   after focused UML signal/timer tests qualify them;
6. verify that the union of completed shards equals the frozen selected
   caselist, detect missing/duplicate cases, and refuse an incomplete or
   early-exit shard; and
7. capture the host revision and dirty state, product/model/OS, backend,
   dependency receipt, rootfs identity, CTS release and build ID, Mesa version,
   kernel/page profile, capsets, renderer/extensions, command/environment,
   caselist digest/count, seed, per-status totals, QPA log, console, journal,
   dmesg, host renderer/frame statistics, exit reason, and shutdown result.

The privileged bootstrap creates a bounded `/run/uml-cts` output directory
owned by the selected graphical-session user before dropping privileges; CTS
itself must not run as root.

Make the campaign definition common, with thin control adapters for the macOS
CLI, the iPhone 17 Pro Max Simulator Kernarium app, and the connected-iPad app.
The iOS adapters must select only validated campaign/shard identifiers, launch
the same guest script, capture the app console/lifecycle, and retrieve the
bounded artifact through the existing console bridge or a diagnostic app
export. A host-only Python runner is insufficient if it cannot start and
control the signed iOS product.

The current generic GPU runner retains only console/run metadata and destroys
the guest upper layer, so a QPA file left in `/run` is lost. Add bounded
artifact egress: compress each QPA and summary and stream them through the
serial command channel in framed, checksummed, acknowledged chunks, or attach
an explicit disposable scratch device distinct from `/dev/ubda`. Never write
the EROFS root block device. Add parser, shard-union, path-safety, timeout, and
artifact-completeness tests, and label long/full/device campaigns as opt-in
`gpu;cts;rootfs` work rather than adding them to ordinary unit acceptance.

One leaf/configuration invocation has this shape:

```sh
cd /opt/vk-gl-cts/openglcts
./glcts \
  --deqp-gl-context-type=egl \
  --deqp-caselist-file=gl_cts/data/mustpass/gles/khronos_mustpass/main/gles31-khr-main.txt \
  --deqp-archive-dir=. \
  --deqp-test-oom=disable \
  --deqp-log-filename=/run/uml-cts/gles31-config-shard.qpa \
  --deqp-log-images=disable \
  --deqp-log-shader-sources=disable \
  --deqp-log-flush=enable
```

Here `egl` is the registered platform context factory; the `KHR-GLES31`
package requests the ES 3.1 API. The driver must still validate registered
factory names against the exact binary before freezing them. For an official
promotion run, use the installed `glcts` with the repository-owned,
externally differential-tested expansion of every configuration declared by
`mustpass.xml`. If that exact expansion cannot be reproduced, pause and ask
the user rather than provisioning `cts-runner`. Do not override the declared
seed, watchdog policy, first/other EGL
configuration split, specified 64x64 or 113x47 surfaces, or required color,
depth, and stencil formats with one convenient pbuffer. A focused developer
case may use an offscreen pbuffer only when that case permits it. Re-run a
failure with image and shader-source logging enabled rather than carrying that
memory cost through every shard.

### Campaign ladder tied to capability work

Run these levels in order:

1. **Identity smoke on every relevant change.** On macOS, run the
   information/capability cases and a small fixed sentinel
   covering context creation, clear/draw/readback, shaders, buffers, textures,
   synchronization, and negative limits. Verify the renderer before using the
   result, then run the identical sentinel on the iPhone 17 Pro Max Simulator.
2. **Change-local groups.** Select exact case groups implicated by the capset,
   format, shader lowering, resource, or synchronization change. GLES 3.1 work
   includes compute, SSBO/layout/array length, image load/store and image size,
   atomics, indirect draw/dispatch, framebuffer-without-attachments, program
   interfaces/separate programs, barriers, multisample/sample shading, limits,
   and negative API/shader cases. Run both `dEQP-GLES31` functional groups and
   the corresponding `KHR-GLES31` groups.
3. **Cross-feature and stretch-profile gap scans.** Nightly or on a repository
   candidate, deterministically shard the complete relevant module with
   `--deqp-fraction=N,M`. Run only the permitted GLES 3.1 and GL 3.0/3.1
   stretch modules on macOS first and then Simulator. Use failures to decide
   whether to continue or stop at the lower profile; do not turn GLES 3.2 or
   GL 4.x into an implementation backlog.
4. **Version-promotion gate.** Run every `mustpass.xml` configuration and its
   shipped Khronos list for the candidate version and every lower version that
   remains claimed. Run macOS first, the identical Simulator campaign second,
   and the complete selected campaign on the connected iPad last.
5. **Performance-ceiling qualification.** For a GLES 3.1 or desktop GL 3.0/3.1
   candidate, run the cumulative Khronos lists and the representative
   performance corpus in the same promotion campaign. A CTS pass does not
   override a performance failure. GLES 3.2 and GL 4.x lists may be run only
   as occasional read-only capability probes that confirm the documented stop
   boundary; they do not authorize emulation work or a higher claim.

The production iPad guest starts at 2,048 MiB. If the emulator is OOM-
terminated, preserve that failed attempt and start a new immutable campaign
64 MiB lower; repeat only after another confirmed OOM. Run one shard at a time, disable
the suite's deliberate OOM tests, keep routine image/shader logging off, bound
QPA and console growth, export artifacts after every shard, and start a fresh
guest when resident memory does not return. Run every required shard under the
16 KiB and 4 KiB profiles for page-, resource-, mapping-, or blob-sensitive
milestones. Bounded shards are not permission to omit cases. Do not compile
CTS or run WebGL Aquarium on the iPad.

### Results, waivers, and source-guided debugging

Only `Pass` satisfies a required case. `Fail`, `InternalError`,
`ResourceError`, `Crash`, `Timeout`, device/context loss, infrastructure OOM,
early exit, missing case, or incomplete shard fails the gate. Treat
`NotSupported`, `QualityWarning`, and `CompatibilityWarning` as non-pass for a
required must-pass case unless that exact CTS release's Khronos criteria
explicitly allow the status and the evidence records that rule. Results
outside the claimed API remain gap-discovery data.

A retry is diagnostic: preserve both attempts, the seed, and the first failure;
a retry pass does not erase a flake. Never convert a baseline failure list into
success. A development exception ledger must name an exact case, observed
signature, CTS identity, backend/platform/device/page profile, published
specification, pre-existing CTS issue or repository-local defect record,
owner, rationale, and expiry, while retaining the failure in totals.
Qualification accepts a non-`Pass` only when that exact CTS release's Khronos
criteria explicitly permit it. Broad wildcard exceptions, local
renderer-defect waivers, and waivers for a newly advertised core requirement
cannot promote a version. The repository parser must enforce this even if an
upstream runner summarizes an allowed `NotSupported` more leniently.

Use `/home/uml/vk-gl-cts-1.4.5.5/external/openglcts/modules` and the matching
GLES module sources under `/home/uml/vk-gl-cts-1.4.5.5/modules` read-only to
trace a failing case from registration to its assertion/expected-result
derivation, identify the precise API state or limit, and construct the smallest
exact-case rerun. Do not patch expected results, reference images, discovery,
or caselists to make the renderer pass.
Because the source has no `.git` metadata, diagnostic rebuilds must use an
authenticated 1.4.5.5 checkout in disposable Simulator/macOS storage with
recorded flags, but they cannot be copied into the guest, alter expectations,
or qualify a release. Do not upload CTS changes. If a CTS defect prevents
qualification, use only an exact status explicitly accepted by that Khronos
release criteria. Otherwise stop qualification, record any independently
published correction as a possible resolution, and ask the user before any
image or guest-CTS change. If the target API or required
CTS revision outgrows the installed 1.4.5.5 set, stop the affected promotion,
record the exact missing coverage, and ask the user before any image change.
Do not rebuild, inject, or mix a newer executable, resources, must-pass data,
or source into the guest under this plan.

## Implementation phases

### Phase 0: freeze and automate the baseline

Before changing a provider:

1. Record the revision and dirty status, compiler/SDK, Apple-GPU platform
   contracts and installed provenance, rootfs Mesa version, pinned Linux base
   and reviewed final-tree identities, ordered kernel-topic manifest, resolved
   common configuration, and both page-profile configurations.
2. Freeze the matched performance corpus, run length, warm-up, sample count,
   noise envelope, and promotion budgets before implementing a higher profile.
   The maximum promotion thresholds are a 3% throughput decrease or
   frame-time/resource-cost increase on unaffected lower-profile workloads; a
   5% median throughput decrease or median frame-time increase on
   representative candidate-feature workloads; and a 10% increase in p95
   frame time, host/guest CPU time, GPU time/energy, or resident memory.
   Improvements are unrestricted. Measurement uncertainty must be
   sufficiently below the applicable limit to establish compliance; otherwise
   the result is inconclusive and cannot promote. These thresholds may be
   tightened; loosening them requires an explicit plan revision before the
   candidate is measured, never a post-hoc exception. For candidate-only
   behavior with no semantic lower-profile A/B, freeze an equivalent
   direct-Metal/reference workload or absolute CPU/GPU, pass, submission,
   bytes-moved, and work-amplification budget. Zero sustained native-display
   CPU fallback, per-draw synchronization, and iPad app termination are hard
   requirements rather than percentage budgets. Begin every connected-iPad
   shard only after a public nominal thermal probe; later thermal changes are
   observational unless they cause a test or measured performance-budget
   failure.
3. Validate the central manifest and patch inventory and record the current
   platform contracts. Copy the already-recorded CGL version/capset result into
   the evidence index as explicitly archival data. Do not rebuild, rerun, or
   preserve a CGL executable, prefix, preset, or CTS lane for comparison.
4. Define the atomic dependency transition: one `apple-gpu` membership, one
   builder, three platform prefixes, and no desktop libepoxy/virglrenderer
   graphics rows in the `macos` product. Prepare all target repositories once
   and verify them offline before any consumer switches.
5. Turn the existing host probes into maintained source and test targets. A
   probe must request ES 3.0, 3.1, and 3.2 separately and report every Virgl
   feature predicate, not only version strings.
6. Build common ANGLE and Metal capability probes. Run the macOS probe first
   on every iteration, then run the separately signed iOS probe on the iPhone
   17 Pro Max Simulator with the same schema. Do not attach to or replace a
   running Kernarium guest merely to obtain host capabilities.
7. Record the connected-iPad host and guest baseline needed for later
   qualification. Capture the exact guest desktop GL value on Simulator and
   the iPad; until then retain “GL 2.1 expected” rather than converting it to
   measured.
8. Record the current standard Virtio-GPU getparams and released-Mesa blob call
   sequence for possible use only if Phase 6 measurements select the
   blob/shared-region optimization or the separate Vulkan plan needs it. This
   inventory is not a GL profile-promotion commitment.
9. Document conditional kernel topic ownership for that optimization. Create
   or amend the UML shared-region/aperture and any generic `virtio_gpu` topic
   only after Phase 6 or the Vulkan plan selects the work; then record the
   required outside-port ownership, overlap/extraction, delta, and next-version
   policy updates.
10. Inventory VK-GL-CTS from the image, verify the release, executable build
   IDs, installed resources, registered modules, `mustpass.xml` files, leaf
   counts, and digests. Search read-only for an existing authoritative
   source/build receipt that proves the co-located source and provisioned
   binaries match. Do not reconstruct missing historical provenance. Store
   recovered identities only in ordinary repository evidence; do not edit the
   rootfs manifest or installed artifacts. If no authoritative receipt exists,
   keep CTS qualification blocked, record the gap, and ask the user before any
   image change.
11. Bring up the dedicated CTS driver with a software-renderer negative control:
   it must reject llvmpipe, unexpected Zink, Xvfb, a missing DRM device, a
   truncated QPA, and an incomplete shard rather than recording a pass.
12. After the atomic shared-path switch in Phase 1, run the identity sentinel
   and shipped GLES 2/GLES 3.0 baseline selections on macOS first, Simulator
   second, and the connected iPad last. Classify existing failures without
   waiving them.

Exit: macOS is the reproducible first-stage development baseline, the
Simulator adapter is ready to validate that exact path, and the connected iPad
has a recorded physical-device prototype baseline. The old CGL number exists
only in archived prose/evidence. The CTS identity/caselist baseline and current
source/build-receipt gap are recorded, and the campaign adapters reject
software rendering and incomplete artifacts on every product shell. This
preparation creates no second runnable macOS graphics lane.

Current status: the app-side campaign allow-list, page-specific generated
launcher, Simulator wrapper, and connected-iPad wrapper are implemented and
tooling-tested. The device wrapper requires a matching passing Simulator
`run.json` plus its QPA before it will launch, and the Simulator wrapper now
requires the matching passing native macOS gadgets run. The exact 16 KiB
sentinel passed 1/1 in three consecutive booted-Simulator runs and then three
consecutive connected-iPad runs, with matching renderer and selected-caselist
identities, guest/runtime status zero, and clean poweroff. Topic 0020 repaired
the first iPad run's intermittent dirty DRM teardown, and transcript validation
now fails closed on its signature or missing clean-stop markers. A later
ordered macOS/Simulator/iPad run captured explicit ES 3.0/3.1/3.2 attempts,
sorted extensions, full raw and decoded Virgl capsets, exact platform diffs,
and zero native CPU fallback on all three products. The broader fixed sentinels,
now comprising five GLES 2 and twelve GLES 3 cases, passed in strict order on
all three products under both page profiles. Selection waived none of its
three diagnostic findings. The current complete macOS matrix classifies the
depth-buffer result as permitted `NotSupported` on the five no-depth
configurations and passes it on the other 15; the repaired transform-feedback
query passes on all 20. The former Simulator-only GLES 2 texture failures are
repaired by arm64-Simulator swizzle and truthful 32-bit-float-filter support;
the complete current Simulator GLES 2 matrix passes all 9,460 executions.
Single fixed Aquarium
canaries now pass on macOS and Simulator under both page profiles, including
repaired-kernel runs with zero dropped or CPU-fallback frames. The current
20-config 16 KiB macOS GLES 2 and GLES 3 matrices now pass all 9,460 and
129,960 required executions. Fixed signed Simulator GLES 2 and GLES 3 config
inventories now match those macOS predecessors exactly at all 20 tuples and
canonical digest, with direct ANGLE/Metal presentation, clean shutdown, and
zero dropped or CPU-fallback frames. The first schema-1 complete-leaf GLES 3
Simulator layout was reproducibly OOM-killed after 2,255 passing results in
each of its first two configs under the fixed 1.5 GiB mobile guest. Schema 2
now compiles one full GLES 2 process and four signed, hash-pinned GLES 3
process slices per config; the aggregate reconstructs and hashes the exact
original leaf. The failed attempts remain immutable evidence. The subsequently
directed product policy uses 4 GiB on Simulator and starts physical-device runs
at 2 GiB; it still adds no swap and does not weaken the bounded process layout.
Fresh 4 GiB Simulator inventories now match the same 20 macOS tuples and
digest. Managed UTM WebKit topic `fff809d63b` enables native Metal sampler
comparison on modern arm64 Simulator after ANGLE's missing-family fallback was
shown to force all shadow comparisons to `Never`. The exact repaired GLES 3
`cfg00` fraction `0/4` passed all 1,625 executions (1,563 `Pass`, 62 permitted
`NotSupported`) with all eight formerly failing depth/stencil shadow-repeat
cases now `Pass` and zero CPU fallback.
The seven interleaved performance pairs,
connected-iPad thermal and sustained lifecycle baselines, complete sliced
Simulator GLES 3 matrix, and both connected-iPad matrices remain missing. The
final 4 GiB Simulator GLES 2 matrix now passes all 9,460 executions as recorded
above; it closes that API on Simulator without substituting for any remaining
gate.
The former global all-package caselist attempt still
fails at `dEQP-GL45-ES3` and remains diagnostic, but it no longer defines the
GLES qualification boundary. The filtered `KHR-GLES2`/`KHR-GLES3` hierarchy
inventory is complete and frozen at 6,972 cases. The missing original-image
receipt and remaining official matrices keep the exit open; no further rootfs
or guest-userspace change is required.

### Phase 1: atomically replace macOS graphics with the common Apple path

1. Introduce the fixed-layout ANGLE/Metal provider boundary. That fixed provider is
   fixed by the common Apple-GPU product contract, not a macOS runtime option.
2. Move EGL-specific state out of the public `VirglRenderer` layout and delete
   the CGL provider, OpenGL-texture native resource, CLI selector, presets, and
   provider-specific tests.
3. Make VNC availability depend on macOS, not on the selected renderer.
4. Replace compile-time scanout selection in `virtio_gpu_device.cpp` with the
   renderer's checked native-resource result.
5. Propagate the verified product-fixed ANGLE provider through the in-process
   service. Kernarium and macOS reject a product whose identical backend was
   not built and verified for all three platforms.
6. Preserve the renderer/vhost-user single-owner and reset ordering. EGL
   provider threading must be stated and tested rather than inferred.
7. Remove the backend-dependent test macro and add lifetime tests for the
   Metal-texture, no-native-handle, unsupported, and CPU-readback paths.
8. Add the common Apple-ANGLE product to `dependencies/manifest.tsv` by product
   membership, not by duplicating source rows or creating a local manifest.
   Generalize the GPU builder, platform contracts, verifier, and imported CMake
   targets together. In the same change, remove desktop libepoxy/virglrenderer
   rows/recipes/targets from the `macos` graphics graph and retire the
   `ios-gpu` entry point so it cannot drift from the shared recipe.
9. Test dependency failure behavior: an unprepared commit, dirty/mismatched
   prepared repository, reordered or hand-modified patch, stale receipt,
   wrong Mach-O platform, unexpected library/framework, or invalid existing
   prefix must fail without replacing the existing source or prefix.

Exit: every macOS GPU binary, test, and package links only the platform slice
from the common Apple-GPU product. The CGL provider/options/tests and desktop
graphics dependency rows/recipes/imported targets are absent, final-link
audits reject their return, and `check-gpu` exercises common ANGLE/Metal. The three
platform contracts select one semantic recipe, all prepared/build inputs are
offline and centrally declared, and negative dependency fixtures fail closed.
There is no phase exit in which both macOS graphics paths remain runnable.

### Phase 2: bring up and validate the shared ANGLE/Metal path

1. Factor the iOS provider, renderer configuration, capset policy, and
   native-resource contract into common device-owned code, then bring it up on
   macOS first without adding a macOS-only graphics branch.
2. Build the macOS slice through the generalized dependency GPU recipe from
   the exact central-manifest UTM repositories, full commits, sparse paths,
   and ordered patch series used by iOS. Do not copy a prepared tree into a
   Kernarium or macOS-local source area.
3. Enable EGL in the matched libepoxy build and configure UTM virglrenderer
   for its EGL/Metal resource-export path.
4. Request `EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE`, hardware device type, and an
   ES 3 context exactly as iOS does.
5. Bring up the host probe and Virgl capsets before booting a macOS guest.
6. Use VNC/CPU readback only for bounded bring-up diagnostics, then require
   AppKit direct `MTLTexture` presentation before performance qualification.
7. On every iteration, run host/unit/guest/CTS work on macOS first and the same
   workload on the iPhone 17 Pro Max Simulator second. Diff the full capsets
   and limits; an
   equal version string is not enough.
8. Run the phase qualification on the connected iPad. Simulator and macOS
   success cannot satisfy this exit gate.
9. Verify all three installed platform contracts and embedded provenance
   against the current central manifest/series, then verify each packaged app
   contains exactly the selected ANGLE frameworks once and no CGL or desktop
   graphics dependency.

Expected result: macOS reports the iOS-class GLES 3.0 ceiling and probably
desktop GL 2.1. This is a successful parity result, not a regression to hide.

Exit: macOS can use only the same ANGLE provider architecture, dependency
identity, EGL attributes, capset logic, and native-resource contract as iOS;
accepted native scanouts have zero unexpected CPU fallback; and the connected
iPad passes frozen absolute frame-pacing, host/guest CPU, memory, thermal,
direct-presentation, and lifecycle budgets. The shipped GLES 2 and GLES 3.0 Khronos
must-pass matrices have complete macOS-first, same-path Simulator, and connected-iPad
result sets: every required core case is `Pass`, and any other accepted status
is explicitly permitted by the exact Khronos criteria.

Current status: items 1 through 5 and the capset/context portion of items 7
through 9 are implemented. The accepted ordered 16 KiB diagnostic and both
page profiles' fixed identity sentinels request ES 3.0, 3.1, and 3.2 on each
host, capture all extensions and both complete Virgl capsets, decode every
known field, record exact cross-product differences, require direct
presentation, and report zero CPU fallback. ES 3.1 and 3.2
fail with `EGL_BAD_MATCH` on macOS, Simulator, and iPad, so no Phase 3 profile
work is authorized by this evidence. Single accepted-artifact Aquarium
canaries now pass on macOS and Simulator under both page profiles, after topic
0021 repaired the 4 KiB nested-TLB RCU starvation. They establish workload
bring-up and zero-fallback coverage only. The current 20-config 16 KiB macOS
GLES 2 and GLES 3 matrices pass all 9,460 and 129,960 required executions,
respectively. Complete Simulator/iPad matrices, the seven-pair performance
campaign, connected-iPad thermal gate, and sustained-lifecycle campaign remain
open.
The normally signed app now records public iOS thermal state at managed-run
start, on every state-change notification, and at clean finish. Its stable
markers are captured by the existing device transcript, providing the input
needed to audit the final repeated-shard campaign without a private API; no
thermal or lifecycle result is claimed until that campaign completes.
A fail-closed repository verifier now joins exactly one passed 20-launch GLES
2 device matrix and one passed 80-launch GLES 3 device matrix. It requires the
same signed app, sealed rootfs, wired device, 16 KiB profile, and final common
memory policy; authenticates every campaign/attempt/run/console input; requires
100 nominal preflight probes, ordered start/change*/finish lifecycles, and
clean shutdowns. It retains all later thermal samples as observations without
letting them reject an otherwise passing shard. This implements the evidence
gate but claims no result until both final device matrices and their
independent CTS reconstructions pass.
The original EROFS build receipt remains missing, the checked derivative owns
its authenticated build receipt, and the scoped GLES hierarchy identity is
complete. Qualification remains fail-closed on those remaining matrices and
performance/lifecycle gates rather than on caselist enumeration.

### Phase 3: evaluate direct ANGLE/Metal GLES 3.1

Run this as a performance feasibility gate before changing the advertised
profile. MoltenVK and ANGLE's Vulkan renderer are forbidden in this phase.

1. Extend only ANGLE's Metal backend for the missing ES 3.1 behavior. Map
   compute, SSBOs, image load/store, atomics, indirect draw/dispatch,
   framebuffer-without-attachments, program interfaces, barriers, limits, and
   formats to native Metal render/compute/resource operations.
2. Keep shader lowering and command generation GPU-oriented. Reject any design
   that interprets shaders on the CPU, expands ordinary draws on the CPU,
   synchronously reads query/image data back to decide later commands, or
   serializes the graphics queue for routine operation.
3. Request only `EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE` with the hardware device
   type. In a combined app MoltenVK may be loaded for Venus, so test the precise
   invariant: the GL facade has no MoltenVK target edge or function resolution
   and creates no Vulkan object, call, submission, or ANGLE Vulkan display.
4. Exercise a focused sentinel for each GLES 3.1/Virgl gate. Then run matched
   candidate-versus-Phase-2 A/B workloads for unaffected GLES 2/3.0 behavior.
   For compute/SSBO/image or another candidate-only API, compare with an
   equivalent direct-Metal reference and record CPU/GPU time, queue
   submissions, passes, bytes moved, and command/memory amplification against a
   frozen absolute budget.
5. Run the performance canaries on macOS first, repeat the same configuration
   on the iPhone 17 Pro Max Simulator, and run the short correctness,
   performance, memory, and thermal-observation canary on the connected iPad
   after a nominal probe. A correct but materially slower ES 3.1
   implementation is rejected; retain GLES 3.0.
6. After the short canaries pass, run the change-local CTS families, direct
   presentation/lifecycle checks, and the decisive sustained connected-iPad
   performance/thermal gate. Only after that gate selects the candidate run
   complete `dEQP-GLES31` diagnostic shards, every
   `gles31-khr-main.txt` configuration, and all lower claimed GLES lists.
   Post-CTS device work confirms stability; it does not defer the first
   performance accept/reject decision.

Exit: either the unmodified guest naturally reports a performant GLES 3.1 on
all three products, or the plan records GLES 3.0 as the final shared ceiling.
Promotion requires complete correctness evidence, direct ANGLE/Metal identity,
no forbidden CPU-heavy path, and every performance budget; CTS success alone
cannot exit the phase.

### Phase 4: evaluate GPU-resident desktop GL 3.0/3.1 gates

GLES 3.1 does not automatically produce desktop GL 3.x. Generate the exact
gate checklist from the guest Mesa already installed in `rootfs.erofs` and compare it with the Virgl
capset and ANGLE/Metal implementation. This phase is optional and stops at the
first gate without an efficient GPU-backed implementation.

For conditional rendering, accept only a Metal/ANGLE mechanism that retains
the predicate and draw decision on the GPU and implements WAIT, NO_WAIT,
BY_REGION, inverted modes, query lifetime, draw/clear/blit/indirect
interactions, reset, and context loss. CPU polling, per-draw readback, command
buffer round trips, or forced serialization are disqualifying. Apply the same
rule to every remaining GL 3.0/3.1 gate and required format.

Prefer truthful hardware-backed extension exposure over forcing a higher core
version. A useful Metal-backed compute/storage capability may remain available
while desktop core GL stays at 2.1 or 3.0 because an unrelated mandatory gate
is unavailable or too slow.

Run focused Piglit and conditional-render interaction sentinels, then the
matched macOS/Simulator/iPad performance canaries. For an operation absent
from the lower GL profile, compare an equivalent direct-Metal/reference
workload or the Phase 0 absolute cost and work-amplification budget. After the
short canaries pass, run change-local CTS, direct presentation/lifecycle
checks, and the decisive sustained connected-iPad performance/thermal gate.
Only after that gate selects the candidate run the cumulative GL 3.0/3.1 main
and GTF lists. If a gate is correct but violates the budget, do not advertise
it and keep the previous desktop version. Do not infer GL 3.1 from conditional
rendering alone.

Exit: either `glxinfo -B` naturally reports a performance-qualified GL 3.0 or
3.1 on macOS, Simulator, and connected iPad, or desktop GL 2.1 remains the
final shared ceiling. GLES behavior and performance must not regress.

### Phase 5: enforce the GLES/GL 3.2 and GL 4.x stop boundary

The selected ANGLE/Metal path lacks a direct geometry stage and is not a
desktop GL 4.x provider. Do not implement those profiles through MoltenVK,
Zink, ANGLE/Vulkan, CPU geometry expansion, shader interpretation, or
unbounded, CPU-driven, or materially slower multi-pass command rewriting.
Those paths conflict with the architecture and performance policy even if they
could raise a version string.

On an authenticated direct-Metal proof, ANGLE/Metal update, or Metal SDK
update, run a small capability probe and the existing next-profile sentinel.
Reopen the ceiling only if the new path maps the missing semantics to public
GPU hardware, remains common to macOS/Simulator/device, needs no sustained CPU
fallback, and passes the predeclared spike on macOS first, the iPhone 17 Pro
Max Simulator second, and the connected iPad last. Until then, GLES 3.2,
desktop GL 3.2+, and GL 4.x are explicitly out of scope rather than an open
implementation backlog.

Exit: documentation, capset tests, and product claims stop at the last
performance-qualified Phase 3/4 result; no higher profile is advertised.

### Phase 6: optimize the selected profile and transport

Optimize the chosen lower profile instead of spending CPU or GPU bandwidth on
a higher version:

1. Require direct same-device `MTLTexture` presentation for sustained native
   display workloads. Keep CPU readback bounded to diagnostics and optional VNC;
   it is not an accepted normal-frame path on Simulator or the connected iPad.
2. Reduce copies, queue submissions, state translation, shader compilation,
   and synchronization stalls. Preserve asynchronous fences and cache compiled
   Metal pipelines without unbounded memory growth.
3. Add standard coherent blob/shared-region support only when a measured
   workload shows that it reduces mapping or transfer overhead at the selected
   profile. Implement the complete host commands, maintained UML shared-region
   mapping, standard released-userspace DRM ABI, strict bounds/lifetime rules,
   and both page profiles before advertising it. Do not use blob support to
   justify GL 4.4-4.6 work.
4. Share only the bounded memory infrastructure with Venus. GL feature
   discovery, contexts, shaders, command submission, and presentation remain
   on ANGLE/Metal and never traverse MoltenVK.
5. Run the matched macOS-first, Simulator-second, connected-iPad-last
   performance campaign after every optimization. Reject an optimization that
   merely shifts substantial work from the GPU to the CPU or worsens measured
   frame pacing. Record device thermal state, but reject it only when the
   resulting run fails a test or frozen performance budget.

Exit: the selected profile has direct hardware rendering and presentation,
bounded memory, stable frame pacing, and measured performance on the connected
iPad under both page profiles where mapping-sensitive. The exit makes no GL
4.x or GLES 3.2 claim.

### Phase 7: product adoption and carried-patch maintenance

1. Ship only the shared ANGLE/Metal provider on macOS even if its initial desktop-GL
   ceiling is below the archived CGL measurement. Keep final-link and option
   tests that reject reintroduction of the removed desktop path.
2. Document macOS as the normal first development loop, the iPhone 17 Pro Max
   Simulator as the required same-change validation loop, and the connected
   iPad as the physical-device release gate.
3. Keep every ANGLE, virglrenderer, or related dependency
   protocol change inside this repository. Maintain it as a clean carried topic
   and regenerate its ordered mail series with
   `dependencies/rebase-patches.sh`; generated patch files are not editing
   authorities. Do not upload the topic, patch, or proposal to an external
   project. Topics must stay focused and removable if a later independently
   published source revision already contains equivalent behavior.
4. Carry required Linux changes in the maintained kernel series; inclusion in
   a later external Linux release is not a product gate. Author or amend
   commits in the
   configured case-sensitive checkout, use `kernel/mainline-6.18/scripts/rebase-patches.sh` to
   regenerate mail patches and identities, and update final-tree, delta,
   ownership/overlap/extraction, next-version, and `kernel/README.md` policy.
   Keep those Linux changes in this emulator repository and do not submit them
   externally.
5. Do not consume a different Mesa/libdrm or update `rootfs.erofs`. If the
   selected profile requires a different guest component, retain the lower
   profile, record the exact proposed image delta and reason, pause this phase,
   and ask the user before preparing or running any image workflow.
6. Promote a version separately for macOS, Simulator, and connected iPad, but
   do not qualify the common product profile until the connected iPad
   completes its acceptance matrix.
7. Record the current CTS release, executable build IDs, reconstructed source
   identity, resources, must-pass data, caselist digests, and immutable rootfs
   identity in ordinary repository evidence, not rootfs provisioning metadata.
   No further Mesa or CTS update is authorized beyond the checked derivative
   recorded above. Never compare totals from different case universes as if
   they were a regression A/B; if a different universe is required, pause and
   ask the user.

## Repository work map

| Area | Primary files | Planned responsibility |
| --- | --- | --- |
| Renderer selection | `darwin/device/gpu/CMakeLists.txt`, `virgl_renderer.hpp`, `virgl_renderer.cpp` | fixed-layout ANGLE/Metal EGL provider boundary, Metal native handle, provider validation, and removal of CGL code/options/tests |
| Device propagation | `in_process_service.*`, `vhost_user_server.*`, `virtio_gpu_device.*` | carry the product-fixed ANGLE/Metal result, reject configuration drift, select the checked presentation handle, and preserve reset/fence ownership |
| Displays | `display.cpp`, `display.hpp`, `native_display.mm`, `native_display_ios.mm` | separate VNC from renderer, share the Metal timeline, retain AppKit/UIKit shells, require direct native texture presentation, and bound CPU readback to diagnostics/VNC |
| macOS shared-path composition | `darwin/app/macos/main.cpp`, common-product and final-link tests | consume the product-fixed Apple-GPU backend with no renderer option; reject CGL, OpenGL.framework, and product drift before guest start |
| Kernarium composition | `Kernarium/Runtime/KernariumRuntime.mm`, runtime CMake | select the supported iOS backend without adding a platform-specific kernel or app API; update the canonical bridge ABI only if its declarations actually change |
| Common Apple GPU dependency identity | `dependencies/manifest.tsv`, `dependencies/prepare.sh`, `dependencies/rebase-patches.sh`, `dependencies/patches/*/series.tsv` | own exact Git repositories/full commits, sparse paths, product membership, and generated ordered mail-patch identity; no product-local manifests or fetches |
| Common Apple GPU build and verification | planned `dependencies/build-apple-gpu.sh`, `dependencies/platforms/*-gpu-arm64.tsv`, generalized Apple-GPU CMake importer/verifier | replace the iOS-only recipe with one macOS/Simulator/device ANGLE recipe; publish transactionally, install provenance, and verify exact payload/platform/closure without backend drift |
| macOS dependency cleanup | `dependencies/manifest.tsv`, `dependencies/build.sh`, `dependencies/verify-prefix.sh`, `dependencies/cmake/UmlThirdPartyTargets.cmake` | remove desktop libepoxy/virglrenderer graphics rows, recipes, and imported targets from the ordinary macOS prefix; make final-link audits reject CGL/OpenGL.framework |
| Product packaging | `Kernarium/scripts/package-ios-frameworks.sh`, `Kernarium/Kernarium.xcodeproj/project.pbxproj`, macOS bundle/runtime-closure verification | consume only verified prefix targets; embed each required framework exactly once and keep signing/rpath/lifecycle differences out of renderer configuration |
| Virgl and ANGLE topics | `dependencies/patches/utm-virglrenderer/`, `dependencies/patches/utm-webkit/` | use one production renderer series on all three products, add only truthful GPU-backed feature mappings, retire desktop-only graphics topics, and regenerate every carried series from commits with `rebase-patches.sh` |
| Conditional host blob optimization | `darwin/device/gpu/protocol.hpp`, `virtio_gpu_device.*`, renderer/resource boundary | only if Phase 6 measurements or Venus select it: standard feature/commands, bounded aperture ownership, map-info/cache semantics, and coherent lifetime |
| Conditional guest-kernel graphics optimization | maintained Linux `arch/um/drivers/virtio_uml.c`, UML physical/IOMEM/MM code as selected, `drivers/gpu/drm/virtio/virtgpu_*`, `include/uapi/drm/virtgpu_drm.h`, and common Kconfig policy | only after selection: implement shared-region discovery and mapping, backport/fix generic Virtio-GPU behavior only when needed, preserve the released-userspace DRM ABI, and share one runtime-negotiated kernel artifact per page profile across all Apple products |
| Kernel series and verification | `kernel/mainline-6.18/patches/series/`, `kernel/mainline-6.18/patches/metadata.env`, ownership/overlap/extraction/next-version policy, `kernel/README.md`, kernel tooling tests | own generated topic identity, reviewed final tree and delta, bridge-ABI updates only when required, offline replay, and LLVM 22 builds of both page profiles |
| Immutable rootfs CTS contract | original read-only `rootfs.erofs`, checked `vk_gl_cts_derivative_rootfs.json`, in-image CTS paths, and ordinary repository evidence | keep the original image untouched; accept only the exact sealed ext4 derivative authorized in the implementation checkpoint; attach it block-read-only with OverlayFS state; reject any identity, exclusion, receipt, or permission drift |
| CTS and guest tests | `tests/integration/gpu/run_vk_gl_cts.py`, `tests/unit/gpu/`, `tests/tooling/gpu/`, dependency/rootfs tests, `tests/test-manifest.tsv` | graphical-session launch, identity rejection, exact caselist/config sharding, QPA egress/parsing, backend lifetime, protocol validation, package shape, guest probes and rendered campaigns |
| Dependency tooling tests | `tests/tooling/dependencies/`, `tests/test-manifest.tsv` | keep `dependency-source-preparer`, `dependency-patch-regenerator`, `dependency-build-manifest`, `dependency-prefix-verifier`, and `dependency-runtime-closure` green; add Apple-GPU platform/payload/provenance/concurrent-publication fixtures |

New host GPU implementation sources must remain in `darwin/device/gpu/` and be
owned by its CMake target. Linux implementation changes remain in the
maintained kernel topic stack. Do not include implementation files into other
C++ sources.

## Verification matrix

### Host and unit gates

- rejection of any runtime GL-backend option and unsupported-platform product;
- fixed `VirglRenderer` and enclosing object size across all consumers;
- provider construction failure, partial cleanup, context sharing, make-current
  behavior, and same-thread teardown;
- EGL error draining and missing-entry-point handling;
- provider-graph, EGL-display, symbol-resolution, and GL-only workload tests
  proving capsets 1/2 terminate at ANGLE/Metal. A combined app may load
  MoltenVK solely for the separate Venus facade, so fail the GL path on any
  MoltenVK target edge/function resolution, ANGLE Vulkan display, Vulkan
  instance/device/queue/resource, or submission rather than on framework
  presence alone;
- execution-path classification and negative tests for CPU shader/raster/
  geometry work, resource-sized conversion, readback-driven decisions, and
  per-operation synchronous feedback;
- Metal-texture, dropped, unsupported, and bounded diagnostic/VNC readback
  presentation paths, plus a negative test rejecting sustained native-display
  fallback;
- ordinary resource/reset/connection-retry validation and, only if Phase 6 or
  Venus selects blob/shared-region work, malformed blob commands, overflow,
  duplicate IDs, and rollback;
- when that optional optimization is selected, host wire-structure/layout
  checks against pinned Linux Virtio-GPU definitions, kernel shared-region
  discovery and fail-closed feature negotiation, standard getparam/ioctl
  compatibility, aperture bounds, coherent visibility, and reset/unmap ordering
  under both page profiles;
- VNC lifetime and port reuse with the common ANGLE/Metal renderer; and
- dependency build receipt/platform contract, architecture, deployment target,
  install name, `LC_SUB_CLIENT`, rpath, embed phase, and runtime dynamic
  closure;
- central-manifest schema/product selection, prepared-repository cleanliness
  and metadata, ordered patch identity, installed-provenance byte comparison,
  stale-prefix refusal, and transactional publication; and
- proof that no deleted Kernarium-local manifest, build script, verifier, or
  ambient dependency lookup has re-entered the graph.

Run the narrow tests first, then:

```sh
dependencies/prepare.sh --validate-manifest-only
dependencies/prepare.sh --product macos
dependencies/prepare.sh --product apple-gpu

# The ordinary macOS prefix now contains non-graphics dependencies only.
dependencies/build.sh
dependencies/build-apple-gpu.sh --platform macos
dependencies/build-apple-gpu.sh --platform macos --verify-only
cmake --workflow --preset check-gpu
cmake --workflow --preset check-gpu-asan-ubsan

dependencies/prepare.sh --product ios-network
dependencies/build-ios-network.sh
dependencies/build-ios-network.sh --verify-only
dependencies/build-apple-gpu.sh --platform ios-simulator
dependencies/build-apple-gpu.sh --platform ios-simulator --verify-only
cmake --preset kernarium-ios-simulator
cmake --build --preset kernarium-ios-simulator
cmake --preset kernarium-ios-simulator-4k
cmake --build --preset kernarium-ios-simulator-4k

dependencies/build-apple-gpu.sh --platform ios
dependencies/build-apple-gpu.sh --platform ios --verify-only
dependencies/build-ios-network.sh --platform ios
dependencies/build-ios-network.sh --platform ios --verify-only
cmake --preset kernarium-ios-device
cmake --build --preset kernarium-ios-device
cmake --preset kernarium-ios-device-4k
cmake --build --preset kernarium-ios-device-4k
Kernarium/scripts/package-ios-frameworks.sh
```

Whenever a maintained Linux graphics topic changes, regenerate and validate it
through the configured case-sensitive source rather than editing patch files:

```sh
kernel/mainline-6.18/scripts/rebase-patches.sh
kernel/mainline-6.18/scripts/prepare-source.sh --verify
kernel/mainline-6.18/scripts/audit-delta.sh --tree HEAD
kernel/common/abi/sync.sh --verify
env UML_KERNEL_CC=/opt/homebrew/opt/llvm/bin/clang \
  UML_KERNEL_HOST_CC=/opt/homebrew/opt/llvm/bin/clang \
  UML_KERNEL_LD=/opt/homebrew/bin/ld.lld \
  kernel/mainline-6.18/scripts/verify-replay.sh --work-root /Volumes/<case-sensitive-volume>
cmake --workflow --preset check-integration
cmake --workflow --preset check-integration-4k
```

The replay must build both profiles; do not use `--skip-build` for promotion.
Ordinary Virtio-GPU/DRM changes do not advance the separate `darwin_uml_*`
bridge ABI. If a canonical bridge declaration actually changes, update its
version, fingerprint, identity/history, generated copies, kernel topic, and
undefined-import checks according to `kernel/common/abi/README.md` before running the
same verification.

Phase 1 must make these common interfaces directly runnable. The command order
is intentional: build and test macOS first, validate the same revision and
recipe on Simulator second, then build and qualify the connected iPad. The
retired `build-ios-gpu.sh` must not remain as an independently maintained
compatibility recipe. `check-gpu` and its sanitizer variant switch atomically
to common ANGLE/Metal and final-link checks prove that CGL, OpenGL.framework, and the
desktop virglrenderer/libepoxy family are absent. An iOS device build alone is
not device evidence. Preserve the architecture audit and test-manifest
ownership.

### Guest API and function gates

For the fixed ANGLE/Metal provider on every claimed Apple platform:

- verify `/dev/dri/card0` and `/dev/dri/renderD128`;
- capture capsets 1 and 2;
- run `eglinfo -B -p gbm`, `glxinfo -B`, and explicit ES context creation;
- assert `virgl` and ANGLE/Metal identity, never llvmpipe or guest Zink;
- compile and execute focused tests for every newly advertised feature family;
- test robustness, reset, context loss, allocation failure, and repeated
  create/destroy cycles; and
- run change-local dEQP/KHR groups on every implicated change, deterministic
  module shards for gap discovery, and the complete matching `mustpass.xml`
  matrix before changing the claimed version.

Version strings are discovery evidence only. A shader compile, dispatch,
render/readback, barrier, and negative-limit test must cover each promoted
feature family. During development run these probes on macOS first, then the
same shared-path Simulator checks. Before promotion, run the complete agreed subset
on the connected iPad. Verify QPA identity, complete exact-case union, and
artifact integrity before interpreting status totals.

### Display and desktop gates

- deterministic framebuffer damage over VNC;
- UIKit direct presentation on the iPhone 17 Pro Max Simulator;
- UIKit direct presentation and lifecycle qualification on the connected
  iPad;
- AppKit native ANGLE presentation with no OpenGL-framework dependency;
- direct same-device Metal-texture presentation with zero unexpected CPU
  fallback for ordinary native-display workloads;
- normal KDE/Phosh compositor startup and clean shutdown;
- WebGL 1 and WebGL 2 functional pages;
- WebGL Aquarium only on macOS and Simulator; and
- the connected-iPad desktop boot, starting at 2 GiB, with the agreed GL
  probes.

### Performance gates

Use macOS as the primary iterative measurement environment, Simulator as the
required iOS validation environment, and the connected iPad for mandatory
final stability, thermal, memory, and performance evidence. macOS measurements
speed iteration but are not an iOS performance proxy. The archived CGL number
is context only and is never rerun as a product-selection gate.
Keep the guest, page profile, CPU count, memory, display geometry, and workload
fixed, and record raw frame statistics, host/guest CPU, memory, shader-compile
behavior, CPU fallback, and shutdown status.

Use matched interleaved A/B runs between the candidate and the immediately
lower truthful ANGLE/Metal profile. The order for each milestone is renderer
identity, a focused correctness sentinel, a macOS performance canary, the same
Simulator canary, a short connected-iPad performance/thermal canary,
change-local CTS, direct presentation/lifecycle checks, the decisive sustained
connected-iPad performance/thermal gate, and only then the full promotion CTS.
Post-CTS device work confirms stability and packaging rather than making the
first performance decision. This prevents a large conformance campaign from
legitimizing an obviously slow implementation. Diagnostic or sanitizer builds
are not score evidence.

Apply the Phase 0 budgets without averaging away a failing primary workload:

- unaffected lower-profile workloads lose no more than 3% throughput and add
  no more than 3% frame time or measured resource cost;
- candidate-feature median throughput decreases or median frame time increases
  by no more than 5%, while p95 frame time, host/guest CPU time, GPU
  time/energy, and resident memory increase by no more than 10%; improvements
  are unrestricted, and measurement uncertainty must be sufficiently below the
  applicable threshold or the result is inconclusive. Candidate-only behavior
  uses the frozen equivalent direct-Metal/reference workload or absolute cost
  and work-amplification budget rather than an undefined lower-profile A/B;
- ordinary native display has zero sustained CPU readback, no per-draw or
  per-dispatch synchronous feedback, and no CPU execution whose work scales
  with vertices, primitives, pixels, samples, invocations, texels, or resource
  bytes; and
- every connected-iPad shard starts only after a nominal thermal probe; later
  state changes are recorded but do not independently reject a passing run.
  Jetsam/app termination, lifecycle failure, or thermal effects that cross a
  frozen performance threshold remain failures.

An inconclusive or noisy result does not promote the profile. CTS success
never overrides a failed performance gate; retaining the lower version is the
preferred outcome.

## Risk register and stop conditions

| Risk | Mitigation or stop condition |
| --- | --- |
| shared ANGLE lowers macOS from the archived GL 4.1 result toward GL 2.1 | make the change explicit in product strings and tests and accept the lower macOS ceiling; do not preserve or rerun the old provider |
| renderer macro changes object layout again | use one opaque provider pointer and one public layout; compile-time assertions and cross-target consumer tests |
| Simulator or macOS evidence is mistaken for device proof | require the full connected-iPad campaign for every promoted capability; no substitution |
| a GL target or runtime path accidentally reaches Vulkan, Venus, MoltenVK, or Zink | fail the provider graph, EGL-display, symbol-resolution, capset, and GL-only workload tests; the only valid route is Virgl to ANGLE/Metal to Metal |
| native Metal texture export is unavailable for an ordinary scanout | do not silently use sustained CPU readback; fix the direct path, narrow the supported format/mode, or retain the lower profile |
| a forced cap produces a higher string but broken behavior | prohibit overrides; gate every capability on functional and conformance tests |
| software rendering or a truncated CTS shard creates a false pass | reject unexpected renderers before execution; require QPA identity/trailer, exact caselist union, artifact digest, clean process exit, and clean guest shutdown |
| full CTS exhausts the physical-device guest | start at 2 GiB; preserve the failed attempt, reduce only by 64 MiB after each confirmed emulator OOM, run one bounded shard per fresh guest, export artifacts immediately, disable deliberate OOM/image logging, and treat OOM or app termination as failure rather than a skip |
| a conformant feature silently executes workload-proportional work on the CPU | classify the data path before promotion and reject CPU shader/raster/geometry/format work, readback-driven decisions, and per-operation round trips |
| GPU lowering requires excessive passes, command amplification, or memory traffic | fail the frozen performance budget and retain the lower profile even when CTS passes |
| a higher profile regresses ordinary lower-profile workloads | require matched interleaved A/B canaries before full CTS and reject any repeatable regression beyond the frozen threshold |
| blob mappings leak or outlive guest memory | bounded ownership table, exact unmap/reset ordering, ASan/UBSan stress, both page profiles |
| a private DRM ioctl, capset, or winsys patch silently makes the rootfs custom | stop the promotion; the exact installed Mesa/libdrm must use the standard ABI naturally, while any repository-specific extension remains negotiated below the userspace boundary |
| the current image lacks required Mesa/libdrm behavior, a usable CTS WSI/configuration path, CTS coverage/provenance, or another guest component | retain the lower profile, record the exact blocker and proposed image delta, pause the affected phase, and ask the user; outside the exact checked derivative already authorized, never rebuild, provision, preload, or overlay-substitute guest userspace |
| blob support is advertised before UML exposes a valid host-visible aperture | require `RESOURCE_BLOB` and `HOST_VISIBLE` getparams, map-info, coherent round trips, and fail-closed reconnect/reset tests before the feature bit or capset gate is enabled |
| a 4 KiB guest mapping aliases another subpage in a 16 KiB host slot | validate whole-slot ownership, offset bounds, remap/TLB visibility, and teardown under both page profiles on macOS, Simulator, and the connected iPad |
| cached map-info is claimed without real CPU/GPU coherence | require bidirectional persistent-map visibility without transfer commands; otherwise report a truthful noncoherent/unsupported mode |
| a kernel patch is hand-edited, stale, or loses ownership metadata | edit the configured Git topic, regenerate with `kernel/mainline-6.18/scripts/rebase-patches.sh`, update final-tree/delta/ownership policy, and require clean replay plus both LLVM 22 page-profile builds |
| a dependency or kernel change is accidentally sent outside this repository | keep prepared sources remote-less, provide no push/submission step, review only generated carried series in this repository, and fail documentation/tooling checks on outbound workflow instructions |
| a future GLES/GL 3.2+ or GL 4.x proposal lacks a direct efficient ANGLE/Metal route | keep it outside the product plan; a version string is not a reason to add a Vulkan provider or heavy emulation |
| dynamic frameworks weaken dependency provenance | exact allowed framework identities and rpaths; no ambient lookup or global verifier relaxation |
| the new common GPU product duplicates or bypasses the refactored dependency authority | one central manifest and prepare step; extend product membership, common builder, platform contracts, CMake verifier, and packaging atomically; reject any Kernarium-local manifest/fetch |
| a hand-edited or stale dependency patch produces an unreviewable prepared tree | edit commits in a clean topic branch, regenerate with `dependencies/rebase-patches.sh`, validate the complete manifest/series inventory, and refuse mismatched prepared repositories |
| atomic removal leaves a hidden desktop graphics consumer | delete the desktop libepoxy/virglrenderer rows, recipes, imports, CGL provider/CLI/tests, and OpenGL-texture bridge together; make source, File API, link-map, and runtime-closure negatives reject each one |
| a combined GL+Vulkan product obscures provider isolation | record `gl_provider=angle-metal` and `vk_provider=venus-moltenvk` separately and prove no GL-facade target edge, EGL choice, function resolution, capset 1/2 behavior, or resource path touches MoltenVK |
| GN, Xcode, submodules, or another transitive tool fetches undeclared source | list every transitive source/commit/path centrally, run only `prepare.sh` with network access, and test builders in an offline environment |

## Completion criteria

The plan is complete only when all of the following are true:

1. macOS can build, test, and ship only the same ANGLE/Metal provider architecture as iOS,
   including source identity, patches, EGL attributes, capset logic, and
   native-resource contract. No CGL provider, CLI choice, preset, test lane,
   desktop graphics dependency row/recipe/target, OpenGL.framework link, or
   fallback exists.
2. The product-fixed ANGLE/Metal provider no longer changes GPU object layout or controls
   VNC/UI platform ownership, and macOS cannot select a provider independently
   of the two iOS slices.
3. The three Apple products consume authenticated slices from one reviewed
   host-graphics source matrix, one common GPU recipe, and platform-specific
   contracts verified against the same central manifest and ordered series.
4. Every required Linux graphics change is carried in the generated maintained
   kernel series, replays to the reviewed final tree, builds both canonical
   page profiles with LLVM 22, and yields the same per-profile kernel artifact
   for macOS, Simulator, and device. The standard Virtio-GPU/DRM ABI remains
   usable by the guest userspace already installed in `rootfs.erofs`; no private userspace dependency is
   introduced.
5. Every claimed GL/GLES version arises naturally from the exact unmodified
   guest Mesa already installed in `rootfs.erofs` and passes both the matching functional/conformance gate and the
   frozen matched performance budget. No claimed feature uses prohibited
   CPU-heavy software emulation.
6. Ordinary AppKit/UIKit presentation uses the direct same-device Metal texture
   path with zero unexpected CPU fallback. Bounded readback is separately
   labeled as diagnostic/VNC behavior and cannot qualify native performance.
7. The provider graph and runtime evidence prove
   `VirglGlFacade -> AngleMetalHostGlProvider -> Metal`. A combined Vulkan
   product may contain a separate `VenusVkFacade -> MoltenVK -> Metal`, but no
   GL target edge, EGL display selection, function-pointer resolution, capset
   1/2 behavior, or GL resource path touches it.
8. Select the GLES ceiling independently as GLES 3.0 or 3.1 and the desktop
   ceiling independently as GL 2.1, 3.0, or 3.1; each is the highest candidate
   that passes every correctness and performance gate. Retaining a lower
   profile is successful completion; GLES/GL 3.2+ and GL 4.x remain unclaimed
   unless a later direct, efficient ANGLE/Metal path causes this plan to be
   explicitly revised.
9. macOS is the documented first development loop; the same revision then
   passes the iPhone 17 Pro Max Simulator validation loop, and every final
   claimed profile completes the required campaign on the connected iPad. Run
   the full must-pass matrix under the default 16 KiB
   profile; repeat it under 4 KiB for page-, resource-, mapping-, or
   blob-sensitive changes, and otherwise run the fixed 4 KiB sentinel and
   regression selection.
10. Every required CTS configuration and shard completes exactly once against a
   frozen source/build receipt and binary/resource/caselist identity; its
   verified union has no missing cases. Every required core case is `Pass`;
   any other accepted status is explicitly permitted by the exact Khronos
   criteria. Required connected-iPad shards run under both profiles for
   mapping-sensitive milestones.
11. `check-gpu` and `check-gpu-asan-ubsan` exercise common ANGLE/Metal and reject the
   removed desktop path; dependency verification, the four
   GPU-bearing Kernarium runtime presets,
   `Kernarium/scripts/package-ios-frameworks.sh`, architecture audit, and the
   selected guest campaigns pass. Run the independent public-library
   `scripts/build-xcframework.sh` gate only when that package graph is affected;
   it is not evidence for ANGLE/Kernarium packaging.
12. The dependency workflow passes manifest/series validation, offline
   prepared-source verification, all three prefix verifiers, installed
   provenance comparison, package/runtime-closure checks, and negative tests
   for stale or wrong-platform inputs. This includes exactly one Apple-GPU
   prefix for macOS, Simulator, and device; no legacy graphics prefix remains.
   No deleted Kernarium-local dependency authority or archive-preparation path
   has returned.
13. `darwin/README.md`, `dependencies/README.md`, `kernel/README.md`,
   `docs/testing.md`, the Virgl implementation plan, feasibility report, kernel
   patch evidence, and evidence index are updated to distinguish current state,
   experiment, and accepted product support.
14. No workflow submits or uploads changes outside this emulator repository.
    Every third-party host change and Linux change is reproducible from the
    checked-in dependency/kernel patch series, while the exact installed guest
    Mesa/libdrm and qualification CTS remain unchanged.
15. No workflow writes, rebuilds, reprovisions, replaces, or supplements
    `rootfs.erofs` or edits its provisioning manifest for this work. The one
    user-authorized derivative described by the implementation checkpoint is
    independently authenticated, sealed, and mounted as a read-only lower
    filesystem; it does not authorize an overlay or preload substitution of a
    guest graphics library, CTS resource, runner, or WSI component. Any later
    guest-userspace need leaves its phase blocked until the plan reports the
    exact delta and asks the user for explicit direction.

The truthful evidence statement is: an older macOS CGL build measured guest
GL 4.1/GLES 3.0 and is archival only. The current common ANGLE/Metal path has a
focused 16 KiB macOS diagnostic and matched signed-app Simulator/iPad
diagnostics measuring guest GL 2.1/GLES 3.0; the same exact frozen GLES 3 case
passes 1/1 on all three products. Its fixed five-case GLES 2 and twelve-case
GLES 3 identity sentinels also pass on macOS, Simulator, and iPad under both
16 KiB and 4 KiB guest profiles. The first iPad poweroff's dirty DRM teardown
was repaired in maintained kernel topic 0020 and followed by three clean
Simulator plus three clean iPad repetitions, with fail-closed transcript
validation. Topic 0021 separately preserves precise Darwin nested TLB ranges
after a 4 KiB Aquarium run exposed repeated full-address-space invalidation and
RCU starvation; a fresh replay, both LLVM 22 builds, accepted artifacts, and
clean macOS/Simulator canaries under both page profiles validate the bounded
repair. The current 20-config 16 KiB macOS GLES 3 matrix completes all 129,960
executions with 125,270 `Pass`, 4,690 Khronos-permitted `NotSupported`, and no
disallowed status. The matching current GLES 2 matrix completes 9,460
executions with 8,760 `Pass`, 700 Khronos-permitted `NotSupported`, and no
disallowed status. Fixed generated signed-app inventories subsequently proved
that the iOS 26.5 Simulator exposes the same exact 20 config tuples and digest
for both APIs while using Gadgets, direct ANGLE/Metal presentation, and the
sealed read-only lower; both runs returned zero with no dropped or
CPU-fallback frames. The current Simulator GLES 2 campaign at
`.build/vk-gl-cts-ios-simulator-matrix/gles2-16k-ce8cfa0f-apple8-restored-4g`
also completes the same 9,460 executions and aggregate status totals, with an
independent 20-shard verify-only reconstruction. A fixed 100-ID schema-2
authority now retains that full GLES 2 process geometry while splitting GLES 3
into four signed bounded processes per config after complete-leaf Simulator
runs reproducibly exhausted the former fixed 1.5 GiB guest. Current adapters
fix Simulator runs at 4 GiB and start physical-device runs at 2 GiB, with only
64 MiB post-OOM device decrements. The mobile adapters and immutable
orchestrator reconstruct the frozen leaf from every slice. Simulator campaigns
may deterministically use as many as four unique booted devices with at most
one active shard on each; physical-device campaigns remain single-worker.
Fresh 4 GiB inventories pass for both APIs. Topic `fff809d63b` enables
native sampler comparison on modern arm64 Simulator, and the exact GLES 3
`cfg00` fraction `0/4` passes 1,625/1,625 with the eight former shadow-repeat
failures repaired and zero CPU fallback. The complete Simulator GLES 3 and
iPad case matrices, absent seven-pair
performance/connected-iPad thermal/sustained-lifecycle campaigns, and missing
original-image receipt still prevent any new shared capability or conformance
claim. The derivative
build has its own authenticated receipt and passed the fixed macOS identity
sentinels under both page profiles plus the one-case Simulator declaration
sentinel under both profiles; that bounded evidence does not satisfy
qualification. Its filtered display-backed `KHR-GLES2`/`KHR-GLES3` hierarchy
inventory is now complete at 6,972 cases and a frozen digest. The separate
all-package diagnostic still stops at the desktop-GL-dependent
`dEQP-GL45-ES3`, but that package is outside the GLES baseline scope and its
partial result is not promoted. The committed shared target
remains the direct-hardware ANGLE/Metal GL 2.1/GLES
3.0 baseline candidate and becomes the performance baseline only after Phase
2 passes. GLES 3.1 and desktop GL 3.0/3.1 are conditional stretch ceilings,
and GLES/GL 3.2+ plus GL 4.x are not planned claims.
