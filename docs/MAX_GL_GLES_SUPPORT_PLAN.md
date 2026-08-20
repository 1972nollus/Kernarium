# Maximum guest GL and GLES support through Virgl on Apple hosts

Written 2026-08-20. This is an implementation plan, not a capability or
conformance claim. It incorporates the measured results in
[GPU_BACKEND_FEASIBILITY.md](GPU_BACKEND_FEASIBILITY.md), the current Virgl
implementation in
[VIRTIO_GPU_VIRGL_IMPLEMENTATION_PLAN.md](VIRTIO_GPU_VIRGL_IMPLEMENTATION_PLAN.md),
and the additional requirement that guest graphics libraries remain
unmodified upstream releases.

Dependency-management assumptions were revised after the 2026-08-20
centralization under `dependencies/`; paths and workflows in this document
refer to that post-refactor ownership model.

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
- Treat truthful GLES 3.1 as the first product target. Prove ANGLE's Vulkan
  backend over MoltenVK on macOS first, validate the same implementation on
  Simulator, and qualify it on the connected iPad. Upstream ANGLE does not
  currently list its Vulkan backend as supported on macOS or iOS, so this is a
  real Apple-platform port rather than a display-selector change.
- Treat GLES 3.2 and desktop GL 4.6 as separately funded maximum-ceiling
  projects. GLES 3.2 needs geometry-stage emulation on MoltenVK/Metal. GL 4.4
  through 4.6 additionally needs coherent Virtio-GPU blob resources and a
  truthful desktop OpenGL implementation beyond the archived macOS GL 4.1
  measurement, implemented on the same provider on every Apple product.

The recommended development order is therefore:

```text
retain the already-recorded CGL result as archived evidence only
    -> replace every Apple product with one ANGLE provider/build product
    -> develop and test the exact shared path on macOS first
    -> validate the same change on the iPhone 17 Pro Max Simulator
    -> qualify the milestone on the connected iPad
    -> qualify truthful guest GLES 3.1
    -> optionally emulate the missing desktop-GL 3.x gates
    -> separately fund geometry emulation and coherent blobs for the maxima
```

## Scope and invariants

This plan keeps the guest API path:

```text
guest application
    -> released upstream Mesa Virgl Gallium
    -> upstream virtio_gpu DRM/KMS
    -> UML Virtio/vhost-user transport
    -> VirtioGpuDevice
    -> virglrenderer
    -> common Apple host GL provider
    -> Metal presentation
```

The following are non-negotiable:

1. iOS is the primary product. Renderer and protocol choices must satisfy iOS
   public-API, signing, sandbox, lifecycle, and 1.5 GiB memory constraints
   first. macOS is the fast development host for that design and may not add
   desktop-only graphics assumptions.
2. Do not patch or replace guest Mesa privately. A rootfs update may consume a
   released, unmodified upstream Mesa. Development images carrying an
   unmerged upstream candidate must be labeled custom and cannot satisfy the
   release gate.
3. Never use `MESA_GL_VERSION_OVERRIDE`, `MESA_GLES_VERSION_OVERRIDE`, forced
   Virgl capability bits, or extension-string injection as evidence. A version
   rises only when natural capset discovery and functional tests agree.
4. Preserve the real Virtio-GPU/Virgl path. Do not substitute an app-owned
   framebuffer or Chromium-specific library preload.
5. Preserve one common 4 KiB/16 KiB guest-kernel source and the existing
   platform-neutral device ownership. Backend selection must not create a
   platform-specific Linux image or public ABI.
6. Keep the shared ANGLE/Metal path and any ANGLE/Vulkan experiment in
   authenticated project-private dependency prefixes. Do not build, link, or
   package CGL or a desktop-only virglrenderer/libepoxy graphics family. Do not
   fall back to Homebrew or ambient Vulkan, EGL, GL, or `pkg-config`
   installations.
7. Keep iOS App Store compatibility: public APIs only, ordinary signing, no
   JIT entitlement, no private Metal API, and no helper process required by
   the graphics backend.
8. Report macOS, Simulator, and connected iPad separately. macOS is the first
   development stage, Simulator is the required iOS validation stage, and the
   connected iPad is the mandatory physical-device qualification stage.
9. Do not run WebGL Aquarium on the connected iPad. Use its normal 1.5 GiB
   desktop boot and the agreed focused graphics/qualification workloads.

Hardware video encode/decode, a new guest display server, and a Vulkan guest
API are outside this file. The Vulkan transport and memory plan is in
[MAX_VULKAN_SUPPORT_PLAN.md](MAX_VULKAN_SUPPORT_PLAN.md); the two plans may
share coherent-blob infrastructure but retain separate acceptance claims.

## Baseline and target ladder

The values below distinguish measured state, a near-term target, and an
architectural maximum. “Expected” means the capset predicates imply the value
but a current guest probe has not yet captured it.

