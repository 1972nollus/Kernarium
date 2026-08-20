# Shared kernel contracts

This directory contains only inputs whose meaning is independent of a kernel
source family:

- [`abi/`](abi/README.md) is the canonical plain-C Darwin host/kernel bridge,
  declaration identity, generated-copy authority, and import policy;
- [`config/page-16k.fragment`](config/page-16k.fragment) defines the shared
  16 KiB page and 128 KiB task-stack geometry; and
- [`tooling/verify-series-policy.py`](tooling/verify-series-policy.py) verifies
  the explicitly selected versioned line passed through `--line-root`; and
- [`artifacts/`](artifacts/) defines and verifies the compact prebuilt-kernel
  bundle consumed by Apple products after a versioned standalone build.

Pinned commits and trees, generated patch bytes, complete product
configurations, compiler policy, source locations, caches, delta ceilings, and
build commands stay in their versioned kernel line. A new line may consume a
common contract only after verifying that contract against its own metadata
and replayed source.

The artifact verifier is local and read-only. It checks the tracked source and
ABI identities, fixed payload inventory, resolved page-size/module policy,
AArch64 ET_DYN geometry, W^X/stack properties, and exact bridge imports without
requiring a kernel checkout or Kbuild toolchain.
