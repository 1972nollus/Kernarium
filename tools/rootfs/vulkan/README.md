# Patched Vulkan guest userspace

This directory owns the source inputs for the separately authorized Vulkan
rootfs derivative. It does not alter `rootfs.erofs` or the sealed GL/GLES
derivative. The image is accepted only when its checked receipt, read-only
attachment, CTS identity, and platform gates all pass.

The host backend is Venus over KosmicKrisp. The guest uses an ordinary Vulkan
ICD manifest rather than a portability-subset driver. Guest and renderer
codecs are generated offline into build directories from the same patched
Venus protocol and pinned SDK registry; existing command IDs and aliases
remain unchanged. Generated codecs, SDK headers, and registry copies are not
carried in the downstream patch series. The guest Mesa
series is intentionally downstream and contains transport, reporting, and
sharing correctness fixes:

- observe renderer pipeline-creation results synchronously;
- size host-image-copy replies from a describable Vulkan format;
- omit DRM display-acquisition extensions that the virtual device cannot use;
- preserve the renderer's published conformance version instead of replacing
  it with Venus's unknown `1.4.0.0` value;
- expose guest opaque-fd and dma-buf memory when the paired renderer can import
  the same HOST3D shared allocation through `VK_EXT_external_memory_host`, and
  return external allocation failures before publishing a guest memory handle;
- keep Venus implicit dma-buf images optimal when the renderer lacks DRM
  format modifiers, and use external queue ownership for their native aliases;
- transport device-address commands and maintenance8/maintenance10 structures
  when the paired renderer supports them, preserving native GPU addresses and
  dynamic-rendering teardown without CPU address remapping;
- preserve the known signed or unsigned color destination of generated integer
  blit shaders before NIR backend finalization, including mixed-signedness
  clamps and multisample paths. Floating-point color, depth/stencil, and
  TGSI-only consumers keep their existing paths;
- store a pixel transfer that converts between two different pure-integer
  formats without a blit. A colour attachment keeps only the destination's low
  bits rather than saturating into its range, so texture upload,
  `glGetTexImage`, `glReadPixels`, and their pixel-buffer paths use the CPU or
  compute transfer that saturates. The compute download reads the layer a
  layered attachment was bound to rather than layer 0, and a 3D download that
  starts away from slice 0 takes the CPU copy. Matching formats, and transfers
  through a floating-point or normalized format, keep their blit;
- return NULL from a threaded-context buffer map whose driver map failed,
  without annotating the untouched transfer or publishing an uninitialized
  CPU shadow, so the same context can still map afterwards;
- delete a pixel-transfer download specialization's compute state only when
  a later download compiled one, and free only the specialized shader a
  driver thread produced, so context teardown never hands the driver a NULL
  compute state; and
- bound the wait the X11 swapchain's event thread spends on Present events, so
  destroying a swapchain whose images the server still owns completes instead
  of deadlocking; and
- bound the wait a Wayland present spends throttling on the previous frame
  callback, and spend an unbounded `vkWaitForPresentKHR` in observing slices
  rather than one indefinite dispatch; and
- serve a descriptor set layout and a pipeline layout from the allocation
  callbacks their create command carried, and return the storage to those
  callbacks when the last internal reference goes away; and
- report a device reset status for a robust virgl context, so the guest can
  create one at all.

