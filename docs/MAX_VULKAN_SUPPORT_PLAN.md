# Maximum guest Vulkan support through Venus and MoltenVK on Apple hosts

Written 2026-08-20. This is an implementation plan, not a capability,
Vulkan Profile, or conformance claim. It builds on the measured feasibility
results in [GPU_BACKEND_FEASIBILITY.md](GPU_BACKEND_FEASIBILITY.md) and assumes
that production guest graphics libraries must be unmodified upstream releases.

Dependency-management assumptions were revised after the 2026-08-20
centralization under `dependencies/`; paths and workflows in this document
refer to that post-refactor ownership model.

## Decision summary

Build one in-process Venus renderer over MoltenVK for macOS, iOS Simulator,
and iOS device, with iOS remaining the primary product target. “One renderer”
means the same renderer session, loader injection, memory-provider abstraction,
capset behavior, source patches/configuration, ANGLE/Virgl GL fallback, and
Metal presentation model. macOS must not retain a CGL renderer, desktop-only
Virgl provider, alternate Vulkan configuration, or graphics fallback. Only
thin AppKit/UIKit view/lifecycle shells and an optional macOS VNC view may
differ; VNC consumes the common CPU-readback/damage output and cannot own a
context, provider, capset, or fallback.

Use macOS first for protocol, unit, sanitizer, memory, guest, CTS, and display
iteration because it has the shortest build/run/debug loop. Every macOS result
then runs unchanged on the iPhone 17 Pro Max Simulator as the required iOS
validation lane. Full qualification requires the complete agreed device matrix
on the connected iPad; neither macOS nor Simulator evidence substitutes for
it.

The target is **Vulkan 1.4 plus `VK_KHR_portability_subset`**, bounded on each
platform by the minimum of the released guest Mesa Venus ICD, the Venus
protocol/renderer, MoltenVK, and the tested Apple GPU. It is not a standard
conformant Vulkan 1.4 claim and is not a named Vulkan Profile unless the exact
profile tool and required tests pass.

There are two important qualifications:

1. The current repository has no usable guest Vulkan path on any Apple
   product. Venus is disabled, capset 4 and blob resources are absent, and the
   required guest/host memory-sharing mechanism is not implemented.
2. Current upstream Mesa Venus does not transport or advertise
   `VK_KHR_portability_subset`. MoltenVK is a portability driver, so an honest
   production path with the upstream-only guest policy is blocked until that
   support lands upstream and appears in a released Mesa. Lowering the core
   API version to 1.3 or earlier does not remove the portability-subset
   obligation.

The recommended critical path is:

```text
reproducible host probes
    -> create the common Apple-GPU dependency product and authenticated MoltenVK slices
    -> Apple-capable virglrenderer Venus
    -> capset 4, blob resources, and asynchronous fences
    -> select and implement the shared-memory model
    -> upstream Mesa Venus portability-subset support
    -> consume a released unmodified Mesa
    -> headless macOS guest bring-up on the iOS graphics path
    -> macOS scanout and desktop integration on that path
    -> iPhone 17 Pro Max Simulator same-path validation
    -> connected-iPad full qualification
    -> claim only the tested Vulkan 1.4 portability result
```

## Scope and invariants

The intended API path is:

```text
guest Vulkan application
    -> upstream Vulkan loader
    -> released upstream Mesa Venus ICD
    -> upstream virtio_gpu DRM/KMS
    -> UML Virtio/vhost-user transport
    -> VirtioGpuDevice
    -> virglrenderer Venus
    -> directly linked MoltenVK
    -> Metal
```

Native presentation is a consumer of this path, not part of the first
headless bring-up gate. The shared ANGLE/Virgl GL renderer remains available
while Venus is developed on every Apple platform. The current macOS CGL path
is removal input only: archive any useful before/after evidence, then remove
its product target, dependency provider, runtime selection, and fallback from
the planned graphics graph.

The following constraints are non-negotiable:

1. iOS is the primary product target, while macOS is the first development
   lane. The renderer, memory, synchronization, and packaging architecture must
   satisfy Simulator/device public APIs, signing, sandbox, lifecycle, and the
   1.5 GiB device-memory policy. macOS executes that exact design and cannot
   add a desktop-only GL/Vulkan provider, resource, capset, or fallback path;
   AppKit or VNC may only consume the common presentation output.
2. Production uses a released, unmodified upstream Mesa Venus ICD. A local
   Mesa patch is permitted only in a clearly labeled development rootfs while
   preparing and validating an upstream change; it cannot satisfy the release
   gate.
3. Do not hide MoltenVK's portability status, inject extension strings, force
   a Vulkan version, or omit required portability features/properties. The
   guest must enumerate, query, and enable the subset honestly.
4. Do not advertise Venus capset 4, blob resources, a memory type, or an
   external-handle extension until the complete behavior exists.
5. Preserve the standard Virtio-GPU/Venus command stream and upstream Vulkan
   loader model. Do not replace it with an app-specific RPC, guest preload, or
   custom Vulkan library.
