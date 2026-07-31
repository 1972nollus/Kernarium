# Darwin UML runtime

This directory contains the Apple host side of the arm64 Linux 6.18 UML port.
Linux architecture contracts stay under `linux-6.18/arch/um` and
`linux-6.18/arch/arm64/um`; the C++ frontend, backend, static CPU, MMUs, and
host devices live here. The macOS release is one kernel/runtime executable plus
a separate writable ext2 image named `rootfs.ext2`.

## Source layout

- `frontend/macos/` is the serial-console CLI. It finds the external rootfs and
  calls only the public backend API.
- `backend/include/` is the stable C ABI. `backend/src/` owns backend lifecycle,
  native-kernel entry, and platform capabilities.
- `backend/cpu/arm64/` ports iSH Asbestos's precompiled ARM64 gadget model.
  Gadget runtime programs contain only precompiled gadget addresses and guest
  data.
- `backend/runtime/jit/` contains the optional Apple ARM64 native JIT, its
  fixed-size block cache, direct instruction emitters, and inline software-TLB
  lookup.
- `backend/runtime/static/` contains the guest executor, 8,192-entry TLB,
  data-only block cache, and optional differential verification.
- `runtime/mmu/address_space.cpp` models independent build-selected userspace
  pages. `native_address_space.cpp` is compiled only for the optional 4 KiB
  compatibility build, where it assembles native-kernel mappings inside
  Darwin's 16 KiB VM slots.
- `network/device.cpp` is the portable Ethernet transport boundary;
  `network/slirp/` is the macOS CLI's unprivileged NAT implementation.
- `loader/` contains the macOS compatibility ELF-kernel loader. It is excluded
  from the sandbox-safe XCFramework slices.
- `probes/` and `backend/tests/` contain host contracts and regression tests.

Project-owned host code is C++ or assembly and is built by CMake. CTest's
`source-tree-cxx-only` audit rejects `.c` sources and raw `Makefile` files
outside the Linux tree and generated build directories.

## Build and run

```sh
cmake -S . -B .build/cmake-macos -G Ninja \
  -DUML_APPLE_PLATFORM=macos \
  -DUML_BUILD_CLI=ON \
  -DUML_BUILD_KERNEL_RUNNER=ON
cmake --build .build/cmake-macos --parallel 8
ctest --test-dir .build/cmake-macos --output-on-failure
.build/cmake-macos/bin/uml-darwin
```

`UML_GUEST_16K_PAGES=ON` is the default. It configures the UML kernel for
16 KiB pages and specializes the runtime, TLB, and gadgets for the same page
geometry. To build the compatibility configuration instead, pass
`-DUML_GUEST_16K_PAGES=OFF`; this selects 4 KiB kernel pages and restores the
Darwin native-shadow mapper required by that layout.

The launcher looks for a writable `rootfs.ext2` beside the executable, then in
the current directory, or at `UML_ROOTFS`. Unless overridden, it adds
`ubd0=... root=/dev/ubda init=/sbin/init mem=256M ip=dhcp rw`. The default
SLiRP provider answers the kernel's DHCP request, so `eth0` does not depend on
a guest network service. Supplying an explicit `ip=` argument preserves the
caller's network configuration. The rootfs is never copied into or embedded
in the executable.

After changing built-in kernel drivers, refresh the external image's text-only
driver inventory with `cmake --build .build/cmake-macos --target
uml_rootfs_kernel_metadata`. This installs `modules.builtin` and an empty
`modules.dep` for userspace driver discovery; it never installs loadable code,
and `CONFIG_MODULES` remains disabled.

Build the static backend for arm64 macOS, arm64 iOS, and arm64 iOS Simulator:

```sh
scripts/build-xcframework.sh
```

The result is `.build/xcframework/UMLBackend.xcframework`. Every slice contains
the static gadget executor, build-selected userspace MMU, POSIX host bridge,
and callback-backed Ethernet device. A 4 KiB build also contains the
native-kernel shadow MMU. It contains no ELF loader, Capstone, Mach exception
server, libslirp, or writable-executable code path.

## Native kernel and sandbox boundary

The macOS CLI embeds the Linux UML ELF in its `__DATA,__uml_elf` section and
uses the compatibility loader. The bytes are part of the executable and no
code is synthesized, but this path is intentionally reported as not sandbox
safe because it must establish executable ELF mappings.

