# Guest GPU backend feasibility: native Vulkan versus completing GLES 3.2

Written 2026-08-19 at `636f9028`. Answers one question: is it easier to build a
native Vulkan backend for the guest GPU, or to keep fixing the existing Virgl
path until it supports every feature GLES 3.2 advertises?

The short answer is that the question contains a false premise, and once the
premise is removed the two options are not close.

The measured ceilings and transport findings remain current inputs to
[MAX_GL_GLES_SUPPORT_PLAN.md](MAX_GL_GLES_SUPPORT_PLAN.md) and
[MAX_VULKAN_SUPPORT_PLAN.md](MAX_VULKAN_SUPPORT_PLAN.md). Those later,
performance-first plans supersede this report's recommendation to evaluate
ANGLE/Vulkan as the shared GL/GLES provider; retain that recommendation here
only as dated investigative history.

## Current product boundary (2026-08-21)

The product has since completed the provider transition selected by
`MAX_GL_GLES_SUPPORT_PLAN.md`: macOS, iOS Simulator, and iOS device now build
one UTM ANGLE/EGL/GLES Metal source matrix, fixed provider, and native
`MTLTexture` contract. CGL/OpenGL.framework and the desktop graphics dependency
family are removed rather than retained as an alternative lane. No Vulkan
facade participates in GL context creation, capsets, symbol resolution, or
resource presentation.

The old Apple OpenGL 4.1 probe is archived input. A focused current 16 KiB
macOS guest diagnostic now naturally reports desktop GL 2.1 and GLES 3.0 and
passes one exact frozen GLES 3 case, but that does not qualify the shared
candidate. The identical signed-app 16 KiB sentinel subsequently passed 1/1
on the iOS 26.5 Simulator and the connected iPad mini 6, measuring the same
desktop GL 2.1/GLES 3.0 guest result. The complete sentinel and agreed matrices,
performance, thermal, and sustained lifecycle evidence remain required. The
first iPad shutdown exposed a `drm_mm` managed-release warning; maintained
kernel topic 0020 now orders atomic scanout shutdown before device unplug, and
three consecutive Simulator plus three connected-iPad sentinel runs powered
off cleanly under a validator that rejects the old signature. GLES 3.1 is
still a direct ANGLE/Metal-only stretch target, and GLES/GL 3.2+ remains out of
scope.

The frozen Aquarium canary has also run through the current direct path on
macOS and Simulator under both page profiles. The first 4 KiB macOS attempt
exposed nested-TLB full-address-space invalidation and RCU starvation, not a
Virgl or ANGLE capability failure. Maintained kernel topic 0021 preserves the
precise Darwin `mmu_gather` range, and fresh accepted 16 KiB/4 KiB kernels then
completed clean macOS canaries at 2.533/2.867 FPS and Simulator canaries at
5.767/5.933 FPS, each over 30 samples with zero dropped or CPU-fallback
frames. These single-run diagnostics validate bring-up and the bounded kernel
repair only; they do not satisfy the frozen seven-pair, thermal, or lifecycle
qualification policy.

A current normal 16 KiB Simulator boot also selected the common hardware
provider and direct Metal presentation and rendered the KDE desktop. That is
product-path evidence. The later allow-listed app CTS run produced a matching
passing Simulator QPA and gated the physical-iPad run, but the focused case
does not revise the qualification boundary or establish a complete profile.

## Method

Every number below is a measurement taken on this host (Apple M1 Ultra,
macOS 26.x, iOS 26.5 Simulator), not a citation. Three probes were written for
this investigation and left in the session scratchpad:

- `glprobe.c` created the accelerated `kCGLOGLPVersion_GL4_Core` CGL context
  used by the former macOS product, then tested exactly
  the extension and version predicates in virglrenderer 1.3.0's
  `src/vrend/vrend_renderer.c` `feature_list[]`.
- `angleprobe.c` does the same against the iOS Simulator ANGLE/Metal host,
  built for `arm64-apple-ios16.0-simulator` and linked against the shipped
  `EGL.framework`/`GLESv2.framework` with `-client_name WebCore` to satisfy
  their `LC_SUB_CLIENT` restriction, then run under `xcrun simctl spawn`. It
  uses the same `EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE` display, the same
  `EGL_OPENGL_ES3_BIT` config, and the same `EGL_CONTEXT_CLIENT_VERSION 3`
  request the emulator uses.
