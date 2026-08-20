# Kernel families

Kernel inputs are separated by source family and version. The current Apple
products use the mainline Linux 6.18 family, while the Android producer lane
has begun with a separately pinned ACK baseline:

- [`mainline-6.18/`](mainline-6.18/README.md) owns its source identity,
  generated patch series, complete configuration, build tools, and replay
  commands;
- [`common/`](common/README.md) owns contracts that are deliberately shared
  across kernel families; and
- [`android-6.18/`](android-6.18/README.md) owns the pinned ACK 6.18 identity,
  independently generated Darwin UML series, and one built-in-only 16 KiB
  Android configuration; and
- [`CMakeLists.txt`](CMakeLists.txt) imports accepted local artifacts for Apple
  products without reading either source checkout or invoking Kbuild.

The Android producer and release support live under `android/aosp-17`; no AOSP
patch, image builder, or release tool is owned by the kernel directories. The
ACK line must not reuse the mainline family's generated patch bytes, source
configuration, cache, or complete product policy.

Standalone mainline source preparation still uses the ignored
`kernel/mainline-6.18/source-location.conf`. Product builds consume the compact
accepted bundles under `.build/kernel/artifacts`, selected by the checked
descriptors in `kernel/mainline-6.18/artifacts`; source and Kbuild state are not
part of the Apple graph.

All maintained changes remain local to this project. Source repositories and
branch names are provenance only; the kernel tooling has no submission or
push workflow.

## Apple graphics ownership

The shared Apple ANGLE/Metal provider transition changes the host renderer,
dependency, presentation, and packaging layers. The mainline 6.18 family
already builds the standard Virtio-GPU DRM/KMS and UML transport support used
by the immutable guest Mesa/libdrm stack. One later lifecycle correction is
carried as topic 0020: system shutdown retires atomic Virtio-GPU scanout state
before unplugging the DRM device so its last framebuffer GEM mapping cannot
outlive the VMA manager. This changes no Kconfig, host/kernel bridge
declaration, userspace ABI, or platform-specific kernel image. macOS, iOS
Simulator, and iOS device continue to consume the same canonical 16 KiB or
4 KiB `vmlinux` for their selected page profile.

Host-visible blobs, a shared-memory aperture, and related mapping/coherency
work remain conditional optimization or Vulkan work. They must not be inferred
from the ANGLE/Metal provider switch or advertised before the standard guest
getparams, UML transport, host commands, reset lifetime, both page profiles,
and maintained generated kernel series have been implemented and verified.
