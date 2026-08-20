# Kernel families

Kernel inputs are separated by source family and version. The current Apple
products use the mainline Linux 6.18 family, while the Android producer lane
has begun with a separately pinned ACK baseline:

- [`mainline-6.18/`](mainline-6.18/README.md) owns its source identity,
  generated patch series, complete configuration, build tools, and replay
  commands;
- [`common/`](common/README.md) owns contracts that are deliberately shared
  across kernel families; and
- [`android-6.18/`](android-6.18/README.md) owns the pinned ACK 6.18 identity
  and will own its independently generated Darwin UML series and one 16 KiB
  Android configuration; and
- [`CMakeLists.txt`](CMakeLists.txt) preserves the current Apple product graph
  by composing the mainline family explicitly.

The Android lane currently provides source preparation and preflight only. It
does not yet provide a Darwin UML ACK patch series, Android product/image, or
Apple runtime target. It must not reuse the mainline family's generated patch
bytes, source configuration, cache, or complete product policy.

The move from the former singleton layout does not change the current Linux
source, patch bytes, final tree, page profiles, kernel target names, or Apple
products. The old ignored `kernel/source-location.conf` remains untouched as
developer-owned data. Copy its value manually into the new ignored
`kernel/mainline-6.18/source-location.conf` when adopting this layout.

All maintained changes remain local to this project. Source repositories and
branch names are provenance only; the kernel tooling has no submission or
push workflow.

## Apple graphics ownership

The shared Apple ANGLE/Metal transition changes only the host renderer,
dependency, presentation, and packaging layers. The mainline 6.18 family
already builds the standard Virtio-GPU DRM/KMS and UML transport support used
by the immutable guest Mesa/libdrm stack, so this transition adds no Linux
topic, Kconfig change, host/kernel bridge declaration, or platform-specific
kernel image. macOS, iOS Simulator, and iOS device continue to consume the
same canonical 16 KiB or 4 KiB `vmlinux` for their selected page profile.

Host-visible blobs, a shared-memory aperture, and related mapping/coherency
work remain conditional optimization or Vulkan work. They must not be inferred
from the ANGLE/Metal provider switch or advertised before the standard guest
getparams, UML transport, host commands, reset lifetime, both page profiles,
and maintained generated kernel series have been implemented and verified.
