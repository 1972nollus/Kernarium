# Apple graphics support

## Current path

Every Apple product uses one Virtio-GPU rendering path on one host GL
provider:

```text
guest Mesa -> Virtio-GPU/Virgl -> vhost-user -> virglrenderer
    -> Mesa Zink over KosmicKrisp -> MTLTexture -> AppKit or UIKit
```

The provider is Mesa's Zink Gallium driver on the packaged KosmicKrisp Vulkan
driver, reached through Mesa's surfaceless EGL platform; it renders on a
desktop OpenGL 4.6 core context. virglrenderer and libepoxy are upstream
releases carrying the maintained host series, not vendor forks. The graphics
dependencies come from the verified `apple-gpu-vulkan-experiment` prefix. No
Apple GPU product links CGL, OpenGL.framework, EAGL, MoltenVK, a Vulkan
loader, or a second ambient libepoxy/virglrenderer family.

macOS and the iOS device both build that stack from the same sources; the
prefixes differ only in their platform contract and deployment target. The iOS
Simulator does not, because KosmicKrisp requires Metal 4 and the Simulator
exposes no Metal 4 device: its GPU preset and
`dependencies/build-apple-gpu.sh --platform ios-simulator` fail early with that
reason rather than configuring against a device that cannot exist. Until a
physical device is attached, the iOS slice is exercised by running the
unmodified device build on an Apple silicon Mac in Designed for iPad mode,
where Metal reports the host GPU and the full Metal 4 family. The device
runtimes take the Vulkan-experiment kernel line with that stack: desktop GL
4.6 needs `GL_ARB_buffer_storage`, whose coherent buffers reach the guest
through the vhost-user shared-memory aperture only that line carries.

The GPU transport thread owns renderer creation, context use, reset, and
teardown. AppKit and UIKit share the Metal presentation layer. VNC is a macOS
consumer of the same renderer and is intended for diagnostics and remote
display, not as a second graphics backend. With no connected VNC client,
scanout flushes retire without a synchronous GPU-to-CPU readback; the first
frame after a zero-client interval refreshes the complete active scanout.

Fenced Virgl submissions and host transfers from ordinary guest contexts may
remain in flight together. Their descriptor chains retire only after the
renderer reports the corresponding host-owned timeline cookie. Commands that
mutate or tear down renderer state wait until those deferred fences retire;
reset cancels the timer and discards deferred transport state before releasing
renderer resources.

Pointer semantics are selected by the product launcher. Android native and VNC
input is exposed as one direct touch contact with absolute coordinates; hover,
auxiliary buttons, and wheel events are suppressed, and coordinates are sent
only for touch down, drag, and release. Non-Android guests retain absolute
pointer hover, primary and auxiliary mouse buttons, and wheel input.

## Guest capability boundary

On macOS the guest reports desktop OpenGL 4.6 core and GLSL 4.60 through
Virgl, because the host renders the Virgl command stream on a desktop OpenGL
4.6 core context. OpenGL 4.4 and above additionally require
`GL_ARB_buffer_storage`, which the guest exposes only over coherent
host-visible blob resources: the virtio-GPU device publishes its host-visible
aperture to Virgl contexts as well as Venus ones, and vrend backs those
coherent buffers with shared host pages pinned into GL. The aperture is part
of the experimental Vulkan device, so a macOS Virgl guest reaches 4.6 only
when that device is enabled.

GLES reaches the guest over the same path and from the same capsets: the
guest's Mesa derives its GLES version from what Virgl advertises, so nothing
targets GLES separately and no GLES-only host provider exists.

The formats Virgl advertises are the formats the host really stores. A sized
depth internal format is only a request in OpenGL -- the host may satisfy
`GL_DEPTH_COMPONENT32` with 24 bits, as Zink does -- and the guest's Mesa turns
the advertised list into its EGL and GLX configs, so an inexact one becomes the
storage of a default framebuffer the guest cannot name. vrend therefore offers
a depth format only where the host's own storage has the width and datatype
that format names, and a guest config with 32 depth bits is backed by a real
32-bit float depth buffer.

