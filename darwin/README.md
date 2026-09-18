# Darwin UML host runtime

## Ownership

```text
api/       installed C API and ABI identity
platform/  Apple capabilities, VM, files, signals, and sockets
memory/    guest address spaces, software MMU, and TLBs
cpu/       gadgets, JIT, shared decode, and neutral engine selection
runtime/   guest-entry composition
device/    block, console, GPU, input, audio, and network providers
emulator/  options and product composition
app/macos/ standalone process adapter
```

Targets and source lists belong to these directories. See
[`../docs/architecture.md`](../docs/architecture.md) for dependency direction.

## Products and defaults

The standalone macOS application supports the normal arm64-userspace Linux
kernel with 16 KiB or 4 KiB guest pages and the separate x86_64-userspace
kernel with 4 KiB pages. When omitted, `mem=` defaults to 8 GiB and `ncpus=`
to 10; explicit kernel arguments override either value independently.
`cmake --preset macos-x86_64` selects the dedicated x86_64 artifact and host
gadget backend; ordinary 4 KiB presets continue to select arm64 userspace. The
x86_64 product defaults to `cgroup.memory=nokmem`: userspace memory-controller
accounting remains available, while kernel-object charging is outside this
compatibility slice. An explicit `cgroup.memory=` argument overrides the
default.

Kernarium uses `6.18`-series iOS runtime frameworks for 16 KiB Linux, 4 KiB
Linux, and 16 KiB Android. Framework names remain stable across patch releases
and kernel commits. It defaults to 16 KiB Linux, 4 GiB on Simulator, 2 GiB on
physical devices, and all processors reported by `NSProcessInfo`. It loads one
runtime for the process lifetime.

The Android macOS composition is a separate product. Its build consumes a
compatible ACK kernel artifact; launch accepts a rootfs directory and
one directly writable GPT image without comparing rootfs release identity, and
it does not search for the Linux desktop image.

## Runtime and scheduling

Each public emulator handle owns its context, engine, stop state, and network
callbacks. One handle admits one run. Native 16 KiB callback runs may coexist
when mappings do not overlap; embedded and 4 KiB runs serialize through the
process address-space gate.

Every configured guest CPU has one persistent `uml-vcpu-N-carrier` pthread.
Linux tasks migrate on guest stacks rather than creating host threads. Host
signals wake carriers, and pending IPIs run only at IRQ-enabled checkpoints.
The early process-directed `SIGALRM` calibration signal remains blocked on
application and launcher threads and is temporarily owned by vCPU0.

Kernarium starts the synchronous guest run on the named `uml-launcher` thread.
The main thread remains available for UIKit and MetalKit.

## Guest memory and kernel loading

All products use the same 128 TiB sparse software-MMU task range. The native
kernel window contains guest RAM plus a page-rounded auxiliary allowance equal
to RAM, bounded from 1 GiB to 4 GiB. macOS places the window at 96 TiB. iOS
asks for the address below its ceiling and, when the host declines that hint,
for wherever a window of that size fits: an iOS app's usable virtual range is
not guaranteed to contain the compile-time envelope, and iOS 27 moved it out.
The window is reserved before the guest starts so nothing else can take part of
it, with only its first pages released for the kernel image and the reserved
physical prefix, which their loaders place by hint.

Accepted arm64-userspace mainline artifacts are shared by macOS and iOS within
each page profile. The x86_64-userspace 4 KiB slice has a distinct artifact
identity and is selected only by the macOS product. The Android product uses
its independent accepted 16 KiB ACK artifact. Apple CMake validates artifact
manifests and never invokes Kbuild.

Kernarium carries each kernel ELF in a signed framework `__TEXT` section. The
loader aliases immutable executable pages from that mapping and copies only
data/BSS into anonymous storage. It rejects carrier mismatch, writable
executable segments, and relocations into signed text. macOS retains the
unsigned embedded-loader path.

