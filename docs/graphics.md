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
desktop OpenGL 4.6 core context. The provider names its EGL device explicitly,
so its display is never the default surfaceless display: another client of the
process's EGL library, such as Kernarium's own graphics check, may initialize
and terminate that one without touching a guest's contexts. virglrenderer and
libepoxy are upstream releases carrying the maintained host series, not vendor
forks. The graphics
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
scanout flushes retire without any GPU-to-CPU readback. A client that connects
after such an interval dropped frames receives its first framebuffer update
only once a complete scanout newer than its connection is published: the
display reads the newest native Metal scanout back at once, and a scanout only
the CPU path can read refreshes in full on the guest's next flush. The client
waits at most 2 s for it, so a static CPU-path scanout then arrives as last
published.
The VNC display converts native Metal scanouts with an asynchronous Metal
readback. A readback requested before the framebuffer was cleared, re-placed
for a new mode, or written by a CPU-path copy is discarded when it completes,
so it never repaints newer contents or counts as a complete frame, and the
next readback reads its whole scanout.

The VNC framebuffer keeps the size it was created with, which the device
advertises as the preferred mode, and shows every applied guest mode
aspect-fitted and centred in it, black outside. A mode the fit leaves at its
own size, such as the preferred one, is copied pixel for pixel. Any other mode,
and any texture stored bottom row first, is drawn into a texture the size of its
fitted rectangle by the native presenter's scanout pass, with the same nearest
magnification and bilinear minification, and only the part of it that the
flush's damage reaches is read back. An oversized mode therefore costs the GPU
one scaled draw and the CPU only the scaled pixels; glmark2's 4096x2160 mode
fills a 1280x720 framebuffer's width at 1280x675. A CPU-path scanout at such a
mode is kept in a host texture of the whole mode and scaled by the same pass,
so it looks the same and a late client receives it at once. Pointer and touch
input map back through the fitted rectangle by the native display's rules
below.

Images shared through native Metal heaps retain tiled storage but disable
usage-dependent lossless compression. A compositor can therefore import a
render target for sampling without reinterpreting its compression metadata as
pixels. Ordinary Vulkan images keep their eligible texture optimizations.
A compositor on Virgl imports such an image as well. Its blob reaches vrend as
a shared-memory file that only names the heap, so vrend reads the pixels where
they are: the rows of a linear image or a buffer from the heap's shared
storage, which it aliases like any shared-memory segment, and an optimal image
through the Metal texture its exporter published for scanout, which it reads
into pages of its own before each read.

The macOS native display turns the emulator into an AppKit application, which
makes the whole process eligible for App Nap whenever its window is occluded or
minimized or the screen is locked. A napped process runs every thread,
including the vCPU carriers, in the background priority band (`ps -M` shows
priority 4 instead of 31) and has its timers coalesced. The display therefore
holds a user-initiated, latency-critical `NSProcessInfo` activity for as long as
its run loop runs; the activity still allows idle system sleep. No other macOS
display mode registers the process with AppKit.

The emulator exits when the guest stops even if a menu of the native window is
open. An open menu runs its own event loop, which drains neither the main
dispatch queue nor `-[NSApplication stop:]`, so the display ends any menu
that is tracking, or that starts tracking, once the guest has finished, and
only then leaves its run loop.

A guest cannot tell a presented frame from a discarded one: every scanout
flush succeeds whether the host presented it, dropped it, refused the native
texture, or read it back out of memory that holds no pixels. Setting
`UML_GPU_DISPLAY_ACTIVITY_LOG` to a writable path makes the device append
every scanout it is given with its resource kind and geometry, the path and
outcome of every flush, the reason a display refused a native texture, and the
guest, presented, dropped, and CPU-fallback frame counters of whichever
display is active. Repeated lines are counted rather than written. It is a
file because the emulator's stderr shares the guest console and loses lines
under load; unset, nothing is opened or written. `UML_GPU_FRAME_STATS=<seconds>`
remains the stderr-only frame-rate report for the native display.

Fenced Virgl submissions and host transfers from ordinary guest contexts may
remain in flight together. Their descriptor chains retire only after the
renderer reports the corresponding host-owned timeline cookie. Commands that
mutate or tear down renderer state wait until those deferred fences retire;
reset cancels the timer and discards deferred transport state before releasing
renderer resources. A command buffer or transfer the renderer refuses fails
alone: the context error it reports is cleared with it, so a context that only
transfers, as Android's gralloc mapper does to lock a buffer for reading, keeps
working after one refusal.

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
coherent buffers with shared host pages pinned into GL. The device always
publishes the aperture, so a macOS Virgl guest reaches 4.6.

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

Applications that use stateful V4L2 decoding can instead use the independent
[Virtio video service](../darwin/device/video/README.md). It exposes the same
host codec profile set through the shared Linux and Android driver and copies
decoded planes into guest memory. Its protocol and buffer ownership do not
depend on Virgl or VA-API.

Which decode API a guest application can reach is a property of that
application's own build, not of this device. A Linux Chromium or CEF built for
arm64 discovers the stateful decoder through `/dev/video-dec*`; the same
upstream project built for x86-64 carries no stateful V4L2 decoder at all and
offers only VA-API, for which no driver over a V4L2 stateful decoder exists.
An x86-64 guest therefore reaches this service only from an application that
speaks V4L2 itself.

