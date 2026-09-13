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
where Metal reports the host GPU and the full Metal 4 family.

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
