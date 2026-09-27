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
targets and source manifest. The neutral CPU factory selects the gadget or JIT
adapter; engines do not depend on one another, and shared CPU code depends on
neither. `architecture-audit` enforces this on each CPU target's declared
engine kind and on textual includes between the engine directories.

An emulator archive is a self-contained final product. CMake merges object
libraries and interface-carried objects into it but never a linked static
archive, so every module the emulator reaches delivers objects, including one
that also keeps a static archive for single-copy process state.
`scripts/build-xcframework.sh` rejects a release archive that leaves a
first-party symbol undefined.

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
physical-device address ceiling. The iOS slice also runs on an Apple silicon
Mac in Designed for iPad mode, where the process is a macOS process; placement
follows the address space the process actually has, so that mode takes the
macOS window.

The window is a reservation, and it never returns address space to the process
while the VM lives. It is created once as a single `PROT_NONE MAP_NORESERVE`
anonymous mapping, guest pages are mapped over it with `MAP_FIXED`, and
releasing one hands its range back to that reservation instead of unmapping it:
one mapping call, no committed pages, and the replacement is atomic. `munmap`
would delete the reservation along with the mapping and leave a free gap inside
the window. Darwin's allocation hint follows the most recently freed range, so
on iOS - where the host chooses the window's address and it therefore sits in
the band the system allocators draw from - such a gap is not merely available to
an unrelated `mmap(NULL)` or `mach_vm_allocate(ANYWHERE)` but preferred by it.
Either party then destroys the other in silence: the guest's next `MAP_FIXED`
replaces the new owner's memory and still reports success, and that owner's own
release removes live guest mappings the guest never asked to drop. The window's
map entries are bounded by its geometry rather than by how often the guest maps,
because re-mapping a released range replaces its entry instead of adding one.
Domain retirement follows the same rule: it ends one address space, not the
process, and a later one maps the same window. Guest RAM that a host-service
view still borrows is the exception: it stays mapped until the last borrowed
view of the root is released, because in-process backends are stopped only
after the run returns, and a host stop leaves their queues started.

Every Darwin UML kernel uses its linear physical window as the anonymous RAM
root. Software-MMU and host-service consumers borrow that root or create
non-copying Mach aliases; no separate full-span guest-RAM mapping is retained.

`mem=` is a ceiling, not a reservation. The guest commits guest RAM before
using it and releases what it frees, through the bridge's
`darwin_uml_physmem_commit` and `darwin_uml_physmem_release`, and
`memory/guest_ram_commitment.cpp` applies both, one host page at a time. The
kernel's page-reporting driver releases free 512 KiB blocks (128 KiB at 4 KiB
pages); the allocator commits a released block again, outside the zone lock,
the first time it hands out a page of it. A released page leaves the process
footprint (`MADV_FREE_REUSABLE`), and a committed one is counted again
(`MADV_FREE_REUSE`). Physical iOS also wires a page when it is committed and
unwires it when it is released, so it wires only the RAM the guest holds; the
signed kernel image and the loader tail stay wired from startup. Simulator and
macOS commit and release without wiring. Wiring prevents compression; it does
not decide whether a page is mapped. A commit that cannot wire leaves the page
committed and usable. The kernel also trims its inactive page cache towards a
floor in the background (`page_cache_trim.*`). The host caches the same disk
blocks, so a large guest page cache mostly holds memory twice.

The Virtio balloon (`device/balloon`) takes the memory the host GPU uses out of
the guest. Its target is the larger of the process's Metal allocation
(`MTLDevice.currentAllocatedSize`) and its graphics footprint (the graphics
ledger of `TASK_VM_INFO`), which also charges the command storage the GPU
driver has written and the allocation omits. The target is capped at the Venus
device-memory limit of half the guest's RAM and followed in 16 MiB steps. A
host page leaves the footprint once the guest has ballooned all of its 4 KiB
frames. Under `VIRTIO_BALLOON_F_MUST_TELL_HOST`, a deflate commits the page
before the guest may use it.

A host mapping call that fails says so. The kernel distinguishes an exhausted
host only by `ENOMEM`, so Mach's resource-shortage and address-space-exhaustion
codes both arrive as that, and the first such failures are reported on the run's
own console rather than behind a trace variable. Without both, an exhausted host
reaches the guest as page tables describing memory that is not there, with
nothing in the run saying why.