H.264 High10 is advertised only on Apple targets where a profile-specific
VideoToolbox hardware probe succeeds. Qualified physical iOS hardware produces
10-bit bi-planar output. A session-only probe on macOS is insufficient: native
decoding rejects a representative High10 frame, so macOS does not advertise
the profile. Upstream Chromium maps HEVC Main and Main10 to its VA-API decoder
but does not map H.264 High10; the latter remains available on qualifying hosts
to upstream VA-API clients such as FFmpeg.

The same rule binds the compressed formats on an iOS device. Metal defines the
BC/S3TC, RGTC and BPTC pixel formats everywhere, but only a device that reports
`MTLDevice.supportsBCTextureCompression` accepts a texture in one; an Apple GPU
on iOS does not, and Metal answers such a descriptor by aborting the process
rather than failing the call, so a driver cannot recover from having offered
one. KosmicKrisp therefore asks the device once and reports those three layouts
as unsupported where it says no, which removes them from the Vulkan format
features and so from the GL formats Virgl advertises above them. The Vulkan
`textureCompressionBC` feature answers from that same question: a driver that
claims the feature while denying all sixteen BC formats fails
`dEQP-VK.api.info.format_properties.compressed_formats` on the disagreement
alone. Every Apple silicon Mac supports BC, so macOS is unaffected.

That version is the profile the guest reports, not a conformance result. Guest
Zink over Venus is the qualified desktop OpenGL path on macOS; the Virgl path's
own conformance standing is whatever its recorded campaigns show.

A point is clipped against user clip and cull distances and against the depth
planes, never against the view-volume side planes, which is what KosmicKrisp
reports as `VK_POINT_CLIPPING_BEHAVIOR_USER_CLIP_PLANES_ONLY`. A wide point
whose centre sits just outside a viewport edge therefore still rasterizes the
part that reaches the viewport, as the conformance reference rasterizer and
OpenGL require; clipping such a centre instead made Zink's wide points vanish
at an edge. The hardware keeps its own guard band around the volume, measured
at 1.5 in normalized device coordinates on every viewport size and whatever
the point size, and drops a point whose centre leaves it.

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
renderer is built without it.

A Virgl scanout reaches the native display as a Metal texture, not as pixels
read back into CPU memory. Zink creates each GL texture's image as an
exportable Metal texture, and virglrenderer names that texture through
`virgl_renderer_resource_get_info_ext()`, asking Zink for it over
`MESA_GLINTEROP`; a resource whose GL storage is not a single-sample,
single-layer 2D texture has none. The device looks the texture up on every
flush, for classic resources and for the blobs a Virgl context creates alike;
Android's gralloc allocates the compositor's framebuffers as such blobs. It
presents the texture only when it holds exactly the scanout's source image, at
that image's size and channel order, and a resource stored bottom row first
(`VIRGL_RESOURCE_Y_0_TOP`) is turned the right way up by the presenting shader.
Presentation needs no host fence of its own: the guest flushes a scanout only
after the fence of the rendering it shows has retired, and a Virgl context
fence retires only once Zink's GPU work has completed. Anything else takes the
CPU readback path, which the activity log records as `path=readback` for a
blob and `path=cpu` for a classic resource, and the frame counters as a
CPU-fallback frame. VNC reads a bottom-up texture back through the same
scanout pass, which turns it upright before the copy. Venus scanouts keep
their native Metal textures.

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
mode with `SET_SCANOUT` or `SET_SCANOUT_BLOB`.

Every applied guest mode is authoritative for presentation, whether selected
by the guest or produced by guest-side rounding of a host request, and whether
its scanout is a classic resource or a blob shown through its Metal texture.
The scanout rectangle is the mode. macOS
resizes its content window to that applied size; the resulting host callback
does not feed the same size back to the guest. iOS instead aspect-fits every
guest mode in its view, and VNC in its fixed framebuffer, magnifying a smaller
mode and minifying a larger one. A guest-initiated mode does not disable later
automatic host-to-guest resize requests. With automatic resizing configured
off, host window or view changes leave the guest mode unchanged and aspect-fit
it in the available host content area. EDID-backed dimensions are limited to
4095 pixels per axis; the non-EDID Virtio-GPU path follows the DRM limit of
8192 pixels per axis.

The macOS native window's Display menu can rotate presentation clockwise or
counterclockwise in 90-degree steps without changing the guest scanout or
advertised mode. Quarter-turns swap the content window dimensions, and later
host resize dimensions are translated back into guest orientation before they
are advertised. The inverse presentation transform is applied to pointer and
touch coordinates. An active contact ends when rotation changes and cannot
restart until the physical gesture is released.

The native presenter draws a scanout's Metal texture whatever the size of its
CPU framebuffer: its Metal pass scales the scanout rectangle, which may be any
part of the source image, into the host view at the rectangle's own aspect
ratio, and samples no texel outside the rectangle. Magnification uses nearest
filtering, so guest pixels stay sharp at Retina and integer scales;
minification is bilinear, blending each host pixel's nearest 2x2 texels rather
than dropping whole rows and columns. Guest textures carry no mip chain, so a
reduction beyond 2:1 still skips texels. The display refuses a texture into the
CPU fallback only when it is not a single-layer 2D texture of the display's
Metal device, when it is smaller than the source, or when the rectangle is
empty or reaches outside the source.

