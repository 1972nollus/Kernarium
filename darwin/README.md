# Darwin UML host runtime

The host implementation is organized by ownership:

```text
api/       public API contract
platform/  Apple capability and platform primitives
memory/    guest address-space, MMU, and TLB contracts
cpu/       interpreter, gadgets, JIT, and the neutral engine factory
runtime/   guest-entry adapter and runtime composition
device/    network, VNC input, GPU, and provider implementations
emulator/  options and product composition
app/macos/ CLI and embedded-kernel process adapter
```

Each guest vCPU runtime owns its engine adapters for the life of its host
thread. `CpuEngine::run()` is a synchronous, non-throwing-by-contract guest
timeslice boundary: implementations return `ExecutionResult` for guest exits;
allocation or host failures are reported as `fatal`. The virtual call occurs
once at the guest-entry boundary, never in an interpreter step or translated
block loop. Gadgets and JIT retain independent caches and do not fall back to
one another.

The CLI requires a replayed Linux source and embeds its Kbuild `vmlinux`.
`UML_GUEST_16K_PAGES=ON` is the default; set it to `OFF` for the independently
tested 4 KiB compatibility build.

Guest integration uses the unified raw EROFS image `rootfs.erofs` at
`/dev/ubda`, with `rootfstype=erofs`. Shared guest campaigns map it with
`ubd0c=` to avoid a conflicting host-file lock. EROFS supplies the read-only
lower filesystem; workloads that need writable state use a tmpfs OverlayFS.
The image owns tty0 getty enablement and login policy; host-fed OverlayFS
launchers preserve it rather than installing a transient getty override, and
mount tmpfs at `/var/lib/docker` before systemd to avoid nested OverlayFS. No
KDE/VNC or WebGL launcher creates a desktop account: both reuse the image's
non-root `uml` user and never synthesize `plasma`. No Unix-mode or read-only
block-argument policy is needed. See
[`../docs/testing.md`](../docs/testing.md) for preparation and run commands.
