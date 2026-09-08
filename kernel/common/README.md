# Shared kernel contracts

This directory contains only source-family-independent inputs:

- [`abi/`](abi/README.md): canonical plain-C host/kernel bridge, identity,
  generated-copy authority, and import policy;
- `config/page-16k.fragment`: shared 16 KiB page and 128 KiB task-stack
  geometry;
- `artifacts/`: compact prebuilt-kernel bundle schema and verifier; and
- `tooling/verify-series-policy.py`: parameterized generated-series policy.

Pinned commits, complete configurations, toolchains, source locations, patch
bytes, delta limits, and build commands stay in the owning versioned kernel
line. The common artifact verifier is local and read-only; it checks semantic
source/ABI identity, page and module policy, ELF geometry, payload inventory,
and exact bridge imports without requiring Kbuild or a source checkout.

Host-terminal console output preserves unread terminal input when changing
newline processing. Input and output may share one host PTY, so a kernel log
must not discard pending interactive commands or bootstrap payload bytes.
The standalone Darwin kernel build verifies this with the actual console
implementation and a native PTY assertion. This producer check takes explicit
source/build paths; Apple product configuration still consumes only bundles.
