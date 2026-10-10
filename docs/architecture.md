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
separate accepted 16 KiB ACK artifact.

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

The non-streaming SVE semantic library,
[`cpu/arm64/detail/sve_semantics.hpp`](../darwin/cpu/arm64/detail/sve_semantics.hpp),
is shared C++ semantics rather than another engine. It executes the SVE
profile's words at PSTATE.SM = 0 and VL 16, routed by the generated encoding
table, for an engine's forms that it does not lower inline. It includes no
engine header and works on `CpuState` alone. It reaches guest memory only
through the caller's translation interface. It never changes PC, and it marks
the SVE state live. A reserved bitmask immediate gives the
illegal-instruction exit. A memory form faults at the first byte, in the
pseudocode's access order, that does not translate, and checks an SP base's
16-byte alignment before any access, also with no element active. A fault
leaves what arm64-builder's Neoverse V2 leaves: a load changes no register, a
store writes its footprint's 16-byte pieces in order up to the faulting one,
and a scatter its elements in order up to the faulting one. A first-fault
load's later elements, and a non-fault load's every element, clear FFR from
the first that does not translate instead of faulting.
A floating-point form computes each active element with the host's own
instruction under the guest FPCR, so inactive elements raise nothing; while
an FPCR trap enable is set it runs as AdvSIMD's transactional path does, and
the same header decides. The library classifies each form's rounding and
notes every value it rounds to a subnormal result, the steps of FADDV,
FADDA, FMAXV, FMINV, FMAXNMV, FMINNMV and FCLAMP included, so flushes and
exact tiny results follow FPRoundBase() as above. An enabled exception
restores Z, V and P, commits nothing, and gives the floating-point trap
with one cause. The cryptographic and integer matrix
forms are computed in software. `sve_semantics_covers()` holds for every row
the table routes; any other word is a fatal internal error. Both engines call
it: the gadget engine through its `sve_library` gadget and the JIT through a
terminal helper.

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

Both engines complete WFE and WFI at once and treat SEV and SEVL as no-ops,
so no wait ever depends on an event. AT_HWCAP therefore always carries
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
engines. The prober reports the profile only under the guest-SME policy,
`DARWIN_UML_GUEST_SME=1`, which is off by default; with it off,
ID_AA64PFR1_EL1.SME, ID_AA64SMFR0_EL1 and the SME HWCAP2 bits read zero on
every host. Each engine's `sme_support` names how much of the profile it
implements: the control core (`control`), all of it but the SME2 forms
(`complete`), or all of it (`sme2`). An SVE or SME word it does not
implement is the illegal-instruction exit. A guest sees FEAT_SME2 only
from an engine at `sme2`: the runtime configures each engine, and derives
the HWCAPs from the selected one, with the host less FEAT_SME2 below that
level (`cpu::engine_host_capabilities`). Below it ID_AA64PFR1_EL1.SME
reads 1, SMEver, I16I32 and BI32I32 read zero, HWCAP2_SME2, SME_I16I32 and
SME_BI32I32 are clear, and the ZT0 forms are undefined.

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

The non-streaming SVE profile of the arm64 virtual CPU is FEAT_SVE with
FEAT_SVE2, plus FEAT_SVE_AES with FEAT_SVE_PMULL128, FEAT_SVE_SHA3 and the
SVE forms of FEAT_I8MM and FEAT_BF16, each where the host has the AdvSIMD
forms both engines lower it onto. No supported host implements SVE, so the
vector length is a fixed 16 bytes on every host, independent of the SME
profile's SVL, and Z_n outside streaming mode is V_n.
[`cpu/arm64/sve_profile.hpp`](../darwin/cpu/arm64/sve_profile.hpp) derives
the profile and the engines' mask form of a predicate, a 16-byte image with
0xff in each active byte. `CpuState` appends P0-P15 and FFR in that form,
apart from the streaming P, and every change of PSTATE.SM zeroes them. The
prober reports the profile only under the guest-SVE policy,
`DARWIN_UML_GUEST_SVE=1`, which is off by default; BitPerm and SM4 are
withheld. Each engine's `sve_support` names how much of the profile it
implements outside streaming mode, `none`, `control` or `complete`, and
`cpu::engine_host_capabilities` presents the profile only from an engine at
`complete`. Below that, under either policy, ID_AA64PFR0_EL1.SVE and
ID_AA64ZFR0_EL1 read zero, the SVE HWCAP bits are clear, and every
non-streaming SVE word is the illegal-instruction exit. Both engines are at
`complete`.

