# Virtio-GPU/Virgl OpenGL Plan for Darwin UML

## Goal

Provide accelerated guest OpenGL through Mesa Virgl:

1. the guest uses Mesa's `virtio_gpu` DRM driver and Virgl Gallium driver;
2. Virtio-GPU commands cross UML's in-process vhost-user transport;
3. host virglrenderer executes them with caller-provided CGL contexts;
4. CGL selects the Apple GPU;
5. scanout contents are presented either in a native Metal window or through
   the VNC framebuffer;
6. KDE Plasma runs through the accelerated DRM device.

Guest Zink/Venus, resource blobs, a host-visible shared-memory aperture,
MoltenVK, and Vulkan protocol extensions are outside this design.

## Architecture

```text
guest OpenGL application / KWin
        |
Mesa Virgl Gallium + GBM/EGL
        |
virtio_gpu DRM/KMS
        |
UML virtio/vhost-user transport
        |
VirtioGpuDevice
        |
VirglRenderer + virglrenderer
        |
CGL / Apple OpenGL / Apple GPU

Virtio-GPU scanout -> Display
                     |-> CGL texture -> IOSurface -> MetalKit window
                     `-> CPU readback -> VncDisplay -> VNC client

VNC keyboard/pointer events
        |
VncInputEvent datagrams
        |
UML VNC input driver
        |
Linux evdev keyboard + absolute tablet
```

The renderer and vhost-user server are created, used, and destroyed on the
same GPU transport thread. CGL current-context state is pthread-local, so this
thread-affinity rule is part of the implementation contract.

## Host implementation

### Build

- Build virglrenderer as a static library with OpenGL enabled and Venus
  disabled.
- Link the GPU emulator with OpenGL, Metal/MetalKit, IOSurface,
  LibVNCServer, and libepoxy.
- Keep rendering in the emulator process; no helper process or host display
  server is required.

### Virtio-GPU device

Implement:

- one control queue and one cursor queue;
- one scanout;
- `RESOURCE_CREATE_2D`, backing, transfer, scanout, flush, and unref;
- Virgl capsets 1 and 2;
- context create/destroy and resource attach/detach;
- `RESOURCE_CREATE_3D`;
- 3D transfers and command submission;
- ordered fences;
- direct native presentation of supported 2D Virgl scanouts, with readback of
  the active scanout for VNC and native fallback.

Virglrenderer owns a GL texture for each guest resource; it does not own the
host window or a presentation swapchain. Guest DRM/KMS may rotate multiple
resource IDs, but those CGL textures are not IOSurface-backed and therefore
cannot be imported directly by Metal. Native mode uses two IOSurface bridge
slots for cross-API lifetime safety. A CGL framebuffer blit is GPU-to-GPU,
Metal consumes at most one slot at a time, the window layer is limited to two
drawables, and a newer ready frame replaces an older one rather than waiting in
a display FIFO. This avoids both CPU transfer and an additional queued frame
of latency.

Advertise only the device features used by this path:

- `VIRTIO_GPU_F_VIRGL`;
- `VIRTIO_GPU_F_CONTEXT_INIT`.

### CGL contexts

- Request accelerated core-profile CGL contexts.
- Share renderer contexts with one process-global root context.
- Clear OpenGL errors produced during host capset probing before accepting
  guest commands; otherwise virglrenderer may attribute a stale Apple OpenGL
  error to the first guest transfer.

## UML changes

- Enable built-in DRM, Virtio-GPU KMS, Virtio UML, and Virtio input.
- Enable UML DMA/IOMEM emulation required by Virtio-GPU.
- Accept the in-process `fd=<number>` transport on Darwin.
- Normalize Darwin's host `EAGAIN`/`EWOULDBLOCK` values.
- Register emulator-request readiness through Darwin's normal UML IRQ path.
- Always retain per-queue call descriptors so completion delivery does not
  depend on the time-travel emulator-request path.
- Register virtual keyboard and absolute-tablet devices backed by an inherited
  datagram descriptor so external VNC input reaches normal Linux evdev.

No reserved physical aperture, special PTEs, blob mappings, or vhost-user
shared-memory requests are required.

## Verification gates

### Gate 1: host build and tests

- Configure and build the macOS target.
- Run unit tests and the host VNC smoke test.
- Run a headless CGL/virglrenderer smoke test.

### Gate 2: verified EROFS guest OpenGL

Boot with a temporary overlay and verify:

- `/dev/dri/card0` and `/dev/dri/renderD128` exist;
- DRM reports Virgl capsets 1 and 2;
- `eglinfo -B -p gbm` succeeds;
- desktop OpenGL and OpenGL ES renderers identify
  `virgl (Apple ...)`, not llvmpipe or Zink;
- the raw `rootfs.erofs` image is mounted at `/dev/ubda` beneath the tmpfs
  overlay and its manifested format and digest verify before and after the run.

This rendered gate passed in the dated partitioned-image campaign with Mesa
26.1.2 and `virgl (Apple M1 Ultra)`. The current EROFS runner and bounded
overlay/systemd guest path pass, but the full rendered EROFS GPU campaign was
not rerun during the rootfs migration.

### Gate 3: display scanout

- Exercise a deterministic DRM/KMS or compositor frame.
- Connect the VNC smoke client.
- Verify the captured framebuffer is non-blank and changes after damage.
- Open the native Metal window and verify scanout plus keyboard/pointer input.
- Prove an accepted native 3D scanout skips renderer readback and CPU display
  update; retain VNC and unsupported-resource fallback coverage.

The VNC side of this gate passed with a 1280x720 Plasma framebuffer. The native
window is a newer external GUI path and its interactive result must be recorded
separately from that dated VNC evidence.

### Gate 4: KDE Plasma

- Start SDDM and a Plasma Wayland session.
- Verify `kwin_wayland` and `plasmashell` remain running.
- Confirm KWin uses the Virgl renderer.
- Interact through VNC and verify mouse and keyboard input.
- Confirm the Folder View desktop runs without the crashing `desktop:/` KIO
  worker.
- Shut down cleanly and re-check the EROFS manifest guard.

This gate has passed. `kwin_wayland` and `plasmashell` remain running, GBM/EGL
reports `virgl (Apple M1 Ultra)`, the VNC framebuffer contains the Plasma
desktop, mouse and keyboard events reach Plasma, and the final audit boot
reports no Virtio-GPU protocol errors or desktop-process crashes.

## Current status

All four implementation gates passed in the dated partitioned-image campaign.
KDE Plasma Wayland was functional over VNC with accelerated Virgl OpenGL on the
host, and the final guest audit reported `VIRGL_COMMAND_ERRORS=0`. The current
launcher selects the native Metal window by default and preserves that dated
VNC campaign through `--display vnc`. Apple core-
profile compatibility fixes in virglrenderer translate its legacy GLSL 1.30
shaders to GLSL 1.50 and avoid requiring promoted extensions that Apple OpenGL
does not advertise. Current launchers use raw EROFS and no longer inspect Unix
mode; a complete KDE/VNC/WebGL EROFS requalification remains an explicit
external campaign rather than an inferred pass.

A focused native-window launch at 800x500 with four guest vCPUs reached
`graphical.target`, passed the overlay guard, and exercised the direct
CGL/IOSurface/Metal message before manual Ctrl-C shutdown. That proves native
window startup and the GPU-only scanout selection, not keyboard/pointer
interaction or a complete timed KDE qualification. The EROFS digest was
reverified after the run.
