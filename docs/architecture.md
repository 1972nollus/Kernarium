# Architecture

## Dependency direction

The enforced first-party dependency direction is:

```text
app -> emulator / device
emulator -> runtime / device / api
runtime -> cpu / memory / device contracts / platform
cpu -> memory / platform
memory -> platform
device -> memory / platform
```

`darwin/CMakeLists.txt` composes owning subdirectories. Each module owns its
targets and source manifest. The neutral CPU factory selects the interpreter,
gadget, or JIT adapter; engines do not depend on one another.

The assembly-visible CPU and TLB layout is owned by
`darwin/cpu/arm64/layout/assembly_offsets.h` and checked from C++ and assembly.
Test-only hooks belong in test/support targets, not production libraries.

## Runtime ownership

Each `uml_emulator` handle owns its runtime context, engine selection, stop
state, and copied network callback table. A handle admits at most one run at a
time. Stop requests affect only that handle's current or queued run and are not
carried into later runs.

Different 16 KiB native-callback handles may run concurrently when their host
mappings do not overlap. Embedded kernels and 4 KiB native-callback runs use a
process address-space gate because their fixed host mappings cannot be safely
shared. Ambiguous unbound callbacks fail closed rather than borrowing another
handle's context.

The caller owns provider lifetimes. The macOS app therefore creates and
destroys SLiRP around emulator use; the library has no process-global network
provider.

## Memory and executable code

All products use a 128 TiB sparse software-MMU guest task range. The default
guest page profile is 16 KiB; 4 KiB is a supported compatibility build. The
native kernel window contains guest RAM plus a RAM-scaled auxiliary mapping
allowance bounded between 1 GiB and 4 GiB. macOS places it at 96 TiB; iOS
top-packs it below the physical-device address ceiling.

Every Darwin UML kernel uses its linear physical window as the anonymous RAM
root. Software-MMU and host-service consumers borrow that root or create
non-copying Mach aliases; no separate full-span guest-RAM mapping is retained.
Physical iOS wires all guest-allocatable pages before publishing them, while
macOS and Simulator retain demand allocation.

Interpreter state, gadget streams, MMU backing, and shadow mappings are data.
Only `darwin/cpu/arm64/jit/jit_arena.cpp` may allocate executable memory,
publish RW pages as RX, control JIT write protection, or publish the instruction
cache.

Ordinary iOS libraries exclude native-VM and embedded-ELF execution. Kernarium
uses versioned runtime frameworks whose kernel ELF is carried in signed Mach-O
`__TEXT`. Its loader aliases immutable executable pages from that mapping and
rejects writable executable segments or relocations into signed text. The app
embeds every selectable framework, links none at launch, and loads exactly one
guest flavour/page profile for the process lifetime. macOS retains its
unsigned embedded-loader path.

Apple products consume accepted kernel bundles and never invoke Kbuild. Linux
uses the same accepted 16 KiB or 4 KiB mainline artifact on macOS, Simulator,
and device. Android uses a separate accepted 16 KiB ACK artifact.

## CPU execution and scheduling

Darwin owns one persistent pthread per configured vCPU, including vCPU0.
Linux task contexts remain on guest kernel stacks and migrate between carriers;
guest task creation does not create host threads. CPU hotplug reuses carriers,
and shutdown returns to a native carrier stack before thread teardown.

Carriers are named `uml-vcpu-N-carrier`. The shipped kernels use a 100 Hz tick
with `NO_HZ_IDLE`. Darwin signals wake carriers but do not execute Linux IPIs
asynchronously; IRQ-enabled spin checkpoints service pending IPIs.

Early UML calibration uses process-directed `SIGALRM`. Application and launcher
threads keep it blocked, and only the vCPU0 carrier unmasks it until cooperative
deadlines take over. Kernarium runs the guest on its named `uml-launcher`
thread, never the main thread.

The interpreter is the reference oracle. The gadget engine executes arrays of
precompiled function pointers and data. The JIT emits native blocks through its
sole executable arena. Guest-visible exceptions, fault priority, retirement,
and feature exposure must agree across engines.

The JIT uses a process budget with a bounded private working set per executor
and a process-global shared translation tier. Shared entries contain immutable
guest identity and relocatable code only; executor-owned link state is rebuilt
when materialized. A managed run owns at most one persistent executor per guest
CPU, and no executor survives its runtime memory domain.

The gadget cache may retain several address spaces. Cache slots and links are
tagged by address space, retirement drops one address space's owned state, and
same-generation stale programs are reused only after exact page-bounded word
validation. Shared gadget translations remain data-only and relocate every
executor-owned link cell before use.

## Kernel and Android ownership

`kernel/mainline-6.18` and `kernel/android-6.18` independently own their pinned
source identity, configuration, generated patch series, and standalone build
policy. `kernel/common` owns only contracts shared across families. The Apple
CMake graph imports compact artifacts and checks their bridge ABI, page
profile, product family, and `6.18` release series. Patch releases, commit
identities, and producer provenance do not become runtime compatibility keys.

`android/aosp-17` owns the Android product, static GPT layout, ramdisks, rootfs
release, and producer tools. Kernel and rootfs releases are selected
independently. Apple emulator builds consume only the ACK ELF artifact; rootfs
and command-line paths are launch inputs. The `uml-darwin-android17` product
reads the supplied GPT through a per-instance COW and has no 4 KiB variant.

A Kernarium `.kern` bundle contains one guest's rootfs payload and persistent
COW. Android bundles provide a kernel command line and one GPT image; a release
manifest may accompany them as provenance. The kernel comes from the selected
runtime framework. Launch does not compare kernel, runtime, or rootfs release
identities; missing or malformed inputs fail at their ordinary file or image
boundary.

## Devices and graphics

The vhost-user transport owns connection, queue framing, and deferred used-ring
publication. GPU, input, sound, console, and network providers own their
protocol behavior independently.

The Apple graphics path is:

```text
Linux virtio-gpu -> vhost-user -> virglrenderer -> ANGLE/EGL on Metal
    -> shared Metal presentation -> AppKit or UIKit
```

GPU commands and rendering stay off the UI thread. The shared presentation
layer keeps the newest frame and at most one command buffer in flight. A
backpressured presentation may drop a superseded frame; only unsupported
resource shapes use CPU readback. See [`graphics.md`](graphics.md).

Kernarium exposes an absolute touchscreen, keyboard, one 48 kHz stereo
Virtio-Sound playback stream, SLiRP networking, a Metal framebuffer, and a
serial terminal view. Point mode maps safe-area points to guest pixels at 160
DPI; Retina mode multiplies both size and density by `nativeScale`. Android's
logical density is derived from the same launch value.

Writable host files are tracked by the platform layer. Emulator sync asks the
guest to write back through the management console and then flushes host files;
quiesce also freezes the guest, and resume thaws it. Guests receive unique run
identities so management-console paths do not collide.

## Tests and structural audit

Every first-party CTest has one primary suite: `unit`, `tooling`, or
`integration`. `tests/test-manifest.tsv` is the exact registration and owner
inventory.

`architecture-audit` checks the CMake File API graph, dependency cycles,
implementation includes, source ownership, test manifests, and advisory
source-size metadata. It does not prove performance, generated code shape, a
kernel boot, or guest behavior; those require their owning validation.

Production files above 800 lines, test/support files above 600, and CMake files
above 300 are reported as advisory review points; they are not acceptance
failures or a request for a dated review ledger.

The host/kernel ABI source is [`../kernel/common/abi`](../kernel/common/abi/).
Version, fingerprint, generated-copy, layout, and import checks fail closed on
drift.
