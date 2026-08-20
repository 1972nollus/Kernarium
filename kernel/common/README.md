# Shared kernel contracts

This directory contains only inputs whose meaning is independent of a kernel
source family:

- [`abi/`](abi/README.md) is the canonical plain-C Darwin host/kernel bridge,
  declaration identity, generated-copy authority, and import policy;
- [`config/page-16k.fragment`](config/page-16k.fragment) defines the shared
  16 KiB page and 128 KiB task-stack geometry; and
- [`tooling/verify-series-policy.py`](tooling/verify-series-policy.py) verifies
  the explicitly selected versioned line passed through `--line-root`.

Pinned commits and trees, generated patch bytes, complete product
configurations, compiler policy, source locations, caches, delta ceilings, and
build commands stay in their versioned kernel line. A new line may consume a
common contract only after verifying that contract against its own metadata
and replayed source.
