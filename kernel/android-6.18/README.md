# Android Common Kernel 6.18 source policy

This line starts from ACK tag `android17-6.18-2026-06_r35` at the exact commit
and tree recorded in [`patches/metadata.env`](patches/metadata.env). It is the
kernel source for the AOSP 17 guest only and does not replace either current
mainline page profile.

The initial checked state records the pristine ACK base with an empty local
series. Darwin UML behavior will be ported as ACK-based topic commits and
regenerated here; mainline patch bytes are never installed as this line's
source of truth.

The Android producer is owned by
[`../../android/aosp-17/`](../../android/aosp-17/README.md). Its scripts execute
locally on an x86_64 Linux builder and receive the ACK Repo workspace as a local
path. This directory contains no builder endpoint or transfer workflow.

The eventual product has exactly one 16 KiB configuration. Loadable modules are
forbidden: `CONFIG_MODULES=n`, every required feature is linked into `vmlinux`,
and no `.ko` or DLKM output is accepted.
