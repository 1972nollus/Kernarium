# Maximum guest Vulkan support through Venus and KosmicKrisp

This document defines the maintained Vulkan experiment for the macOS
User-Mode Linux product. It records durable architecture, capability, and
acceptance requirements. Measured campaign results belong in
[VULKAN_SUPPORT_EVIDENCE.md](VULKAN_SUPPORT_EVIDENCE.md).

## Scope

The supported experimental route is:

```text
Linux application
    -> Vulkan Loader
    -> Mesa Venus ICD
    -> Virtio-GPU capset 4 and HOST3D blobs
    -> in-process virglrenderer Venus
    -> directly linked Mesa KosmicKrisp
    -> Metal
```

The route is macOS-only and requires macOS 26. The iOS GPU product has no
build: KosmicKrisp requires Metal 4, so it targets a physical device only and
is pending its Mesa/KosmicKrisp port.

Vulkan exists only in the separately named `apple-gpu-vulkan-experiment`
product and only when `UML_GPU_EXPERIMENTAL_VULKAN=1` is set. The experiment
must never change capsets 1/2, GL context selection, or native Metal
presentation.

MoltenVK is not an input, fallback, packaged framework, runtime provider, or
test target in this design. Historical feasibility documents may discuss it,
but no active build or product contract may select it.

## Dependency and build contract

`dependencies/manifest.tsv` is the host source authority. The Vulkan product
adds pinned Mesa, Vulkan-Headers, and Venus-protocol sources to the ordinary
upstream libepoxy/virglrenderer matrix. Mesa supplies KosmicKrisp and the Zink
Gallium OpenGL driver; the standalone
SDK supplies Vulkan headers and the registry used to generate paired guest and
renderer codecs in build directories. The SDK and protocol inputs match the
guest lock under `tools/rootfs/vulkan`. Host topics are regenerated into their
owning series under `dependencies/patches/`; the shared protocol series has one
owner under `tools/rootfs/vulkan/patches/venus-protocol/`. Generated patches are
never edited directly or used to carry duplicate SDK or codec copies.

`dependencies/build-apple-gpu.sh` builds offline and publishes:

- static libepoxy and virglrenderer;
- the dynamic `MesaEGL` and `MesaGallium` frameworks; and
- one dynamic `KosmicKrisp.framework`.

The format-4 macOS contract records
`gl_provider=zink-kosmickrisp` and `vk_provider=venus-kosmickrisp`. Verification
requires an arm64 macOS 26 payload, exact source and patch provenance, one
`@rpath/KosmicKrisp.framework/KosmicKrisp` dependency, a directly exported
`vkGetInstanceProcAddr`, and no Vulkan loader, `dlopen`, `dlsym`, second
Vulkan provider, or undeclared framework.

The host receives the provider entry point at link time. Runtime discovery
through an ambient Vulkan loader or ICD is forbidden.

## Capability policy

Only behavior that executes reliably through the complete guest-to-Metal path
may be advertised. A native host probe is diagnostic evidence, not a
guest-capability override. Venus must expose the provider's real core version,
extensions, features, properties, formats, queues, limits, and memory model
without extension-string injection or version forcing.

Optional functionality may be disabled when Metal or KosmicKrisp cannot
execute it reliably. In particular, the maintained KosmicKrisp topic does not
advertise shader float16, float16 FMA, or float16 signed-zero/Inf/NaN and RTE
controls. Native half arithmetic can time out Apple GPUs under concurrent,
NaN-heavy graphics workloads. Reporting the feature as unsupported is the
conformant behavior; silently running an unreliable implementation is not.
Integer-8 support remains independent.

No promoted capability may depend on CPU shader interpretation, CPU
rasterization, resource-sized steady-state copies, readback-driven command
decisions, or another CPU-heavy data path. Control-plane validation and
bounded diagnostic/VNC readback remain allowed and do not qualify a feature
as a native presentation path.

