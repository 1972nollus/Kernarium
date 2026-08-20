# Maximum guest Vulkan support through Venus and MoltenVK on Apple hosts

Written 2026-08-20. This is an implementation plan, not a capability,
Vulkan Profile, or conformance claim. It builds on the measured feasibility
results in [GPU_BACKEND_FEASIBILITY.md](GPU_BACKEND_FEASIBILITY.md) and assumes
that production guest userspace graphics libraries must be unmodified upstream
releases. The guest kernel is not external: this project owns its Linux build,
`virtio_gpu` driver, UML transport, and maintained patch series.

Dependency-management assumptions were revised after the 2026-08-20
centralization under `dependencies/`; paths and workflows in this document
refer to that post-refactor ownership model.

## Decision summary

Build one in-process Venus renderer over MoltenVK for macOS, iOS Simulator,
and iOS device, with iOS remaining the primary product target. One global
renderer session may own both APIs, but its facades are disjoint:

```text
guest GL/GLES -> Virgl capsets 1/2 -> ANGLE Metal -> Metal
guest Vulkan  -> Venus capset 4    -> MoltenVK    -> Metal
```

GL/GLES must never enter Zink, Venus, a Vulkan loader, ANGLE's Vulkan backend,
or MoltenVK. The two facades may share bounded memory/presentation
infrastructure and lifetime ownership, not an API translation route. macOS
must not retain CGL, a desktop-only Virgl provider, an alternate Vulkan
configuration, or a graphics fallback. Only thin AppKit/UIKit lifecycle shells
and optional macOS VNC output may differ; VNC is not native-performance
evidence.

Use macOS first for protocol, unit, sanitizer, memory, guest, CTS, and display
iteration because it has the shortest build/run/debug loop. Every macOS result
then runs unchanged on the iPhone 17 Pro Max Simulator as the required iOS
validation lane. Full qualification requires the complete agreed device matrix
on the connected iPad; neither macOS nor Simulator evidence substitutes for
it.

The architectural ceiling is **up to Vulkan 1.4 plus
`VK_KHR_portability_subset`**. The production target is the highest truthful
API tier whose required behavior is Metal-native or efficiently GPU-lowered
and passes the frozen performance gates on the connected iPad. A lower
high-performance tier takes precedence over a higher but materially slower
tier. The result is not standard conformant Vulkan 1.4 and is not a named
Vulkan Profile unless the exact profile tool, required tests, and performance
gate all pass.

There are four important qualifications:

1. The current repository has no usable guest Vulkan path on any Apple
   product. Venus is disabled, capset 4 and blob resources are absent, and the
   required guest/host memory-sharing mechanism is not implemented.
2. Current upstream Mesa Venus does not transport or advertise
   `VK_KHR_portability_subset`. MoltenVK is a portability driver, so an honest
   production path with the upstream-only guest policy is blocked until that
   support appears independently in a released Mesa. This project will not
   author or upload the missing Mesa work. Lowering the core API version to 1.3
   or earlier does not remove the portability-subset obligation.
3. That release gate applies to guest userspace, not to Linux. Required
   `virtio_gpu`, DRM, UAPI implementation, and UML transport changes may be
   carried in the authenticated kernel series; the product need not wait for
   those changes to appear in a later upstream Linux release.
4. There is no CPU-heavy full-emulation escape hatch. If a Vulkan core
   requirement, optional feature, memory path, or presentation path requires
   steady-state CPU work proportional to vertices, primitives, pixels,
   invocations, texels, or resource bytes, reduce the advertised tier/feature
   set or leave Vulkan unsupported. Efficient Metal GPU lowering remains
   eligible only after measurement.

The recommended critical path is:

```text
reproducible host probes
    -> create the common Apple-GPU dependency product and authenticated MoltenVK slices
    -> Apple-capable virglrenderer Venus
    -> capset 4, blob resources, and asynchronous fences
    -> implement/backport required virtio_gpu and UML transport support in the maintained kernel
    -> select and implement the shared-memory model
    -> wait for an independently released Mesa with Venus portability-subset support
    -> consume a released unmodified Mesa
    -> headless macOS guest bring-up on the iOS graphics path
    -> direct-GPU macOS scanout plus focused correctness/performance canaries
    -> iPhone 17 Pro Max Simulator same-path canaries
    -> connected-iPad decisive sustained performance/thermal/lifecycle gate
    -> select the highest candidate tier that passed those gates
    -> complete CTS for that tier on macOS, Simulator, and the connected iPad
    -> post-CTS connected-iPad stability and packaging confirmation
    -> claim only that tested high-performance portability tier, up to 1.4
```

## Scope and invariants

The intended API path is:

```text
guest Vulkan application
    -> upstream Vulkan loader
    -> released upstream Mesa Venus ICD
    -> project-maintained Linux virtio_gpu DRM/KMS
    -> project-maintained UML Virtio/vhost-user transport
    -> VirtioGpuDevice
    -> virglrenderer Venus
    -> directly linked MoltenVK
    -> Metal
```

Native presentation is a consumer of this path, not part of the first
headless bring-up gate. The shared ANGLE/Virgl GL renderer remains available
while Venus is developed on every Apple platform, but remains exclusively on
ANGLE/Metal. The current macOS CGL path is removal input only: archive any
useful before/after evidence, then remove its product target, dependency
provider, runtime selection, and fallback from the planned graphics graph.

The following constraints are non-negotiable:

1. iOS is the primary product target, while macOS is the first development
   lane. The renderer, memory, synchronization, and packaging architecture must
   satisfy Simulator/device public APIs, signing, sandbox, lifecycle, and the
   1.5 GiB device-memory policy. macOS executes that exact design and cannot
   add a desktop-only GL/Vulkan provider, resource, capset, or fallback path;
   AppKit or VNC may only consume the common presentation output.
2. Only guest userspace is externally constrained. Development and production
   use released, unmodified upstream Mesa, libdrm, the Vulkan loader, and the
   Venus ICD. Do not author, carry, or test a project-patched guest graphics
   library as a substitute for the missing portability path; it cannot satisfy
   any plan gate.
3. Do not hide MoltenVK's portability status, inject extension strings, force
   a higher Vulkan version, or omit required portability
   features/properties. The guest must enumerate, query, and enable the subset
   honestly. Standard Venus negotiation may cap the exposed API downward to
   the last complete performance-qualified tier; never rewrite a version
   string independently of that behavior.
4. Do not advertise Venus capset 4, blob resources, a memory type, or an
   external-handle extension until the complete behavior exists.
5. Preserve the standard Virtio-GPU/Venus command stream and upstream Vulkan
   loader model. Do not replace it with an app-specific RPC, guest preload, or
   custom Vulkan library.
6. The guest kernel is controlled by this repository. Backport or implement
   required `virtio_gpu`, DRM, UAPI implementation, Kconfig, and `arch/um`
   transport behavior in the maintained common patch series without waiting
   for an upstream Linux release. Keep one relocatable Linux source for
   Simulator, device, and macOS and build only the existing 16 KiB and 4 KiB
   profiles. Prefer standard Virtio-GPU/DRM interfaces; any project-specific
   kernel-to-emulator extension must be negotiated, bounded, invisible to
   userspace, and expose an upstream-compatible ABI that released Mesa/libdrm
   already consumes. A private userspace ioctl or library cannot be required.
   Any new graphics Kconfig support belongs built-in in
   `kernel/config/common.fragment` because loadable modules remain disabled;
   page fragments vary only genuine page-derived values.
7. Keep all third-party source identities, patches, build recipes, slices, and
   dynamic dependencies authenticated by the private dependency workflows.
   Never discover Homebrew or ambient Vulkan components in production.
8. Run Venus in process. iOS must not depend on a helper executable, Unix
   process spawning, or desktop ICD discovery.
9. Preserve the GPU transport's single-owner/reset rules and implement fence
   completion asynchronously. A long Vulkan operation must not block the sole
   Virtio-GPU command thread or shutdown.
10. Use public Apple, Metal, and Vulkan APIs only. Preserve ordinary app signing
   and the no-JIT/no-private-entitlement product contract.
11. Report macOS, Simulator, and connected iPad separately. macOS is the
    preferred iteration lane, Simulator is mandatory same-path iOS validation,
    and the connected iPad is mandatory physical-device qualification.
12. Do not call a result a named Vulkan Profile or Vulkan conformant merely
    because `vulkaninfo` prints version 1.4 or a CTS subset passes.
13. Keep connected-iPad workloads within the repository's normal 1.5 GiB guest
    policy. WebGL Aquarium remains prohibited there and is unrelated to the
    Vulkan acceptance gate.
14. External-project access is fetch-only. Never push a branch, open or update
    a pull request, merge request, or issue, send a patch or email, or upload
    an artifact to an external project. First-party emulator, test, and rootfs
    build/orchestration changes live directly in this repository; third-party
    virglrenderer, MoltenVK, or ANGLE source changes live only in
    `dependencies/patches`; Linux changes live only in `kernel/patches`.
    Qualification CTS source and guest Mesa, libdrm, loader, and ICD remain
    unmodified released inputs; a missing guest-userspace capability is an
    external wait condition, not permission to patch or contact that project.
15. Bounded command decoding, validation, resource bookkeeping,
    synchronization, and shader compilation are ordinary control-plane work.
    Data-plane feature emulation on the CPU is forbidden when work scales with
    rendered or computed data. GPU-backed lowering through public Metal render,
    compute, blit, texture, buffer, or indirect-command facilities is eligible
    only after the matched performance gates pass.
16. Profile version is subordinate to performance. Freeze the workload corpus,
    noise envelope, and thresholds before implementation; compare each
    candidate with the immediately lower truthful tier; and keep the lower tier
    when correctness, performance, memory, thermals, lifecycle, or frame pacing
    fails. CTS or Profiles-tool success cannot override that decision.

The maximum GL/GLES work is specified separately in
[MAX_GL_GLES_SUPPORT_PLAN.md](MAX_GL_GLES_SUPPORT_PLAN.md). That path is
ANGLE/Metal only. Zink-over-Venus/MoltenVK is not a product, fallback,
qualification, or performance path.

## Baseline, ceilings, and claim boundaries

### Current component state

| Layer | Current state | Consequence |
| --- | --- | --- |
| Dependency authority | one central Git-only `dependencies/manifest.tsv`, product selectors, prepared repositories, ordered mail-patch series, platform contracts, builders, CMake imports, and prefix verifiers | all Vulkan host inputs and transitive sources must extend this model; retired Kernarium-local/archive workflows are not valid owners |
| Repository device protocol | Virgl capsets 1/2; no Venus capset 4 or blob commands | guest cannot discover or use Venus |
| Current renderer split (historical removal input) | macOS selects upstream virglrenderer 1.3.0 plus CGL from the static desktop aggregate, while iOS selects the UTM virglrenderer/ANGLE topic through `dependencies/build-ios-gpu.sh`; both pass `-Dvenus=false` | this divergence must be deleted, not preserved as a migration preset or fallback; the common Apple-GPU product becomes macOS's only graphics provider |
| Accepted Vulkan host dependencies | no Vulkan-Headers, MoltenVK, or SPIRV-Cross manifest row, prefix payload, imported target, platform component, or package slice | the local MoltenVK build cannot enter a production claim; a new common Apple-GPU product is required |
| Maintained guest kernel | pinned Linux 6.18 already contains the standard Virtio-GPU blob, context-init, Venus capset, and DRM UAPI, but the UML transport lacks the complete shared-region/guest-VRAM implementation needed by Venus | kernel/UML work is an owned production deliverable, not an external ceiling; preserve the released-userspace ABI and patch the implementation through the maintained common series |
| Current signed rootfs | Debian Mesa 25.0.7; Virtio ICD manifest reports API 1.3.305 | mechanical guest-ICD ceiling is 1.3.305 even after transport work |
| Candidate upstream guest Mesa | Mesa 25.1 and later can advertise Vulkan 1.4 | version is sufficient, but portability-subset transport is still missing |
| Prepared virglrenderer 1.3.0 Venus | `VKR_MAX_API_VERSION` caps the major/minor API at Vulkan 1.4; generated protocol inputs use Vulkan headers 1.4.334 | establishes a 1.4 design cap, not a 1.4.334 profile, conformance, or patch-level ceiling |
| Unmanaged local macOS MoltenVK 1.4.3 probe | library advertises 1.4.357; an unsandboxed smoke accepts an instance requested at 1.4.357 and reports a Vulkan 1.4 physical device | proves macOS host major/minor feasibility only; it is not an accepted dependency, guest result, or either iOS product |
| MoltenVK feature model | portability driver; macOS probe reports geometry shaders false | target is Vulkan plus portability subset, not full core conformance |
| Apple MoltenVK packaging | no accepted MoltenVK product exists; the local XCFramework has only macOS arm64 | macOS/Simulator/device slices, imports, contracts, verification, packaging, signing, and probes are Phase 1 work |