The guest kernel learns the same answer from
`darwin_uml_guest_sve_vector_length()`, 16 exactly when the selected engine
presents the profile, and gives tasks SVE state only then. A task's P0-P15
and FFR travel in its bridge tail after the SME registers, packed 16 bits
each, with a live flag, the image of native `TIF_SVE`. Each entry takes
`CpuState::sve_live` from the tail and unpacks P and FFR only for a live
task; each return packs them back only while `sve_live` is set and zeroes the
vCPU's copies, so a task that is not live finds them zero when it executes
SVE. The engines set `sve_live` and never clear it; the kernel clears the
flag where native Linux stops tracking the state, as the
[shared kernel README](../kernel/common/README.md) describes. A tail whose
flags are anything but clear or live at the run's vector length is fatal.

The gadget engine is at `complete`: for SVCR.SM = 0, on capabilities that
carry the profile, it runs every word of the profile, so under the
guest-SVE policy it presents the profile to its guest. Generated gadgets
run the control forms (PTRUE, PTRUES, PFALSE and PTEST, the predicate
logical operations, CNTP, INCP and DECP, WHILE*, CNT*, INC*, DEC*, RDVL,
ADDVL and ADDPL, SETFFR, RDFFR and WRFFR, MOVPRFX, DUP, DUPM, FDUP, CPY, FCPY
and SEL) and the register-only hot set: the unpredicated, predicated and
immediate integer arithmetic, logical, multiply, min/max and shift forms,
including the shifts by wide elements, the SVE2 widening (also indexed),
narrowing, high-half and shift-accumulate forms, the unsigned saturating
forms, BSL, BSL1N, BSL2N, NBSL, EOR3, EORBT, EORTB, XAR, ZIP, UZP and TRN
of vectors and predicates, REV, EXT, TBL of one or two registers, the dot
products, the integer compares (also by immediate), the floating-point
compares, PUNPK, SUNPK and UUNPK, BRKA and BRKB, INDEX, COMPACT and SPLICE,
UADDV, FADDV and FADDA, ADDP, USQADD, URHADD, SUBHNB and SUBHNT, and the
floating-point arithmetic (also by immediate, and with reversed operands),
multiply-add, indexed, rounding and same-size conversion forms, and the
vector-math and scalar-loop helpers: FEXPA, FTMAD, FTSMUL and FTSSEL, FCADD,
FSCALE, FCVT between single and double precision, SCVTF and UCVTF of words
to doubles, FCVTZS and FCVTZU of doubles to words, ASRD, and UQINC and UQDEC
of a general register. FEXPA and FTMAD read the tables of the semantic
library (`cpu/arm64/detail/sve_trig_tables.hpp`) through a pointer in their
stream. FSCALE adds the scale to the exponent field of every active normal
element whose result is normal (a zero or an infinity is its own result) and
raises nothing; any other active element, a NaN, a denormal or an overflow
or underflow, sends the whole instruction to the semantic library from the
gadget. The half-precision FCVT stays in the library, since AdvSIMD's
conversions honour FPCR.AHP and SVE's do not. A predicated
floating-point form tests its governing predicate for all-true and then
runs the unpredicated AdvSIMD form; under a partial predicate it first
replaces its inputs' inactive elements with +0.0 (a divisor with 1.0), so
they raise nothing, and merges under the lane mask, which a predicated
integer form always does since that costs less than the test. A
saturating form saturates by compare and select, since AdvSIMD's
saturating forms set FPSR.QC and SVE's never do, and a shift by an
immediate has a gadget per value of tsz:imm3. A contiguous load or store
(LD1*, ST1*, LDNT1*, STNT1*, LDFF1*, LDNF1*, LD1R*, LD1RQ*, LD2-LD4,
ST2-ST4, and LDR and STR of a Z or P register) whose footprint lies in one
guest page the TLB holds runs on the host address: a load reads the whole
footprint and zeroes the inactive elements (a first-fault or non-fault
load, whose elements all translate, leaves FFR as it is), and a store under
an all-true predicate is one store, and under any other writes its active
elements one by one, never touching an inactive element's bytes. A
crossing, a page the TLB cannot fill, a fault, an SP base that is
misaligned or tagged (an SP base is never untagged) runs through the semantic
library, which decides faults, partial writes and FFR. A gather or scatter
(LD1*, LDFF1*, ST1*, and LDNT1* and STNT1* with a vector base) forms,
untags under TBI and translates each active element's address inline; an
element whose page the TLB lacks, an element crossing a page or a bad SP base
sends the whole instruction to the library, which is exact because a load
writes its destination last and a scatter's stores are idempotent in element
order.
SVE prefetches, gather prefetches included, are NOPs. A size or index a
form's gadgets leave out runs through the semantic library. Every other
word runs through the semantic library from the
`sve_library` gadget, which stores the guest's NZCV and, while the native FP
environment is active, its cumulative FPSR flags to `CpuState`, calls the
library with guest memory through the TLB, and reloads both; a library
fault leaves through the memory-fault exit. While
`count_sve_library_exits` is enabled (it is off by default), the words that
reach the library are counted per table row, with the words the builder
translated and sent there. That is the library-exit report:
`DARWIN_UML_SVE_LIBRARY_EXITS=PATH` enables counting from a process's start
and writes the report at its exit and as each executor retires (the emulator
leaves through `_Exit`, so the last write is the complete one; a process that
ran no gadget executor and counted nothing leaves the file to the JIT, which
writes the same format), and
`tools/arm64-tests/sve-library-report.py`
sums reports and ranks the forms by exits. A reserved bitmask immediate of
AND, ORR, EOR or DUPM ends the block in front of it as the
illegal-instruction exit. While an FPCR trap enable is set, the executor's
one-instruction mode runs each non-streaming SVE word through the library,
which decides a trap, so a trapping word commits nothing. Its SVE part
follows the SME part, so it moves no base or SME entry and re-encodes no
veneer call, and holds no SVE or SME instruction:
`engine/generate_sve.py` writes its gadgets at build time from the encoding
table, one per value of the bits that select a form's lowering, and each
loads the registers its stream names from `CpuState`, P and FFR in mask
form, computes on AdvSIMD and general registers at VL 16, and stores its
destination back. A predicate governing elements wider than a byte becomes a
lane mask by one CMTST against the element pattern, and a flag-setting form
computes PredTest into the guest's NZCV. A WHILE* reads its predicate and
NZCV from a table indexed by its clamped active count, which
`darwin/cpu/arm64/gadgets/sve_layout.h` lays out, and one followed by a
B.cond on N or Z runs as one entry whose fall-through is translated
inline. CNT*, INC*, DEC*, RDVL, ADDVL and ADDPL select base
entries on a constant the builder computes. A program holding one of these
words accesses the FP state before it runs, since P and FFR travel with it,
and enters through `sve_link_entry`, which also sets `CpuState::sve_live`.
`arm64-gadget-sve` checks the control forms against the pseudocode and
every generated form against the semantic library on random states, with
the native FP environment inactive and active: the memory forms from bases
inside a page, across pages, unmapped and tagged, comparing memory and
faults too. It also checks that no
program translated for SM = 1 selects an SVE-part entry and none for SM = 0
a streaming one.

