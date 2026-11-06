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

Destroying a GL context frees the pipelines, shader variants, pipeline cache
and library references of the Zink programs it built, and the programs
themselves when they hold only a vertex and a fragment shader, although the
screen's shared shaders outlive it, so closing a guest application returns the
Metal pipelines and generated MSL it caused instead of keeping them for the
life of the process. A set of shaders whose pipeline libraries no program holds
and no lookup can reach has its libraries freed at once, not when the last
shader of the set dies. A set's default library is made once: a context that
links a set another context already linked reuses the library in the screen's
cache instead of making a second, which would replace it and outlive it.

macOS and the iOS device both build that stack from the same sources; the
prefixes differ only in their platform contract. The iOS Simulator cannot run
it, because KosmicKrisp requires Metal 4 and the Simulator exposes no Metal 4
device; it has the CPU-rendering variant of
[its own section](#ios-simulator-software-renderer). The unmodified device slice
also runs on an Apple silicon Mac in Designed for iPad mode, where Metal
reports the host GPU and the full Metal 4 family; the physical-device lanes are
in [`testing.md`](testing.md). The device runtimes use the Vulkan-experiment GPU
prefix and the accepted kernel descriptors. Desktop GL 4.6 needs
`GL_ARB_buffer_storage`, whose coherent buffers reach the guest through the
vhost-user shared-memory aperture of the shared kernel series.

The GPU transport thread owns renderer creation, context use, reset, and
teardown. Renderer cleanup frees its external EGL display wrapper even when
GBM is disabled; the host retains ownership of the shared display itself.
AppKit and UIKit share the Metal presentation layer. VNC is a macOS
consumer of the same renderer and is intended for diagnostics and remote
display, not as a second graphics backend. With no connected VNC client, a
native Metal scanout's flushes retire without any GPU-to-CPU readback; a
scanout only the CPU path can read is still read back from the renderer on
each flush. A client that connects after such an interval dropped frames
receives its first framebuffer update only once a complete scanout newer than
its connection is published. The display reads the newest native Metal
scanout back at once, and a CPU-path scanout shown scaled as well, since it is
kept in a host texture of the whole mode; a CPU-path scanout shown pixel for
pixel refreshes in full on the guest's next flush. The client waits at most
2 s for that flush, so a static one then arrives as last published.
The VNC display converts native Metal scanouts with a damage-bounded
asynchronous Metal blit into one shared staging buffer, keeps at most one
newer frame pending behind the readback in flight, and publishes to
LibVNCServer only after GPU completion, so neither readback, scaling, nor
encoding blocks the guest's flush timeline. A readback requested before the
framebuffer was cleared, re-placed for a new mode, or written by a CPU-path
copy is discarded when it completes, so it never repaints newer contents or
counts as a complete frame, and the next readback reads its whole scanout.

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
into pages of its own before each read. The guest names the layout, so every
import is bounded by what backs it: each row the image spans, at its stride
and offset, has to lie inside the segment or the heap, the size is passed to
the driver that aliases it, and an optimal texture has to have the guest
format's texel size. An import that does not fit is refused.

The native presenter can also mirror the guest's display to the application
(`frame_tap.hpp`) for a view it shows while its own window cannot, such as the
Picture in Picture window that keeps an iOS guest running in the background.
While a sink is set, a thread of its own renders the latest content, with the
user's rotation, into pooled IOSurface BGRA buffers no wider than the sink asked
for, at most the requested rate, and again about once a second when the display
is static. It reads the same texture or CPU framebuffer the window shows, so it
needs no drawable, and it renders nothing while no sink is set. Clearing the
sink returns only once no delivery is running, so the receiver may be released
at once.

`--gpu-display=offscreen` is the macOS mode for memory measurements. An iPad
allocates presentation memory that a VNC run never does -- the view's three
drawables, the presentation's textures and, in Picture in Picture, the frame
mirror's pool -- and a native window on a Mac opens on a shared screen. The
offscreen display drives the same presentation into a `CAMetalLayer` that
belongs to no window (BGRA8, `framebufferOnly`, three drawables) and presents
each guest frame as it arrives, on a serial queue of its own, because no display
link paces it; a frame that arrives while one is drawing replaces the one
waiting, as in a window. It clears and presents each of the three drawables once
at start, because a view that presents has rendered into all of them within a
few frames and a layer asked for a drawable only when the guest draws would show
two at idle. `--gpu-offscreen-size=WIDTHxHEIGHT` sets the drawables' pixel size
(default: the guest mode; a run states the size of the view it imitates), and
`--gpu-offscreen-mirror=on|off|WIDTH@FPS` keeps the frame mirror running into a
receiver that holds the last two frames, as Picture in Picture's display layer
does, so the mirror's pool is three buffers (`on` is 800 pixels at 15 frames a
second, off by default). Nothing reaches the guest from the host: there
is no input, no host resize and no VNC server, and the guest keeps its own mode.
Like the VNC display, it does not make the process an AppKit application. A
layer outside any window keeps handing out its drawables without the one-second
wait for a free one, which `gpu-native-metal-presentation` checks.

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
transfers, as Android's gralloc mapper does to lock a buffer for reading (a
buffer with padded rows by a command buffer that holds one transfer), keeps
working after one refusal.

A texture a guest context created and wrapped in a blob, as Android's gralloc
does for every GPU-only buffer, has no guest pages. The guest reaches its pixels
through a staging buffer: a copy transfer moves them between the texture and
the staging buffer's guest storage. The renderer requires that storage of the
staging buffer, whichever way the pixels move, and asks nothing of the texture.

A texture the renderer creates for a guest reads as zeros until the guest
writes it, as the zeroed pages the guest allocated for it would. The host
driver hands out the memory of a released resource again with its pixels still
in it, so without a clear a new texture shows what an earlier one held, which
may have belonged to another guest process. The renderer clears every level of
a new texture at creation. A texture imported from an image keeps the content it
was imported with, and a texture in a compressed format keeps what the driver
allocated, because a clear cannot write one. The clear is queued on the GPU and
adds no CPU time to the creating thread; it costs GPU time per level, plus the
bandwidth to fill the texture, before the first frame that uses it.

Pointer semantics are selected by the product launcher. Android native and VNC
input is exposed as one direct touch contact with absolute coordinates; hover,
auxiliary buttons, and wheel events are suppressed, and coordinates are sent
only for touch down, drag, and release. Non-Android guests retain absolute
pointer hover, primary and auxiliary mouse buttons, and wheel input.

## iOS Simulator software renderer

The Simulator has no Metal 4 device for KosmicKrisp, so Kernarium and Andarium
render a guest there on the CPU. That is a product for UI tests, not a faster or
second graphics backend, and no other product can carry it:

```text
guest Mesa -> Virtio-GPU/Virgl -> vhost-user -> virglrenderer
    -> Mesa llvmpipe over surfaceless EGL -> readback -> UIKit
```

- Its `apple-gpu-vulkan-experiment` prefix (`ios-simulator-arm64`, contract
  `ios-simulator-gpu-software-arm64.tsv`) holds libepoxy, virglrenderer built
  without Venus, and `MesaEGL.framework` and `MesaGallium.framework` with
  Mesa's llvmpipe and softpipe Gallium drivers. It has no KosmicKrisp, Vulkan
  header or Venus protocol, so its receipt names no protocol.
  `dependencies/build-apple-gpu.sh --platform ios-simulator` builds it, and the
  drivers are named once, in that script's `ios-simulator` branch, which is also
  the contract's `gl_provider`.
- llvmpipe JITs through LLVM. `dependencies/build-apple-gpu-llvm.sh` builds
  LLVM's AArch64 libraries for the Simulator from the source archive that
  `dependencies/llvm/archives.tsv` pins (prepared with
  `dependencies/prepare.sh --archive-manifest`), and Mesa links them statically
  into `MesaGallium`, so the prefix ships no LLVM file and records the archive
  manifest as provenance. softpipe, an interpreter that needs no JIT, stays as
  the `GALLIUM_DRIVER=softpipe` fallback.
- The JIT is Mesa's own: gallivm maps its code with `mmap`, makes it executable
  with `mprotect` and publishes it with `sys_icache_invalidate`, and neither
  uses `MAP_JIT` nor goes through `darwin/cpu/jit_memory`. It is the one
  exception to the [executable-memory rule](architecture.md), granted for this
  test-only product.
