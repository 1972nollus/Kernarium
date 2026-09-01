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
constrained-baseline, main, and high profiles up to 4096x2304; HEVC Main and
Main10 up to 8192x4320 and level 6.2; and JPEG Baseline/MJPEG up to 4096x2304.
The supported inputs are progressive 4:2:0. Eight-bit output uses NV12 or IYUV
guest buffers, while 10-bit HEVC output uses P010. The Apple host reconstructs
H.264 or HEVC parameter sets from the standard Virgl picture descriptor and
passes complete JPEG pictures to the corresponding VideoToolbox hardware
decoder. Guests use unmodified upstream Mesa and libva through the standard
VA-API path; applications, including Chromium builds, must enable Linux VA-API
decoding themselves.

H.264 High10 is advertised only on Apple targets where a profile-specific
VideoToolbox hardware probe succeeds. Qualified physical iOS hardware produces
10-bit bi-planar output. A session-only probe on macOS is insufficient: native
decoding rejects a representative High10 frame, so macOS does not advertise
the profile. Upstream Chromium maps HEVC Main and Main10 to its VA-API decoder
but does not map H.264 High10; the latter remains available on qualifying hosts
to upstream VA-API clients such as FFmpeg.

The stock Debian libva/Mesa interface does not expose the required H.264 or
HEVC 4:2:2 and 4:4:4 profile and surface combinations, so the host does not
advertise them. This boundary deliberately avoids guest-library forks or
private profiles. Encoding, interlaced H.264, and other codec profiles are not
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
pending host target do not change geometry authority. This correlation accepts
the up-to-16-pixel downward adjustment per dimension that Linux virtio-gpu
allows when CVT rounds a preferred host mode. A later guest-selected mode that
matches neither is independent and disables further host-to-guest resizing
until the next guest boot. Every active guest mode change resizes the macOS
content window, independently of that host-to-guest policy. iOS instead
aspect-fits every guest mode in its view. With automatic resizing configured
off, later host window or view changes leave the guest mode unchanged and
aspect-fit it in the available host content area.

Rendering and input use the same centered aspect-fit rectangle. AppKit maps
that rectangle into the fixed tablet range, while Kernarium maps it into the
touchscreen range advertised at boot. A touch beginning in a letterbox or
pillarbox bar remains outside the guest until its path enters the fitted
rectangle, where a new contact begins at the exact edge. Crossing back out
releases the contact at the exit edge; re-entry while the physical gesture
continues begins a new guest contact. Pointer and touch coordinates therefore
remain aligned with the pixels that are actually visible.

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