The JIT is at `complete`: for SVCR.SM = 0, on capabilities that carry the
profile, it lowers PTRUE, PTRUES, PFALSE and PTEST, the predicate logical
operations, CNTP, INCP and DECP, WHILE*, CNT*, INC*, DEC*, RDVL, ADDVL and
ADDPL, SETFFR, RDFFR and WRFFR, and MOVPRFX, DUP, DUPM, FDUP, CPY, FCPY, SEL
and the unpredicated logical forms onto AdvSIMD and general registers
([`cpu/arm64/jit/jit_compiler_sve.cpp`](../darwin/cpu/arm64/jit/jit_compiler_sve.cpp)),
and lowers the register-only and contiguous-memory hot set of the profile
onto AdvSIMD for an all-active governing predicate
([`cpu/arm64/jit/jit_compiler_sve_inline.cpp`](../darwin/cpu/arm64/jit/jit_compiler_sve_inline.cpp);
the forms are listed in
[`cpu/arm64/jit/jit_sve_inline.hpp`](../darwin/cpu/arm64/jit/jit_sve_inline.hpp)): the floating-point
multiply-add (negated forms included), arithmetic (divide, square root and
rounding included), compare (into a predicate, flags untouched; FCMUO through FCMEQ for its
exceptions and integer compares for its NaNs)
and same-size conversion forms, FCVT between single and double precision
(half precision stays in the library, since AdvSIMD's FCVT honours FPCR.AHP
and SVE's does not), the 32-bit integer conversions of a double (SCVTF and
UCVTF into doubles, FCVTZS and FCVTZU out of them), FSCALE for normal elements
whose results stay normal (an exponent add; any other element sends the word
to the library), FTSMUL and FTSSEL, the integer arithmetic, unary (negate,
count, extend), logical, shift (by a vector, UQSHL included, by an immediate
and merging), halving, difference, widening, narrowing, high-half, dot-product
(USDOT and SUDOT where the host has FEAT_I8MM) and UADDV forms, ZIP,
UZP and TRN (also of predicates), TBL of any element size (one or two
tables), unpacks, EXT, REV and INDEX, BRKA and BRKB (flag-setting forms
included), COMPACT, SPLICE, EORBT and EORTB, SQINC, SQDEC, UQINC and UQDEC of a
general register, and LD1*, ST1*, LD1R*, LD1RQ*, LD2-LD4, ST2-ST4, PRF* and
LDR and STR of a Z or P register whose footprint lies in one guest page the TLB
holds, and the gathers and scatters (LD1*, LDNT1*, ST1* and STNT1* with an
address per element) whose every access does. A gather or scatter translates
one element at a time without the bridge, so a page the TLB lacks sends the
word to the library, which fills the TLB; a gather loads into scratch and
writes Z last, a scatter writes its elements in order, and either leaves the
block before it changes anything the library would not repeat. A tracked block
(another vCPU holds an exclusive reservation) leaves scatters to the library,
since it records one store range per instruction. LDFF1 and LDNF1 stay in the
library. A predicated form checks its governing predicate once per block and
element size, after a PTRUE that sets every element it checks nothing, and
leaves the block for the library, which alone knows what an inactive lane must
not do, when the predicate is partial. A memory word with a register base
checks `CpuState::tbi_enabled` once per block, at the first such word: no
guest instruction changes it, and with top-byte ignore enabled the block
leaves at that word for the library, which untags. A load with such a base and
a constant offset starts a window with the loads of the same base behind it,
up to 17 words within 256 bytes, that follow with between them only SVE words
that name no general register and no memory: the first word proves that the
window lies in one guest page, translates its lowest byte once and keeps the
host address in the chain state, and each later word reads it back in place of
translating. Words stay in program order, so a proof that fails leaves the
block at the first word and the library runs it, and the words behind it
compile again as their own block. The breaks, COMPACT and SPLICE, which
raise nothing, take any governing predicate: a prefix OR of the mask form, a
table of the active-element masks (COMPACT) and a TBL index built from the first
and last active byte (SPLICE).
The saturating forms saturate by compare and select, since AdvSIMD's set
FPSR.QC and SVE's never do. A 64-bit multiply, minimum or maximum, which
AdvSIMD lacks, is built from 32-bit multiplies or a compare and select, and
a shift by a count of a whole element or more is clamped to the element's
width. A word that lowering declines (an element size
AdvSIMD lacks, a crossing or missing page, an SP base or a tagged one) and
every other word of the profile ends its block in a terminal helper
([`cpu/arm64/jit/jit_semantic_sve.cpp`](../darwin/cpu/arm64/jit/jit_semantic_sve.cpp)),
which runs the word through the semantic library on `CpuState` as the SME
slow paths do: the library marks the SVE state live, reports a fault at the
word's PC, clears the local exclusive monitor on a store and invalidates the
software reservations the store wrote. While library-exit counting is on,
which it is not by default (`semantic::count_sve_library_exits`), the helper
and the trapped mode count each row's library runs and keep its last word, and
the compiler counts each row's translations and those that ended in the
library; the counts name the words a workload sends to the library.
`DARWIN_UML_SVE_LIBRARY_EXITS` writes them as the gadget engine's report, from
each retiring executor too. A reserved bitmask or shifted byte
immediate, and a row the capabilities lack, are the illegal-instruction
exit. No translation holds an SVE word, and SM=1 translations and
translations without an SVE word are unchanged. While an FPCR trap enable
is set, or a single step is pending, the executor runs each SVE word through
the library itself, which decides the trap, the flags it commits and the one
cause it reports, as the gadget engine's trapped mode does. Z is V in the
block-local vector bindings, and P and FFR are bound beside them in mask
form and written back at every exit. A flag-setting form computes PredTest
into the guest's NZCV (PTRUES tests its result under itself, a WHILE* under
the all-true predicate). A WHILE* reads its predicate and NZCV from a table
indexed by its active count clamped to 16, and one followed in the block by
a B.cond on N, Z or C branches on that bit of the table's NZCV and ends the
block with the pair. A block holding an inline SVE word accesses the FP
state before it runs, since P and FFR travel with it, and stores
`CpuState::sve_live`. `arm64-jit-sve-control` checks the control forms
against the pseudocode on injected hosts, and `arm64-jit-sve-forms` checks
every row of the profile against the library on random states: alone in a
block, behind an FP instruction that leaves the native environment active,
behind PTRUE of P0-P7, and after every writer of a predicate that a PTRUE
made all-active, from memory bases inside a page, across pages, unmapped and tagged, and
under random FPCR trap enables and, where the host implements FEAT_AFP, random
FIZ, AH and NEP. A pass from a clean state (every predicate and FFR
all-active, no trap enable, an aligned base inside a page) requires the
library-exit count of every lowered word's row to stay zero, so a lowering
that always bailed to the library cannot pass as a match.