Darwin UML guest RAM uses the kernel's linear physical window as its root. The
signed kernel image occupies the reserved prefix and the allocatable suffix is
created anonymously in place; software-MMU and host-service views borrow that
window rather than retaining a second full-span mapping. This applies to both
mainline page profiles and the Android kernel. Physical iOS commits and wires
the anonymous suffix, the loader-to-reserve tail, and any prefix pages as the
kernel releases them into guest RAM; a wire failure aborts startup. Simulator
and macOS retain demand allocation. Disk and image descriptors remain
file-backed.

## CPU engines

Engine selection is explicit. Gadgets execute non-executable data streams of
precompiled entries, and the JIT publishes native code only through
`cpu/arm64/jit/jit_arena.cpp`. The JIT owns its semantic slow path: generated
code routes a fixed set of instruction classes to helpers that decode only
those classes, and any other word is a fatal internal error. The arm64
`Dispatcher` decodes and executes single instructions; it is not a selectable
engine, and only unit tests that call it directly exercise it.

The macOS x86_64 slice always uses the x86_64 gadget backend for userspace
while UML kernel-mode execution remains arm64. The kernel slice cannot load
arm64 userspace and does not switch ISA per task. Integer, vector, x87/MMX,
TLS, atomic, and extended-state execution is qualified in both long64 and
compat32 modes against the pinned FEX handwritten ASM corpora. The ordinary
userspace feature profile covers scalar x86, x87, MMX, SSE through SSE4.2,
AVX, AVX2, F16C, FMA, AES/PCLMUL, BMI1/2, and their 32-bit operand/address
forms; CPUID advertises that profile, ERMS included: a repeated string
operation moves whole page-bounded blocks, so the REP MOVSB that ERMS steers
libc onto is this engine's fastest copy rather than its slowest. Register,
YMM, MMX, and x87 state is serialized in the task-owned bridge frame across
carrier scheduling. Passing those corpora does not by itself establish
arbitrary distribution or complete Linux syscall compatibility. Other host
products and page profiles retain arm64 userspace execution.

The x86_64 gadget engine assembles against ARMv8.5-A, so it lowers guest
semantics onto LSE atomics, FlagM/FlagM2 condition transfer, and FRINTTS
truncating conversion directly. Every arm64 Mac implements those extensions,
and the engine is a macOS-only backend, so this constrains no shipping product.
The arm64 engine keeps its own, higher, ARMv8.6-A declaration.

The gadget cache retains a bounded number of tagged address spaces. Retiring an
address space drops only its owned state, and stale same-generation programs
must match every consumed guest word before reuse. Shared gadget translations
contain no executor-owned link pointers after publication; consumers relocate
links into their own state.

A cache slot indexes a translation rather than holding one. Each stored program
is a single exact-sized allocation carrying its stream and the guest words it is
validated against, taken from an arena with sixteen-byte size classes, and the
program refers to those words rather than owning a second copy. A build
assembles its stream in a scratch buffer that the next build rewinds, so a
translation allocates only when it is stored. The arena marks where every live
translation starts, one bit per granule, and that map is how a terminal exit
inside a linked successor is attributed to the successor's program; the cache
keeps no index keyed by stream address and maintains nothing per store for
that lookup.

The default-on `UML_GADGET_SCALAR_FORWARDING` build keeps one write-through scalar
result in host `x0` between adjacent declared gadgets. Assembly templates emit
both immutable operand contracts and precompiled source-mask variants; the
builder selects a variant from the actual decoded entry, not an opcode-pair
whitelist. Canonical GPR/SP stores remain unchanged. Arithmetic, logical,
bitfield, select, shift, ordinary scalar loads, and scalar memory inputs share
this contract. Store data crosses translation helpers in a callee-saved host
register. Unsupported entries, fused decoder groups, branches, and block
boundaries end the chain. Pair results and writeback-load results are not
exported. There are no runtime register tags or executable-memory writes.