Rendering and input use the same centered aspect-fit rectangle. AppKit and VNC
map that rectangle into the fixed tablet range, while Kernarium maps it into
the touchscreen range advertised at boot. VNC rounds the rectangle to whole
framebuffer pixels and maps a pointer at the centre of the pixel it names, so a
pointer lands on the guest pixel drawn under it. A mouse in a bar reaches the
guest only while it drags buttons first pressed inside, clamped to the edge,
and buttons pressed in a bar stay with the host until all are released. A
guest mode change releases the buttons or contact the guest holds and keeps
the rest of that gesture from it. A touch beginning in a letterbox or
pillarbox bar remains outside the guest until its path enters the fitted
rectangle, where a new contact begins at the exact edge. Crossing back out
releases the contact at the exit edge; re-entry while the physical gesture
continues begins a new guest contact. Pointer and touch coordinates therefore
remain aligned with the pixels that are actually visible.

## Vulkan

Every GPU build advertises guest Vulkan. Venus runs over the packaged
KosmicKrisp on Metal, and the guest reaches the host ring through the kernel's
vhost-user shared-memory aperture. The device always offers the Venus capset,
blob resources, and that aperture; no runtime switch withholds them.

Venus exposes KosmicKrisp's real version, extensions, features, properties,
formats, queues, limits, and memory model, with no injected extension or forced
version, and only what runs reliably through the whole guest-to-Metal path is
advertised. No advertised capability depends on CPU shader interpretation, CPU
rasterization, resource-sized steady-state copies, or readback-driven command
decisions. Functionality Metal or KosmicKrisp cannot run reliably is reported
unsupported instead, as `shaderFloat16` is below.

That route makes no conformance claim. Support is asserted only by the
receipt-bound guest CTS campaigns described in
[`testing.md`](testing.md#graphics-conformance-campaigns), which bind a run to
its sealed image receipt, its CTS and Mesa source locks, and the staged
provider digest. Accepted `NotSupported`, warning, and `Waiver` results are not
passes. The last complete Vulkan CTS 1.4.6.2 campaign, on Mesa 26.2.1 in
September 2026, ran all 16 fractions of the mandatory list with no disallowed
result.

### Guest memory boundary

A guest application drives this renderer, so every layer that writes through an
address derived from guest state bounds that write itself. No guest can make
the host write outside the memory it gave the host, whatever the GPU does.

Only a resource's attached backing reaches guest RAM: a blob resource carries
no guest pages, and the device translates a backing entry only as a whole range
inside one negotiated memory region. A control-queue reply is clamped to the
writable descriptors the guest supplied. A Venus reply stream ends where the
window the guest declared through `vkSetReplyCommandStreamMESA` ends rather
than where the shared-memory resource holding it ends, so an oversized reply
marks the context fatal instead of running on through whatever else the guest
keeps in that resource.

In the driver, a transform feedback store is bounded by the buffer range bound
for it. A capture store's address is derived from the bound buffer's capacity,
the byte counter the application supplies through a counter buffer, and the
output layout, and the counter survives across draws, so every term of that
derivation is application state. Each store recomputes its own extent and drops
a write that would leave the bound range.

### Host-visible memory is bounded by the blob aperture

Host-visible Vulkan memory never becomes guest pages. The renderer allocates it
as host shared memory and the device maps it into the guest's blob aperture,
which is one fixed guest-physical region of
`protocol::hostVisibleSharedMemoryRegionSize`. That constant and the kernel's
`UML_VHOST_USER_SHMEM_SIZE` are one value, because `uml_vhost_user_shmem_claim`
refuses a region of any other size; changing it is a change to both maintained
kernel lines as well as to the host.

The aperture, not host memory, is therefore what bounds host-visible Vulkan
memory, and the renderer advertises it that way: the synthesized host-visible
memory type gets its own heap whose size is the aperture, the same split a
discrete GPU makes between its VRAM and the visible window into it. Heap
budgets the host driver reports are mapped onto those heaps and bounded by
them. Without that split a guest sizes its largest host-visible mapping from
the host's whole system memory and asks for a mapping the aperture can never
place.

The peak the aperture must hold is larger than the mapping itself. The guest's
blob allocator releases an aperture node only after the host has retired the
resource, which takes one transport round trip, so a guest that allocates,
maps and frees in a loop keeps the previous node resident while the next one is
placed: two nodes concurrently on an idle host, and more where the host's
retirement lags further behind. An aperture that cannot place the next node
fails the blob at `vkMapMemory` with `VK_ERROR_OUT_OF_DEVICE_MEMORY`, which a
conformance run reports as a session-ending `ResourceError`. The memory itself
stays mappable: the guest kernel has already created the blob resource when
placement fails and destroys it at once, and the renderer lets a memory whose
resource is gone be exported again, so a later `vkMapMemory` succeeds once the
aperture has room. A host-visible heap of one aperture therefore leaves a
mapping of a quarter of it usable with room to spare.

Device memory is charged to the application like guest RAM, so the host gives
Venus a share of the guest's allocation: half of `mem=`. The renderer shrinks
every advertised heap and budget to that share, so a guest sizes its
allocations from the share instead of from the host's memory. The Virtio
balloon then takes the same amount out of the guest's RAM (see [memory
architecture](architecture.md#memory-and-executable-code)).

The renderer also refuses a new, non-import allocation that would take the
process's Metal allocation (`MTLDevice.currentAllocatedSize`, which also counts
presentation and host GL memory) past the share. The application does not see
that refusal as `VK_ERROR_OUT_OF_DEVICE_MEMORY`. Venus allocates
asynchronously, so `vkAllocateMemory` has already succeeded in the guest; the
first later command that names the missing memory fails in the renderer, which
marks the Venus ring fatal, and the guest driver aborts the process. The
counter also includes memory the guest has already freed while its blob
resource is still referenced: a freed swapchain image stays allocated until the
guest's resource release arrives on the control queue, a few milliseconds after
`vkFreeMemory`. A loop that frees and reallocates large images can therefore be
refused while its live memory is below the share.

