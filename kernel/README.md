# Kernel families

Kernel inputs are scoped by source family and version:

- [`mainline-6.18/`](mainline-6.18/README.md) owns the maintained stable Linux
  source identity, generated Darwin patch series, complete configuration,
  standalone build, and accepted 16 KiB/4 KiB publications.
- [`android-6.18/`](android-6.18/README.md) owns the pinned ACK identity,
  generated Darwin series, complete built-in-only 16 KiB configuration, and
  accepted Android kernel publication.
- [`common/`](common/README.md) owns only contracts deliberately shared between
  families.
- [`CMakeLists.txt`](CMakeLists.txt) imports accepted local artifacts for Apple
  products without reading a source checkout or invoking Kbuild.

The AOSP product and rootfs release are independently owned by
[`../android/aosp-17`](../android/aosp-17/README.md). Kernel and rootfs release
selection must remain independent.

Maintained kernel changes are local to this project. Generated patch series,
not replay checkouts, are the outer repository's source of truth.
