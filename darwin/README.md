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

Guest integration always uses the immutable GPT image `rootfs.img`, partition
`/dev/ubda1`, and `ubd0r=... ro`. The temporary writable root is a tmpfs
OverlayFS. The CLI rejects writable root mappings; no normal CMake target or
test modifies the image. See [`../docs/testing.md`](../docs/testing.md) for
preparation and run commands.