### Android guest requirements

The Android guest reaches the same renderer by a separate route. One Soong port
of this project's own guest Mesa source provides both of its graphics stacks:
the Virgl gallium driver behind the `kernarium` EGL and GLES libraries, and
Mesa's Venus driver installed as the Vulkan HAL `vulkan.virtio.so`, with
gralloc buffers from minigbm and presentation through
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
dedicated allocation, no depth or stencil attachment, and an explicit plane
layout accepted only where it matches the layout Metal's own alignment rule
produces. What it carries that plain linear tiling does not is a colour and an
input attachment, because a swapchain image is one: Mesa's common WSI has no
fallback when no modifier can hold a colour attachment (it filters the modifier
list with `vkGetPhysicalDeviceImageFormatProperties2` and, when nothing
survives, fails `vkCreateSwapchainKHR` with `VK_ERROR_OUT_OF_HOST_MEMORY`, which
dEQP records as a `ResourceError` that ends the whole CTS session), and
Android's loader offers `VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT` on every surface,
which Unity requests. Metal renders into a linear texture; what it refuses, by
an assertion that ends the host process, is a linear texture of any type but
`MTLTextureType2D`. The driver reads every input attachment as a 2D array, so
a linear image stays a 2D texture and its input attachment is read through a 2D
array view of it, which Metal does make of a texture over a buffer. A layout
Metal could not represent becomes a Vulkan error before Metal sees the
descriptor.

The guest's common WSI takes that path as well. A dma-buf shared without a
modifier names no layout, and once the renderer has modifiers every guest
importer reads one as linear: Zink gives every shareable image
`DRM_FORMAT_MOD_LINEAR` on Venus and imports an implicit dma-buf as linear, as
upstream Venus does. A swapchain whose surface offers modifiers is therefore a
`DRM_FORMAT_MOD_LINEAR` image, and one whose surface offers none renders
optimally and reaches the window system through common WSI's linear prime
blit. The implicit native-heap share, an optimal image whose dma-buf names only
its MTLHeap, is made only for a renderer with a native heap and no modifiers,
the one configuration in which Zink imports it as that image. Either way the
guest allocates a shared image's device-local memory as an MTLHeap, as it does
for a compositor's own scanout buffer that Zink allocates through GBM. The
renderer publishes the Metal texture of every colour target created for
external memory, whatever its tiling, once it is bound to a native heap, so a
linear texture over that heap reaches the display with no copy. The guest
Venus driver's two conditions for `VK_ANDROID_native_buffer` and
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
  allocation it binds or unbinds, and the guest reports `reportAddressBinding`
  as the renderer answers it, so a device that enables the feature is backed by
  a host device that reports. What no report reaches is an application: Venus
  answers `VK_EXT_debug_utils` in the guest, and the protocol carries nothing
  from the renderer to a guest callback, so a report the host driver raises
  finds no messenger listening and stops there. Delivering one needs a
  host-to-guest path the protocol does not have: a renderer-side messenger, a
  queue of reports named by the guest object ids the renderer already maps, and
  a guest drain after each command that can bind or unbind. That is the one
  part of this extension the stack still owes, and it is what a client would
  notice; conformance does not, because it checks only that every reported bind
  has a matching unbind.

`VK_KHR_pipeline_binary` needs no host driver entrypoints: KosmicKrisp compiles
shader objects rather than monolithic pipelines, so Mesa's common runtime
serves all five commands from the shader binaries it already produces. Its
internal-cache properties are all false, because the driver keeps no pipeline
cache of its own. `MTLBinaryArchive` cannot stand in for a pipeline binary: it
is keyed by internal pipeline descriptors and serializes only as a whole
archive to a file URL, so it can neither produce one binary per pipeline nor
return a binary's bytes.

A binary's key is a hash of the bytes that binary carries rather than the
cache key of the shader they were compiled from. That cache key names what a
compile was asked for - every stage of the linked partition, the pipeline
state and the pipeline layout - and different requests regularly compile to
the same output, so keying by it gave byte-identical binaries different names
and left an application's own store holding each copy separately. The cost is
that a binary loaded back into a pipeline names its entry in the driver's
internal shader cache by those bytes, so a pipeline created later from the
same state without binaries compiles again instead of finding it.

