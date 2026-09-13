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
allowance bounded between 1 GiB and 4 GiB, plus the fixed guarded aperture the
kernel reserves for negotiated vhost-user shared-memory regions and its stub
pages. The host and the kernel derive that reservation from the same constants,
so a host window smaller than the kernel's expectation faults on the kernel's
own stub write. macOS places the window at 96 TiB; iOS top-packs it below the
physical-device address ceiling.

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

The iOS device and iOS Simulator products build at an iOS 26.0 minimum. iOS
code therefore carries no availability guard, fallback path, or dependency
build for a release older than that, and every iOS dependency prefix and
platform contract records the same 26.0 deployment target. macOS keeps the
baseline each macOS product declares.

Apple products consume accepted kernel bundles and never invoke Kbuild. The
arm64 userspace product uses the same accepted 16 KiB or 4 KiB mainline
artifact on macOS and on the iOS device. The ARM64EC Wine appliance uses a
separate accepted 4 KiB arm64 profile on both. The macOS x86_64 userspace
product uses a separately identified 4 KiB mainline slice. Android uses a
separate accepted 16 KiB ACK artifact.

Kernarium is an iOS device product only. Its GPU stack renders through
KosmicKrisp, which requires Metal 4, and the iOS Simulator exposes no Metal 4
device, so the Simulator has no build of the application or of the Apple GPU
prefix. The unmodified iOS device slice runs on an Apple silicon Mac in
Designed for iPad mode, where it reaches the host GPU directly; that mode is
where iOS behaviour is verified when no physical device is attached. The
Simulator remains a target of the ordinary, non-GPU iOS libraries.

## CPU execution and scheduling

Darwin owns one persistent pthread per configured vCPU, including vCPU0.
Linux task contexts remain on guest kernel stacks and migrate between carriers;
guest task creation does not create host threads. CPU hotplug reuses carriers,
and shutdown returns to a native carrier stack before thread teardown.

Carriers are named `uml-vcpu-N-carrier`. The normal kernels use a 100 Hz tick;
the latency-oriented Wine profile uses 250 Hz. Both use `NO_HZ_IDLE`. Darwin
signals wake carriers but do not execute Linux IPIs asynchronously; IRQ-enabled
spin checkpoints service pending IPIs.

Early UML calibration uses process-directed `SIGALRM`. Application and launcher
threads keep it blocked, and only the vCPU0 carrier unmasks it until cooperative
deadlines take over. Kernarium runs the guest on its named `uml-launcher`
thread, never the main thread.

The interpreter is the reference oracle. The gadget engine executes arrays of
precompiled function pointers and data. The JIT emits native blocks through its
sole executable arena. Guest-visible exceptions, fault priority, retirement,
and feature exposure must agree across engines.

The macOS 4 KiB x86 compatibility slice keeps native arm64 UML kernel-mode
execution while exposing the x86_64 and i386 userspace ABIs. It cannot load
arm64 userspace; per-task mode selects only an x86-family operand and address
width. The host routes userspace instruction execution through immutable,
data-only streams of pointers to precompiled handwritten arm64 assembly
gadgets. There is no semantic C++ execution fallback. Exact x87 formats and
transcendentals use bounded software-math primitives called by those native
gadgets. The userspace ISA
boundary includes scalar x86 and x87/MMX, SSE through SSE4.2, AVX/AVX2,
F16C/FMA, AES/PCLMUL, BMI1/2, atomic and extended-state operations in both
long64 and compat32 modes. The pinned FEX handwritten x86-64 and i386 corpora
are the external instruction acceptance gate. AVX-512 and later encodings,
RTM/MPX, obsolete protected-mode bounds and far-pointer-load families, and
privileged, real-mode, virtual-machine, enclave, and system-management
instructions are outside this Linux userspace profile. That gate is not proof
of an arbitrary distribution or complete Linux syscall compatibility. The
normal mainline slices and Android remain arm64-userspace-only and contain
none of the x86_64 kernel overlay.

The separate ARM64EC Wine product also keeps an arm64 Linux kernel and an
ARM64-only Debian/XFCE rootfs, but embeds the x86 gadget engine as a private CPU
service. Wine's ARM64EC `xtajit64.dll` transition module issues a reserved arm64
breakpoint carrying an x86-64 register context. Its native ARM64 `xtajit.dll`
WoW64 module uses the same service with an i386 context and returns system and
Unix calls to native ARM64 Wine. The runtime consumes both calls before Linux
signal delivery, runs x86 instructions against the same guest address space,
and uses Wine's EC-code bitmap to return x86-64 execution directly to native
ARM64EC/ARM64X code. This target places a conservative inner-shareable barrier
at every x86 retirement and at native/x86 boundaries; both bridges refuse the
service unless that strict-ordering capability is advertised. Linux continues
to own processes, filesystems, memory, devices, and windowing; the i386 Wine
modules are PE-only, and no x86 Linux ABI, second kernel, or nested emulator is
present. The dedicated
`uml_wine_arm64ec_emulator` archive, `uml-darwin-wine-arm64ec` macOS executable,
and `KRuntime_Wine_6_18_4K` iOS framework keep it distinct from ordinary 4 KiB
products.

The JIT uses a process budget with a bounded private working set per executor
and a process-global shared translation tier. Shared entries contain immutable
guest identity and relocatable code only; executor-owned link state is rebuilt
when materialized into writable data outside executable mappings. Links and
retained activation contexts are scoped to an address space. A managed run
owns at most one persistent executor per guest CPU, and no executor survives
its runtime memory domain. Cache and publication contracts are described in
[`../darwin/README.md`](../darwin/README.md).

