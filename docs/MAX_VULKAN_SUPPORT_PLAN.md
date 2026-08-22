# Maximum guest Vulkan support through Venus and MoltenVK on Apple hosts

Written 2026-08-20 and revised 2026-08-23 after the GL/GLES implementation and
qualification campaigns. This is an implementation plan, not a capability,
Vulkan Profile, or conformance claim. It builds on the measured feasibility
results in [GPU_BACKEND_FEASIBILITY.md](GPU_BACKEND_FEASIBILITY.md), the
completed common Apple ANGLE/Metal migration, and the qualification lessons in
[MAX_GL_GLES_SUPPORT_PLAN.md](MAX_GL_GLES_SUPPORT_PLAN.md). The original
authenticated `rootfs.erofs` remains immutable. The sealed GL/GLES derivative
is also immutable and deliberately excludes the Vulkan runtime and Vulkan CTS,
so it is not a Vulkan development or qualification image. The guest kernel is
not external: this project owns its Linux build, `virtio_gpu` driver, UML
transport, and maintained patch series.

Dependency-management assumptions were revised after the 2026-08-20
centralization under `dependencies/`; paths and workflows in this document
refer to that post-refactor ownership model.

## Revision checkpoint and lessons imported from GLES (2026-08-23)

The GL/GLES campaign retired several assumptions that appeared in the first
version of this plan. Treat the following as plan inputs, not optional process
improvements:

1. The common `apple-gpu` product, `dependencies/build-apple-gpu.sh`, fixed
   ANGLE/Metal provider, direct Metal presentation owner, and macOS removal of
   CGL/OpenGL.framework are complete. Vulkan extends that product behind a
   separate experimental membership; it does not repeat the GL migration.
2. Qualification needs source-built provenance. Co-located source, a release
   label, an ELF build ID, or success in a native arm64 Debian VM does not prove
   which source and configuration produced a guest executable. The original
   image's `deqp-vk` remains development-only. A Vulkan qualification image
   must contain a clean build from one pinned released CTS source plus its
   exact source/build/install receipt, matching resources, generated lists, and
   runner policy.
3. The running native arm64 Debian VM is the mandatory build environment for
   every Vulkan rootfs or CTS rebuild; do not rebuild either inside this UML
   emulator. It is also useful for differential diagnosis, but it is never a
   correctness or conformance oracle. Audit every transferred payload, then
   establish every product result inside this emulator.
4. Capability truth precedes broad CTS. GLES exposed a normalized 32-bit depth
   format backed by 24-bit storage; query-only smoke passed and the full matrix
   found the mismatch. Vulkan therefore freezes and operation-checks every
   advertised API version, extension, feature, property, queue family, format,
   usage bit, memory heap/type, external-memory property, limit, surface
   format, and present mode before the corresponding CTS family can count.
5. Treat the decoded guest capset and the host MoltenVK probe as independent
   evidence. Compare their semantics field by field; do not require raw byte
   equality across different Apple GPUs, and do not let a host capability
   silently leak into the guest advertisement.
6. Failures may belong to MoltenVK, virglrenderer/Venus and its generators,
   guest Mesa, the maintained kernel/UML transport, memory coherence, WSI,
   campaign code, or the test itself. Reduce an unmodified exact CTS case,
   instrument only disposable development builds, remove diagnostics from the
   accepted product, rerun the exact case, then rerun the complete frozen
   manifest affected by the repair. A native pass or failure only helps locate
   the owner.
7. Campaign inputs and failures are immutable. Freeze Git and dirty state,
   emulator and signed-app identities, kernel artifacts, rootfs and receipt,
   CTS executable/resources/manifests, capability inventory, runner oracle,
   timeout, device list, and shard geometry. A concurrent Git commit does not
   invalidate already frozen evidence; a retry, however, never overwrites or
   erases a failed/incomplete attempt.
8. Use the existing acknowledged multiport Virtio serial channel for QPA,
   capability, summary, and lifecycle artifacts:
   `/dev/virtio-ports/org.uml.host-serial.0` over
   `--virtio-serial-socket`. `/dev/hvc0` belongs to the image's getty and must
   not be repurposed or masked. Every bounded compressed frame needs an exact
   host acknowledgement and the final raw length and digest must verify.
9. Repeated display-backed macOS CTS boots run inside VNC so they do not open
   AppKit windows. Headless compute does not need a display; native WSI,
   zero-copy, frame pacing, and performance still require the real direct
   Metal presentation path and cannot be qualified through VNC/readback.
10. The fixed execution policy is macOS JIT with 10 vCPUs and 8 GiB, Simulator
    Gadgets with 4 GiB, and physical-device Gadgets starting at 2 GiB. A device
    run may decrease RAM only after a confirmed emulator OOM, in fresh 64 MiB
    steps. JIT's known IPC-heavy multithread performance weakness must not be
    misdiagnosed as a Vulkan correctness defect without a focused engine
    control.
11. The signed Simulator campaign should generalize the proven four-device
    orchestrator: up to four matching booted Simulators, one unique UDID and at
    most one active guest per worker, deterministic assignment, fail-stop
    batches, permanent attempt directories, and independent `--verify-only`
    reconstruction. A physical device remains one shard at a time.
12. Physical-iOS Vulkan work is held at the user's direction. Do not run a
    connected-device probe, CTS shard, performance test, or thermal wait until
    the user resumes it. When resumed, cool to nominal before each shard when
    thermal state can affect the result; ignore thermal state only for a test
    whose outcome cannot be affected. macOS and Simulator work may continue,
    but neither can satisfy eventual device completion criteria.

The GLES campaign also proved that package scope matters: its global hierarchy
export legitimately entered an unrelated desktop-GL package that required a GL
4.5 context. Vulkan qualification must similarly derive its case universe from
the exact released Vulkan must-pass/Profile scope being claimed, not from an
all-package export, a convenient historical list, or a hand-pruned manifest.
The counts currently recorded for the original image are inventory history;
the receipt-backed rebuilt CTS establishes the normative counts and digests.

The fixes themselves also changed the expected debugging posture. GLES found a
nonterminating shader in ANGLE's MSL boolean selection, over-broad fast math for
non-finite values, incorrect transform-feedback query accounting and layout
mapping, a GLES 3.0 multisample renderbuffer fallback gap, and a false Virgl
depth-format advertisement. None was visible from the API version alone, and
several crossed the ANGLE/Virgl boundary. The Vulkan analogue is to inspect
MoltenVK/SPIRV-Cross shader lowering and math mode, Venus generated marshalling,
query/synchronization accounting, format and memory mapping, kernel/transport
coherence, and WSI ownership before attributing a guest failure to one layer.
Keep strict or slow behavior scoped to the shaders/operations that need it;
never trade broad steady-state performance for a narrow correctness repair
without the frozen performance gate.

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
then runs through the same renderer configuration on the iPhone 17 Pro Max
Simulator as the required active iOS validation lane. Run independent full
macOS and Simulator manifests: a macOS result is a predecessor and diagnostic
reference, not a substitute for Simulator execution. Full product completion
eventually requires the agreed connected-iPad matrix, but that lane is paused
until the user explicitly resumes it.

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
   product. The common Apple ANGLE/Metal provider and presentation foundation
   now exist, but Venus is disabled, capset 4 and blob resources are absent,
   and the required guest/host memory-sharing mechanism is not implemented.
2. The Mesa Venus installed in the original rootfs does not transport or
   advertise `VK_KHR_portability_subset`. MoltenVK is a portability driver, so
   an honest production path is blocked. The sealed GL/GLES derivative cannot
   be reused because it intentionally excludes Vulkan runtime and CTS payloads.
   A future independently released Mesa could remove the software limitation,
   but consuming it requires a separately authorized, receipt-backed Vulkan
   derivative lifecycle. Lowering the core API version to 1.3 or earlier does
   not remove the portability-subset obligation.
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

The revised critical path is:

```text
freeze the completed common Apple-GPU/GLES baseline
    -> reproducible MoltenVK probes and a truthful host capability inventory
    -> extend apple-gpu with authenticated MoltenVK slices
    -> Apple-capable virglrenderer Venus
    -> capset 4, blob resources, and asynchronous fences
    -> implement/backport required virtio_gpu and UML transport support in the maintained kernel
    -> select and implement the shared-memory model
    -> freeze and operation-check the complete decoded Vulkan capability inventory
    -> audit released portability-capable Mesa and source-built CTS inputs read-only
    -> VULKAN DERIVATIVE AUTHORIZATION STOP: report the exact delta and ask the user
    -> without explicit authorization: stop; production guest Vulkan remains unsupported
    -- separately authorized plan revision only -->
    -> build one minimal derivative and its CTS in the running Debian VM
    -> transfer, audit, filesystem-check, and seal it read-only on the host
    -> boot only with that sealed lower plus disposable OverlayFS state
    -> headless macOS guest bring-up on the common graphics path
    -> direct-GPU macOS scanout plus focused correctness/performance canaries
    -> complete receipt-backed macOS CTS under VNC plus separate native-display gates
    -> four-way iPhone 17 Pro Max Simulator canaries and full CTS
    -> hold physical work until the user resumes it
    -> connected-iPad decisive sustained performance/thermal/lifecycle and full CTS gates
    -> select the highest candidate tier that passed every required platform gate
    -> claim only that tested high-performance portability tier, up to 1.4
```

Host, dependency, renderer, protocol, kernel, transport, and synthetic memory
work may execute through the authorization stop. The continuation after the
dashed marker documents downstream dependencies, but no Vulkan userspace or
CTS payload may be added to an image unless the user explicitly authorizes the
exact derivative contents and lifecycle. Existing GL/GLES derivative authority
does not carry over.

## Scope and invariants

The intended API path is:

```text
guest Vulkan application
    -> Vulkan loader installed in the authenticated sealed guest lower
    -> Mesa Venus ICD installed in that same lower
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
ANGLE/Metal. The macOS CGL path has already been removed; its historical
evidence is not a Vulkan fallback or a baseline that new work must recreate.

The following constraints are non-negotiable:

1. iOS is the primary product target, while macOS is the first development
   lane. The renderer, memory, synchronization, and packaging architecture must
   satisfy Simulator/device public APIs, signing, sandbox, lifecycle, and the
   4 GiB Simulator and 2 GiB initial physical-device memory policies. macOS
   executes that exact design and cannot
   add a desktop-only GL/Vulkan provider, resource, capset, or fallback path;
   AppKit or VNC may only consume the common presentation output.
2. Only guest userspace is externally constrained. Before derivative
   authorization, development uses the exact Mesa, libdrm, Vulkan loader,
   Venus ICD/manifest, CTS, WSI, and tools installed in the original
   authenticated `rootfs.erofs`; they remain development-only inputs. Do not
   preload or OverlayFS-inject replacements. After a separate authorization,
   qualification may use only the exact independently released, unmodified
   Vulkan userspace and source-built CTS named by a checked derivative receipt.
   Build the complete derivative as one auditable epoch, exclude unrelated
   payloads, shut it down, filesystem-check it, seal the lower read-only, and
   use disposable OverlayFS state thereafter. A missing capability is a hard
   pause-and-ask condition, not permission to mutate either sealed image.
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
   userspace, and expose a standard-compatible ABI consumed by the original
   development image and the audited released qualification candidate. A
   private userspace ioctl or library cannot be required.
   Any new graphics Kconfig support belongs built-in in
   `kernel/mainline-6.18/config/common.fragment` because loadable modules remain disabled;
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
    and the connected iPad is mandatory physical-device qualification after
    that lane is explicitly resumed. Until then, run no physical-device work.
12. Do not call a result a named Vulkan Profile or Vulkan conformant merely
    because `vulkaninfo` prints version 1.4 or a CTS subset passes.
13. Start connected-iPad workloads at the repository's normal 2 GiB guest
    policy. Reduce only after a confirmed emulator OOM, by exactly 64 MiB per
    fresh immutable attempt. Never add swap or a disk-backed memory workaround.
    WebGL Aquarium remains prohibited there and is unrelated to the Vulkan
    acceptance gate.
14. External-project access is fetch-only. Never push a branch, open or update
    a pull request, merge request, or issue, send a patch or email, or upload
    an artifact to an external project. First-party emulator and test-orchestration
    changes live directly in this repository; third-party
    virglrenderer, MoltenVK, or ANGLE source changes live only in
    `dependencies/patches`; Linux changes live only in `kernel/mainline-6.18/patches`.
    The exact installed qualification CTS and guest Mesa, libdrm, loader, and
    ICD remain unchanged; a missing guest-userspace capability is a stop
    condition, not permission to patch or contact that project.
15. Bounded command decoding, validation, resource bookkeeping,
    synchronization, and shader compilation are ordinary control-plane work.
    Data-plane feature emulation on the CPU is forbidden when work scales with
    rendered or computed data. GPU-backed lowering through public Metal render,
    compute, blit, texture, buffer, or indirect-command facilities is eligible
    only after the matched performance gates pass.
16. Profile version is subordinate to performance. Freeze the workload corpus,
    noise envelope, and thresholds before implementation. Compare later
    candidates with the immediately lower truthful tier. For the first usable
    Vulkan tier there is no honest guest lower-tier A/B: use frozen absolute
    budgets and a native MoltenVK diagnostic control without treating that
    control as ground truth. Never manufacture an A/A comparison or resurrect
    the retired CGL provider. Keep the lower tier when correctness,
    performance, memory, thermals, lifecycle, or frame pacing fails. CTS or
    Profiles-tool success cannot override that decision.
17. The current `rootfs.erofs`, its provisioning manifest, and the sealed
    GL/GLES ext4 derivative are read-only evidence, not Vulkan work products.
    Do not install a package, replace an ICD JSON, or use preload/OverlayFS to
    inject graphics libraries, CTS executables/resources, tools, WSI
    components, permissions, or service rules. Disposable overlay and `/run`
    are for first-party campaign scripts, transient configuration, and bounded
    output only. If a Vulkan guest byte must change, stop before preparing the
    new derivative, report its exact released inputs, inclusions, exclusions,
    source-build receipt, and consequence if declined, and ask the user for
    explicit authorization.
18. Qualification artifacts use the named multiport Virtio serial channel and
    exact acknowledgements. Do not redirect them through the console/journal,
    `/dev/hvc0`, an unacknowledged stream, or the root block device. Preserve
    every raw QPA and transport transcript and verify them again from the host.
19. Freeze support claims per API/profile scope. A test outside the claimed
    Vulkan must-pass or Profile universe is not a failure of that claim, while
    a missing case inside the frozen universe is always a failure. Never edit
    the released manifest to make this distinction.

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
| Common Apple renderer | all Apple products select the verified `apple-gpu` UTM virglrenderer/ANGLE/Metal recipe through `dependencies/build-apple-gpu.sh`; CGL/OpenGL.framework, the desktop provider, and the independent `ios-gpu` recipe are absent; Venus remains disabled | Phase 1 extends this accepted provider behind explicit Vulkan experiment membership; it must not redo or fork the completed GL migration |
| Accepted Vulkan host dependencies | the common provider exists, but no accepted Vulkan-Headers, MoltenVK, or SPIRV-Cross payload/import/package slice exists | the unmanaged MoltenVK probe cannot enter a production claim; add authenticated Vulkan members to the existing product without contaminating the accepted GL-only prefix |
| Maintained guest kernel | pinned Linux 6.18 already contains the standard Virtio-GPU blob, context-init, Venus capset, and DRM UAPI, but the UML transport lacks the complete shared-region/guest-VRAM implementation needed by Venus | kernel/UML work is an owned production deliverable, not an external ceiling; preserve the ABI consumed by the current rootfs and patch the implementation through the maintained common series |
| Original signed rootfs | Debian Mesa 25.0.7; Virtio ICD manifest reports API 1.3.305; installed `deqp-vk` lacks authoritative source/build provenance and reports invalid build identity | useful for no-device development and transcript capture only; it cannot qualify a Vulkan claim |
| Sealed GL/GLES derivative | authenticated read-only ext4 lower with receipt, but its checked exclusion list removes Vulkan runtime and Vulkan CTS payloads | it cannot be reused, expanded, or booted writable for Vulkan; a new minimal Vulkan derivative requires separate authorization |
| Candidate upstream guest Mesa | Mesa 25.1 and later can advertise Vulkan 1.4 | version is sufficient, but portability-subset transport is still missing |
| Prepared virglrenderer 1.3.0 Venus | `VKR_MAX_API_VERSION` caps the major/minor API at Vulkan 1.4; generated protocol inputs use Vulkan headers 1.4.334 | establishes a 1.4 design cap, not a 1.4.334 profile, conformance, or patch-level ceiling |
| Unmanaged local macOS MoltenVK 1.4.3 probe | library advertises 1.4.357; an unsandboxed smoke accepts an instance requested at 1.4.357 and reports a Vulkan 1.4 physical device | proves macOS host major/minor feasibility only; it is not an accepted dependency, guest result, or either iOS product |
| MoltenVK feature model | portability driver; macOS probe reports geometry shaders false | target is Vulkan plus portability subset, not full core conformance |
| Apple MoltenVK packaging | no accepted MoltenVK product exists; the local XCFramework has only macOS arm64 | macOS/Simulator/device slices, imports, contracts, verification, packaging, signing, and probes are Phase 1 work |
| Qualification infrastructure already available | acknowledged named Virtio serial, immutable receipt validation, VNC macOS boots, signed fixed-campaign launchers, four-Simulator orchestration pattern, and verify-only artifact reconstruction | reuse and generalize these mechanisms for Vulkan; do not create an unacknowledged console exporter or a mutable retry model |

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
| Released Mesa/source-built CTS inputs exist but a Vulkan derivative is not authorized | paused | paused | paused | report the exact proposed inclusions, exclusions, build receipt, and image delta; keep production Vulkan unsupported |
| Separately authorized sealed Vulkan derivative | conditionally up to 1.4 + portability subset | same conditional shared-path ceiling | held until user resumes device work | report only the tier passing the complete frozen platform-specific gates; no claim inherits from another platform |

A maintained kernel/UML patch does not make a result “custom guest userspace”
or lower the permitted claim. A patched Mesa, libdrm, loader, or Venus ICD is
prohibited rather than a fallback state; the maintained kernel is an
authenticated part of this emulator product.

Under the immutable-rootfs policy, today's production ceiling is no usable guest Venus Vulkan,
not Vulkan 1.3: lowering the core version does not remove MoltenVK's mandatory
portability-subset path. Vulkan 1.4 plus the subset remains a conditional
architectural target only if a future unmodified Mesa release independently
supplies the missing support and the user separately authorizes a minimal
receipt-backed derivative. There is no full software-emulation alternative. Without both
conditions, production Venus remains unsupported.

The current rootfs's 1.3.305 manifest is not a useful reason to promise Vulkan
1.3. Host, renderer, and kernel implementation may remain version-capable
through 1.4, but this plan must not update the signed rootfs or sealed GL/GLES
derivative to consume a later Mesa. If a suitable release appears, audit it
read-only, prepare the exact Vulkan-only derivative delta and build/receipt
procedure without building the image, pause, and ask the user. If authorization
is declined, record the terminal shipping result as unsupported.
Mesa 25.1's
release notes record the
[return of Venus Vulkan 1.4 support](https://docs.mesa3d.org/relnotes/25.1.0.html),
but API-version support alone does not solve portability.

At the Mesa main revision audited for this plan, Venus's
[complete passthrough-extension whitelist](https://gitlab.freedesktop.org/mesa/mesa/-/blob/302736bcf272ccf12572879dcc6846196a45f143/src/virtio/vulkan/vn_physical_device.c#L1240-1484)
still omits `VK_KHR_portability_subset`, and its generated Venus protocol has
no codecs for the subset feature/property structures. This identifies what a
future candidate must contain; it does not authorize consuming that candidate.

[`VK_KHR_portability_enumeration`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_portability_enumeration.html)
controls discovery of portability devices. MoltenVK must be enumerated with
the extension and `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`.
[`VK_KHR_portability_subset`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_portability_subset.html)
describes the missing or restricted device behavior and must be exposed,
queried, transported, and enabled when supported. These are distinct
requirements. MoltenVK's own documentation describes it as a
[Vulkan 1.4 subset implementation](https://github.com/KhronosGroup/MoltenVK#moltenvk-and-vulkan-compliance).
Any future candidate guest ICD also needs the loader's documented
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

Extend the implemented platform-parameterized `apple-gpu` product as the Phase
1 deliverable. Use `apple-gpu-vulkan-experiment` for opt-in bring-up until
promotion so the verified accepted GL-only prefixes remain reusable and
fail-closed. The existing common interface is:

```text
dependencies/build-apple-gpu.sh
dependencies/cmake/AppleGpuDependencies.cmake
dependencies/cmake/VerifyAppleGpuPrefix.cmake
dependencies/platforms/{ios-simulator,ios-device,macos}-gpu-arm64.tsv
```

The old independent `build-ios-gpu.sh` recipe is already retired. Do not
restore it or create a second renderer builder. The common builder emits macOS arm64, iOS Simulator
arm64, and iOS device arm64 from one semantic recipe, in that development
order. Only target triple, SDK, deployment target, signing shell, and final
platform packaging may vary. Continue using the one accepted UTM
virglrenderer repository/full commit and ordered patch series for GL and
Venus. The desktop virglrenderer/CGL rows and imports are already absent.
Every macOS graphics configure must continue resolving the same `apple-gpu`
provider family as iOS, and a configure/link graph must fail if it can resolve
a second virglrenderer, libepoxy, GL implementation, or renderer archive.

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
separate from the guest, which continues to use the Vulkan Loader and Mesa
Venus ICD manifest already installed in `rootfs.erofs`.

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
the advertised tier through ordinary Venus negotiation. If the current
rootfs's guest userspace cannot consume that cap truthfully, keep production
Venus disabled.
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
the behavior required by the exact Mesa 25.0.7 userspace installed in the
current rootfs. Do not wait for a
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
  exercised by the current rootfs userspace, plus only an explicitly versioned
  guest-VRAM extension that a read-only trace proves that userspace consumes.

Match the current rootfs Mesa DRM transcript explicitly:

| Guest operation | Required maintained-kernel/host behavior |
| --- | --- |
| `GETPARAM` | report 3D, capset-query-fix, resource-blob, and context-init only when complete; report host-visible or an exact current-image-proven guest-VRAM route truthfully |
| `GET_CAPS` | return Venus capset 4/version 0 with exact renderer bytes, including wire-format, extension, blob-ID-0, wait-sync, timeline, and guest-VRAM fields |
| `CONTEXT_INIT` | accept capset 4, 64 rings, and poll-rings mask 0; preserve per-file context/ring ownership |
| Ring/reply shared memory | create HOST3D + MAPPABLE + blob ID 0, then support DRM map and `mmap`; this remains required even when Vulkan device BOs use guest-VRAM |
| Device BO | use HOST3D when host-visible is supported, otherwise an exact current-image-proven guest-VRAM route; return accurate resource-info blob memory and rounded size |
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
the current rootfs Mesa Venus path always needs: ring/reply shared memory is
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

That future-source observation does not authorize selecting the route. Use
guest-VRAM in executable work only if a read-only audit of the exact Mesa
25.0.7 binary/source identity in the current rootfs proves the same transcript;
otherwise reject it for the immutable-image plan and use a route the installed
userspace already supports. It may remain only a synthetic future-candidate
test behind the Phase 5 authorization stop.

For a current-image guest-VRAM route proven by that audit, getparam 9 and blob memory value `0x4`
are an exact Mesa/virglrenderer guest-VRAM extension rather than Virtio 1.3
core. Pin the exact current Mesa call transcript, do not renumber existing
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
- the heap is charged against guest RAM and remains practical at the 4 GiB
  Simulator policy and the 2 GiB initial physical-device policy;
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

Choose guest-owned VRAM only if the exact Mesa 25.0.7 in the current rootfs can
use it without a custom guest library, the mandatory HOST3D ring/reply mapping also works, and
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

## Vulkan CTS provenance, development, and qualification strategy

The original authenticated guest image contains a VK-GL-CTS 1.4.5.5-labeled
installation and can drive no-device harness work and failure localization
through the host/kernel development ceiling. Its current build is useful only
for development: an audited build-configuration defect and missing
source/build receipt make it permanently ineligible for qualification. The
sealed GL/GLES derivative does not contain this Vulkan payload.

Before Phase 5 receives explicit Vulkan-derivative authorization, execute only the
installed binary's no-device identity/harness cases and failure-localization
runs. Positive portability enumeration, feature tests, full released-manifest
shards, WSI qualification, and the Profiles tool are dormant even if the host
and kernel prototype is ready.

### Audited inventory and trust boundary

| Historical original-image item | Guest path or measured identity |
| --- | --- |
| Executable | `/opt/vk-gl-cts/vulkan/deqp-vk`; AArch64/Linux PIE; 77,904,360 bytes; build ID `b4652c3d3923932193c272300201985e4d438863` |
| Resources and generated current leaf lists | `/opt/vk-gl-cts/vulkan/`; `amber`, `vulkan`, and `vk-default` must remain beside the executable |
| Co-located release-labeled source | `/home/uml/vk-gl-cts-1.4.5.5`; no `.git` metadata, so current identity is the 1.4.5.5 label plus rootfs provenance, not a guessed commit or proof that it produced the ELF |
| Current must-pass manifest | `/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/mustpass/main/vk-default.txt` |
| Historical case universe | 2,869,003 cases across 96 manifest leaf lists; inventory only, not a normative qualification count |
| Mandatory fraction | `/home/uml/vk-gl-cts-1.4.5.5/external/vulkancts/mustpass/main/vk-fraction-mandatory-tests.txt`; 19 identity/mandatory-feature cases |
| Bounded frozen lane | `/opt/vk-gl-cts/vulkan/vk-main-2026-03-01/`; 266,574 cases; regression convenience only, never a substitute for the current universe |
| Required guest ICD | `/usr/share/vulkan/icd.d/virtio_icd.json`; original image declares Virtio/Venus API 1.3.305 |

The image also installs lavapipe. Every guest campaign must set
`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json` and then verify the
QPA physical-device/driver identity. An LVP pass is a harness control at most;
it is never Venus/MoltenVK evidence. `deqp-vk` resolves Vulkan at runtime, so
omitting this contract can silently test the wrong ICD.

The historical top-level `vk-default.txt` contains ordered leaf-list filenames such as
`vk-default/api.txt`. This CTS build recognizes that group-file form and it is
valid for development-only focused work. Do not freeze a qualification
campaign from these counts or paths. The separately authorized build must
derive its exact top-level must-pass/Profile scope, native fraction contract,
mandatory list, resources, and expected case universe from its pinned released
source and receipt. Independently parse that manifest for path validation,
scheduling, and evidence, record every manifest/leaf digest, and verify the
release-defined exact-once/repeated coverage semantics. Do not assume that the
historical 96 leaves, 2,869,003 cases, or 19 mandatory cases remain unchanged.

There is a release-blocking build-configuration defect in the provisioned
executable: `dEQP-VK.info.build` reports `DE_CPU_X86_64` even though `uname -m`
and the ELF are AArch64, reports release name `unknown`, release ID
`0xcafebabe`, and target `Default`. `DE_CPU` can select compiled behavior, so
this is more than a cosmetic metadata discrepancy. Keep using the binary for
bring-up and regression localization, but label its results development-only.
Do not rebuild, stage, provision, or overlay-inject a corrected CTS into either
existing image. For authorization, name the exact unmodified released source,
toolchain/configuration, selected build targets, installed executable/resource
closure, manifest scope, unrelated exclusions, and receipt fields together
with the Mesa delta. If authorized, build the arm64/Linux CTS and rootfs
derivative in the running Debian VM, never in this emulator; transfer them
through an audited staging boundary and verify every installed byte and dynamic
dependency before sealing the new lower. The VM result itself is never
accepted. Until that authorization and receipt-backed build exist,
qualification is unavailable.

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

After the Phase 5 authorization gate only, use the receipt-backed suite's
native released-manifest fraction contract for broad campaigns. The campaign
generator resolves all paths and digests from the checked receipt; it accepts
no arbitrary executable, resource, manifest, or source path. The generated
command has this shape, where every uppercase value is a frozen receipt field:

```sh
cd "$CTS_INSTALL_DIR"
export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json
./deqp-vk \
  --deqp-caselist-file="$CTS_MUSTPASS" \
  --deqp-archive-dir="$CTS_INSTALL_DIR" \
  --deqp-fraction="$FRACTION_INDEX,$FRACTION_COUNT" \
  --deqp-fraction-mandatory-caselist-file="$CTS_MANDATORY_LIST" \
  --deqp-log-filename=/run/uml-vk-cts/shard.qpa \
  --deqp-log-images=disable \
  --deqp-log-shader-sources=disable \
  --deqp-test-oom=disable