`vkGetPipelineBinaryDataKHR` answers a buffer too small for the binary with
`VK_ERROR_NOT_ENOUGH_SPACE_KHR`, the size the caller needs, and no data, which
makes it the one command in the protocol whose in-out data size can come back
larger than it went in. A reply's blob is measured by the size the driver
reports, while the room for it was reserved from the size the guest offered,
so the renderer drops the blob from such a reply: the guest learns the size it
needs and reads nothing into a buffer that could not have held it. Every other
blob a reply carries takes its size by value or can only shrink it.

`vkGetPipelineKeyKHR` names the pipeline it should key through
`VkPipelineCreateInfoKHR::pNext`, and `vk.xml` gives that chain no member,
because each structure a caller chains there is a pipeline create info in its
own right rather than an extension of anything. The generator is told about
the chain instead: the three pipeline create infos the command can be asked
about, together with the structures that extend them, because a Venus chain
travels flat and a node's own chain is encoded by the codec of the chain it
belongs to. Without those siblings a dynamic-rendering or pipeline-binary
chain would be dropped along with the create info's own extensions. The
renderer still refuses a request that arrives with an empty chain, which the
specification gives a caller no way to make and which the host driver would
read without checking.

### A wide buffer load covers at most sixteen bytes

Apple's GPU backend compiler cannot be trusted with a shader that loads a
vector wider than sixteen bytes. A four-component 64-bit load - the only
32-byte access the driver has reason to emit - can end the Metal compiler
service with `unable to legalize instruction` on the `<4 x s64>` value the
load produces, not in every shader that has one, and `vkCreateGraphicsPipelines` answers `VK_ERROR_INVALID_SHADER_NV`
after Metal has retried the crash. This is a compiler limit rather than a
device one: it reproduces on an Apple7 Mac and on the Apple8 device alike, and
the matching 32-byte store compiles on both.

The driver emits such a load as two wide halves instead. Each half keeps the
alignment NIR proved, so the access costs two rather than the eight a packed
four-component 64-bit vector would, and nothing narrower changes.

### Apple8 (A15) device behaviour

The physical iOS device the Vulkan route has been exercised on is an iPad
mini 6: an A15, Metal family Apple8, 4 GiB, under iOS 27. It differs from the
Apple silicon Macs the same stack runs on -- the one these comparisons were
made against reports Metal family Apple7 -- in ways that change results rather
than only speed. Each item below either names the behaviour that answers it or
stands as a limitation; none of them has been observed on a Mac, and none has
yet been checked on another iOS device.

Only two of the Metal capability answers differ between that Mac and this
device, and the driver takes both from the device rather than assuming:
`supportsBCTextureCompression` and `supports32BitFloatFiltering` are false here
and true there. Every other capability the driver could key on -- 32-bit MSAA,
query texture LOD, pull-model interpolation, shader barycentrics, programmable
sample positions, raster order groups, read-write texture tier, argument buffer
tier, the supported texture sample counts and vertex amplification counts --
answers the same on both. The device's ceilings are of course smaller:
`maxBufferLength` is 2 GiB against 38.9 GiB, and the recommended working set
2.67 GiB against 51.8 GiB.

#### A GPU fault is reported as a timeout

An Apple8 GPU under iOS 27 reports a GPU fault as a timeout. The report carries
`MTLCommandBufferErrorTimeout` (or `MTL4CommandQueueErrorTimeout` on the queue
the driver uses) with Metal's placeholder end timestamp, while the per-encoder
execution status that only the classic `MTLCommandBuffer` error options expose
names the encoder `MTLCommandEncoderErrorStateFaulted`. Metal 4 has no
equivalent of that status, so the driver's own queue cannot tell the two apart.
A "timeout" on this device is therefore a label, not a diagnosis, and must not
be read as evidence that a shader ran too long. There is no command-buffer
duration limit anywhere near the affected work: renders of 84 ms and 178 ms
complete without error, while a submission that faults is reported 13-18 ms
after its commit call, far short of the work it was given.

The timestamps on a failed commit do not measure execution either. A fault
carries the placeholder end, which the driver prints as
`gpu_end_s=6148826548.220315` and describes as a submission the GPU started
and never reported finishing; a refusal by a dead queue carries an equal or
microseconds-apart start and end. Timeouts that host contention causes on
macOS are a different event: they carry a real interval and leave a working
queue behind.

#### A faulted command queue never works again

An `MTL4CommandQueue` that has killed one submission admits every later commit
and reports it failed within 0.1 ms, still labelled a timeout, for as long as
the queue object exists; a classic queue answers the same way with an
access-revoked code. Nothing in Metal resets a queue. Left alone, one fault
turned roughly the remaining ninety percent of a mustpass fraction into
failures, about 14,600 reports across tests that cost nothing.

The driver retires a queue a failure has killed and commits to a replacement
on the queue's own submit thread, once Metal has reported every submission the
retired queue was given, so no submission is split across the two and
single-queue ordering holds. A fault now costs the results of its own
submission and nothing else: the device is not lost, and the same fraction
carries one report per faulting case. A storm of timeout reports across cheap
tests means a dead queue is still in use, not that the tests are expensive.

#### A killed submission takes its signals and query results with it

A queue that kills a submission also discards the queue-level event signal
ordered behind it and the signals of the other work it kills, and it reports
only the submission that faulted. The shared event stops below its value, the
Vulkan fence or timeline semaphore never signals, and every waiter blocks
forever, in the host process and, through Venus, in the guest. An availability
write for a query is lost the same way.