A kernel-range flush completes or the kernel stops. `flush_cache_vmap()` and
`flush_cache_vunmap()` are both `flush_tlb_kernel_range()` on this port, so
every vmalloc map and unmap is one such call and its result is never discarded:
a transient host failure is retried a bounded number of times, and a persistent
one panics. A flush requested from inside the walker that already owns the
kernel turnstile is recorded and drained by that walker before it releases
ownership, so it is deferred but never dropped. A guest returning from
`flush_tlb_kernel_range()` may therefore assume the host agrees with `init_mm`'s
page tables over that range. The unmap side needs this most:
`__purge_vmap_area_lazy()` releases those addresses for reuse immediately
afterwards, and nothing ever faults on a mapping that should not exist, so a
dropped unmap leaves the host serving a freed page with no fault to repair it.
A failed kernel map unmaps its own address for the same reason, degrading to
"not mapped", which the fault path does repair.

A generation's software-MMU mappings are published only by a page-table
walker's batch. The batch records the generation's invalidation epoch when it
begins and is rejected when any invalidation arrives before it ends, so a walk
which sampled a PTE before Linux changed it cannot publish it afterwards.
Nothing else installs a mapping. A kernel uaccess pins its page from a PTE
whose lock it has already dropped, so when the host names other backing than
that page either of the two may be the stale one; the uaccess discards the
mapping and the next access replays the PTE. An absent page is always sound.

Gadget streams, MMU backing, and shadow mappings are data.
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
artifact on macOS and on the iOS device. The macOS x86_64 userspace
product uses a separately identified 4 KiB mainline slice. Android uses a
separate accepted 16 KiB ACK artifact.

Kernarium is an iOS device product only. Its GPU stack renders through
KosmicKrisp, which requires Metal 4, and the iOS Simulator exposes no Metal 4
device, so the Simulator has no build of the application or of the Apple GPU
prefix. The unmodified iOS device slice runs on an Apple silicon Mac in
Designed for iPad mode, where it reaches the host GPU directly; that mode is
where iOS behaviour is verified when no physical device is attached. The
Simulator remains a target of the ordinary, non-GPU iOS libraries.

A Kernarium launch runs its guest once. When the run returns, whether the
guest powered off or the runtime failed, the application says how it ended
and asks to be relaunched; it never starts a second run in the same process.

## CPU execution and scheduling

Darwin owns one persistent pthread per configured vCPU, including vCPU0.
Linux task contexts remain on guest kernel stacks and migrate between carriers;
guest task creation does not create host threads. CPU hotplug reuses carriers,
and shutdown returns to a native carrier stack before thread teardown.

A host can call `exit()` while carriers still run, as an iOS application and
an XCTest host do. The arm64 gadget contract indexes, the shared-backing
state, and the active-access registry therefore have no exit-time destructor;
`arm64-gadget-exit` keeps carriers using them through `exit()`.

Carriers are named `uml-vcpu-N-carrier`. The kernels use a 100 Hz tick with
`NO_HZ_IDLE` and high-resolution timers: the one-shot clock event records a
per-CPU deadline that idle carriers wait on and busy carriers poll, so an
hrtimer expires at its own deadline rather than on the next tick. Darwin
signals wake carriers but do not execute Linux IPIs asynchronously; IRQ-enabled
spin checkpoints service pending IPIs.

Darwin descriptor IRQs preserve Linux UML's edge-triggered epoll contract.
Separate kqueues retain dispatch edges and wake idle carriers without consuming
those edges. Reentrant dispatch cannot repeat a readable level while an outer
handler owns it; modifying a registration rearms an already-ready descriptor.
This contract is shared by mainline and Android kernels.

Early UML calibration uses process-directed `SIGALRM`. Application and launcher
threads keep it blocked, and only the vCPU0 carrier unmasks it until cooperative
deadlines take over. Kernarium runs the guest on its named `uml-launcher`
thread, never the main thread.