Sandboxed apps instead set `uml_backend_config.native_kernel_entry` to an
ahead-of-time Mach-O-linked UML kernel entry. The backend invokes that function
directly, so all executable code is already code-signed in the containing app.
`uml_backend_get_capabilities()` then reports `can_run=1`,
`kernel_is_native_macho=1`, and `sandbox_safe=1`. The optional
`uml_backend_network_callbacks` table supplies an fd-backed packet transport;
the kernel's normal `eth0` driver remains unchanged. The macOS CLI selects its
built-in SLiRP provider when no callbacks are supplied.

## Execution model

The UML kernel executes natively as arm64 code. All Linux userspace processes
remain inside that same host process; Linux task switches select an MMU
generation and execute the task on Darwin pthreads. Select the userspace engine
with `--execution-engine=jit`, `--execution-engine=gadgets`, or
`--execution-engine=interpreter`. macOS defaults to JIT, while iOS defaults to
gadgets. The interpreter is a correctness/debug path.

The gadget engine translates each 4-byte instruction to a data-only program of
fixed precompiled gadgets. The JIT engine places generated ARM64 blocks in a
`MAP_JIT` arena and directly emits covered Base/FP/SIMD operations. It never
falls back to gadget execution: an instruction without a verified native
emitter is a fatal JIT event. Guest memory operations still use the software MMU;
guest virtual addresses are never used as unchecked host pointers. `svc #0`,
signal return, faults, and interrupts return structured events to the native
kernel.

Guest `AT_HWCAP`, `AT_HWCAP2`, and `/proc/cpuinfo` are derived from Darwin host
features. Optional crypto and direct JIT instruction families are advertised
only when the host supports them, preventing compliant guest runtimes from
generating unsupported native instructions.

`UML_STATIC_TRACE=1` prints a bounded block trace.
`UML_STATIC_VERIFY_GADGETS=1` checks each gadget block against the fixed decoder
without creating executable memory. `UML_MMU_TRACE=1` traces native-kernel
mapping operations.

## Guest page MMU

The default Linux configuration uses 16 KiB pages, matching Apple Silicon's
host VM page size. Userspace accesses still go through the data MMU and its
16 KiB TLB, so no guest executable page needs to be host executable. Physical
aliases map the shared backing directly; shadow synchronization, quarter-page
reconciliation, and their syscall-boundary work are excluded at build time.
Each Linux `mm` has a generation, so fork, COW, unmap, and task switches keep
independent mappings inside one host address space.

The optional 4 KiB compatibility configuration retains the native shadow MMU.
Host-aligned runs map directly from the reserved physical-memory backing, while
unaligned 4 KiB pages are reconstructed as quarters of anonymous 16 KiB slots.
Each quarter stores an original snapshot: writeback occurs only after a byte
change, and is discarded if an authoritative physical alias changed in the
meantime. Removing an absent quarter is a successful no-op and cannot
accidentally unmap a live sibling.

## Networking

The in-kernel `eth0` module exchanges complete Ethernet frames through four
host callbacks: open, IRQ fd, read, and write. The macOS provider uses libslirp
for unprivileged NAT with gateway `10.0.2.2`, DNS `10.0.2.3`, and guest address
`10.0.2.15/24`. The kernel obtains that configuration from libslirp's DHCP
server before starting userspace. Its poll worker wakes the UML idle thread
through an fd and is joined when the backend run ends. `UML_DISABLE_NETWORK=1`
disables this default provider and its automatic `ip=dhcp` argument for
diagnostics.

`CONFIG_VIRTIO_UML` can expose a vhost-user virtio-net device, but it adds ring
and protocol overhead in front of the same fd wake path. Profiling showed the
old multi-second latency came from wake/timer handling, not frame transport, so
the direct device remains the default.

## Measured performance

On the development Apple Silicon host, repeated final macOS boots reached
`/sbin/init` and the login prompt in 0.22--0.48 seconds. Root
`neofetch --stdout` runs completed in 2.41--2.53 seconds. Local SLiRP gateway
checks measured 0.24--0.36 ms average latency with no packet loss. These are
end-to-end guest measurements, not targeted command shortcuts.
