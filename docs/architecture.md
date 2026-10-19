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

A process runs one guest. After a run's kernel has claimed guest RAM, every
later run in the process, on any handle, returns `UML_EMULATOR_UNSUPPORTED`
without entering a kernel; see
[ADR 0002](decisions/0002-emulator-handle-lifetime.md).

Runs on different handles serialize through a process address-space gate,
because an embedded kernel's fixed host mappings cannot be shared. A stop
request cancels a run queued there. A callback that resolves no single run,
because runs overlap or none is active, fails closed rather than borrowing
another handle's context.

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
own stub write. macOS places the window at 96 TiB. iOS first asks for the
address that top-packs it below the physical-device address ceiling and, when
the host declines that hint, for wherever a window of that size fits, because
an iOS app's usable virtual range is not guaranteed to contain that envelope
(iOS 27's does not). The iOS slice also runs on an Apple silicon
Mac in Designed for iPad mode, where the process is a macOS process; placement
follows the address space the process actually has, so that mode takes the
macOS window.

On a physical iOS device the window is a reservation, and it never returns
address space to the process while the VM lives. It is created once, before the
guest starts, as a single `PROT_NONE MAP_NORESERVE` anonymous mapping; a run
whose window cannot be reserved fails rather than map guest RAM over a range
nothing holds. macOS and Designed for iPad do not reserve the window up front:
the kernel image and the loader tail take it by an address hint the host
honours, far above the ranges its allocators use. On every product guest pages
are mapped over the window with `MAP_FIXED`, and releasing one hands its range
back as a reservation instead of unmapping it: one mapping call, no committed
pages, and the replacement is atomic. `munmap`
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
Disk and image descriptors remain file-backed.

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

Kernel aliases are shared with the other views of their frames on both page
profiles. The 16 KiB profile maps every `init_mm` slot as a whole host page,
shared with the backing, so a vmalloc or vmap alias reads and writes the memory
that the linear map, the user mappings and the devices reach. Under 4 KiB pages
a 16 KiB host slot carries four `init_mm` PTEs, and only a slot whose four PTEs
name one 16 KiB-aligned run of frames of a single backing, with one protection,
is the same shared host page; an empty slot is unmapped in one call. Any other
slot is a private copy of its frames that the host publishes and refreshes only
at its synchronization points, so it is not coherent with the other aliases of
those frames for as long as both live. vmalloc therefore aligns, rounds and
allocates every area in groups of four pages (`arch_vmap_group_order()`) and
`vmap()` aligns its areas to a group, which makes the memory the kernel shares
with user space (BPF maps and ring buffers, perf and PACKET_MMAP rings that are
vmalloc'ed, io_uring regions) and the memory a device reads (sound buffers, GPU
command buffers) congruent. Only a kernel-private area falls back to small
pages when groups are unavailable. An alias that cannot be made whole groups, a
BPF ring buffer smaller than 16 KiB or a vmap of foreign pages, is kept in step
with the kernel-vmap hooks, `flush_kernel_vmap_range()` and
`invalidate_kernel_vmap_range()`, which publish a private slot and refresh it
from its frames and cost a page-table check on a shared one. Linux calls
`flush_cache_vunmap()` before it clears an alias's PTEs, and on this profile it
publishes the range's private slots, so the stores of a short-lived alias, such
as the `vm_map_ram()` output of EROFS's LZ4 decompressor, reach their frames
before the linear map reads them or the frames are freed; a TLB sync applies
only changed PTEs and publishes nothing for an unchanged one. A ring that lives
in the application's own memory cannot be regrouped, so on this profile io_uring
refuses with `EINVAL` a `IORING_SETUP_NO_MMAP` ring, a registered memory region
or a provided-buffer ring whose pages are neither one folio nor whole aligned
groups; the application uses a huge page or lets the kernel allocate it, as
`IOU_PBUF_RING_MMAP` does. The host replaces a slot's shadow when the kernel
maps a whole host page over it, publishing the shadow's stores to their own
backing first, and keeps the shadow only when the call runs on a stack inside
that slot. `guest-kernel-user-shared-memory` checks each sharing in both
directions and the refusal, and `guest-erofs-lz4` reads back an LZ4 EROFS image
with 4 KiB clusters.

A generation's software-MMU mappings are published only by a page-table
walker's batch. The batch records the generation's invalidation epoch when it
begins and is rejected when any invalidation arrives before it ends, so a walk
which sampled a PTE before Linux changed it cannot publish it afterwards. The
walker walks a rejected batch again before it returns to the guest; the bridge
reports the rejection as `DARWIN_UML_VM_BATCH_REJECTED`, not as an errno,
because Darwin's errno values are not Linux's. Nothing else installs a mapping.
A kernel uaccess pins its page from a PTE whose lock it has already dropped, so
when the host names other backing than that page either of the two may be the
stale one. Each uaccess page makes one `darwin_uml_vm_discard_if_stale()`
call, which discards such a mapping and leaves an absent or equal one alone,
and the next access replays the PTE. An absent page is always sound.

Gadget streams, MMU backing, and shadow mappings are data.
Only `darwin/cpu/jit_memory/executable_memory.cpp` may allocate executable
memory, publish RW pages as RX, control JIT write protection, or publish the
instruction cache. JIT engines reach executable memory only through it; the
arm64 JIT's arena maps and publishes its blocks with it. Its target's `native`
engine kind and the source audit keep the gadget engines and shared CPU code
from linking or including it. FEXCore, which the macOS-only x86-64 JIT embeds,
has no executable-memory path of its own: its maintained series routes
allocation, release, and the write window through hooks the embedder installs,
and prefix verification rejects a FEXCore archive that toggles JIT write
protection.

On macOS the owner serves JIT memory from one window in the host image's
4 GiB block, claimed when the process first asks whether it can run JIT code,
before its kernel boots. Apple cores predict an indirect branch's target
cheaply only near the branch, and generated code calls into the image: FEXCore
reaches its x87 fallbacks that way, and its code ran x87 a third slower from a
buffer the kernel had placed hundreds of GiB away. The window takes the largest
free range there, up to 1.5 GiB; a mapping beyond its capacity goes wherever
the kernel puts it. A release keeps the pages mapped and reusable, so memory
served again may hold its old bytes.

A library built without the embedded ELF runner, such as every XCFramework
slice, excludes native-VM and embedded-ELF execution; only the macOS unit
presets keep the native-VM bridge, for their tests. Kernarium uses versioned runtime frameworks whose kernel ELF is carried in signed Mach-O
`__TEXT`. Its loader aliases immutable executable pages from that mapping,
copies only data and BSS into anonymous storage, and rejects a carrier
mismatch, writable executable segments, or relocations into signed text. The app
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
separate accepted 4 KiB artifact of the `android-6.18` family, built from the
same mainline tree with the Android configuration.

Kernarium is an iOS device product only; [`graphics.md`](graphics.md)
gives the Metal 4 reason the Simulator has no build of it or of the Apple GPU
prefix, and how the device slice runs on an Apple silicon Mac. The application,
including its one run per launch, is described in
[`../Kernarium/README.md`](../Kernarium/README.md).

## CPU execution and scheduling

Darwin owns one persistent pthread per configured vCPU, including vCPU0.
Linux task contexts remain on guest kernel stacks and migrate between carriers;
guest task creation does not create host threads. CPU hotplug reuses carriers,
and shutdown returns to a native carrier stack before thread teardown. An
embedded run returns to the loader, which unmaps the image, only after the
kernel has joined every carrier and stopped its RTC alarm helper; a stop also
retires carriers parked before CPU bring-up or after hotplug.

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

With every FPCR trap enable clear, both engines run FP&SIMD data processing
under the guest's FPCR, and the cost of trapped mode stays out of that path.
While an enable is set, each FP&SIMD data-processing instruction runs on a
copy of the vCPU state with FPSR's exception flags and the host's enables
clear, and `darwin/cpu/arm64/detail/floating_exception.hpp` decides the
outcome for every engine. If the instruction raised an enabled exception,
it traps and commits nothing: FPSR, its destination and every other register
keep their values, flags it raised that are not enabled included. The
syndrome has TFV set and reports one cause, the first enabled one the
instruction raised in the order Input Denormal, Invalid Operation, Divide by
Zero, Overflow, Underflow, Inexact, whichever lane raised it. The
architecture leaves both choices to the implementation; this is the M1's
behaviour, measured on its native trapping. Otherwise the copy commits, with
every flag it raised. The decision follows FPRoundBase(): a flush to zero
sets FPSR.UFC but never traps, and with UFE set an exact tiny result raises
Underflow. An execution with UFE clear flags neither case correctly, so the
header decodes the form's output format and result elements, and evaluates
each step of an FMAXV, FMINV, FMAXNMV or FMINNMV reduction, which rounds
every intermediate maximum or minimum. Under FPCR.AH it also knows which
forms never flush (FMAX and FMIN) and which raise no Underflow of their own
(the estimate, reciprocal step and BFloat16 multiply-add forms).

The arm64 gadget code is one region in `darwin/cpu/arm64/gadgets/engine.S`:
the bank, the sections its gadgets push forwarding variants, cold paths and
FEAT_AFP twins into, and an island of veneers. Every image that carries it
links `UML::GadgetRegionLayout`, which merges those sections into
`__TEXT,__text` and orders them first, so the region starts the image's text
on a 16 KiB page and each part starts on its boundary plus the phase
`engine.S` defines. The region's bytes and image offsets therefore depend on
`engine.S` alone, and uml-darwin and the shipped and automation iOS runtimes
carry it identically; only the C++ code after it moves with unrelated changes.
A call to a helper outside the region branches to that helper's veneer, the
only words that encode a distance out of the region. Its private labels take
the Mach-O assembler-temporary prefix `L`, which keeps them out of the symbol
table; a branch to one in another part encodes its distance past the nearest
named symbol before it as a signed 24-bit addend, so such a target must lie
within 8 MiB of one, and the cold part names only its bounds and is 0.1 MB.
`gadget-engine-temporary-labels` fails on a `.L` symbol in either engine's
object. The FEAT_AFP twins follow every base part: they run only on a host
with FEAT_AFP, where they need the same fixed placement, and elsewhere their
position moves no base entry.

The bank holds no padding. A new entry or family is appended after the
established ones of its part, which moves none of them. Removing one re-packs
the region and moves every later entry, so a removal, like any gadget change,
is measured on the iPad and the phases are reselected if it costs time.
`arm64-gadget-region-layout` checks this contract on the images each tree
builds. An image linked outside this build, such as a consumer of the
XCFramework's static archive, keeps the parts as separate page-aligned
sections after `__text`.

The pin fixes the region and nothing else. The C++ code after it still moves
with unrelated changes, and dyld maps the runtime at an address that varies per
launch among a few clusters that move with the sizes of the images it loads.
A probe whose mode follows the load address, or is set by code outside the
region, can therefore still change its odds when an unrelated image changes
size, so an iPad layout comparison logs each launch's load address and counts
modes per arm.

An arm64 gadget stream carries no PC word for a memory transfer. The operand
of every load, store, pair, atomic, exclusive, ordered and SIMD structure form
ends in the instruction's position in its block, in its top byte, so a block
holds at most 256 instructions. A fault reports the position shifted left by
two with bit 0 set, which no instruction address and no translation key has,
and the boundary turns it into the PC from the block holding the faulting
stream, as `resolve_fault_position` does for A64; a T32 block, whose
instructions have two lengths, finds the instruction in the block's halfwords.
Only MOPS and a literal load whose target does not fit 48 bits keep a PC word.
`arm64-gadget-memory-fault-position` faults one instruction of each family at
several positions, directly and through a link.

A constant stored to a register is one stream word when it fits 48 bits or is
ones above bit 47: the destination's CpuState offset in bits 63:48 over the
value's low 48 bits, which MOVZ and MOVN take below a shift of 48, ADR and
ADRP (with the ADD that folds into them) and the emulated identification
registers when their value fits. Any other value keeps
`move_wide_constant` and its two words. `arm64-gadget-packed-immediate` runs
each form against the architectural definition.

A TBZ or TBNZ carries no mask word: its operand word holds the bit number in
bits 5:0, where the register shift that tests it reads it, and the register's
CpuState offset in bits 31:16, and an inlined branch's retire suffix sits in
bits 47:32. The `test_branch` entries of the former form stay in the bank
unselected, because removing them would move every later entry.
`arm64-gadget-test-bit-branch` runs every bit, both polarities, XZR and each
shape the builder emits.

A logical immediate on W registers shares the word that names its registers,
in bits 63:32 above the destination and source fields. A 64-bit immediate has a
word of its own, and a form whose destination is its source, which bakes the
register into the entry, has that word alone. `arm64-gadget-packed-immediate`
runs every W encoding and a sample of the X ones in each shape.

An SBFM or UBFM is a gadget pointer and one operand word: the two shifts that
compute it, whose amounts the builder derives from the encoding, and the
CpuState offsets of its source and destination. Only BFM keeps its two mask
words. The SBFM and UBFM entries of the former form stay in the bank
unselected, because removing them would move every later entry. A RET through
X30 is the gadget pointer and its indirect site; any other register keeps the
operand that names it.

The x86-64 gadget code in `darwin/cpu/x86_64/gadgets/` is a pinned region
too, a bank and a cold part placed by the same `UML::GadgetRegionLayout` and
order file. It follows the arm64 region in an image that carries both and
starts `__text` in one that does not, and each part starts on a 16 KiB page.
Each part is a run of fixed slots, one per group, from the table in
`engine/region.inc`; the groups are the common entry code, a veneer island
like the arm64 one, each baked family and each `engine/*.inc` file. A group
begins at its slot's start plus its 128-byte phase and every slot is whole
128-byte lines, so a group that changes size within its slot moves no other
group's bytes, and one that outgrows it moves the later groups by whole lines,
keeping every gadget's phase. The bytes between groups follow an
unconditional branch and are never reached. The region's bytes, but for the
island's branches out of it, therefore depend on the engine's sources alone,
and its image offset moves only in whole pages, when the arm64 region ahead of
it crosses one.
`x86-64-gadget-region-layout` checks this contract on the images each tree
builds, and the region layout section of the
[x86-64 gadget README](../darwin/cpu/x86_64/gadgets/README.md) says what a
change to a group moves.

Data-alignment faults retain the effective data address and the faulting PC.
The host exports a distinct bridge event, and both kernel lines deliver
`SIGBUS/BUS_ADRALN` so a guest handler can repair the access and resume. They
do not enter page-fault repair or retire the faulting instruction.

Both engines advertise USCAT exactly when the host implements FEAT_LSE2, and
then follow its alignment rule: an LSE atomic, an exclusive, or a
load-acquire/store-release access that is misaligned but lies inside one
16-byte block completes single-copy atomically, and only one that crosses a
block takes the alignment fault. A pair counts as one access of its total
size. Without host LSE2 every misaligned such access faults. Each executor
records the choice in its TLB. Gadgets execute the native host instruction on
an admitted address; the JIT applies the same rule in its semantic path, which
performs each such access as one host instruction, with LDAR and STLR for the
ordered forms and a CASAL loop for a misaligned read-modify-write. The rule
matches native LSE2 hardware for every form the virtual CPU implements: the
LSE, exclusive, LDAR/STLR, and LRCPC/LRCPC2 forms.

A load/store-exclusive loop makes progress whatever the TLB holds. Gadgets run
the host's own exclusives on the translated address, so the TLB miss of a
store-exclusive, a helper call, loses the host monitor, and the 4 KiB TLB is
direct-mapped: a plain store in the loop to a page that shares the target's set
would evict the target's write entry on every pass. One mechanism in
`engine/exclusive.inc`, which the AArch32 exclusives reuse, prevents that. An
exclusive load probes the write tag, and a miss fills the write entry of a
page that is writable now (`Tlb::translate_exclusive_load`, a read that faults
like any load) so the store-exclusive that follows hits. The load also records
the host address and the loaded values in the `CpuState` exclusive fields, and
every store clears them. A store-exclusive that still misses has lost the
host monitor to its own miss, so it completes in software: one host CAS (CASP
for a pair) of the recorded values, with status zero exactly when it
exchanged and a failure when the recorded address or a value differs. That
completion is value-based, so it cannot see a write that restored the value,
and it never runs when the store hits. A fault between the pair leaves the
loop for the kernel, whose next entry clears the record (an exception return
ends the monitor; `arm64-static-runtime` checks it), and the loop retries
from its head. The JIT's fused regions check both mappings before the
monitor, and its semantic path keeps a software reservation; its ordinary
stores clear the monitor, as the architecture permits.
`arm64-exclusive-progress` and `arm64-tlb` check the gadget contract on both
engines in both page profiles, and `guest-exclusive-progress-gadgets` and
`guest-exclusive-progress-jit` check it in a guest.

Both engines complete WFE and WFI at once and treat SEV and SEVL as no-ops,
so no wait ever depends on an event. Completing WFI is a deliberate deviation
from arm64 Linux, which traps WFI at EL0 and kills the program with SIGILL, so
no working program depends on either behaviour. AT_HWCAP therefore always carries
EVTSTRM, arm64 Linux's promise that WFE wakes at least once per event-stream
period, 100 µs, without an SEV.
`guest-gadgets-smoke` and `guest-jit-smoke` time a WFE loop against that
period.

The SME profile of the arm64 virtual CPU is FEAT_SME with FEAT_SME_F64F64 and
FEAT_SME_I16I64, plus FEAT_SME2 where the host has it, at the host's streaming
vector length (SVL), whatever that is: native execution needs the guest's SVL
to equal the host's, and EL0 cannot change it.
[`cpu/arm64/sme_profile.hpp`](../darwin/cpu/arm64/sme_profile.hpp) derives
the profile, the register-file layout and the SVCR transition rules for both
engines. The prober reports the profile under the guest-SME policy, which
is on by default wherever the host has the profile, at any SVL, on macOS and
iOS alike; `DARWIN_UML_GUEST_SME=0` forces it off, and then
ID_AA64PFR1_EL1.SME, ID_AA64SMFR0_EL1 and the SME HWCAP2 bits read zero on
every host, as they do on a host without SME. Each engine's `sme_support`
names how much of the profile it implements: the control core (`control`),
all of it but the SME2 forms (`complete`), or all of it (`sme2`). An SVE or
SME word it does not implement is the illegal-instruction exit. The default
needs both engines at `complete` or better, which `cpu/engine_factory.cpp`
asserts. A guest sees FEAT_SME2 only from an engine at `sme2`: the runtime
configures each engine, and derives the HWCAPs from the selected one, with
the host less FEAT_SME2 below that level (`cpu::engine_host_capabilities`).
Below it ID_AA64PFR1_EL1.SME reads 1, SMEver, I16I32 and BI32I32 read zero,
HWCAP2_SME2, SME_I16I32 and SME_BI32I32 are clear, and the ZT0 forms are
undefined.

Both engines are `complete`, so a guest is told of SME1 only: FEAT_SME,
FEAT_SME_F64F64 and FEAT_SME_I16I64. On the M4, SME1 against the policy
off made Geekbench 6's Object Detection, Background Blur and Photo Library
1.34x faster (geomean) on the gadget engine and 1.39x on the JIT, without
regressing its single- or multi-core score, while adding SME2's forms to
the gadget engine lost to SME1 (geomean 0.975), so the engines stay at
`complete` ([optimization history](optimization-history.md)).

Each vCPU's SME register file is one zeroed, 256-byte-aligned allocation of
the size its SVL needs, made at start-up only when the run implements SME and
reached through a `CpuState` pointer. An embedded block would have to carry
the largest SVL's 64 KiB of ZA in every vCPU. The file has the kernel's task
storage layout, Z, P, ZA, then ZT0, each part at a multiple of SVL, so Z and
P offsets fit an engine's 16-bit operand at every SVL and a ZA row is its
base plus the row times SVL. Reaching the file costs one pointer load per
engine entry or exit that moves SME state; a run without SME carries only
the null pointer. A write of SVCR that would set SM or ZA in a task without
SME storage retires nothing and leaves through the `sme_access` exit, which
the runtime reports to the kernel as its SME access event; the kernel
allocates the task's storage and enters again at the same PC. Each entry
loads the task's SVCR, TPIDR2 and storage from the bridge tail, and each
return writes them back, as the
[shared kernel README](../kernel/common/README.md) describes. Debug
builds trap when an engine is entered or returns with the host in streaming
mode or with ZA on; only a host with FEAT_SME in hardware reads SVCR. Only
the M4's SVL of 64 bytes has been checked on hardware, and the iOS SME path
is unvalidated on hardware.

The gadget engine implements all of the profile but the SME2 forms
(`sme_support` is `complete`). Its control core is SMSTART and SMSTOP, MRS
and MSR of SVCR and TPIDR2_EL0, RDSVL, ADDSVL and ADDSPL, LDR and STR of ZA
and ZT0, and ZERO of ZA and ZT0, with every size read from the vCPU's SVL at
run time. Its SME part follows the veneer island, so appending to it moves
no base entry and re-encodes no veneer call. A translation belongs to one SVCR mode, PSTATE.SM and
PSTATE.ZA: each mode has its own program cache and links, an SVCR write
ends its block, and a mode change empties the return ring. A program
translated with SM = 0 and ZA = 1 reaches ZA and ZT0 through a helper on the
register file. One translated with SM = 1 runs with the host in streaming
mode and, when ZA = 1, with guest ZA, and ZT0 under SME2, resident in the
host's; a helper call leaves streaming mode around itself, spilling and
refilling them, so no C++ runs in streaming mode. XNU resets FPSR on every
kernel entry while a thread is in streaming mode, so a streaming program
keeps the guest's FPSR in `CpuState` and never runs with the native FP
environment active: only the guest's FPCR, with its trap enables clear, is
the host's while it runs, and MSR FPCR runs as a twin that sets both
copies. Such a program may select
only the entries that `engine/sme.inc` lists: the control core, the base
gadgets that execute no AdvSIMD instruction and call no helper, and the
streaming twins of the base GPR gadgets that do. A twin is its base gadget
assembled a second time from the same source, as `uml_arm64_gadget_sm_`
plus the base name, with each helper call made through a trampoline that
leaves streaming mode around it and each AdvSIMD word of a slow path
replaced by an FMOV; CSSC CNT's twin counts in GPRs. A table maps each base
entry to its twin, and the builder selects the twin. Twins cover the integer
loads, stores and pairs, atomics and exclusives, DC ZVA, IC IVAU, the MOPS
forms and CSSC CNT, so their hot paths run in streaming mode unchanged and
only a TLB miss, a page crossing or an alignment fault leaves it. A
streaming program also runs every register-only SVE, SVE2 and SME form of
the SME1 profile, and every FP&SIMD data-processing class legal in streaming
mode, from gadgets that `engine/generate_streaming.py` writes at build time
from the encoding table, one per value of a form's bits that are not
register numbers. Each loads the registers its instruction names from the
register file at offsets the builder computes from the SVL, or from
`CpuState` for general registers, into fixed host registers, runs the
instruction on them and stores its destination back. A form whose
immediates would take hundreds of gadgets runs an equivalent register form
on an immediate, or a count from the SVL, that the builder computes. A
gadget that may change NZCV runs with the guest's flags in the host's, and a
floating-point one ORs the exceptions it raised into the guest's FPSR, and
runs again if FPSR.QC shows that a kernel entry reset the host's. RDVL,
ADDVL and ADDPL run as RDSVL, ADDSVL and ADDSPL. Every SVE and SME1 load and
store, and every SIMD&FP load and store legal in streaming mode, runs from
a generated vector memory gadget, one per instruction the host executes for
it (shape, direction, sizes, extension, ZA tile and slice direction), with
the address mode, registers and shape resolved by the builder into the
stream `darwin/cpu/arm64/gadgets/sme_layout.h` describes. Its fast path
forms the address, and when every byte the access spans lies in one guest
page the TLB holds, runs the native instruction on registers loaded from the
register file. Anything else takes a helper that does the whole access on
the register file: it touches only active elements, translates every page
they lie in before it moves a byte, and reports a fault as M4 hardware does,
a predicated access at its first faulting active byte rounded down to
4 KiB and an unpredicated one at that byte. SVE prefetches are NOPs. The
builder ends a block at the illegal-instruction exit on any other word:
AdvSIMD, a word outside the profile, or an SME2 form, which the guest is
not told of.
`arm64-gadget-streaming-safety` walks every listed entry, and every 16-byte
boundary of the generated gadgets, in the images each tree links and fails
on an AdvSIMD word executed in streaming mode, a helper called there, or an
indirect call.

The JIT is `complete` too. Its control core runs SMSTART and SMSTOP, MRS
and MSR of SVCR and TPIDR2_EL0, RDSVL, ADDSVL and ADDSPL, and LDR, STR and
ZERO of ZA and ZT0. It keys every translation, link
and indirect site by the guest's SVCR mode as well as its PC, so a block runs
only in the mode it was translated for; mode 0 translations are unchanged. A
guest write of SVCR ends its block. An SM=1 translation keeps the guest's Z,
P, ZA and ZT0 in the identically numbered host registers: its external
prologue enters streaming mode and fills them from the register file, and
every epilogue and host helper call first leaves streaming mode and spills
them. Helpers are terminal, so no SM=1 code runs after one, and a TLB miss
takes a streaming bridge that leaves and re-enters around the walker. The
transitions preserve NZCV, and SM=1 code emits no AdvSIMD instruction of its
own. An SM=1 translation also runs every register-only SVE, SVE2 and SME form
of the SME1 profile, and every FP&SIMD data-processing class legal in
streaming mode, as the guest's own word, with each general register it names,
and a ZA slice index, renamed to a host register that holds the guest's.
MOVPRFX runs as a move of its own. A form that may change NZCV runs with the
guest's flags in the host's. XNU resets FPSR on every kernel entry while a
thread is in streaming mode, so the guest's FPSR stays in `CpuState`: the
streaming entry installs the guest's FPCR with trap enables clear, which
kernel entries keep, MSR FPCR writes both copies, and the exit restores the
host's. A floating-point form runs on a cleared FPSR, ORs what it raised into
the guest's, and restores its destination and runs again if FPSR.QC shows
that a kernel entry reset the host's. Every SVE and SME1 load and store
runs as the guest's own word, its base renamed to the host address and its
offset zeroed, when every byte it can span, whichever elements are active,
lies in one guest page the TLB holds. A crossing, a TLB miss, a failed
translation, a misaligned SP base or a tagged base takes a terminal helper
that does the whole access on the register file: it touches only active
elements, translates every page they lie in before it moves a byte, and
reports a fault as the gadget engine does. The SIMD&FP loads and stores
legal in streaming mode run the same way, with any writeback after the
access. SVE prefetches are NOPs. SME2, the reserved bitmask immediates of
ORR, EOR, AND and DUPM, every other SVE and SME word, and in streaming mode
AdvSIMD and the FEAT_LSFE and FEAT_LSUI loads and stores are its
illegal-instruction exit; every other instruction it runs at SVCR 0 runs in
streaming mode too. SM=0 translations run with the host's SME off and
hold no SVE or SME instruction: they reach ZA and ZT0 in the register file
with GPR copies. SM=1 code reads SVL with RDSVL and SM=0 code from
`CpuState`, so no translation depends on it.

The arm64 virtual CPU has no SVE outside streaming mode, whatever the host
reports, and advertises none: ID_AA64PFR0_EL1.SVE and ID_AA64ZFR0_EL1 read
zero, the SVE HWCAP bits are clear, and `platform::without_sve` clears the
SVE tokens from the prober's and every engine's presented profile. The
engine-neutral vector length of 16 bytes, the mask form of a predicate and
`CpuState`'s P0-P15 and FFR remain in
[`cpu/arm64/sve_profile.hpp`](../darwin/cpu/arm64/sve_profile.hpp) as the
reserved storage the SM = 0 family and streaming mode share: every change of
PSTATE.SM zeroes them, and no instruction writes them. Every SVE word outside
streaming mode, other than the family below, is the illegal-instruction exit
in both engines, so a binary that computes with SVE outside streaming mode
takes SIGILL. A reserved bitmask immediate is the illegal-instruction exit
everywhere. The generated encoding table keeps every SVE row, with the
non-streaming requirement of the rows no engine executes outside streaming
mode unimplemented.

One deliberate departure from the architecture covers a virtual CPU with
FEAT_SME, which is every one that implements SME. CNTB, CNTH, CNTW and CNTD,
RDVL, ADDVL, ADDPL and the scalar INCB, INCH, INCW, INCD, DECB, DECH, DECW and
DECD, with every predicate pattern and multiplier, execute at PSTATE.SM = 0
for one constant vector length of 128 bits for the whole process (CNTB = 16,
CNTD = 2), and so do LDR and STR of a vector register Z and of a predicate
register P with a scalar base and an immediate offset (MUL VL), the
compilers' callee-saved spills, which move 16 and 2 bytes. The architecture
makes every SVE word UNDEFINED there on such a CPU and an M4 traps them.
Compilers target armv9-a, which implies SVE2, so a locally-streaming function
reads the non-streaming vector length with CNTD before it enters streaming
mode, and would take SIGILL on every host that advertises SME; one that must
keep Z8-Z23 and P4-P15 across the mode change spills them first. Z_n is V_n
outside streaming mode, so a Z access is the 16-byte access of the V
register. The CPU has no predicate state there, so STR of P stores two zero
bytes and LDR of P performs its access, faulting as a load does, and drops
the data. In streaming mode the same words keep their architectural meaning
with the streaming vector length, and the ID registers and hwcaps still
report no SVE. Every other SVE word at SM = 0 stays the illegal-instruction
exit, so a binary built for SVE and SME that computes with SVE outside
streaming mode (its data processing, contiguous and gather loads and stores,
and predicate construction) still takes SIGILL: the family keeps SME code
running, and is no promise to run SVE code.
[`cpu/arm64/detail/sm0_vector_length.hpp`](../darwin/cpu/arm64/detail/sm0_vector_length.hpp)
defines the family once: both engines decode a vector-length word to the A64
word of the same effect (MOVZ, MOVN, ADD, SUB or NOP), so no translation of
one touches the FP or SVE state, and a spill word to the SIMD&FP or halfword
access it is, with the offset in the translation, since a Z offset reaches
four times as far as an unscaled A64 access. The JIT's slow path for a spill
word that crosses a page or misses the TLB is
[`cpu/arm64/detail/sm0_spill.hpp`](../darwin/cpu/arm64/detail/sm0_spill.hpp),
which works on `CpuState` alone. No hardware observes the results, so the
corpus holds them as project-defined (`arm64-sm0-vl`).

The guest kernel learns the same answer from the HWCAPs and the ID registers:
the bridge imports no SVE vector length and its register tail holds FP and SME
state only (bridge ABI 23), so a task has no SVE state to carry across an
entry.

The arm64 engines are growing a 32-bit Arm guest: AArch32 EL0, A32 and T32
with interworking and IT, for the armhf userspace of Debian and Android's
armeabi-v7a, which needs hardware VFPv4-D32 and Advanced SIMD, so there is no
soft-float-only profile. The profile, derived from the host as the SME profile is, is in
[`cpu/arm64/aarch32/a32_profile.hpp`](../darwin/cpu/arm64/aarch32/a32_profile.hpp).
Each engine's `a32_support` names how far it implements it, `none`, `base`
(the integer core), `simd` or `complete`, and `base` and `simd` are
milestones, not products. `CpuEngine::run_aarch32` is the entry for a task in
AArch32 state, the A64 `run` is unchanged, and an engine below `base` takes the
new entry as the illegal-instruction exit without touching the state. The
gadget engine and the JIT are both at `complete` on macOS. The development
policy `DARWIN_UML_GUEST_AARCH32=1` is off by default and adds no host token;
`a32_presented_support` gives the level a run presents, nothing with the
policy off and the engine's level with it on, and the AArch32 identification
registers and the compat AT_HWCAP and AT_HWCAP2 words derive from that level
and the host in `cpu/arm64/id_registers.hpp` as arm64 Linux derives them, and
are zero below `base`. The default turns on once both engines are complete.

An AArch32 task keeps R0-R14 zero-extended in the integer registers, NZCV in
the A64 flags and D0-D31 in the low sixteen vectors; what has no A64 field is
appended to `CpuState` after the reserved SVE block (GE, the sticky Q flag, ITSTATE,
FPSCR's flags, TPIDRURO, CP15BEN and an entry marker), so no earlier offset
moves. The PC is a translation key
([`cpu/arm64/aarch32/guest_key.hpp`](../darwin/cpu/arm64/aarch32/guest_key.hpp))
for the whole entry, which lets one gadget program cache, one JIT arena and
one shared tier serve A64, A32 and T32 without a second instance of any of
them. An A64 key is the address with the SME mode in its two low bits. An A32
key is bit 63 with the word address; a T32 key adds bit 62 and shifts the
halfword address left one bit, so that bit 2, where every cache hash starts,
varies between adjacent instructions. A branch target's bit 0 selects T32, so
the key carries PSTATE.T and no stub switches the state. A valid AArch32 key
has both low bits clear, so no key reaches the SME mode the JIT reads from an
A64 key, is the all-ones empty cell, or is a fault position, and an A32 target
with bit 1 set is a PC-alignment exit before it is a key. The IT state is not
part of a key because no block ends inside an IT block. A translation records
its ISA and counts its saved guest units in that ISA's unit, a word for A64
and A32 and a halfword for T32, so one page-bounded validation serves the
gadget cache's validating lookups, the JIT's stale lookups and the shared
tier, whose entries must carry the ISA of the key they are stored under.

The gadget engine's AArch32 part translates A32 and T32 with the shared
decoder into the data-only streams A64 uses, so one executor, one program
cache and one execution boundary run both
([`gadgets/a32`](../darwin/cpu/arm64/gadgets/a32/a32_builder.hpp);
`run_translated_aarch32_blocks` in `cpu/arm64/engine_execution.hpp` is the
loop an engine's `run_aarch32` takes). Its gadgets come from the decoder's
table by `engine/generate_a32.py` and `engine/a32.inc`, in a bank, a cold
section and a veneer island of their own after the SME part, so no A64 entry
moves and the part holds no SVE or SME word. A conditional instruction is a
skip over a run of instructions sharing its condition, a conditional
terminal gives the skipped instruction back, and a block never ends inside an
IT block: one the block limit cuts off ends in an unlinked exit with ITSTATE
stored. A block holds at most 64 instructions (`a32::block_limit`); a T32
block stops five short of that unless an IT block is open, so that an IT
block it starts completes inside it. Conditional data processing that sets
no flags and does not write the PC (MOV, MVN, ADD, SUB, RSB, AND, ORR, EOR
and BIC of an immediate or a register, MOVW, and VMOV between S or D
registers) is a select instead: its gadget computes the result and keeps the
old value where the condition fails, so nothing branches on the guest's
flags. A plain CMP of a register or of an immediate below 2^24, outside an
IT block and followed at once by a conditional branch, is the A64 fused
compare-and-branch gadget of the branch's condition, which reads the
CpuState slots a register lives in, so the pair costs one dispatch. An
unconditional VCMP or VCMPE followed at once by an unconditional VMRS
APSR_nzcv is likewise one gadget that sets FPSCR's flags and copies them to
NZCV. ADD and SUB of an immediate to the same register have an entry per
register R0 to R14 with the displacement baked in. A word the decoder rejects, a coprocessor word the profile lacks and
a form above the engine's level end the block in the illegal-instruction exit
at the word whatever its condition, except a coprocessor word that is not
UNPREDICTABLE in the complete profile, which a failing condition skips like
any other instruction. SVC, BKPT and a retiring exit count their instruction;
a fault or undefined exit does not. SVC leaves PC after itself and ITSTATE
advanced; a fault, UDF or BKPT leaves PC at the instruction with its own
ITSTATE, which a T32 block recovers by decoding its saved halfwords again. An
entry with ITSTATE set, and a single step, run one uncached instruction at a
time until ITSTATE is zero. The parallel add and subtract family and SEL are
gadgets: Advanced SIMD does the lanes on the low half of a vector register, GE
is the byte-lane mask CpuState keeps, and the saturating forms compute their
saturation with lane arithmetic, because the saturating instructions would set
FPSR.QC, which the guest reads in FPSCR. LDM, STM, PUSH and POP that cross a
page, an STM that stores the PC, and the rare integer forms (USAD8, the dual
multiplies, the 16-bit saturates and extends, QADD and its relatives), run
through the shared AArch32 semantic library
(`cpu/arm64/aarch32/semantics.hpp`): the `a32_library` gadget and the
crossing paths spill NZCV, decode the word again and call `execute_a32`,
which translates every page before a load changes a
register, stores the words ahead of a faulting STM and leaves the base
register as it was. LDREX and STREX, and LDAEX and STLEX, are the A64
exclusive gadgets and share their forward-progress mechanism; LDA and STL are
the A64 ordered ones. Like them, the single and doubleword transfers, VLDR,
VSTR, TBB, TBH and the block transfers carry no key: their operand holds the
instruction's position in bits 31:24
(`darwin/cpu/arm64/gadgets/a32_layout.h`), which a fault reports as the A64
transfers do, and a block transfer's crossing path passes it to the library
in place of the key. The library-exit counters name any lowered form that
reaches the library, and the unit tests require none does.

VFP and Advanced SIMD are lowered onto the A64 instructions with the same
lanes. D0-D31 and S0-S31 are byte offsets into `CpuState::vectors`, and a
gadget stores only the bytes the A32 destination has. VFP runs under the
guest's FPCR in the native FP environment. Advanced SIMD floating point runs
under the standard FPSCR value (flush to zero, default NaN, round to nearest):
each such gadget starts with `a32_fp_standard` and each VFP gadget with
`a32_fp_guest` (`engine/a32.inc`), which write the host FPCR only when
`CpuState::a32_fp_standard` says the native environment holds the other one,
so a loop of one kind switches once and the execution boundary restores the
host's FPCR whichever it holds. A switch costs more than the instructions
around it, so the forms that interleave the two kinds in image and signal
code run in the active environment when their result is the same in both and
switch only for operands where it is not: the VFP conversions between floats
and integers (all but a denormal float to integer, and an integer to float
under a rounding mode other than round to nearest), VMAX and VMIN (a NaN or a
denormal), and the single-precision VADD, VSUB and VMUL of both kinds (a
nonzero operand whose exponent field is outside 64 to 191, which keeps a sum,
a difference and a product normal, and a guest rounding mode other than round
to nearest). The test is integer arithmetic on the operands' bits, made
before the instruction so that it raises no flags of its own. The A32 shifts by an immediate use the A64
shift-by-register instructions with a replicated signed amount, so a gadget
serves every amount; the forms that exist in A64 only with an immediate
(SQSHLU, the converts with a fraction width, VEXT) have a gadget per
immediate. VLDn and VSTn are native (`engine/a32_structure.py`): a copy for
VLD1 and VST1 of several registers, LD2 to LD4 and ST2 to ST4 for the
interleaved ones, element copies for one lane and LD1R to LD4R for all lanes,
with the register or immediate writeback; a VLD1 or VST1 copy that crosses a
page stays in the gadget and moves its bytes with the A64 slow transfer helper,
and one without an alignment hint has a gadget for each writeback that tests
nothing the instruction does not need. A copy with an alignment hint and a
VLD1 to all lanes of one or two registers have gadgets of their own too, which
test the alignment and leave a misaligned access to the library. An
unconditional ADD or SUB of an immediate that sets no flags, names no PC and
computes the base of a copy without hint or writeback runs inside the copy's
gadget when only unconditional Advanced SIMD data processing that reads no
core register comes between: the gadget writes the register before the
transfer, so a fault finds it as the ADD left it, and nothing between can
exit or fault.
VRECPS and VRSQRTS (AArch32's unfused steps, not the fused A64 ones), an
interleaved or one-lane structure access or VLDM and VSTM that crosses a page,
and a misaligned access with an alignment hint run
through the semantic library, whose exit commits the live FPSR to
CpuState before the call and reinstalls it after, as the A64 engine's
software floating-point helpers do. A saturating instruction leaves QC in the
cumulative FPSR flags the native environment keeps; `engine/a32_neon.py` and
`engine/a32_simd.py` document the groups and the operand words.

The complete level adds the Armv8 floating-point additions and the optional
extensions. The engine decodes in the profile its host gives it
(`a32::decode_profile`): the extensions the host has the A64 feature for
(`a32_features`), which are the ones the guest is shown, so a form of any other
extension is UNDEFINED, as the HWCAPs say. The generator
(`engine/a32_armv8.py`) lowers each shown extension onto the A64 instruction
with the same lanes: CRC32, AES, SHA-1 and SHA-256, VMULL.P64, SB, VSEL,
VMAXNM, VMINNM, VRINT and VCVT with a rounding mode, the double/half
conversions, the half-precision VFP forms (arithmetic, compares, conversions,
VMOVX and VINS), half-precision Advanced SIMD, the dot products, VQRDMLAH and
VQRDMLSH, VCADD, VCMLA and VFMAL and VFMSL. A decoded form with no gadget is
the semantic library's, run through the `a32_library` gadget: the BF16 and
Int8 matrix forms and VJCVT, on a host that has them. The FP16 VFP forms share
the single-precision plans (aux bit 2) and store a half whole through its S
register, which a half result zero-extends into.
The level the engine translates at is `a32_support`; a test or a bring-up
names another one in `BuildOptions::a32_level`, and the runtime never does.
`arm64-gadget-a32-control` checks the contract above, `arm64-gadget-a32-forms`
that the engine defines exactly what the decoder defines at its level,
`arm64-gadget-a32-corpus` that it matches the AArch32 corpus records of that
level, and `arm64-gadget-region-layout` the part's placement.

The runtime enters an AArch32 task through `run_aarch32` only while the run
presents AArch32, which `guest_a32_support()` decides in one place: the
selected engine's level under the development policy. The compat
`AT_HWCAP` and `AT_HWCAP2` words the guest selector returns derive from the
same level, so a run advertises exactly what its engine runs, a non-zero word
is the kernel's whole evidence that a run executes AArch32, and an AArch32
entry the run does not present is a fatal event. The conversions live in
[`runtime/aarch32_guest.hpp`](../darwin/runtime/aarch32_guest.hpp) and the
formats of the status word and FPSCR in
[`cpu/arm64/aarch32/state_format.hpp`](../darwin/cpu/arm64/aarch32/state_format.hpp).
Loading takes (PC, T) to the key; NZCV, Q, the GE flags (as the byte-lane
mask SEL needs) and ITSTATE out of the SPSR-layout status; TPIDRURW, TPIDRURO
and CP15BEN; and FPSCR's two halves, with FPSCR's N, Z, C and V in
`a32_fpscr_nzcv` because the hosts' FPSR[31:28] is RAZ/WI and its control
bits cut to AHP, DN, FZ, RMode and FZ16 (no trap enables, no short vectors).
It clears the exclusive monitor and top-byte ignore and leaves the vector
registers lazy, as for A64. Saving repacks only what an engine owns (flags,
GE, IT, T, the PC) and keeps the kernel's own status bits, writes R0-R14
zero-extended and leaves the other registers of the bridge alone. An entry
is fatal unless the status is User mode with E clear, IT comes with T, and
the task is neither streaming nor holds live SVE registers. An engine's exit
becomes the bridge event for a 32-bit task: BKPT carries its immediate and
the BKPT32 exception class, a completed single step the software-step class,
and a PC alignment fault the 32-bit target address.

The JIT lowers AArch32 in `cpu/arm64/jit/a32`, beside the A64 compiler and
without changing it. An A32 or T32 block is decoded by the shared decoder and
compiled by its own compiler, which reuses the A64 compiler's register
residency, flag machine, cold paths, direct and indirect links, tracked stores
and publication: R0-R14 are resident host registers (each core register has
one: x3-x7, x12, x13, x17 and the callee-saved x22-x28, which an AArch32
translation's frame saves and the A64 translations never touch). A linked
translation keeps them there from block to block: the registers are loaded
before the block's internal entry, which only the executor reaches, a link
leaves them in their hosts, and every return to the executor, a refused link
included, writes all fifteen back; its link sequences therefore use x10 and
x11, and no lowering may use a host register of a core register as scratch.
An unlinked translation (the tracked-store variant, a single instruction) loads
at first use and writes back at every exit. NZCV is the host's with the lazy-flag states of the A64 compiler, a
condition is a branch over the body (a conditional data-processing instruction
that sets no flags and writes no PC, and MOVW, instead runs on both arms and
a CSEL on the host flags keeps its result only where the condition holds, so
no data-dependent branch is left to mispredict), and a link's target is the
key. Every exit
whose successor is static (a branch, the fall-through of a block the 60
instructions of a window cut short, the arm of a conditional instruction or
CBZ that is not taken) is a link while the ITSTATE that follows is zero, so a
straight run chains like a loop. A chain states what is set up for the whole
run, not what its first block needs: the floating-point state is loaded, and
the native environment is on whenever any earlier block switched it on, so a
chain that began at an integer block links into a floating-point one and back
instead of returning to the executor at each edge. An indirect branch of
AArch32 code looks in its site's two cells and then in the executor's
direct-mapped table of the targets its misses have trained (4096 entries,
tagged with the link epoch so that the translation of the same address in
another address space or after a code change never matches), and returns to the
executor only when both miss: a function with many callers, such as libm's
`lroundf`, has a return to each of them. A conditional branch (B with a
condition, CBZ, CBNZ) does not end a block: the block goes on with its
registers and flags in the host, while the taken path leaves by a cold side
exit that publishes what is dirty at the branch and links to the target. A
branch back is a loop's end or a `continue`, and the side exit costs the link
of the taken path either way. An unconditional branch ends the block, and so
does a conditional one back to the translation's own first instruction: that
block is a loop in the host. A probe compilation records the vector registers it holds at
the back edge and the core registers the body names, the
translation loads them before the loop head and counts
every one as dirty so that each exit writes them back, and the back edge is the
chain budget's test for one more iteration and a branch. The iteration that
cannot be afforded leaves like any linked exit. Memory
takes the inline TLB sequence after a 32-bit address and the alignment
rule of the form (on 4 KiB pages the entry index comes from the address's own
bits and the tag compare shifts the address, two instructions fewer than the
A64 compiler's). An access already checked to be aligned to its size (a
single-precision VLDR or VSTR, a Neon transfer whose alignment hint covers its
bytes, an ordered or exclusive access) is not tested for crossing a page. On
4 KiB pages any other access of more than one byte compares the entry's tag
with the page of its last byte, so a crossing access misses (the index never
gives neighbouring pages of a 32-bit address space one entry) and its cold path
tells it from a miss; 16 KiB pages test the offset before the probe. A
single load or store of a byte, halfword or word does not add the entry's
delta to the address in a register of its own: the access takes the delta in
its addressing mode (`[x9, w8, uxtw]`), and the miss path leaves the host
pointer in x9 and zero in x8; a pair, a multiple transfer, an acquire or
release and a store that records its host range keep the host pointer in x8.
The Armv8 load-acquire and store-release (LDA, STL and their
byte and halfword forms) are an alignment-checked LDAR and STLR. A
load-exclusive, up to four register instructions and the store-exclusive
(acquire and release forms included) on the same address at the start of a
block become one host load-exclusive and store-exclusive, so the guest's
monitor is the host's; a failed store leaves the status to the guest's retry
loop. A conditional forward branch or CBZ/CBNZ between the two (the
compare-and-swap that leaves on a mismatch, the try-lock) stays in the run as
a side exit whose taken arm clears the monitor and links to the target. The
parallel add and subtract forms run in the vector unit on the
packed lanes (UMAX and UMIN stand in for the unsigned saturating forms and a
widening add for GE, so the host's QC flag is never set). What the lowering
declines (an access across a page or one that must fault, an exclusive that
does not fuse, the signed saturating parallel forms, SSAT16, USAT16 and USAD8)
leaves the block through `uml_arm64_jit_a32_execute`, a terminal helper that
runs the one instruction in the AArch32 semantic library over the TLB, with
the library's fault ordering. The helper restores the host's FPCR and FPSR
unless the instruction is a memory transfer or integer arithmetic, which
leave the guest's native environment installed for the next block. The
exclusives run there too, over the JIT's versioned software reservation: the
helper overrides the library's monitor hooks, because the guest monitor is
shared with the tracked stores and the fused host exclusives and the library's
default compare-and-swap monitor is not. An entry in the middle of an IT block
and a single step run one instruction in the single-instruction arena, with
the IT state the exit recorded. Translations validate their saved guest bytes
in the ISA's unit, so a halfword of a 32-bit T32 instruction rewritten under a
translation is seen. Executable memory comes only from the arena the A64 JIT
already uses; the A32 files allocate and publish none.

The JIT's AArch32 vector support (VFP and Advanced SIMD on D0-D31) keeps Q0-Q15
in the A64 compiler's vector residency: CpuState vectors 0-15 are the resident
host registers V16-V31, a D register is the low or high half of its Q and an S
register a 32-bit lane, so a narrower operand is staged in a scratch register
and a result is inserted into its half or lane. An insert rewrites the whole Q,
so in a run of unconditional single-precision VFP instructions an S register
that was read from its lane or written also keeps a copy, a shadow, in one of
V0-V4 and V7, which the run's later reads take instead of the lane: operations
on different lanes of one Q do not wait for each other's inserts. The Q
register still takes every write, so an exit, a link and a cold path need no
shadow; a conditional, double-precision or Advanced SIMD instruction, or any
other that may write an S register or use those host registers, forgets them,
and the TLB miss bridge preserves V0-V7. A VFP instruction runs under the
guest's FPCR and FPSR, which the executor installs for a block that holds one
(the native floating-point environment of the A64 compiler); an Advanced SIMD
floating-point instruction runs under the Standard FPSCR value (FZ and DN set,
round to nearest, AHP and FZ16 the guest's). The host FPCR keeps the mode it
has across exits and links, as the gadget engine's does:
`CpuState::a32_fp_standard` records it, the executor clears it where it
activates the environment, and a translation tests the byte at the first
instruction that needs a mode and writes the FPCR only when the mode differs.
The two modes part only on a nonzero denormal operand, a NaN operand, a
result that is tiny before rounding and a rounding mode other than to nearest,
so a Neon VMAX, VMIN, VADD, VSUB, VMUL, VMLA or VMLS of single precision in a
run of at most eight Neon floating-point operations that a VFP instruction
(or the end of the block) follows runs under the guest's FPCR behind integer
tests of its operands, and under the Standard value from a cold path when a
test fails; a longer run switches once, and so does a loop that holds such
operations and no VFP instruction or FPSCR write (the probe compilation counts
them): it sets the Standard value before its head and runs every lap unguarded.
A run under the guest's FPCR leaves its cumulative
exception flags behind, so what only it would raise is found before it: a
product whose operand exponents sum to less than 129 (it may underflow,
inexactly) is tested before the multiplication, and a sum needs no such test
because a tiny sum of normal numbers is exact. A saturating
instruction sets QC in the host FPSR, which is the guest's. A VCMP
leaves its flags in FPSCR's copy in `CpuState` and in the host NZCV, so an
unconditional VMRS APSR_nzcv, FPSCR right after it keeps them there and
the branch on them waits for no reload. VRECPS and VRSQRTS
round the product before the step, as the AArch32 pseudocode does and the A64
instructions do not, and answer 2.0 or 1.5 for an infinity times a zero
without raising Invalid Operation. VLDR, VSTR and the register-list transfers
move whole Q registers where the list is aligned to one; the structure loads
and stores use the host LDn and STn on scratch registers. They share the
scalar transfers' TLB sequence and alignment rule. A faulting alignment, a PC
base or a list past the register file runs the instruction in the semantic
library instead, as does a store that crosses a page. A load of at most 32
bytes that crosses a page stays in the translation: its cold path probes the
TLB for both pages and, when the two mappings are adjacent in the host, reads
through the first as within one page, and otherwise copies the last 32 bytes
of the first page and the first 32 of the second side by side into
`CpuState::a32_cross` and reads from there at the guest's offset. A page
missing from the TLB takes the library, which fills it.

At the complete level the JIT presents the Armv8 extensions as well, each where
the host has its A64 feature: it decodes against `a32_decode_profile`, the
extensions the guest is shown, so a form of an extension the host lacks is
UNDEFINED and never reaches the semantic library. It lowers the ones that are
the host's own instruction on the same operands: CRC32, the AES and SHA-1 and
SHA-256 instructions, VSEL (FCSEL on the guest flags), VMAXNM and VMINNM, VRINT
and VCVT with a named rounding (scalar under the guest's FPCR, Advanced SIMD
under the Standard FPSCR value), the signed and unsigned dot products, VQRDMLAH
and VQRDMLSH, VCADD and VCMLA (half precision as well, with FP16) and SB (a DSB
and an ISB). VJCVT is FJCVTZS with its Z flag taken into FPSCR; on a host whose
FJCVTZS reports a flushed subnormal as exact, the A64 JIT's correction (a
subnormal input is never exact) applies here too. A D-register form that A64
has no 64-bit arrangement for (single-precision VCMLA by element) runs on the
128-bit arrangement with the doubleword in both halves, which raises the same
flags. The rest of the level runs in the semantic library: the other
half-precision forms, the fused widening half-precision multiply-accumulates,
the BFloat16 and 8-bit matrix forms and the mixed-sign dot products, and a
half-precision variant of an Advanced SIMD floating-point form that the
compiler does not lower, which shares the form of the single-precision one and
differs in the operands' esize.

The macOS 4 KiB x86 compatibility slice keeps native arm64 UML kernel-mode
execution while exposing the x86_64 and i386 userspace ABIs. It cannot load
arm64 userspace; per-task mode selects only an x86-family operand and address
width. As on x86 Linux, a task always runs with RFLAGS.IF set: the host sets it
on entry, whatever the kernel's register image holds, and the kernel keeps it
in that image as the [mainline README](../kernel/mainline-6.18/README.md)
describes. Register, YMM, MMX, and
x87 state is serialized in the task-owned bridge frame across carrier
scheduling. The host routes userspace instruction execution through immutable,
data-only streams of pointers to precompiled handwritten arm64 assembly
gadgets. There is no semantic C++ execution fallback. Two gadget banks serve
scalar integer instructions: an ordinary bank that decodes its operands from
the stream, and a register-baked bank generated at build time in which every
register offset is an immediate. A store publishes a code change only when a
per-address-space registry says some executor holds a translation of that
page, which keeps publication precise where READ_IMPLIES_EXEC makes every
readable page executable. The static MMU is as precise: a mapping change
publishes a code change only for a page a translation can be wrong about,
so a fork's write-protection of a process publishes none. The banks, the
host-flag fusion rules and the registry contract are in
[`darwin/cpu/x86_64/gadgets/README.md`](../darwin/cpu/x86_64/gadgets/README.md).
What any x86 engine shares with the gadget engine is not part of it: the guest
CPU state, the decoder, CPUID, the software TLB entry, the code-page registry,
the split-operand lock and the segment model, which runs every selector load,
far transfer and descriptor inspection for both engines, form
`uml_x86_64_target` in `darwin/cpu/x86_64/`, a
`shared` CPU target that the source audit keeps free of engine includes.
Exact x87 formats and transcendentals use bounded software-math primitives
called by those native gadgets. The userspace ISA boundary includes scalar x86
and x87/MMX, SSE through SSE4.2, AVX/AVX2,
F16C/FMA, AES/PCLMUL, BMI1/2, atomic and extended-state operations in both
long64 and compat32 modes. The pinned FEX handwritten x86-64 and i386 corpora
are the external instruction acceptance gate. AVX-512 and later encodings,
RTM/MPX, obsolete protected-mode bounds, 16-bit code segments, and
privileged, real-mode, virtual-machine, enclave, and system-management
instructions are outside this Linux userspace profile, and AMD's 3DNow!, SSE4a
and CLZERO raise #UD, as on an Intel core. That gate is not proof of an
arbitrary distribution or complete Linux syscall compatibility. The normal
mainline slices and Android remain arm64-userspace-only and contain
none of the x86_64 kernel overlay.

On macOS the x86_64 product can instead run long64 and compat32 tasks on the
x86-64 JIT, FEXCore embedded behind the static MMU, which
`--execution-engine jit` selects. The product's default is the gadget engine,
by name; `--jit-tier=N` runs each address space on the gadgets for its first N
instructions and on the JIT after that. The
[JIT README](../darwin/cpu/x86_64/jit/README.md) records its contracts and
known gaps. The JIT is a macOS product only, and four independent checks keep
it out of every iOS product: only the `macos` dependency prefix carries
FEXCore; configuration refuses `UML_BUILD_X86_64_JIT` for any other platform;
in a tree whose `UML_APPLE_PLATFORM` is not `macos`, `architecture-audit`
rejects the `uml_x86_64_jit` target, a target compiling the engine or its
runtime seam, a compiled source that includes `cpu/x86_64/jit/`, and a link
fragment naming FEXCore; and `scripts/check-no-fexcore.py` fails the
XCFramework, Kernarium framework and Kernarium app packaging on any FEXCore
symbol or FEXCore's code-mapping names.

The [Steam rootfs](../tools/rootfs/steam-x86_64/README.md) runs Valve's native
x86-64 Linux client on this x86_64 userspace product; its producer owns the
image, its persistent data disk and its startup contract.

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
validation. On arm64, an address space's bodies and link state are shared among
every executor running it, while each executor keeps its own slots and
retirement order; `DARWIN_UML_GADGET_SHARED_BODIES=0` keeps them private, as
[`../darwin/README.md`](../darwin/README.md) records. An address space a
single executor keeps to itself sweeps its link records and indirect sites once
they number half again what its last sweep kept and enough of its translations
have left the cache to have left some unnamed: one that no resident
translation's stream names and that is no resident block's own record is
released for reuse, and the site cells and target-table entries naming it are
emptied, so link state follows what the cache holds rather than all the code
the address space ever ran. A shared address space is not swept, but is
renewed once its arena holds at least two mebibytes no body uses and more than
its bodies do: it is closed, and each executor attached lets go of it at the
start of its next entry into translated code, so the next one to run the
address space makes a new one and the old one dies with its last executor.
An executor also lets go of an address space whose generation the MMU has
destroyed, at the same point and as the count of destroyed generations moves,
leaving its number in the window's order until the least-recently-used rule
reaches it; the x86 gadget executors do the same at the start of each run.
Cached
returns consult a private per-carrier return-record ring after their site
cells miss: linked calls push the return PC with its block link, a matching
record follows that link and fills an empty cell of the site, and the executor
empties the ring on every address-space switch and after a link sweep, so a
stale record can only fall back to the ordinary miss exit. A cached indirect
branch whose cells (and, for a return, ring) miss then looks the target up in
its address space's direct-mapped target table, which every miss exit trains
with the target's block link; a hit follows that link and likewise fills only
an empty cell, so vCPUs sharing a polymorphic site do not keep rewriting it,
while the miss exit retrains its cells round-robin. The table belongs to the
address space's link state and is retired with it, so it never names another
address space's code, and a link reset by a code change leaves through its
ordinary unlinked exit.
The software TLB keys its entries by guest page number and indexes 16,384 sets
directly by the page number's low bits: two ways of a 64-byte set on 16 KiB
pages, one 32-byte entry on 4 KiB pages, where a cache line holds two
neighbouring pages. A same-page transfer compares the page of its final byte
against the way its first byte indexes: the direct index never places
neighbouring pages in one set, so a crossing transfer can only miss and is
recognized on the cold path. `arm64-tlb-probe-set` checks in both page
profiles that every inline probe of both engines hits the entry the C++ fill
path placed, in each way of its set, and misses a page its set does not hold.
The gadget engine's fill path looks the page up in the generation its entered
execution context holds (`darwin_uml_static_vm_execution_context_translate()`),
without resolving the thread's address space and active generation each time.
It reads the generation's fill index (`darwin/memory/mmu/fill_index.hpp`), a
radix table of host addresses and guest permissions that mirrors the mapping
map and is read without a lock: whatever removes or changes a mapping quiesces
execution first, so an entered context never sees a stale word. A page the index
does not hold is looked up in the map under its reader lock.

Guest debuggers depend on two kernel/host contracts. To single-step a task,
Linux sets the architectural step bit in the entry registers (arm64 PSTATE.SS,
x86 RFLAGS.TF). Every engine then retires exactly one instruction and reports a
single-step exit. The runtime returns that exit as a breakpoint event with trap
number 0x32 (arm64 software step) or 1 (x86 `#DB`, whose error code carries
DR6 with its single-step bit BS set), and Linux delivers `SIGTRAP`/`TRAP_TRACE`.
x86 ICEBP raises `#DB` after itself with BS clear, which Linux delivers as
`TRAP_BRKPT`. A stepped instruction that exits on its own, such as a
system call, breakpoint or fault, reports that exit instead. A `popf` or `iret`
that sets RFLAGS.TF leaves the x86 program at the next instruction, and the
runtime retires exactly that instruction before it reports the step, as native
`#DB` timing requires. As in x86 Linux, a ptrace step over `popf` or `iret`
leaves the TF value that instruction wrote.

Linux writes another process's memory, such as ptrace breakpoints, gdb's
displaced-stepping pad, `/proc/pid/mem` or `process_vm_writev()` stores, through
its own view of the frame, so no translated store publishes the change.
`copy_to_user_page()` calls `flush_icache_user_page()`, and `process_vm_writev()`
calls the local `flush_icache_remote_user_range()` hook, a no-op on other
architectures. On Darwin both call `darwin_uml_vm_invalidate_code()` for the
target mm's generation. That call moves the code stamp of each executable page
and publishes the range, as a guest code synchronization would. Pages mapped
without execute permission are skipped, so data transfers publish nothing.

The kernel runs AArch32 tasks (armhf and Android's armeabi-v7a: ARMv7-A and
Armv8 AArch32 with VFPv4-D32, NEON and EABI hard-float) beside AArch64 ones, as
arm64 Linux's `COMPAT` does. The bridge identifies one by PSTATE.nRW in the
entry's pstate, with R0-R14 in x0-x14, D0-D31 in V0-V15, FPSCR split between
fpsr and fpcr, TPIDRURW in the guest's TLS value and TPIDRURO in the `tpidrro`
field. The header comment on `struct darwin_uml_guest_registers` defines the
registers and what each event means. An AArch32 entry that carries SME or SVE
state, or that the engine cannot execute, returns a fatal event. An engine
reports what it can execute as the compat HWCAPs, and while they are zero the
kernel answers a 32-bit ELF file with `ENOEXEC` and `personality(PER_LINUX32)`
with `EINVAL`. `cacheflush()` and a tracer's code writes reach the engines
through `darwin_uml_vm_invalidate_code()`, as any other code write does, and
the kuser helpers and signal-return pages are ordinary code pages the kernel
installs in the task. A 16 KiB guest runs only 32-bit programs whose segments
are 16 KiB congruent; others fail cleanly at exec or `dlopen`, so stock armhf
userspace needs the 4 KiB profile. The
[shared kernel README](../kernel/common/README.md) holds the kernel side.

One decoder serves every AArch32 consumer: `darwin/cpu/arm64/aarch32` decodes
A32, T16 and T32 words, in the caller's IT state, from a table generated from
the pinned Arm AArch32 XML and its Decode pseudocode. It is a CPU-layer
`shared` target that includes no engine, so the gadget engine, the JIT and the
semantic library link the same one, and it implements the Armv8-A AArch32 EL0
profile: whatever the profile lacks is undefined, never guessed. Its
[README](../darwin/cpu/arm64/aarch32/README.md) holds the contract.

The AArch32 semantic library
([`cpu/arm64/aarch32/semantics.hpp`](../darwin/cpu/arm64/aarch32/semantics.hpp))
is shared C++ semantics rather than another engine: it includes no engine
header, works on `CpuState` alone, reaches guest
memory only through the caller's `A32Memory`, and executes any covered word
one instruction at a time with the architecture's fault ordering.
`a32_semantics_covers()` holds for every implemented form, and a word
outside it is a fatal internal error. The gadget engine and the JIT route
their slow paths and long tails to it, and the corpus's model lane runs the
instruction corpus through it. The JIT links it for what its lowering
declines.

## Kernel and Android ownership

`kernel/mainline-6.18` owns the pinned source identity, configuration,
generated patch series, and standalone build policy of both the Linux and the
Android kernel products. `kernel/common` owns only contracts shared across
families. The Apple CMake graph imports compact artifacts and checks their
bridge ABI, page profile, product family, and `6.18` release series. Patch
releases, commit identities, and producer provenance do not become runtime
compatibility keys.

`android/lineage-24` owns the Android product, static GPT layout, ramdisks,
rootfs release, and producer tools. Kernel and rootfs releases are selected
independently. Apple emulator builds consume only the Android kernel artifact;
rootfs and command-line paths are launch inputs. The `uml-darwin-android17` product
opens one directly writable supplied GPT and has no 16 KiB variant.

Guest bundles, disk capacity, and compressed rootfs delivery belong to the
application ([`../Kernarium/README.md`](../Kernarium/README.md)). Runtime
frameworks receive an ordinary disk image and no capacity policy, and reject a
GPT whose declared extent differs from its file; the guest only grows a
filesystem after observing that its containing partition is larger. This keeps
one host-capacity workflow for every image format without putting disk policy
into a kernel runtime.

## Devices and graphics

The vhost-user transport in `darwin/device/vhost_user` owns connection, queue
framing, and deferred used-ring publication. GPU, video, input, sound, console,
battery, and network providers own their protocol behavior independently. Only
the GPU provider depends on `UML_BUILD_GPU`; the others build on the transport
alone.

A disabled ring is given no new data. A buffer the backend only fills, such as
a virtio-input event buffer, stays in the ring until the ring is enabled again:
the front end reads a used buffer whatever its length, so returning it empty
would replay the event it held before. A buffer that carries a request is
consumed and dropped, because its sender may be waiting for the completion with
interrupts off, as a console's output does; only a device whose requests are
asynchronous, the GPU, asks to have them kept, and a request it has not run
then waits in the ring until the ring is enabled again. A suspended guest
disables its rings, so input that arrives meanwhile reaches it once, after the
resume.

A service can be given a request gate, which another thread closes to hold the
device's requests back. A closed gate lets no request reach the device, and
closing it waits, for a bounded time, for the request the device is already
running. A request that arrives meanwhile stays in its ring, unconsumed,
whatever the ring does: a guest that suspends and resumes while the gate is
closed finds every request still waiting, and opening the gate wakes the server,
which runs them in the order they were queued. A deferred request completing,
the device's event source and the control channel are not gated. Kernarium
closes the GPU's gate as it enters the background, because iOS revokes the
host GPU within seconds and a request that reached Metal after that would make
the renderer's Vulkan device lost for good, and opens it as the application
returns.

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
healthy device and a spurious wake are unaffected. A message that cannot be
sent because the peer has gone reports the same loss. Deleting the virtqueues
of a lost backend sends it nothing: disabling the rings only stops a live
backend from using them, so the driver's removal of the retired device warns
about nothing. A buffer the backend never used is still the driver's when its
queue breaks, and the removal frees it as the driver queued it: a console's
hvc output, which waits for the backend with interrupts disabled, is queued
as a port buffer like the console's other output.
`guest-vhost-user-backend-gone` loses a console's backend while its driver is
bound and its hvc output is in flight, under SLUB debugging.

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

Every request on the control socket has a reply, and only their order pairs
the two, so the guest holds the socket from a request until it has read that
request's reply, for a configuration read as for an acknowledged kick. A kick
on one vCPU therefore waits for another vCPU's `GET_CONFIG` to finish rather
than taking its reply, which would make a healthy backend look gone, and a
reply that names another request is malformed.
`guest-vhost-user-round-trips` reads the configuration of a console that keeps
resizing, and answers late, while every vCPU kicks its output queue.

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
backend request inside the first. A front-end closes the backend channel only
when it lets the device go, as a guest that powers off with a config change
unanswered does, so the server ends the connection without reporting a failure
when that channel reaches end of file or a write finds either socket closed,
as it does at end of file on the main socket.

Retiring the device is where the guest's recovery ends. virtio-gpu has no
broken-device handling of its own: it never tests `virtqueue_is_broken()`, so a
submitter already parked waiting for control-queue space is not woken, and
`drm_dev_unplug()` blocks behind it because that wait sits inside the section
the unplug synchronises against. In practice a lost GPU backend retires the
device and the guest's Vulkan client is killed within seconds, but a client
that happens to be waiting on a full control queue stays blocked until Mesa's
own ring timeout, on the order of fifteen minutes.

The default Apple graphics path, from guest Mesa through Virgl and
virglrenderer to Zink on KosmicKrisp and the shared Metal presentation, is in
[`graphics.md`](graphics.md). GPU commands and rendering stay off
the UI thread. The shared presentation layer keeps the newest frame and at
most one command buffer in flight. A backpressured presentation may drop a
superseded frame; only unsupported resource shapes use CPU readback.

A guest drives the renderer, so no host write into guest memory is sized by
the renderer alone: the device bounds a translated guest range to one
negotiated memory region, never writes a reply past the writable descriptors
the guest supplied (a reply that does not fit ends the connection), and the
renderer ends a Venus reply stream at the window the guest declared rather
than at the resource holding it.

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
remaining work and power off. This state belongs to the context: destroying a
context takes its renderer fence slots with it, so the transport completes the
commands still held behind its fences and drops its rings' escape counts, and
a later context the guest gives the same ID starts with none.

Venus keeps shareable Zink scanouts in their renderer-optimal Metal
layout. An unlinked vnode-backed identity descriptor reconnects each guest
dma-buf with its in-process MTLHeap and native MTLTexture; the descriptor never
stores pixel data. Native presentation samples that texture directly; the VNC
consumer reads it back as [`graphics.md`](graphics.md) describes.

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

The devices Kernarium attaches and its display geometry are listed in
[`../Kernarium/README.md`](../Kernarium/README.md).

## Guest lifecycle

The platform layer tracks every writable host descriptor opened for the guest.
`uml_emulator_sync()` asks the guest to write back through its management
console, then flushes those host descriptors. `uml_emulator_quiesce()` enters
full Linux system suspend and waits until devices, secondary CPUs, interrupts,
and timekeeping have reached the platform sleep boundary before flushing the
host. `uml_emulator_resume()` sends the console wake edge. Nothing else on the
guest's descriptors ends the suspend, and an edge sent before the guest reached
the boundary, as after a suspend request whose reply timed out, still ends it;
the [mainline README](../kernel/mainline-6.18/README.md) gives the kernel's
side. `uml_emulator_wake()` sends that edge whatever the host last asked for,
except to a guest it has quiesced, as a phone's power key does: it ends a
suspend the guest entered by itself, such as Android's once its display is off,
and a running guest only answers it. Kernarium sends it after forwarding its
Power key, whose virtio input cannot wake a suspended guest itself; the macOS
VNC display maps XF86PowerOff to the guest's power key, and VNC input is itself
a wake source. A kernel without the suspend command falls back to the stop/go
pair. Guests that need writeback requests boot with magic SysRq enabled; if the
physical iOS sandbox cannot bind the console, host-file flushing still runs but
guest writeback and freeze are unavailable.

An application that can be backgrounded holds the guest's GPU requests before it
quiesces, with the runtime's `set_gpu_hold` (see the request gate under
[devices](#devices-and-graphics)), and releases them before it resumes. The
guest's rings stay disabled until the resume, and what it queued waits in them.

The guest binds the management console as a Unix datagram socket and replies
to each request's own bound socket, which the host places beside it. Both live
in a directory the host passes as `uml_dir=`, under short fixed names, never
loose in the shared `/tmp`. On macOS and the iOS Simulator each run makes its
own `uml-kern<pid>` directory, mode 0700, in `$TMPDIR`, or in the user's
temporary directory or `/tmp` when `$TMPDIR` is unset or too long. In the
Simulator both are its application's `tmp`, too long for `sun_path`, so it uses
`/tmp/uml-kern<pid>`. The run removes it when it returns, and every terminating
signal and kernel-driven exit the process observes removes it too; only a
process killed outright leaves it, and the next run removes the directories of
runs that have exited. It also removes the `/tmp/uml-kern<pid>-mconsole`
sockets left by exited runs of older builds, which bound the console loose in
`/tmp`. An iOS device uses its application temporary directory
itself and removes a socket left there before booting. Every such path must
fit Darwin's 104-byte `sun_path`; one that does not is refused on both sides
rather than truncated or moved. The guest prints the console's path when it
binds it. `guest-host-residue` ends guests by poweroff, SIGTERM, SIGHUP, a
panic and SIGKILL and finds nothing left in `/tmp` or `$TMPDIR`.

## Tests and structural audit

Every first-party CTest has one primary suite: `unit`, `tooling`, or
`integration`, set by the `cmake/UmlTesting.cmake` helper that registers it.

`architecture-audit` checks the CMake File API graph, dependency cycles,
implementation includes, source ownership, and test registration. Ownership
includes that only a darwin/ source's owner compiles it: no other CMake file
and no Xcode project names one. A first-party shell script writes each `cd`
as `CDPATH= cd --` unless it names a literal absolute directory, so an
exported `CDPATH` can neither redirect it nor print into captured output.
No compile definition names a source or build directory, so no binary needs
the checkout it was built from at run time. No host source under darwin/ or
Kernarium/ names x18 outside a comment, whether assembly, an assembly
contract, inline assembly or a generator: Darwin clears that platform register
when it interrupts a thread, so a value kept there is lost at a random
preemption. `arm64-emitted-registers` and `x86-64-jit-emitted-registers`
hold the emitted code to the same rule, as [`testing.md`](testing.md)
describes. Outside macOS the configured graph holds none of the macOS-only
x86-64 JIT, as the x86 section above describes. The audit does not prove
performance, generated code shape, a kernel boot, or guest behavior; those
require their owning validation.

The host/kernel ABI source is [`../kernel/common/abi`](../kernel/common/abi/).
Version, fingerprint, generated-copy, layout, and import checks fail closed on
drift.