The local MoltenVK probe also found none of the six Linux-oriented Venus
external-handle extensions currently assumed by the renderer:

- `VK_KHR_external_memory_fd`;
- `VK_EXT_external_memory_dma_buf`;
- `VK_EXT_image_drm_format_modifier`;
- `VK_EXT_queue_family_foreign`;
- `VK_KHR_external_fence_fd`; and
- `VK_KHR_external_semaphore_fd`.

It did report `VK_EXT_external_memory_host` and `VK_EXT_metal_objects`. The
Apple port must use the semantics MoltenVK actually supplies and filter the
guest-visible extension set truthfully; it must not synthesize Linux fd or
dma-buf behavior.

### Honest support ladder

| State | macOS | Simulator | Connected iPad | Permitted claim |
| --- | --- | --- | --- | --- |
| Current repository | none | none | none | no guest Venus Vulkan |
| Host/transport prototype with current rootfs | experimental only | experimental only | experimental only | at most enumeration/smoke evidence; not an honest MoltenVK portability profile |
| Project-patched guest Mesa/libdrm/loader/ICD | forbidden | forbidden | forbidden | not a development or qualification state in this plan |
| Future independently released Mesa carrying portability support, current rootfs otherwise upgraded | up to 1.4 + portability subset on the first development lane | same conditional shared-path ceiling; required iOS validation | same conditional ceiling; mandatory device performance qualification | report the highest API tier and optional features that pass correctness and the frozen performance budget on every required platform |

A maintained kernel/UML patch does not make a result “custom guest userspace”
or lower the permitted claim. A patched Mesa, libdrm, loader, or Venus ICD is
prohibited rather than a fallback state; the maintained kernel is an
authenticated part of this emulator product.

Under this policy, today's production ceiling is no usable guest Venus Vulkan,
not Vulkan 1.3: lowering the core version does not remove MoltenVK's mandatory
portability-subset path. Vulkan 1.4 plus the subset remains a conditional
target only if a future unmodified Mesa release independently supplies the
missing support. There is no full software-emulation alternative. If no such
release appears, production Venus remains unsupported. After a qualifying
release exists, if a higher tier cannot be implemented through efficient Metal
GPU behavior, cap production at the last lower tier whose complete requirements
pass.