Darwin gives a process-directed signal to one thread when it is raised and
never moves it, so a tick raised while every thread blocks `SIGALRM` stays
pending on the main thread until that thread unblocks it, which the macOS
AppKit thread does only after the guest has stopped. The embedded kernel's
host signal handlers and interval timer also outlive its entry. The loader
therefore takes them back before it releases the image: it disarms
`ITIMER_REAL`, passes every signal whose handler lies in the image through
`SIG_IGN`, which discards its pending instances on every thread, reinstalls
the disposition the host had before the run, and restores the launcher
thread's signal mask. No signal the kernel owned is delivered after it stops.

The product has two selectable engines. The gadget engine executes arrays of
precompiled function pointers and data. The JIT emits native blocks through its
sole executable arena. Guest-visible exceptions, fault priority, retirement,
and feature exposure must agree across engines and with the independently
derived expectations of the A64 instruction corpus. Instruction classes that
the JIT's native lowering leaves to a helper run in the JIT's own semantic
slow path, never in another engine; a helper treats any word outside its
routed classes as a fatal internal error.

Data-alignment faults retain the effective data address and the faulting PC.
The host exports a distinct bridge event, and both kernel lines deliver
`SIGBUS/BUS_ADRALN` so a guest handler can repair the access and resume. They
do not enter page-fault repair or retire the faulting instruction.

The macOS 4 KiB x86 compatibility slice keeps native arm64 UML kernel-mode
execution while exposing the x86_64 and i386 userspace ABIs. It cannot load
arm64 userspace; per-task mode selects only an x86-family operand and address
width. The host routes userspace instruction execution through immutable,
data-only streams of pointers to precompiled handwritten arm64 assembly
gadgets. There is no semantic C++ execution fallback. Two gadget banks serve
scalar integer instructions: an ordinary bank that decodes its operands from
the stream, and a register-baked bank generated at build time in which every
register offset is an immediate. A store publishes a code change only when a
per-address-space registry says some executor holds a translation of that
page, which keeps publication precise where READ_IMPLIES_EXEC makes every
readable page executable. The banks, the host-flag fusion rules and the
registry contract are in
[`darwin/cpu/x86_64/gadgets/README.md`](../darwin/cpu/x86_64/gadgets/README.md).
Exact x87 formats and transcendentals use bounded software-math primitives
called by those native gadgets. The userspace ISA boundary includes scalar x86
and x87/MMX, SSE through SSE4.2, AVX/AVX2,
F16C/FMA, AES/PCLMUL, BMI1/2, atomic and extended-state operations in both
long64 and compat32 modes. The pinned FEX handwritten x86-64 and i386 corpora
are the external instruction acceptance gate. AVX-512 and later encodings,
RTM/MPX, obsolete protected-mode bounds and far-pointer-load families, and
privileged, real-mode, virtual-machine, enclave, and system-management
instructions are outside this Linux userspace profile. That gate is not proof
of an arbitrary distribution or complete Linux syscall compatibility. The
normal mainline slices and Android remain arm64-userspace-only and contain
none of the x86_64 kernel overlay.

The [Steam rootfs](../tools/rootfs/steam-x86_64/README.md) runs Valve's native
x86-64 Linux client on this x86_64 userspace product, without a desktop
environment; Steam manages Proton itself. Its experimental Vulkan variant
consumes the receipt-verified Mesa/Venus guest runtime and the paired
KosmicKrisp host product. The producer owns gaming-session validation and
documents any remaining GPU startup boundary. Its image-owned init mounts
EROFS read-only and binds home and system state from a separate persistent
GPT/ext4 disk. Replacing the rootfs never resets that disk or reseeds an
existing Steam client. Boot and client-update progress use a Kernarium splash
before handing the display to Big Picture.

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
switch, so a stale record can only fall back to the ordinary miss exit. A
cached indirect branch whose cells (and, for a return, ring) miss then looks the
target up in its address space's direct-mapped target table, which every miss
exit trains with the target's block link; a hit follows that link and trains
the site's cell. The table belongs to the address space's link state and is
retired with it, so it never names another address space's code, and a link
reset by a code change leaves through its ordinary unlinked exit.
The software TLB keys its entries by guest page number. A same-page transfer
compares the page of its final byte against the way its first byte indexes: the
16 KiB direct-mapped index never places neighbouring pages in one set, so a
crossing transfer can only miss and is recognized on the cold path, while the
folded 4 KiB index keeps the explicit crossing check ahead of the lookup.