6. Keep one relocatable Linux image for Simulator, device, and macOS, with the
   existing 4 KiB and 16 KiB page profiles. Any required Linux changes belong
   in the maintained common patch series, not a platform-specific kernel.
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

The maximum GL/GLES work is specified separately in
[MAX_GL_GLES_SUPPORT_PLAN.md](MAX_GL_GLES_SUPPORT_PLAN.md). Zink may consume
the finished Vulkan path, but Zink results must be reported as GL results and
must not stand in for native Vulkan qualification.

## Baseline, ceilings, and claim boundaries

### Current component state

| Layer | Current state | Consequence |
| --- | --- | --- |
| Dependency authority | one central Git-only `dependencies/manifest.tsv`, product selectors, prepared repositories, ordered mail-patch series, platform contracts, builders, CMake imports, and prefix verifiers | all Vulkan host inputs and transitive sources must extend this model; retired Kernarium-local/archive workflows are not valid owners |
| Repository device protocol | Virgl capsets 1/2; no Venus capset 4 or blob commands | guest cannot discover or use Venus |
| Current renderer split (historical removal input) | macOS selects upstream virglrenderer 1.3.0 plus CGL from the static desktop aggregate, while iOS selects the UTM virglrenderer/ANGLE topic through `dependencies/build-ios-gpu.sh`; both pass `-Dvenus=false` | this divergence must be deleted, not preserved as a migration preset or fallback; the common Apple-GPU product becomes macOS's only graphics provider |
| Accepted Vulkan host dependencies | no Vulkan-Headers, MoltenVK, or SPIRV-Cross manifest row, prefix payload, imported target, platform component, or package slice | the local MoltenVK build cannot enter a production claim; a new common Apple-GPU product is required |
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
| Prototype with a locally patched guest Mesa | development only | development only | development only | validates the proposed upstream protocol; explicitly custom guest |
| Future released Mesa carrying portability support, current rootfs otherwise upgraded | target 1.4 + portability subset on the first development lane | same nominal shared-path target; required iOS validation | same nominal target; mandatory device qualification | report the minimum API/version/features actually measured on each platform |
| Host emulates every MoltenVK portability deviation | theoretical standard Vulkan | theoretical | theoretical | a much larger conformance project; not this plan's primary route |

The current rootfs's 1.3.305 manifest is not a useful reason to target Vulkan
1.3. The implementation should be version-agnostic through 1.4, then update
the signed whole rootfs to a released upstream Mesa containing both Vulkan 1.4
and the required portability work. If the released components intersect at a
lower major/minor version, claim that lower result and record the instance,
requested-instance, and physical-device API values separately. Mesa 25.1's
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
    VirglGlFacade         capsets 1 and 2, GL contexts/resources
    VenusVkFacade         capset 4, Vulkan contexts/resources

GpuNativeResource
    MetalTexture
    CpuReadback
    Unsupported
