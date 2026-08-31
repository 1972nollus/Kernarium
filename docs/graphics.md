# Apple graphics support

## Current path

macOS, iOS Simulator, and iOS device use one Virtio-GPU rendering path:

```text
guest Mesa -> Virtio-GPU/Virgl -> vhost-user -> virglrenderer
    -> EGL/GLES through ANGLE Metal -> MTLTexture -> AppKit or UIKit
```

The graphics dependencies come from the verified `apple-gpu` prefix. No Apple
GPU product links CGL, OpenGL.framework, EAGL, MoltenVK, or a second ambient
libepoxy/virglrenderer family.

The GPU transport thread owns renderer creation, context use, reset, and
teardown. AppKit and UIKit share the Metal presentation layer. VNC is a macOS
consumer of the same renderer and is intended for diagnostics and remote
display, not as a second graphics backend.

Pointer semantics are selected by the product launcher. Android native and VNC
input is exposed as one direct touch contact with absolute coordinates; hover,
auxiliary buttons, and wheel events are suppressed, and coordinates are sent
only for touch down, drag, and release. Non-Android guests retain absolute
pointer hover, primary and auxiliary mouse buttons, and wheel input.

## Guest capability boundary

The maintained guest path exposes Virgl desktop OpenGL 2.1 and OpenGL ES 3.0
on the qualified images. GLES 2 and GLES 3 conformance matrices have been run
on macOS and the signed 16 KiB iOS path. These results do not imply GLES 3.1,
GLES 3.2, desktop OpenGL above the exposed Virgl profile, or Vulkan support.

Unsupported resource shapes may use a counted CPU conversion path. Native
performance qualification requires zero sustained CPU fallback. Dropped
superseded presentation frames are successful guest completions; they must not
force synchronous GPU readback.

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
Virtio-GPU advertised mode and raises its display event. Presentation storage
remains sized to the active guest scanout until the guest accepts a mode with
`SET_SCANOUT`. EDID-backed dimensions are limited to 4095 pixels per axis; the
non-EDID Virtio-GPU path follows the DRM limit of 8192 pixels per axis.

The first active scanout and a modeset matching the current advertised or a
pending host target do not change geometry authority. A later guest-selected
mode that matches neither is independent and disables further host-to-guest
resizing until the next guest boot. With automatic resizing configured on, the
macOS window follows each independent guest mode; iOS instead aspect-fits the
guest mode in its view. With automatic resizing configured off, both products
retain the guest mode and aspect-fit it in the host content area.

Rendering and input use the same centered aspect-fit rectangle. AppKit maps
that rectangle into the fixed tablet range, while Kernarium maps it into the
touchscreen range advertised at boot. Input beginning in letterbox or
pillarbox bars is ignored; an ongoing pointer or touch is constrained to the
fitted edge so that its final release remains accurate.

## Vulkan

Guest Vulkan is not implemented. The current transport lacks the complete blob
resource, shared-memory, synchronization, and lifetime contract required by
Venus. MoltenVK alone does not provide that Virtio-GPU protocol boundary.

Any future Vulkan work must remain a separate capability project with an
explicit memory model, maintained kernel protocol changes, shared Apple
dependency provenance, and Vulkan CTS qualification. Until those pieces are
implemented and tested, the product must not advertise a Vulkan capset or
claim Vulkan support.

## Validation

Use `check-gpu` for native GPU ownership and VNC checks and
`check-gpu-asan-ubsan` for the sanitized path. Guest conformance, WebGL,
Aquarium, and physical-device campaigns are opt-in; see
[`testing.md`](testing.md).

Historical backend and performance experiments are summarized in
[`optimization-history.md`](optimization-history.md).
