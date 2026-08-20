# Android Common Kernel 6.18 source policy

This line starts from ACK tag `android17-6.18-2026-06_r35` at the exact commit
and tree recorded in [`patches/metadata.env`](patches/metadata.env). It is the
kernel source for the AOSP 17 guest only and does not replace either current
mainline page profile.

The checked series contains 23 ACK-based topic commits and resolves to the
exact final tree recorded in the metadata. The first 16 topics port the
applicable Darwin UML behavior; the final seven are ACK-only local compatibility
topics discovered by compiling the 16 KiB Android product. Mainline patch
bytes are never installed as this line's source of truth.

The Android producer is owned by
[`../../android/aosp-17/`](../../android/aosp-17/README.md). Its scripts execute
locally on an x86_64 Linux builder and receive the ACK Repo workspace as a local
path. This directory contains no builder endpoint or transfer workflow.

The product has exactly one 16 KiB configuration in
[`config/android.fragment`](config/android.fragment). Loadable modules are
forbidden: `CONFIG_MODULES=n`, Binder, ashmem compatibility, SELinux, audit,
DMA-BUF heaps, storage, networking, and guest devices resolve built-in, and no
`.ko` or DLKM output is accepted. Android userdata metadata encryption also
requires the built-in block inline-encryption core, its software fallback,
fscrypt inline hooks, and `dm-default-key`; UBD has no hardware crypto offload.