KosmicKrisp exposes `VK_KHR_pipeline_library` and
`VK_EXT_graphics_pipeline_library`. Graphics libraries retain their owned
shader and partial-state inputs until a complete pipeline link, and complete
links are cached without changing the ordinary monolithic pipeline path.
`graphicsPipelineLibraryFastLinking` is true: a library compiles its stages
when it is created, so a link without link-time optimization builds only the
render pipeline state, and a fast link that Metal refuses falls back to linking
the whole chain as a monolithic pipeline does.

The guest Zink path targets the highest baseline OpenGL profile that satisfies
that policy. On the maintained macOS provider this is OpenGL 3.1. Custom border
colors and their component swizzles, non-solid polygon modes, rectangular
lines, and transform feedback execute on the GPU. Transform-feedback draws
lower the application vertex stage to compute only for pipelines that capture
vertices; ordinary rasterization retains the direct Metal vertex path, and
counter queries and indirect byte-count draws remain GPU-driven.

Software line rasterization keeps vertex outputs and stipple phase separate for
each active multiview index and application instance. Sparse view masks use a
dense GPU dispatch dimension while shaders retain the original `ViewIndex` and
`InstanceIndex` values. Direct and indirect draws use the same checked layout;
the final Metal vertex stage selects the matching per-view record. Ordinary
native line rendering is unchanged. Without multiview, fragment `ViewIndex`
remains zero even when the vertex shader selects a nonzero array layer.

Dynamic polygon and line-rasterization pipelines own at most five executable
classes, compiled at pipeline creation or GPL final link. Provoking vertex,
depth clipping, and stipple remain dynamic within each class. Fill and eligible
native lines retain the direct Metal path; draws select a class without compilation.
Cache identity includes final-raster choices even when GS or TES output differs
from input assembly. Multisampled Bresenham sample placement follows that final
primitive, including polygon lines, while explicit sample locations take
priority. Generated polygon points and lines preserve source-polygon facing,
attributes, and depth bias. Bounded point guard bands retain large-point
coverage, and smooth lines include partial endpoint coverage.

The provider implements general geometry shaders, `VK_EXT_depth_clip_enable`,
and RGB32 uniform texel buffers on the GPU. RGB32 buffer views carry a raw GPU
address and element count in the ordinary descriptor; shader lowering rejects
out-of-range coordinates before 64-bit address arithmetic and reads each
12-byte texel with one three-component 32-bit load. Storage texel-buffer support
for those formats remains unadvertised, and no CPU repacking is used.

These capabilities remove the former OpenGL 3.2 and RGB32 OpenGL 4.0 baseline
blockers. The durable advertised maximum remains OpenGL 3.1 until the complete
Zink profile reconciliation and OpenGL qualification are finished; feature
bits alone do not raise that maximum.

## Renderer and memory contract

One renderer session owns virglrenderer's process-global lifetime. The
Zink/KosmicKrisp GL facade and the Venus/KosmicKrisp Vulkan facade share only
that owner and bounded device resources; neither resolves or calls the other's API.
The render server runs in that same address space, so it shares those
process-global tables rather than owning a second copy of them. It takes and
releases references to them, and the end of its last context never destroys
state the renderer session still holds.

The renderer implements:

- capset 4, context creation, and reset;
- HOST3D blob creation, mapping, unmapping, and range validation;
- asynchronous, ring-indexed fence retirement;
- explicit acknowledgement ordering; and
- a bounded 1 GiB vhost-user shared-memory aperture.

KosmicKrisp exposes one unified native memory class. The maintained host
translation presents separate guest-visible choices:

- optimal images use device-local Metal heap-backed allocations; and
- buffers and linear images that require host access use importable host
  memory.

Bindings that cannot produce the required native Metal resource fail rather
than leaving a partially usable Vulkan object. Resource ownership, mappings,
fences, and acknowledgements are revoked on context reset or disconnect.

Per-submit residency pinning is forbidden. Resources follow Metal heap and
command-buffer lifetime, avoiding an unmatched residency request on every
submission.

## Guest userspace contract

The Vulkan derivative is a distinct, receipt-bound ext4 image. It is attached
read-only and used below a disposable overlay. It does not alter the accepted
desktop `rootfs.erofs`.