The independent, default-on `UML_GADGET_VECTOR_FORWARDING` option applies the same
template-owned selection to SIMD/FP results naturally produced in host `v2`.
Arithmetic, logic, comparisons, widening/narrowing, reductions, and
accumulator templates declare their source masks. Consumers capture all
forwarded inputs before overwriting `v2`; narrow reads retain architectural
upper-bit clearing, and canonical vector stores remain write-through. Declared
software fallbacks restore the complete result in `v2` on their cold return
path. Undeclared entries and block/link boundaries invalidate residency. Vector
memory values and templates producing results in other scratch registers are
not exported. This is not a persistent guest-vector register cache.

Both forwarding options can be disabled independently for controls or code-size
tradeoffs. With both options and coverage disabled, the original gadget text is
unchanged. The default policy does not imply a general application speedup:
measured gains are workload-dependent and the forwarding bank is larger.

`UML_GADGET_DEPENDENCY_COVERAGE` is a separate, default-off diagnostic build.
Its entered-group counters describe opportunities in both scalar and SIMD/FP
contracts, independently of whether forwarding is enabled. Each group occupies
one scalar, vector, or unsupported bucket; combined totals do not double-count.
They do not measure retirement, conditional operand use, or speed. The
measurement controls are owned by [`../tests/perf/README.md`](../tests/perf/README.md).

`UML_GADGET_EXECUTION_HISTOGRAM` is the other default-off diagnostic build and
excludes dependency coverage. The same counter gadget indexes a table keyed by
the exact (predecessor, entry) pair the translator selected, so executed
entries, executed adjacent pairs, and entered guest words are exact. Its dump
also reports why each linked chain returned to the executor and how many
distinct targets the missing indirect sites had. Instrumented streams carry
one extra dispatch per guest instruction, so the build is evidence for
instruction mix and static cost, never for speed.

The JIT derives private and shared cache geometry from one process budget,
defaulting to half of guest RAM. A managed run keeps at most one executor per
guest CPU, with up to 256 MiB of private cache per executor; the shared tier
receives the remainder. Smaller total budgets reduce private geometry.
Exhausted executor borrowers receive FIFO handoffs. A contended return
synchronizes notification with the queue mutex so a borrower cannot miss the
transition to an available executor; uncontended returns remain lock-free.
Shared translations own immutable code and validation identity. Bounded
replacement admits new working sets after the shared tier fills, retaining
entries whose code or validation words are still in use. The shared index
retains its high-water allocation charge.

Direct and indirect links live in executor-private writable data, outside
executable mappings. Links are tagged by address space, and each executor
retains activation contexts for up to 64 address spaces. Switching back to an
unchanged context preserves trained links; code publication or context
eviction deactivates that context before reuse. Stale blocks still require
exact guest-word validation. Runtime native compilations use one lowering pass,
avoiding speculative GPR residency recompilation during process startup.

On a private miss, executors first try an exact shared translation, then
compile eagerly.

Native stores publish per-vCPU state on separate 128-byte host cache lines.
Bounded load/compare/conditional-branch/store-exclusive retry regions use host
exclusives after checking both mappings and permissions. Unsupported shapes
and tagged accesses take the JIT's semantic slow path. Native and semantic
stores share the JIT's software reservation table and publication protocol.
An ordered store through an alias invalidates the matching software reservation
even when the stored value is unchanged.

## Devices

The generic vhost-user transport supports independent Virtio-GPU,
Virtio-input, Virtio-Sound, Virtio-console, and Kernarium battery services.
Deferred used-ring publication lets a device retain a descriptor until
asynchronous host work is complete.

The Apple graphics stack uses upstream virglrenderer on one packaged host GL
provider: Mesa Zink over KosmicKrisp. macOS is the only platform with a build
of that stack; iOS presentation code remains but the iOS GPU product is
pending its Mesa/KosmicKrisp port. macOS provides native AppKit and VNC
display consumers; iOS provides UIKit Metal presentation and deliberately
excludes VNC. See [`../docs/graphics.md`](../docs/graphics.md).