```

The session interface should cover initialization, capset enumeration,
context/resource dispatch, command submission, transfer/readback, fence
creation and completion, reset, and optional native presentation. The facades
must not call global cleanup independently, and the interface must not leak
Vulkan or Objective-C types into platform-neutral public headers. If the
selected virglrenderer revision cannot initialize both renderers together,
make GL and Venus mutually exclusive per run until that upstream limitation is
fixed; do not claim concurrent capsets in that configuration.

`VirtioGpuDevice` continues to own Virtio request validation, resource IDs,
scanouts, transport replies, and reset ordering. Renderer callbacks carry
stable IDs, not pointers into a command buffer or a destroyed guest mapping.
Capsets 1/2 remain available through the common ANGLE/Virgl GL path on all
three platforms; capset 4 is returned only after the Venus backend and selected
memory model initialize successfully. There is no macOS CGL capset provider or
runtime fallback.

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
virglrenderer source, submit those commits upstream, and use
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

Audit at least `src/venus/vkr_instance.c`, `vkr_renderer.c`, allocator and
device-memory code, device/extension filtering, fence code, renderer flags,
render-server/proxy sources, protocol XML/generator inputs, and Meson feature
checks. Change generator inputs and regenerate protocol output within the
upstream topic commit; do not edit checked-in mail patches or generated output
outside that owning source workflow.

### 4. Add the standard Virtio-GPU Venus surface

Extend the local device protocol from its current 19-command subset with the
standard definitions required by the selected released kernel and Mesa. Keep
the wire definitions aligned with the
[Virtio specification](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html),
not a repository-private approximation:

- `VIRTIO_GPU_F_RESOURCE_BLOB`;
- Venus capset ID 4;
- `VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB`;
- `VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB`;
- `VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB`;
- `VIRTIO_GPU_RESP_OK_MAP_INFO`;
- `VIRTIO_GPU_CMD_SET_SCANOUT_BLOB` when scanout is enabled; and
- the standard blob memory, flags, map-cache, and UUID values actually used by
  the selected upstream stack.

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

#### Preferred spike: bounded guest-owned VRAM

Newer upstream Mesa contains a Venus guest-VRAM path using
`VIRTGPU_PARAM_GUEST_VRAM`, blob memory value `0x4`, and
`VkImportMemoryResourceInfoMESA`; virglrenderer 1.3.0 contains the matching
`VIRGL_RENDERER_USE_GUEST_VRAM` mode. The current Linux 6.18 guest does not
provide the complete ABI/path needed here.

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
paths, and every required Vulkan buffer and image usage.

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
custom guest library and every required MoltenVK allocation class works.
Choose the standardized aperture if that path is incomplete, cannot cover
images, or requires a permanent downstream Mesa change. Record the rejected
alternative and measurements in the feasibility evidence.

### 6. Keep presentation out of the memory bring-up critical path

The first successful Venus guest is headless and proves compute plus rendered
readback. Then implement display in two stages:

1. validated CPU transfer/readback into the existing scanout framebuffer; and
2. optional zero-copy `MTLTexture` presentation through
   `VK_EXT_metal_objects` or another upstream, same-device interop contract.

Direct presentation must validate device identity, format, extent, planes,
layout, ownership, synchronization, and texture lifetime. The display may
retain only a renderer-owned stable reference valid through the in-flight
Metal command buffer. Unsupported formats fall back to CPU transfer without
silently dropping the frame.

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
Before any qualification gate, rebuild VK-GL-CTS from an authenticated pinned
upstream revision configured so `info.build` reports `DE_CPU_ARM_64` and real
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
5. **Compute.** Start with
   `dEQP-VK.compute.pipeline.basic.copy_ssbo_single_invocation`, expand to the
   complete basic pipeline group, and then run `vk-default/compute.txt`.
6. **Rendering.** Start with basic/simple render-pass draw and readback, then
   image, renderpass/renderpasses, rasterization, texture, SSBO, UBO, shader,
   descriptor, query, and synchronization2 leaves. Complete all non-WSI
   current leaves before presentation becomes the blocker.
7. **Presentation.** Run the current WSI leaf separately as the image-selected
   non-root graphical-session user: `uml` for KDE or the UID-1000 Phosh user
   under greetd/Phrog. Inherit its real `XDG_RUNTIME_DIR`,
   `WAYLAND_DISPLAY`/`DISPLAY`, authentication, and DRI permissions; do not use
   an artificial root/headless display. Keep CPU-readback and direct-Metal
   presentation results distinct.
8. **Ceiling.** Expand the current `vk-default.txt` manifest and run every
   referenced leaf, perform the Vulkan 1.4 feature/property audit, and run the
   Vulkan Profiles tool only for a specifically claimed profile.

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
platform/page profile, first-bad revision, owner, diagnosis, upstream issue,
and expiry. It keeps failures visible; it never converts them to passes.
Qualification waivers are limited to exact applicable Khronos waivers with
matching vendor/device scope and upstream issue. Do not automatically consume
the large checked-in `waivers.xml`, whose entries cover unrelated vendors, and
never waive a crash, timeout, device loss, malformed QPA, unrun shard, or an
advertised/core requirement.

Use `/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/modules` and the adjacent
framework/must-pass sources read-only to find a failed test's feature predicate
and expected behavior, reduce it to an exact-case rerun, and prepare an upstream
CTS fix. Instrumented builds in disposable macOS/Simulator storage are valid
diagnostic tools but must be labeled custom CTS. Do not patch discovery,
expected results, or lists to make Venus pass. A CTS defect must be upstreamed
and consumed from a later pinned release; promotion uses an unmodified,
correctly identified `/opt` executable and matching current manifest/resources
in a whole authenticated rootfs.

## Implementation phases

### Phase 0: freeze and automate the baselines

1. Record revision/dirty state, SDK/Xcode, kernel and rootfs versions, the
   current macOS and iOS GPU contracts/receipts and installed provenance,
   selected Apple device, and exact commands.
2. Freeze the post-refactor dependency baseline: validate the central manifest
   and complete patch inventory; offline-verify the prepared `macos` and
   `ios-gpu` repositories and installed prefixes; record the two distinct
   renderer repository/commit/series identities, `-Dvenus=false`, and the
   absence of accepted Vulkan-Headers/MoltenVK/SPIRV-Cross components. Treat
   the desktop virglrenderer/CGL identity strictly as removal inventory, not a
   supported baseline, fallback, or future validation dependency. Keep the
   unmanaged local MoltenVK probe explicitly outside this baseline.
3. Preserve the current iOS ANGLE/Virgl capset 1/2 and rendered-workflow
   results as the shared GL regression schema. Archive any useful CGL results
   as historical evidence, but do not keep a CGL target or require it to remain
   green after macOS moves to the common path.
4. Turn the direct MoltenVK probe into a maintained target. Report loader and
   device API versions, portability extensions, every external-memory/fence/
   semaphore extension, memory heaps/types, import alignment, features,
   properties, queue families, formats, and Metal GPU families.
5. Build a common probe target and make macOS run the default repeatable schema
   first. Run that same probe/configuration in a separately signed iPhone 17
   Pro Max Simulator app as required iOS validation.
6. Record the connected-iPad probe at every qualification milestone. Do not
   attach to or terminate a live Kernarium guest merely to obtain host
   capabilities, and do not silently substitute another physical target.
7. The current image does not contain `vulkaninfo`. Force the Virtio ICD and
   capture the expected no-device result with `deqp-vk` information cases or a
   repository-owned machine-readable probe, plus the exact Mesa/ICD identity.
   Record LVP only as a separately labeled harness control. Add upstream
   `vulkan-tools` only through an authenticated whole-image update if its
   additional report is worth maintaining.
8. Inventory the in-image CTS executable, source, resources, current manifest,
   mandatory fraction, leaf lists, build ID, case counts, and digests. Preserve
   the measured bad `info.build` CPU/release metadata as a test that the
   qualification rebuild must fix, and recover or recreate a source/build
   receipt rather than inferring provenance from co-location.
9. Bring up the dedicated CTS driver through manifest expansion, exact-case
   sharding, QPA export, and software-ICD rejection. It must fail closed on LVP,
   a malformed/truncated QPA, an incomplete shard, or an unclean shutdown.
10. Run the development-grade 19-case identity fraction on macOS as a harness
   baseline, then the iPhone 17 Pro Max Simulator and the connected iPad on the
   same path. These results locate infrastructure defects but cannot qualify
   Vulkan while the CTS build metadata is invalid and Venus remains absent.

Exit: macOS is the daily raw MoltenVK development baseline, the Simulator has
required same-path iOS evidence, the connected iPad has the mandatory
qualification baseline, current guest absence is reproducible, and the shared
protocol plus iOS ANGLE GPU workflows remain green. CTS identities and the
current case universe are frozen, and the campaign driver proves that it cannot
mistake LVP or an incomplete artifact for Venus success.

### Phase 1: build authenticated MoltenVK for all Apple products

1. Add MoltenVK, Vulkan-Headers, SPIRV-Cross, and every transitive source as
   exact HTTPS-repository/full-commit rows (plus declared sparse paths and an
   ordered series where needed) in `dependencies/manifest.tsv`. Use a distinct
   experimental Apple-GPU product until promotion; do not add graphics payloads
   to the generic `macos` aggregate recipe/verifier.
2. Select one iOS-capable virglrenderer repository/commit/series for the common
   product and explicitly reconcile the current upstream-macOS and UTM-iOS
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
5. Use macOS host unit/sanitizer and guest tests as the primary iteration gate,
   then run the identical behavioral probe on the Simulator. Run the focused
   renderer/device probe on the connected iPad before phase exit.
6. Keep `-Dvenus=false` in the accepted production products until the backend
   initializes without false capability advertisement.
7. Represent the opt-in Venus state in the experimental product's common recipe
   and all three platform contracts/verifiers. Promote one configuration
   atomically; never let macOS and iOS select different renderer rows or
   render-server/helper-process modes while sharing a product name.

Exit: host-side Venus creates a MoltenVK renderer device on macOS first; the
identical configuration passes Simulator validation and the connected-iPad
device gate; and unit probes demonstrate correct extension filtering, subset
handling, thread shutdown, and asynchronous fences on every product. Only then
may the exact experimental product configuration be promoted atomically into
accepted `apple-gpu` membership and platform contracts.

### Phase 3: add capset 4, blobs, and deferred completion

1. Introduce the single-owner renderer session and its GL/Venus facades; add
   tests proving one global init/cleanup pair and correct capset coexistence.
2. Add feature negotiation, capset 4, blob commands, map-info responses, and
   resource tables with strict validation.
3. Connect Venus command streams without enabling guest-visible support.
4. Convert renderer fences into deferred replies/callbacks tied to stable
   connection generations. Reset and disconnect cancel or retire every
   outstanding completion before destroying resources.
5. Fuzz malformed commands, lengths, IDs, flags, and state transitions.

Exit: synthetic tests can create, map, submit to, fence, unmap, and destroy a
blob through the complete local transport without leaks, stalls, or reset
races. Feature advertisement remains off until Phase 5.

### Phase 4: implement the selected shared-memory mechanism

Run both memory spikes with small focused host/guest payloads on macOS first,
unchanged on the Simulator for iOS validation, and on the connected iPad before
making the selection final. Select one using upstream compatibility, buffer/image
coverage, coherent semantics, code size, memory overhead at 1.5 GiB, reset
safety, and cross-platform behavior. Reject any design that works only on
macOS.

If the selection needs Linux changes:

1. add a new ordered topic under `kernel/patches/series/`;
2. update `kernel/patches/series/manifest.tsv`, stable IDs, hashes, expected
   final tree, delta ceilings, ownership/overlap/extraction metadata, and
   `kernel/README.md` together;
3. add mutation-pinned tooling tests for negotiation, bounds, mapping, and
   reset ordering;
4. replay and build both 16 KiB and 4 KiB kernels with the required LLVM 22
   toolchain; and
5. prove one runtime-negotiated kernel image works on all Apple products.

Audit the likely upstream touch points explicitly:
`drivers/gpu/drm/virtio/virtgpu_ioctl.c`, `virtgpu_object.c`,
`virtgpu_drv.h`, `virtgpu_vq.c`, `virtgpu_kms.c`, the applicable Virtio-GPU
UAPI, and `arch/um/drivers/virtio_uml.c`. Keep only the files required by the
selected design in the final topic.

Exit: coherent buffer and image round trips work under both guest page
profiles on macOS and Simulator, the connected iPad passes
the required memory campaign, and allocation pressure, invalid maps, backend
reconnect, and teardown leave no stale host access.

### Phase 5: upstream the guest portability path

Implement and submit the missing Mesa Venus support in its owning upstream
project. At minimum, the change must cover:

- renderer and guest extension enumeration for
  `VK_KHR_portability_subset`;
- generated Venus codecs for
  `VkPhysicalDevicePortabilitySubsetFeaturesKHR` and
  `VkPhysicalDevicePortabilitySubsetPropertiesKHR`;
- physical-device feature/property `pNext` query handling;
- logical-device enablement and validation;
- a generated guest ICD manifest with `is_portability_driver: true` and a
  compatible manifest format, plus tests that the loader hides the device
  without portability enumeration and reveals it with the standard flag; and
- protocol/version compatibility with renderers that do not expose the
  extension.

Add Mesa unit/integration tests and obtain upstream review. A local patched
Mesa may validate the end-to-end design while review is pending, but all
reports and images must say “custom guest.” Do not permanently carry the
change as a rootfs-only patch.

Exit: a released upstream Mesa contains the complete support, and the signed
EROFS image is rebuilt and provisioned as a whole with that unmodified
release. The same whole-image update contains the correctly rebuilt,
ARM64-identified pinned CTS executable and matching source/resources/current
must-pass data. Only then may capset 4 be enabled in a production
configuration or CTS results be treated as qualification evidence.

### Phase 6: headless common-path guest bring-up, macOS first

1. Enable Venus and the selected memory model without scanout on macOS. Keep
   this as the continuous guest-development lane, using only the common
   Apple-GPU provider and iOS-compatible renderer configuration.
2. Verify the guest loader selects the Virtio Venus ICD, not lavapipe or an
   accidentally installed second ICD.
3. Run allocation/map/flush/invalidate, transfer, compute, rendering,
   synchronization, query, descriptor, pipeline-cache, and reset smoke tests.
4. Capture the `dEQP-VK.info.*` QPA and the repository-owned machine-readable
   feature/property/extension report. If authenticated upstream Vulkan Tools
   was deliberately added to the rebuilt image, capture `vulkaninfo --summary`
   too; do not make an absent ambient tool a prerequisite.
5. Run the CTS enumeration and instance/device gates, then the exact
   blob/memory, synchronization, compute, rendering, and reset families as
   their implementations land. Investigate every non-permitted result before
   increasing the claimed API version.
6. Complete deterministic shards of every applicable non-WSI leaf referenced
   by the current `vk-default.txt` manifest. Verify every non-mandatory case
   occurs exactly once across fractions and all 19 mandatory cases occur in
   each fraction, along with QPA/device identity; do not use the smaller
   2026-03-01 universe as the promotion gate.
7. Stress repeated device/context creation, memory pressure, connection loss,
   guest shutdown, and host renderer initialization failure.
8. Repeat the identical backend/configuration on the iPhone 17 Pro Max
   Simulator as required iOS validation, then run the required headless subset
   on the connected iPad.

Exit: released upstream guest user space reports the target portability
device naturally on macOS and the Simulator through the same path, and the
connected-iPad headless gate passes. The selected current non-WSI manifest
union is complete and has no unexplained required-case failure. Simulator or
macOS success alone is insufficient.

### Phase 7: add scanout and desktop integration

1. Implement CPU readback scanout first on macOS and run deterministic damage
   tests there during development through the common Metal presentation path.
2. Boot the ordinary macOS-hosted guest desktop and verify Vulkan clients
   coexist with the shared ANGLE/Virgl GL path. The macOS product must contain
   no CGL, IOSurface-GL, VNC renderer backend, or alternate renderer fallback.
   If VNC remains available, it consumes only the common CPU-readback/damage
   stream after rendering.
3. Add direct Metal-object scanout only after device/format/synchronization
   probes prove it safe.
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

Exit: headless and displayed Vulkan workloads render correctly, CPU readback
is the common explicit correctness fallback, native presentation has no stale
image/layout/fence race, macOS and Simulator use the same graphics
implementation, and the connected iPad passes the promotion gate.

### Phase 8: cross-platform parity and connected-iPad qualification

Use this order:

1. Continuous macOS development and acceptance under both guest page profiles,
   using the common Apple-GPU/ANGLE/Venus/MoltenVK/Metal graph only.
2. iPhone 17 Pro Max Simulator same-path validation under both profiles.
3. Complete physical-iOS qualification on the connected iPad under both page
   profiles.
4. Final XCFramework/app packaging and external consumer checks.

On iOS verify direct MoltenVK loading, framework signing, install-name/rpath
rules, foreground/background transitions, drawable loss, memory warnings, app
interruption, clean guest shutdown, and repeated launch. Verify the final app
contains exactly one MoltenVK framework and that both page-profile runtime
frameworks carry the same load command without embedding their own copies. Run the
complete agreed device qualification matrix on the connected iPad. CTS may be
sharded and each shard bounded for the 1.5 GiB guest, but every required shard
from the current manifest must run under both page profiles. Use one fresh
guest and one CTS process per shard, export the QPA immediately, and resume
only at a verified boundary; neither Simulator nor macOS results substitute
for device evidence.

Exit: every platform has an independent result table and evidence bundle. A
platform that exposes fewer MoltenVK features retains a lower truthful claim.

### Phase 9: qualify Vulkan 1.4 portability and upstream the host work

1. Audit every Vulkan 1.4 promoted command, feature, property, limit, and
   extension against the exact guest Mesa, protocol XML, renderer, and
   MoltenVK revision.
2. Run the complete current Vulkan CTS manifest with portability expectations
   on macOS first, then on the same-path Simulator and connected iPad. Verify
   every manifest leaf/shard and its QPA, and record only exact applicable
   Khronos waivers with upstream issue links; the claim cannot advance until
   every required iPad shard runs under both page profiles.
3. Run the Vulkan Profiles tool only for profiles whose requirements are
   expected to fit the portability device; name only profiles that pass.
4. Submit the virglrenderer Apple loader, portability, memory, and thread
   changes upstream. Maintain each carried change as a Git topic commit,
   regenerate its series with `dependencies/rebase-patches.sh`, and drop the
   topic when the pinned upstream commit contains it. Never hand-edit generated
   patch bytes.
5. Retain the shared ANGLE/Virgl GL backend until Venus has equivalent reset,
   desktop, packaging, and stability acceptance. The shared backend is the
   only GL fallback on macOS and iOS; no CGL, desktop-only Virgl, VNC renderer,
   or macOS-only Vulkan architecture remains. A VNC view, if retained, is only
   a consumer of common output.

Exit: the product documentation states the exact per-platform Vulkan API and
portability subset, not a broader profile or conformance claim.

## Repository work map

| Area | Primary files | Planned responsibility |
| --- | --- | --- |
| Virtio-GPU protocol | `darwin/device/gpu/protocol.hpp`, `virtio_gpu_device.hpp`, `virtio_gpu_device.cpp` | feature bit, capset 4, blobs, validation, ownership, map responses |
| Renderer session | `virgl_renderer.*`, new Venus/session sources, `darwin/device/gpu/CMakeLists.txt` | own one process-global init/cleanup pair; isolate GL and Venus facade state and native resources |
| vhost-user transport | `vhost_user_device.hpp`, `vhost_user_message.*`, `vhost_user_server.*`, `in_process_service.*` | shared-memory messages/aperture if selected, deferred fences, reconnect/reset generations |
| Display | `display.*`, `native_display.mm`, `native_display_ios.mm`, scanout framebuffer | one CPU-readback/Metal-resource/presentation contract for every Apple platform, with thin AppKit/UIKit surface adapters; remove CGL, IOSurface-GL, VNC renderer, and platform-only fallbacks, while any VNC view consumes only common output |
| Common Apple GPU provenance | `dependencies/manifest.tsv`, `dependencies/prepare.sh`, `dependencies/rebase-patches.sh`, `dependencies/patches/*/series.tsv`, `dependencies/platforms/*-gpu-arm64.tsv` | own repository/full-commit/transitive-source/product identity, generated ordered topics, and checked-in platform/common-recipe contracts; no product-local manifest or downstream fetch |
| Common Apple GPU build | planned `dependencies/build-apple-gpu.sh`, `dependencies/cmake/AppleGpuDependencies.cmake`, `dependencies/cmake/VerifyAppleGpuPrefix.cmake` | emit and verify separate macOS/Simulator/device prefixes from one renderer/ANGLE/Venus/MoltenVK recipe; make it the sole macOS graphics provider, inject the loader directly, install provenance, remain offline, and fail on any second provider; retire the iOS-only builder after atomic migration |
| macOS dependency cleanup | `dependencies/manifest.tsv`, `dependencies/build.sh`, `dependencies/verify-prefix.sh`, `dependencies/cmake/UmlThirdPartyTargets.cmake` | remove desktop libepoxy/virglrenderer/CGL graphics rows, recipes, and imports while retaining only required non-graphics macOS dependencies |
| Thin product packaging | `Kernarium/scripts/package-ios-frameworks.sh`, `Kernarium/Kernarium.xcodeproj/project.pbxproj`, macOS runtime-closure verification | assemble/sign one app-level MoltenVK framework and verify thin AppKit/UIKit embed/rpath/lifecycle shells without changing or supplementing the common renderer implementation |
| Kernel | `kernel/patches/series/`, manifest and metadata, Virtio-GPU and `arch/um/drivers/virtio_uml.c` patch content | guest-VRAM or shared-region negotiation and mapping in one common image |
| Rootfs | `tools/rootfs/manifest.json`, provisioning inputs and tests | consume released unmodified Mesa portability support and a correctly identified, matched CTS executable/source/resource/must-pass set as one signed whole image |
| CTS and guest tests | `tests/integration/gpu/run_vk_cts.py`, shared QPA/artifact support, `tests/unit/gpu/`, `tests/tooling/gpu/`, dependency/kernel/rootfs tooling tests, `tests/test-manifest.tsv` | manifest expansion, ICD/device identity, deterministic shards, QPA egress/parsing, protocol, memory, portability, package, lifecycle, guest, and conformance gates |
| Dependency tooling tests | `tests/tooling/dependencies/`, `tests/test-manifest.tsv` | retain source-preparer, patch-regenerator, build-manifest, prefix-verifier, and runtime-closure gates; add offline transitive-source, three-platform recipe/payload, concurrent-publication, direct-loader, and one-copy-package fixtures |
| Documentation | this plan, feasibility report, implementation plan, architecture/testing/dependency docs, evidence index | distinguish current absence, custom prototype, accepted portability support, and profile claims |

New first-party renderer sources remain in `darwin/device/gpu/` and follow the
enforced device-layer dependencies. Do not include implementation sources into
other C++ files. Generated Mesa/virglrenderer protocol output remains owned by
the relevant upstream generator.

## Validation plan

### Unit and protocol tests

Add focused coverage for:

- feature negotiation and capset ordering with Venus unavailable, available,
  and initialization-failed;
- exact capset version/size, short buffers, unknown IDs, and retry behavior;
- every blob command, memory/flag combination, overflow, alignment, duplicate
  ID, context ownership, rollback, and map/unmap transition;
- guest-VRAM or shared-aperture range validation under both page profiles;
- imported host-pointer alignment, compatible memory types, flush/invalidate,
  aliasing, allocation pressure, and failure cleanup;
- asynchronous fence ordering, out-of-order completion, disconnect/reset,
  renderer failure, connection generation, and shutdown races;
- portability enumeration flags, exact subset features/properties, logical
  device enablement, and rejection when the subset cannot be transported;
- filtering of every unsupported fd/dma-buf/foreign/DRM/fence/semaphore
  extension;
- CPU scanout and direct Metal scanout format, plane, stride, offset, extent,
  layout, device identity, fallback, and in-flight lifetime; and
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

The `apple-gpu*`, `build-apple-gpu.sh`, and `*-venus*` names are Phase 1/2
deliverables selected by this plan; they are intentionally not claims about
the current tree. After promotion, the same commands select `apple-gpu` rather
than the experimental product. A retained `build-ios-gpu.sh` entry point may
only forward to the common builder and must pass a tooling test proving it
selects the same recipe during migration; remove it before Phase 1 exits. Do
not run the old static macOS GPU aggregate or CGL
workflow as planned graphics validation. Also verify the Simulator network
prefix needed by Kernarium. Run dependency verification, architecture audit,
and, for kernel changes, the canonical LLVM 22 replay/build for both page
profiles. The macOS runtime campaign is the default per-change gate; the
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

Measure after correctness:

- command throughput and host renderer CPU;
- pipeline/shader creation latency and cache reuse;
- mapped-memory bandwidth and flush/invalidate cost;
- resident guest/host memory and bounded heap/aperture overhead;
- fence latency and command-thread occupancy;
- CPU readback versus direct Metal presentation copies and frame rate; and
- clean shutdown time under idle, active, and device-loss states.

Use macOS as the primary iterative correctness/benchmark environment, the
Simulator as required same-path iOS validation, and the connected iPad for
mandatory final memory, thermal, stability, and performance evidence. macOS
measurements guide iteration but are not iOS performance evidence; do not
compare the platforms as identical hardware. Sanitizer and diagnostic builds
are not performance evidence.

## Claim ladder

Use these exact claim boundaries in reports and product documentation:

1. **“MoltenVK and Venus build.”** Build/link/package evidence only.
2. **“The guest enumerates Venus Vulkan X.Y.”** Loader, capset, instance, and
   physical-device discovery only.
3. **“Basic guest Vulkan works.”** Memory, compute, rendering,
   synchronization, and readback smoke passed.
4. **“Guest Vulkan X.Y portability subset is supported.”** The subset is
   exposed, queried, enabled, and validated, and the agreed functional/CTS
   coverage passed on that named platform.
5. **“Named Vulkan Profile P is supported.”** The Vulkan Profiles tool and all
   profile requirements passed for that exact profile and platform.
6. **“Vulkan conformant.”** Use only with the applicable Khronos conformance
   status; neither a version string nor local CTS success grants this claim.

The word “maximum” in this plan means the highest truthful intersection of
released guest Mesa, negotiated Venus protocol, host renderer, MoltenVK, and
the tested Apple GPU. It does not mean the highest version constant compiled
into one layer.

## Risk register and stop conditions

| Risk | Mitigation or stop condition |
| --- | --- |
| guest Mesa never accepts portability support | keep production Venus disabled; a custom guest remains prototype evidence only |
| MoltenVK host-pointer import cannot cover required images | reject guest-VRAM design and implement the standardized shared-memory aperture or another upstream-capable memory model |
| memory works on 4 KiB but aliases on a 16 KiB Apple host | require alignment/range tests under both profiles before feature negotiation is enabled |
| a Venus Linux fd assumption remains hidden | run exact extension filters and failure injection; no fd-like extension is advertised without complete semantics |
| asynchronous completion outlives reset or guest RAM | generation-tag callbacks and drain/cancel them before renderer/memory destruction; sanitizer stress is mandatory |
| macOS convenience causes renderer/configuration drift from iOS | one common Apple renderer target/configuration, macOS-first execution followed by mandatory Simulator validation, configuration-diff/runtime-closure gates, and no macOS-only GL/Vulkan provider, resource, capset, or fallback backend |
| Simulator succeeds but the physical GPU/lifecycle differs | require the full connected-iPad memory, lifecycle, presentation, and CTS matrix under both page profiles; no substitution |
| direct Metal scanout creates a cross-device or stale texture | keep CPU readback as the correctness path; gate zero-copy on device identity and explicit lifetime/synchronization tests |
| Vulkan 1.4 string is mistaken for a profile | use the claim ladder and machine-readable feature/profile reports in every evidence bundle |
| connected iPad exceeds the physical-device memory policy | bound VRAM/aperture and CTS shards; retain the normal 1.5 GiB guest and no disk-swap workaround |
| LVP or another ICD creates a false guest success | force `VK_DRIVER_FILES` to the Virtio ICD and validate the QPA device/driver identity before counting a case |
| current CTS binary is mistaken for qualification-grade | retain it for development, but require a whole-image rebuild with ARM64 CPU and pinned release metadata plus matching resources/lists before qualification |
| a large CTS run truncates, OOMs, or loses its QPA | one bounded shard per fresh iPad guest, framed checksummed artifact egress, exact non-mandatory/mandatory-fraction coverage verification, and hard failure on OOM, device loss, missing trailer, or incomplete shard |
| platform contracts still select different renderer identities or feature flags | one `apple-gpu` product, one selected virglrenderer row/series and semantic recipe, byte-verified contracts, and a three-platform configuration-diff test before promotion |
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
   ordered topics, and feature configuration.
2. virglrenderer enumerates portability devices, transports and enables the
   portability subset, filters unsupported external handles, and shuts down
   cleanly in process.
3. Virtio-GPU capset 4, blob resources, map/unmap, deferred fences, and reset
   semantics pass validation and sanitizer stress.
4. The selected shared-memory model is bounded, coherent, safe under both
   guest page profiles, and usable for every required buffer/image class.
5. Any Linux changes are replayed through the maintained patch series and
   both canonical page-profile builds pass with LLVM 22.
6. A released upstream Mesa containing the required Venus portability work is
   installed through the authenticated whole-image workflow; no private guest
   graphics patch or version override is present.
7. macOS is the maintained first development/acceptance target, and an
   unmodified guest naturally enumerates and uses the target MoltenVK
   portability device there. The iPhone 17 Pro Max Simulator then passes the
   identical configuration as mandatory iOS validation.
8. The connected iPad completes the full agreed physical-device qualification
   for both 4 KiB and 16 KiB profiles. Simulator or macOS success cannot
   satisfy this criterion.
9. The qualification rootfs contains a correctly ARM64-identified pinned CTS
   build with matching source/resources/current must-pass data. Every required
   manifest shard completes; every non-mandatory case occurs exactly once and
   the 19 mandatory cases occur in each native fraction; and every required QPA
   and lifecycle artifact is intact on macOS, the same-path Simulator, and the
   connected iPad under both page profiles.
10. Headless functionality, common ANGLE GL fallback, scanout/readback behavior,
   lifecycle, memory pressure, current CTS, architecture audit, and the
   dedicated macOS-first, Simulator-validation, and connected-iPad workflows
   pass at the agreed scope. All dependency preparation, patch-regeneration,
   offline-build, three-prefix,
   provenance, package, and runtime-closure tests pass; there is no host Vulkan
   Loader/dlopen fallback and no macOS CGL, desktop renderer, or VNC renderer
   provider. Any VNC view consumes only common output; both page runtimes name
   the same MoltenVK install name, and the app embeds exactly one MoltenVK
   framework.
11. `dependencies/README.md`, `docs/testing.md`, product documentation, and
   evidence use only the centralized dependency paths, name the exact API
   version and subset per platform, contain no retired Kernarium dependency
   owner, and make no unsupported Vulkan Profile or conformance claim.

Until those gates pass, the accurate production statement is: **guest Vulkan
through Venus and MoltenVK is unsupported on iOS Simulator, connected iPad,
and macOS; Vulkan 1.4 plus portability subset on one iOS-targeted shared path,
developed on macOS first and qualified on Simulator plus iPad, is the
implementation target.**