- The host GL provider selects the software device (`LIBGL_ALWAYS_SOFTWARE`),
  which Mesa answers with llvmpipe, and renders a desktop OpenGL 4.6 core
  context as the other products do. The guest sees a Virgl GPU with OpenGL ES
  3.2 and no Vulkan: the device offers no Venus capset, refuses a Venus
  context, and runs the renderer with no Venus flag and no device-memory pool,
  so SurfaceFlinger falls back to GL.
- Every flush is a readback; there is no native Metal texture to look up, and
  the log says `renderer=software`. The runtime plans no GPU budget, because the
  budget only sizes the Venus pool, and says so at launch.
- The rasterizers are a build-time property of the product:
  `UML_GPU_SOFTWARE_RENDERER` derives from the `ios-simulator` platform alone
  (`dependencies/cmake/AppleGpuDependencies.cmake`) and excludes the Venus and
  native-lookup code from every other build as a preprocessor condition on a
  constant. The architecture audit rejects it, and a link of a software prefix,
  outside an `ios-simulator` tree; the prefix verifier requires every driver of
  the contract's `gl_provider` in the Simulator's `MesaGallium` and forbids a
  software screen and LLVM in the macOS and iOS device prefixes; and
  `Kernarium/scripts/package-ios-frameworks.sh` checks every device slice for
  either and for the provider's marker.
- llvmpipe rasterizes on one thread per host core, so a scroll keeps about three
  cores busy; it presents about 20 frames per second and a cold app launch
  draws within about 7 s. A UI test allows about 20 s for a cold launch.
  softpipe, on one thread, is an order of magnitude slower.

## Guest capability boundary

On macOS the guest reports desktop OpenGL 4.6 core and GLSL 4.60 through
Virgl, because the host renders the Virgl command stream on a desktop OpenGL
4.6 core context. OpenGL 4.4 and above additionally require
`GL_ARB_buffer_storage`, which the guest exposes only over coherent
host-visible blob resources: the virtio-GPU device publishes its host-visible
aperture to Virgl contexts as well as Venus ones, and vrend backs those
coherent buffers with shared host pages pinned into GL. The device always
publishes the aperture, so a macOS Virgl guest reaches 4.6.

That version is the profile the guest reports, not a conformance result. Guest
Zink over Venus is the qualified desktop OpenGL path on macOS; the Virgl path's
own conformance standing is whatever its recorded campaigns show.

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

A stencil buffer needs no depth plane beside it. vrend offers
`VIRGL_FORMAT_S8_UINT`, one byte of stencil per texel, as a depth-stencil target
that attaches at the stencil attachment point; the format probe and every draw
use that one, so an image the probe found complete is complete when a guest
renders to it. A guest that wants stencil and no depth, as Android's hardware
renderer does for every window, then holds one byte per texel where it held a
depth-stencil image of four or eight, and every guest's `GL_STENCIL_INDEX8`
renderbuffer and texture lands on it too. An image with one plane detaches the
other plane from the framebuffer it joins: a stencil-only image that follows a
depth-stencil one must not inherit that image's depth plane, a pair Zink cannot
draw to, so the switch from a window's depth-stencil buffer to a stencil-only
renderbuffer keeps drawing.

The scanout mask is held to the same standard. Only the 8-bit RGB layouts the
display presents carry a scanout bit, and no YUV format does. The guest's
minigbm keeps the scanout use on exactly the combinations the mask names, and
it refuses an NV12 combination that has both scanout and its emulation over R8
buffers, so a scanout bit on NV12 costs the guest every video encoder, video
decoder and camera use of that format. P010 carries no scanout bit either,
because the host does not sample it: minigbm emulates it over an R8 resource for
CPU access, as the input of a video encoder and the output of a video decoder,
and keeps scanout off it.

Without a host allocator the renderer cannot create any multi-planar resource,
so a YUV buffer exists on the host only as the bytes of an R8 image, whatever
the buffer is used for. minigbm decides that by what the host can create rather
than by the capabilities a buffer's use flags name, which are none for a buffer
only the CPU, a codec or a camera touches: the renderer refuses a native NV12
create as an invalid parameter and every later command on that resource as an
invalid resource id.

The renderer samples such an image as the planes it holds, when the guest says
what the image is. A resource created with `VIRGL_RESOURCE_FLAG_YUV_BIPLANAR`
(luma rows, then interleaved chroma rows) or `VIRGL_RESOURCE_FLAG_YUV_TRIPLANAR`
(luma rows, then the two half-stride chroma planes) and the picture's width in
pixels in the high half of the flags is checked to be one picture, and keeps a
texture for each plane: R8 for luma, RG8 for interleaved chroma, and R8 for each
planar chroma plane. A sampler view of the image sends the plane it selects where
a view carries its layers, plane 0 as the ordinary layer range and a later plane
as its number, and reads that plane's texture. A triplanar image makes the
textures when the first view is, from the image as it then is, and every write
the guest transfers into the image redraws the plane rows its bytes feed in the
blitter's own context; an image that only a codec or the CPU touches never gets
them. Writes the GPU makes into such an image are not followed, because nothing
allocates it as a render target.

A biplanar image (NV12 and NV21, which is what the hardware decoders produce)
keeps its pixels only in its plane textures, which exist from its creation: its
R8 texture is 1x1 and only names it. A transfer of any box writes or reads the
plane textures that the box's luma rows and chroma rows fall in, and nothing is
drawn from an image, so a decoded frame costs its planes and no R8 image, plane
draw or upload staging beside them. A chroma box that cuts an interleaved pair
in two, or whose guest rows are an odd number of bytes apart, goes through the
whole pairs it touches, so the half the box does not carry is kept. The bytes
of a row past the picture's width, or past its last chroma pair, are padding
that no plane keeps: a write drops them and a read leaves the guest's bytes
there alone. A use of the image other than a transfer (a surface, a copy
region, a blit, a shader image, a clear, a view of a plane it does not have, or
a presenter) first gives it the R8 storage of a plain image, read back from the
planes, with a warning, and the image is a plain one from then on; playback and
the CTS reach none of them. An image imported from an EGL image keeps its
storage, and triplanar images keep the path above. The capability
`VIRGL_CAP_V2_PACKED_YUV` says the renderer knows the flags. minigbm sends them
for the 8-bit YUV buffers it emulates (NV12, NV21 and YV12), only when the
capability is present, so an older renderer is never sent a flag it refuses and
an older minigbm sends none, which leaves the image the single R8 texture it
always was. A P010 buffer is not tagged: its two-byte samples do not fit the R8
plane textures, and nothing samples it. A guest that imports each plane of a
picture as its own resource sends the plane number as well, and the renderer
reads it as the whole texture of that resource unless the resource has the
plane's image, so a planar picture whose planes are separate buffers samples
correctly too.