| Host renderer | macOS guest | Simulator guest | Connected-iPad guest | Status |
| --- | --- | --- | --- | --- |
| Apple CGL 4.1 | GL 4.1, GLES 3.0 | unavailable | unavailable | archived pre-migration measurement only; no planned build, runtime, or test lane |
| Current UTM ANGLE/Metal | GL 2.1 expected, GLES 3.0 target | GL 2.1 expected; host ES 3.0 measured and guest WebGL 2 workload passed; direct guest GLES probe pending | GL 2.1 expected, GLES 3.0 source-derived | existing iOS path and required shared production path; macOS slice not yet built |
| ANGLE/Vulkan/MoltenVK | GL 2.1 expected, GLES 3.1 target | same target | same target | unproven Apple composition; macOS-first development, Simulator validation |
| Previous row plus complete desktop GL 3.1 gate emulation | GL 3.1 target, GLES 3.1 | same | same | host virglrenderer work; connected-iPad qualification required |
| Geometry-capable Vulkan/Metal translation | at least GL 3.2 and GLES 3.2 | same | same | large geometry-emulation project |
| Complete desktop GL provider plus coherent blobs | GL 4.6, GLES 3.2 architectural maximum | same architectural protocol maximum | same architectural protocol maximum | absolute Virgl target, not a committed product claim |

Important limits behind the table:

- ANGLE's Metal backend currently has
  [`kMaxSupportedGLVersion = 3.0`](https://chromium.googlesource.com/angle/angle/+/1da7e3afb21d4f1f3e5a9615b8673c15288f8727/src/libANGLE/renderer/metal/mtl_common.h),
  and [ANGLE's own platform support table](https://chromium.googlesource.com/angle/angle/+/main/README.md#platform-support)
  marks Metal complete through ES 3.0, not ES 3.1.
- The shipped Simulator ANGLE build refuses explicit ES 3.1 and ES 3.2
  contexts. Changing macOS to that exact path aligns the version ceiling; it
  does not increase it.
- The macOS MoltenVK probe reports the headline compute, SSBO, image, and
  atomic functionality needed for a GLES 3.1 feasibility target, but reports
  `geometryShader = VK_FALSE`. Every ANGLE gate still needs a per-platform
  probe, and the Vulkan backend cannot naturally expose GLES 3.2 on that
  device.
- Unchanged virglrenderer does not advertise conditional rendering on a GLES
  host lacking `GL_NV_conditional_render`. Released Mesa consequently cannot
  derive desktop GL 3.0 even when the same host reaches GLES 3.1.
- Virgl's legacy resource path can reach GL 4.3. Mesa requires coherent blob
  support for the buffer-storage gate used by GL 4.4 through GL 4.6, matching
  the upstream project's published
  [Virgl capability and blob-resource summary](https://www.collabora.com/news-and-blog/blog/2025/01/15/the-state-of-gfx-virtualization-using-virglrenderer/).

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
product-fixed GL provider:  ANGLE Metal | ANGLE Vulkan
thin UI shell:              UIKit (Simulator/device) | AppKit (macOS)
optional output consumer:   native view | VNC view (macOS only)
resource result:            Metal texture | CPU readback | unsupported
```

Prefer a fixed-layout `VirglRenderer` with an opaque provider object rather
than another public layout-changing macro. Candidate internal types are:

```text
HostGlProvider
    AngleHostGlProvider       (Metal or Vulkan EGL display attributes)

NativeResource
    MetalTextureResource
```

The provider owns context creation, sharing, current-context state, EGL
cleanup, and native-resource discovery. `VirglRenderer` retains protocol
submission, capsets, resource lifetime, and fences. All callers see one class
layout, and the provider remains in the device layer.

If the Vulkan plan is implemented too, its renderer session becomes the sole
owner of virglrenderer's process-global initialization and this provider lives
under the session's GL facade. Do not create independent GL and Venus owners
that each call global init/cleanup.

Each accepted or experimental Apple-GPU product contract selects exactly one
ANGLE backend for macOS, Simulator, and device. A backend experiment therefore
builds and tests the same product-wide configuration on all three platforms;
macOS has no runtime selector or private provider choice that can diverge from
iOS. CGL has no enum, option, preset, imported target, or compiled fallback.
An incomplete platform slice or unsupported common backend fails dependency
verification before the guest starts.

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
- full CPU fallback after an unsupported direct resource;
- existing `UML_GPU_FRAME_STATS` accounting; and
- no Objective-C types in platform-neutral headers.

Keep thin platform shells:

- AppKit retains window creation, main-thread event loop, keyboard, pointer,
  wheel input, window title, and VNC availability;
- UIKit retains the application-provided `MTKView`, touch/text integration,
  safe-area sizing, and lifecycle;
- ANGLE backends deliver the native Metal texture when their resource-export
  contract supports it.

If retained, VNC is only a presentation consumer of the common renderer's CPU
readback/damage stream. It cannot create a context, select a provider, change a
capset, or fall back to desktop GL.

Bring up every new renderer over CPU readback first. Direct presentation is a
separate optimization gate, not a prerequisite for proving GL correctness.
ANGLE/Vulkan must not reuse UTM's ANGLE/Metal native-share flag unless a probe
proves that it returns a valid same-device Metal texture.

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
source produced the ELF files; Phase 0 must recover or recreate a build receipt
before treating them as a matched qualification set.

### Installed artifacts and their roles

| Guest path | Identity | Intended use |
| --- | --- | --- |
| `/opt/vk-gl-cts/gles2/deqp-gles2` | AArch64/Linux PIE, 11,118,432 bytes, build ID `d2f695f3ac1ed9e7d4b85b2ff19326ce5cce3f1a` | fast GLES 2 functional and regression groups |
| `/opt/vk-gl-cts/gles3/deqp-gles3` | AArch64/Linux PIE, 17,923,136 bytes, build ID `54febf8ac17f594ca10ec7523a3e91af5d6f1e69` | fast GLES 3.0 functional and regression groups |
| `/opt/vk-gl-cts/openglcts/glcts` | AArch64/Linux PIE, 65,486,984 bytes, build ID `884796d1a220663afb5b79d1e96f622b3c1e7e14` | Khronos GLES 2/3/3.1/3.2 and desktop GL 3.0 through 4.6 promotion gates |
| `/home/uml/vk-gl-cts-1.4.5.5` | co-located 1.4.5.5-labeled source; no `.git` directory | read-only test-logic and expected-result derivation, failure localization, and preparation of upstream CTS fixes |

The resource trees beside each executable are part of the artifact. Run from
the executable's directory or pass that directory through
`--deqp-archive-dir`; never copy only the executable or mix a new executable
with old resources and caselists. The image has no `deqp-runner` or
`cts-runner`, so the repository must own the campaign orchestration rather than
silently depending on an ambient package. The source does define upstream
`cts-runner`; add that exact target to the next authenticated rootfs build and
prefer it for official GL/GLES must-pass configuration enumeration. Continue
to use `glcts` for exact-case and diagnostic fraction runs. Until the upstream
runner is provisioned, differential-test the repository's configuration
expansion against a disposable build of that runner before accepting a
promotion result.

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

At Phase 0, export the binary's complete text caselist and record its digest,
case count, registered modules, executable build ID, and the digest of each
selected must-pass leaf. `glcts` should expose `KHR-GLES2`, `KHR-GLES3`,
`dEQP-GLES31`, `KHR-GLES31`, `KHR-GLESEXT`, `KHR-GLES32`, and `KHR-GL30`
through `KHR-GL46`. Fail closed if the binary and installed lists disagree.
Do not compare raw pass counts across different CTS releases or caselist
digests.

### Repository-owned campaign driver

Add `tests/integration/gpu/run_vk_gl_cts.py`, modeled on the existing rendered
guest workflows rather than on the pre-display arbitrary-command runner. It
must:

1. boot the ordinary Virtio-GPU guest with a disposable OverlayFS upper layer
   and use the gadget or JIT engine, never the interpreter for a long CTS run;
2. wait for the real display manager and DRM nodes, then use the actual CTS WSI
   and selected image account. The provisioned binaries currently require the
   real X11/Xwayland socket and xauth: use `uml` for KDE, or the image's
   UID-1000 Phosh session user under greetd/Phrog, inheriting that session's
   `DISPLAY`, `XAUTHORITY`, `XDG_RUNTIME_DIR`, and DRI permissions. Prove
   Xwayland exists on the selected iOS desktop before qualification; otherwise
   rebuild an appropriate upstream CTS platform target as part of the matched
   whole-image update;
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
promotion run, use the provisioned upstream `cts-runner` (for example its
verified `--type=es31` or `--type=gl46` mode) to consume every configuration
declared by `mustpass.xml`, or reproduce and differential-test those generated
arguments exactly. Do not override its seed, watchdog policy, first/other EGL
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
3. **Cross-feature and next-version gap scans.** Nightly or on a merge
   candidate, deterministically shard the complete relevant module with
   `--deqp-fraction=N,M`. Run the next version periodically on macOS first and
   then Simulator to
   turn its failures into an implementation backlog; this is discovery, not a
   profile claim.
4. **Version-promotion gate.** Run every `mustpass.xml` configuration and its
   shipped Khronos list for the candidate version and every lower version that
   remains claimed. Run macOS first, the identical Simulator campaign second,
   and the complete selected campaign on the connected iPad last.
5. **Absolute-ceiling qualification.** For GLES 3.2 include both
   `gles32-khr-main.txt` and `gles32-khr-glesext.txt`, geometry/tessellation,
   texture-buffer, cube-array, advanced-blend, sample-shading, copy-image, and
   interaction coverage. At every desktop GL transition run that version's
   main and GTF lists cumulatively. GL 4.4 adds non-CTS blob/coherent-mapping
   stress; GL 4.5/4.6 add cumulative DSA, sync, image/SSBO/atomic, indirect,
   robustness, limit, and negative coverage.

The production iPad guest remains 1,536 MiB. Run one shard at a time, disable
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
signature, CTS identity, backend/platform/device/page profile, upstream spec or
CTS issue, owner, rationale, and expiry, while retaining the failure in totals.
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
Because the source has no `.git` metadata, reproduce any proposed CTS fix in
an authenticated upstream 1.4.5.5 checkout before submission. Diagnostic
rebuilds may run in disposable Simulator/macOS storage with recorded flags,
but cannot qualify a release. If the target API or required CTS revision
outgrows 1.4.5.5, rebuild the binary, resources, must-pass data, and source as
one authenticated rootfs update; never mix generations or mutate the image in
place.

## Implementation phases

### Phase 0: freeze and automate the baseline

Before changing a provider:

1. Record the revision and dirty status, compiler/SDK, Apple-GPU platform
   contracts and installed provenance, rootfs Mesa version, and kernel page
   profile.
2. Validate the central manifest and patch inventory and record the current
   platform contracts. Copy the already-recorded CGL version/capset result into
   the evidence index as explicitly archival data. Do not rebuild, rerun, or
   preserve a CGL executable, prefix, preset, or CTS lane for comparison.
3. Define the atomic dependency transition: one `apple-gpu` membership, one
   builder, three platform prefixes, and no desktop libepoxy/virglrenderer
   graphics rows in the `macos` product. Prepare all target repositories once
   and verify them offline before any consumer switches.
4. Turn the existing host probes into maintained source and test targets. A
   probe must request ES 3.0, 3.1, and 3.2 separately and report every Virgl
   feature predicate, not only version strings.
5. Build common ANGLE and Metal capability probes. Run the macOS probe first
   on every iteration, then run the separately signed iOS probe on the iPhone
   17 Pro Max Simulator with the same schema. Do not attach to or replace a
   running Kernarium guest merely to obtain host capabilities.
6. Record the connected-iPad host and guest baseline needed for later
   qualification. Capture the exact guest desktop GL value on Simulator and
   the iPad; until then retain “GL 2.1 expected” rather than converting it to
   measured.
7. Inventory VK-GL-CTS from the image, verify the release, executable build
   IDs, installed resources, registered modules, `mustpass.xml` files, leaf
   counts, and digests. Recover or recreate the source/build receipt needed to
   prove the co-located source and provisioned binaries match, and store those
   identities in the evidence bundle.
8. Bring up the dedicated CTS driver with a software-renderer negative control:
   it must reject llvmpipe, unexpected Zink, Xvfb, a missing DRM device, a
   truncated QPA, and an incomplete shard rather than recording a pass.
9. After the atomic shared-path switch in Phase 1, run the identity sentinel
   and shipped GLES 2/GLES 3.0 baseline selections on macOS first, Simulator
   second, and the connected iPad last. Classify existing failures without
   waiving them.

Exit: macOS is the reproducible first-stage development baseline, the
Simulator adapter is ready to validate that exact path, and the connected iPad
has a recorded qualification baseline. The old CGL number exists only in
archived prose/evidence. The CTS identity/caselist baseline and current
source/build-receipt gap are recorded, and the campaign adapters reject
software rendering and incomplete artifacts on every product shell. This
preparation creates no second runnable macOS graphics lane.

### Phase 1: atomically replace macOS graphics with the common Apple path

1. Introduce the fixed-layout ANGLE provider boundary. The selected backend is
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
audits reject their return, and `check-gpu` exercises common ANGLE. The three
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
6. Bring up macOS over VNC/CPU readback, then add AppKit direct
   `MTLTexture` presentation.
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
iPad passes the phase qualification. The shipped GLES 2 and GLES 3.0 Khronos
must-pass matrices have complete macOS-first, same-path Simulator, and connected-iPad
result sets: every required core case is `Pass`, and any other accepted status
is explicitly permitted by the exact Khronos criteria.

### Phase 3: prove the GLES 3.1 host provider

Run this as a feasibility gate before changing production packages.

#### Preferred spike: ANGLE/Vulkan over MoltenVK

1. Add Vulkan-Headers, MoltenVK, and every required transitive source as exact
   repository/full-commit rows or declared sparse paths in the central
   manifest. Select them through a distinct experimental product/prefix (for
   example `apple-gpu-vulkan-experiment`) but use the same generalized,
   platform-parameterized GPU builder, platform-contract schema, installed
   provenance rules, verifier, and CMake target design. Do not contaminate the
   accepted ANGLE/Metal prefix, add a second Vulkan manifest, or fetch from
   Xcode, GN/gclient, Git submodules, or any downstream build step.
2. Build the authenticated MoltenVK macOS slice and run the shared ANGLE probe
   first; then build the Simulator slice and separately signed iOS probe from
   the same prepared inputs and recipe.
3. Port/build ANGLE's Vulkan renderer for iOS and request
   `EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE`.
4. Handle MoltenVK portability enumeration in the host composition and verify
   on macOS, then the iPhone 17 Pro Max Simulator, that ANGLE creates an ES 3.1
   context without overrides.
5. Exercise compute, SSBOs, image load/store, atomic counters, indirect draws,
   framebuffer-without-attachment behavior, arrays-of-arrays, and the exact
   Virgl gates individually.
6. Use macOS for the first debugger, sanitizer, focused CTS, and guest runs,
   but reject any change that cannot be built and validated through the same
   Simulator product configuration.
7. Build the authenticated device slice and run the complete phase gate on the
   connected iPad. Upstream ANGLE's Apple rules currently exclude iOS from the
   Vulkan renderer and its support table does not claim this combination, so
   build flags, loader injection, portability, feature discovery, packaging,
   lifecycle, and native export all need proof on iOS.
8. Start with CPU readback. Design Vulkan-to-Metal direct scanout only after
   the API path is correct.
9. Promote the proven revisions/features from the experimental product into
   the accepted common Apple-GPU product only after macOS, same-path Simulator,
   and connected-iPad gates pass. Publication must remain completely
   offline after `prepare.sh`.

#### Fallback: extend ANGLE's Metal backend to ES 3.1

If the Vulkan composition cannot be made supportable, implement the missing
ES 3.1 Metal functionality in ANGLE rather than faking Virgl caps. This means
upstream-quality support for compute, storage buffers, image operations,
atomics, limits, barriers, program interfaces, and conformance. Land or carry
the host-side work under authenticated patches; guest Mesa remains unchanged.

Exit: an unmodified guest Mesa naturally reports GLES 3.1 on macOS, Simulator,
and the connected iPad; functional tests for all mandatory feature families
pass; the complete `dEQP-GLES31` module has been sharded as diagnostic gap
coverage; and every `mustpass.xml` configuration using
`gles31-khr-main.txt`, plus every lower claimed GLES list, has no unexplained
local failure: required core cases are `Pass`, and any other accepted status is
explicitly permitted by the exact Khronos criteria. The phase cannot exit
without the connected-iPad campaign.

### Phase 4: raise desktop GL to the truthful GLES-3.1-backed limit

GLES 3.1 does not automatically produce desktop GL 3.x. Generate a checklist
from the exact released guest Mesa's `src/mesa/main/version.c`, then compare it
with the Virgl capset and host implementation.

The first known blocker is conditional rendering. Implement its complete
semantics in host virglrenderer when the GLES host lacks native
`GL_NV_conditional_render`:

- WAIT, NO_WAIT, BY_REGION, and inverted modes;
- query creation, completion, deletion, and context ownership;
- draw, clear, blit, transform-feedback, indirect, and multi-draw paths;
- nested/invalid command rejection;
- reset and context-loss behavior; and
- no synchronous guest-visible stall in a NO_WAIT mode.

Advertise the capability only after positive and negative tests pass. Repeat
the same process for every remaining GL 3.0 and GL 3.1 gate and required
format. Do not infer GL 3.1 from conditional rendering alone.

Exit: `glxinfo -B` naturally reports the target desktop version; focused
macOS development tests, Piglit, conditional-render interaction cases, and
cumulative GL 3.0/3.1 main and GTF must-pass matrices pass, followed by the
identical Simulator validation and connected-iPad qualification; GLES 3.1
remains unchanged.

### Phase 5: reach the GLES 3.2 maximum

GLES 3.2 requires geometry shaders. Current MoltenVK/Metal does not expose a
geometry stage, so this phase is a compiler/runtime project, not a capset
change.

Run a design spike comparing:

1. geometry-shader lowering in ANGLE's Vulkan backend using compute, mesh, or
   multi-pass expansion;
2. a reusable geometry emulation in MoltenVK; and
3. geometry emulation in a host desktop-GL-on-Vulkan provider that can also
   serve the GL 4.6 phase.

Select the smallest implementation that preserves primitive ordering,
adjacency, amplification limits, transform feedback, clipping, layered
rendering, queries, indirect draws, synchronization, and robust-access
semantics. A Metal-private or device-family-only shortcut is not acceptable.

After geometry works, close every other GLES 3.2 gate, including copy image,
texture-buffer ranges, advanced blend equations, tessellation, cube-map
arrays, sample shading, ASTC LDR, and the required limits and formats.

Exit: the host creates ES 3.2 without a testing override and the guest
naturally reports GLES 3.2. Run the complete GLES 3.2 gap scan continuously on
macOS first and validate it on the iPhone 17 Pro Max Simulator; require every configured run of
`gles32-khr-main.txt`, `gles32-khr-glesext.txt`, and all lower claimed GLES
lists. Keep both on the same renderer path and complete the identical agreed
physical-device selection on the connected iPad before qualification. Every
remaining non-`Pass` is explicitly permitted by the exact Khronos criteria;
local waivers cannot promote the version.

### Phase 6: reach the desktop GL 4.6 maximum

ANGLE's GLES frontend is not a complete desktop GL 4.6 provider.
First evaluate an authenticated upstream desktop-GL-on-Vulkan provider, such
as Mesa Zink over MoltenVK, in an offscreen EGL configuration. First prove
that the provider can be built, packaged, and created through public APIs on
macOS first, then the iPhone 17 Pro Max Simulator. Use macOS to accelerate
shared-code debugging without admitting a desktop-only provider, and require the connected iPad
before making any product claim. A macOS-only GL 4.6 context is irrelevant if
the same provider cannot run on iOS. If the iOS feasibility gate fails, the
remaining work is equivalent to implementing the missing desktop GL semantics
in a host library and must be estimated as such.

Provision that spike as an explicitly experimental product in the same central
Git manifest and common Apple-GPU builder. Pin the provider's repository/full
commit, every transitive source, sparse paths, and any generated ordered patch
series; add its payload and feature policy to separate experimental platform
contracts, prefixes, and verifiers. Prove the builder is offline after the
sole `prepare.sh` network step. Do not
place a Zink/Mesa checkout under Kernarium, discover an ambient build through
`pkg-config`, or merge its libraries into an accepted prefix until the
macOS-first and Simulator-validation gates select it. If selected, reconcile imported-target names and
process-global loader state before it can coexist with or replace ANGLE.

In parallel, add the standard coherent resource path needed above GL 4.3:

- `VIRTIO_GPU_F_RESOURCE_BLOB`;
- `RESOURCE_CREATE_BLOB`;
- `RESOURCE_MAP_BLOB` and `RESOURCE_UNMAP_BLOB`;
- `SET_SCANOUT_BLOB` where required;
- strict blob ID, size, flag, offset, cache-mode, and lifetime validation;
- a coherent mapping design valid for both 4 KiB and 16 KiB guest profiles;
- reset-time unmapping before guest memory disappears; and
- a truthful `supports_coherent` result in the guest winsys.

Share the bounded mapping mechanism with the Vulkan plan where possible, but
keep GL and Vulkan feature negotiation independent. Do not advertise blob or
coherency merely because the commands parse.

Then qualify GL versions in order: 3.2, 3.3, 4.0, 4.1, 4.2, 4.3, 4.4, 4.5,
and 4.6. At every transition, record the exact newly satisfied Mesa gates,
formats, limits, shader version, and test coverage. Stop at the last truthful
version if the Apple portability backend cannot supply the next one.

Exit: the guest naturally reports GL 4.6 with a released upstream Mesa; the
full agreed Piglit coverage and the cumulative main/GTF must-pass matrices for
every claimed desktop version pass macOS first and Simulator second; the
selected shard union matches the frozen caselist exactly; the
connected iPad completes the mandatory device matrix; and coherent blob stress
passes both guest page profiles without leaks or stale mappings.

### Phase 7: product adoption and upstreaming

1. Ship only the shared ANGLE provider on macOS even if its initial desktop-GL
   ceiling is below the archived CGL measurement. Keep final-link and option
   tests that reject reintroduction of the removed desktop path.
2. Document macOS as the normal first development loop, the iPhone 17 Pro Max
   Simulator as the required same-change validation loop, and the connected
   iPad as the physical-device release gate.
3. Upstream ANGLE, virglrenderer, MoltenVK, Mesa/Zink, or protocol changes in
   their owning projects. For a carried dependency change, maintain the commit
   in a clean topic branch and regenerate its ordered mail series with
   `dependencies/rebase-patches.sh`; generated patch files are not editing
   authorities. Repository topics must stay focused and removable.
4. Update the whole EROFS image only when consuming a released Mesa. Follow
   the signed whole-image provisioning workflow; do not mutate the image in
   place.
5. Promote a version separately for macOS, Simulator, and connected iPad, but
   do not qualify the common product profile until the connected iPad
   completes its acceptance matrix.
6. Pin the CTS release, executable build IDs, source identity, resources,
   must-pass data, and caselist digests in rootfs provisioning metadata. After
   any Mesa or CTS update, rebuild the whole authenticated image and rerun the
   baseline; never compare totals from different case universes as if they
   were a regression A/B.

## Repository work map

| Area | Primary files | Planned responsibility |
| --- | --- | --- |
| Renderer selection | `darwin/device/gpu/CMakeLists.txt`, `virgl_renderer.hpp`, `virgl_renderer.cpp` | fixed-layout ANGLE/EGL provider boundary, Metal native handle, backend validation, and removal of CGL code/options/tests |
| Device propagation | `in_process_service.*`, `vhost_user_server.*`, `virtio_gpu_device.*` | carry the product-fixed provider result, reject configuration drift, select the checked presentation handle, and preserve reset/fence ownership |
| Displays | `display.cpp`, `display.hpp`, `native_display.mm`, `native_display_ios.mm` | separate VNC from renderer, share Metal timeline, retain AppKit/UIKit shells and CPU fallback |
| macOS shared-path composition | `darwin/app/macos/main.cpp`, common-product and final-link tests | consume the product-fixed Apple-GPU backend with no renderer option; reject CGL, OpenGL.framework, and product drift before guest start |
| Kernarium composition | `Kernarium/Runtime/KernariumRuntime.mm`, runtime CMake | select the supported iOS backend without changing app/kernel ABI |
| Common Apple GPU dependency identity | `dependencies/manifest.tsv`, `dependencies/prepare.sh`, `dependencies/rebase-patches.sh`, `dependencies/patches/*/series.tsv` | own exact Git repositories/full commits, sparse paths, product membership, and generated ordered mail-patch identity; no product-local manifests or fetches |
| Common Apple GPU build and verification | planned `dependencies/build-apple-gpu.sh`, `dependencies/platforms/*-gpu-arm64.tsv`, generalized Apple-GPU CMake importer/verifier | replace the iOS-only recipe with one macOS/Simulator/device ANGLE recipe; publish transactionally, install provenance, and verify exact payload/platform/closure without backend drift |
| macOS dependency cleanup | `dependencies/manifest.tsv`, `dependencies/build.sh`, `dependencies/verify-prefix.sh`, `dependencies/cmake/UmlThirdPartyTargets.cmake` | remove desktop libepoxy/virglrenderer graphics rows, recipes, and imported targets from the ordinary macOS prefix; make final-link audits reject CGL/OpenGL.framework |
| Product packaging | `Kernarium/scripts/package-ios-frameworks.sh`, `Kernarium/Kernarium.xcodeproj/project.pbxproj`, macOS bundle/runtime-closure verification | consume only verified prefix targets; embed each required framework exactly once and keep signing/rpath/lifecycle differences out of renderer configuration |
| Virgl and ANGLE topics | `dependencies/patches/utm-virglrenderer/`, `dependencies/patches/utm-webkit/` | use one production renderer series on all three products, add truthful host emulation, retire desktop-only graphics topics, and regenerate every carried series from commits with `rebase-patches.sh` |
| Blob protocol | `darwin/device/gpu/protocol.hpp`, `virtio_gpu_device.*`, renderer/resource boundary | standard commands, bounded mapping, coherent lifetime |
| Rootfs CTS | `tools/rootfs/manifest.json`, provisioning inputs/tests, in-image `/opt/vk-gl-cts` and `/home/uml/vk-gl-cts-1.4.5.5` | provision the upstream `cts-runner`; keep executable, source/build receipt, resources, must-pass data, build IDs, and caselist identities matched through authenticated whole-image updates |
| CTS and guest tests | `tests/integration/gpu/run_vk_gl_cts.py`, `tests/unit/gpu/`, `tests/tooling/gpu/`, dependency/rootfs tests, `tests/test-manifest.tsv` | graphical-session launch, identity rejection, exact caselist/config sharding, QPA egress/parsing, backend lifetime, protocol validation, package shape, guest probes and rendered campaigns |
| Dependency tooling tests | `tests/tooling/dependencies/`, `tests/test-manifest.tsv` | keep `dependency-source-preparer`, `dependency-patch-regenerator`, `dependency-build-manifest`, `dependency-prefix-verifier`, and `dependency-runtime-closure` green; add Apple-GPU platform/payload/provenance/concurrent-publication fixtures |

New implementation sources must remain in `darwin/device/gpu/` and be owned by
its CMake target. Do not include implementation files into other C++ sources.

## Verification matrix

### Host and unit gates

- backend option parsing and unsupported-platform rejection;
- fixed `VirglRenderer` and enclosing object size across all consumers;
- provider construction failure, partial cleanup, context sharing, make-current
  behavior, and same-thread teardown;
- EGL error draining and missing-entry-point handling;
- Metal-texture, dropped, unsupported, and CPU-fallback
  presentation paths;
- malformed resource/blob commands, overflow, duplicate IDs, rollback, reset,
  and connection retry;
- VNC lifetime and port reuse with the common ANGLE renderer; and
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

Phase 1 must make these common interfaces directly runnable. The command order
is intentional: build and test macOS first, validate the same revision and
recipe on Simulator second, then build and qualify the connected iPad. The
retired `build-ios-gpu.sh` must not remain as an independently maintained
compatibility recipe. `check-gpu` and its sanitizer variant switch atomically
to common ANGLE and final-link checks prove that CGL, OpenGL.framework, and the
desktop virglrenderer/libepoxy family are absent. An iOS device build alone is
not device evidence. Preserve the architecture audit and test-manifest
ownership.

### Guest API and function gates

For every claimed ANGLE backend and Apple platform:

- verify `/dev/dri/card0` and `/dev/dri/renderD128`;
- capture capsets 1 and 2;
- run `eglinfo -B -p gbm`, `glxinfo -B`, and explicit ES context creation;
- assert `virgl` and the selected host renderer, never llvmpipe or guest Zink;
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
- zero CPU fallback only after native interop is implemented and proved;
- normal KDE/Phosh compositor startup and clean shutdown;
- WebGL 1 and WebGL 2 functional pages;
- WebGL Aquarium only on macOS and Simulator; and
- the ordinary 1.5 GiB connected-iPad desktop boot with the agreed GL probes.

### Performance gates

Use macOS as the primary iterative measurement environment, Simulator as the
required iOS validation environment, and the connected iPad for mandatory
final stability, thermal, memory, and performance evidence. macOS measurements
speed iteration but are not an iOS performance proxy. The archived CGL number
is context only and is never rerun as a product-selection gate.
Keep the guest, page profile, CPU count, memory, display geometry, and workload
fixed, and record raw frame statistics, host/guest CPU, memory, shader-compile
behavior, CPU fallback, and shutdown status.

Correctness acceptance must precede performance comparison. Diagnostic or
sanitizer builds are not score evidence.

## Risk register and stop conditions

| Risk | Mitigation or stop condition |
| --- | --- |
| shared ANGLE lowers macOS from the archived GL 4.1 result toward GL 2.1 | make the change explicit in product strings and tests and accept the lower macOS ceiling; do not preserve or rerun the old provider |
| renderer macro changes object layout again | use one opaque provider pointer and one public layout; compile-time assertions and cross-target consumer tests |
| Simulator or macOS evidence is mistaken for device proof | require the full connected-iPad campaign for every promoted capability; no substitution |
| ANGLE/Vulkan cannot build or enumerate MoltenVK on iOS | fail the feasibility gate before production integration; either upstream the Apple port or use the Metal-ES3.1 fallback |
| native-share API is ANGLE/Metal-only | begin with CPU readback; design Vulkan/Metal interop independently |
| a forced cap produces a higher string but broken behavior | prohibit overrides; gate every capability on functional and conformance tests |
| software rendering or a truncated CTS shard creates a false pass | reject unexpected renderers before execution; require QPA identity/trailer, exact caselist union, artifact digest, clean process exit, and clean guest shutdown |
| full CTS exhausts the 1.5 GiB iPad guest | run one bounded shard per fresh guest, export artifacts immediately, disable deliberate OOM/image logging, and treat OOM or app termination as failure rather than a skip |
| geometry emulation changes ordering or query semantics | do not advertise GLES/GL 3.2 until complete geometry and interaction coverage passes |
| blob mappings leak or outlive guest memory | bounded ownership table, exact unmap/reset ordering, ASan/UBSan stress, both page profiles |
| host GL 4.6 provider is not viable on iOS | retain the last truthful per-platform profile; do not turn an architectural protocol maximum into a product promise |
| dynamic frameworks weaken dependency provenance | exact allowed framework identities and rpaths; no ambient lookup or global verifier relaxation |
| the new common GPU product duplicates or bypasses the refactored dependency authority | one central manifest and prepare step; extend product membership, common builder, platform contracts, CMake verifier, and packaging atomically; reject any Kernarium-local manifest/fetch |
| a hand-edited or stale dependency patch produces an unreviewable prepared tree | edit commits in a clean topic branch, regenerate with `dependencies/rebase-patches.sh`, validate the complete manifest/series inventory, and refuse mismatched prepared repositories |
| atomic removal leaves a hidden desktop graphics consumer | delete the desktop libepoxy/virglrenderer rows, recipes, imports, CGL provider/CLI/tests, and OpenGL-texture bridge together; make source, File API, link-map, and runtime-closure negatives reject each one |
| an experimental MoltenVK/Zink dependency leaks into the accepted product | use distinct manifest products, prefixes, platform contracts, CMake targets, and package checks until every promotion gate passes |
| GN, Xcode, submodules, or another transitive tool fetches undeclared source | list every transitive source/commit/path centrally, run only `prepare.sh` with network access, and test builders in an offline environment |

## Completion criteria

The plan is complete only when all of the following are true:

1. macOS can build, test, and ship only the same ANGLE provider architecture as iOS,
   including source identity, patches, EGL attributes, capset logic, and
   native-resource contract. No CGL provider, CLI choice, preset, test lane,
   desktop graphics dependency row/recipe/target, OpenGL.framework link, or
   fallback exists.
2. The product-fixed backend no longer changes GPU object layout or controls
   VNC/UI platform ownership, and macOS cannot select a provider independently
   of the two iOS slices.
3. The three Apple products consume authenticated slices from one reviewed
   host-graphics source matrix, one common GPU recipe, and platform-specific
   contracts verified against the same central manifest and ordered series.
4. Every claimed GL/GLES version arises naturally from an unmodified released
   guest Mesa and passes the matching functional/conformance gate.
5. Native presentation and CPU fallback have explicit, measured behavior on
   each platform.
6. GL 4.4 or later is claimed only after coherent blobs work under both guest
   page profiles.
7. GLES 3.2 or desktop GL 3.2+ over MoltenVK is claimed only after geometry
   shader behavior is implemented and qualified.
8. macOS is the documented first development loop; the same revision then
   passes the iPhone 17 Pro Max Simulator validation loop, and every final
   claimed profile completes the required campaign on the connected iPad. Run
   the full must-pass matrix under the default 16 KiB
   profile; repeat it under 4 KiB for page-, resource-, mapping-, or
   blob-sensitive changes, and otherwise run the fixed 4 KiB sentinel and
   regression selection.
9. Every required CTS configuration and shard completes exactly once against a
   frozen source/build receipt and binary/resource/caselist identity; its
   verified union has no missing cases. Every required core case is `Pass`;
   any other accepted status is explicitly permitted by the exact Khronos
   criteria. Required connected-iPad shards run under both profiles for
   mapping-sensitive milestones.
10. `check-gpu` and `check-gpu-asan-ubsan` exercise common ANGLE and reject the
   removed desktop path; dependency verification, the four
   GPU-bearing Kernarium runtime presets,
   `Kernarium/scripts/package-ios-frameworks.sh`, architecture audit, and the
   selected guest campaigns pass. Run the independent public-library
   `scripts/build-xcframework.sh` gate only when that package graph is affected;
   it is not evidence for ANGLE/Kernarium packaging.
11. The dependency workflow passes manifest/series validation, offline
   prepared-source verification, all three prefix verifiers, installed
   provenance comparison, package/runtime-closure checks, and negative tests
   for stale or wrong-platform inputs. This includes exactly one Apple-GPU
   prefix for macOS, Simulator, and device; no legacy graphics prefix remains.
   No deleted Kernarium-local dependency authority or archive-preparation path
   has returned.
12. `darwin/README.md`, `dependencies/README.md`, `docs/testing.md`, the Virgl
   implementation plan, feasibility report, and evidence index are updated to
   distinguish current state, experiment, and accepted product support.

The truthful evidence statement is: an older macOS CGL build measured guest
GL 4.1/GLES 3.0, while the current iOS ANGLE/Metal path measured an ES 3.0 host
and passed the recorded WebGL 2 workload. The macOS number is archival only and
does not define a lane in this plan. No new shared capability is claimed until
the exact Apple-GPU path passes macOS first, the iPhone 17 Pro Max Simulator
second, and the connected iPad qualification last.