- `vkprobe.c` / `vkfeat.c` query MoltenVK for exactly the requirements
  transcribed from virglrenderer's `src/venus/`.

MoltenVK 1.4.0 and the Vulkan loader here come from Homebrew. That is a
deliberate measurement-only use: it establishes what the only Vulkan
implementation on Apple can do before any production prefix work is proposed.
Nothing in the build graph was changed.

## Finding 1: virglrenderer is not what is missing

`external/sources/virglrenderer/docs/GL-status.txt` records the renderer as
complete through GL 4.5 / GLSL 4.50, including every feature GLES 3.1 and 3.2
need: `GL_ARB_compute_shader`, `GL_ARB_shader_storage_buffer_object`,
`GL_ARB_shader_image_load_store`, `GL_ARB_shader_atomic_counters`,
`GL_ARB_copy_image`, `GL_ARB_texture_view`, `GL_ARB_tessellation_shader`,
`GL_ARB_texture_cube_map_array`, and the rest, all marked `DONE`.

There is therefore no "fix Virgl until it reaches GLES 3.2" project. Virgl
reached it years ago. Every feature the guest does not get is a feature
virglrenderer probed for on the host GL driver and did not find.

## Finding 2: the former macOS host GL driver was the CGL ceiling

`glprobe` output:

```
GL_VERSION  = 4.1 Metal - 90.5
GL_RENDERER = Apple M1 Ultra
GLSL        = 4.10
host reports 43 extensions
```

| virgl feature | needs | on Apple GL | gates guest |
| --- | --- | --- | --- |
| `compute_shader` | GL 4.3 / `ARB_compute_shader` | **absent** | GLES 3.1 |
| `ssbo` | GL 4.3 / `ARB_shader_storage_buffer_object` | **absent** | GLES 3.1 |
| `images` | GL 4.2 / `ARB_shader_image_load_store` | **absent** | GLES 3.1 |
| `atomic_counters` | GL 4.2 / `ARB_shader_atomic_counters` | **absent** | GLES 3.1 |
| `arrays_of_arrays` | GL 4.3 | **absent** | GLES 3.1 |
| `fb_no_attach` | GL 4.3 | **absent** | GLES 3.1 |
| `gles31_compatibility` | GL 4.5 / `ARB_ES3_1_compatibility` | **absent** | GLES 3.1 |
| `copy_image` | GL 4.3 | **absent** | GLES 3.2 |
| `texture_buffer_range` | GL 4.3 | **absent** | GLES 3.2 |
| `blend_equation_advanced` | `KHR_blend_equation_advanced` | **absent** | GLES 3.2 |
| `indirect_draw` | GL 4.0 | present | GLES 3.1 |
| `texture_gather` | GL 4.0 | present | GLES 3.1 |
| `geometry_shader` | GL 3.2 | present | GLES 3.2 |
| `tessellation` | GL 4.0 | present | GLES 3.2 |
| `cube_map_array` | GL 4.0 | present | GLES 3.2 |
| `sample_shading` | GL 4.0 | present | GLES 3.2 |

Seven of the nine GLES 3.1 gates are missing, and three more GLES 3.2 gates on
top. Apple's OpenGL is frozen at 4.1, is itself a Metal translation layer
(`4.1 Metal - 90.5`), has been formally deprecated since 2018, and exposes 43
extensions. It is closed source. Nothing in this repository can raise it.

The guest therefore reports desktop GL 4.1 and OpenGL ES 3.0, which matches
the renderer strings already recorded in `docs/refactor-evidence.md` and
`docs/HOT_PATH_EXPERIMENTS.md`. The guest is not advertising features it
cannot deliver; it is advertising exactly what the host can back.

## Finding 3: the iOS host is lower still

`angleprobe` output on the booted Simulator:

```
EGL         = 1.5  (ANGLE 2.1.22612 git hash: 40dfb3a8bd65)
GL_VERSION  = OpenGL ES 3.0.0 (ANGLE 2.1.22612)
GL_RENDERER = ANGLE (Apple, ANGLE Metal Renderer: Apple iOS simulator GPU, Version 26.5)
explicit ES 3.2 context: REFUSED
explicit ES 3.1 context: REFUSED
host reports 98 extensions

missing for guest GLES 3.1: 9
additional for guest GLES 3.2: 7
```

All sixteen gates are missing. Of the whole set, ANGLE/Metal offers exactly
one related extension, `GL_EXT_multi_draw_indirect`, and that one is not a
GLES 3.1 or 3.2 gate. The Metal backend is ES 3.0 and refuses an ES 3.1
context outright.

