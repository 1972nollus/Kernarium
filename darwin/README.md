# Darwin UML host runtime

## Ownership

```text
api/       installed C API and ABI identity
include/   host copies of the host/Linux bridge and host-only guest contracts
platform/  Apple capabilities, VM, files, signals, and sockets
memory/    guest address spaces, software MMU, and TLBs
cpu/       gadgets, JIT, executable memory, shared decode, and neutral
           engine selection
runtime/   guest-entry composition
device/    block, console, GPU, input, audio, and network providers, and the
           vhost-user transport they share
emulator/  options and product composition
app/macos/ standalone process adapter
app/initramfs/ host tool that writes a guest's per-run initramfs
```

Targets and source lists belong to these directories. See
[`../docs/architecture.md`](../docs/architecture.md) for dependency direction.

## Products and defaults

The standalone macOS application supports the normal arm64-userspace Linux
kernel and the separate x86_64-userspace kernel, both with 4 KiB guest pages.
When omitted, `mem=` defaults to 8 GiB and `ncpus=`
to 10; explicit kernel arguments override either value independently.
`cmake --preset macos-x86_64` selects the dedicated x86_64 artifact and host
gadget backend; ordinary 4 KiB presets continue to select arm64 userspace. The
x86_64 product defaults to `cgroup.memory=nokmem`: userspace memory-controller
accounting remains available, while kernel-object charging is outside this
compatibility slice. An explicit `cgroup.memory=` argument overrides the
default.

The emulator library chooses no guest init and configures no guest network;
the image owns both. Each Linux product embeds the boot base archive for its
userspace architecture. Unless the command line names `init=` or `initrd=`, the
launcher boots that base with a per-run initramfs: by default the EROFS image
under a tmpfs overlay and then the image's own init, and with `--guest-init=`
or `--guest-file=` whatever those supply. An explicit `init=/bin/sh` still
starts a raw shell on the image. The
[boot ramdisk contract](../tools/rootfs/boot-ramdisk/README.md) owns the
archive, its parameters and the host-side `uml-initramfs` tool.

Kernarium's runtime frameworks, defaults, and one run per launch are described
in [`../Kernarium/README.md`](../Kernarium/README.md).

The Android macOS composition is a separate product. Its build consumes a
compatible Android kernel artifact; launch accepts a rootfs directory and
one directly writable GPT image without comparing rootfs release identity, and
it does not search for the Linux desktop image.

## Runtime, memory, and kernel loading