The separately named macOS-only `apple-gpu-vulkan-experiment` composition adds
a Venus facade over one directly linked Mesa KosmicKrisp framework, and routes
host OpenGL through Mesa Zink on that same driver. It is compiled only by the `*-venus*` presets and
remains invisible at runtime unless `UML_GPU_EXPERIMENTAL_VULKAN=1` is set
exactly. Under that gate, the host implements capset 4/version 0, HOST3D blob
resources, ring-indexed deferred fences on the same transport pipeline the
default Virgl path uses, and a bounded 1 GiB vhost-user shared-memory
aperture; invalid memory types, ranges, ownership, acknowledgements, or
reconnect state fail closed. The guest can map up to one blob per 16 KiB
aperture page without a smaller record-count limit. Page ownership stays
reserved through pending map/unmap acknowledgements; successful teardown or a
failed map releases it for reuse. A receipt-bound guest image supplies the
paired Mesa Venus driver and Vulkan CTS payload. The accepted product contains
no KosmicKrisp and advertises none of these experimental features; passing
local probes or CTS runs does not create a formal Vulkan conformance claim.

Kernarium attaches:

- an absolute touchscreen and evdev keyboard;
- one signed-16-bit stereo 48 kHz playback stream through Virtio-Sound;
- an Android-only Virtio battery reflecting the Apple host's percentage and
  charging state, with a 100-percent charging fallback;
- SLiRP networking from the verified iOS prefix;
- a Virtio-GPU Metal framebuffer; and
- the existing `ttyS0` stream mirrored to a terminal view.

Point display mode maps safe-area points to guest pixels at 160 DPI. Retina
mode multiplies geometry and density by `nativeScale`. The GPU, touchscreen,
and Android logical-density command line use the same launch geometry.

The audio device owns a bounded period/completion ring. The callback path does
not allocate or lock. iOS owns AVAudioSession interruption and route recovery.
Capture and mixer controls are not exposed.

SLiRP host forwards are owned by the provider so they survive provider reopen.
An unavailable individual host port is reported without removing the guest's
network interface. Filesystem socket paths are removed only by their owning
guards or the common termination cleanup.

## Guest storage and lifecycle

The platform layer tracks every writable host descriptor opened for the guest.
`uml_emulator_sync()` asks the guest to write back through its management
console, then flushes those host descriptors. `uml_emulator_quiesce()` enters
full Linux system suspend and waits until devices, secondary CPUs, interrupts,
and timekeeping have reached the platform sleep boundary before flushing the
host. `uml_emulator_resume()` sends the console wake edge. A kernel without the
suspend command falls back to the legacy stop/go pair.

Every run receives a unique UML identity and management-console path. Guests
that need writeback requests boot with magic SysRq enabled. If the physical iOS
sandbox cannot bind the console, host-file flushing still runs but guest
writeback/freeze is unavailable.

Guest disk capacity is an application concern rather than a runtime option.
Kernarium transactionally grows raw Android images and their final userdata
GPT entry through Swift `DiskUtil` and the statically linked GPT fdisk core.
The standalone macOS Android wrapper performs the same host-side operation
with the system `truncate` utility and Homebrew `sgdisk`. Android invokes the
idempotent ext4 `resize2fs` tool before mounting `/data`; OrangeFox performs an
explicit extent comparison before `e2fsck` and `resize2fs`. The runtime
validates that the GPT matches the file and boots it without rewriting either.

Kernarium flushes when losing focus, quiesces before backgrounding or
termination, and resumes on foregrounding. A `.kern` bundle owns its persistent,
directly writable image; launch must reopen that same image.

## API and validation

The installed API is under `api/include/uml/`; its ABI identity is independent
of the host/kernel bridge under `../kernel/common/abi`. Follow the ABI README
for declaration changes.

Use the workflows in [`../docs/testing.md`](../docs/testing.md). The
architecture audit verifies source ownership, dependency direction,
executable-memory ownership, test registration, and platform isolation. Guest,
package, graphics, and performance behavior require their separate workflows.