The gadget cache may retain several address spaces. Cache slots and links are
tagged by address space, retirement drops one address space's owned state, and
same-generation stale programs are reused only after exact page-bounded word
validation. Shared gadget translations remain data-only and relocate every
executor-owned link cell before use. Cached returns consult a private
per-carrier return-record ring after their site cells miss: linked calls push
the return PC with its block link, a matching record follows that link and
trains the cell, and the executor empties the ring on every address-space
switch, so a stale record can only fall back to the ordinary miss exit.
The software TLB keys its entries by guest page number. A same-page transfer
compares the page of its final byte against the way its first byte indexes: the
16 KiB direct-mapped index never places neighbouring pages in one set, so a
crossing transfer can only miss and is recognized on the cold path, while the
folded 4 KiB index keeps the explicit crossing check ahead of the lookup.

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
opens one directly writable supplied GPT and has no 4 KiB variant.

A Kernarium `.kern` bundle contains one guest's persistent rootfs payload.
Android bundles provide a kernel command line and one directly writable GPT
image; a release
manifest may accompany them as provenance. The kernel comes from the selected
runtime framework. Launch does not compare kernel, runtime, or rootfs release
identities; missing or malformed inputs fail at their ordinary file or image
boundary. The app's `DiskUtil` owns raw-file capacity before launch and
dispatches any format-specific partition-table update. Android GPT growth uses
the verified static GPT fdisk core linked into the app; the standalone macOS
launcher copies a changed release to its persistent 8 GiB disk and uses
Homebrew `sgdisk`. Runtime frameworks receive no capacity policy
and reject a GPT whose declared extent differs from its file. The guest only
grows a filesystem after observing that its containing partition is larger.
This keeps the same host-capacity workflow available to later Linux image
formats without putting disk policy back into a kernel runtime.

Compressed rootfs delivery is also an app boundary. `ZstdUtil` streams a zstd
source into an adjacent temporary file, optionally represents zero regions as
sparse holes, and publishes the destination only after a complete decode.
Runtime frameworks receive only the resulting ordinary disk image.

## Devices and graphics

The vhost-user transport owns connection, queue framing, and deferred used-ring
publication. GPU, input, sound, console, battery, and network providers own
their protocol behavior independently.

The Apple graphics path is:

```text
Linux virtio-gpu -> vhost-user -> virglrenderer -> Mesa Zink on KosmicKrisp
    -> shared Metal presentation -> AppKit or UIKit
```

GPU commands and rendering stay off the UI thread. The shared presentation
layer keeps the newest frame and at most one command buffer in flight. A
backpressured presentation may drop a superseded frame; only unsupported
resource shapes use CPU readback. See [`graphics.md`](graphics.md).

The separately configured macOS Vulkan experiment adds an independent route:

```text
Linux Vulkan -> Mesa Venus -> virglrenderer capset 4
    -> directly linked Mesa KosmicKrisp -> Metal
```

That route exists only in the verified `apple-gpu-vulkan-experiment` product
and is enabled at runtime only by `UML_GPU_EXPERIMENTAL_VULKAN=1`. It does not
replace or enter the GL facade, and no iOS runtime or application target
contains the experimental provider. Its fences ride the same deferred
fence pipeline the default path uses: the device creates a context fence per
pipelined request, and the transport retires it from the queue-worker timer.
Venus gives each guest ring its own timeline, so a Venus context is polled
individually while one poll still drains every Virgl context. Software 2D
console transfers and flushes can progress while renderer fences are pending;
otherwise a console command can block a later host signal needed by that fence.
Renderer-resource flushes and destructive resource operations retain their
fence ordering.

A fence the renderer does not retire never costs the guest its GPU device.
Because a held-back guest command can itself carry the signal a pending fence
waits for, the first expiry releases that ordering and grants the fence a
second window; only a second expiry retires the fence without its renderer
work. Each pending fence carries its own escape state, so fences that expire
together each get the release step and the second window rather than being
retired because another fence already relaxed ordering for the connection.
Both transitions report one line naming the context, ring, and command.
The transport keeps serving the connection either way, so a wedged submission
fails the workload that owns it while the guest stays able to finish its
remaining work and power off.

The experiment keeps shareable Zink scanouts in their renderer-optimal Metal
layout. An unlinked vnode-backed identity descriptor reconnects each guest
dma-buf with its in-process MTLHeap and native MTLTexture; the descriptor never
stores pixel data. Native presentation samples that texture directly. The VNC
consumer instead performs a damage-bounded asynchronous Metal blit into one
shared staging buffer, keeps at most one newer pending frame, and publishes to
LibVNCServer only after GPU completion, so neither readback nor encoding blocks
the guest flush timeline.

Kernarium exposes an absolute touchscreen, keyboard, one 48 kHz stereo
Virtio-Sound playback stream, SLiRP networking, a Metal framebuffer, and a
serial terminal view. Android additionally receives the Apple host's battery
percentage and charging state through a dedicated Virtio device, falling back
to 100 percent and charging when the host has no readable battery. Point
mode maps safe-area points to guest pixels at 160 DPI; Retina mode multiplies
both size and density by `nativeScale`. Android's logical density is derived
from the same launch value.

Writable host files are tracked by the platform layer. Emulator sync asks the
guest to write back through the management console and then flushes host files.
Quiesce enters full Linux system suspend and waits for the platform sleep
boundary before flushing host files; resume sends the console wake edge.
Accepted kernels without system suspend retain a stop/go fallback. Guests
receive unique run identities so management-console paths do not collide.

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