Handle and run ownership, the vCPU carriers and host signal ownership, the
native kernel window, guest RAM commitment and the balloon, accepted-artifact
sharing, and the signed kernel loader are cross-module contracts owned by the
architecture: see its
[runtime ownership](../docs/architecture.md#runtime-ownership),
[memory and executable code](../docs/architecture.md#memory-and-executable-code),
and [CPU execution and scheduling](../docs/architecture.md#cpu-execution-and-scheduling)
sections.

## CPU engines

Engine selection, executable-memory ownership, the JIT's semantic slow path,
the x86_64 slice's userspace profile, and the gadget cache's address-space tags
are in the
[architecture](../docs/architecture.md#cpu-execution-and-scheduling). This
section owns the engines' internal detail.

The x86_64 backend's CPUID advertises ERMS: a repeated string operation moves
whole page-bounded blocks, so the REP MOVSB that ERMS steers libc onto is this
engine's fastest copy rather than its slowest.

The x86_64 gadget engine assembles against ARMv8.2-A with the CRC and
cryptography extensions. The arm64 engine keeps its own, higher, ARMv8.6-A
declaration.

A gadget cache slot indexes a translation rather than holding one. Each stored
program is a single exact-sized allocation carrying its stream and the guest
words it is validated against, taken from its address space's arena in
sixteen-byte granules, and the program refers to those words rather than
owning a second copy. An arena reuses freed space by best fit and merges a
freed block with its free neighbours, so the hole an evicted translation
leaves serves translations of any size; it unmaps a 64 KiB chunk once the
chunk's last translation leaves. An arena maps its chunks a few at a time (four,
doubling to sixteen), and an address space that dies unmaps what remains
without freeing a translation of it. A mapping can take addresses that the
arena's earlier mappings gave back, so their ranges interleave; a chunk is
accounted to the mapping that holds it, and a dying arena unmaps only chunks
it still holds, never an address another mapping has taken since. A build
assembles its stream in a scratch buffer that the next build rewinds, so a
translation allocates only when it is stored. The arena marks where every
live translation starts, one bit per granule, and that map is how a terminal
exit inside a linked successor is attributed to the successor's program; the
cache keeps no index keyed by stream address and maintains nothing per store
for that lookup.

A slot is three words on arm64 and four on x86: a tag, the translation, and a
memo. The tag packs the translation's key (an A64 PC below the guest address
limit, an AArch32 address under its ISA flags, or an x86 address and execution
mode) under the 16-bit id the cache gives each resident address space, so a
lookup is one comparison and an empty slot, whose tag is zero, matches nothing.
The memo packs the link record the slot publishes through with a 16-bit
validation epoch. An address space's epoch advances when its code-change number
moves, a slot whose epoch is the current one has matched guest memory since,
and a sweep of the space's slots when the epoch wraps keeps that exact. An
arm64 body header is 16 bytes, its build's stop kept in a tail only for a block
that stopped early or holds no instruction, and an x86 translation record is
96 bytes. Hit rates and victims are unchanged by this packing: the set still
hashes the guest's address-space number. The sizes hold with shared bodies
too, where every executor keeps its own slots and a body's header is paid
once however many slots hold it.

The x86 cache takes a translation's set from one multiply of its address
space, byte-granular RIP and execution mode
(`x86_64/gadgets/program_cache_index.hpp`), so clustered code loads the sets as
a uniform spread would, where two xor folds of the address left far more full
sets than that.

The arm64 and x86 private gadget caches use the same `UML_GADGET_CACHE_SLOTS`
and `UML_GADGET_RESIDENT_GENERATIONS` controls: eight ways, 256-slot lazy slabs,
and eight resident address spaces. The slot count defaults to 131,072 on every
product, so that macOS behaves like iOS, and a slot is committed whether or not
it is occupied; an explicit `-DUML_GADGET_CACHE_SLOTS` selects another power
of two. Their payloads and mutable branch links remain ISA-specific. Cached
x86 programs release spare decoder and native-stream capacity before
publication.

Unless `DARWIN_UML_GADGET_SHARED_BODIES=0` keeps them private, arm64 gadget
executors running one address space with the same build options and SVCR mode
share its translations: the bodies, link records, indirect sites and target
table belong to one process-wide space, and each executor's private slots only
point into it. A body lives while any executor's slot holds it, so an address
space costs the union of what its executors hold. A miss with no
slot for its PC adopts the body the space indexes for it after the same exact
word validation a stale slot gets, without copying or relocating it. Links and
indirect-site cells are retargeted with one sixteen-byte store, single-copy
atomic with FEAT_LSE2, which sharing requires; a site takes the space's lock
only to enrol on its first training, and translated code fills only a site's
empty cells, so vCPUs sharing a polymorphic site do not keep rewriting it. A
code change raises the space's gate before it resets the live links, so an
executor still validating at an older sequence cannot relink. A body no slot
holds is freed once every other executor attached to the space has left
translated code since it was unpublished, and the last executor to retire a
space frees it with its arena. A code change one vCPU publishes resets the
space's links before that vCPU continues, so every other executor of the space
leaves translated code at its next block transition. Sharing is the default
on a host that retargets a link pair in one access (FEAT_LSE2, which every
Apple Silicon product has): it halves the four-vCPU Geekbench multi-core
cache, and renewal, below, keeps a shared space from accumulating the freed
arena and link records that process churn leaves in it, which WebGL
Aquarium's cache did before (G227, G267, G268). A private cache sweeps away
the link records and indirect sites that nothing it holds names any more; a
shared space is never swept, and renewal replaces it instead.

A shared space bounds its freed arena by renewal. It counts the bytes of its
arena and of the bodies some slot holds; once it holds at least two mebibytes
no body uses, and more than its bodies use, the executor that let go of a body
or stored one closes it. A closed space leaves the registry, so the next
executor to run its address space gets a new one, and every executor attached
lets go of it, as the window lets go of the least recently used space, at the
start of its next entry into translated code. The old space then dies with its
last executor, taking its arena, link records and freed bodies, and the
executors translate again what they still run.

The kernel numbers every Linux mm it creates (fork, exec and the first process
each get a new one), never reuses a number, and destroys the generation when
the mm's last reference goes; the MMU counts destroyed generations. An arm64
gadget executor loads that count at the start of every entry into translated
code and, when it moved, lets go of each address space it holds, in every SVCR
mode cache, whose generation the MMU reports destroyed, as it lets go of a
closed one. A generation that was destroyed can never run again, so nothing it
held can be looked up again. Its number keeps its place in the executor's
window order as a tombstone that the least-recently-used rule retires with no
work, so a window keeps exactly the live address spaces it kept before. A
shared space dies with the last executor attached to it, which lets go at its
own next entry; a vCPU that is idle keeps it until then. Geekbench's windows
otherwise hold the boot-time processes' spaces for the whole run (G272).

x86 gadget executors keep private translations: their links carry the block's
guest range for self-modifying-store checks, which translated code reads in
two loads that one atomic store cannot keep consistent. They let go of the
address spaces of destroyed generations the same way, at the start of each run
([`cpu/x86_64/gadgets/README.md`](cpu/x86_64/gadgets/README.md)).

In both arm64 engines, code another vCPU changed and synchronized takes effect
at this vCPU's next context synchronization point. Every run publishes in
`CpuState` the address of its address space's code-change sequence and the
value its translations and links were validated at. An ISB, the A64, A32 or
CP15 one, compares the two after its native ISB, one load and a compare, and
when they differ leaves translated code at its fall-through, so the next run
revalidates and relinks first. The gadget A64 ISB leaves from inside its block
and gives back the instructions after it; a gadget A32 block ends at its ISB.
A kernel write into a guest-code page, such as a system call's output that a
uaccess copies back from the kernel's alias, likewise reaches arm64 code only
through the guest's own IC IVAU or `cacheflush()`, as on hardware that
advertises neither DIC nor IDC. It publishes a code change
of its own only once x86 code is about to run in the process, since x86 fetch
sees every store; otherwise a process with an executable stack would publish
one, and revalidate every translation, for each system call that writes its
result there.

Scalar forwarding keeps one write-through scalar result in host `x0` between
adjacent declared gadgets. Assembly templates emit
both immutable operand contracts and precompiled source-mask variants; the
builder selects a variant from the actual decoded entry, not an opcode-pair
whitelist. Canonical GPR/SP stores remain unchanged. Arithmetic, logical,
bitfield, select, shift, ordinary scalar loads, and scalar memory inputs share
this contract. Store data crosses translation helpers in a callee-saved host
register. Unsupported entries, fused decoder groups, branches, and block
boundaries end the chain. Pair results and writeback-load results are not
exported. There are no runtime register tags or executable-memory writes.

Vector forwarding applies the same template-owned selection to SIMD/FP results
naturally produced in host `v2`. Arithmetic, logic, comparisons,
widening/narrowing, reductions, and accumulator templates declare their source
masks. Consumers capture all
forwarded inputs before overwriting `v2`; narrow reads retain architectural
upper-bit clearing, and canonical vector stores remain write-through. Declared
software fallbacks restore the complete result in `v2` on their cold return
path. Undeclared entries and block/link boundaries invalidate residency. Vector
memory values and templates producing results in other scratch registers are
not exported. This is not a persistent guest-vector register cache.

`BuildOptions` can disable either kind per program, which is how tests and the
scalar ceiling bench compare forwarding in one binary. Forwarding does not imply
a general application speedup: measured gains are workload-dependent and the
forwarding bank is larger.

`UML_GADGET_EXECUTION_HISTOGRAM` is a default-off diagnostic build. A threaded
counter gadget indexes a table keyed by the exact (predecessor, entry) pair the
translator selected, so executed entries, executed adjacent pairs, and entered
guest words are exact. Its dump also reports why each linked chain returned to
the executor and how many distinct targets the missing indirect sites had.
Instrumented streams carry one extra dispatch per guest instruction, so the
build is evidence for instruction mix and static cost, never for speed.

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
eviction deactivates that context before reuse. Link records outlive their
address space and are reclaimed only with the executor's private cache, so a
full direct or indirect table recycles that cache as a full code arena does,
rather than publishing unlinked blocks. Stale blocks still require
exact guest-word validation. Native compilation is one lowering pass; there is
no speculative GPR residency recompilation.

On a private miss, executors first try an exact shared translation, then
compile eagerly.

Native stores publish per-vCPU state on separate 128-byte host cache lines.
Bounded load/compare/conditional-branch/store-exclusive retry regions use host
exclusives after checking both mappings and permissions. Unsupported shapes
and tagged accesses take the JIT's semantic slow path, which performs each
atomic, exclusive and ordered access as one host access on the translated
page, with LSE atomics or CASP for a read-modify-write. Those accesses are
therefore single-copy atomic against native code on other vCPUs and against
host threads that share the page. Only an exclusive load, or an instruction
run while its software reservation is held, waits out other vCPUs' native
store blocks. Native and semantic stores share the JIT's software reservation
table: every data store, including a pair or structure store that leaves its
native lowering, invalidates the granules it writes through any alias of the
same backing, even when the stored value is unchanged.

## Devices

The vhost-user transport and the Apple graphics stack are in the
[architecture](../docs/architecture.md#devices-and-graphics) and
[`../docs/graphics.md`](../docs/graphics.md). The Venus route's host side
fails closed on an invalid memory type, range, ownership, acknowledgement, or
reconnect state, and keeps an aperture page owned through pending map and unmap
acknowledgements; a completed teardown or a failed map releases it for reuse.

The devices Kernarium attaches and its display geometry are listed in
[`../Kernarium/README.md`](../Kernarium/README.md#devices-and-display).

The audio device owns a bounded period/completion ring. The callback path does
not allocate or lock. iOS owns AVAudioSession interruption and route recovery.
Capture and mixer controls are not exposed.

SLiRP host forwards are owned by the provider so they survive provider reopen.
An unavailable individual host port is reported without removing the guest's
network interface. Filesystem socket paths are removed only by their owning
guards or the common termination cleanup.

libslirp comes from the maintained series in
[`../dependencies/README.md`](../dependencies/README.md). A forward's listening
socket queues up to the host's `SOMAXCONN` pending connections, so each of a
burst of concurrent host connects reaches the guest
(`slirp-network-forward-backlog`, and through a booted guest in
`guest-slirp-network`). The receive window SLiRP advertises never exceeds its
free buffer (`slirp-network-tcp-window`). SLiRP closes the host end of a
connection whose guest end stops answering within about ten minutes, whether
it was retransmitting to the guest or probing a window the guest closed
(`slirp-network-tcp-liveness`). An idle connection has no keep-alive, the
RFC 1122 default, and stays open until one side closes it.

The guest network driver has no transmit flow control, so a frame the provider
refuses is lost and TCP must retransmit it. A guest transmit into a full
transmit ring therefore waits, up to 500 ms, for the SLiRP event loop to free a
slot; the loop never waits on the guest. `guest-slirp-network-transfer` checks
that bulk, paced, concurrent, reader-stalled and request/response uploads reach
a host listener intact, promptly and without a dropped frame.

## Guest storage and lifecycle

Sync, quiesce, resume, and the management console are in the
[architecture](../docs/architecture.md#guest-lifecycle). Disk capacity is an
application concern: the runtime validates that a GPT matches its file and
boots it without rewriting either, and the
[Android static disk contract](../android/lineage-24/README.md#static-disk-and-boot-contract)
owns how an image and its filesystem grow. When Kernarium syncs, quiesces, and
resumes is in [`../Kernarium/README.md`](../Kernarium/README.md#lifecycle).

## API and validation

The installed API is under `api/include/uml/`; its ABI identity is independent
of the host/kernel bridge under `../kernel/common/abi`. Follow the ABI README
for declaration changes.

Use the workflows in [`../docs/testing.md`](../docs/testing.md). The
architecture audit verifies source ownership, dependency direction,
executable-memory ownership, test registration, and platform isolation. Guest,
package, graphics, and performance behavior require their separate workflows.
