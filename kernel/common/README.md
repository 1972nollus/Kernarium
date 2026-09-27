# Shared kernel contracts

This directory contains only source-family-independent inputs:

- [`abi/`](abi/README.md): canonical plain-C host/kernel bridge, identity,
  generated-copy authority, and import policy;
- `config/page-16k.fragment`: shared 16 KiB page and 128 KiB task-stack
  geometry;
- `artifacts/`: compact prebuilt-kernel bundle schema and verifier;
- `patches/`: the generated topic series every maintained line applies
  first, regenerated from the mainline replay and applied unchanged to each
  line's own upstream base; and
- `tooling/kernel_series.py`: replay, verification, and regeneration of a
  line's ordered generated series.

Pinned commits, complete configurations, toolchains, source locations, patch
bytes, and build commands stay in the owning versioned kernel line. The common artifact verifier is local and read-only; it checks semantic
source/ABI identity, page and module policy, ELF geometry, payload inventory,
and exact bridge imports without requiring Kbuild or a source checkout.

Host-terminal console output preserves unread terminal input when changing
newline processing. Input and output may share one host PTY, so a kernel log
must not discard pending interactive commands or bootstrap payload bytes.
The standalone Darwin kernel build verifies this with the actual console
implementation and a native PTY assertion. This producer check takes explicit
source/build paths; Apple product configuration still consumes only bundles.

Darwin's `sigprocmask()` sets the mask of every thread in the process, vCPU
carriers included, so host-side kernel code changes a signal mask only with
`pthread_sigmask()`. Every kernel build rejects an image that imports
`sigprocmask`, and the console PTY assertion checks that a console write
leaves another thread's mask unchanged.

Both maintained kernel lines perform futex updates atomically against native
userspace running on other vCPUs. Compare-and-exchange reports the value
observed by that operation, and wake-operation arithmetic retries competing
updates instead of overwriting them. The optional `guest-futex-atomic` test
checks concurrent kernel/userspace updates and robust priority-inheritance
mutexes on both Linux page profiles.