Guest debuggers depend on two kernel/host contracts. To single-step a task,
Linux sets the architectural step bit in the entry registers (arm64 PSTATE.SS,
x86 RFLAGS.TF). Every engine then retires exactly one instruction and reports a
single-step exit. The runtime returns that exit as a breakpoint event with trap
number 0x32 (arm64 software step) or 1 (x86 `#DB`), and Linux delivers
`SIGTRAP`/`TRAP_TRACE`. A stepped instruction that exits on its own, such as a
system call, breakpoint or fault, reports that exit instead. A `popf` that sets
RFLAGS.TF leaves the x86 program at the next instruction, and the runtime
retires exactly that instruction before it reports the step, as native `#DB`
timing requires. As in x86 Linux, a ptrace step over `popf` or `iret` leaves the
TF value that instruction wrote.

Linux writes another process's memory, such as ptrace breakpoints, gdb's
displaced-stepping pad, `/proc/pid/mem` or `process_vm_writev()` stores, through
its own view of the frame, so no translated store publishes the change.
`copy_to_user_page()` calls `flush_icache_user_page()`, and `process_vm_writev()`
calls the local `flush_icache_remote_user_range()` hook, a no-op on other
architectures. On Darwin both call `darwin_uml_vm_invalidate_code()` for the
target mm's generation. That call moves the code stamp of each executable page
and publishes the range, as a guest code synchronization would. Pages mapped
without execute permission are skipped, so data transfers publish nothing.

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
publication. GPU, video, input, sound, console, battery, and network providers own
their protocol behavior independently.

The [Virtio video device](../darwin/device/video/README.md) bridges the shared
mainline/Android V4L2 driver to the platform VideoToolbox decoder. It uses its
own vhost-user connection and requires no GPU context. The
[private wire header](../kernel/common/video/README.md) is separate from both
the public API and the host/kernel bridge ABI.

The transport also owns this process's open-descriptor ceiling, and raises it
to whatever the host permits before the first connection serves a request. A
backend that publishes a shared-memory region hands the guest one descriptor
for every host-visible blob it maps, the guest holds each one for as long as
that mapping lives, and the transport is an in-process socketpair, so guest and
backend draw on a single file table bounded only by the region's mapping count.
Darwin refuses to install an arriving `SCM_RIGHTS` once that table is full and
reports the refusal as `EMSGSIZE` on the read, which the transport reports as
the descriptor exhaustion it is rather than as an over-long message.

A lost backend retires the device rather than being warned about. A virtqueue
call or kick descriptor, or the backend request stream, that reports end of
file or an error which is not a would-block means the backend is gone: the
guest stops that interrupt source at once, reports it once per queue naming the
device, and hands the device to the connection-broken path, which breaks its
virtqueues and unregisters it so that pending and later requests fail instead
of waiting for completions that cannot arrive. Stopping the source cannot wait
for that teardown, because a host read registration is level triggered and a
descriptor at end of file stays readable: its handler would otherwise be
re-entered until it consumed a whole vCPU, which no amount of disabling the
interrupt line prevents. A would-block still ends an ordinary pass, so a
healthy device and a spurious wake are unaffected.

In-band notifications are always negotiated, so a queue is kicked by a
synchronous request on the control socket rather than through a kick
descriptor, and the backend answers a kick only after it has run that queue.
The guest endpoint of that socket is therefore blocking and the guest imposes
no deadline on the answer: a kick behind a long submission is waited out, for
as long as the submission takes. That is what makes every failure of the round
trip a real one, because `virtqueue_notify()` latches a virtqueue broken
permanently and irreversibly as soon as a notification reports failure, and no
driver can undo it. A backend that answers late is healthy; a backend whose
answer never comes, or comes malformed, is gone: the guest reports the failed
round trip once, naming the queue and the error, and retires the device by the
same path as a lost descriptor, so a failed kick never leaves a silently broken
virtqueue behind.