This ANGLE is WebKit-derived (`utmapp/WebKit`, `Source/ThirdParty/ANGLE`), and
its Vulkan backend is compiled out: requesting
`EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE` returns `EGL_BAD_ATTRIBUTE` (0x3004).

## Finding 4: GLES 3.2 is unreachable on any single Apple API

This is the decisive result, and it is why the framing needs replacing.

| GLES tier needs | Apple GL 4.1 | ANGLE/Metal | MoltenVK 1.4 |
| --- | --- | --- | --- |
| compute, SSBO, images, atomics (3.1) | **no** | **no** | yes |
| geometry shader (3.2) | yes | **no** | **`geometryShader = FALSE`** |
| tessellation (3.2) | yes | **no** | yes |
| cube map array, ASTC LDR (3.2) | yes | **no** | yes |

`vkfeat` measured MoltenVK directly:

```
geometryShader                  = FALSE   <- GLES 3.2 mandatory
tessellationShader              = TRUE
fragmentStoresAndAtomics        = TRUE
imageCubeArray                  = TRUE
textureCompressionASTC_LDR      = TRUE
```

Apple's GL has the GLES 3.2-tier stages but none of the GLES 3.1-tier compute
and storage features. MoltenVK has the compute and storage features but not
the geometry stage, because Metal has no geometry shader stage and MoltenVK
does not emulate one. Since GLES 3.2 is a strict superset of GLES 3.1, neither
host can reach it, and the union does not exist behind one API.

So "support all the advertised features in GLES 3.2" is not a milestone either
backend can be driven to on this hardware without also writing a
geometry-shader-in-compute emulator. The realistic target is **GLES 3.1**, and
that changes which option wins.

## Finding 5: Venus and MoltenVK do not currently fit together

virglrenderer already ships a complete Venus renderer: 8,617 lines under
`src/venus/` plus a 99,810-line generated protocol. It is disabled at build
time (`dependencies/build.sh` passes `-Dvenus=false`). So the Vulkan option is
also not a from-scratch renderer — it is an integration.

The integration does not currently close:

- **MoltenVK is a portability driver.** Its ICD manifest carries
  `"is_portability_driver": true`, so the loader hides it unless the caller
  passes `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` and enables
  `VK_KHR_portability_enumeration`. `vkr_instance.c` overwrites the guest's
  instance extension list with its own and never sets that flag. Measured
  without the opt-in: `vkCreateInstance = -9`
  (`VK_ERROR_INCOMPATIBLE_DRIVER`). Venus would not see the GPU at all.
- **Venus's memory model is fd-based and Apple's is not.** Venus requires
  `VK_KHR_external_memory_fd` unconditionally in `vkr_allocator.c:222`, and
  exports device memory as `VIRGL_RESOURCE_FD_DMABUF` or
  `VIRGL_RESOURCE_FD_OPAQUE` (`vkr_device_memory.c:528-546`). Measured on
  MoltenVK, with portability enumeration forced on:

  ```
  device[0] = Apple M1 Ultra  (API 1.1.323), 128 device extensions
  VK_KHR_external_memory_fd          ABSENT
  VK_EXT_external_memory_dma_buf     ABSENT
  VK_KHR_external_fence_fd           ABSENT
  VK_KHR_external_semaphore_fd       ABSENT
  VK_EXT_image_drm_format_modifier   ABSENT
  VK_EXT_queue_family_foreign        ABSENT
  => Venus-required extensions absent: 6 of 6
  ```

  MoltenVK offers `VK_EXT_external_memory_host` and `VK_EXT_metal_objects`
  instead. Bridging those into Venus is new upstream work, not configuration.
- **The threading contract changes.** `vkr_renderer_init` refuses to start
  without both `VKR_RENDERER_THREAD_SYNC` and `VKR_RENDERER_ASYNC_FENCE_CB`.
  `docs/VIRTIO_GPU_VIRGL_IMPLEMENTATION_PLAN.md` makes single-thread affinity
  on the GPU transport thread an explicit implementation contract, because CGL
  current-context state is pthread-local.

## Finding 6: the transport cannot carry Vulkan memory today