The driver records every signal against the submission serial it sits behind
and completes what a failure destroyed: everything at or below a reported
failure, everything before a later completion once the queue has failed,
everything outstanding once Metal has reported as many submissions as were
made, and, for a queue that stops reporting altogether, whatever a waiter has
been owed for ten seconds. A query availability wait on a queue that has failed
is bounded rather than polled forever. Behind both, the renderer bounds a stuck
fence at fifteen seconds per stage, releasing queue ordering first and retiring
the fence without the renderer second, so a loss no layer explains cannot
wedge a guest for longer than that.

#### Fragment-stage store work faults the GPU

A render pass whose fragment shader performs enough `device` store work faults.
A native Metal program in the driver's process renders 8200 loop iterations per
fragment over 256x256 in 84 ms, and in 178 ms when each invocation stores to a
distinct address, without error. Removing the store alone takes the same shader
from 84 ms to 1 ms and it never faults, and spreading the stores over distinct
addresses moves the fault threshold from 12288 to 16384 iterations, so what the
fault tracks is the fragment stage's store work rather than elapsed time. The
same program runs 32768 iterations on that Mac without faulting.

Nothing in the driver removes it. The stores belong to the application, the
fragment invocation count is the render area, the render pass tile size
(`tileWidth`/`tileHeight`, whose default for a single 8-bit colour attachment
is 32x32, with 16x16 the only other size the device accepts) changes nothing on
the device, and splitting the render area with a scissor helps unreliably.
Fifteen of the 552 `dEQP-VK.spirv_assembly.type.vec4.i64` cases stand on it -
`clamp_frag` and fourteen of the sixteen
`bit_field_insert_offset{8,16,32,64}_count{8,16,32,64}_frag` - because dEQP has
every one of 65,536 fragment invocations write every element of the output
buffer on each of 5120 or 8200 loop trips. dEQP reports each as
`Upper left corner mismatch`, since a killed draw leaves the clear colour, never
as a timeout. Their `vert`, `tessc`, `tesse`, `geom` and `comp` variants run
the same loop a handful of times and pass, and which of the cases nearest the
threshold fault varies from run to run. The `vec4.u64` group loses the same
cases for the same reason, `clamp_frag` and twelve of its `bit_field_insert`
cases in a full mustpass run, and the threshold is near enough for a narrow
type to cross it as well: `vec4.i8.clamp_frag` failed once in that run.

Two driver changes lower the per-iteration work and so moved several sibling
cases under the threshold; they are mitigations, not a fix. Descriptor loads
are emitted in MSL's `constant` address space, where the compiler can hoist
them out of a loop that also stores, and buffer accesses use MSL's unpacked
vector types where NIR proves the alignment. The `constant` address space is a
small uniform-access path on this GPU, not a free read-only annotation:
streaming a 262 KiB input buffer through it made the same cases fault more
often. The rejected levers are recorded in
[`optimization-history.md`](optimization-history.md).

#### An out-of-bounds texture atomic faults the GPU

Metal carries `robustImageAccess2` semantics for a texture read and a texture
write by itself: a read outside the texture returns zero and a write outside it
is discarded. A texture atomic is not covered. An atomic whose coordinate, array
index or cube face lies outside the texture reaches memory the texture does not
own, and on this device that access never completes: the submission is killed and
reported as a timeout, which is a label rather than a diagnosis as above. An
out-of-range texture read or write in the same shader, in the same numbers, is
harmless, and so is the atomic itself on a Mac.

`dEQP-VK.robustness.robustness2` stands on this wherever a format-qualified
`r32i`/`r32ui` storage image takes an `imageAtomicAdd` at a deliberately out of
range coordinate from a render stage; those cases issue thousands of them per
fragment or vertex in a loop the shader does not unroll. A kill does not always
land on the submission whose verdict it decides, so the group reports fewer
failures than kills and which cases fail varies from run to run.

The driver rejects the coordinate before the atomic issues. KosmicKrisp runs
`nir_lower_robust_access` over image atomics alone, ahead of descriptor lowering,
so the comparison is made in the Vulkan coordinate space against the size the
image deref still names, before the cube and 1D rewrites fold a face or layer
into Metal's coordinate. Reads, writes and buffer accesses keep the robustness
they already had, so nothing else gains a bounds test. A twelve-case device rig
covering every layered and flat storage-image atomic in the group goes from seven
or eight kills a run to none, and `dEQP-VK.image.atomic_operations`, which
validates the in-bounds result of an atomic on every view type, is unchanged.

#### Render-stage 3D texture sampling faults the GPU

A draw whose vertex or fragment shader samples a 3D floating-point texture often
enough faults, the way fragment-stage store work does above. Every access
involved is ordinary and in range, so this is a work limit and not a robustness
problem.

Two `dEQP-VK.robustness.image_robustness` cases stand on it,
`push.notemplate.r32f.dontunroll.nonvolatile.sampled_image.no_fmt_qual.img.samples_1.3d.frag`
and its `vert` sibling. Their shader samples a 5x11x6 `R32_SFLOAT` `sampler3D`
nine times on each of the 2,101 trips of a loop it is told not to unroll, about
19,000 samples per invocation. The `comp` sibling issues the identical Metal
shader from a dispatch and never faults.

