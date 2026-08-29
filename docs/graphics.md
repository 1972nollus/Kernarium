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

Virgl also exposes hardware H.264 bitstream decoding for baseline,
constrained-baseline, main, and high profiles up to 4096x2304, with
progressive 8-bit 4:2:0 output in NV12 or IYUV guest buffers. The Apple host
reconstructs H.264 parameter sets from the standard Virgl picture descriptor
and requires a VideoToolbox hardware decoder. Guests use the upstream Mesa
VA-API path without a Mesa fork; applications, including Chromium builds,
must enable Linux VA-API decoding themselves. Encoding, interlaced H.264,
10-bit profiles, and non-H.264 codecs are not advertised.

Unsupported resource shapes may use a counted CPU conversion path. Native
performance qualification requires zero sustained CPU fallback. Dropped
superseded presentation frames are successful guest completions; they must not
force synchronous GPU readback.

The GPU advertises display geometry and density through EDID. Kernarium point
mode uses safe-area points and 160 DPI. Retina mode multiplies dimensions and
density by `nativeScale`. Android receives the same effective density on its
kernel command line.

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