That version is the profile the guest reports, not a conformance result. Guest
Zink over Venus is the qualified desktop OpenGL path on macOS; the Virgl path's
own conformance standing is whatever its recorded campaigns show.

The boundary that fixes this ladder has two sides. Guest userspace is an
external input: the Mesa, libdrm, WSI, CTS executables and desktop packages
installed in `rootfs.erofs` are used exactly as they are, never replaced,
preloaded, overlay-substituted, rebuilt, or repackaged. The guest kernel is
this project's: required `virtio_gpu`, DRM, UAPI, Kconfig, or `arch/um`
transport work belongs in the maintained Linux series. Prefer standard
Virtio-GPU and DRM interfaces; a kernel-to-emulator extension is acceptable
only when negotiated, bounded, invisible to guest userspace, and presented
through an ABI the unmodified guest Mesa and libdrm already understand. A
private userspace ioctl or library must never become a production dependency.

Hardware video decode is not advertised. It published frames through a Metal
texture client buffer that only the retired ANGLE provider exposed, and the
renderer is built without it. Scanouts of Virgl resources take the host
readback path, while Venus scanouts keep their native Metal textures.

The GPU advertises display geometry and density through EDID. Kernarium point
mode uses safe-area points and 160 DPI. Retina mode multiplies dimensions and
density by `nativeScale`. Android receives the same effective density on its
kernel command line.

## Dynamic native geometry

Native AppKit and UIKit displays publish host geometry only after it settles.
AppKit retains the active guest mode throughout live window resize and
publishes the final logical-point content size when live resize ends. UIKit
retains it through an interactive view-controller transition and publishes the
final point or Retina-pixel size from transition completion. Non-interactive
layout changes are coalesced until their geometry is stable.

Host-to-guest automatic resizing is enabled by default. Kernarium stores the
choice in its iOS app settings; the macOS executable selects it with
`--gpu-auto-resize=on|off`. When enabled, a settled host change updates the
Virtio-GPU advertised mode and raises one display event. Duplicate callbacks
for the same settled size do not raise another event. Presentation storage
remains sized to the active guest scanout until the guest applies its eventual
mode with `SET_SCANOUT`.

Every applied guest mode is authoritative for presentation, whether selected
by the guest or produced by guest-side rounding of a host request. macOS
resizes its content window to that applied size; the resulting host callback
does not feed the same size back to the guest. iOS instead aspect-fits every
guest mode in its view. A guest-initiated mode does not disable later automatic
host-to-guest resize requests. With automatic resizing configured off, host
window or view changes leave the guest mode unchanged and aspect-fit it in the
available host content area. EDID-backed dimensions are limited to 4095 pixels
per axis; the non-EDID Virtio-GPU path follows the DRM limit of 8192 pixels per
axis.

The macOS native window's Display menu can rotate presentation clockwise or
counterclockwise in 90-degree steps without changing the guest scanout or
advertised mode. Quarter-turns swap the content window dimensions, and later
host resize dimensions are translated back into guest orientation before they
are advertised. The inverse presentation transform is applied to pointer and
touch coordinates. An active contact ends when rotation changes and cannot
restart until the physical gesture is released.

Rendering and input use the same centered aspect-fit rectangle. AppKit maps
that rectangle into the fixed tablet range, while Kernarium maps it into the
touchscreen range advertised at boot. A touch beginning in a letterbox or
pillarbox bar remains outside the guest until its path enters the fitted
rectangle, where a new contact begins at the exact edge. Crossing back out
releases the contact at the exit edge; re-entry while the physical gesture
continues begins a new guest contact. Pointer and touch coordinates therefore
remain aligned with the pixels that are actually visible.

## Vulkan

Guest Vulkan is an experimental, separately named capability, not part of the
supported product. It exists only in the `apple-gpu-vulkan-experiment`
dependency product, where Venus runs over KosmicKrisp on Metal and the guest
reaches the host ring through the kernel's vhost-user shared-memory aperture.
No build advertises a Vulkan capset without `UML_GPU_EXPERIMENTAL_VULKAN`.