What it is not. Holding every one of those sample coordinates inside [0,1] in
the driver leaves the fault exactly where it was, and so do bounds-checking the
shader's deliberately out-of-range `texelFetch`, replacing its explicit gradient
with an explicit level, forcing every sampler to point sampling, and forcing
clamp to edge in place of the test's clamp-to-border sampler. Discarding the
samples removes the fault; discarding the fetches does not. Nothing out of
bounds is involved, and the driver has no coordinate to reject.

What it tracks is how much 3D sampling the render stage does. Keeping only the
first N samples of each trip and discarding the rest moves it: nine and eight
kill two submissions a run, six is marginal (two, none and one kill over three
runs), five and below never kill. The same shader over 21 trips is clean, as is
a `texture3d<int>` or `texture3d<uint>` at nine samples a trip and a
`texture2d<float>` at six. Serialising the samples into a dependency chain makes
it much worse -- ten kills a run, and then every 3D-sampling draw in the rig
faults -- so it is not a limit on requests in flight.

Nothing in the driver removes it. The neighbouring
`dEQP-VK.robustness.robustness2` case uses the same image and the same sampler
with a strictly larger shader -- three extra out-of-range-LOD `texelFetch`es per
trip around the same nine samples -- and stays under the threshold in every run;
that spacing is the margin a driver would have to buy back, and there is no
per-sample work left in the generated MSL to remove. Both cases pass on a Mac,
where the whole `image_robustness` and `robustness2` family is clean.

The limit is not confined to 3D textures. The `2d_array` siblings of the same
test,
`{bind,push}.notemplate.{r32f,rgba32f}.dontunroll.nonvolatile.sampled_image.no_fmt_qual.img.samples_1.2d_array.{frag,vert}`
and their pipeline-library variants, run the same loop over a layered image. In
a full mustpass run five of the twelve `rgba32f` render-stage cases of
`pipeline_robustness` fail, each with two submissions killed, and so do the
`bind` `r32f` `frag` and `vert` cases, while every `1d` and `2d` sibling passes.

#### A long run of small render passes faults the GPU

A process that has already pushed a few thousand small render passes through
the device starts having single submissions killed, reported as the timeout
above. `dEQP-VK.pipeline.*.render_to_image.core.{1d_array,2d_array}.huge.layers`
stands on it: each of those cases renders one layer at a time into a 256-layer
image, so it records 256 single-layer render passes of 44x23 (or 54x1), one
draw each, into a single command buffer, which reached Metal as 257
`MTLCommandBuffer`s in one queue submit while every render encoder had its own.
Sequential encoders now share one Metal command buffer, and the family has not
been rerun on the device since. Fraction 05
lost seven of those cases to six kills. A fourteen-case device rig covering the
family reproduces it at nought to seven kills a run, and the kill always lands
on the submission carrying the render passes, never on anything else.

What it is not. Not the size of one Metal kick: committing the same command
buffers in bounded batches changes nothing, and at one command buffer per
commit a submission carrying a single 44x23 render pass is still killed. Not
memory: the device's residency set holds ten or eleven allocations totalling
1-5 MiB and Metal reports 22-29 MiB allocated, against a 2.8 GiB working-set
budget. Not API misuse: with Metal API validation enabled the run reports no
validation error and the kills continue. Not the pipeline construction type:
`monolithic`, `pipeline_library` and `fast_linked_library` of the same case
produce byte-identical work, 256 passes and 257 command buffers each, and which
of them fails changes from run to run. Not fragment or tile work:
`cube_array.huge.layers` does the most of both, 252 passes of 35x35, and has
never failed in any run. Not the per-device geometry heap: raising it to
256 MiB leaves the kills where they were.

What it tracks is how much the process has already done. The four failing
`2d_array` cases, run as the entire workload, pass in four consecutive runs
with no kill at all; the same four fail whenever the five neighbouring cases
have run before them in the same process, and the first run after the device
has been idle is usually clean as well. Which submission the kill lands on is
random -- the 65th and the 190th of 257 in one arm. Nothing in the driver has
been found that removes it, and the whole family is clean on an Apple silicon
Mac.

#### Memory budget

iOS ends the app at a physical footprint of about 3007 MB on this device. With
a 1536 MiB guest the host side plateaus near 2673 MB, and the
`render_to_image` cases with huge attachments add a transient of about 650 MB
of `IOAccelerator` memory for a fraction of a second, so guest RAM cannot be
raised to make room for a workload. Two consequences follow.

The per-device geometry heap is 16 MiB rather than a fixed 128 MiB, because a
Venus host pays it once per guest `VkDevice`. The heaviest command buffer in the
CTS tessellation, geometry, draw and rasterization groups asks it for
1.53 MiB; `MESA_KK_HEAP_SIZE_MIB` raises it without a rebuild. An undersized
heap produces wrong results rather than a diagnosable failure, because the
overflow report is a shader-side `printf` and `abort()` that reaches neither
Metal nor the host log, so a smaller size is validated only by a conformance
run.