The first fix prevents a rejected native pipeline from becoming a stale guest
object and a later fatal command stream. The second prevents the renderer from
writing beyond a guest-sized reply for formats absent from Gallium's table.
The reporting fix keeps KosmicKrisp's CTS-recognized `1.4.3.2` value visible
through Venus. The external-memory fixes virtualize only the fd handles that
virtio-gpu can genuinely export and re-import. On Metal, an implicit guest fd
names the same native MTLHeap used by both optimal Vulkan image aliases, so
rendering and display scanout remain zero-copy without advertising a fictitious
DRM modifier. Native presentation matches the window system's character-device
fd to the renderer's exact guest DRM primary or render identity, including
non-PCI virtio devices. Different devices retain the cross-device path; an
import cannot grow the exported allocation.
For single-plane native-heap WSI images, the private implicit-import metadata
supplies a logical row pitch required by DRI3. Its envelope bounds the image's
width, height and total allocation, and the pitch itself is a 32-bit value that
linux-dmabuf and DRI3 v1.2 carry unchanged, so a wide image keeps its envelope
instead of failing swapchain creation. The metadata does not describe a linear
pixel buffer; the image remains optimal and the fd still carries only native
allocation identity. Legacy clients such as Gamescope that use Mesa's private
scanout image-create chain receive the same native-heap allocation and logical
pitch through their subresource-layout query. Ordinary Vulkan images retain
the renderer's subresource-layout result.
Because one such image is one renderer blob resource whose refusal is reported
asynchronously, a native-heap surface advertises at most 8192 in each direction
as its maximum image extent and refuses an image above the per-image budget
with `VK_ERROR_OUT_OF_DEVICE_MEMORY`, rather than presenting a dma-buf with no
resource behind it. Ordinary images keep the device's own
`maxImageDimension2D`.
The X11 swapchain's event thread waits for a Present event whenever the server
owns two or more presented images, which common WSI treats as a guarantee that
a `PresentIdleNotify` will arrive. It is not one: a compositor keeps the buffer
it is showing for as long as the window stays mapped, and an application that
destroys its window before the swapchain leaves the server with no present
state for it at all. Because `xcb_wait_for_special_event()` cannot be
interrupted, either state deadlocked `vkDestroySwapchainKHR` on joining that
thread. The thread now waits with a bound and re-reads the swapchain status,
which costs one poll wakeup per interval only while an event is genuinely
outstanding.
The Wayland swapchain has the same shape of assumption. On a compositor without
`wp_fifo_v1` a FIFO present is throttled with a `wl_surface.frame` callback, and
common WSI waited for it without a bound. A compositor only sends frame
callbacks for a surface it is painting, so an occluded or unpainted surface
never releases that wait, and because Venus runs presents on its own thread the
stall reaches the application as a `vkDestroySwapchainKHR` that never returns. A
present now gives up on the callback after a second and proceeds un-throttled;
the acquire still waits for a `wl_buffer.release`, so no more than the
swapchain's images are ever in flight. An unbounded `vkWaitForPresentKHR` is
still unbounded, as the specification requires, but is spent in observing
slices, so a retired swapchain ends it with `VK_ERROR_OUT_OF_DATE_KHR` instead
of waiting for a present id its successor owns.
Both layouts are reference counted, because the specification lets an
application destroy one while descriptor sets, pipelines, update templates and
recorded push descriptors still describe themselves with it. That is why the
driver took their storage from the device rather than from the creating
command's callbacks, which left an application's allocator unaware of the
layouts it paid for and let a layout succeed while every one of its callbacks
was refusing memory. A layout now keeps the callbacks that served it and
returns its storage to them on the final unref, with the device allocation
scope its extended lifetime needs; reference counting, and the renderer object
the last reference destroys, are unchanged.
A robust OpenGL context needs `EGL_EXT_create_context_robustness`, which the
guest EGL advertises only for a screen that answers a device reset query. The
virgl driver never claimed one, so every robust `eglCreateContext` failed with
`EGL_BAD_ATTRIBUTE` on a guest rendering through virgl. The claim now follows
the host's own `VIRGL_CAP_ROBUST_BUFFER_ACCESS` bit, needing no protocol
change, and the query answers `PIPE_NO_RESET`: a renderer that loses its device
loses the command stream the asking context lives in, so a live guest context
has no reset to report.

Passing this project's receipt-gated CTS campaign demonstrates the tested
stack's result. It is not by itself a Khronos conformance claim; that also
requires the applicable submission, review, and trademark process.

`sources.tsv` pins every upstream commit, tree, annotated release object where
one exists, and generated Mesa, Venus-protocol, and CTS mail-patch series.
Vulkan-Headers and Vulkan-Loader use the matching 1.4.357 SDK tag. The CTS source
remains based on Vulkan CTS 1.4.6.2 and carries two authenticated correctness
patches: memory/image-copy address ranges stay within their buffer in both
directions, and compute-only cases are skipped when no exclusive compute queue
exists. Generated patch files must be changed through a topic checkout and
regenerated with:

```sh
tools/rootfs/vulkan/rebase-patches.sh \
  --component mesa --source /path/to/mesa-topic
```

The accepted CTS image and its receipt name one patched tree per component, and
`EXPECTED_FINAL_TREES` in `seal-in-vm.py` names the same trees. Advancing a
series therefore invalidates the accepted receipt until a new image is sealed
and accepted: every receipt-bound tooling gate fails with `<component>.patches
differs from the generated series` in the meantime. That is the intended
interlock, not a defect in the gates; the way to clear it is to seal a new
image, never to relax a verifier.

The final build, installation, CTS build, image creation, filesystem checks,
and receipt generation run in a native arm64 Debian VM or, under `fakeroot`,
in an unprivileged arm64 Debian container that cannot create device nodes; the
sealer inspects the guest tree with a clean environment so no host preload
reaches the recorded runtime closure, and the receipt names the environment
kind it ran in. Prepared checkouts live
below `WORK/sources`, out-of-tree builds below `WORK/build`, staged installs
below `WORK/stage`, and immutable inputs below `WORK/input`. Mesa builds the
patched Virtio Vulkan driver together with matched GLX, EGL, GBM, OpenGL, and
GLES frontends. Its Gallium runtime contains Virgl for the ordinary desktop
path, Zink for OpenGL-over-Venus validation, and Softpipe as the non-LLVM
recovery path. Vulkan beta declarations remain enabled for the pinned CTS/API
surface but do not mark the ICD as a portability driver. Only Meson's
runtime-tagged install is accepted, and the sealer audits its exact files,
symlinks, and build options before copying the real runtime files.

`exclusions.txt` is the exact removal set applied to a fresh extraction of the
base image. The derivative retains the base libdrm, replaces the base Vulkan
loader with the pinned source-built 1.4.357 runtime, replaces the Virtio ICD in
place, and installs only the release-built Vulkan CTS runtime and its
manifest-referenced data. With `--include-opengl-cts`, the same source build
also installs `glcts` and its generated runtime-data closure. The GL46 scope is
derived from the four public must-pass XML collections used by the pinned
upstream runner: ES-for-GL, no-context, single-config, and per-config GL tests.
Unreferenced text files, including the retired GTF list, are not executable
requirements. This combined mode has a distinct
artifact identity and never replaces the accepted Vulkan-only image.

The immutable base identity is 3,802,316,800 bytes, SHA-256
`73e36c063e8374b68bdaca992f789d41dda59e159fd4b426dbbb5515dcf84c91`,
with EROFS UUID `b84fcbe2-0673-4126-8f81-bdee45997306`. The derivative is
8,589,934,592 bytes with ext4 UUID
`2240061a-2b64-4626-b8bf-dd0f9993a4a9`; its content digest and complete build
identity are owned by `tests/integration/gpu/vulkan_cts_receipt.json` and its
verifier.

The combined Vulkan/Zink/OpenGL derivative is named
`rootfs-vulkan-zink-gl.ext4`, uses ext4 UUID
`3b59fbc9-4b35-4a2b-890d-286a77b04afd`, and is consumed only with its emitted
schema-4 receipt. It binds the upstream runner identity, all four XML payloads
and hashes, their derived configuration records, and the eight referenced leaf
identities. Full qualification additionally freezes an ordered runtime
`CTS-Configs.gl46` inventory: the first actual EGL configuration receives the
one-time suites and first-config records; later configurations receive their
other-config records. XML surface sizes, seeds, and named FBO configurations
are retained. Historical schema-3 receipts remain readable for diagnostics,
but cannot define a full GL46 campaign. The OpenGL runner proves both the
Zink Gallium renderer and the underlying Virtio-GPU Venus device before
executing a shard. Vulkan and OpenGL qualification require the same current
source trees and ordered patch series for Mesa, Venus protocol, and CTS, plus
the pinned Vulkan-Headers and Vulkan-Loader sources. Their image, executable,
and runtime-closure identities remain separate.

Seal only new stage, image, and receipt paths:

```sh
sudo tools/rootfs/vulkan/seal-in-vm.py \
  --work-root /home/vm/uml-vulkan-rootfs \
  --metadata-root /home/vm/uml-vulkan-rootfs/input/repo/tools/rootfs/vulkan \
  --outer-root /home/vm/uml-vulkan-rootfs/input/repo \
  --base-rootfs /home/vm/uml-vulkan-rootfs/input/rootfs.erofs \
  --mesa-build-root /home/vm/uml-vulkan-rootfs/build/mesa \
  --mesa-stage-root /home/vm/uml-vulkan-rootfs/stage/mesa \
  --stage-root /home/vm/uml-vulkan-rootfs/stage/rootfs-kosmickrisp \
  --output /home/vm/uml-vulkan-rootfs/output-kosmickrisp/rootfs-vulkan-venus.ext4 \
  --receipt /home/vm/uml-vulkan-rootfs/output-kosmickrisp/rootfs-vulkan-venus.receipt.json
```

For a new combined qualification image, add the flag and replace the final
three path arguments with fresh paths:

```sh
  --include-opengl-cts \
  --stage-root /home/vm/uml-vulkan-rootfs/stage/rootfs-vulkan-zink-gl \
  --output /home/vm/uml-vulkan-rootfs/output-zink-gl/rootfs-vulkan-zink-gl.ext4 \
  --receipt /home/vm/uml-vulkan-rootfs/output-zink-gl/rootfs-vulkan-zink-gl.receipt.json
```

The sealer refuses existing output paths. It authenticates sources and stable
patch identities, independently regenerates every driver header from the
selected protocol source and SDK, checks the selected build's complete header
inventory and bytes, and records their digests. Mesa must explicitly select
that generated-header directory; it cannot fall back to its older vendored
codecs. The sealer synchronizes the selected offline Mesa build and byte-compares the
supplied runtime stage with a fresh runtime-only Meson install. It then audits
the CTS build's exact source/build directories and options, and reads each
CTS binary's embedded release back against the authenticated source revision.
This CTS check does not rebuild the executables. It audits dynamic dependencies,
extracts the EROFS base afresh, emits the guest receipt,
creates ext4 with `mkfs.ext4 -d`, runs a repairing check followed by a read-only
zero-error check, reads key bytes back with `debugfs`, and changes the image to
mode 0444. It never mounts or boots an image writable.

After direct transfer from the VM, verify both the receipt and image:

```sh
tools/rootfs/vulkan/verify-receipt.py \
  --image .build/rootfs/rootfs-vulkan-venus.ext4
```

## Relocatable guest runtime

The guest runtime layout differs between architectures only in its multiarch
library directory. `seal-in-vm.py` owns that single contract in
`architecture_profile()`: the driver inventory, install mapping, Meson and
Vulkan-Loader options, ICD identity, and audited runtime closure are shared,
and each supported Debian architecture names the same runtime in its own
library directory.

`stage-runtime.py` produces that runtime for one architecture as a relocatable
root plus its own receipt, with no CTS payload and no image identity. It runs
natively on the target architecture, reuses the sealer's source, protocol,
build, and stage verification, and records the runtime's dynamic-dependency
closure against a base root of the distribution the consuming image will use,
so an image's package set is known before the image exists.

```sh
sudo tools/rootfs/vulkan/stage-runtime.py --architecture amd64 \
  --work-root WORK --runtime-root WORK/stage/runtime-amd64 \
  --receipt WORK/output/guest-runtime-amd64.receipt.json \
  --closure-root WORK/closure/trixie-amd64
```

`install-runtime.py` imports that pair into an image root. It authenticates
every file against the receipt before the image is touched, recreates the
recorded symlinks, removes every other ICD manifest so a software driver
cannot mask a Venus failure, refuses an unresolved dynamic dependency, and
records what it installed in `/usr/share/uml-vulkan/guest-runtime.json`.

```sh
sudo tools/rootfs/vulkan/install-runtime.py --architecture amd64 \
  --source WORK/stage/runtime-amd64 \
  --receipt WORK/output/guest-runtime-amd64.receipt.json --root IMAGE_ROOT
```

The ARM64EC producer keeps `tools/rootfs/steam-arm64ec/install-vulkan.py`,
which imports the same runtime from the receipt-gated CTS image and is bound
to that image's accepted identity.
