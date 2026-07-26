# Virtio-GPU/Virgl OpenGL Plan for Darwin UML

## Goal

Provide accelerated guest OpenGL through Mesa Virgl:

1. the guest uses Mesa's `virtio_gpu` DRM driver and Virgl Gallium driver;
2. Virtio-GPU commands cross UML's in-process vhost-user transport;
3. host virglrenderer executes them with caller-provided CGL contexts;
4. CGL selects the Apple GPU;
5. scanout contents are copied to the VNC framebuffer;
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

Virtio-GPU scanout -> VncDisplay -> VNC client

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
- Link the GPU backend with OpenGL, LibVNCServer, and libepoxy.
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
- readback of the active Virgl scanout into the VNC framebuffer.

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
- Register backend-request readiness through Darwin's normal UML IRQ path.
- Always retain per-queue call descriptors so completion delivery does not
  depend on the time-travel backend-request path.
- Register virtual keyboard and absolute-tablet devices backed by an inherited
  datagram descriptor so external VNC input reaches normal Linux evdev.

No reserved physical aperture, special PTEs, blob mappings, or vhost-user
shared-memory requests are required.

## Verification gates

### Gate 1: host build and tests

- Configure and build the macOS target.
- Run unit tests and the host VNC smoke test.
- Run a headless CGL/virglrenderer smoke test.

### Gate 2: immutable guest OpenGL

Boot with a temporary overlay and verify:

- `/dev/dri/card0` and `/dev/dri/renderD128` exist;
- DRM reports Virgl capsets 1 and 2;
- `eglinfo -B -p gbm` succeeds;
- desktop OpenGL and OpenGL ES renderers identify
  `virgl (Apple ...)`, not llvmpipe or Zink;
- the frozen `rootfs.img` mode, size, and SHA-256 remain unchanged.

This gate has passed with Mesa 26.1.2 and `virgl (Apple M1 Ultra)`.

### Gate 3: display scanout

- Exercise a deterministic DRM/KMS or compositor frame.
- Connect the VNC smoke client.
- Verify the captured framebuffer is non-blank and changes after damage.

This gate has passed with a 1280x720 Plasma framebuffer captured through VNC.

### Gate 4: KDE Plasma

- Start SDDM and a Plasma Wayland session.
- Verify `kwin_wayland` and `plasmashell` remain running.
- Confirm KWin uses the Virgl renderer.
- Interact through VNC and verify mouse and keyboard input.
- Confirm the Folder View desktop runs without the crashing `desktop:/` KIO
  worker.
- Shut down cleanly and re-check the immutable rootfs guard.

This gate has passed. `kwin_wayland` and `plasmashell` remain running, GBM/EGL
reports `virgl (Apple M1 Ultra)`, the VNC framebuffer contains the Plasma
desktop, mouse and keyboard events reach Plasma, and the final audit boot
reports no Virtio-GPU protocol errors or desktop-process crashes.

## Current status

All four gates pass. KDE Plasma Wayland is functional over VNC with accelerated
Virgl OpenGL on the host. Apple core-profile compatibility fixes in
virglrenderer translate its legacy GLSL 1.30 shaders to GLSL 1.50 and avoid
requiring promoted extensions that Apple OpenGL does not advertise. The final
guest audit reports `VIRGL_COMMAND_ERRORS=0`.