An uninterrupted released mustpass fraction runs about 200,000 cases in one
`deqp-vk` process, and a 1536 MiB guest cannot hold every group in that traversal.
The cost is in two parts, both measured on
macOS at the same guest profile. A baseline that grows: the process holds 134 MiB
before its first case, from the released case hierarchy the enumeration builds,
reaches about 560 MiB within the first 15,000 cases, and about 1.1 GB by 88
percent of the fraction. And a per-group transient on top of it: fraction 00's
380 `dEQP-VK.reconvergence` cases all pass in a fresh process, but their peak
resident size is 665 MiB, because those tests simulate each subgroup's control
flow on the CPU. The guest reaper takes `deqp-vk` where the two meet. Raising
guest RAM is not available, and a 1024 MiB guest is killed earlier still.

Two further consequences. The already-transported `results.qpa` stays in the
guest's `/run` tmpfs, which is guest RAM: 271 MiB, 18 percent of the guest, by
the time the reaper fires. And a killed `deqp-vk` takes the whole service cgroup
with it, so the guest prints no further progress line and sends no artifact
frames: the run goes silent rather than reporting a stall, and its verdicts
survive only as the result frames already on the console. The device lane ends a
run within three minutes of the reaper's own line rather than waiting out its
stall guard, and writes whatever the console carried.

A baked device campaign therefore leaves the groups it cannot hold out of the
fraction, which is why the campaign names carry a generation suffix. The guest
drops them from the list it enumerated and transports exactly which cases and
under which reason, so an excluded case is a case not run, never a pass and
never a failure, and the same selection can be run on a device with more memory.
Each baked profile also fixes guest memory and any planned CTS process
boundary. The contract is in [`testing.md`](testing.md); the reasoned exclusion
list is `DEVICE_EXCLUSIONS` in `tests/integration/gpu/run_vk_cts.py`.

### Experimental host memory

Native graphics allocations are separate from the configured guest RAM limit.
KosmicKrisp releases obsolete pipeline resources with their last Vulkan owner
and a submission's recording storage after GPU completion; see the
[dependency ownership contracts](../dependencies/README.md).

Budget campaign concurrency from host physical footprint and the
`IOAccelerator (graphics)` row of `vmmap -summary`, rather than process RSS,
which omits swapped-out allocations.

## Metal shader cache

Metal keeps one shader cache per process identity: a `libraries` data and
index file pair for the front end's output from MSL source, and a `functions`
pair for the GPU binaries pipelines link. KosmicKrisp's `MTL4Compiler`, the
display's pipelines and the terminal's all read and extend the same files, and
nothing in them says which entries came from a guest shader. The process maps
the data files, but the first compile reads the whole index into memory, so
footprint grows with the index whatever the data files hold: about 3.5 MiB per
MB of index on the iPad mini 6 and 5 MiB per MB on an M1 Ultra. A pipeline
built from a precompiled metallib skips only the `libraries` index. Metal
stops each data file near 1.1 GB and evicts from there, so a full cache holds
about 2.2 GB behind a 22 to 24 MB index, which costs 80 to 115 MiB; GPU
conformance campaigns fill it. Metal has no call that clears, bounds, splits
or bypasses the cache: `MTL4CompilerDescriptor` carries a label and a pipeline
data-set serializer, and binary archives add lookups without taking a
pipeline off the cache.

Kernarium therefore sizes its cache as the application starts, before any
view or runtime exists and so before anything can read it, and removes the
whole cache when its `.list` index files together hold more than
`MetalShaderCache.indexLimit`, 1 MB. It logs what it found either way. Real
workloads stay far below the limit, because their keys are stable: a repeated
boot, benchmark or page reuses its entries and adds none. A desktop boot,
glmark2 over Virgl and over guest Zink on Venus, WebGL Aquarium in Chromium
and a Steam Big Picture boot together leave about 0.12 MB of index and 30 MB of
data. Each entry's index record has the same size on the iPad and the Mac, so
the same workload leaves the same index on both. At the limit the index costs
about 3.5 MiB on the iPad. Removing a full cache takes under 0.1 s there;
the next boot then spends about 2 s compiling the host's own pipelines before
the guest's first frame, and each workload compiles its shaders once more.

The cache is `<bundle identifier>/com.apple.metal` in the Darwin user cache
directory: the container's `Library/Caches` on a device, where
`confstr(_CS_DARWIN_USER_CACHE_DIR)` fails, and the per-application directory
that call names in Designed for iPad mode. That layout is Metal's own and
undocumented; if it moves, the application finds no cache, logs so, and
removes nothing. The system log's `fopen failed for data file` from
`libCoreFSCache` at launch is Metal opening an empty cache, not a failure. The
application also sets `MESA_SHADER_CACHE_DISABLE`: Mesa's own disk cache would
live below `$HOME/.cache`, which a device container does not let it create, so
Metal's cache is the only one that outlives a launch.

The macOS command-line emulator has no bundle identifier. It shares
`$(getconf DARWIN_USER_CACHE_DIR)/com.apple.metal` with every unsandboxed
process of the user, one numbered file pair per concurrent process, and leaves
that cache alone.

## Validation

Use `check-gpu` for native GPU ownership and VNC checks and
`check-gpu-asan-ubsan` for the sanitized path. Guest conformance, WebGL,
Aquarium, and physical-device campaigns are opt-in; see
[`testing.md`](testing.md).

Historical backend and performance experiments are summarized in
[`optimization-history.md`](optimization-history.md).