That route makes no conformance claim. Support is asserted only by the
receipt-bound guest CTS campaigns described in
[`testing.md`](testing.md#graphics-conformance-campaigns), which bind a run to
its sealed image receipt, its CTS and Mesa source locks, and the staged
provider digest. Accepted `NotSupported`, warning, and `Waiver` results are not
passes.

### Android guest requirements

The Android guest reaches the same renderer by a separate route. Its GLES stack
is a GLES-only Soong port that installs `gallium_dri`, the Mesa EGL and GLES
libraries, and a `virtio_gpu_dri.so` symlink; its Vulkan HAL is Mesa's Venus
driver, built from this project's own guest Mesa source and installed as
`vulkan.virtio.so`, with gralloc buffers from minigbm and presentation through
`VK_ANDROID_native_buffer`. That contract, including which guest epoch the
hardware-buffer path needs and why no dEQP level is declared, lives in
[`../android/aosp-17/README.md`](../android/aosp-17/README.md#graphics).

Khronos publishes Android's mandatory Vulkan functionality as the
`VP_ANDROID_17_requirements` profile, which chains the
`VP_ANDROID_vulkan_profile_2025` baseline. The Linux Venus guest meets its API
version (the guest reports 1.4.354 against a required 1.4.335), its limits, its
formats, and both of its either-or capability groups, but three requirements are
standing limits of this stack rather than work that is merely outstanding:

* `protectedMemory` has no Metal equivalent.
* `shaderBufferInt64Atomics` needs 64-bit buffer atomics this Metal feature set
  does not provide.
* `shaderFloat16` is withheld on purpose, because native half arithmetic can
  time out Apple GPUs when NaN-heavy graphics shaders run concurrently. The
  16-bit storage classes are separate from it and are supported, including
  `storageInputOutput16`: a varying narrower than 32 bits is widened to a
  32-bit one before the shader chain is linked, because Metal's raster
  interface does not carry every half bit pattern and a 16-bit integer varying
  needs it to. The widening costs no varying space, since a 16-bit varying
  occupies a whole slot here either way.

The Android window-system prerequisites the renderer owns are in place.
KosmicKrisp advertises `VK_EXT_queue_family_foreign`, which costs it nothing on
a single-queue device over unified memory, and implements
`VK_EXT_image_drm_format_modifier` with `DRM_FORMAT_MOD_LINEAR` as its only
modifier, because Metal describes no tiling an agent outside the driver could
agree on. A modifier image is therefore exactly a linear image with a name for
its layout and inherits nearly every linear limit: one 2D level and layer, a
dedicated allocation, no depth or stencil attachment, no input attachment, and
an explicit plane layout accepted only where it matches the layout Metal's own
alignment rule produces. The one capability it carries that plain linear tiling
does not is a colour attachment, because a swapchain image is one and Mesa's
common WSI has no fallback when no modifier can hold one: it filters the
modifier list with `vkGetPhysicalDeviceImageFormatProperties2` and, when nothing
survives, fails `vkCreateSwapchainKHR` with `VK_ERROR_OUT_OF_HOST_MEMORY`, which
dEQP records as a `ResourceError` that ends the whole CTS session. Metal renders
into a linear texture; what it refuses, by an assertion that ends the host
process, is a linear texture of any type but `MTLTextureType2D`, and this driver
binds every input attachment as a 2D array. An input attachment is therefore
what a linear layout has to refuse, and a layout Metal could not represent
becomes a Vulkan error before Metal sees the descriptor. The guest Venus driver's two conditions for
`VK_ANDROID_native_buffer` and
`VK_ANDROID_external_memory_android_hardware_buffer` are therefore met, and an
Android guest presents its swapchain through them.

The four window-system extensions the profile lists are the Android Vulkan
loader's own work, not an ICD's. `frameworks/native/vulkan/libvulkan`
implements `VK_GOOGLE_surfaceless_query`,
`VK_EXT_present_mode_fifo_latest_ready`, `VK_GOOGLE_display_timing` and
`VK_EXT_present_timing` on top of `VK_ANDROID_native_buffer` and advertises
them as loader extensions: the first unconditionally, the second behind a
build flag, the third when `service.sf.present_timestamp` is set, and the
fourth additionally only when the ICD exposes `VK_KHR_calibrated_timestamps`.
Every layer of this stack already carries that one dependency, so the guest
Venus driver owes no common-WSI work for any of the four.

The pinned venus-protocol is v1.1.3, which tracks the Vulkan 1.4.357 registry
and carries 201 extensions; the maintained series on top of it raises the
table to 205. An extension the protocol does not name has no
index in `vn_info_extension_index`, so the guest silently refuses it however
the other layers are configured; the uprev is what makes `VK_KHR_maintenance8`,
`VK_KHR_maintenance9`, `VK_KHR_maintenance10`,
`VK_KHR_device_address_commands`, `VK_KHR_unified_image_layouts`,
`VK_EXT_vertex_attribute_robustness` and `VK_EXT_primitive_restart_index`
reachable at all. Both the guest driver and the renderer generate their halves
of the encoder from that one pin at build time - the driver through
`-Dvenus-protocol-dir`, which makes Mesa's vendored `src/virtio/venus-protocol`
copies unused here - so the two halves must be replaced together: a guest image
sealed against one protocol cannot talk to a renderer built from another.

The four extensions the pin itself does not describe -
`VK_EXT_image_compression_control` with its swapchain companion,
`VK_EXT_device_address_binding_report` and `VK_KHR_pipeline_binary` - are
carried by the maintained protocol series. The first three define no command
and need only an entry in the generator's extension list; pipeline binaries
additionally need command ids for five entrypoints and a `VkPipelineBinaryKHR`
object type in the renderer's object tracking, which is the real cost of that
one.

Two of the four are advertised honestly rather than completely, because Metal
does not express what they describe:

* Metal decides for itself whether a texture's contents are compressed, never
  says so, and offers no fixed-rate control. Every image compression query the
  host driver answers therefore reports no compression, and a format query that
  insists on a fixed rate is refused. Accepting the control an application
  passes costs nothing, because there is nothing to turn off.
* An address binding report is delivered through a debug messenger callback.
  The host driver emits one for every buffer, image plane and device memory
  allocation it binds or unbinds, so a messenger on the host sees them, but
  Venus answers `VK_EXT_debug_utils` in the guest and the protocol carries no
  host-side callback, so a report cannot cross the ring. The guest advertises
  the extension, which is what the profile asks for, and leaves
  `reportAddressBinding` false.

`VK_KHR_pipeline_binary` needs no host driver entrypoints: KosmicKrisp compiles
shader objects rather than monolithic pipelines, so Mesa's common runtime
serves all five commands from the shader binaries it already produces. Its
internal-cache properties are all false, because the driver keeps no pipeline
cache of its own. `MTLBinaryArchive` cannot stand in for a pipeline binary: it
is keyed by internal pipeline descriptors and serializes only as a whole
archive to a file URL, so it can neither produce one binary per pipeline nor
return a binary's bytes.

One of its five commands does not cross the ring. `vkGetPipelineKeyKHR` names
the pipeline it should key through `VkPipelineCreateInfoKHR::pNext`, and no
create info declares itself an extension of that structure, so the generated
codecs carry nothing in that chain and the renderer receives it empty. The
renderer refuses such a request with `VK_ERROR_UNKNOWN` instead of handing an
empty chain to a driver entitled to read it; only a key for the device itself,
which names no pipeline, survives the round trip. Carrying the create info
would change the wire format, so it belongs with a guest image epoch built
against the same protocol.

### Experimental host memory

Native graphics allocations are separate from the configured guest RAM limit.
KosmicKrisp releases obsolete pipeline resources with their last Vulkan owner
and one-time recording storage after GPU completion; see the
[dependency ownership contracts](../dependencies/README.md).

Budget campaign concurrency from host physical footprint and the
`IOAccelerator (graphics)` row of `vmmap -summary`, rather than process RSS,
which omits swapped-out allocations.

## Validation

Use `check-gpu` for native GPU ownership and VNC checks and
`check-gpu-asan-ubsan` for the sanitized path. Guest conformance, WebGL,
Aquarium, and physical-device campaigns are opt-in; see
[`testing.md`](testing.md).

Historical backend and performance experiments are summarized in
[`optimization-history.md`](optimization-history.md).
