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

The maintained guest path exposes Virgl desktop OpenGL 2.1 and OpenGL ES 3.0
on the qualified images. GLES 2 and GLES 3 conformance matrices have been run
on macOS and the signed 16 KiB iOS path. These results do not imply GLES 3.1,
GLES 3.2, desktop OpenGL above the exposed Virgl profile, or Vulkan support.

Virgl exposes hardware bitstream decoding for H.264 baseline,
constrained-baseline, main, and high profiles and for HEVC Main and Main10, up
to 4096x2304. Progressive 8-bit 4:2:0 output uses NV12 or IYUV guest buffers;
10-bit output uses P010. The Apple host reconstructs H.264 or HEVC parameter
sets from the standard Virgl picture descriptor and requires the corresponding
VideoToolbox hardware decoder. Guests use the upstream Mesa VA-API path
without a Mesa fork; applications, including Chromium builds, must enable
Linux VA-API decoding themselves.

H.264 High10 is advertised only when a profile-specific VideoToolbox hardware
probe succeeds. The connected A15 iPad on iPadOS 18.5 passes that probe and
produces 10-bit bi-planar output. Current macOS VideoToolbox rejects a
representative High10 frame, so macOS does not advertise the profile. Upstream
Chromium maps HEVC Main and Main10 to its VA-API decoder but does not map H.264
High10; the latter remains available to upstream VA-API clients such as
FFmpeg. Encoding, interlaced H.264, and other codec profiles are not
advertised.

Supported NV12 and P010 surfaces keep VideoToolbox's IOSurface-backed
CoreVideo planes on the GPU: the host imports them through the ANGLE Metal
device and copies each plane into the existing guest texture without changing
its identity. IYUV and unsupported resource shapes may use a CPU conversion
path. Native performance qualification requires zero sustained CPU fallback.
Dropped superseded presentation frames are successful guest completions; they
must not force synchronous GPU readback.

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