`tools/rootfs/vulkan/sources.tsv` pins Mesa 26.2.1, matching Vulkan-Headers
and Vulkan-Loader inputs, the paired downstream Venus protocol, and Vulkan
CTS 1.4.6.2 with its authenticated correctness patches. The guest
[userspace contract](../tools/rootfs/vulkan/README.md) owns the transport fixes
and build-only codec generation; the host and guest consume one canonical
protocol patch series and SDK identity.

The checked receipt owns source trees, stable patch identities, installed
files, loader and CTS build identities, filesystem UUID, image size, and image
digest. A mismatched input, writable base, missing file, unexpected ICD, or
software renderer fails closed.

## CTS acceptance

The qualification lane uses the receipt-selected Vulkan CTS mandatory list and
the runner's released fraction semantics. It runs eight fractions with at most
four guests concurrently. Every guest receives exactly 4 vCPUs and 4 GiB RAM.

The canonical per-fraction command is:

```sh
python3 tests/integration/gpu/run_vk_cts.py \
  --qualification \
  --experimental-venus \
  --mode current-must-pass \
  --page-profile 16k \
  --memory 4G \
  --cpus 4 \
  --fraction INDEX,8 \
  --shard-timeout 21600 \
  --guest-timeout 22200 \
  --console-stall-timeout 600 \
  --emulator .build/presets/macos-integration-venus/bin/uml-darwin \
  --rootfs .build/rootfs/rootfs-vulkan-venus.ext4
```

The canonical pool coordinator and combined Zink/OpenGL command are documented
in [testing.md](testing.md). Its OpenGL coverage includes eight fractions of
each of the released core and GTF leaves, with the same four-worker bound.

All eight Vulkan fractions must use one verified dependency prefix, emulator bundle,
rootfs digest, CTS build identity, and implementation source identity.
Documentation-only edits do not invalidate an otherwise byte-identical
candidate. A fraction passes only when:

- its runner and guest exit successfully;
- every selected case has exactly one result;
- every result is `Pass`, `NotSupported`, `QualityWarning`, or
  `CompatibilityWarning`;
- no mandatory case is missing or duplicated;
- no software ICD is selected;
- the QPA and renderer/receipt evidence transfer completely; and
- the guest shuts down and is reaped.

An earlier result from another framework build cannot be mixed into the final
eight-fraction campaign.

The 4 KiB page profile must pass the focused integration and transport
workflows. A second full CTS campaign is required only when a Vulkan change is
page-size-sensitive.

## Lifecycle and resource safety

The CTS runner must never leave an emulator consuming CPU or memory after a
failure, timeout, interruption, or completed artifact transfer. The runner
sends TERM, waits for bounded cleanup, then sends KILL if needed. It masks
repeated INT/TERM delivery during that cleanup so a second terminal interrupt
cannot skip the KILL fallback.

Before filling a CTS slot, process inspection must confirm that every existing
`uml-darwin` guest has a live `run_gpu_guest.py` parent and a live
`run_vk_cts.py` owner. Detached CTS guests are stopped and reaped before any
new shard starts. Unrelated emulator processes outside this worktree are not
targets.

## Required validation

The final branch requires:

1. source-manifest and generated-series validation;
2. Vulkan guest input and receipt verification;
3. Apple GPU prefix and runtime-closure verification;
4. capability and Venus backend probes;
5. host unit/tooling workflows at 16 KiB and 4 KiB;
6. the Venus GPU workflow, including sanitizer coverage;
7. focused guest integration at both page profiles;
8. the complete eight-fraction CTS campaign above; and
9. an architecture audit proving the GL/Vulkan provider separation and the
   absence of active MoltenVK and iOS-Vulkan paths.

## Completion and claim boundary

Implementation is complete when the reproducible dependency topics, host
renderer, guest image, probes, lifecycle tests, and required workflows are
green and every fraction of the exact final CTS campaign passes.

The resulting statement is limited to the tested project stack: guest Vulkan
through Venus and KosmicKrisp is experimental on macOS. Local CTS success is
not a Khronos conformance submission, certification, or trademark claim.
iOS device and iOS Simulator Vulkan support are out of scope.