A single-plane buffer's rows are padded to the first whole number of pixels
that makes them a multiple of 16 bytes (a 642-pixel RGBA row is 2576 bytes and
the buffer's stride 644 pixels), because the host imports a buffer into Vulkan
at the pitch the guest reports and Metal reads a texture over a buffer only at
such a pitch. The buffer and its host texture keep their width, which Android
allows to be smaller than the stride and which the guest samples the texture
by. A swapchain image, a GPU blob, is laid out by the same rule. The renderer
takes the guest rows of a transfer at the row length of the texture's width
unless the transfer names a stride, and the kernel's transfer ioctls carry none
for a classic resource, so a buffer with padded rows is locked and unlocked by a
`VIRGL_CCMD_TRANSFER3D` in a command buffer, which names the pitch as its stride
and the offset of the box's first byte. That is one command, and one readback or
upload, for a whole lock or a rectangle of it in either direction, however tall
the buffer is: a transfer for each row cost one each, and half a second to lock
a 342x604 buffer.

The emulated layouts are the layouts Android defines, so that the media
framework, the Image API and an importer that reads the buffer's own plane
offsets and strides agree on where a plane is. An NV12 image is the luma rows
followed by the interleaved chroma rows, and a row is padded to a multiple of 16
bytes, which also holds the one extra chroma byte of an odd width. A P010 image
is the same with two-byte samples, its rows padded the same way and the chroma
box of a partial lock clamped at the padded width. A YV12 image is Android's:
the V plane after the Y plane, the U plane after the V plane, and a chroma stride of half
the luma stride, which minigbm makes a multiple of 32 so that the half is a
multiple of 16. A chroma row is therefore half as wide as the R8 image and two
of them fill one image row, so a 1920x1080 frame is a 1920x1620 image. Locking
part of a buffer flushes and invalidates exactly the bytes of that rectangle,
as one box for the luma plane and as up to two boxes for each planar chroma
plane, and gives every box the offset of its first byte, because the renderer
reads a box from the guest copy at the offset of the transfer.

Applications that use stateful V4L2 decoding can instead use the independent
[Virtio video service](../darwin/device/video/README.md). It exposes the same
host codec profile set through the shared Linux and Android driver and copies
decoded planes into guest memory. Its protocol and buffer ownership do not
depend on Virgl or VA-API.

Which codec API a guest application can reach is a property of that
application's own build, not of this device. Chromium on Linux enumerates
`/dev/video0` through `/dev/video255` itself where it has a V4L2 decoder at all
(the `/dev/video-dec*` names are a ChromeOS convention), and its only hardware
encode on Linux is VA-API. Valve's x86-64 CEF carries no stateful V4L2 decoder,
only VA-API. The guest therefore ships a VA-API driver over the same V4L2 nodes,
[`uml_drv_video.so`](../tools/video/va-v4l2/README.md): the Debian Chromium
images select it through `/etc/chromium.d/kernarium-video`, and it gives
FFmpeg, GStreamer and any other VA-API client H.264 and HEVC encode on both
guest architectures. Chromium's own H.264 encode is on there: the gbm module
below allocates the NV12 frames it encodes, and a guest kernel that exports
virtio-gpu buffers read-write and transfers them around a CPU access makes the
frames the GPU painted readable; the driver README owns the contract.
An application that speaks V4L2 itself, such as FFmpeg's `*_v4l2m2m` codecs or
GStreamer's `v4l2` elements, reaches the service without it.

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

One guest file is the single exception: the gbm backend module
`virtio_gpu_gbm.so`, which the Debian desktop images ship beside the video
driver ([va-v4l2](../tools/video/va-v4l2/README.md#the-gbm-module-planar-buffers-for-debians-libgbm)).
Debian's libgbm cannot allocate NV12, P010 or any other planar format, so a
video pipeline has nowhere to put its frames (Chromium's frame for a hardware
encoder is `gbm_bo_create(NV12)`), and neither the kernel nor the host can
change that. The module is not a replacement or a preload: Debian's unmodified
libgbm loads it through its documented backend interface, by the DRM driver
name, and it makes the device Mesa's own backend makes, adding only planar
buffers, one resource per plane. Mesa, its EGL platform and every call on a
colour buffer stay as shipped, and deleting the file, or `GBM_BACKEND=dri` for
one process, restores Mesa alone.

Two framebuffers for which `KHR-GL46.direct_state_access.framebuffers_check_status`
prefers an incomplete status are reported complete, as the specification
requires. One mixes a renderbuffer created with one sample and one created
with two. `RenderbufferStorageMultisample` may grant more samples than were
asked for, and the frontend promotes a one-sample request to two, because
Gallium represents no one-sample multisample surface: one sample there is a
single-sampled surface, on which Zink deletes a shader's `gl_SampleMask` writes
and the `sample_variables` `multisample_rbo_1` cases lose the per-sample
fragment operations they verify. Both renderbuffers therefore carry the same
`RENDERBUFFER_SAMPLES`, and section 9.4.2 requires the framebuffer that mixes
them to be reported complete. The other attaches one slice of a
three-dimensional texture beside a two-dimensional texture, which leaves no
attachment layered, and the rule requiring a common texture target applies
only when one is.

Hardware video decode is not advertised over Virgl; the renderer is built
without it.

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

The GPU advertises display geometry and density through EDID; Kernarium's
point and Retina geometry is in
[`../Kernarium/README.md`](../Kernarium/README.md#devices-and-display).

## Dynamic native geometry

Native AppKit and UIKit displays publish host geometry only after it settles.
AppKit retains the active guest mode throughout live window resize and
publishes the final logical-point content size when live resize ends. UIKit
retains it through an interactive view-controller transition and publishes the
final point or Retina-pixel size from transition completion. An iPadOS window
resize is one gesture through many sizes: UIKit retains the mode until the
window scene reports the gesture over and the last transition has completed,
so the guest sees only the size the gesture settles at. Non-interactive layout
changes are coalesced until their geometry is stable.

Host-to-guest automatic resizing is enabled by default. Kernarium stores the
choice in its iOS app settings; the macOS executable selects it with
`--gpu-auto-resize=on|off`. When enabled, a settled host change updates the
Virtio-GPU advertised mode and raises one display event. Duplicate callbacks
for the same settled size do not raise another event, but a host that settles
back at the active guest size before the guest has applied the mode advertised
since is returning to it, not following it, so the size is advertised again.
Presentation storage remains sized to the active guest scanout until the guest
applies its eventual mode with `SET_SCANOUT` or `SET_SCANOUT_BLOB`.

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

Apart from the VNC display's fixed framebuffer, only the CPU path keeps host
copies of a guest mode's pixels, and only once it uses them. There are three,
each the whole scanout rectangle at four bytes per pixel: the native display's
CPU framebuffer, the shared Metal texture that framebuffer is uploaded into,
and the Virtio-GPU device's readback staging for a Virgl scanout. An applied mode
and a cleared display start without any of them. The first CPU write allocates
the framebuffer, zeroed, and the first draw that shows it creates the texture;
the first flush that reads a Virgl scanout back allocates the staging. Each is
then reused for the CPU frames that follow. A mode change releases the
framebuffer and its texture, and a clear releases both at once, including when
a native frame supersedes the clear before it is drawn. In-flight Metal work
keeps the resources it uses until completion. The staging is released when the
scanout changes size, becomes a 2D or native Metal scanout, or is disabled.
A mode shown only through Metal textures therefore holds none of them, and neither does the frame
that follows each mode change, which presents the view's clear colour alone. A
direct frame leaves any copies an earlier CPU frame made, because a later
partial CPU update writes into them. Failing to allocate the framebuffer or the
staging is reported to the flush that needed it, as an out-of-memory response,
rather than to the mode set. The view's drawables, three on iOS, stay sized to
the view. The device memory pool counts the process's Metal and graphics
memory, so growth there, including the guest's own scanout buffers and the CPU
path's texture, uses the GPU budget, while the framebuffer and the staging add
to the footprint.

CPU pixel-format conversion writes directly into the locked framebuffer with
the same channel permutation for vector and scalar pixels. It needs no
full-image temporary allocation or second copy; a source overlapping the
framebuffer, in any format, is first copied to temporary storage so that every
pixel is read as it was before the update. The lock keeps a complete update
atomic with drawing and mode changes.

Kernarium's EDID lists only the host's preferred mode, so a guest reaches a
larger one, such as 4096x2160, by setting a user-defined mode. The renderer can
hold a finished client's scanout buffers for a minute or more, until later
rendering releases them; repeated mode cycles do not accumulate them.

Rendering and input use the same centered aspect-fit rectangle. AppKit, VNC,
and Kernarium map that rectangle into the fixed absolute range of 0 through
32767 on both axes. The range belongs to the device, not to the display or the
launch size, so a view resized to any size keeps the full resolution and the
guest never has to re-probe it. VNC rounds the rectangle to whole
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

Every GPU build advertises guest Vulkan except the iOS Simulator's [software
renderer](#ios-simulator-software-renderer), which has no Vulkan driver. Venus
runs over the packaged KosmicKrisp on Metal, and the guest reaches the host ring
through the kernel's vhost-user shared-memory aperture. The device of every
other build always offers the Venus capset, blob resources, and that aperture;
no runtime switch withholds them.

Venus exposes KosmicKrisp's real version, extensions, features, properties,
formats, queues, limits, and memory model, with no injected extension or forced
version, and only what runs reliably through the whole guest-to-Metal path is
advertised. No advertised capability depends on CPU shader interpretation, CPU
rasterization, resource-sized steady-state copies, or readback-driven command
decisions. Functionality Metal or KosmicKrisp cannot run reliably is reported
unsupported instead, as `shaderFloat16` is below.

That route makes no conformance claim. Support is asserted only by the
receipt-bound guest CTS campaigns in
[`testing.md`](testing.md#graphics-conformance-campaigns). The last complete
Vulkan CTS 1.4.6.2 campaign ran all 16 fractions of the mandatory list with no
disallowed result; its receipt records the Mesa it ran.

### Guest memory boundary

A guest application drives this renderer, so every layer that writes through an
address derived from guest state bounds that write itself. No guest can make
the host write outside the memory it gave the host, whatever the GPU does.

Only a resource's attached backing reaches guest RAM: a blob resource carries
no guest pages, and the device translates a backing entry only as a whole range
inside one negotiated memory region. A control-queue reply is never written
past the writable descriptors the guest supplied. A command whose reply would
not fit there does not run: it is answered with
`VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER`, or, when not even a response header
fits, completed with nothing written. A Venus reply stream ends where the
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

### Device memory budget

Device memory is charged to the application like guest RAM, so the host gives
the GPU a fixed budget, B, computed once at launch from the process's memory
limit and the guest's memory (the formula, its launch parameters and the
warning below the floor are in [memory
architecture](architecture.md#memory-and-executable-code)). The budget is one
pool (`device/gpu/device_memory_pool.*`) for everything the host GPU holds for
the process. Its usage is the larger of the process's Metal allocation
(`MTLDevice.currentAllocatedSize`, which also counts presentation, host GL
memory and the host driver's own memory) and its graphics footprint (the
graphics ledger of `TASK_VM_INFO`, which also charges the command storage the
driver has written), plus the reservations of allocations under way. The
renderer shrinks every advertised heap to B and bounds every heap budget it
reports by the heap's usage plus what the pool has left, so a guest sizes its
allocations from B, less what the host already holds, instead of from the
host's memory. The guest driver passes budgets on only under
`VN_DEBUG=mem_budget`.

KosmicKrisp is the one place every allocation passes. Whoever owns the host
(`uml-darwin`, the Kernarium runtime) hands the renderer the pool, and the
renderer states it to the driver for the whole process
(`kk_set_device_memory_pool`, exported by the driver library beside the
per-device structure of `kk_device_memory_pool.h`, in this project's Mesa
series). Every device the process creates then charges it, the Venus devices of
guests and the device Zink creates for host GL alike, without either chaining
anything: a guest cannot chain the structure itself, because the protocol
decoder rejects a chain member it does not know. A pooled device reserves a
heap's size, in whole host pages, before Metal makes it, and releases the
reservation once Metal counts the heap or has refused it; one lock decides
every reservation, so allocations that each fit alone never together take the
process past B, whichever threads make them. A reservation that does not fit
first frees what the host GL driver caches, and is decided once more: the pool
takes one reclaim hook, which the host GL provider fills with
`eglReleaseCachedMemoryMESA` (`EGL_MESA_context_trim` of this project's Mesa
series, callable from any thread, with no display lock because the refused
allocation may hold one) and which releases every Zink screen's buffer cache and
slabs. Zink frees only for a refusal of its own, so without the hook a Venus
guest's allocation could be refused by memory that Zink's idle cache held. The
pool counts the reservations that fit only after the release and those still
refused although it freed something; the first refusal's stderr line says
whether a release came before it.

What the driver refuses and what it only counts:

- `vkAllocateMemory` of any memory type is refused with
  `VK_ERROR_OUT_OF_DEVICE_MEMORY` when the pool cannot hold it, and nothing is
  allocated. That covers every buffer and texture of the OpenGL path, which Zink
  places in memory it allocates this way, its pb_cache slabs included: Zink
  answers the failure as it answers any, by dropping its cached buffers and
  trying once more before the resource fails.
- The geometry heap charges itself through the same pool and follows the
  pool-aware heap rules (see [memory budget](#memory-budget)), except that a
  device's first heap, at its 1 MiB floor, is made when the pool refuses it.
- Host-visible memory the renderer makes for the guest and the driver imports
  is not a driver allocation, so the renderer reserves its size before it makes
  the pages and releases the reservation once the driver has made its Metal
  buffer over them.
- Memory a device imports names memory that exists already and is not charged.
- The device's other storage (command pools, descriptor pools, query pools,
  events and upload buffers), presentation, the VNC surface and decoder surfaces
  are counted once Metal does and never refused: what needs them cannot fail
  the way an allocation can.

What a guest sees of a refusal depends on the path:

- OpenGL. vrend reads the GL error queue after the storage calls of a texture
  or buffer, and `GL_OUT_OF_MEMORY` fails the creation with `ENOMEM`; the device
  answers `VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY` and releases nothing of the
  resource but its half-made object. The guest kernel's creation ioctl does not
  read that answer, so the guest still gets a texture that has no storage and no
  error: the host's Zink logs `couldn't allocate memory`, and what the guest
  draws to or reads from the texture is lost. The context, the renderer and
  every resource made within the budget stay as they were. While the budget is
  full even a transfer is refused, since it needs a staging buffer, and the
  host's GL context records `GL_OUT_OF_MEMORY` for the guest's `glReadPixels`.
  Once the guest releases memory new textures store again, although the driver
  keeps released memory for reuse: a refused reservation first frees what the
  driver caches. `gpu-host-gl-device-memory-budget` drives this without a guest.
- Vulkan on Venus. The guest driver submits `vkAllocateMemory` asynchronously
  and waits for its result only when it maps, exports or frees the memory, so
  the host's refusal reaches the guest as a context that fails at the next
  command naming the allocation, and the guest process aborts. The render worker
  retires the fences of a failed context at once, so the process ends when its
  next fence wait returns instead of after the host's 30 s overdue-fence
  escape. The host, the budget and every other guest process are untouched, and
  a new process allocates at once. With `VN_PERF=no_async_mem_alloc` in the
  process's environment the call waits and returns
  `VK_ERROR_OUT_OF_DEVICE_MEMORY`, and the device, the allocations already made
  and later allocations stay usable. The advertised heap is B, but the pool
  counts everything the host holds, so a guest cannot tell from the heap size
  whether an asynchronous allocation will fit.
- A draw whose geometry heap the pool refused. A device with no geometry heap
  fails the recording of a command buffer, `vkEndCommandBuffer` is the call that
  reports it, and Venus sends that call without a reply, so the guest would see
  `VK_SUCCESS` and the host silently drop the whole command buffer. A device's
  first heap, at the 1 MiB floor, is therefore made although the pool refused
  its reservation: the pool counts it once Metal does and never refuses it, so
  a full pool can leave the process one floor per device past B. A draw that
  needs more than the floor loses its output and the loss is logged once, as on
  any heap too small for its draw, and the command buffer is whole. Growth past
  the floor stays within the pool.

The counter also includes memory the guest has already freed while its blob
resource is still referenced: a freed swapchain image stays allocated until the
guest's resource release arrives on the control queue, a few milliseconds after
`vkFreeMemory`. A loop that frees and reallocates large images can therefore be
refused while its live memory is below B. The pool writes one `uml: the GPU
budget is full` line to stderr the first time it refuses an allocation, with
the usage and the limit at that moment.

### Android guest requirements

The Android guest reaches the same renderer by a separate route. One Soong port
of this project's own guest Mesa source provides both of its graphics stacks:
the Virgl gallium driver behind the `kernarium` EGL and GLES libraries, and
Mesa's Venus driver installed as the Vulkan HAL `vulkan.virtio.so`, with
gralloc buffers from minigbm and presentation through
`VK_ANDROID_native_buffer`. That contract, including which guest epoch the
hardware-buffer path needs and why no dEQP level is declared, lives in
[`../android/lineage-24/README.md`](../android/lineage-24/README.md#graphics).

Khronos publishes Android's mandatory Vulkan functionality as the
`VP_ANDROID_17_requirements` profile, which chains the
`VP_ANDROID_vulkan_profile_2025` baseline. The Linux Venus guest meets its API
version (the guest reports 1.4.363 against a required 1.4.335), its limits, its
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
layout accepted only where Metal can read it: a plane may start at any offset
that is a multiple of Metal's alignment and may have any row pitch that holds a
row, keeps to the same alignment and is no wider than the widest texture, which
is how a half-width chroma plane laid out at the pitch of its luma plane is
imported. What it carries that plain linear tiling does not is a colour and an
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
the one configuration in which Zink imports it as that image. Because
such an image is one renderer blob resource whose refusal is reported
asynchronously, a native-heap surface advertises at most 8192 in each direction
as its maximum image extent and refuses an image above the per-image budget
with `VK_ERROR_OUT_OF_DEVICE_MEMORY` rather than presenting a dma-buf with no
resource behind it; ordinary images keep the device's own
`maxImageDimension2D`. Either way the
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

The guest driver and the renderer generate their halves of the encoder at
build time from one pinned venus-protocol and its maintained series
([`../dependencies/README.md`](../dependencies/README.md)), so the two halves
change together: a guest image sealed against one protocol cannot talk to a
renderer built from another. An extension the protocol does not name has no
index in `vn_info_extension_index`, so the guest silently refuses it however
the other layers are configured. The maintained series adds the four
extensions the pin does not describe: `VK_EXT_image_compression_control` with
its swapchain companion, `VK_EXT_device_address_binding_report` and
`VK_KHR_pipeline_binary`.

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
cache of its own. A binary's key is a hash of the bytes it carries, so
byte-identical binaries share one key; a pipeline created later from the same
state without binaries therefore compiles again rather than finding the entry
a loaded binary left in the driver's shader cache. A
`vkGetPipelineBinaryDataKHR` buffer too small for the binary is answered with
`VK_ERROR_NOT_ENOUGH_SPACE_KHR`, the size the caller needs, and no data, and
the renderer refuses a `vkGetPipelineKeyKHR` whose `VkPipelineCreateInfoKHR`
carries no chain. The topic messages of the Mesa, virglrenderer and
venus-protocol series record why.

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

### KosmicKrisp driver contracts

These are the run-time guarantees of the host KosmicKrisp driver as this
project's Mesa series patches it; the series and its regression suites are in
[`../dependencies/README.md`](../dependencies/README.md).

KosmicKrisp never hands Metal a texture view it cannot make, because Metal
answers one with an assertion that ends the process. An image view whose type,
levels or layers its image cannot provide fails with
`VK_ERROR_OUT_OF_DEVICE_MEMORY`, except that a 3D view of a non-3D image is made
as the 2D or 2D-array texture the image is: Venus creates views without waiting
for the result, so a refusal would surface later as a lost device. Metal still
decides pixel-format compatibility.

Programmable primitive IDs cross the Metal vertex/fragment interface as flat
user varyings. Layer and viewport indices use unsigned native Metal attributes
without interpolation qualifiers.

KosmicKrisp gates unsatisfied timeline waits on the CPU before native Metal
submission, using the common Vulkan runtime's on-demand submit worker. This
keeps delayed host signals from outliving Metal's finite GPU event-wait window;
`MESA_VK_ENABLE_SUBMIT_THREAD=false` still preserves that required fallback.
Only a wait whose signal has not been submitted is gated. A value already
signalled on the CPU or enqueued on the queue's Metal timeline is pending, so
ordinary dependencies keep the immediate path, the worker stays off, and
`vkQueueSubmit` never waits for the GPU work its batch depends on. A binary
semaphore wait is pending by definition because its signal must have been
submitted first. That distinction is what keeps a gated batch from blocking
the caller that has to release it. Once the worker runs, a binary semaphore's
payload moves to the batch waiting on it when the worker reaches that batch,
after any signal queued ahead of it, never inside `vkQueueSubmit`, where a
still-queued signal would land on a fresh payload. A timeline wait whose
signal is submitted later to the same queue remains unsupported: the device
exposes one queue and submits in order. CPU waits preserve their absolute
deadlines and observe device loss so pending submissions and queue drains can
shut down before device resources are released.

A render pass clear depth reaches Metal already rounded to the attachment
format's nearest representable value and offset inside that texel's interval.
Metal converts the clear into a normalized-integer depth attachment by
truncating, so an unrounded value would clear `D16_UNORM` one step low and
disagree with the rasterizer's own depth writes, which round. Depth formats
Metal stores as floats keep their exact value.

Subgroup operations reach Metal's SIMD groups, which exist in kernel and
fragment functions only, so the driver advertises compute and fragment as its
subgroup stages. A vertex, tessellation or geometry shader that reads the
subgroup builtins or uses a group operation still compiles: those stages
present the single-invocation subgroup their properties imply, with size and
count one, invocation zero, and every group operation returning the value that
one invocation contributed.

The shader outputs Metal names itself keep the shape Metal gives them, not
the shape the shader's stores imply: the clip coordinate is four components
even when a shader writes only `gl_Position.xy`, point size is one float, the
layer and viewport indices are unsigned, and the clip and cull distances are
float arrays. A component no store reaches keeps the value the output is
initialized with, which for the clip coordinate is the identity `(0, 0, 0, 1)`
rather than a degenerate zero `w`.

An image atomic is complete before the invocation's next image or global
access, barrier or termination, and before the shader ends. Metal issues an
atomic whose result goes unused without waiting for it, and the texture fence
does not order it, so a load could read the texel from before the atomic and a
store could land ahead of it. That is KosmicKrisp's `KK_WORKAROUND_20`,
reproduced in plain Metal on Apple7, where `compute_shader.resource-image`
read zero for one SIMD group's block of texels. The driver emits one
device-scope fence over device and texture memory where an atomic may still be
outstanding rather than one after every atomic, which keeps an atomic-only
kernel within about 2% of its unordered time. The Mesa tests
`kosmickrisp_image_atomic_order` and `kosmickrisp_msl_image_atomic_order`
cover the order and the placement.

A vertex or tessellation evaluation shader that writes memory and contains a
loop runs as compute, and a generated vertex function rasterizes what it
wrote. Apple GPUs kill a Metal vertex function invocation that runs for longer
than about 10 ms and report a timeout, losing what the invocation had not yet
done; a compute thread runs for far longer. On Apple7,
`texture_cube_map_array.image_op_vertex_sh` and its tessellation evaluation
twin copy a whole image from one invocation and land just past the limit,
failing under Metal validation or on a busy GPU. That is KosmicKrisp's
`KK_WORKAROUND_21`. Each kill is a GPU lockup that the system recovers from by
discarding the in-flight work of every process on the GPU, WindowServer and
other guests included, and Metal 4 reports those discards with the same
timeout code. KosmicKrisp's report of a failed submission therefore adds the
reason Metal nests under the error, which names a hang or a discard. A
store-free shader is left alone: killed, it would lose its
draw's rasterized output and set off that recovery, but no realistic one comes
near the limit.
The routed draws of a pass overlap as vertex functions do and cost about
0.12 ms per pass segment and 0.3 us per draw on an M1 Ultra. The Mesa tests
`kosmickrisp_vertex_compute_routing`, `kosmickrisp_long_vertex_invocation`
and `kosmickrisp_vertex_compute_draws` cover the decision, invocations of
50-100 ms and many routed draws in one pass.

The device residency set holds every Metal heap, and Apple's residency sets
make a heap's placed resources resident with it, so image textures need no
entry of their own. Metal's API and shader validation check each texture a
command buffer reaches by itself, though, and shader validation silently drops
the image loads and stores it does not find; under it, a vertex shader copying
a cube map array image read and wrote nothing. While a validation layer wraps
the device, which its class shows at creation, KosmicKrisp therefore also
registers the textures of bound images, image views and texel buffer views for
their lifetimes. Without validation it registers none, since that costs about
1.4 us per image view on an M1 Ultra. The Mesa test
`kosmickrisp_validated_image_access` runs the copy under shader validation.
Metal reads the residency sets of a queue while it commits to it, and a set's
methods are not thread-safe; under shader validation the commit walks every
heap of the device set, and a heap freed on another thread meanwhile crashed
the process. Every queue commit therefore holds the lock that every write to
the set holds, so a write waits for at most the commit in flight and a freed
allocation still stops counting against the device when `vkFreeMemory`
returns. The Mesa test `kosmickrisp_residency_commit_race` frees memory on one
thread while another submits under shader validation.
A cube or cube map array storage image is a 2D array view of its image, so
every shader access to one, its size query and the bounds check in front of an
image atomic included, reads the descriptor through the 2D array type. Metal
answers a query through another texture type only by chance, and under shader
validation it answers zero. The Mesa test `kosmickrisp_validated_cube_storage`
runs atomics and size queries on cube images under shader validation.

Linked graphics pipelines share native programs and dynamic vertex-input
variants only while a live Vulkan pipeline owns them. The lookup cache drops
its entry and releases those resources when the last pipeline owner is
destroyed, so obsolete programs do not accumulate until device teardown. A
dynamic vertex-input variant frees its generated MSL once its Metal pipelines
are built; the one use left for it, a render pass that binds fewer colour
attachments than the pipeline declares, compiles the variant again from the
parent's NIR and the variant's vertex input. `MESA_KK_REGENERATE_MSL=1` makes
every draw take that path, so the tests can exercise it.
Pipeline statistics use draw-time guards in the same executable, including its
bounded raster variants; query begin and end never compile or retain a second
shader tree. Every active statistics query counts into one shared block; a
statistics query begin or end that follows counted work adds the block into
the report of every query active through it and starts a fresh one, so
overlapping queries each count what they would alone. Vulkan forbids that
overlap (VUID-vkCmdBeginQuery-queryPool-01922), but Zink begins one statistics
query per GL target. A statistics query becomes available only once the
encoder that adds its counts has ended, because Zink keeps the first result
that a poll without waiting finds available. A discard variant preserves
application shader effects and transform feedback without raster-only
materialization or provoking-vertex conversion. Dynamic discard changes select
the corresponding precompiled variant, including after pipeline-cache reload.

Sequential encoders of one recording share a Metal command buffer. Only
compute that must run before or after a whole render pass, and each encoder of
a layered or multiview pass, gets a Metal command buffer of its own. The queue
starts such an encoder's command buffer only once everything committed before
it has completed, so a recording that holds one reaches Metal as several
commits with a queue-level event wait before each such command buffer: Metal
otherwise loses leading vertex batches of a layer-selecting draw that is
ordered behind another layer's draw only by a barrier or fence. A command
buffer takes a native command allocator for a lane only when that lane
records. Every submission takes its allocators and Metal command buffers with
it and hands the allocators to the device's allocator cache when the GPU
finishes; a reset hands over only allocators that were never submitted. A
resubmitted command buffer is recorded again, so its re-record first waits
for the GPU to finish its last submission, which a `SIMULTANEOUS_USE` buffer
may still be running. An allocator keeps its
high-water storage until it is released, and a recording that takes a grown
one keeps all of it, so the cache recycles only allocators of at most 16 MiB,
keeps at most 64 MiB in total, and releases every other allocator. Feedback
reporting remains valid after Vulkan device teardown through a separate
reference-counted context.

### Apple8 (A15) device behaviour

The physical iOS device the Vulkan route has been exercised on is an iPad
mini 6: an A15, Metal family Apple8, 4 GiB, under iOS 27. It differs from the
Apple silicon Macs the same stack runs on, which report Metal family Apple7,
in ways that change results rather than only speed. Each item below either
names the behaviour that answers it or stands as a limitation; none of them
has been observed on a Mac, and none has yet been checked on another iOS
device.

Of the Metal capabilities the driver keys on, only
`supportsBCTextureCompression` and `supports32BitFloatFiltering` differ from
the Mac: both are false here, and the driver takes both from the device rather
than assuming. The device's ceilings are smaller as well: `maxBufferLength` is
2 GiB and the recommended working set 2.67 GiB.

#### A GPU fault is reported as a timeout

An Apple8 GPU under iOS 27 reports a GPU fault as a timeout:
`MTLCommandBufferErrorTimeout`, or `MTL4CommandQueueErrorTimeout` on the queue
the driver uses, with Metal's placeholder end timestamp. Only the classic
`MTLCommandBuffer` error options expose the per-encoder status that names the
encoder `MTLCommandEncoderErrorStateFaulted`; Metal 4 has no equivalent, so the
driver's own queue cannot tell the two apart. A "timeout" on this device is
therefore a label, not a diagnosis, and must not be read as evidence that a
shader ran too long: a faulting submission is reported within about 20 ms of
its commit, while renders several times that long complete without error.

The timestamps on a failed commit do not measure execution either. A fault
carries the placeholder end, which the driver prints as
`gpu_end_s=6148826548.220315` and describes as a submission the GPU started
and never reported finishing; a refusal by a dead queue carries an equal or
microseconds-apart start and end. A timeout that host contention causes on
macOS carries a real interval and leaves a working queue behind.

#### A faulted command queue never works again

An `MTL4CommandQueue` that has killed one submission admits every later commit
and reports it failed within 0.1 ms, still labelled a timeout, for as long as
the queue object exists; a classic queue answers the same way with an
access-revoked code. Nothing in Metal resets a queue, so one fault would fail
every later submission of the device.

The driver retires a queue a failure has killed and commits to a replacement
on the queue's own submit thread, once Metal has reported every submission the
retired queue was given, so no submission is split across the two and
single-queue ordering holds. A fault costs the results of its own submission
and nothing else, and the device is not lost. A storm of timeout reports across
cheap tests means a dead queue is still in use, not that the tests are
expensive.

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

#### A rejecting queue drops a bare signal in silence

Metal reports a failure through the feedback of a commit, and a signal enqueued
without a command buffer behind it has none. A queue that rejects every
submission, as the queue of an app iOS has denied the GPU does, therefore
drops such a signal and reports nothing: the failure count the recovery above
keys on stays zero, and the queue holds nothing it has not answered for, which
is how a queue that has stopped looks. The driver lost the device for it after
thirty seconds, for good, with the guest's GL client waiting in
`vkQueueWaitIdle`.

A waiter on a signal that has been overdue for a second on a queue that has
reported no failure and holds no unanswered work therefore gives the queue
something to report: an empty command buffer committed behind everything it
holds, once a second at most (`kk_queue_probe`). A queue that rejects it
reports a failure, which completes what the queue owes as any other failure
does and retires the queue at the next submission; the wait ends about a second
after it began. A queue that runs the probe says nothing, and one held up
behind a wait only the host can release answers when the host does, so the
probe never completes a signal and a stopped queue is still lost at thirty
seconds. The test `kosmickrisp_queue_denied` reproduces the denial on a Mac by
rejecting commits and dropping signals in the Metal queue's methods.

A query a rejected submission was to make available stays unavailable, since
its availability write died with the commit, and `vkGetQueryPoolResults` with
`VK_QUERY_RESULT_WAIT_BIT` ends the wait for it after ten seconds with
`VK_ERROR_DEVICE_LOST` for that call. The Zink GL clients the iOS hold
protects cannot reach that bound: Zink never passes the bit to
`vkGetQueryPoolResults`, its blocking waits are on the batch's timeline
semaphore, which the recovery above completes, and its
`vkCmdCopyQueryPoolResults` with the bit set is a GPU copy that reads
availability without waiting. Only a Vulkan client that waits on such a query
sees the loss.

#### Fragment-stage store work faults the GPU

A render pass whose fragment shader performs enough `device` store work faults.
The fault tracks the fragment stage's store work, not elapsed time: a native
Metal program that loops over a store in every fragment of a 256x256 pass
faults at about 12,000 iterations per fragment, never faults without the
store, and runs 32768 iterations on a Mac.

Nothing in the driver removes it, because the stores and the fragment count
belong to the application. In `dEQP-VK.spirv_assembly.type.vec4.i64` and
`vec4.u64`, `clamp_frag` and most of the
`bit_field_insert_offset{8,16,32,64}_count{8,16,32,64}_frag` cases stand on it,
because dEQP has every one of 65,536 fragment invocations write every element
of the output buffer on each of 5120 or 8200 loop trips; even a narrower
type's `clamp_frag` can cross the threshold. dEQP reports each as
`Upper left corner mismatch`, since a killed draw leaves the clear colour,
never as a timeout. Their `vert`, `tessc`, `tesse`, `geom` and `comp` variants
run the same loop a handful of times and pass, and which cases nearest the
threshold fail varies from run to run.

Two driver changes lower the per-iteration work and so keep some sibling cases
under the threshold; they are mitigations, not a fix. Descriptor loads are
emitted in MSL's `constant` address space, where the compiler can hoist them
out of a loop that also stores, and buffer accesses use MSL's unpacked vector
types where NIR proves the alignment. On this GPU `constant` is a small
uniform-access path rather than a free read-only annotation, so streamed data
stays out of it. The rejected levers are recorded in
[`optimization-history.md`](optimization-history.md).

#### An out-of-bounds texture atomic faults the GPU

Metal carries `robustImageAccess2` semantics for a texture read and a texture
write by itself: a read outside the texture returns zero and a write outside it
is discarded. A texture atomic is not covered. An atomic whose coordinate, array
index or cube face lies outside the texture reaches memory the texture does not
own, and on this device that access never completes: the submission is killed
and reported as a timeout. An out-of-range texture read or write in the same
shader is harmless, and so is the atomic itself on a Mac.

The driver rejects the coordinate before the atomic issues. KosmicKrisp runs
`nir_lower_robust_access` over image atomics alone, ahead of descriptor lowering,
so the comparison is made in the Vulkan coordinate space against the size the
image deref still names, before the cube and 1D rewrites fold a face or layer
into Metal's coordinate. Reads, writes and buffer accesses keep the robustness
they already had, so nothing else gains a bounds test. The
`dEQP-VK.robustness.robustness2` cases in which a format-qualified
`r32i`/`r32ui` storage image takes an `imageAtomicAdd` at a deliberately out of
range coordinate from a render stage, thousands of times per invocation,
reproduce the fault without that check and run clean with it. A kill does not
always land on the submission whose verdict it decides, so without the check
that group reports fewer failures than kills.
`dEQP-VK.image.atomic_operations` validates the in-bounds result of an atomic
on every view type.

#### Render-stage 3D texture sampling faults the GPU

A draw whose vertex or fragment shader samples a 3D floating-point texture often
enough faults, the way fragment-stage store work does above. Every access
involved is ordinary and in range, so this is a work limit and not a robustness
problem, and the driver has no coordinate to reject. The fault tracks how much
3D sampling the render stage does, and there is no per-sample work left in the
generated MSL to remove, so nothing in the driver removes it.

Two `dEQP-VK.robustness.image_robustness` cases stand on it,
`push.notemplate.r32f.dontunroll.nonvolatile.sampled_image.no_fmt_qual.img.samples_1.3d.frag`
and its `vert` sibling. Their shader samples a 5x11x6 `R32_SFLOAT` `sampler3D`
nine times on each of the 2,101 trips of a loop it is told not to unroll, about
19,000 samples per invocation. The `comp` sibling issues the identical Metal
shader from a dispatch and never faults, and both render-stage cases pass on a
Mac.

The limit is not confined to 3D textures. The `2d_array` siblings of the same
test,
`{bind,push}.notemplate.{r32f,rgba32f}.dontunroll.nonvolatile.sampled_image.no_fmt_qual.img.samples_1.2d_array.{frag,vert}`
and their pipeline-library variants, run the same loop over a layered image,
and several of their render-stage cases fail, while every `1d` and `2d`
sibling passes.

#### A long run of small render passes faults the GPU

A process that has already pushed a few thousand small render passes through
the device starts having single submissions killed, reported as the timeout
above. The fault tracks how much the process has already done rather than the
size of one submission. The kill lands on one of the submissions carrying the
render passes, never on anything else, and which one is random.
`dEQP-VK.pipeline.*.render_to_image.core.{1d_array,2d_array}.huge.layers`
stands on it: each of those cases renders one layer at a time into a 256-layer
image, so it records 256 single-layer render passes of 44x23 (or 54x1), one
draw each, into a single command buffer. The failing `2d_array` cases pass when
run alone and fail when their neighbouring cases have run before them in the
same process, and the first run after the device has been idle is usually
clean. These observations were made while every render encoder reached Metal
as its own `MTLCommandBuffer`; the driver now records sequential encoders into
one. Nothing in the driver has been found that removes it, and the whole family
is clean on an Apple silicon Mac.

#### Memory budget

iOS ends the app at a physical footprint of about 3 GB on this device. A
1536 MiB guest already leaves the host side within a few hundred megabytes of
that before the brief `IOAccelerator` transient of `render_to_image` cases with
huge attachments, so guest RAM cannot be raised to make room for a workload.
Two consequences follow.

The per-device geometry heap starts at 16 MiB rather than a fixed 128 MiB,
because a Venus host pays it once per guest `VkDevice` that draws through it;
a device that never does allocates none. It grows, at least doubling, to a cap
of 128 MiB or its starting size, whichever is larger, and
`MESA_KK_HEAP_SIZE_MIB` sets the start from 1 to 4095 MiB without a rebuild.
What a guest sees:

- A direct draw whose reservations the driver can bound grows the heap before
  it is recorded and renders on its first submission. The restart unroll's
  bound is exact; tessellation is bounded at the largest level the TCS writes
  when every level the domain reads is a compile-time constant, and at the
  level-64 maximum otherwise.
- A reservation only the GPU can size, from an indirect draw or a stage after
  tessellation or geometry, is refused when it does not fit. That draw loses its
  output and the command buffer completes; the first loss on a device is logged
  once, and later command buffers get a heap large enough for what the refused
  render segments needed, as far as the pool below allows.
- If Metal refuses the first heap, a draw that needs it is not recorded and its
  command buffer ends with `VK_ERROR_OUT_OF_DEVICE_MEMORY`; if it refuses a
  larger one, the device keeps the heap it has.

The level-64 bound is conservative, so when the TCS computes its levels the
heap grows with the patches a render pass draws directly, whatever their
levels: past about a thousand quad patches it reaches the cap. Constant levels
bound it by the level written instead, and 65,536 triangle patches at level 1
fit the starting 16 MiB.

Zink keeps freed buffers in a cache for 500 ms so that a new allocation reuses
one, and Mesa freed an expired buffer only when another was added or reclaimed,
so a client that stopped allocating left the cache's contents held: 25 to
30 MiB at idle and about 75 MiB right after apps closed. Each Zink screen runs a
reaper thread (`pb_cache_reaper.c`) that sleeps while the cache is empty,
otherwise until the oldest entry expires, and then frees what has expired; adding
a buffer wakes it. The cache keeps at most 32 MiB of unused buffers, or an eighth
of the heap when that is smaller, so its bound is the same on every host rather
than following the heap size KosmicKrisp reports. `ZINK_BO_CACHE_MAX_MIB`
changes the cap, and assertions check that the cache stays within it.

A guest GL context that stops submitting keeps what its last batches used. Zink
holds a flushed context's completed batch states, and every resource object they
reference, until that context's next batch asks for a state, and the states a
context has handed to the screen wait for the next submission of any context;
glthread and the threaded context keep the last reference to a texture the guest
deleted until the context's next fence. At Retina idle that was three to five
full-screen heaps for as long as the guest stayed still. The renderer therefore
records each Virgl context's last command, transfer, fence and fence retirement.
A context with no fence outstanding and no activity for one second is trimmed
with `virgl_renderer_context_trim`, and trimmed once more a second later, and
any new activity arms it again. The second trim is the one that matters to a
context that only uploads: the flush inside the first trim submits its last
batch, which has not completed when that trim looks at it, and the idle context
would not submit again to reset it. The renderer's own context, context 0, is
tracked the same way. A texture or buffer made and written when a command names
no guest context (a gralloc allocation, an upload before the guest's first
command) is made and written there, and its GL context kept the storage of what
the guest had since deleted until the renderer next submitted on it, which
for a guest that had stopped allocating was never: 48 textures of 4 MiB stayed
allocated, 384 MiB, for as long as the renderer was quiet, and counted against
the budget. A resource's creation, a transfer and a readback on it count as the
activity of context 0, and `virgl_renderer_context_trim(0)` trims vrend's own
context. The GPU service thread sleeps on a timer for
the earliest moment a context could be due, so an idle guest costs no wakeups. A
texture one context deletes may still have a sampler view that another made, and
Mesa destroys a context's own views when that context next flushes. A resource
the guest releases, a resource a context detaches and a context the guest
destroys therefore mark every context, without moving its own idle period. A
marked context is trimmed an interval after the first mark, which frees the
views, and an interval after that, which releases what they held; drops that
second trim finds bring a third two intervals later, and so on until one finds
none. Resource churn costs an idle context one trim per two intervals
and never keeps it from its own trim. `UML_GPU_IDLE_TRIM_MS` replaces the
interval and zero turns the trim off. The trim visits each of the context's GL
contexts, runs what glthread and the threaded context still have queued behind a
`glFlush`, and calls `eglTrimContextMESA`, which Mesa advertises as
`EGL_MESA_context_trim`. That reaches `pipe_context::trim`, which Zink answers by
handing the context's completed batch states to the screen and resetting every
state the screen holds, without waiting for the device and keeping the states for
reuse. A context that has not finished its last batch keeps it, and a render
target that is still the context's bound framebuffer stays referenced until the
context binds another. So does the sampler view of a texture that is already
deleted while an idle context still has it bound: that context frees it at its
next flush, not at the trim, so a closed app can leave one full-screen heap for
the seconds until then.

KosmicKrisp reports physical memory as its one host heap, 64 GiB on a large Mac
and 4 GiB on an iPad mini 6, and the memory budget it reports follows. Apps and
drivers that size their caches from either see that difference.
`MESA_KK_SYSMEM_HEAP_MIB` replaces the figure, up to physical memory, and every
heap's budget is held to the heap size; a measurement run states 4096 to see what
the device reports. Unset, the heap is physical memory.

The shared-memory segments that virglrenderer maps with the host stack (a texture
export, a Venus shared-memory blob) are unlinked container files on iOS, where
`shm_open` is denied, and POSIX shared memory on macOS. A file's pages are
outside an iOS application's footprint and POSIX shared memory is inside it, so a
macOS run reads high by the size of the segments: 25 MiB at a 1133x687 display
and 58 MiB at 2266x1374 for the Android UI drawn by Venus. `VIRGL_ANON_FILE_UNLINKED=1`
makes a macOS run take the file, so a measurement reads what the device would;
unset, empty or `0`, macOS keeps POSIX shared memory, because the pages of a file
can be written back to the volume of the temporary directory.

A pooled device charges its heap to the host's device-memory pool (see [the
budget](#device-memory-budget)), so the heap and the guest's allocations share
one bound. A heap the device creates or
grows takes at most half of the pool's headroom, a heap the pool cannot hold is
not created, and a device's first heap is made at 1 MiB even then. A heap
grown past its start is freed once nothing holds it and the headroom has fallen
below half its size. A device created without a pool keeps the fixed cap. The
rules are the comment above
`KK_HEAP_START_MIB` in `src/kosmickrisp/vulkan/kk_cmd_draw.c` and
`kk_device_memory_pool.h`, in this project's Mesa series.

An uninterrupted released mustpass fraction runs about 200,000 cases in one
`deqp-vk` process, and a 1536 MiB guest cannot hold every group in that
traversal. The cost has two parts: a baseline that grows through the fraction,
from the released case hierarchy the enumeration builds to about 1 GB near its
end, and per-group transients on top of it, such as `dEQP-VK.reconvergence`,
whose tests simulate each subgroup's control flow on the CPU. The guest reaper
takes `deqp-vk` where the two meet. Raising
guest RAM is not available, and a 1024 MiB guest is killed earlier still.

Two further consequences. The already-transported `results.qpa` stays in the
guest's `/run` tmpfs, which is guest RAM, and holds nearly a fifth of the guest
by the time the reaper fires. And a killed `deqp-vk` takes the whole service cgroup
with it, so the guest prints no further progress line and sends no artifact
frames: the run goes silent rather than reporting a stall, and its verdicts
survive only as the result frames already on the console. The device lane ends a
run within three minutes of the reaper's own line rather than waiting out its
stall guard, and writes whatever the console carried.

A baked device campaign therefore leaves out the groups its guest memory cannot
hold, under the exclusion contract in [`testing.md`](testing.md).

### Experimental host memory

Native graphics allocations are separate from the configured guest RAM limit.
KosmicKrisp releases obsolete pipeline resources with their last Vulkan owner
and a submission's recording storage after GPU completion; see the
[KosmicKrisp driver contracts](#kosmickrisp-driver-contracts).

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
footprint grows with the index, by several MiB per MB of index, whatever the
data files hold. A pipeline built from a precompiled metallib skips only the
`libraries` index. Metal stops each data file near 1.1 GB and evicts from
there, so a full cache holds about 2.2 GB behind an index of about 23 MB that
costs around 100 MiB; GPU conformance campaigns fill it. Metal's public API
has no call that clears, bounds, splits or bypasses the cache:
`MTL4CompilerDescriptor` carries a label and a pipeline data-set serializer,
and binary archives add lookups without taking a pipeline off the cache. Its
binary exports `MTLSetShaderCachePath`, which no header declares and which
places the whole process's cache in an existing directory before first use;
only the GPU unit tests call it.

Kernarium therefore sizes its cache as the application starts, before any
view or runtime exists and so before anything can read it, and removes the
whole cache when its `.list` index files together hold more than
`MetalShaderCache.indexLimit`, 1 MB. It logs what it found either way. Real
workloads stay far below the limit, because their keys are stable: a repeated
boot, benchmark or page reuses its entries and adds none. A desktop boot,
glmark2 over Virgl and over guest Zink on Venus, WebGL Aquarium in Chromium
and a Steam Big Picture boot together leave about 0.12 MB of index and 30 MB of
data. Each entry's index record has the same size on the iPad and the Mac, so
the same workload leaves the same index on both. At the limit the index costs a
few MiB; the measured costs are beside `indexLimit` in
`GuestHost/Sources/MetalShaderCache.swift`. Removing a full cache is quick,
and the next boot then compiles the host's own pipelines before the guest's
first frame, and each workload compiles its shaders once more.

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

The macOS command-line emulators, `uml-darwin` and `uml-darwin-android17`,
carry the bundle identifier `com.imxieyi.uml-darwin` in an Info.plist embedded
in the executable (`darwin/app/macos`), so their cache is
`$(getconf DARWIN_USER_CACHE_DIR)/com.imxieyi.uml-darwin/com.apple.metal`, one
numbered file pair per concurrent process. Both executables share it because
they compile the same Mesa and KosmicKrisp shaders. A process with no bundle
identifier, such as a script or a unit test, shares `com.apple.metal` in that
directory with every other unsandboxed process of the user, so what an
emulator caches never reaches the host's other tools. Conformance campaigns
are what fill the emulator's cache: a Vulkan CTS case that compiles shaders no
earlier case compiled adds 8 to 23 KB of data and about 160 bytes of index, a
repeated case adds nothing, and each pair stops near 1.1 GB while more pairs
appear with concurrent processes.

The emulator bounds that cache the way Kernarium bounds its own, with the same
limit. When it starts with a GPU device, before anything in the process can
create a Metal device, it removes the cache once its `.list` index files
together hold more than 1 MB, and says so on stderr
(`darwin/device/gpu/metal_shader_cache.hpp`). That is the index of some 6,000
CTS cases' new shaders, and the cache costs a few MiB at the limit instead of
about 100 MiB at Metal's own ceiling. Processes that run on the cache when
another one removes it keep the files they have open and lose only what they
add afterwards, and a process that finds the directory gone creates it again,
so shards that start together need no coordination. A process without a bundle
identifier, such as a script or a unit test, shares `com.apple.metal` with
every other such process and is not bounded: the GPU unit tests keep their own
cache for this reason (see [`testing.md`](testing.md)), and a shared cache that
earlier emulators grew may simply be deleted.

## Validation

Use `check-gpu` for native GPU ownership and VNC checks and
`check-gpu-asan-ubsan` for the sanitized path. Guest conformance, WebGL,
Aquarium, and physical-device campaigns are opt-in; see
[`testing.md`](testing.md).

Historical backend and performance experiments are summarized in
[`optimization-history.md`](optimization-history.md).