Venus needs host-visible blob resources. The guest gets those from
`virtio_get_shm_region(vdev, VIRTIO_GPU_SHM_ID_HOST_VISIBLE)`
(`drivers/gpu/drm/virtio/virtgpu_kms.c:177`), and in Linux 6.18.41 only
`virtio_mmio.c` and `virtio_pci_modern.c` implement `.get_shm_region`.
`virtio_uml_config_ops` does not, and there is no vhost-user
`SHMEM_MAP`/`SHARED_OBJECT` handling anywhere in the kernel tree. So
`vgdev->has_host_visible` is structurally false under UML.

The host side is equally bare: `darwin/device/gpu/protocol.hpp` implements 19
Virtio-GPU commands and none of `RESOURCE_CREATE_BLOB`, `SET_SCANOUT_BLOB`,
`RESOURCE_MAP_BLOB`, `RESOURCE_UNMAP_BLOB`; a grep for blob or shmem across
the whole GPU device returns nothing.

Closing this means inventing a host-visible aperture for UML: a new
vhost-user backend message, a new `.get_shm_region` in `virtio_uml.c`, a UML
physical-memory mechanism to map host pages into guest physical space at a
reserved range, and a matching Linux patch topic in the maintained series.
That is the single largest item in either option.

## Finding 7: the guest is already ready for Vulkan

Scanning `rootfs.erofs` finds `libvulkan_virtio.so` (Mesa's Venus driver),
`virtio_icd.json`, `zink_dri.so` (Mesa's GL-on-Vulkan), and
`libvulkan_lvp.so`. The guest needs no new packages for the Vulkan path;
Zink over Venus is the route to guest GLES, and it is already installed.

## Verdict

Neither option reaches GLES 3.2. The honest comparison is at GLES 3.1:

| | complete GLES 3.2 via Virgl | native Vulkan (Venus) |
| --- | --- | --- |
| Reaches GLES 3.1 | **impossible** — host GL lacks compute/SSBO/images on both products | plausible — MoltenVK has all of them |
| Reaches GLES 3.2 | impossible | impossible — `geometryShader = FALSE` |
| Renderer code to write | none; virgl is already complete | none; Venus is already complete |
| Blocked by | Apple's closed, frozen, deprecated GL 4.1 and a WebKit ANGLE capped at ES 3.0 | portability enumeration, 6 absent extensions, blob transport, UML aperture, threading model |
| Can we unblock it | **no** | yes, but it is a multi-part project including a Linux patch topic |

So the Vulkan backend is the only one of the two that can raise the ceiling at
all, and "fixing Virgl" is not merely harder, it is closed: the missing
features are in a vendor driver we cannot patch. That said, the Vulkan path as
literally specified is large, and its riskiest piece (the UML host-visible
aperture) is a kernel change, not a host change.

## Historical recommendation (superseded): measure the cheap middle path first

The investigation proposed a third option before the performance-first plan
forbade a Vulkan translation layer in the GL facade:

**keep the entire Virgl protocol, host device, kernel, and guest stack exactly
as they are, and change only which GL implementation the host hands to
virglrenderer** — from Apple CGL / WebKit-ANGLE-on-Metal to upstream ANGLE
with its Vulkan backend running on MoltenVK.

The evidence for testing it: everything virgl needs for guest GLES 3.1 that
Apple GL lacks is present in MoltenVK's feature set (compute, SSBO, images,
`fragmentStoresAndAtomics`, indirect draw), and ANGLE's Vulkan backend does
support ES 3.1 upstream. Nothing in the Virtio-GPU protocol, `virtio_uml.c`,
the vhost-user transport, or the rootfs would change; only
`createGlContext`/`getEglDisplay` in `darwin/device/gpu/virgl_renderer.cpp`
and the dependency prefix.

This remained explicitly **unmeasured** and was not selected. It could not be probed here because upstream
ANGLE with the Vulkan backend is not in the tree and building it needs network
access outside `dependencies/prepare.sh`. The next step is to bring that
build in under the normal dependency rules and re-run `angleprobe.c` against
it was superseded by the direct ANGLE/Metal-only GL policy. Vulkan and Venus
remain separate work under `MAX_VULKAN_SUPPORT_PLAN.md`; they cannot promote a
GL/GLES claim under the current plan.

## What was not done

No production code, build configuration, dependency prefix, or kernel patch
was changed by this investigation. No guest was booted; the guest-side
renderer strings quoted here are the ones already recorded in the repository.
The ANGLE-on-Vulkan-on-MoltenVK hypothesis in the recommendation is reasoning
from measured MoltenVK features, not a measurement of that stack.