```

The driver rejects an unset or mismatched receipt field before boot. It also
freezes the exact generated argv and released-runner status oracle in the
campaign manifest so a later source, image, or Git change cannot silently
change the case universe or result interpretation.

In the separately authorized qualification branch, development may defer video until presentation/video prerequisites are ready,
but final released-manifest qualification executes every in-scope leaf. Unsupported video
must be reported naturally from truthful feature discovery; advertised video
support requires its authenticated sample data. Missing required resources are
a harness failure, not permission to omit the leaf.

The runner's privileged bootstrap creates a bounded `/run/uml-vk-cts` output
directory owned by the non-root test account before dropping privileges; CTS
itself must not run as root.

Add a dedicated `tests/integration/gpu/run_vk_cts.py`, sharing safe artifact
and QPA parsing code with the GL CTS driver but not turning
`run_gpu_guest.py` into a CTS-specific monolith. Reuse the existing authenticated
read-only-lower/disposable-OverlayFS bootstrap, acknowledged artifact channel,
and process-group cleanup. The driver
must provide:

1. `smoke`, exact-family, frozen-regression, and current-must-pass modes;
2. typed manifest/leaf/shard selection, CTS-native deterministic `N/M`
   fractions plus the mandatory-fraction file, checkpoint/resume only at a
   verified shard boundary, page-profile and engine selection, an outer
   timeout, and a separately qualified dEQP watchdog;
3. forced Virtio ICD selection followed by validation of the QPA session's
   device/vendor identity and absence of LVP or another ambient ICD, plus
   parsing of `dEQP-VK.info.*`, decoded capset bytes, and renderer logs for
   driver/API version, extensions, features, properties, queue families,
   formats, limits, memory, and WSI state;
4. a separate capability-inventory mode that freezes every advertised field
   and performs focused create/bind/map/copy/render/present probes for the
   claimed format, memory, usage, synchronization, and WSI combinations. A
   queried capability without a successful representative operation cannot
   enter the CTS campaign input;
5. one CTS process per guest, strict QPA parsing, raw QPA preservation, first
   failure and seed retention, and no-pass-on-crash, truncation, device loss,
   timeout, missing/duplicate/reordered case, or unclean emulator/guest
   shutdown. Reproduce the exact released runner's successful-result policy in
   a checked oracle: `Pass`, and only those `NotSupported`, `QualityWarning`,
   or `CompatibilityWarning` results that the pinned release accepts, may be
   successful. `Waiver`, `Fail`, resource/internal errors, crashes, timeouts,
   device loss, or incomplete coverage always fail unless an exact applicable
   Khronos waiver is separately recorded where waivers are allowed;
6. exact host revision/dirty state, product/model/OS/SDK, backend and
   dependency manifest/series/platform-contract/build-receipt evidence,
   rootfs/CTS/build ID/list digests, guest Mesa/kernel/page
   profile, virglrenderer/MoltenVK identity, shard first/last/count, result
   totals/duration, guest peak RSS/minimum `MemAvailable`, host renderer/Metal
   memory, exact selected capability-inventory digest, console/dmesg, QPA,
   serial transcript, and lifecycle outcome;
7. immutable macOS and signed-app matrix specifications that freeze every
   input before the first boot, preserve a failed attempt forever, and permit
   resume only by skipping already verified shards; and
8. an independent verify-only mode plus unit tests for receipt/manifest
   expansion, path safety, released status classification, shard coverage,
   QPA identity/trailer parsing, acknowledged-frame length/digest validation,
   frozen-input drift, timeout, resume, device assignment, and artifact
   completeness.

The guest upper layer is disposable. Compress each QPA and every required
inventory/summary artifact in the guest and export it through
`/dev/virtio-ports/org.uml.host-serial.0` in bounded, checksummed frames carrying
the shard name, raw/compressed lengths, and digest. Wait for the exact host ACK
after the begin record, every chunk, and the end record. Preserve the raw host
transcript and reconstruct the canonical artifact independently. A missing
trailer, bad ACK, bad length/digest, reconnect gap, or lost artifact fails the
shard. Do not fall back to journal/console streaming, do not consume `/dev/hvc0`
or its getty, and never write to the root block device.

For the app products, add a narrowly validated CTS boot mode rather than a
general remote shell. The macOS launcher should select the campaign and shard
as the default development path. The iPhone 17 Pro Max Simulator launcher then
uses the same mode/configuration as the required iOS validation lane, and the
device launcher uses it unchanged on the connected iPad while returning the
framed QPA and lifecycle result. Keep long/full/device campaigns opt-in and run
only the gadget or JIT engine, never the interpreter.

### Development ladder mapped to implementation work

Pre-authorization work is limited to the installed 19-case identity/no-device
baseline, runner/QPA/sharding mechanics, LVP rejection, and host/synthetic
compatibility tests. The positive numbered ladder below is dormant until Phase
5 is explicitly authorized and a corrected image is available. Then run each
gate first on macOS for fast iteration, repeat the identical
backend/configuration on the iPhone 17 Pro Max Simulator for iOS validation,
and defer every connected-iPad step until the user resumes physical work:

1. **Enumeration.** Run `dEQP-VK.info.*`, every mandatory case named by the
   receipt-backed released manifest,
   `dEQP-VK.api.version_check.*`, and `dEQP-VK.api.info.*`. Prove that the
   loader hides the portability ICD without portability enumeration and
   reveals it only with the standard extension/instance flag.
2. **Truthful capability inventory.** Decode and freeze the capset, then
   compare API version, extensions, features, properties, queue families,
   formats/usages, limits, memory heaps/types, external-memory properties, and
   WSI modes with the independent MoltenVK host probe. Operation-check every
   advertised class, reject an overclaim, and rerun the inventory after every
   renderer/protocol/kernel/dependency change that can affect it.
3. **Instance and device.** Run basic `create_device`, `features2`, API smoke,
   and `get_device_proc_addr` groups. Verify that
   `VK_KHR_portability_subset` features/properties traverse Venus and that the
   logical device enables the required subset without injected strings or
   guest overrides.
4. **Blob/imported memory.** Run requirements, basic allocation, mapping,
   regular binding, coherent/noncoherent host-write/device-read and
   device-write/host-read cases, plus `external_memory_host` when it is
   truthfully advertised. If it is absent, test the suppression path rather
   than forcing the extension.
5. **Submission and synchronization.** Run synchronization smoke, basic
   fences, binary/timeline semaphores where exposed, queue/device idle, reset,
   repeated-context, connection-loss, and teardown stress.
6. **Focused functionality and performance canary.** Run
   `dEQP-VK.compute.pipeline.basic.copy_ssbo_single_invocation` plus one
   basic/simple render-pass draw/readback, then the matched macOS and Simulator
   canaries. For the first usable tier apply frozen absolute budgets and the
   native MoltenVK diagnostic control; for later tiers use the immediately
   lower truthful tier. Do not expand broad CTS coverage for a candidate that
   already fails performance. Run the device canary only after resume.
7. **Change-local expansion.** For a passing candidate, expand to the complete
   basic compute pipeline group and the relevant memory, image, render-pass,
   rasterization, texture, SSBO, UBO, shader, descriptor, query, and
   synchronization2 leaves. Run full leaves only when implicated; the complete
   manifest remains the final selected-tier gate.
8. **Presentation.** Run display-backed macOS correctness CTS inside VNC to
   avoid AppKit windows, then run the WSI/native-presentation acceptance
   separately as the image-selected
   non-root graphical-session user: `uml` for KDE or the UID-1000 Phosh user
   under greetd/Phrog. Inherit its real `XDG_RUNTIME_DIR`,
   `WAYLAND_DISPLAY`/`DISPLAY`, authentication, and DRI permissions; do not use
   an artificial root/headless display. Keep CPU-readback and direct-Metal
   presentation results distinct.
9. **Selected ceiling.** After the candidate passes performance, expand the
   receipt-backed released manifest and run every referenced leaf, perform the
   selected-tier feature/property audit (including 1.4 only if it won), and run
   the Vulkan Profiles tool only for a specifically claimed profile.

Do not pre-remove geometry, tessellation, external-handle, DRM, Android, video,
or other capability-sensitive cases merely to improve the result. Scope comes
only from the exact released must-pass/Profile authority and advertised
portability state. A case using an advertised capability may not be accepted
as `NotSupported`; an optional or portability-limited case may be
`NotSupported` only when the exact spec and pinned release policy permit it.
Missing geometry in MoltenVK is not a blanket waiver.

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

For the separately authorized qualification branch, macOS runs JIT with ten
vCPUs and 8 GiB; each Simulator runs Gadgets with 4 GiB. Generalize the signed
GLES orchestrator to accept up to four unique matching Simulator UDIDs and run
at most one active fresh guest per device. Deterministically assign shards,
stop later batches after a failure unless an explicit keep-going diagnostic is
selected, retain every started attempt, and require a separate verify-only
reconstruction before promotion.

Physical-device execution remains paused. After the user resumes it, start the
connected iPad at 2,048 MiB with no swap or disk-backed memory workaround and
one CTS process/shard at a time. Reduce only after a confirmed emulator OOM,
in a new campaign exactly 64 MiB lower per attempt. Cool to nominal before a
shard when thermal state can change the result. Boot a fresh guest at each
boundary, disable deliberate OOM tests and routine image/shader logging, bound
QPA size, export it immediately, and monitor guest/host memory, app
termination, battery, and thermal state. OOM, Jetsam/app termination, timeout,
device loss, incomplete QPA, or unclean shutdown fails the shard. Resume only
at a recorded boundary. Run every required shard under both 16 KiB and 4 KiB
profiles; macOS and Simulator timing may size shards, but cannot replace
eventual device evidence.

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

Use the exact receipt-backed released source tree read-only to find a failed
test's feature predicate and expected behavior and reduce it to an exact
unmodified-case rerun. Record a minimal reproducer and analysis only in this
repository. Instrument disposable renderer, protocol, kernel, or campaign
builds when needed, but remove that instrumentation before the accepted rebuild
and do not patch or instrument qualification CTS, change discovery/expected
results/lists, or submit/upload a fix externally. A genuine CTS defect blocks
promotion unless an exact pre-existing Khronos waiver applies. Record any
later unmodified CTS correction as a possible resolution, then pause and ask
before creating a new sealed derivative epoch. The original installed
executable remains development-only; producing a correctly identified
qualification set is a Vulkan-derivative authorization checkpoint.

## Implementation phases

### Phase 0: freeze and automate the baselines

1. Record revision/dirty state, SDK/Xcode, rootfs userspace versions, pinned
   Linux base and reviewed final-tree identities, ordered kernel-topic
   manifest, resolved common configuration, both page-profile configurations,
   current `apple-gpu` contracts/receipts and installed provenance, sealed
   rootfs identities, selected Simulator UDIDs, physical-device hold state,
   and exact commands.
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
   bounce/repacking and app termination are hard requirements. Freeze the
   eventual physical-device thermal budgets now, but collect no new device
   result while that lane is held.
3. Freeze the completed dependency baseline: validate the central manifest and
   complete patch inventory; offline-verify the prepared common `apple-gpu`
   repository and macOS/Simulator/device prefixes; record the one
   repository/commit/series identity, fixed `gl_provider=angle-metal`,
   `-Dvenus=false`, direct Metal presentation contract, and absence of accepted
   Vulkan-Headers/MoltenVK/SPIRV-Cross components. Record CGL and the old
   independent iOS recipe as absent, not as removal work or a fallback.
4. Preserve the passing current 16 KiB macOS and 4 GiB four-way Simulator
   GLES2/GLES3 matrices, decoded capsets 1/2, zero-fallback native
   presentation, and GPU workflows as the shared GL regression schema. Do not
   rerun an entire GLES matrix for an unrelated Vulkan-only edit; require the
   focused GL canaries and canonical GPU workflow, and rerun full matrices when
   a shared renderer/dependency/capability change can affect their frozen
   product.
5. Turn the direct MoltenVK probe into a maintained target. Report loader and
   device API versions, portability extensions, every external-memory/fence/
   semaphore extension, memory heaps/types, import alignment, features,
   properties, queue families, formats, and Metal GPU families.
6. Build a common probe target and make macOS run the default repeatable schema
   first. Run that same probe/configuration in a separately signed iPhone 17
   Pro Max Simulator app as required iOS validation. Freeze semantic reports,
   not raw equality between hardware-specific property blobs.
7. Record that connected-iPad work is paused. Keep the fixed probe and signed
   campaign definitions buildable, but do not launch them, attach to a live
   guest, wait on thermals, or silently substitute another physical target.
8. The current image does not contain `vulkaninfo`. Force the Virtio ICD and
   capture the expected no-device result with the installed `deqp-vk`
   information cases plus host-side parsing of the exact Mesa/ICD identity.
   Record LVP only as a separately labeled harness control. Do not add Vulkan
   Tools or inject a guest probe; if the installed information cases are
   insufficient for a required decision, pause and ask the user.
9. Freeze the pinned kernel's standard Virtio-GPU/DRM layouts and the exact
   current-rootfs Mesa getparam, capset, context-init, HOST3D ring/reply blob,
   device-BO, resource-info, map/mmap, execbuffer/fence, GEM-close, and reset
   transcript. Record current failures and add a sentinel rejecting any private
   userspace ioctl or library dependency.
10. Define likely kernel topic ownership before implementation: amend an
   existing logical owner where appropriate or add a focused topic for UML
   shared-region/guest-VRAM work and independently reviewable generic driver
   changes. Record outside-port ownership/justification, overlap/extraction,
   delta, and next-version policy obligations.
11. Preserve the original-image CTS executable, source label, resources,
   manifest, mandatory fraction, leaf lists, build ID, historical case counts,
   and digests as development inventory. Preserve the bad `info.build`
   CPU/release metadata and absent receipt as permanent qualification blockers;
   do not infer provenance from co-location. Define the exact receipt schema
   and audit for a future source-built released CTS without changing an image.
12. Bring up the dedicated CTS driver through manifest expansion, exact-case
   sharding, released-status oracle validation, named Virtio-serial QPA export,
   immutable attempt retention, verify-only reconstruction, capability
   inventory, and software-ICD rejection. It must fail closed on LVP, a
   malformed/truncated QPA, an incomplete shard, drift, or an unclean shutdown.
13. Run the development-grade 19-case identity fraction on macOS and then the
   iPhone 17 Pro Max Simulator. Do not run it on the connected iPad while that
   lane is held. These results locate infrastructure defects but cannot qualify
   Vulkan while the CTS build metadata is invalid and Venus remains absent.

Exit: macOS is the daily raw MoltenVK development baseline, the Simulator has
required same-path iOS evidence, physical work is explicitly held, current
guest absence is reproducible, and the common ANGLE GPU workflows remain
green. Historical CTS inventory and the future receipt contract are frozen,
and the campaign driver proves that it cannot mistake LVP, an over-advertised
capability, or an incomplete artifact for Venus success. The absolute
first-tier and later lower-tier comparison rules are frozen before candidate
results exist.

### Phase 1: build authenticated MoltenVK for all Apple products

1. Add MoltenVK, Vulkan-Headers, SPIRV-Cross, and every transitive source as
   exact HTTPS-repository/full-commit rows (plus declared sparse paths and an
   ordered series where needed) in `dependencies/manifest.tsv`. Use a distinct
   experimental Apple-GPU product until promotion; do not add graphics payloads
   to the generic `macos` aggregate recipe/verifier.
2. Extend the already selected UTM virglrenderer repository/commit/series. Do
   not introduce a second renderer identity, restore desktop CGL imports, or
   reopen the completed provider migration.
3. Extend the existing `build-apple-gpu.sh`, common CMake imports/verifier, and
   three platform contracts. Produce verified macOS, Simulator, and device
   outputs, in that order, from one semantic recipe and separate prefixes.
   Keep the retired `build-ios-gpu.sh` absent.
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
   after direct macOS and Simulator probes, all installed provenance/package
   gates, and—after the user resumes physical work—the connected-iPad probe
   pass. Until then, keep Vulkan membership experimental without weakening the
   accepted GL-only product.

Exit: a direct host probe creates the maximum truthful MoltenVK instance and
device from only authenticated artifacts, first on macOS and then on the
Simulator, with their semantic capability deltas recorded. The three prefixes
pass provenance and binary-shape verification, their contracts select one
common semantic recipe, the build succeeds with network unavailable, both page
runtimes resolve the same framework install name, and the packaged app contains
one MoltenVK copy. Physical runtime promotion remains pending while held.

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
7. Use macOS host unit/sanitizer and current-image no-device/GL-regression guest
   tests as the primary iteration gate, then run the identical behavioral probe
   on the Simulator. Keep the focused connected-iPad probe compiled and signed,
   but do not launch it until physical work resumes. Positive guest portability
   tests remain dormant.
8. Keep `-Dvenus=false` in the accepted production products until the backend
   initializes without false capability advertisement.
9. Represent the opt-in Venus state in the experimental product's common recipe
   and all three platform contracts/verifiers. Promote one configuration
   atomically; never let macOS and iOS select different renderer rows or
   render-server/helper-process modes while sharing a product name.

Exit: host-side Venus creates a MoltenVK renderer device on macOS first and the
identical configuration passes Simulator validation. Unit probes demonstrate
correct extension filtering, subset handling, path classification, GL
isolation, thread shutdown, and asynchronous fences. The exact experimental
configuration remains unpromoted until the deferred connected-iPad device gate
also passes after physical work resumes.

### Phase 3: add capset 4, blobs, and deferred completion

1. Introduce the single-owner renderer session and its GL/Venus facades; add
   tests proving one global init/cleanup pair and correct capset coexistence.
2. Add exact VIRGL/resource-blob/context-init negotiation, capset 4/version 0,
   blob/map-info commands, ring-index propagation, and resource tables with
   strict validation.
3. Add a maintained-kernel/host compatibility harness for the current-rootfs Mesa
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

Run both memory spikes with small focused host/kernel compatibility payloads on
macOS first and unchanged on the Simulator for active iOS validation. Keep the
connected-iPad probe ready, but defer its execution and final cross-device
selection confirmation until physical work resumes. An experimental guest
run may use only executables already installed in the immutable image and may
not claim a portability device. If no installed executable can exercise the
needed behavior, use the host/synthetic compatibility harness or pause and ask;
do not inject a payload. Select one using current-rootfs compatibility,
direct buffer/image coverage, coherent semantics, code size, memory overhead
at 4 GiB Simulator and 2 GiB initial device memory, reset safety, and
cross-platform behavior. Reject any design that
works only on macOS or requires steady-state full-resource CPU bounce,
repacking, or serialization.

The kernel side is an owned Phase 4 deliverable, not a conditional external
dependency. Complete it as follows for whichever device-memory route wins:

1. Preserve the current-rootfs Mesa transcript and all existing Virtio-GPU DRM ioctl
   numbers, structures, getparams, errors, and mmap behavior. Implement the
   mandatory HOST3D/mappable/blob-ID-0 ring/reply path plus either truthful
   host-visible device blobs or an exact current-image-proven guest-VRAM route.
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
   `kernel/mainline-6.18/scripts/rebase-patches.sh`; never edit generated patch or manifest identity
   bytes by hand.
5. Add mutation-pinned tooling tests for feature/capset/context/ring
   negotiation, unknown parameter and memory-type rejection, mapping bounds,
   rounded sizes, partial mmap, info-ring fences, reset, and unmap ordering.
   Ordinary DRM/UAPI changes do not advance the separate `darwin_uml_*` bridge
   ABI; follow the full `kernel/common/abi` version/fingerprint/sync workflow only if a
   canonical bridge declaration actually changes.
6. Replay and build both 16 KiB and 4 KiB kernels with LLVM 22. Test 4 KiB
   subpages on a 16 KiB host page, alignment, alias isolation, noncoherent
   flush/invalidate, mmap exhaustion, disconnect, and memory-pressure behavior.
7. Prove the exact 16 KiB artifact and exact 4 KiB artifact are each shared by
   macOS, Simulator, and device and select capabilities at runtime. Inclusion
   in a later external Linux release is not a product gate.

Exit: the host/kernel compatibility harness and synthetic Venus client prove
the required HOST3D ring/reply mapping and coherent buffer/image round trips on
both page profiles, macOS first and Simulator second. Any current-image guest
result remains development-only, production feature advertisement remains off,
and the phase makes no portability-device claim. Allocation pressure, invalid
maps, backend reconnect, and teardown leave no stale host access or
steady-state bounce path. Final device confirmation remains an explicit held
exit item rather than permission to run the iPad.

### Phase 5: audit the external gate, then pause for Vulkan-derivative authorization

This is an external release and user-authorization gate, not project
implementation work. Do not
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

The candidate derivative also needs a correctly arm64-identified CTS built
from one pinned unmodified release with matching source, resources, generated
must-pass/Profile data, and an exact build/install receipt. Audit those inputs
read-only and prepare only a written proposal containing:

- the base EROFS identity and a new derivative filename/size/filesystem UUID;
- exact released Mesa/libdrm/loader/ICD and CTS source identities;
- toolchain, configuration, selected targets, runtime closure, receipt schema,
  and the normative manifest expansion/status-policy oracle;
- every included and excluded path, with unrelated benchmark, language,
  GL/GLES, Vulkan SC, video/sample, and build payload excluded unless the
  selected Vulkan scope demonstrably requires it;
- the mandatory Debian-VM build commands plus host-side transfer and link
  audit; no rootfs or CTS rebuild command may run in the emulator;
- final filesystem check, mode-0444 lower, whole-image digest, read-only block
  attachment, disposable OverlayFS upper, and refusal of every later writable
  boot; and
- expected guest API consequence, qualification coverage, and the shipping
  ceiling if the change is declined.

Do not edit `tools/rootfs/manifest.json`, expand or mutate the sealed GL/GLES
derivative, prepare an image, build/sign it, run provisioning, install packages,
or inject candidates through OverlayFS before authorization. The new
derivative is a whole-image lifecycle. The running Debian VM owns every rootfs
and CTS rebuild command; transferred files must be audited rather than trusted
because of where they were built.

Then stop and ask the user. Host, renderer, protocol, and maintained-kernel
work may continue behind experimental feature advertisement and synthetic
compatibility tests, but honest guest enumeration, end-to-end CTS, production
capset 4, and product qualification remain blocked. If authorization is
declined or absent, record the terminal production result as unsupported.

Exit: **blocked awaiting explicit user direction.** Phase 6 may begin only
after the user separately authorizes the precise Vulkan derivative and its
authenticated build/seal workflow completes. Existing GL/GLES derivative
authority is insufficient.

### Phase 6: headless common-path guest bring-up, macOS first

This and Phases 7-9 are dormant downstream work. They cannot start with the
original immutable rootfs, the sealed GL/GLES derivative, or merely because a
suitable release exists.

1. Enable Venus and the selected memory model without scanout on macOS. Keep
   this as the continuous guest-development lane, using only the common
   Apple-GPU provider and iOS-compatible renderer configuration.
2. Verify the guest loader selects the Virtio Venus ICD, not lavapipe or an
   accidentally installed second ICD.
3. Run allocation/map/flush/invalidate, transfer, compute, rendering,
   synchronization, query, descriptor, pipeline-cache, and reset smoke tests.
4. Capture the `dEQP-VK.info.*` QPA, decoded capset, and host-side
   machine-readable capability inventory. Compare it semantically with the
   independent MoltenVK host probe, then operation-check all advertised
   formats/usages, memory types, queues, synchronization paths, and limits
   before freezing the inventory digest used by CTS.
5. Evaluate available core tiers in ascending order. Classify every required
   behavior and omit costly optional features. Do not expose the next tier if
   any mandatory operation is prohibited CPU emulation or cannot be capped
   truthfully through the authorized image's exact guest userspace.
6. After the focused correctness sentinel, run the macOS performance canary and
   the same Simulator canary before broad CTS. For the first Venus candidate,
   where no lower guest tier exists, use the native MoltenVK diagnostic control
   plus frozen absolute budgets; for later candidates use the immediately
   lower truthful tier. Do not manufacture an A/A control. Defer the short
   connected-iPad performance/thermal canary until physical work resumes.
7. Only for a candidate that passes performance, run CTS enumeration and
   instance/device gates plus the exact blob/memory, synchronization, compute,
   rendering, reset, and change-local feature families. Investigate every
   non-permitted result before increasing the claimed API tier.
8. Stress repeated device/context creation, memory pressure, connection loss,
   guest shutdown, and host renderer initialization failure.

Exit: released, unmodified receipt-backed guest user space reports a truthful
headless candidate tier naturally on macOS and the Simulator, and both
capability inventories and focused operations agree. This is not final tier
selection: direct presentation and the held decisive device gate remain in
Phases 7-8. A lower candidate or continued unsupported state is valid;
Simulator or macOS success alone cannot complete the eventual device claim.

### Phase 7: add scanout and desktop integration

1. Reuse bounded CPU readback for deterministic damage tests and repeated
   display-backed CTS inside macOS VNC. It cannot qualify native display or
   performance.
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
5. Keep connected-iPad presentation and lifecycle qualification held until the
   user resumes it.
6. Run the receipt-backed WSI leaf as the selected image's real non-root graphical
   session user (`uml` for KDE or the UID-1000 Phosh user) on macOS first, the
   same-path Simulator second, and the connected iPad last. Keep headless,
   CPU-readback, and direct-Metal results separate.
7. Measure copy count and `UML_GPU_FRAME_STATS`; do not claim zero copy based
   solely on API selection.

Exit: headless and displayed Vulkan workloads render correctly; ordinary
native presentation is GPU-backed with zero sustained CPU fallback and no
stale image/layout/fence race; and macOS and Simulator use the same graphics
implementation. Readback remains diagnostic/VNC-only. Device correctness,
frame-pacing, thermal, and performance remain held exit items.

### Phase 8: cross-platform prequalification and connected-iPad performance gate

Use this order:

1. Continuous macOS development and acceptance under both guest page profiles,
   using the common session with isolated ANGLE/Metal GL and Venus/MoltenVK
   Vulkan facades.
2. iPhone 17 Pro Max Simulator same-path validation under both profiles.
3. Candidate XCFramework/app packaging and external consumer checks.
4. After explicit resume, decisive physical-iOS presentation, lifecycle,
   sustained performance, and thermal qualification on the connected iPad
   under both page profiles before selecting a final product tier.

On Simulator verify direct MoltenVK loading, framework signing, install-name/rpath
rules, foreground/background transitions, drawable loss, memory warnings, app
interruption, clean guest shutdown, and repeated launch. Verify the final app
contains exactly one MoltenVK framework and that both page-profile runtime
frameworks carry the same load command without embedding their own copies. Do
not run the focused parity/presentation or sustained lifecycle/performance/
thermal matrix on the connected iPad while held. After resume, use one fresh
guest for each bounded device segment and export artifacts immediately;
neither Simulator nor macOS results substitute for device evidence. The
complete released-manifest device shards are Phase 9 work after those decisive
gates pass.

Exit while physical work is held: macOS and Simulator have independent result
tables and evidence bundles, and the device table explicitly records `held`
rather than `pass` or `NotSupported`. Final Phase 8 exit requires the resumed
device bundle. A platform that exposes fewer MoltenVK features or fails a
higher-tier performance canary retains a lower truthful candidate claim.

### Phase 9: select and qualify the highest high-performance portability tier

1. Audit each available core tier through 1.4 in ascending order against the
   exact guest Mesa, protocol XML, renderer, MoltenVK revision, execution-path
   classification, and Phase 0 performance budget. Stop at the first tier that
   cannot meet all mandatory requirements efficiently and truthfully.
2. For the highest candidate that passed focused correctness, change-local
   CTS, direct presentation/lifecycle checks, and the decisive performance gate,
   run the complete receipt-backed Vulkan CTS manifest with portability
   expectations on macOS first, then on four same-path Simulators. After the
   user resumes physical work and its decisive gates pass, run it on the
   connected iPad. Verify
   every manifest leaf/shard and its QPA, and record only exact applicable
   Khronos waivers with pre-existing published references; do not open an
   external issue. The claim cannot advance until every required iPad shard
   runs under both page profiles.
   A macOS or Simulator completion may advance implementation confidence but
   cannot complete a product claim while the device lane is held. If the full
   CTS exposes a core failure, first distinguish a harness or test
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
| Display | `display.*`, `native_display.mm`, `native_display_ios.mm`, `metal_presentation.*`, scanout framebuffer | extend the completed common Metal presentation owner for Vulkan image/layout/synchronization, keep native display GPU-backed, and retain CPU readback only for diagnostics/VNC |
| Common Apple GPU provenance | `dependencies/manifest.tsv`, `dependencies/prepare.sh`, `dependencies/rebase-patches.sh`, `dependencies/patches/*/series.tsv`, `dependencies/platforms/*-gpu-arm64.tsv` | own repository/full-commit/transitive-source/product identity, generated ordered topics, and checked-in platform/common-recipe contracts; no product-local manifest or downstream fetch |
| Common Apple GPU build | existing `dependencies/build-apple-gpu.sh`, `dependencies/cmake/AppleGpuDependencies.cmake`, `dependencies/cmake/VerifyAppleGpuPrefix.cmake` | extend the verified three-platform recipe with experimental Vulkan membership and distinct `gl_provider=angle-metal` / `vk_provider=venus-moltenvk` fields; inject MoltenVK only into Venus, remain offline, and fail on cross-facade edges or a second provider |
| Completed provider cleanup | `dependencies/manifest.tsv`, common imports/verifiers, product graphs | keep desktop libepoxy/virglrenderer/CGL/OpenGL.framework rows, the independent iOS GPU builder, and runtime fallbacks absent; do not recreate migration fixtures |
| Thin product packaging | `Kernarium/scripts/package-ios-frameworks.sh`, `Kernarium/Kernarium.xcodeproj/project.pbxproj`, macOS runtime-closure verification | assemble/sign one app-level MoltenVK framework and verify thin AppKit/UIKit embed/rpath/lifecycle shells without changing or supplementing the common renderer implementation |
| Guest kernel graphics | maintained Linux `drivers/gpu/drm/virtio/virtgpu_*`, applicable Virtio-GPU DRM UAPI implementation, `arch/um/drivers/virtio_uml.c`, and UML physical/IOMEM/MM code as selected | preserve the current-rootfs userspace ioctl transcript; implement ring/reply HOST3D mapping, guest-VRAM only if current-image-proven or otherwise host-visible device memory, context/ring/fence semantics, shared-region negotiation, resource-info, mmap, and reset in one runtime-negotiated artifact per page profile |
| Kernel series and verification | `kernel/mainline-6.18/patches/series/`, `kernel/mainline-6.18/patches/metadata.env`, ownership/outside-port/overlap/extraction/next-version policy, `kernel/README.md`, and kernel tooling tests | own generated topic identities, reviewed final tree/delta, bridge-ABI changes only when necessary, offline replay, and LLVM 22 builds of both page profiles; inclusion in a later external Linux release is not a product gate |
| Vulkan derivative authorization boundary | read-only `rootfs.erofs`, sealed GL/GLES derivative/receipt, future Vulkan derivative receipt, and repository evidence | preserve both existing images; describe a minimal released-userspace/source-built-CTS derivative and stop before build; after separate authorization, verify its exclusions/receipt/digest, seal it read-only, and boot only below disposable OverlayFS |
| CTS and guest tests | `tests/integration/gpu/run_vk_cts.py`, shared QPA/acknowledged-serial support, signed fixed campaign generators/adapters, `tests/unit/gpu/`, `tests/tooling/gpu/`, `tests/test-manifest.tsv` | receipt/manifest expansion, status oracle, truthful capability inventory, ICD identity, immutable deterministic shards, four-Simulator scheduling, verify-only reconstruction, protocol, memory, portability, WSI, lifecycle, and claim gates |
| Dependency tooling tests | `tests/tooling/dependencies/`, `tests/test-manifest.tsv` | retain source-preparer, patch-regenerator, build-manifest, prefix-verifier, and runtime-closure gates; add offline transitive-source, three-platform recipe/payload, concurrent-publication, direct-loader, and one-copy-package fixtures |
| Documentation | this plan, feasibility report, implementation plan, architecture/testing/dependency docs, evidence index | distinguish current absence, external-release-blocked state, locally carried host/kernel work, accepted portability support, and profile claims |

New first-party host renderer sources remain in `darwin/device/gpu/` and follow
the enforced device-layer dependencies. Linux implementation work remains in
the maintained kernel topic stack. Do not include implementation sources into
other C++ files. Generated Mesa/virglrenderer protocol output remains owned by
the relevant authenticated source generator.

## Validation plan

Before the Phase 5 authorization gate, this validation plan executes only
host/unit/synthetic tests, package/prototype checks, current-image no-device
identity runs, GL regressions, and artifact-runner mechanics. Positive guest
portability enumeration, Vulkan feature/WSI tests, full CTS/Profile work, and
production qualification are dormant. Commands below that build Simulator or
device products are prototype parity gates until authorization, not evidence of
guest Vulkan support.

### Unit and protocol tests

Add focused coverage for:

- feature negotiation and capset ordering with Venus unavailable, available,
  and initialization-failed;
- exact capset version/size, short buffers, unknown IDs, and retry behavior;
- current-rootfs Mesa transcript compatibility for required getparams, capset
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

# Deferred physical package/prototype lane: do not execute while device work is held.
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
Ordinary Virtio-GPU DRM/UAPI implementation does not advance the separate
`darwin_uml_*` bridge ABI. If a canonical bridge declaration changes, update
its version, fingerprint, identity/history, synchronized copies, kernel topic,
and undefined-import checks according to `kernel/common/abi/README.md` before running
the same gates.

`build-apple-gpu.sh` and accepted `apple-gpu` are current infrastructure;
`apple-gpu-vulkan-experiment` and `*-venus*` presets are Phase 1/2
deliverables, not current-tree claims. After promotion, the same commands
select `apple-gpu` rather than the experimental membership. Keep
`build-ios-gpu.sh` absent. Do not run an old static macOS GPU aggregate or CGL
workflow as graphics validation. Also verify the Simulator network
prefix needed by Kernarium. Run dependency verification and architecture
audit. Because the Venus memory path includes maintained-kernel work, its
canonical LLVM 22 replay/build of both page profiles is a mandatory promotion
gate, not a conditional extra. The macOS runtime campaign is the default per-change gate; the
Simulator run is required same-path iOS validation. Connected-iPad execution is
deferred until the user resumes it; packaging may remain a static build gate
but is not device evidence. Only the separately authorized downstream branch
may treat a resumed device run as a qualification gate. The
generic `ios-*` presets and root `scripts/build-xcframework.sh` do not exercise
or package this GPU path.

### Guest functional gates

These positive gates are dormant until Phase 5 receives explicit authorization
and a corrected image exists. Before then, run only the installed information
cases expecting no usable Venus device, host/synthetic transcript tests, LVP
rejection, permissions inspection, and GL regressions.

On macOS and the iPhone 17 Pro Max Simulator for every active claimed
platform/page-profile combination, and on the connected iPad only after
physical work resumes:

- verify `/dev/dri/card0` and `/dev/dri/renderD128` ownership and permissions;
- capture the kernel Virtio-GPU feature negotiation and capsets;
- run the receipt-backed `dEQP-VK.info.*` cases and generate the complete
  machine-readable capability inventory, including formats/usages, memory,
  queue families, limits, external properties, and WSI. Operation-check the
  advertisement rather than trusting query output alone;
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
- run change-local CTS families as features land and the complete released
  manifest, with its exact native-fraction coverage contract, released status
  oracle, and intact artifacts independently verified before promoting the
  claim.

If the authorized image's device permissions, groups, ICD JSON, desktop-session
environment, or service rules are insufficient, do not inject an overlay rule
or guest configuration workaround. Stop the affected gate, record the exact
required image change, and ask the user.

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
- ordinary Simulator foreground, background, interruption, and memory-pressure
  behavior; repeat the same lifecycle gates on the connected iPad only after
  physical work resumes.

### Performance and resource gates

Performance is a profile-promotion gate, not a report written after full CTS.
For every candidate tier, run renderer identity and focused correctness first,
then the applicable macOS comparison and identical Simulator canary. A passing
candidate may proceed through macOS/Simulator change-local CTS and direct
presentation/lifecycle while physical work is held, but cannot receive a final
product promotion. After resume, run the short and then decisive sustained
connected-iPad performance/thermal gates before device full CTS. The complete
released manifest runs only after the applicable platform performance gates
pass.

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
eventual mandatory final memory, thermal, stability, and performance evidence.
Do not collect that evidence while the lane is held. macOS
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

The word “maximum” in this plan means the highest truthful intersection of the
authorized receipt-backed guest Mesa, negotiated Venus protocol, host renderer,
MoltenVK, and tested Apple GPU after the owned kernel layer implements the
compatible Virtio-GPU/DRM contract. The current kernel implementation is not a
frozen external ceiling. The performance gate is part of that intersection.
“Maximum” does not mean the highest version constant compiled into one layer.

## Risk register and stop conditions

| Risk | Mitigation or stop condition |
| --- | --- |
| no released Mesa provides the required portability support, or a suitable release exists but a Vulkan derivative is not authorized | keep production Venus disabled; audit candidates read-only, report the exact blocker/inclusions/exclusions/receipt, pause and ask, and do not create a custom guest or external submission workaround |
| GL/GLES reaches Venus, a Vulkan loader, ANGLE/Vulkan, Zink, or MoltenVK | fail the product graph and runtime route; capsets 1/2 must terminate at ANGLE/Metal while only capset 4 reaches MoltenVK |
| a required Vulkan behavior executes data-parallel work on the CPU | reduce the advertised API tier or remain unsupported; do not use CPU shader/raster/geometry work, resource-sized conversion, or readback-driven command decisions |
| GPU-backed lowering exceeds the frozen performance budget | select the immediately lower tier or omit the optional feature even when CTS passes |
| MoltenVK host-pointer import cannot cover required images | reject guest-VRAM design and implement the standardized shared-memory aperture or another model the audited released guest consumes; otherwise stop at the Phase 5 authorization gate |
| a guest-VRAM-only design omits mandatory Venus ring/reply shared memory | require the separate HOST3D, mappable, blob-ID-0 map/mmap path before Mesa initialization can pass |
| memory works on 4 KiB but aliases on a 16 KiB Apple host | require alignment/range tests under both profiles before feature negotiation is enabled |
| a private or renumbered DRM UAPI makes the rootfs depend on custom userspace | stop promotion; pin the current rootfs Mesa transcript, preserve existing ioctl layouts/semantics, reject unknown values, and keep project-specific details below the userspace boundary |
| a guest-local sync-file fence is mistaken for a MoltenVK external-fence fd | test the delayed Virtio response and exact context/ring/fence identity locally; keep unsupported host Vulkan fd extensions filtered |
| capset/blob/context-init is partially advertised | fail closed unless the complete driver, ring/reply memory, device-memory, fence, reset, and renderer path is usable |
| a format, usage, memory type, queue, limit, feature, extension, or WSI mode is queried successfully but not implemented truthfully | reject the capability inventory and keep the affected advertisement disabled until a focused operation succeeds; rerun every implicated full manifest after repair |
| a kernel patch is hand-edited, stale, or loses outside-port ownership policy | author the managed Git topic, regenerate with `kernel/mainline-6.18/scripts/rebase-patches.sh`, update final-tree/delta/ownership metadata, and require clean LLVM 22 replay/builds of both profiles |
| a dependency, CTS, or kernel change is accidentally sent outside this repository | keep prepared sources remote-less, provide no push/submission step, review only checked-in carried series and local defect records, and fail documentation/tooling checks on outbound workflow instructions |
| a Venus Linux fd assumption remains hidden | run exact extension filters and failure injection; no fd-like extension is advertised without complete semantics |
| asynchronous completion outlives reset or guest RAM | generation-tag callbacks and drain/cancel them before renderer/memory destruction; sanitizer stress is mandatory |
| macOS convenience causes renderer/configuration drift from iOS | one common Apple renderer target/configuration, macOS-first execution followed by mandatory Simulator validation, configuration-diff/runtime-closure gates, and no macOS-only GL/Vulkan provider, resource, capset, or fallback backend |
| Simulator succeeds but the physical GPU/lifecycle differs | record device state as held, not pass; after resume require the full connected-iPad memory, lifecycle, presentation, and CTS matrix under both page profiles; no substitution |
| direct Metal scanout creates a cross-device or stale texture | retain readback only for diagnosis while fixing or narrowing the direct path; no displayed high-performance claim until device identity and lifetime/synchronization tests pass with zero sustained CPU fallback |
| Vulkan 1.4 string is mistaken for a profile | use the claim ladder and machine-readable feature/profile reports in every evidence bundle |
| performance passes only on macOS or Simulator | continue active implementation but do not make the final product promotion; the resumed connected-iPad performance/thermal gate is mandatory and non-substitutable |
| enabling unused Venus regresses ANGLE/Metal GL | lazily initialize Vulkan state where possible and compare GL startup, idle RSS/CPU, caches, and throughput with capset 4 absent, enabled-unused, and active |
| concurrent Vulkan starves or serializes GL | require bounded memory, scheduler/fence fairness, and matched concurrent workload tests before enabling both capsets together |
| connected iPad exceeds the physical-device memory policy | after resume start at 2 GiB, bound VRAM/aperture and CTS shards, add no swap, and reduce only after confirmed emulator OOM in fresh 64 MiB steps |
| LVP or another ICD creates a false guest success | force `VK_DRIVER_FILES` to the Virtio ICD and validate the QPA device/driver identity before counting a case |
| original CTS binary or co-located source is mistaken for qualification provenance | retain it for development only; require a pinned released source build, exact build/install receipt, resource/manifests closure, and checked runner oracle in a separately authorized derivative |
| a package install, preload, OverlayFS copy, ICD replacement, permission rule, manifest edit, or writable boot mutates or supplements a sealed lower | fail the immutability gate, discard that evidence, preserve the failed attempt, and ask before a new whole-image derivative epoch |
| a large CTS run truncates, OOMs, or loses its QPA | use bounded fresh-guest shards, acknowledged named Virtio-serial egress, released fraction coverage verification, and hard failure on OOM, device loss, missing ACK/trailer, bad digest, or incomplete shard |
| console or `/dev/hvc0` is reused for private artifact transfer | refuse the campaign; preserve the image getty and use only `/dev/virtio-ports/org.uml.host-serial.0` with exact acknowledgements |
| a retry hides a failure or a concurrent commit changes the working tree | freeze all inputs per campaign, preserve each attempt permanently, allow resume only by skipping verified shards, and accept concurrent repository changes only when they do not alter the frozen identities |
| a rootfs or CTS rebuild runs in the emulator, or a Debian VM result is treated as ground truth | run every such rebuild in the running native arm64 Debian VM, audit transfers, and require the full case inside this emulator on every claimed platform; VM execution is diagnostic only |
| an all-package or hand-pruned case export replaces the claimed Vulkan scope | derive the universe only from the receipt-backed released must-pass/Profile authority; fail on every missing in-scope case and do not count unrelated packages |
| released runner warnings or `NotSupported` are classified ad hoc | freeze and test the exact released status oracle; accept only release-permitted outcomes and always fail waivers without exact authority, failures, internal/resource errors, crashes, timeouts, device loss, or incomplete coverage |
| four Simulator workers reuse a device or race one guest | require four unique frozen UDIDs, deterministic assignment, and at most one active shard per Simulator; fail on device-list or runtime drift |
| platform contracts still select different renderer identities or feature flags | one `apple-gpu` product, one selected virglrenderer row/series and semantic recipe, byte-verified contracts, and a three-platform configuration-diff test before promotion |
| execution-path classification is unknown | keep the feature/tier disabled until tracing proves Metal-native, GPU-lowered, or bounded CPU control-plane behavior |
| MoltenVK/ANGLE CMake, CPM, submodules, or another tool downloads an undeclared transitive source | manifest every transitive repository/commit/path, inject prepared sources explicitly, force disconnected mode, and run builders with network unavailable |
| the retired desktop macOS graphics provider becomes reachable again | fail configure and runtime-closure tests; the completed common `apple-gpu` migration must remain the only provider and CGL/OpenGL.framework stay absent |
| `vkGetInstanceProcAddr` resolves from an ambient Vulkan Loader | disable host Vulkan dynamic loading, inject the exact MoltenVK pointer before initialization, audit the runtime closure, and fail if a loader/ICD/dlopen path appears |
| static MoltenVK is duplicated in both page-profile runtimes | use one dynamic app-level framework and verify identical runtime load commands plus exactly one embedded copy |
| two virglrenderer/libepoxy providers or sessions enter one process | expose only the common `apple-gpu` provider family to every Apple graphics target, fail on a second provider, and retain one process-global renderer session owner |
| experimental Vulkan dependencies contaminate an accepted prefix | use distinct product membership, prefixes, platform contracts, targets, and package checks until Simulator gates and, after resume, connected-iPad promotion gates pass |
| generated dependency patch bytes are edited manually | author/rebase topic commits and regenerate series with `dependencies/rebase-patches.sh`; validate order, stable IDs, SHA-256, subjects, and topology |

## Completion criteria

### Current pre-derivative completion

The executable scope of this plan reaches its authorized terminal point when:

1. Phases 0-4 have completed at the chosen host/kernel prototype scope without
   changing or supplementing `rootfs.erofs` or the sealed GL/GLES derivative.
2. The exact current Mesa/ICD and CTS limitations remain recorded: production
   guest Vulkan is unsupported and the installed CTS is development-only.
3. A suitable independently released Mesa and CTS-source candidate, if one
   exists, has been audited read-only and the exact minimal derivative delta,
   exclusions, source-build receipt, and consequence of declining it have been
   documented without editing provisioning inputs or preparing an image.
4. Phase 5 has stopped and asked the user before any Vulkan derivative build,
   transfer, sign/seal, provision, package-install, preload, or
   overlay-substitution action.
5. No production capset 4, Vulkan API/Profile, or conformance claim is made.

This is a valid no-update completion outcome, not Vulkan support.

### Conditional production completion after separate derivative authorization

The following downstream criteria are dormant. They become executable only if
the user explicitly authorizes the precise Vulkan derivative lifecycle. Device
criteria additionally remain dormant until the user resumes physical work:

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
   page profiles with LLVM 22. The authorized image's exact Mesa getparam,
   capset, context, ring-reply, device-BO, map/mmap, execbuffer, fence, and
   close transcript passes with no private userspace dependency, and the same
   per-profile kernel artifact runs on macOS, Simulator, and device.
6. Only after separate explicit authorization, an independently released,
   unmodified Mesa containing the required Venus portability work and a clean
   source-built released CTS are installed through one authenticated minimal
   derivative workflow. Its checked receipt records the base, inclusions,
   exclusions, build/install closure, and final identity; the image is
   filesystem-checked, sealed mode 0444, attached block-read-only thereafter,
   and used only below disposable OverlayFS. No private guest graphics patch or
   version override is present.
7. macOS is the maintained first development/acceptance target, and an
   unmodified guest naturally enumerates and uses the selected
   performance-qualified MoltenVK portability tier there. The iPhone 17 Pro
   Max Simulator then passes the identical configuration as mandatory iOS
   validation, with full campaigns distributable over four unique devices and
   at most one active guest per Simulator.
8. The connected iPad completes the full agreed correctness, performance,
   memory, thermal, presentation, and lifecycle qualification for both 4 KiB
   and 16 KiB profiles. Simulator or macOS success cannot satisfy this
   criterion.
9. The qualification derivative contains a correctly arm64-identified pinned
   CTS build with matching source, resources, must-pass/Profile data, released
   status oracle, and source/build/install receipt. Every required manifest
   shard completes with the exact released fraction/mandatory semantics, and
   every QPA, capability inventory, serial transcript, and lifecycle artifact
   is intact and independently reconstructed on macOS, same-path Simulator,
   and—after resume—the connected iPad under both page profiles.
10. Headless functionality, isolated ANGLE/Metal GL, GPU-backed native scanout,
   lifecycle, memory pressure, receipt-backed CTS, architecture audit, and the
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
    checked-in dependency/kernel patch series; the authorized image's guest
    Mesa/libdrm/loader/ICD and qualification CTS remain independently released
    and unmodified.
13. Every promoted operation is classified as Metal-native, GPU-lowered, or
    bounded CPU control-plane work. No promoted feature uses CPU-heavy
    data-path emulation, and checked-in matched evidence records both accepted
    and rejected API tiers against the frozen performance budgets. The exposed
    API and any named Profile are no higher than the last tier that passed.
14. Every advertised API/extension/feature/property/queue/format/usage/memory/
    limit/WSI claim appears in the frozen decoded capability inventory and has
    a focused operational proof. Host-probe and guest-capset reports are
    semantically reconciled per platform, with no false format or capability
    advertisement and no native VM result counted as emulator evidence.

Under the current pre-derivative policy, the accurate production statement
is: **guest Vulkan through Venus and MoltenVK is unsupported on iOS Simulator,
connected iPad, and macOS. Up to Vulkan 1.4 plus portability subset remains an
architectural target only; reaching it additionally requires explicit user
authorization for a sealed derivative containing independently released,
unmodified portability-capable Mesa and a receipt-backed source build of the
released CTS. Without that authorization, the plan stops at Phase 5 and the
shipping Vulkan ceiling is none. Even after macOS and Simulator pass, the
physical-iOS claim remains held—not passed—until the user resumes and the full
device gates complete.**