The arm64 engines are growing a 32-bit Arm guest: AArch32 EL0, A32 and T32
with interworking and IT, for the armhf userspace of Debian and Android's
armeabi-v7a, which needs hardware VFPv4-D32 and Advanced SIMD, so there is no
soft-float-only profile. The profile, derived from the host as the SVE and
SME profiles are, is in
[`cpu/arm64/aarch32/a32_profile.hpp`](../darwin/cpu/arm64/aarch32/a32_profile.hpp).
Each engine's `a32_support` names how far it implements it, `none`, `base`
(the integer core), `simd` or `complete`, and `base` and `simd` are
milestones, not products. Both engines are at `none`: `CpuEngine::run_aarch32`
is the entry for a task in AArch32 state, the A64 `run` is unchanged, and an
engine below `base` takes the new entry as the illegal-instruction exit
without touching the state. The development policy
`DARWIN_UML_GUEST_AARCH32=1` is off by default and adds no host token;
`a32_presented_support` gives the level a run presents, nothing with the
policy off and the engine's level with it on, and the AArch32 identification
registers and the compat AT_HWCAP and AT_HWCAP2 words derive from that level
and the host in `cpu/arm64/id_registers.hpp` as arm64 Linux derives them, and
are zero below `base`. The default turns on once both engines are complete.

An AArch32 task keeps R0-R14 zero-extended in the integer registers, NZCV in
the A64 flags and D0-D31 in the low sixteen vectors; what has no A64 field is
appended to `CpuState` after the SVE block (GE, the sticky Q flag, ITSTATE,
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
readable page executable. The banks, the host-flag fusion rules and the
registry contract are in
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
by name. The
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
validation. Cached returns consult a private
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
is shared C++ semantics rather than another engine, in the SVE library's
mould: it includes no engine header, works on `CpuState` alone, reaches guest
memory only through the caller's `A32Memory`, and executes any covered word
one instruction at a time with the architecture's fault ordering.
`a32_semantics_covers()` holds for every implemented form, and a word
outside it is a fatal internal error. The gadget engine and the JIT route
their slow paths and long tails to it, and the corpus's model lane runs the
instruction corpus through it. Until an engine calls it, no product binary
links it.

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
host. `uml_emulator_resume()` sends the console wake edge. A kernel without the
suspend command falls back to the stop/go pair. Guests that need writeback
requests boot with magic SysRq enabled; if the physical iOS sandbox cannot bind
the console, host-file flushing still runs but guest writeback and freeze are
unavailable.

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