Neither side of a vhost-user connection waits on one channel alone. Only CPU 0
polls host descriptors, and a request on either channel can need an answer
before its sender reads the other channel again, so a side that waited on one
channel would leave both blocked. The guest serves the backend request stream
during every synchronous wait on the control socket, for a `GET_CONFIG` reply
or a kick's acknowledgement alike. Serving a request inside a wait records its
interrupt and replies on the request stream without touching the control
socket. Darwin drops a read edge whose data is gone before it is collected, so
a request read there leaves no edge for the request IRQ; the interrupts it
recorded are delivered through `irq_work` instead.

A backend request that needs an acknowledgement (an in-band completion, a
config change, or a shared-memory map or unmap) waits for it while still
answering the guest's feature, queue-size, shared-memory-configuration, and
config queries. Any other request is held until the acknowledgement arrives and
is then handled before anything still unread, because its sender drains the
backend channel while it waits, and handling it early could start a second
backend request inside the first.

Retiring the device is where the guest's recovery ends. virtio-gpu has no
broken-device handling of its own: it never tests `virtqueue_is_broken()`, so a
submitter already parked waiting for control-queue space is not woken, and
`drm_dev_unplug()` blocks behind it because that wait sits inside the section
the unplug synchronises against. In practice a lost GPU backend retires the
device and the guest's Vulkan client is killed within seconds, but a client
that happens to be waiting on a full control queue stays blocked until Mesa's
own ring timeout, on the order of fifteen minutes.

The Apple graphics path is:

```text
Linux virtio-gpu -> vhost-user -> virglrenderer -> Mesa Zink on KosmicKrisp
    -> shared Metal presentation -> AppKit or UIKit
```

GPU commands and rendering stay off the UI thread. The shared presentation
layer keeps the newest frame and at most one command buffer in flight. A
backpressured presentation may drop a superseded frame; only unsupported
resource shapes use CPU readback. See [`graphics.md`](graphics.md).

A guest drives the renderer, so no host write into guest memory is sized by
the renderer alone: the device bounds a translated guest range to one
negotiated memory region, clamps a reply to the writable descriptors the guest
supplied, and the renderer ends a Venus reply stream at the window the guest
declared rather than at the resource holding it.

Every GPU build also carries an independent Vulkan route:

```text
Linux Vulkan -> Mesa Venus -> virglrenderer capset 4
    -> directly linked Mesa KosmicKrisp -> Metal
```

The host always advertises that route. It does not replace or enter the GL
facade. Its fences ride the same deferred
fence pipeline the default path uses: the device creates a context fence per
pipelined request, and the transport retires it from the queue-worker timer.
Venus gives each guest ring its own timeline, so a Venus context is polled
individually while one poll still drains every Virgl context, and only a
Venus context defers a fence on a ring other than zero. Software 2D
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

Venus keeps shareable Zink scanouts in their renderer-optimal Metal
layout. An unlinked vnode-backed identity descriptor reconnects each guest
dma-buf with its in-process MTLHeap and native MTLTexture; the descriptor never
stores pixel data. Native presentation samples that texture directly. The VNC
consumer instead performs a damage-bounded asynchronous Metal blit into one
shared staging buffer, keeps at most one newer pending frame, and publishes to
LibVNCServer only after GPU completion, so neither readback nor encoding blocks
the guest flush timeline.

Sharing runs the other way too, for a guest whose buffer allocator is not the
Vulkan driver: when another context asks to import a GL resource, the renderer
gives that resource a host shared-memory segment the size of its tightly
packed linear layout and hands out the segment's descriptor. The texture is
refreshed from the segment before each GL read of it, and the segment is
rewritten from the texture at the end of a submission that drew into it and
before its descriptor is handed out. Ordering between the two consumers is the
guest's own fences; the renderer adds none. A resource no other context
imports keeps ordinary texture storage, and a host with dma-buf shares through
that instead.

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
`integration`, set by the `cmake/UmlTesting.cmake` helper that registers it.

`architecture-audit` checks the CMake File API graph, dependency cycles,
implementation includes, source ownership, and test registration. It does not
prove performance, generated code shape, a kernel boot, or guest behavior;
those require their owning validation.

The host/kernel ABI source is [`../kernel/common/abi`](../kernel/common/abi/).
Version, fingerprint, generated-copy, layout, and import checks fail closed on
drift.