The current rootfs's 1.3.305 manifest is not a useful reason to promise Vulkan
1.3. The implementation should remain version-capable through 1.4, then update
the signed whole rootfs to a released, unmodified Mesa containing portability
support and protocol capability sufficient for the selected candidate tier,
up to Vulkan 1.4. Negotiate no higher than the measured performance-qualified
tier. If the released components or performance gates
intersect at a lower major/minor version, claim that lower result and record
the instance, requested-instance, and physical-device API values separately.
Mesa 25.1's
release notes record the
[return of Venus Vulkan 1.4 support](https://docs.mesa3d.org/relnotes/25.1.0.html),
but API-version support alone does not solve portability.

At the Mesa main revision audited for this plan, Venus's
[complete passthrough-extension whitelist](https://gitlab.freedesktop.org/mesa/mesa/-/blob/302736bcf272ccf12572879dcc6846196a45f143/src/virtio/vulkan/vn_physical_device.c#L1240-1484)
still omits `VK_KHR_portability_subset`, and its generated Venus protocol has
no codecs for the subset feature/property structures. This is the specific
upstream release gate, not an inference from the current rootfs alone.

[`VK_KHR_portability_enumeration`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_portability_enumeration.html)
controls discovery of portability devices. MoltenVK must be enumerated with
the extension and `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`.
[`VK_KHR_portability_subset`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_portability_subset.html)
describes the missing or restricted device behavior and must be exposed,
queried, transported, and enabled when supported. These are distinct
requirements. MoltenVK's own documentation describes it as a
[Vulkan 1.4 subset implementation](https://github.com/KhronosGroup/MoltenVK#moltenvk-and-vulkan-compliance).
The released guest ICD also needs the loader's documented
[`is_portability_driver` manifest contract](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#driver-manifest-file-version-101),
so guest applications enumerate it through the standard portability path.

## Architectural design

### 1. Give GL and Vulkan separate facades under one renderer session

The current `VirglRenderer` owns a GL context and GL-native resource export.
Virglrenderer itself also has process-global `virgl_renderer_init`/cleanup
state, so independent GL and Venus owners cannot each initialize it safely.
Introduce one device-layer session that owns exactly one process-global
initialization with the union of enabled GL/Venus flags, then put disjoint
facades and state beneath that session:

```text
VirglRendererSession      sole init/cleanup and callback owner
    VirglGlFacade         capsets 1 and 2 -> ANGLE Metal only
    VenusVkFacade         capset 4 -> injected MoltenVK only

GpuNativeResource
    MetalTexture
    DiagnosticReadback
    Unsupported
```

The session interface should cover initialization, capset enumeration,
context/resource dispatch, command submission, transfer/readback, fence
creation and completion, reset, and optional native presentation. The facades
must not call global cleanup independently, and the interface must not leak
Vulkan or Objective-C types into platform-neutral public headers. If the
selected virglrenderer revision cannot initialize both renderers together,
keep GL and Venus mutually exclusive only during bring-up. Completion requires
an authenticated carried virglrenderer fix in the common Apple-GPU product;
keep that fix in this repository and do not upload it. Two renderer sessions or
a false concurrent-capset claim remain forbidden.

`VirtioGpuDevice` continues to own Virtio request validation, resource IDs,
scanouts, transport replies, and reset ordering. Renderer callbacks carry
stable IDs, not pointers into a command buffer or a destroyed guest mapping.
Capsets 1/2 remain available through the common ANGLE/Virgl GL path on all
three platforms; capset 4 is returned only after the Venus backend and selected
memory model initialize successfully. There is no macOS CGL capset provider or
runtime fallback. Add graph and runtime tests proving that a GL-only workload
creates no Venus context, Vulkan instance/device, MoltenVK queue submission, or
Vulkan-backed resource, while capsets 1/2 continue to work with capset 4
disabled. In a combined product, platform contracts record
`gl_provider=angle-metal` and `vk_provider=venus-moltenvk` separately.

### 2. Extend the refactored dependency model and directly inject MoltenVK

The refactor is the only dependency authority. `dependencies/manifest.tsv`
records HTTPS Git repositories, full commits, product membership, optional
sparse paths, and ordered `series.tsv` topics. `dependencies/prepare.sh` is the
only network-capable step and must publish clean, detached, remote-less source
repositories before an offline builder runs. Third-party changes are authored
as commits and regenerated with `dependencies/rebase-patches.sh`; never edit a
generated `.patch` or generated Venus mail-patch payload by hand.

Create, or extend the GL/GLES plan's implementation of, a
platform-parameterized `apple-gpu` product as an explicit Phase 1 deliverable.
Use `apple-gpu-vulkan-experiment` for the opt-in bring-up until promotion. The
selected common interface is:

```text
dependencies/build-apple-gpu.sh
dependencies/cmake/AppleGpuDependencies.cmake
dependencies/cmake/VerifyAppleGpuPrefix.cmake
dependencies/platforms/{ios-simulator,ios-device,macos}-gpu-arm64.tsv
```

`build-ios-gpu.sh` may exist only as a forwarding shim during the atomic
consumer migration and must be removed before Phase 1 exits. The common
builder must emit macOS arm64, iOS Simulator
arm64, and iOS device arm64 from one semantic recipe, in that development
order. Only target triple, SDK, deployment target, signing shell, and final
platform packaging may vary. Select one iOS-capable virglrenderer
repository/full commit and one ordered patch series for the production GL and
Venus renderer. Remove the current desktop virglrenderer/CGL rows and imported
graphics targets from the macOS aggregate as part of the atomic consumer
migration; do not retain them in a migration preset. Every macOS graphics
configure must resolve the same `apple-gpu` provider family as iOS, and a
configure/link graph must fail if it can resolve a second virglrenderer,
libepoxy, GL implementation, or renderer archive.

Add central-manifest rows for MoltenVK, Vulkan-Headers, SPIRV-Cross, and every
other transitive input actually consumed. Supply each prepared source through
explicit build-system overrides and set the equivalent of
`FETCHCONTENT_FULLY_DISCONNECTED=ON`; forbid CPM, submodule, Meson-wrap,
GN/gclient, Xcode, or CMake downloads after preparation. A lazy fetch or
ambient `pkg-config` discovery is a hard build failure.

The three checked-in platform contracts own platform, architecture, deployment
target, linkage/payload kind, components, graphics/presentation APIs, common
recipe identity, and source-manifest ownership. Installed prefixes carry the
central manifest, applicable series/patch bytes, and exact platform contract.
Introduce those fields through one explicit format bump and update all three
contracts, the builder, verifier, CMake imports, packaging, and tooling fixtures
atomically.
The verifier compares that provenance byte-for-byte and validates payload,
symbols, architecture, Mach-O platform, minimum OS, install names, rpaths,
symlink absence, and runtime closure. Record real SDK/compiler/build-time
observations separately or in a generated per-prefix build receipt; do not add
normalized timestamps or binary-equality requirements to checked-in contracts.
Publish through exclusive staging/rename, verify a concurrent winner before
reuse, and never overwrite an invalid existing prefix.

The host loader contract is direct and immutable: the common dependency target
provides the exact MoltenVK `PFN_vkGetInstanceProcAddr` to
`VirglRendererSession` before renderer initialization; the pointer remains
valid until callbacks/fences drain and teardown completes. Build host Venus
with Vulkan dynamic loading disabled. It must not resolve a host Vulkan Loader,
ICD JSON, `VK_ICD_FILENAMES`, ambient `vulkan.pc`, or generic `dlopen`. This is
separate from the guest, which continues to use the upstream Vulkan Loader and
released Mesa Venus ICD manifest.

Use a dynamic MoltenVK framework as the production package contract. Dependency
builds emit verified unsigned platform framework slices; packaging assembles
`MoltenVK.xcframework`, and the final app signs its selected slice on copy.
Both 4 KiB and 16 KiB runtime frameworks carry the same
`@rpath/MoltenVK.framework/MoltenVK` dependency, while Kernarium embeds exactly
one MoltenVK framework outside those runtimes. macOS stages the matching slice
through its thin product shell and reaches it through the same dependency
target and loader-injection contract. Verify architecture, Mach-O platform,
minimum OS, exported symbols, install name, rpaths, signature/embed phase, one
app-level copy, and complete runtime closure. Static MoltenVK is allowed only as
an explicitly non-production spike because it risks duplicating global state
across the two page-profile runtimes; it is never a macOS fallback.

### 3. Port Venus to Apple and MoltenVK semantics

Develop the Apple port as commits on a clean topic based on the pinned
virglrenderer source, keep those commits solely in this repository, and use
`dependencies/rebase-patches.sh` to regenerate the carried ordered series.
Generated mail files are review artifacts, not editing authorities. The Apple
port must:

1. obtain Vulkan entry points from the injected MoltenVK function;
2. add portability enumeration before physical-device selection;
3. preserve and expose `VK_KHR_portability_subset` rather than filtering it;
4. marshal its feature and property structures in the Venus protocol;
5. enable the subset when creating the renderer device;
6. remove assumptions that every usable allocation, fence, or semaphore can
   be represented by a Unix fd;
7. use `VK_EXT_external_memory_host` for the selected shared allocation path;
8. filter fd, dma-buf, DRM modifier, foreign-family, fence-fd, and
   semaphore-fd extensions unless a complete implementation later exists;
9. use an in-process render thread and portable synchronization instead of an
   external render-server process, `eventfd`, or Linux-only polling; and
10. retain Venus's thread-synchronization and asynchronous-fence guarantees.

Classify every enabled path as Metal-native, GPU-lowered, bounded CPU
control-plane work, or forbidden CPU data-plane emulation. Do not enable a
feature with unknown classification. If mandatory behavior for a proposed API
tier cannot remain Metal-native/GPU-lowered within the performance budget, cap
the advertised tier through ordinary Venus negotiation. If released guest
userspace cannot consume that cap truthfully, keep production Venus disabled.
Omit costly optional features/extensions rather than chasing a named Profile.

Audit at least `src/venus/vkr_instance.c`, `vkr_renderer.c`, allocator and
device-memory code, device/extension filtering, fence code, renderer flags,
render-server/proxy sources, protocol XML/generator inputs, and Meson feature
checks. Change generator inputs and regenerate protocol output within the
carried dependency topic commit; do not edit checked-in mail patches or
generated output outside that owning source workflow, and do not upload the
topic externally.

### 4. Implement the host and maintained-kernel Venus contract

Own this boundary end to end: extend the local device protocol and, wherever
its implementation is incomplete, patch the maintained Linux
`virtio_gpu`/UML transport. The pinned kernel already carries the standard
Venus DRM UAPI, so retain its ioctl numbers and structure layouts and implement
the behavior required by the exact released guest Mesa. Do not wait for a
later Linux release. Keep standard wire definitions aligned with the
[Virtio specification](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html),
not a repository-private approximation:

- negotiate `VIRTIO_GPU_F_VIRGL`, `VIRTIO_GPU_F_RESOURCE_BLOB`, and
  `VIRTIO_GPU_F_CONTEXT_INIT`; advertise UUID support only when its complete
  semantics exist;
- enumerate Venus capset ID 4, version 0 through capset-info/get-capset and
  return the exact virglrenderer size and fields rather than a hard-coded
  shell;
- implement `VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB`,
  `VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB`,
  `VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB`, and
  `VIRTIO_GPU_RESP_OK_MAP_INFO`;
- implement `VIRTIO_GPU_CMD_SET_SCANOUT_BLOB` only when scanout requires it;
- accept context-init capset 4, 64 rings, and poll-rings mask 0, encode capset
  4 in context creation, and retain per-file ownership;
- propagate execbuffer ring index and BO handles to submit-3D and preserve
  `VIRTIO_GPU_FLAG_INFO_RING_IDX`; and
- support the standard blob memory, flags, map-cache, and UUID values actually
  exercised by the selected released userspace, plus only the explicitly
  versioned released-Mesa/virglrenderer guest-VRAM extension described below.

Match the released Mesa DRM transcript explicitly:

| Guest operation | Required maintained-kernel/host behavior |
| --- | --- |
| `GETPARAM` | report 3D, capset-query-fix, resource-blob, and context-init only when complete; report host-visible or the exact selected-release guest-VRAM route truthfully |
| `GET_CAPS` | return Venus capset 4/version 0 with exact renderer bytes, including wire-format, extension, blob-ID-0, wait-sync, timeline, and guest-VRAM fields |
| `CONTEXT_INIT` | accept capset 4, 64 rings, and poll-rings mask 0; preserve per-file context/ring ownership |
| Ring/reply shared memory | create HOST3D + MAPPABLE + blob ID 0, then support DRM map and `mmap`; this remains required even when Vulkan device BOs use guest-VRAM |
| Device BO | use HOST3D when host-visible is supported, otherwise the exact guest-VRAM route; return accurate resource-info blob memory and rounded size |
| Submit/sync | carry execbuffer ring index and BO handles to submit-3D, preserve info-ring-index, and return a guest-local fence fd when requested |
| Close/reset | `munmap`, GEM close, context/file close, owner reset, and disconnect revoke renderer access and drain delayed responses before backing disappears |

`VIRTGPU_EXECBUF_FENCE_FD_OUT` is a Linux guest-local `sync_file` produced from
the delayed Virtio fence response. It is not MoltenVK
`VK_KHR_external_fence_fd`, must not cause that host extension to be exposed,
and must preserve the exact context, ring, and fence identity through reset.
Wire map/unmap commands apply to host-visible blobs; a guest-VRAM GEM may map
its owned backing directly, but must return the same standard DRM semantics.

For every command, validate header type, context, resource/blob ID, memory and
flag combinations, iovec/scatter lengths, offsets, plane count, stride,
integer overflow, alignment, duplicate ownership, and teardown state before
calling the renderer. An allocation/import failure must leave no guest-visible
resource and no host mapping. Unmap must be idempotent only where the protocol
says so; invalid lifetime transitions return the correct Virtio-GPU error.

Do not set `VIRTIO_GPU_F_RESOURCE_BLOB`, increment the capset count, or return
capset 4 merely because the parser has been added. Feature advertisement is a
runtime consequence of a usable Venus renderer and memory provider.

### 5. Select a memory model through an explicit spike

Venus normally assumes shareable host memory. MoltenVK does not export Linux
dma-bufs or opaque fds, and arbitrary discontiguous guest pages cannot safely
be presented as one host pointer. Select one of the following designs only
after testing its complete allocation, mapping, cache, and teardown behavior.

Before choosing device memory, implement the control-plane memory class that
the audited released Mesa Venus path always needs: ring/reply shared memory is
a small HOST3D, mappable, blob-ID-0 resource followed by the standard map ioctl
and `mmap`.
Guest-VRAM does not replace this class. Every viable design therefore supplies
(A) ring/reply shared memory and (B) Vulkan device memory through either
host-visible blobs or guest-VRAM.

#### Preferred device-memory spike: bounded guest-owned VRAM

Newer upstream Mesa contains a Venus guest-VRAM path using
`VIRTGPU_PARAM_GUEST_VRAM`, blob memory value `0x4`, and
`VkImportMemoryResourceInfoMESA`; virglrenderer 1.3.0 contains the matching
`VIRGL_RENDERER_USE_GUEST_VRAM` mode. The current Linux 6.18 guest does not
provide the complete implementation needed here, but that work is owned by
this repository and may be carried in the maintained kernel series.

For the audited released-Mesa route, getparam 9 and blob memory value `0x4`
are an exact Mesa/virglrenderer guest-VRAM extension rather than Virtio 1.3
core. Pin the selected released Mesa call transcript, do not renumber existing
UAPI, return `EINVAL` for unknown parameters or memory types, and advertise
this route only when its complete heap, resource-info, renderer, and mapping
semantics exist. A guest-VRAM heap without the separate HOST3D ring/reply map
path cannot initialize Venus.

Reserve a bounded heap within the existing contiguous UML physical-memory
backing and make its host address importable through
`VK_EXT_external_memory_host`. The design must prove:

- the pointer meets MoltenVK's `minImportedHostPointerAlignment` under both
  guest page profiles;
- the returned Vulkan memory-type bits and buffer/image compatibility are
  accurate;
- coherent and noncoherent flush/invalidate behavior is correct;
- an allocation cannot escape the reserved range or alias ordinary guest RAM;
- the heap is charged against guest RAM and remains practical at Kernarium's
  1.5 GiB default;
- allocation metadata is bounded and cannot be exhausted independently;
- reset revokes renderer access before physical guest memory is released; and
- the same kernel image discovers the host capability at runtime on all three
  products.

The host transport already retains the physical-memory descriptor and mapping
offset. Resolve only a driver-reserved, range-checked VRAM offset through that
owner; never accept an arbitrary guest pointer or reconstruct a nominally
contiguous allocation from unrelated guest pages.

The fact that a pointer imports successfully is insufficient. Exercise device
and host writes, noncoherent atom sizes, suballocation, aliasing, sparse/error
paths, and every required Vulkan buffer and image usage. Reject a route that
requires steady-state full-resource bounce buffers, CPU repacking, or copies
proportional to primary buffer/image traffic.

#### Fallback spike: standardized vhost-user shared-memory aperture

Implement a host-visible shared-memory region and the modern vhost-user
messages needed to map allocations into it. The current vhost-user
specification defines `VHOST_USER_PROTOCOL_F_SHMEM_MAP`,
`VHOST_USER_GET_SHMEM_CONFIG`, and backend `SHMEM_MAP`/`SHMEM_UNMAP` requests.
The exact message layout and negotiation must track the
[upstream vhost-user specification](https://gitlab.com/qemu-project/qemu/-/blob/master/docs/interop/vhost-user.rst).
The UML transport then supplies `.get_shm_region` to Virtio-GPU, and the guest
kernel exposes the aperture through the standard shared-memory-region path.

This design must define bounded aperture size, guest physical placement,
alignment for 4 KiB and 16 KiB guests, vhost-user reconnect behavior, mapping
IDs, permissions, cache mode, rollback, reset, and ownership when the backend
disconnects. Do not introduce a Darwin-private command if the standardized
mechanism suffices.

Choose guest-owned VRAM only if released upstream Mesa can use it without a
custom guest library, the mandatory HOST3D ring/reply mapping also works, and
every required MoltenVK allocation class works without a steady-state bounce
or repacking path. Choose the standardized aperture if that route is
incomplete, cannot cover images, requires CPU-heavy copying, or requires a
permanent downstream Mesa change. Record the rejected alternative and matched
measurements in the feasibility evidence.

### 6. Keep presentation out of the memory bring-up critical path

The first successful Venus guest is headless and proves compute plus rendered
readback. CPU transfer/readback may then serve bounded bring-up, correctness,
and optional VNC diagnostics. A displayed high-performance product requires
direct GPU-backed presentation through `VK_EXT_metal_objects` or another
published same-device interop contract.

Direct presentation must validate device identity, format, extent, planes,
layout, ownership, synchronization, and texture lifetime. The display may
retain only a renderer-owned stable reference valid through the in-flight
Metal command buffer. If a format requires sustained CPU conversion/readback
and exceeds the frozen budget, report that format or displayed mode unsupported
instead of silently falling back. Readback/VNC evidence cannot establish
native-display performance.

Reuse the GL plan's newest-frame-wins Metal presentation timeline where the
resource contract matches, while retaining separate Vulkan image-layout and
queue-ownership handling. A Metal object extracted from MoltenVK is not
automatically interchangeable with ANGLE's Metal texture export.

## In-image Vulkan CTS development and qualification strategy

The authenticated guest image already contains a VK-GL-CTS 1.4.5.5-labeled
installation and can drive the implementation from first enumeration through
the Vulkan ceiling. Its current build is immediately useful for development,
but an audited build-configuration defect and missing source/build receipt mean
it must be rebuilt before it becomes qualification evidence.

### Audited inventory and trust boundary

| Item | Guest path or measured identity |
| --- | --- |
| Executable | `/opt/vk-gl-cts/vulkan/deqp-vk`; AArch64/Linux PIE; 77,904,360 bytes; build ID `b4652c3d3923932193c272300201985e4d438863` |
| Resources and generated current leaf lists | `/opt/vk-gl-cts/vulkan/`; `amber`, `vulkan`, and `vk-default` must remain beside the executable |
| Co-located release-labeled source | `/home/uml/vk-gl-cts-1.4.5.5`; no `.git` metadata, so current identity is the 1.4.5.5 label plus rootfs provenance, not a guessed commit or proof that it produced the ELF |
| Current must-pass manifest | `/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/mustpass/main/vk-default.txt` |
| Current case universe | 2,869,003 cases across 96 manifest leaf lists |
| Mandatory fraction | `/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/mustpass/main/vk-fraction-mandatory-tests.txt`; 19 identity/mandatory-feature cases |
| Bounded frozen lane | `/opt/vk-gl-cts/vulkan/vk-main-2026-03-01/`; 266,574 cases; regression convenience only, never a substitute for the current universe |
| Required guest ICD | `/usr/share/vulkan/icd.d/virtio_icd.json`; current image declares Virtio/Venus API 1.3.305 |

The image also installs lavapipe. Every guest campaign must set
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json` and then verify the
QPA physical-device/driver identity. An LVP pass is a harness control at most;
it is never Venus/MoltenVK evidence. `deqp-vk` resolves Vulkan at runtime, so
omitting this contract can silently test the wrong ICD.

The top-level `vk-default.txt` contains ordered leaf-list filenames such as
`vk-default/api.txt`. This CTS build recognizes that group-file form and it is
the canonical value to pass to `--deqp-caselist-file` for a current full or
fraction campaign; a single referenced leaf remains valid for focused work.
Independently parse the manifest for path validation, scheduling, and evidence,
record every manifest/leaf digest, and verify coverage. Across native fractions
each non-mandatory case occurs exactly once, while the 19 cases supplied via
`--deqp-fraction-mandatory-caselist-file` intentionally repeat in every
fraction.

There is a release-blocking build-configuration defect in the provisioned
executable: `dEQP-VK.info.build` reports `DE_CPU_X86_64` even though `uname -m`
and the ELF are AArch64, reports release name `unknown`, release ID
`0xcafebabe`, and target `Default`. `DE_CPU` can select compiled behavior, so
this is more than a cosmetic metadata discrepancy. Keep using the binary for
bring-up and regression localization, but label its results development-only.
Before any qualification gate, rebuild VK-GL-CTS from authenticated,
unmodified release source configured so `info.build` reports `DE_CPU_ARM_64` and real
release metadata, then provision the binary, source, Amber/data archives,
current manifest, and all generated leaf lists as one signed whole-image
update. Re-audit the new build report, ELF, build ID, caselist counts, and
resource/list digests.

### Exact invocation and artifact contract

A current-build identity smoke has this shape:

```sh
cd /opt/vk-gl-cts/vulkan
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json
./deqp-vk \
  --deqp-caselist-file=vk-default/info.txt \
  --deqp-archive-dir=. \
  --deqp-log-filename=/run/uml-vk-cts/info.qpa \
  --deqp-log-images=disable \
  --deqp-log-shader-sources=disable \
  --deqp-test-oom=disable \
  --deqp-visibility=hidden
```

`vk-default/info.txt` and its source copy are identical flat 19-case lists.
Always run from the Vulkan directory or set `--deqp-archive-dir=.` so shaders,
Amber files, and data resolve from the matched installed tree. Keep dEQP's
watchdog/crash handlers off in the baseline command until focused UML
timer/signal/crash tests qualify them; the outer shard timeout remains
mandatory.

Use the suite's native current-manifest fraction contract for broad campaigns:

```sh
cd /opt/vk-gl-cts/vulkan
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json
./deqp-vk \
  --deqp-caselist-file=/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/mustpass/main/vk-default.txt \
  --deqp-archive-dir=/opt/vk-gl-cts/vulkan \
  --deqp-fraction=0,16 \
  --deqp-fraction-mandatory-caselist-file=/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/mustpass/main/vk-fraction-mandatory-tests.txt \
  --deqp-log-filename=/run/uml-vk-cts/current-00-of-16.qpa \
  --deqp-log-images=disable \
  --deqp-log-shader-sources=disable \
  --deqp-test-oom=disable
```

Development may defer video until presentation/video prerequisites are ready,
but final current-manifest qualification executes its leaf. Unsupported video
must be reported naturally from truthful feature discovery; advertised video
support requires its authenticated sample data. Missing required resources are
a harness failure, not permission to omit the leaf.

The runner's privileged bootstrap creates a bounded `/run/uml-vk-cts` output
directory owned by the non-root test account before dropping privileges; CTS
itself must not run as root.

Add a dedicated `tests/integration/gpu/run_vk_cts.py`, sharing safe artifact
and QPA parsing code with the GL CTS driver but not turning
`run_gpu_guest.py` into a CTS-specific monolith. Reuse the existing safe EROFS
lower/disposable OverlayFS bootstrap and process-group cleanup. The driver
must provide:

1. `smoke`, exact-family, frozen-regression, and current-must-pass modes;
2. typed manifest/leaf/shard selection, CTS-native deterministic `N/M`
   fractions plus the mandatory-fraction file, checkpoint/resume only at a
   verified shard boundary, page-profile and engine selection, an outer
   timeout, and a separately qualified dEQP watchdog;
3. forced Virtio ICD selection followed by validation of the QPA session's
   device/vendor identity and absence of LVP or another ambient ICD, plus
   parsing of `dEQP-VK.info.*` results and renderer logs for driver/API version,
   extensions, features, and portability properties;
4. one CTS process per guest, strict QPA parsing, raw QPA preservation, first
   failure and seed retention, and no-pass-on-crash, truncation, device loss,
   timeout, missing case, or unclean emulator/guest shutdown;
5. exact host revision/dirty state, product/model/OS/SDK, backend and
   dependency manifest/series/platform-contract/build-receipt evidence,
   rootfs/CTS/build ID/list digests, guest Mesa/kernel/page
   profile, virglrenderer/MoltenVK identity, shard first/last/count, result
   totals/duration, guest peak RSS/minimum `MemAvailable`, host renderer/Metal
   memory, console/journal/dmesg, QPA, and lifecycle outcome; and
6. unit tests for manifest expansion, path safety, shard coverage (non-mandatory
   exact-once plus mandatory repeated-per-fraction), QPA identity and trailer
   parsing, length/digest verification, timeout, resume, and artifact
   completeness.

The current runners preserve console/run metadata but discard the guest upper
layer. Compress each QPA in the guest and export it in framed, checksummed,
acknowledged serial chunks carrying shard name, raw/compressed lengths, and
digest, or use an explicit disposable scratch device distinct from the EROFS
root device. A missing trailer, bad length/digest, or lost artifact fails the
shard. Never write to `/dev/ubda`.

For the app products, add a narrowly validated CTS boot mode rather than a
general remote shell. The macOS launcher should select the campaign and shard
as the default development path. The iPhone 17 Pro Max Simulator launcher then
uses the same mode/configuration as the required iOS validation lane, and the
device launcher uses it unchanged on the connected iPad while returning the
framed QPA and lifecycle result. Keep long/full/device campaigns opt-in and run
only the gadget or JIT engine, never the interpreter.

### Development ladder mapped to implementation work

Run each gate first on macOS for fast iteration, repeat the identical
backend/configuration on the iPhone 17 Pro Max Simulator for iOS validation,
and require the connected iPad before the capability is qualified:

1. **Enumeration.** Run `dEQP-VK.info.*`, all 19 mandatory-fraction cases,
   `dEQP-VK.api.version_check.*`, and `dEQP-VK.api.info.*`. Prove that the
   loader hides the portability ICD without portability enumeration and
   reveals it only with the standard extension/instance flag.
2. **Instance and device.** Run basic `create_device`, `features2`, API smoke,
   and `get_device_proc_addr` groups. Verify that
   `VK_KHR_portability_subset` features/properties traverse Venus and that the
   logical device enables the required subset without injected strings or
   guest overrides.
3. **Blob/imported memory.** Run requirements, basic allocation, mapping,
   regular binding, coherent/noncoherent host-write/device-read and
   device-write/host-read cases, plus `external_memory_host` when it is
   truthfully advertised. If it is absent, test the suppression path rather
   than forcing the extension.
4. **Submission and synchronization.** Run synchronization smoke, basic
   fences, binary/timeline semaphores where exposed, queue/device idle, reset,
   repeated-context, connection-loss, and teardown stress.
5. **Focused functionality and performance canary.** Run
   `dEQP-VK.compute.pipeline.basic.copy_ssbo_single_invocation` plus one
   basic/simple render-pass draw/readback, then the matched macOS, Simulator,
   and short connected-iPad canaries against the immediately lower truthful
   tier. Do not expand broad CTS coverage for a candidate that already fails
   performance.
6. **Change-local expansion.** For a passing candidate, expand to the complete
   basic compute pipeline group and the relevant memory, image, render-pass,
   rasterization, texture, SSBO, UBO, shader, descriptor, query, and
   synchronization2 leaves. Run full leaves only when implicated; the complete
   manifest remains the final selected-tier gate.
7. **Presentation.** Run the current WSI leaf separately as the image-selected
   non-root graphical-session user: `uml` for KDE or the UID-1000 Phosh user
   under greetd/Phrog. Inherit its real `XDG_RUNTIME_DIR`,
   `WAYLAND_DISPLAY`/`DISPLAY`, authentication, and DRI permissions; do not use
   an artificial root/headless display. Keep CPU-readback and direct-Metal
   presentation results distinct.
8. **Selected ceiling.** After the candidate passes performance, expand the
   current `vk-default.txt` manifest and run every referenced leaf, perform the
   selected-tier feature/property audit (including 1.4 only if it won), and run
   the Vulkan Profiles tool only for a specifically claimed profile.

Do not pre-remove geometry, tessellation, external-handle, DRM, Android, or
other capability-sensitive cases to improve the result. The CTS must make its
decision from the advertised feature and portability state. A case using an
advertised capability may not be accepted as `NotSupported`; an optional or
portability-limited case may be `NotSupported` only when the exact spec and
that CTS release's criteria permit it. Missing geometry in MoltenVK is not a
blanket waiver.

Run the same pinned CTS tag natively against MoltenVK where practical as a
diagnostic control. A native failure points first toward MoltenVK/device/CTS;
a guest-only failure points toward Mesa Venus, protocol, shared memory,
virglrenderer, or lifecycle. Native success does not replace any guest result.

### Sharding, connected-iPad policy, and result handling

Treat manifest order as canonical. Category leaves are natural shards; split
large API, compute, shader, memory, and rendering leaves further according to
macOS iteration data and Simulator duration/peak-memory measurements. Each
shard records its first
and last case, exact count/digest, page profile, platform, and resources.

On the connected iPad retain the normal 1,536 MiB guest, no swap or disk-backed
memory workaround, and one CTS process/shard at a time. Boot a fresh guest at
each shard boundary, disable deliberate OOM tests and routine image/shader
logging, bound QPA size, export it immediately, and monitor guest/host memory,
app termination, battery, and thermal state. Treat OOM, Jetsam/app termination,
timeout, device loss, incomplete QPA, or unclean shutdown as a failed shard.
Resume only at a recorded boundary. Run every required shard under both 16 KiB
and 4 KiB profiles; macOS and Simulator timing may size shards, but cannot replace device
evidence.

Keep a development failure ledger with the exact test, status/signature,
platform/page profile, first-bad revision, owner, diagnosis, existing external
reference or repository-local defect, and expiry. Use only a pre-existing
public reference if one is already
available; otherwise link a repository-local defect record and do not open an
external issue. The ledger keeps failures visible; it never converts them to
passes. Qualification waivers are limited to exact applicable Khronos waivers
with matching vendor/device scope and an existing reference. Do not automatically consume
the large checked-in `waivers.xml`, whose entries cover unrelated vendors, and
never waive a crash, timeout, device loss, malformed QPA, unrun shard, or an
advertised/core requirement.

Use `/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/modules` and the adjacent
framework/must-pass sources read-only to find a failed test's feature predicate
and expected behavior and reduce it to an exact unmodified-case rerun. Record a
minimal reproducer and analysis only in this repository. Do not patch or
instrument the qualification CTS, change discovery/expected results/lists, or
submit/upload a fix externally. A genuine CTS defect blocks promotion until a
later released, unmodified CTS fixes it or an exact pre-existing Khronos waiver
applies. Promotion uses a correctly identified `/opt` executable and matching
current manifest/resources in one authenticated rootfs.

## Implementation phases

### Phase 0: freeze and automate the baselines

1. Record revision/dirty state, SDK/Xcode, rootfs userspace versions, pinned
   Linux base and reviewed final-tree identities, ordered kernel-topic
   manifest, resolved common configuration, both page-profile configurations,
   current macOS and iOS GPU contracts/receipts and installed provenance,
   selected Apple device, and exact commands.
2. Freeze the matched performance corpus, native MoltenVK diagnostic control,
   run length, warm-up, sample count, noise envelope, and promotion budgets
   before implementing a higher API tier. The maximum promotion thresholds are
   a 3% throughput decrease or frame-time/resource-cost increase on unaffected
   GL/Vulkan workloads; a 5% median throughput decrease or median frame-time
   increase on representative candidate-feature workloads; and a 10% increase
   in p95 frame time, host/guest CPU time, GPU time/energy, or resident memory.
   Improvements are unrestricted. Measurement uncertainty must be sufficiently
   below the applicable limit to
   establish compliance; otherwise the result is inconclusive and cannot
   promote. These thresholds may be tightened; loosening them requires an
   explicit plan revision before the candidate is measured, never a post-hoc
   exception. For the first usable Venus candidate, the campaign manifest must
   also freeze numeric minimum median throughput and maximum p95 latency,
   host/guest CPU, GPU time/energy, resident-memory, thermal-state/duration, and
   virtualization-tax limits relative to the native MoltenVK diagnostic
   control for every representative workload. Record those absolute limits
   before implementation; do not derive them from the candidate result. Zero
   sustained native-display CPU fallback, steady-state resource
   bounce/repacking, new iPad thermal escalation, and app termination are hard
   requirements.
3. Freeze the post-refactor dependency baseline: validate the central manifest
   and complete patch inventory; offline-verify the prepared `macos` and
   `ios-gpu` repositories and installed prefixes; record the two distinct
   renderer repository/commit/series identities, `-Dvenus=false`, and the
   absence of accepted Vulkan-Headers/MoltenVK/SPIRV-Cross components. Treat
   the desktop virglrenderer/CGL identity strictly as removal inventory, not a
   supported baseline, fallback, or future validation dependency. Keep the
   unmanaged local MoltenVK probe explicitly outside this baseline.
4. Preserve the current iOS ANGLE/Virgl capset 1/2 and rendered-workflow
   results as the shared GL regression schema. Archive any useful CGL results
   as historical evidence, but do not keep a CGL target or require it to remain
   green after macOS moves to the common path.
5. Turn the direct MoltenVK probe into a maintained target. Report loader and
   device API versions, portability extensions, every external-memory/fence/
   semaphore extension, memory heaps/types, import alignment, features,
   properties, queue families, formats, and Metal GPU families.
6. Build a common probe target and make macOS run the default repeatable schema
   first. Run that same probe/configuration in a separately signed iPhone 17
   Pro Max Simulator app as required iOS validation.
7. Record the connected-iPad probe at every qualification milestone. Do not
   attach to or terminate a live Kernarium guest merely to obtain host
   capabilities, and do not silently substitute another physical target.
8. The current image does not contain `vulkaninfo`. Force the Virtio ICD and
   capture the expected no-device result with `deqp-vk` information cases or a
   repository-owned machine-readable probe, plus the exact Mesa/ICD identity.
   Record LVP only as a separately labeled harness control. Add a released,
   authenticated `vulkan-tools` package only through a whole-image update if its
   additional report is worth maintaining.
9. Freeze the pinned kernel's standard Virtio-GPU/DRM layouts and the exact
   released-Mesa getparam, capset, context-init, HOST3D ring/reply blob,
   device-BO, resource-info, map/mmap, execbuffer/fence, GEM-close, and reset
   transcript. Record current failures and add a sentinel rejecting any private
   userspace ioctl or library dependency.
10. Define likely kernel topic ownership before implementation: amend an
   existing logical owner where appropriate or add a focused topic for UML
   shared-region/guest-VRAM work and independently reviewable generic driver
   changes. Record outside-port ownership/justification, overlap/extraction,
   delta, and next-version policy obligations.
11. Inventory the in-image CTS executable, source, resources, current manifest,
   mandatory fraction, leaf lists, build ID, case counts, and digests. Preserve
   the measured bad `info.build` CPU/release metadata as a test that the
   qualification rebuild must fix, and recover or recreate a source/build
   receipt rather than inferring provenance from co-location.
12. Bring up the dedicated CTS driver through manifest expansion, exact-case
   sharding, QPA export, and software-ICD rejection. It must fail closed on LVP,
   a malformed/truncated QPA, an incomplete shard, or an unclean shutdown.
13. Run the development-grade 19-case identity fraction on macOS as a harness
   baseline, then the iPhone 17 Pro Max Simulator and the connected iPad on the
   same path. These results locate infrastructure defects but cannot qualify
   Vulkan while the CTS build metadata is invalid and Venus remains absent.

Exit: macOS is the daily raw MoltenVK development baseline, the Simulator has
required same-path iOS evidence, the connected iPad has the mandatory
qualification baseline, current guest absence is reproducible, and the shared
protocol plus iOS ANGLE GPU workflows remain green. CTS identities and the
current case universe are frozen, and the campaign driver proves that it cannot
mistake LVP or an incomplete artifact for Venus success. The lower-tier and
GL-unused baselines, classification rules, and numeric stop budgets are frozen
before candidate results exist.

### Phase 1: build authenticated MoltenVK for all Apple products

1. Add MoltenVK, Vulkan-Headers, SPIRV-Cross, and every transitive source as
   exact HTTPS-repository/full-commit rows (plus declared sparse paths and an
   ordered series where needed) in `dependencies/manifest.tsv`. Use a distinct
   experimental Apple-GPU product until promotion; do not add graphics payloads
   to the generic `macos` aggregate recipe/verifier.
2. Select one iOS-capable virglrenderer repository/commit/series for the common
   product and explicitly reconcile the current desktop-macOS and UTM-iOS
   topics. Remove the old macOS virglrenderer/libepoxy/CGL graphics provider,
   CMake imports, preset selection, and runtime fallback as the common consumer
   migration lands; do not preserve a CGL migration preset.
3. Implement `build-apple-gpu.sh`, the common CMake imports/verifier, and three
   platform contracts. Produce verified macOS, Simulator, and device outputs,
   in that order, from one semantic recipe and separate prefixes. A temporary
   `build-ios-gpu.sh` forwarding shim may make the consumer migration atomic,
   but remove it before this phase exits.
4. Supply every prepared transitive source explicitly, force fully disconnected
   dependency resolution, and test the build with network unavailable. No
   downstream build step may fetch or initialize a submodule.
5. Export backend-qualified imported targets as necessary so a configure graph
   has exactly one virglrenderer/libepoxy provider. The MoltenVK target supplies
   headers, direct `PFN_vkGetInstanceProcAddr` injection, required system
   frameworks, and no ambient loader fallback.
6. Extend prefix checks for installed manifest/series/platform-contract
   identity, clean prepared sources, common recipe policy, architecture and
   Mach-O platform, minimum OS, symbols, install names, rpaths, framework
   module map, allowed payload, and dependency closure. Add negative fixtures
   for a wrong platform, stale series, hidden network input, duplicate provider,
   invalid existing prefix, and concurrent publication.
7. Produce one dynamic `MoltenVK.xcframework`, embed it once at app level, and
   verify both page-profile runtimes resolve the same install name rather than
   containing separate static copies. Stage its macOS slice first through the
   same target and loader contract used by the iOS slices.
8. Promote the experimental product to accepted `apple-gpu` membership only
   after the direct macOS, Simulator, and connected-iPad probes pass and all
   installed provenance/package gates are green.

Exit: a direct host probe creates the maximum truthful MoltenVK instance and
device from only the authenticated artifacts, first on macOS, then on the
Simulator and connected iPad, with all platform deltas recorded. The three
prefixes pass byte-level provenance and binary-shape verification, their
contracts select one common semantic recipe, the build succeeds with network
unavailable, both page runtimes resolve the same framework install name, and
the packaged app contains one MoltenVK copy.

### Phase 2: Apple-enable virglrenderer Venus

1. Enable Venus in the common Apple GPU build and make its macOS slice the
   first runtime bring-up target. Build the Simulator and device slices from
   that same target/configuration; do not create an iOS-only or macOS-only
   feature switch.
2. Add MoltenVK entry-point injection and portability enumeration.
3. Replace or conditionalize fd/process/event assumptions with the Apple
   memory provider and portable in-process thread primitives.
4. Implement exact renderer extension filtering and portability-subset
   feature/property forwarding.
5. Classify each enabled operation as Metal-native, GPU-lowered, bounded CPU
   control-plane work, or prohibited CPU data-path emulation. Add tracing that
   proves the classification without becoming part of performance builds.
6. Measure ANGLE/Metal startup, idle RSS, host CPU, shader/cache state, and GL
   throughput with Venus absent, enabled but unused, and used. Lazily create
   MoltenVK/Venus state where the single-session lifetime permits it; unused
   Vulkan support must not materially regress the primary GL path.
7. Use macOS host unit/sanitizer and guest tests as the primary iteration gate,
   then run the identical behavioral probe on the Simulator. Run the focused
   renderer/device probe on the connected iPad before phase exit.
8. Keep `-Dvenus=false` in the accepted production products until the backend
   initializes without false capability advertisement.
9. Represent the opt-in Venus state in the experimental product's common recipe
   and all three platform contracts/verifiers. Promote one configuration
   atomically; never let macOS and iOS select different renderer rows or
   render-server/helper-process modes while sharing a product name.

Exit: host-side Venus creates a MoltenVK renderer device on macOS first; the
identical configuration passes Simulator validation and the connected-iPad
device gate; and unit probes demonstrate correct extension filtering, subset
handling, path classification, GL isolation, thread shutdown, and asynchronous
fences on every product. Only then
may the exact experimental product configuration be promoted atomically into
accepted `apple-gpu` membership and platform contracts.

### Phase 3: add capset 4, blobs, and deferred completion

1. Introduce the single-owner renderer session and its GL/Venus facades; add
   tests proving one global init/cleanup pair and correct capset coexistence.
2. Add exact VIRGL/resource-blob/context-init negotiation, capset 4/version 0,
   blob/map-info commands, ring-index propagation, and resource tables with
   strict validation.
3. Add a maintained-kernel/host compatibility harness for the released Mesa
   getparam, get-caps, context-init, ring/reply blob, device-BO, execbuffer,
   fence, map/mmap, resource-info, GEM-close, and reset transcript. Keep the
   production feature bits disabled while using an explicit experimental gate
   for this bring-up.
4. Connect Venus command streams without enabling production guest-visible
   support.
5. Convert renderer fences into deferred replies/callbacks tied to stable
   connection generations. Reset and disconnect cancel or retire every
   outstanding completion before destroying resources.
6. Fuzz malformed commands, lengths, IDs, flags, rings, memory types, and state
   transitions.
7. Exercise concurrent capset 1/2 and capset 4 workloads with bounded resource
   quotas, scheduler/fence fairness, and shutdown. Vulkan may not starve,
   serialize, or inflate the ANGLE/Metal GL path beyond the frozen budgets.

Exit: synthetic tests can create, map, submit to, fence, unmap, and destroy a
blob through the complete local transport without leaks, stalls, or reset
races. Concurrent GL/Vulkan fairness stays within budget. Feature advertisement
remains off until Phase 5.

### Phase 4: implement the selected shared-memory mechanism

Run both memory spikes with small focused host/guest payloads on macOS first,
unchanged on the Simulator for iOS validation, and on the connected iPad before
making the selection final. Select one using released-userspace compatibility,
direct buffer/image coverage, coherent semantics, code size, memory overhead
at 1.5 GiB, reset safety, and cross-platform behavior. Reject any design that
works only on macOS or requires steady-state full-resource CPU bounce,
repacking, or serialization.

The kernel side is an owned Phase 4 deliverable, not a conditional external
dependency. Complete it as follows for whichever device-memory route wins:

1. Preserve the released-Mesa transcript and all existing Virtio-GPU DRM ioctl
   numbers, structures, getparams, errors, and mmap behavior. Implement the
   mandatory HOST3D/mappable/blob-ID-0 ring/reply path plus either truthful
   host-visible device blobs or the exact selected-release guest-VRAM route.
2. For guest-VRAM, reserve and charge a bounded UML heap, implement the exact
   getparam/blob-memory values, accurate resource-info memory type and rounded
   size, GEM mmap bounds, and revocation before renderer or guest-memory
   teardown. For a host-visible aperture, implement UML `.get_shm_region`, the
   selected vhost-user shared-region negotiation, standard map/unmap and
   cache-info, and the same reset guarantees.
3. Audit the likely touch points explicitly:
   `drivers/gpu/drm/virtio/virtgpu_ioctl.c`, `virtgpu_object.c`,
   `virtgpu_drv.h`, `virtgpu_vq.c`, `virtgpu_kms.c`, `virtgpu_submit.c`,
   `virtgpu_fence.c`, the applicable Virtio-GPU UAPI, and
   `arch/um/drivers/virtio_uml.c`. Modify generic driver/UAPI code only when
   the selected implementation needs it; do not create a private userspace
   dependency.
4. Author or amend the logical topic commit in the configured case-sensitive
   managed Linux checkout. Add a focused topic only when the work is
   independently reviewable. Update the reviewed final tree, delta ceilings,
   path ownership, outside-port justification, overlaps, extraction and
   next-version policy, and `kernel/README.md`, then run
   `kernel/rebase-patches.sh`; never edit generated patch or manifest identity
   bytes by hand.
5. Add mutation-pinned tooling tests for feature/capset/context/ring
   negotiation, unknown parameter and memory-type rejection, mapping bounds,
   rounded sizes, partial mmap, info-ring fences, reset, and unmap ordering.
   Ordinary DRM/UAPI changes do not advance the separate `darwin_uml_*` bridge
   ABI; follow the full `kernel/abi` version/fingerprint/sync workflow only if a
   canonical bridge declaration actually changes.
6. Replay and build both 16 KiB and 4 KiB kernels with LLVM 22. Test 4 KiB
   subpages on a 16 KiB host page, alignment, alias isolation, noncoherent
   flush/invalidate, mmap exhaustion, disconnect, and memory-pressure behavior.
7. Prove the exact 16 KiB artifact and exact 4 KiB artifact are each shared by
   macOS, Simulator, and device and select capabilities at runtime. Inclusion
   in a later external Linux release is not a product gate.

Exit: Venus initializes through the required HOST3D ring/reply mapping and
coherent buffer/image round trips use the selected direct device-memory route under
both guest page profiles on macOS and Simulator. The connected iPad passes the
required memory and performance campaign with no steady-state bounce path, and
allocation pressure, invalid maps, backend reconnect, and teardown leave no
stale host access.

### Phase 5: wait for and consume released guest-userspace portability support

This is an external release gate, not project implementation work. Do not
modify, patch, submit, or upload Mesa, libdrm, the Vulkan loader, or the Venus
ICD. Monitor independently published releases and audit a candidate unmodified
release only after it is available. It must already contain all of the
following:

- guest extension enumeration for `VK_KHR_portability_subset`;
- generated Venus codecs for
  `VkPhysicalDevicePortabilitySubsetFeaturesKHR` and
  `VkPhysicalDevicePortabilitySubsetPropertiesKHR`;
- physical-device feature/property `pNext` query handling;
- logical-device enablement and validation;
- a generated guest ICD manifest with `is_portability_driver: true` and a
  compatible manifest format, with the loader hiding the device without
  portability enumeration and revealing it with the standard flag; and
- protocol/version compatibility with renderers that do not expose the
  extension.

Do not create a locally patched Mesa prototype or custom rootfs to bypass this
gate. Host, renderer, protocol, and maintained-kernel work may continue behind
experimental feature advertisement and synthetic compatibility tests, but
honest guest enumeration, end-to-end CTS, and product qualification remain
blocked until the independently released guest userspace satisfies the list.

Exit: an independently released, unmodified Mesa contains the complete
support, and the signed EROFS image is rebuilt and provisioned as a whole with
that release. The same whole-image update contains the correctly rebuilt,
ARM64-identified pinned unmodified CTS executable and matching
source/resources/current must-pass data. Only then may capset 4 be enabled in a
production configuration or CTS results be treated as qualification evidence.

### Phase 6: headless common-path guest bring-up, macOS first

1. Enable Venus and the selected memory model without scanout on macOS. Keep
   this as the continuous guest-development lane, using only the common
   Apple-GPU provider and iOS-compatible renderer configuration.
2. Verify the guest loader selects the Virtio Venus ICD, not lavapipe or an
   accidentally installed second ICD.
3. Run allocation/map/flush/invalidate, transfer, compute, rendering,
   synchronization, query, descriptor, pipeline-cache, and reset smoke tests.
4. Capture the `dEQP-VK.info.*` QPA and the repository-owned machine-readable
   feature/property/extension report. If an authenticated released Vulkan Tools
   package was deliberately added to the rebuilt image, capture `vulkaninfo --summary`
   too; do not make an absent ambient tool a prerequisite.
5. Evaluate available core tiers in ascending order. Classify every required
   behavior and omit costly optional features. Do not expose the next tier if
   any mandatory operation is prohibited CPU emulation or cannot be capped
   truthfully through released userspace.
6. After the focused correctness sentinel, run the matched macOS performance
   canary, the same Simulator canary, and a short connected-iPad
   performance/thermal canary before broad CTS. Compare with the immediately
   lower truthful tier and the native MoltenVK diagnostic control. For the
   first Venus candidate, where no lower guest tier exists, use the native
   control plus the frozen absolute budgets; absence of an A/B baseline does
   not waive the gate.
7. Only for a candidate that passes performance, run CTS enumeration and
   instance/device gates plus the exact blob/memory, synchronization, compute,
   rendering, reset, and change-local feature families. Investigate every
   non-permitted result before increasing the claimed API tier.
8. Stress repeated device/context creation, memory pressure, connection loss,
   guest shutdown, and host renderer initialization failure.

Exit: released, unmodified guest user space reports a truthful headless
candidate tier naturally on macOS and the Simulator, and the connected-iPad
headless correctness/performance canary passes. This is not final tier
selection: direct presentation and the decisive sustained device gate remain
in Phases 7-8. A lower candidate or continued unsupported state is a valid
outcome; Simulator or macOS success alone is insufficient.

### Phase 7: add scanout and desktop integration

1. Implement bounded CPU readback only for macOS bring-up, deterministic
   damage tests, and optional VNC diagnostics. It cannot qualify native display.
2. Boot the ordinary macOS-hosted guest desktop and verify Vulkan clients
   coexist with the shared ANGLE/Virgl GL path. The macOS product must contain
   no CGL, IOSurface-GL, VNC renderer backend, or alternate renderer fallback.
   If VNC remains available, it consumes only the diagnostic
   CPU-readback/damage stream after rendering.
3. Add direct Metal-object scanout and require it for the ordinary displayed
   product after device/format/synchronization probes prove it safe. Narrow or
   reject a format/mode that would otherwise require sustained CPU copies.
4. Consume the exact scanout/resource/synchronization implementation on the
   iPhone 17 Pro Max Simulator; only the thin AppKit/UIKit view and lifecycle
   adapter may differ.
5. Run presentation and lifecycle qualification on the connected iPad.
6. Run the current WSI leaf as the selected image's real non-root graphical
   session user (`uml` for KDE or the UID-1000 Phosh user) on macOS first, the
   same-path Simulator second, and the connected iPad last. Keep headless,
   CPU-readback, and direct-Metal results separate.
7. Measure copy count and `UML_GPU_FRAME_STATS`; do not claim zero copy based
   solely on API selection.

Exit: headless and displayed Vulkan workloads render correctly; ordinary
native presentation is GPU-backed with zero sustained CPU fallback and no
stale image/layout/fence race; macOS and Simulator use the same graphics
implementation; and the connected iPad passes correctness, frame-pacing,
thermal, and performance gates. Readback remains diagnostic/VNC-only.

### Phase 8: cross-platform prequalification and connected-iPad performance gate

Use this order:

1. Continuous macOS development and acceptance under both guest page profiles,
   using the common session with isolated ANGLE/Metal GL and Venus/MoltenVK
   Vulkan facades.
2. iPhone 17 Pro Max Simulator same-path validation under both profiles.
3. Decisive physical-iOS presentation, lifecycle, sustained performance, and
   thermal qualification on the connected iPad under both page profiles before
   selecting a tier.
4. Candidate XCFramework/app packaging and external consumer checks.

On iOS verify direct MoltenVK loading, framework signing, install-name/rpath
rules, foreground/background transitions, drawable loss, memory warnings, app
interruption, clean guest shutdown, and repeated launch. Verify the final app
contains exactly one MoltenVK framework and that both page-profile runtime
frameworks carry the same load command without embedding their own copies. Run the
focused parity and presentation matrix plus the sustained lifecycle,
performance, and thermal matrix on the connected iPad before final tier
selection. Use one fresh guest for each bounded test segment and export
artifacts immediately; neither Simulator nor macOS results substitute for
device evidence. The complete current-manifest device shards are Phase 9 work
after the tier passes these decisive gates.

Exit: every platform has an independent result table and evidence bundle. A
platform that exposes fewer MoltenVK features or fails a higher-tier
performance canary retains a lower truthful candidate claim.

### Phase 9: select and qualify the highest high-performance portability tier

1. Audit each available core tier through 1.4 in ascending order against the
   exact guest Mesa, protocol XML, renderer, MoltenVK revision, execution-path
   classification, and Phase 0 performance budget. Stop at the first tier that
   cannot meet all mandatory requirements efficiently and truthfully.
2. For the highest candidate that passed focused correctness, change-local
   CTS, direct presentation/lifecycle checks, and the decisive performance gate,
   run the complete current Vulkan CTS manifest with portability expectations
   on macOS first, then on the same-path Simulator and connected iPad. Verify
   every manifest leaf/shard and its QPA, and record only exact applicable
   Khronos waivers with pre-existing published references; do not open an
   external issue. The claim cannot advance until every required iPad shard
   runs under both page profiles.
   If the full CTS exposes a core failure, first distinguish a harness or test
   infrastructure defect, an ordinary implementation defect at the current
   tier, and a mandatory behavior that is intrinsically unavailable or cannot
   meet the performance policy. Fix and retest the current tier when the defect
   is repairable without violating the policy. If the fix touches a measured
   path, rerun focused correctness, direct presentation/lifecycle, and the
   decisive performance gate before resuming the full manifest; presume a
   renderer, kernel, protocol, or dependency fix is measured unless evidence
   proves otherwise. Lower the candidate only for the last category, apply and
   verify the standard Venus downward API cap, then
   rerun focused correctness, change-local CTS, direct presentation/lifecycle,
   the decisive performance gate, and the complete manifest for the lower
   tier. Repeat until one tier passes or Venus remains unsupported.
   An extension that is core-mandatory may lower the core tier under that rule.
   If it is required only by a named Profile, drop or lower the Profile while
   retaining the passing core tier. If it is optional to both, suppress the
   extension and retest.
3. Run the Vulkan Profiles tool only for profiles whose requirements fit the
   portability device. Name a profile only when its tool/CTS results and
   representative performance workloads all pass; no Profile is preselected.
4. Keep the virglrenderer Apple loader, portability, memory, and thread changes
   solely as emulator-owned Git topics and generated dependency series. Never
   push, submit, mail, or upload them externally. Regenerate the series with
   `dependencies/rebase-patches.sh`; if a future independently published pin
   already contains equivalent behavior, verify it before optionally dropping
   the local topic. Never hand-edit generated patch bytes.
5. Retain shared ANGLE/Metal as the independent, only GL/GLES provider on
   macOS and iOS. Venus neither replaces nor backs it. No CGL, desktop-only
   Virgl, VNC renderer, Zink, ANGLE/Vulkan, or macOS-only Vulkan architecture
   remains. A VNC view, if retained, consumes only bounded diagnostic output.

Exit: the product documentation states the exact per-platform Vulkan API and
portability subset selected by correctness and performance, not a broader
profile or conformance claim. Vulkan 1.4 is reported only if it is the winning
tier; otherwise the lower high-performance result is the intended outcome.

## Repository work map

| Area | Primary files | Planned responsibility |
| --- | --- | --- |
| Virtio-GPU host protocol | `darwin/device/gpu/protocol.hpp`, `virtio_gpu_device.hpp`, `virtio_gpu_device.cpp` | VIRGL/blob/context-init feature gates, capset 4/version 0, context rings, submit/fence identity, blobs, validation, ownership, and map responses |
| Renderer session | `virgl_renderer.*`, new Venus/session sources, `darwin/device/gpu/CMakeLists.txt` | own one process-global init/cleanup pair; enforce `VirglGlFacade -> ANGLE Metal` and `VenusVkFacade -> MoltenVK` isolation; classify execution paths; preserve fairness and bounded native resources |
| vhost-user transport | `vhost_user_device.hpp`, `vhost_user_message.*`, `vhost_user_server.*`, `in_process_service.*` | shared-memory messages/aperture if selected, deferred fences, reconnect/reset generations |
| Display | `display.*`, `native_display.mm`, `native_display_ios.mm`, scanout framebuffer | require direct GPU-backed Metal presentation for native display, bound CPU readback to diagnostics/VNC, share only the presentation timeline, and remove CGL, IOSurface-GL, VNC renderer, and platform-only fallbacks |
| Common Apple GPU provenance | `dependencies/manifest.tsv`, `dependencies/prepare.sh`, `dependencies/rebase-patches.sh`, `dependencies/patches/*/series.tsv`, `dependencies/platforms/*-gpu-arm64.tsv` | own repository/full-commit/transitive-source/product identity, generated ordered topics, and checked-in platform/common-recipe contracts; no product-local manifest or downstream fetch |
| Common Apple GPU build | planned `dependencies/build-apple-gpu.sh`, `dependencies/cmake/AppleGpuDependencies.cmake`, `dependencies/cmake/VerifyAppleGpuPrefix.cmake` | emit and verify separate macOS/Simulator/device prefixes from one recipe with distinct `gl_provider=angle-metal` and `vk_provider=venus-moltenvk` fields; inject MoltenVK only into Venus, install provenance, remain offline, fail on cross-facade edges or a second provider, and retire the iOS-only builder after atomic migration |
| macOS dependency cleanup | `dependencies/manifest.tsv`, `dependencies/build.sh`, `dependencies/verify-prefix.sh`, `dependencies/cmake/UmlThirdPartyTargets.cmake` | remove desktop libepoxy/virglrenderer/CGL graphics rows, recipes, and imports while retaining only required non-graphics macOS dependencies |
| Thin product packaging | `Kernarium/scripts/package-ios-frameworks.sh`, `Kernarium/Kernarium.xcodeproj/project.pbxproj`, macOS runtime-closure verification | assemble/sign one app-level MoltenVK framework and verify thin AppKit/UIKit embed/rpath/lifecycle shells without changing or supplementing the common renderer implementation |
| Guest kernel graphics | maintained Linux `drivers/gpu/drm/virtio/virtgpu_*`, applicable Virtio-GPU DRM UAPI implementation, `arch/um/drivers/virtio_uml.c`, and UML physical/IOMEM/MM code as selected | preserve the released-userspace ioctl transcript; implement ring/reply HOST3D mapping, guest-VRAM or host-visible device memory, context/ring/fence semantics, shared-region negotiation, resource-info, mmap, and reset in one runtime-negotiated artifact per page profile |
| Kernel series and verification | `kernel/patches/series/`, `kernel/patches/metadata.env`, ownership/outside-port/overlap/extraction/next-version policy, `kernel/README.md`, and kernel tooling tests | own generated topic identities, reviewed final tree/delta, bridge-ABI changes only when necessary, offline replay, and LLVM 22 builds of both page profiles; inclusion in a later external Linux release is not a product gate |
| Rootfs | `tools/rootfs/manifest.json`, provisioning inputs and tests | consume released unmodified Mesa portability support and a correctly identified, matched CTS executable/source/resource/must-pass set as one signed whole image |
| CTS and guest tests | `tests/integration/gpu/run_vk_cts.py`, shared QPA/artifact support, `tests/unit/gpu/`, `tests/tooling/gpu/`, dependency/kernel/rootfs tooling tests, `tests/test-manifest.tsv` | manifest expansion, ICD/device identity, deterministic shards, QPA egress/parsing, protocol, memory, portability, package, lifecycle, guest, and conformance gates |
| Dependency tooling tests | `tests/tooling/dependencies/`, `tests/test-manifest.tsv` | retain source-preparer, patch-regenerator, build-manifest, prefix-verifier, and runtime-closure gates; add offline transitive-source, three-platform recipe/payload, concurrent-publication, direct-loader, and one-copy-package fixtures |
| Documentation | this plan, feasibility report, implementation plan, architecture/testing/dependency docs, evidence index | distinguish current absence, external-release-blocked state, locally carried host/kernel work, accepted portability support, and profile claims |

New first-party host renderer sources remain in `darwin/device/gpu/` and follow
the enforced device-layer dependencies. Linux implementation work remains in
the maintained kernel topic stack. Do not include implementation sources into
other C++ files. Generated Mesa/virglrenderer protocol output remains owned by
the relevant authenticated source generator.

## Validation plan

### Unit and protocol tests

Add focused coverage for:

- feature negotiation and capset ordering with Venus unavailable, available,
  and initialization-failed;
- exact capset version/size, short buffers, unknown IDs, and retry behavior;
- released-Mesa transcript compatibility for required getparams, capset
  4/version 0, context-init with 64 rings/poll mask 0, HOST3D ring/reply blob,
  resource-info, map/mmap, execbuffer ring/BO propagation, guest-local sync-file
  fence, GEM close, and reset;
- every blob command, memory/flag combination, overflow, alignment, duplicate
  ID, context ownership, rollback, and map/unmap transition;
- guest-VRAM or shared-aperture range, rounded-size, partial-mmap, 4 KiB
  subpage, alias, and unknown-parameter/memory-type validation under both page
  profiles;
- imported host-pointer alignment, compatible memory types, flush/invalidate,
  aliasing, allocation pressure, and failure cleanup;
- asynchronous fence ordering, out-of-order completion, disconnect/reset,
  renderer failure, connection generation, and shutdown races;
- portability enumeration flags, exact subset features/properties, logical
  device enablement, and rejection when the subset cannot be transported;
- filtering of every unsupported fd/dma-buf/foreign/DRM/fence/semaphore
  extension;
- strict facade isolation: GL-only workloads select ANGLE/Metal and create no
  Venus context, Vulkan instance/device, MoltenVK submission, or Vulkan-backed
  resource; capsets 1/2 work with capset 4 disabled;
- execution-path classification and negative tests for CPU shader/raster/
  geometry work, resource-sized bounce/repacking, readback-driven decisions,
  and per-operation synchronous feedback;
- GL startup/idle/throughput with Venus absent, enabled-but-unused, and active,
  plus concurrent GL/Vulkan scheduler/fence fairness and bounded memory;
- bounded diagnostic readback and direct Metal scanout format, plane, stride,
  offset, extent, layout, device identity, rejection/fallback, and in-flight
  lifetime; and
- initialization/teardown at every partial-failure point.

Use the existing GPU support/lifetime tests and add narrowly owned files when
the behavior is distinct. Keep test-only hooks in the support variant and
register every test in the manifest. Expected existing owners include
`virtio_gpu_device_lifetime_tests.cpp`, `vhost_user_protocol_tests.cpp`,
`vhost_user_server_lifetime_tests.cpp`,
`in_process_service_lifetime_tests.cpp`, and
`in_process_service_stop_wake_tests.cpp` under `tests/unit/gpu/`.

Run the narrowest tests first, then:

```sh
dependencies/prepare.sh --validate-manifest-only
dependencies/prepare.sh --product macos
dependencies/prepare.sh --product apple-gpu-vulkan-experiment

# Fast first lane: macOS uses the exact common Apple-GPU product.
dependencies/build.sh
dependencies/build-apple-gpu.sh \
  --product apple-gpu-vulkan-experiment --platform macos
dependencies/build-apple-gpu.sh \
  --product apple-gpu-vulkan-experiment --platform macos --verify-only

cmake --workflow --preset check-gpu-venus
cmake --workflow --preset check-gpu-venus-4k
cmake --workflow --preset check-gpu-venus-asan-ubsan

# Required iOS validation lane: identical recipe and backend configuration.
dependencies/prepare.sh --product ios-network
dependencies/build-ios-network.sh
dependencies/build-ios-network.sh --verify-only
dependencies/build-apple-gpu.sh \
  --product apple-gpu-vulkan-experiment --platform ios-simulator
dependencies/build-apple-gpu.sh \
  --product apple-gpu-vulkan-experiment --platform ios-simulator --verify-only
cmake --preset kernarium-ios-simulator-venus
cmake --build --preset kernarium-ios-simulator-venus
cmake --preset kernarium-ios-simulator-venus-4k
cmake --build --preset kernarium-ios-simulator-venus-4k

# Mandatory physical qualification lane.
dependencies/build-apple-gpu.sh \
  --product apple-gpu-vulkan-experiment --platform ios
dependencies/build-apple-gpu.sh \
  --product apple-gpu-vulkan-experiment --platform ios --verify-only
dependencies/build-ios-network.sh --platform ios
dependencies/build-ios-network.sh --platform ios --verify-only
cmake --preset kernarium-ios-device-venus
cmake --build --preset kernarium-ios-device-venus
cmake --preset kernarium-ios-device-venus-4k
cmake --build --preset kernarium-ios-device-venus-4k
Kernarium/scripts/package-ios-frameworks.sh
```

The Venus memory implementation necessarily changes or verifies the maintained
kernel topic stack. After authoring/amending the logical topic in the configured
case-sensitive checkout, regenerate and run the canonical gates:

```sh
kernel/rebase-patches.sh
kernel/prepare-source.sh --verify
kernel/audit-delta.sh --tree HEAD
kernel/abi/sync.sh --verify
env UML_KERNEL_CC=/opt/homebrew/opt/llvm/bin/clang \
  UML_KERNEL_HOST_CC=/opt/homebrew/opt/llvm/bin/clang \
  UML_KERNEL_LD=/opt/homebrew/bin/ld.lld \
  kernel/verify-replay.sh --work-root /Volumes/<case-sensitive-volume>
cmake --workflow --preset check-integration
cmake --workflow --preset check-integration-4k
```

The replay must build both profiles; do not use `--skip-build` for promotion.
Ordinary Virtio-GPU DRM/UAPI implementation does not advance the separate
`darwin_uml_*` bridge ABI. If a canonical bridge declaration changes, update
its version, fingerprint, identity/history, synchronized copies, kernel topic,
and undefined-import checks according to `kernel/abi/README.md` before running
the same gates.

The `apple-gpu*`, `build-apple-gpu.sh`, and `*-venus*` names are Phase 1/2
deliverables selected by this plan; they are intentionally not claims about
the current tree. After promotion, the same commands select `apple-gpu` rather
than the experimental product. A retained `build-ios-gpu.sh` entry point may
only forward to the common builder and must pass a tooling test proving it
selects the same recipe during migration; remove it before Phase 1 exits. Do
not run the old static macOS GPU aggregate or CGL
workflow as planned graphics validation. Also verify the Simulator network
prefix needed by Kernarium. Run dependency verification and architecture
audit. Because the Venus memory path includes maintained-kernel work, its
canonical LLVM 22 replay/build of both page profiles is a mandatory promotion
gate, not a conditional extra. The macOS runtime campaign is the default per-change gate; the
Simulator run is required same-path iOS validation; and connected-iPad
execution plus package verification is the required qualification gate. The
generic `ios-*` presets and root `scripts/build-xcframework.sh` do not exercise
or package this GPU path.

### Guest functional gates

On macOS, the iPhone 17 Pro Max Simulator, and the connected iPad for every
claimed platform/page-profile combination:

- verify `/dev/dri/card0` and `/dev/dri/renderD128` ownership and permissions;
- capture the kernel Virtio-GPU feature negotiation and capsets;
- run `dEQP-VK.info.*` and the repository-owned machine-readable
  feature/property dump; optionally add `vulkaninfo --summary` only when the
  authenticated image deliberately provisions Vulkan Tools;
- assert the selected ICD and physical device are Venus/MoltenVK, not lavapipe
  or another ambient ICD;
- create devices at each intended API version and test the required
  portability subset enable path;
- exercise coherent and noncoherent mapped memory, copies, compute, SSBOs,
  storage images, atomics, descriptors, dynamic rendering/render passes,
  queries, synchronization 2, timeline semaphores, pipeline cache, and
  rendering/readback;
- test allocation exhaustion, device loss, guest reset, repeated launch, and
  clean shutdown; and
- run change-local CTS families as features land and the complete current
  manifest, with verified non-mandatory exact-once and mandatory-per-fraction
  coverage plus intact artifacts, before promoting the claim.

Test unsupported extension requests and false feature combinations as
carefully as successful ones. A portability implementation is defined in part
by accurately exposing what is absent. `NotSupported` is acceptable only where
the exact portability/spec/CTS criteria permit it; it cannot excuse a core or
advertised feature. Native MoltenVK runs help classify a defect but never
replace guest results.

### Packaging and platform gates

- exact central manifest/product/repository/commit/sparse-path/ordered-series
  identity, clean prepared repositories, and byte-matched installed
  provenance;
- no build-time network, submodule, wrap, CPM, FetchContent, gclient, Xcode,
  or ambient package fallback;
- identical common semantic recipe policy across the three platform contracts,
  with only the declared target/SDK/deployment/packaging fields varying;
- explicit `gl_provider=angle-metal` and `vk_provider=venus-moltenvk` fields,
  no GL-facade dependency on the Vulkan loader/MoltenVK, and no ANGLE Vulkan or
  Zink route;
- framework architecture and Mach-O platform;
- SDK and deployment target;
- exported and hidden symbol policy;
- install names, rpaths, runtime closure, and duplicate loads;
- direct immutable MoltenVK `vkGetInstanceProcAddr`, Venus dynamic loading
  disabled, and no host Vulkan Loader/ICD/dlopen dependency;
- Venus enabled only in the selected product and render-server/helper-process
  modes disabled on every platform;
- iOS code signature, framework embed/copy phases, and external consumer
  builds;
- the same MoltenVK load command in both runtime frameworks and exactly one
  app-embedded MoltenVK copy;
- both 4 KiB and 16 KiB Kernarium artifacts using the same matched dependency
  set;
- a macOS runtime-closure audit proving the old CGL/desktop virglrenderer
  graphics libraries and targets are absent and that any VNC view consumes
  only the common renderer output; and
- ordinary Simulator and connected-iPad foreground, background, interruption,
  and memory-pressure behavior.

### Performance and resource gates

Performance is a profile-promotion gate, not a report written after full CTS.
For every candidate tier, run renderer identity and focused correctness first,
then a matched macOS A/B canary, the identical Simulator canary, and a short
connected-iPad performance/thermal canary. Only a candidate that passes those
steps proceeds to change-local CTS, direct-presentation/lifecycle checks, and
the decisive sustained connected-iPad performance/thermal gate. The complete
current manifest runs only after all of those performance gates pass; post-CTS
device work confirms stability and packaging rather than deferring tier
selection.

Measure:

- command throughput and host/guest CPU;
- pipeline/shader creation latency and cache reuse;
- mapped-memory bandwidth and flush/invalidate cost;
- resident guest/host memory and bounded heap/aperture overhead;
- fence latency and command-thread occupancy;
- direct Metal presentation copies/frame rate and any diagnostic readback; and
- clean shutdown time under idle, active, and device-loss states.

Use macOS as the primary iterative correctness/benchmark environment, the
Simulator as required same-path iOS validation, and the connected iPad for
mandatory final memory, thermal, stability, and performance evidence. macOS
measurements guide iteration but are not iOS performance evidence; do not
compare the platforms as identical hardware. Sanitizer and diagnostic builds
are not performance evidence.

Use interleaved repeated runs against the immediately lower truthful Venus
tier with the same revision, resolution, workload, memory, page profile, and
device. For the first usable Venus candidate, where no lower guest tier
exists, use the native MoltenVK diagnostic control plus the frozen absolute
budgets and execution-path classification; absence of a guest A/B does not
waive the gate. The native control is never a substitute for the guest. Apply
the Phase 0 budgets without hiding a failing primary workload in an aggregate:

- unaffected GL/Vulkan workloads lose no more than 3% throughput and add no
  more than 3% frame time or measured resource cost;
- representative candidate-feature median throughput decreases or median
  frame time increases by no more than 5%, while p95 frame time, host/guest CPU
  time, GPU time/energy, and resident memory increase by no more than 10%;
  improvements are unrestricted, and measurement uncertainty must be
  sufficiently below the applicable threshold or the result is inconclusive;
- no promoted path uses CPU work proportional to vertices, primitives, pixels,
  samples, invocations, texels, or resource bytes, steady-state resource bounce
  or repacking, or sustained native-display readback; and
- no worse sustained iPad thermal state, throttling, Jetsam/app termination, or
  lifecycle failure is permitted.

An inconclusive result does not promote. CTS or Profiles-tool success never
overrides a performance failure; select the lower API tier or omit the costly
optional feature.

## Claim ladder

Use these exact claim boundaries in reports and product documentation:

1. **“MoltenVK and Venus build.”** Build/link/package evidence only.
2. **“The guest enumerates Venus Vulkan X.Y.”** Loader, capset, instance, and
   physical-device discovery only.
3. **“Basic guest Vulkan works.”** Memory, compute, rendering,
   synchronization, and readback smoke passed.
4. **“Guest Vulkan X.Y portability subset is supported.”** The subset is
   exposed, queried, enabled, and validated, and the agreed functional/CTS
   coverage and frozen performance budget passed on that named platform.
5. **“Named Vulkan Profile P is supported.”** The Vulkan Profiles tool and all
   profile requirements, CTS coverage, and representative performance budget
   passed for that exact profile and platform.
6. **“Vulkan conformant.”** Use only with the applicable Khronos conformance
   status; neither a version string nor local CTS success grants this claim.

The word “maximum” in this plan means the highest truthful intersection of
released guest Mesa, negotiated Venus protocol, host renderer, MoltenVK, and
the tested Apple GPU after the owned kernel layer implements the compatible
Virtio-GPU/DRM contract. The current kernel implementation is not treated as a
frozen external ceiling. The performance gate is part of that intersection.
“Maximum” does not mean the highest version constant compiled into one layer.

## Risk register and stop conditions

| Risk | Mitigation or stop condition |
| --- | --- |
| no released Mesa provides the required portability support | keep production Venus disabled; do not create a custom guest or external submission workaround, and report the external release gate explicitly |
| GL/GLES reaches Venus, a Vulkan loader, ANGLE/Vulkan, Zink, or MoltenVK | fail the product graph and runtime route; capsets 1/2 must terminate at ANGLE/Metal while only capset 4 reaches MoltenVK |
| a required Vulkan behavior executes data-parallel work on the CPU | reduce the advertised API tier or remain unsupported; do not use CPU shader/raster/geometry work, resource-sized conversion, or readback-driven command decisions |
| GPU-backed lowering exceeds the frozen performance budget | select the immediately lower tier or omit the optional feature even when CTS passes |
| MoltenVK host-pointer import cannot cover required images | reject guest-VRAM design and implement the standardized shared-memory aperture or another released-userspace-compatible memory model |
| a guest-VRAM-only design omits mandatory Venus ring/reply shared memory | require the separate HOST3D, mappable, blob-ID-0 map/mmap path before Mesa initialization can pass |
| memory works on 4 KiB but aliases on a 16 KiB Apple host | require alignment/range tests under both profiles before feature negotiation is enabled |
| a private or renumbered DRM UAPI makes the rootfs depend on custom userspace | stop promotion; pin the released-Mesa transcript, preserve existing ioctl layouts/semantics, reject unknown values, and keep project-specific details below the userspace boundary |
| a guest-local sync-file fence is mistaken for a MoltenVK external-fence fd | test the delayed Virtio response and exact context/ring/fence identity locally; keep unsupported host Vulkan fd extensions filtered |
| capset/blob/context-init is partially advertised | fail closed unless the complete driver, ring/reply memory, device-memory, fence, reset, and renderer path is usable |
| a kernel patch is hand-edited, stale, or loses outside-port ownership policy | author the managed Git topic, regenerate with `kernel/rebase-patches.sh`, update final-tree/delta/ownership metadata, and require clean LLVM 22 replay/builds of both profiles |
| a dependency, CTS, or kernel change is accidentally sent outside this repository | keep prepared sources remote-less, provide no push/submission step, review only checked-in carried series and local defect records, and fail documentation/tooling checks on outbound workflow instructions |
| a Venus Linux fd assumption remains hidden | run exact extension filters and failure injection; no fd-like extension is advertised without complete semantics |
| asynchronous completion outlives reset or guest RAM | generation-tag callbacks and drain/cancel them before renderer/memory destruction; sanitizer stress is mandatory |
| macOS convenience causes renderer/configuration drift from iOS | one common Apple renderer target/configuration, macOS-first execution followed by mandatory Simulator validation, configuration-diff/runtime-closure gates, and no macOS-only GL/Vulkan provider, resource, capset, or fallback backend |
| Simulator succeeds but the physical GPU/lifecycle differs | require the full connected-iPad memory, lifecycle, presentation, and CTS matrix under both page profiles; no substitution |
| direct Metal scanout creates a cross-device or stale texture | retain readback only for diagnosis while fixing or narrowing the direct path; no displayed high-performance claim until device identity and lifetime/synchronization tests pass with zero sustained CPU fallback |
| Vulkan 1.4 string is mistaken for a profile | use the claim ladder and machine-readable feature/profile reports in every evidence bundle |
| performance passes only on macOS or Simulator | do not promote; the connected iPad performance/thermal gate is mandatory and non-substitutable |
| enabling unused Venus regresses ANGLE/Metal GL | lazily initialize Vulkan state where possible and compare GL startup, idle RSS/CPU, caches, and throughput with capset 4 absent, enabled-unused, and active |
| concurrent Vulkan starves or serializes GL | require bounded memory, scheduler/fence fairness, and matched concurrent workload tests before enabling both capsets together |
| connected iPad exceeds the physical-device memory policy | bound VRAM/aperture and CTS shards; retain the normal 1.5 GiB guest and no disk-swap workaround |
| LVP or another ICD creates a false guest success | force `VK_DRIVER_FILES` to the Virtio ICD and validate the QPA device/driver identity before counting a case |
| current CTS binary is mistaken for qualification-grade | retain it for development, but require a whole-image rebuild with ARM64 CPU and pinned release metadata plus matching resources/lists before qualification |
| a large CTS run truncates, OOMs, or loses its QPA | one bounded shard per fresh iPad guest, framed checksummed artifact egress, exact non-mandatory/mandatory-fraction coverage verification, and hard failure on OOM, device loss, missing trailer, or incomplete shard |
| platform contracts still select different renderer identities or feature flags | one `apple-gpu` product, one selected virglrenderer row/series and semantic recipe, byte-verified contracts, and a three-platform configuration-diff test before promotion |
| execution-path classification is unknown | keep the feature/tier disabled until tracing proves Metal-native, GPU-lowered, or bounded CPU control-plane behavior |
| MoltenVK/ANGLE CMake, CPM, submodules, or another tool downloads an undeclared transitive source | manifest every transitive repository/commit/path, inject prepared sources explicitly, force disconnected mode, and run builders with network unavailable |
| the retired desktop macOS graphics provider remains reachable | remove its virglrenderer/libepoxy/CGL rows, imported targets, preset selection, and runtime fallback from the macOS graphics graph; fail configure and runtime-closure tests if anything except the common `apple-gpu` provider is reachable |
| `vkGetInstanceProcAddr` resolves from an ambient Vulkan Loader | disable host Vulkan dynamic loading, inject the exact MoltenVK pointer before initialization, audit the runtime closure, and fail if a loader/ICD/dlopen path appears |
| static MoltenVK is duplicated in both page-profile runtimes | use one dynamic app-level framework and verify identical runtime load commands plus exactly one embedded copy |
| two virglrenderer/libepoxy providers or sessions enter one process | expose only the common `apple-gpu` provider family to every Apple graphics target, fail on a second provider, and retain one process-global renderer session owner |
| experimental Vulkan dependencies contaminate an accepted prefix | use distinct product membership, prefixes, platform contracts, targets, and package checks until Simulator and connected-iPad promotion gates pass |
| generated dependency patch bytes are edited manually | author/rebase topic commits and regenerate series with `dependencies/rebase-patches.sh`; validate order, stable IDs, SHA-256, subjects, and topology |

## Completion criteria

The plan is complete only when all of the following are true:

1. One central `apple-gpu` product selects one reviewed virglrenderer,
   MoltenVK, Vulkan-Headers, SPIRV-Cross, and transitive-source identity and
   emits three verified platform slices from one semantic recipe. macOS,
   Simulator, and connected iPad use that one Venus-to-MoltenVK
   implementation—not merely equal version strings—with one renderer session,
   direct immutable loader injection, memory abstraction, capset policy,
   ordered topics, and feature configuration. Its contracts record distinct
   `gl_provider=angle-metal` and `vk_provider=venus-moltenvk` routes.
2. virglrenderer enumerates portability devices, transports and enables the
   portability subset, filters unsupported external handles, and shuts down
   cleanly in process.
3. Virtio-GPU capset 4, blob resources, map/unmap, deferred fences, and reset
   semantics pass validation and sanitizer stress.
4. The selected shared-memory model is bounded, coherent, safe under both
   guest page profiles, and directly usable for every required buffer/image
   class without steady-state full-resource CPU bounce or repacking.
5. Every required Linux graphics change is carried in the generated maintained
   patch series, replays to the reviewed final tree, and builds both canonical
   page profiles with LLVM 22. The exact released-Mesa getparam/capset/context/
   ring-reply/device-BO/map/mmap/execbuffer/fence/close transcript passes with
   no private userspace dependency, and the same per-profile kernel artifact
   runs on macOS, Simulator, and device.
6. An independently released, unmodified Mesa containing the required Venus
   portability work is installed through the authenticated whole-image
   workflow; no private guest graphics patch or version override is present.
7. macOS is the maintained first development/acceptance target, and an
   unmodified guest naturally enumerates and uses the selected
   performance-qualified MoltenVK portability tier there. The iPhone 17 Pro
   Max Simulator then passes the identical configuration as mandatory iOS
   validation.
8. The connected iPad completes the full agreed correctness, performance,
   memory, thermal, presentation, and lifecycle qualification for both 4 KiB
   and 16 KiB profiles. Simulator or macOS success cannot satisfy this
   criterion.
9. The qualification rootfs contains a correctly ARM64-identified pinned CTS
   build with matching source/resources/current must-pass data. Every required
   manifest shard completes; every non-mandatory case occurs exactly once and
   the 19 mandatory cases occur in each native fraction; and every required QPA
   and lifecycle artifact is intact on macOS, the same-path Simulator, and the
   connected iPad under both page profiles.
10. Headless functionality, isolated ANGLE/Metal GL, GPU-backed native scanout,
   lifecycle, memory pressure, current CTS, architecture audit, and the
   dedicated macOS-first, Simulator-validation, and connected-iPad workflows
   pass at the agreed scope. All dependency preparation, patch-regeneration,
   offline-build, three-prefix,
   provenance, package, and runtime-closure tests pass; there is no host Vulkan
   Loader/dlopen fallback and no macOS CGL, desktop renderer, or VNC renderer
   provider. The GL facade never reaches MoltenVK, and a GL-only workload
   creates no Vulkan state. Any VNC view consumes only diagnostic output;
   ordinary native display has zero sustained CPU fallback; both page runtimes name
   the same MoltenVK install name, and the app embeds exactly one MoltenVK
   framework.
11. `dependencies/README.md`, `kernel/README.md`, `docs/testing.md`, product
   documentation, kernel patch evidence, and graphics evidence use only the
   centralized dependency and maintained-kernel paths, name the exact API
   version and subset per platform, contain no retired Kernarium dependency
   owner, and make no unsupported Vulkan Profile or conformance claim.
12. No workflow submits or uploads changes outside this emulator repository.
    Every third-party host change and Linux change is reproducible from the
    checked-in dependency/kernel patch series; guest Mesa/libdrm/loader/ICD and
    qualification CTS remain independently released and unmodified.
13. Every promoted operation is classified as Metal-native, GPU-lowered, or
    bounded CPU control-plane work. No promoted feature uses CPU-heavy
    data-path emulation, and checked-in matched evidence records both accepted
    and rejected API tiers against the frozen performance budgets. The exposed
    API and any named Profile are no higher than the last tier that passed.

Until those gates pass, the accurate production statement is: **guest Vulkan
through Venus and MoltenVK is unsupported on iOS Simulator, connected iPad,
and macOS; up to Vulkan 1.4 plus portability subset on one iOS-targeted shared
path, developed on macOS first and qualified on Simulator plus iPad, is the
conditional architectural ceiling if an independently released guest Mesa
provides the required portability support. The shipping tier may be lower when
that is the highest result meeting the performance policy.**
