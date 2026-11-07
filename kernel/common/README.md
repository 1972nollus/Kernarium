# Shared kernel contracts

This directory contains only source-family-independent inputs:

- [`abi/`](abi/README.md): canonical plain-C host/kernel bridge, identity,
  generated-copy authority, and import policy;
- `config/darwin-uml.fragment`: the kernel configuration policy every product
  shares, including the 4 KiB guest page size and the 64 KiB task-stack
  geometry it implies. Each product applies its own overlay after it, which
  lists only the settings in which that product differs and whose README says
  why;
- `artifacts/`: compact prebuilt-kernel bundle schema and verifier, plus the
  shared `KEY='value'` metadata parser in `contract_checks.py`;
- `patches/`: the generated topic series every maintained line applies
  first, regenerated from the mainline replay and applied unchanged to each
  line's own upstream base;
- `tooling/kernel_series.py`: replay, verification, and regeneration of a
  line's ordered generated series; it reads and writes `metadata.env` through
  that parser;
- `tooling/config-fragments.sh`: the kernel configuration composition both
  kernel builds source. It copies a product's first fragment as the starting
  `.config`, applies each later one through `scripts/config`, and after
  `olddefconfig` requires every requested symbol to hold the value its last
  fragment asked for;
- `tooling/verify-console-input.py` and `tooling/console_input_probe.cpp`: the
  console PTY assertion described below; and
- [`video/`](video/README.md): the private Virtio video wire contract and the
  tool that syncs it into each kernel checkout.

The series supports only the Darwin host. Every product is built with
`ARCH=um SUBARCH=arm64 UML_HOST_OS=Darwin`, the x86_64 slice included (an
arm64 kernel for x86-64 userspace). A topic carries no `arch/um/os-Linux`
change and does not keep the Linux-host UML build compiling.

Pinned commits, configuration overlays, toolchains, source locations, the
bytes of each line's own overlay series, and build commands stay in the owning
versioned kernel line. The common artifact verifier is local and read-only; it
checks semantic source/ABI identity, page and module policy, ELF geometry,
payload inventory, and exact bridge imports without requiring Kbuild or a
source checkout.

The contracts of the shared series follow, grouped by subject. A topic's
behavior is described here once, whichever product builds it; a line's README
says only where its products differ.

## Console, signals and run end

Host-terminal console output preserves unread terminal input when changing
newline processing. Input and output may share one host PTY, so a kernel log
must not discard pending interactive commands or bootstrap payload bytes.
The standalone Darwin kernel build verifies this with the actual console
implementation and a native PTY assertion. This producer check takes explicit
source/build paths; Apple product configuration still consumes only bundles.
Closing a serial line, as its exitcall does at power-off, never closes a host
descriptor: an `fd:` channel's close only restores the terminal, and the
duplicate output descriptor `close_one_chan()` would close exists only in
time-travel mode, which needs a uniprocessor kernel.

A console write runs with interrupts off and allocates no memory. A serial or
console line allocates its output ring when it first opens, so the room it
reports to the line discipline is room the ring has, and a full ring holds the
writer back rather than losing its output. The Virtio console copies hvc output
into a buffer it allocated with its port. `guest-console-write-memory` traces
every allocation while it writes to both and checks that every byte arrives.

Darwin's `sigprocmask()` sets the mask of every thread in the process, vCPU
carriers included, so host-side kernel code changes a signal mask only with
`pthread_sigmask()`. The bridge import check rejects an image that imports
`sigprocmask`, and the console PTY assertion checks that a console write
leaves another thread's mask unchanged.

Linux asks for the current CPU on every use of `current`, and a Darwin vCPU
carrier keeps its CPU number in a pthread key. `uml_curr_cpu()` reads that
key's word directly from the carrier's thread-specific data, whose base
`TPIDRRO_EL0` holds, instead of calling `pthread_getspecific()`, a host call
that iOS pays an order of magnitude more for than macOS. The layout is
Darwin's own, not an interface, so each carrier checks it as it comes up: the
base must lie inside the carrier's own pthread structure and name the thread
in its first word, and the words of both CPU keys must equal what
`pthread_getspecific()` returns. The direct read starts once a carrier has
passed, and any mismatch stops it for the rest of the run with a warning on
stderr, so a changed layout costs speed and never correctness. The guest
integration harness and Kernarium automation runs fail a boot that prints the
warning, so such a change is seen rather than only measured.

Host-side kernel objects pass variadic arguments in registers, while Apple's
arm64 ABI reads them from the stack, so a direct call to a variadic libc
function hands it arbitrary arguments. `fcntl()` goes through the fixed-arity
bridge function `darwin_uml_fcntl()`, and `open()`, `ioctl()` and `printf()`
through shims in `arch/um/os-Darwin/compat.c`. Every kernel build rejects an
image that imports `fcntl`.

A Darwin vCPU carrier reports each host signal it takes on stderr, with its
literal pc and, for a fault, its address and ESR, before Linux handles it. The
one exception is a fault that a nofault accessor recovers: that is the
accessor's expected result and prints nothing. `guest-kernel-nofault` checks
that its sysrq t dump prints no report.

Signal dispositions are process-wide, so a host thread that carries no vCPU
can take UML's handlers too. Its SIGSEGV or SIGBUS is reported with its real
host thread and literal addresses and left to the host's default action. A
synchronous fault enters the guest path only from a virtual CPU carrier, or
from the boot thread while it still owns CPU0. SIGALRM, which the process-directed ITIMER_REAL may hand
to any thread that leaves it unblocked, never runs a guest tick there: the
kernel reports it once, keeps SIGALRM blocked on that thread afterwards, and
raises it again for the process, where CPU0's carrier takes it. Embedders
should still keep SIGALRM blocked on their own threads.

An embedded run returns to its host only once no kernel thread can execute
the image, which the loader unmaps right after. Each vCPU carrier still runs
image code after it has signalled the run end, so the run end joins every
carrier. A stop also wakes carriers parked before CPU bring-up or after
hotplug, which reach no IPI boundary. A host stop skips Linux exitcalls, so the
run end stops the RTC alarm helper itself. `guest-run-end-stop` stops guests
throughout their boot and requires each run to end with no kernel thread left.

A public stop reaches every carrier: one leaving translated guest code honours
it before its task continues, and an idle carrier's wait includes the stop
descriptor. The last-ditch handler for a fatal host signal exits at once
without Linux exitcalls, because the carrier that receives a process-directed
signal may be running its vCPU's idle task.

The kernel tells the host how the guest ends, with the bridge import
`darwin_uml_guest_shutdown(kind, command)`, which `machine_restart()`,
`machine_power_off()` and `machine_halt()` call before they run the exitcalls.
The kinds are power-off, halt and restart; a restart passes the command the
guest gave (`reboot recovery`), which is NULL when it gave none. Recording
first keeps the intent if an exitcall never returns. `device_shutdown()` runs
before `machine_*()`, so a reboot notifier of the highest priority reports the
same intent as well, before any driver is shut down, and a driver whose
`.shutdown` never returns still leaves it with the host; the report
`machine_*()` makes later replaces it. A panic does not report:
it aborts the process, and no restart follows it. The kernel's exit status
keeps its meaning, so a host that reads only the status sees what it saw
before.

## Machine description and host threads

Every kernel describes the virtual machine in the SMBIOS tables firmware
writes on other machines, so `/sys/class/dmi/id`, `/sys/firmware/dmi` and the
programs that read them, such as dmidecode and systemd, find it there. The
system, baseboard, chassis, BIOS, processor and memory vendor is `Kernarium`,
the product, family and board are `Virtual Machine`, the BIOS version is the
kernel release, the chassis type is Other, and the BIOS characteristics mark a
virtual machine, so `systemd-detect-virt` reports `vm-other`. The processor is
the virtual CPU the host names for `/proc/cpuinfo` (`Kernarium Virtual CPU`),
with the possible CPUs as its cores and threads, and one memory device is the
RAM the guest was given.
No serial number, UUID, speed or firmware date is claimed. The tables sit
behind an SMBIOS 3.0 entry point in guest RAM, which the kernel writes before
the DMI core scans for it. No kernel has `/dev/mem`: UML's physical address 0
is the kernel image, so its legacy BIOS window is kernel text, and a mapping
of it gave root the kernel's memory, while one past RAM never resolved.
Geekbench 6's x86-64 chassis probe, which scanned that window when sysfs had
no tables, could leave a shared mapping of it for the rest of a run.
`guest-kernel-dmi` checks the tables and the missing `/dev/mem`.

Every host thread the kernel creates names itself, so host tools, crash
reports and a host's lifecycle code can tell it from the host's own: the vCPU
carriers are `uml-vcpu-N-carrier`, and the IRQ notification thread and the
RTC alarm helper are `uml-irq-notify` and `uml-rtc-alarm`.
`guest-kernel-thread-names` lists the emulator's threads and finds each one.

A Darwin kernel never changes the host process's session, process group or
controlling terminal, so the emulator stays where its launcher put it,
whatever its console is, and a signal to the launcher's process group, such
as a timeout's or a Ctrl-C, reaches it. Upstream follows a terminal
console's size with a helper that takes the terminal as its own controlling
terminal; on Darwin that helper would be a thread and would make the whole
emulator a session leader, so a Darwin kernel starts none, and a console
keeps the size its input terminal had when the line opened.
`guest-console-session` checks the session and process group.

## Stack traces and user memory

A Darwin kernel stack trace walks the stack its starting stack pointer lies
on: a task's own stack in guest memory or, on the current vCPU carrier, the
carrier's native stack or the signal stack its interrupts run on. An
interrupt's trace continues on the task stack it interrupted, a kernel fault's
starts at the faulting instruction, and a blocked task's starts at the context
it saved when it switched away. A task running on another vCPU still prints no
trace. The walk follows frame records, so every WARN, BUG and sysrq trace
lists only reliable entries, and a Darwin kernel without frame pointers fails
to build; every maintained kernel has them.
`guest-kernel-nofault` requires its sysrq t dump to list entries and to mark
none of them unreliable (`?`).

A string copied from user memory, such as a path or a task name, is copied a
page at a time up to its terminator. As with the native
`strncpy_from_user()`, the destination past the terminator is left as it was,
so a short path costs the same wherever it lies in its page.
`guest-user-strings` checks the copy's length limits, page crossings and
faults.

## arm64 task interface

An arm64 signal handler installed without `SA_RESTORER` returns to a
trampoline in the final guest page, above `STACK_TOP`, where the Darwin host
runs `rt_sigreturn` itself. Every arm64 task maps that page read-only as
`[sigpage]`, holding native arm64's `__kernel_rt_sigreturn` instructions at
the trampoline, and the kernel places a frame record with the interrupted x29
and x30 above the signal frame. Unwinders such as libgcc's therefore cross a
signal frame as on native arm64, which `backtrace()` in a handler and thread
cancellation inside a system call rely on. Like native's sigreturn page, the
page is sealed where the kernel seals system mappings, and a debugger may
write it as the tracee's private copy; a breakpoint written at the trampoline
never fires, because the host returns from the handler before fetching there.
`guest-signal-unwind` checks the unwinds, the frame record and a tracer's
write.

Signal delivery fails or restarts only a system call the task is still in.
Every other entry from userspace, a fault, an interrupt or a trap, leaves the
task's system call number at -1, so whatever the task holds in its return
register, even one of the -ERESTART* codes an interrupted call returns,
reaches a signal handler and survives it unchanged. `guest-signal-restart`
checks x0 on arm64 and the x86 exception probe checks RAX.

A tracer sees an arm64 task's system calls as it does on native arm64. At the
entry stop x0 (r0 for an AArch32 task) holds the first argument, and what a
tracer writes there is the argument the call gets. `PTRACE_GET_SYSCALL_INFO`
and audit report the argument as the task passed it, and
`PTRACE_SET_SYSCALL_INFO` replaces both copies. Only the number -1 presets x0
with -ENOSYS; a tracer or seccomp that sets it skips the call and leaves x0 as
it wrote it, and any other number with no call returns -ENOSYS. The exit stop
shows the return value, and a call restarted after a signal shows its first
argument again. A tracer reads and sets the number of the call through
`NT_ARM_SYSTEM_CALL`: another number runs that call, and -1 skips it. A call
whose number a tracer changed and that then restarts after a signal restarts
as the call the task made, because x8 stays as the task wrote it. A stop
holds 0 (entry) or 1 (exit) in x7, r12 for an AArch32 task, and the task's
value returns when the stop ends. The x86_64 slice keeps x86's convention,
-ENOSYS in rax at every entry stop, and has neither of these.
`guest-ptrace-syscall-args-gadgets` and `-jit` check A64 and AArch32 tracees.

A task's stack pointer is the task's own. A system call or a fault taken
with it at zero or outside the task's address space returns, or delivers the
signal, that native Linux does, however often it comes on one vCPU.
`guest-stack-pointer` checks this on arm64 and the x86 exception probe on
both x86 ABIs.

The arm64 kernels give a task the AArch64 tagged-address ABI
(`Documentation/arch/arm64/tagged-address-abi.rst`) as native arm64 Linux
without MTE does. `prctl(PR_SET_TAGGED_ADDR_CTRL, PR_TAGGED_ADDR_ENABLE)` lets
a task pass pointers with any top byte to the system calls that read and write
its memory, futex, `process_vm_readv` and `process_vm_writev`, and
`PR_GET_TAGGED_ADDR_CTRL`, `abi.tagged_addr_disabled` and
`NT_ARM_TAGGED_ADDR_CTRL` report and control it. The memory-management calls
untag whether or not it is on. Threads and forks inherit it and an exec ends
it; an AArch32 task has none and gets `EINVAL`. The kernel offers it only
while `darwin_uml_guest_top_byte_ignore()` is 1, which is every run unless the
host passes `--guest-no-top-byte-ignore`, because a tagged pointer faults
otherwise; with the off switch enabling it fails with `EINVAL` through both
interfaces. A fault's
address is the one the engine translated, already without its tag, so
`segv()` never untags it and a tagged access with the ignore off faults at the
tagged address instead of retrying forever. The one difference from native
Linux is `SA_EXPOSE_TAGBITS`: `si_addr` never carries the tag, because the
engines do not report the address the instruction presented. A non-fixed
`mmap()` hint the guest cannot use, outside the guest address range or inside
the native window, is ignored as on native Linux, and only a fixed placement
that touches the window is refused. The x86_64 slice has none of this.
`guest-tagged-addr-gadgets`, `-jit` and `-no-tbi` compare every result of a
probe with the rows native arm64 Linux printed for it.

When the host implements SME for a run,
`darwin_uml_guest_sme_vector_length()` returns its streaming vector length
(SVL) and an arm64 task gets what `arch/arm64/kernel/fpsimd.c` gives it on a
CPU whose only SME vector length is that SVL; when it returns zero, as on
the x86_64-userspace slice, none of this is active. The task's SVCR, TPIDR2
and a pointer to its SME storage (Z, P, ZA, then ZT0) follow the FP registers
in the bridge tail; the storage is allocated on the host's first SME access
event or when the kernel needs it, and freed at exec and task release. As on
native arm64, a system call from streaming mode leaves it with ZA kept,
signal frames carry `sve_context` with `SVE_SIG_FLAG_SM`, `tpidr2_context`,
`za_context` and, with SME2 and ZA on, `zt_context`, moving into
`extra_context` when they do not fit, a handler starts with SM and ZA off
and TPIDR2 zero, and sigreturn rejects what `parse_user_sigframe()` rejects.
`clone()` with `CLONE_VM` gives the child no ZA and a zero TPIDR2, a fork
copies both, ptrace adds `NT_ARM_SSVE`, `NT_ARM_ZA`, `NT_ARM_ZT` and
TPIDR2 in `NT_ARM_TLS`, and `PR_SME_SET_VL` and
`abi.sme_default_vector_length` accept any length and settle on the SVL.
Unlike native arm64, `AT_MINSIGSTKSZ` counts the ZA data a frame can carry,
so a stack of that size holds every frame. `guest-sme-gadgets` and
`guest-sme-jit` check the contract, and `guest-sme-off-gadgets` and
`guest-sme-off-jit` that none of it shows without SME.

The virtual CPU has no SVE outside streaming mode and the HWCAPs say so:
there is no `NT_ARM_SVE`, `PR_SVE_SET_VL` and `PR_SVE_GET_VL` fail with
`EINVAL`, `abi.sve_default_vector_length` does not exist, and with SME a
signal frame's `sve_context` outside streaming mode is a bare header with
vector length zero. The bridge tail holds no SVE state, so a task never keeps
P or FFR across an entry.

XNU delivers a signal to a thread in streaming mode without leaving it, and
returning from the handler loses the upper bits of the Z registers. With SME,
a vCPU carrier therefore blocks the IPI wake signal SIGUSR1 outside its idle
`pselect()` and blocks the process-directed SIGINT, SIGTERM, SIGHUP, SIGQUIT
and SIGCHLD throughout, so the process takes those on a thread that runs no
guest. Every other carrier wait polls the pending IPIs or does not depend on
EINTR. Without SME no carrier blocks these signals.

Every arm64 task has `NT_ARM_TLS`, as natively: TPIDR_EL0, then TPIDR2_EL0,
which reads as zero and ignores writes unless the run has SME.
`guest-sme-off-gadgets` and `guest-sme-off-jit` check it without SME.

## Memory and address space

The native mapping window contains guest RAM plus an auxiliary address-space
allowance equal to RAM, bounded from 1 GiB to 4 GiB. Vmapped task stacks and
other kernel virtual mappings use that allowance; their physical pages still
come from the guest's `mem=` allocation. The window additionally carries the
fixed vhost-user shared-memory reservation described below plus the stub pages,
added after that bound so the aperture never reduces the vmalloc allowance or
the configured guest RAM. The host emulator admits the same span
(`UML_NATIVE_VHOST_USER_SHMEM_RESERVATION` and `UML_NATIVE_STUB_PAGES` in the
top-level CMake layout); a host that reserves less faults on the kernel's own
stub write, so the two sizes move together.

The window lies inside every task's address range, and `mmap()` refuses any
user mapping that would enter it. A user TLB flush can still cover it: freeing
page tables widens the flush to each table's whole span, which starts inside
the window when a table also maps the first pages above it. Such a flush drops
and replays only the guest ranges beside the window, because the host would
take a range inside it for the kernel's own mappings.

Darwin UML negotiates the standardized vhost-user shared-memory-region
transport (`VHOST_USER_PROTOCOL_F_SHMEM_MAP`), obtains one region through
`VHOST_USER_GET_SHMEM_CONFIG`, and services acknowledged backend
`SHMEM_MAP`/`SHMEM_UNMAP` messages with exact descriptor, range, permission,
and owner validation. The kernel reserves a 1 GiB guarded guest-physical
aperture outside RAM, enforces 16 KiB mapping alignment (the host page), and
supports one mapping per 16 KiB aperture page through a
constant-time page-to-record index. Reset, disconnect, and release revoke every
mapping in bounded descriptor batches, and a request-stream lock serializes the
backend-request IRQ against the drain in every synchronous reply wait
([architecture](../../docs/architecture.md)) so two vCPUs cannot split one
framed request. The virtqueue and config interrupts either path records are
taken and cleared under the same lock before they are delivered, so a
recorded interrupt is never lost between two deliverers. Virtio-GPU
host-visible blobs use the same 16 KiB granularity without a private userspace
ioctl or a bridge ABI change. Android's Venus Vulkan HAL needs the aperture:
Venus refuses to initialize without `VIRTGPU_PARAM_HOST_VISIBLE` or guest VRAM.

The kernel requests the runtime's anonymous physical-memory token. The
kernel's linear physical window becomes the RAM root, so MMU and host-service
consumers do not retain a second full-span mapping. Guest RAM is committed on
demand: every chunk of the anonymous root starts released, `mem_init` commits
what memblock reserved, `arch_alloc_page()` commits a released chunk the first
time the allocator hands out a page of it, and the free-page reporting driver
releases each whole reported chunk through the bridge. Physical iOS wires only
committed chunks. `page_cache_trim` reclaims inactive page cache above a floor
(`page_cache_trim.floor_mb`, by default 1/32 of RAM) through memcg proactive
reclaim, which leaves anonymous memory alone, and only when the host asks. The
management console's `trim on` and `trim off`, like `page_cache_trim.enabled`,
start and stop a pass every `page_cache_trim.interval_ms` that takes a quarter
of the excess. While more than a quarter of what a pass reclaimed is read back
before the next one, as when a program starts, the interval doubles, up to 32
times, and a pass whose evictions stay out restores it. `trim once` takes the
excess for at most a second, then compacts memory for at most another, since
the pages reclaim frees lie between pages still in use and only whole chunks go
back, then runs free-page reporting until it finds nothing more to release. It
replies with both amounts and each step's time, so a host can suspend a guest
that holds no cold cache. `guest-page-cache-trim` checks the default, all three
commands, the back-off, and a pass over file cache read between the pages of a
tmpfs file.

Every slice selects `HAVE_ALIGNED_STRUCT_PAGE` and `HAVE_CMPXCHG_DOUBLE`, as
native arm64 does; the x86-64 userspace slice keeps the arm64 kernel. SLUB
therefore updates a slab's freelist and counters with one CASP, which runs
directly on the host CPU, and `kmalloc_nolock()`, which BPF timers allocate
with, can use every cache. A build assertion checks that the FLATMEM memory
map's `SMP_CACHE_BYTES` allocation alignment covers the double-word alignment
this needs.

Threads of one process replay their mappings to the host side by side; a user
`mm` has no replay lock, and the host orders its walkers instead. A batch
records the generation's invalidation epoch when it begins, Linux invalidates
the host range after it changes a PTE and before it frees the page, every
invalidation advances the epoch, and a batch publishes only while the epoch
still matches, so a walker that sampled a PTE before it changed cannot publish
it afterwards. Two walkers of one page can still publish out of order across a
change that needs no invalidation. Every such change makes the PTE more
permissive, so the stale batch leaves the page less accessible than Linux
allows and the next access replays it. A user `mm` queues exact page ranges,
while `init_mm`, which the host maps natively, still rounds to the 16 KiB host
page. A fault replays before Linux resolves it only when the PTE already grants
the access, and then brings its 16 KiB neighbours along; the replay after
resolution covers the faulting page and joins the pending snapshot under one
lock hold, so a sibling cannot take the range away and leave the task to return
to a page nobody has published. A user replay is published in batches of 64
pages which span the ranges of its snapshot, and only a rejected batch is walked
again, with a timer, IPI and scheduling checkpoint after every full batch and
every sixteenth rejection, so a whole-`mm` replay after a fork makes progress
while siblings keep invalidating. Within a page table the walk sends a run of
neighbouring user PTEs as one host call when they are all absent or map
consecutive offsets of one backing with one protection; it sends the run when
the next PTE breaks it, when the page table ends or when the batch is full, and
a walk that stops early drops its unsent run, because its range is walked
again. `guest-tlb-replay-runs` checks the runs' frames and protections.
`init_mm` keeps its serialized walker and its per-page calls.

`init_mm` has one synchronizing owner at a time and walks without a batch
budget. A synchronization its owner requests fails at once instead of
blocking, and the entry for callers that already hold an `mmap` lock accepts
only a user `mm`. Every synchronization takes and clears the pending ranges
under one lock hold, applies all of them, and requeues all of them when any
part fails, so no pending range is lost. One 16 KiB host slot carries four
`init_mm` PTEs: they are sampled under the PTE lock, the
whole host slot is dropped before the present quarters are mapped and the
absent ones unmapped, and the first failure stops the slot. A slot whose four
PTEs are present, name consecutive frames from a 16 KiB-aligned offset of one
backing, and agree in protection is mapped as one shared host page, so it is
the same memory as the frames' other views; an empty slot is unmapped in one
call; every other slot is a host-private copy that is coherent with the frames
only at a synchronization point. The four samples are not one atomic snapshot;
a writer publishes its range after it changes a PTE, so a later walk reapplies
it. A walk applies only the `init_mm` PTEs that carry `_PAGE_NEEDSYNC`, and so
only the slots one of whose quarters carries it, and clears the
bit by compare-and-exchange once the host mapping is applied. A writer that
changes the PTE meanwhile has set the bit again, and its own flush applies the
new value. A vmalloc purge flushes the whole span between the areas it frees,
so without the bit such a walk mapped every present page of the span again.
User PTEs keep the bit. A kernel-mode fault marks and publishes exactly its
Linux page, synchronizes `init_mm`, and resumes only when the host confirms
the access is backed; any other outcome panics instead of faulting again. A
nested mmu-gather flush keeps the range it collected
instead of becoming a whole-`mm` flush, because a Darwin range invalidation
already quiesces the generation's executors and advances its epoch.

With 4 KiB pages, a kernel alias outside the linear map is the same memory as
the frames' other views only where one 16 KiB host slot holds a whole aligned
group of four, so the architecture reports a vmap group of four pages. Four
shared topics make that so for memory the kernel shares: the vmalloc group hook
in `mm/vmalloc.c`, the host mapping of congruent `init_mm` slots with the
`flush_kernel_vmap_range()`, `invalidate_kernel_vmap_range()` and
`flush_cache_vunmap()` hooks in `arch/um`, the io_uring region allocation, and
the fbdev shmem flush for the virtio-gpu console. vmalloc aligns, rounds and
allocates every area in groups and `vmap()` aligns its area to a group, so BPF
maps and ring buffers, perf and PACKET_MMAP rings and the io_uring regions that
the kernel allocates share their memory with user space. An area that cannot be
grouped falls back to small pages only when it is kernel-private (never a
`VM_USERMAP` one), and `remap_vmalloc_range` warns once about memory that is
not whole groups. The kernel-vmap hooks publish and refresh the private copy of
a slot that cannot be shared, as they do for the callers that already bracket a
vmap alias (XFS, dm, the DMA-BUF heaps). `flush_cache_vunmap()` publishes the
private slots of a range before Linux clears its PTEs, so the stores of a
`vm_map_ram()` alias, such as EROFS's LZ4 output, reach the page cache before it
is read or freed: a kernel TLB sync applies only changed PTEs, so without it
they would stay in the host's copy while the linear map read stale frames. The
first three topics are inert on any architecture that reports no vmap group,
and the flush costs a page-table check there. Pages that user space owns cannot
be regrouped, so io_uring refuses with `EINVAL` an `IORING_SETUP_NO_MMAP` ring,
a registered memory region or a provided-buffer ring whose pages are neither
one folio nor whole aligned groups; a ring in one page of the application's
memory is one folio and stays valid, so the x86_64 slice's pinned-ring
accounting below sees no change. The BPF ring buffer, a topic of the mainline
overlay (`kernel/bpf/ringbuf.c`), is laid out so that the first data copy ends
on a group boundary, and a buffer under 16 KiB brackets its wrapped tail with
the vmap hooks. Perf no longer selects `PERF_USE_VMALLOC`, as native arm64 does
not. The virtio-gpu console draws into a vmap of scattered shmem pages, which
is a private copy on this profile, so the fbdev shmem dirty callback flushes
the damaged lines before the device reads them; without it a VNC client sees a
blank console. The arm64 kernels run `guest-kernel-user-shared-memory` and
`guest-erofs-lz4`, which check the sharing in both directions and read back an
LZ4 EROFS image with 4 KiB clusters
([`tests/integration/kernel`](../../tests/integration/kernel)); the x86_64 slice
runs the latter's runner directly.

In the x86_64 slice, a guest entry is marked shared while another agent may
observe the memory of the task's mm: while another task may run on it
(`mm_users` beyond the entering task, where a vfork child does not count its
waiting parent), while it maps a `VM_MAYSHARE` vma (any `MAP_SHARED` mapping,
read-only ones, SysV shm, memfd and kernel rings included), or while io_uring
holds pinned memory of it for a ring (`IORING_SETUP_NO_MMAP`, provided-buffer
rings), which completions and other rings' `IORING_OP_MSG_RING` write from
other CPUs. The kernel works this out for every entry: `mmap_region()` sets
the mapping condition, the unmap of a shared vma (`munmap()`, `mremap()`,
`shmdt()`, a `MAP_FIXED` replacement) rescans the vmas, a fork child has it
only if a shared vma came with it, and each pinned ring region counts on its
mm until io_uring frees it. The entry carries `DARWIN_UML_DISPATCH_MM_SHARED`,
and a host may rely on that meaning from bridge ABI 19: once the last
condition ends, every store another agent made is visible to the first entry
without the flag, since the entering task reads `mm_users` and the pin count
with acquire loads that pair with `mmput()` and the unpin, and a new sharer
always starts inside a system call of a task on the mm. The x86-64 JIT keeps
x86's store order for marked entries only. Writes through `ptrace()`,
`process_vm_writev()` or `/proc/<pid>/mem`, and a `MAP_PRIVATE` view of a file
that another process writes through `MAP_SHARED`, do not mark the mm, and a
temporary `mm_users` reference such as a `/proc` reader marks entries only
while it lasts. Only the x86-64 JIT reads the flag, so the work is under
`UML_DARWIN_MM_SHARED`, which the x86_64 slice selects (it selects the generic
`ARCH_HAS_MM_SHARED_HOOKS` that makes io_uring record the pinning mm). The
arm64 kernels build none of it: their entries, mmaps, munmaps, forks and ring
pins do no mm-shared work, and the flag stays clear there, where the arm64
host does not read it.
`tests/integration/x86_64/run_guest_x86_64_mm_shared.py` checks each cause and
each way it ends, and `run_guest_x86_64_tso_litmus.py` that memory order holds
through cycles of sharing.

From bridge ABI 20, a guest entry of an x86_64-slice task carries in `fp` the
address of its mm's LDT, 8192 descriptors in x86 format with zero where no
entry is installed, or 0 while the mm has none, and in `compat_tls` the i386
TLS descriptors of a task of either ABI, since a 64-bit task installs them
too, with INT 0x80 `set_thread_area()`, as Wine's WoW64 does. The kernel
changes an LDT descriptor only with one aligned 8-byte store and frees the
table only with the mm, so the host reads it while the mm's tasks run. Both
engines' shared segment model reads the two fields. The arm64 kernels set
neither, and the arm64 host reads neither; the implementation is the x86_64
slice's (see the [mainline README](../mainline-6.18/README.md)).

## CPU identity and AArch32

An arm64 task reads `/proc/cpuinfo` exactly as arm64 Linux's `c_show()`
prints it for a 64-bit task, and both engines give the same bytes. Each online
CPU has a block of `processor`, `BogoMIPS`, `Features`, `CPU implementer`,
`CPU architecture`, `CPU variant`, `CPU part` and `CPU revision` lines, ended
by a blank line. arm64 Linux prints a `model name` only under `PER_LINUX32`.
`BogoMIPS` is what arm64 Linux derives from its architected timer,
`DIV_ROUND_CLOSEST(CNTFRQ_EL0, HZ)` taken as `loops_per_jiffy`, for the counter
frequency a task reads; UML's calibrated delay loop still times `udelay()` but
no longer reaches the file. `Features` names exactly the AT_HWCAP and AT_HWCAP2
bits every task receives, in the order and spelling of arm64 Linux's own
`hwcap_str` table, which the build takes from `arch/arm64/kernel/cpuinfo.c`.
The identification lines decode the MIDR_EL1 value a task reads with MRS,
which the kernel mirrors from `darwin/cpu/arm64/id_registers.hpp`, and
`CPU architecture` is 8, as on arm64 Linux. LLVM's host feature query, and
with it llvmpipe and `-mcpu=native`, reads the `Features` line.
`guest-gadgets-smoke` and `guest-jit-smoke` compare the file with the
auxiliary vector, MRS and the kernel's `CONFIG_HZ`, and require LLVM to see the
host's NEON. The x86_64-userspace slice describes its emulated x86 CPU
instead: its `model name` is the name the host gives the virtual CPU,
`Kernarium Virtual CPU` (`darwin/cpu/guest_identity.hpp`), which is also the
CPUID brand string of both x86 engines and the SMBIOS processor version of
every kernel. The Android image names the same CPU as `ro.soc.manufacturer`
and `ro.soc.model`, which a space joins, and a tooling test requires the two
to agree.

The normal arm64 kernels run AArch32 tasks, armhf (the hard-float EABI) and
Android's armeabi-v7a (the softfp EABI), both ARMv7-A and Armv8 AArch32 with
VFPv4-D32 and NEON in hardware, beside
AArch64 ones, as arm64 Linux's `COMPAT` does. Two shared topics carry it. The
generic `arch/um` topic gives a task its own ABI: `TIF_32BIT` with its own
`TASK_SIZE`, `STACK_TOP` and mmap base, a syscall table, count, `sched_yield`
number and out-of-table handler picked per task through the `UML_SYSCALL_*`
hooks of `skas/syscall.c`, and an `asm/compat.h` that includes the port's
`asm/compat-abi.h`; the x86_64 slice's i386 support uses the same hooks. The
arm64 topic supplies the port's AArch32 support in `arch/arm64/um`: ELF32
loading, the kuser helpers page and the signal-return page as ordinary code
pages the kernel installs, 32-bit signal frames and system calls, the
ARM-private calls, `cacheflush()` through `flush_icache_remote_user_range()`
and `darwin_uml_vm_invalidate_code()`, the `NT_PRSTATUS`, `NT_ARM_VFP` and
`NT_ARM_TLS` regsets with arch/arm's ptrace requests (`GETREGS`, `GETVFPREGS`,
`GET_THREAD_AREA`, `SET_SYSCALL`), and a compat `vm.mmap_rnd_compat_bits` of
11 up to 16. Both Android and mainline build it (`CONFIG_COMPAT`,
`CONFIG_KUSER_HELPERS`); the x86_64 slice turns `UML_ARM64_AARCH32` off.

Bridge ABI 22 carries the task: nRW in `pstate`, R0-R14 in x0-x14, D0-D31 in
V0-V15, FPSCR split between `fpsr` and `fpcr`, TPIDRURW in `tls` and the new
`tpidrro` for TPIDRURO. The comments on `struct darwin_uml_guest_registers`
in [`abi/darwin_uml_abi.h`](abi/darwin_uml_abi.h) define the registers and what
each event means, and `abi.cp15_barrier` (0 to 2, default 1) is passed as
`DARWIN_UML_DISPATCH_A32_CP15BEN`. The engines raise the
undefined-instruction event for SWP and SWPB whatever their condition, and
`abi.swp` (0 or 1, default 0, as on arm64 Linux; Android's init sets 1)
decides what the kernel does with it: 0 delivers SIGILL, and 1 emulates the
instruction as arm64 Linux's `armv8_deprecated.c` does. The exchange is one
atomic operation on the page the task sees, so it is atomic against exclusive
pairs, atomic instructions and futex operations on other vCPUs. An access
that fails, an unaligned SWP included, raises SIGSEGV with the pc at the
instruction, a failing condition retires the instruction untouched, and a
stepped one ends with `TRAP_TRACE`. A pc operand, which the architecture
leaves UNPREDICTABLE, is UNDEFINED, where arm64 Linux would use an unrelated
register. SETEND is UNDEFINED, as on a CPU without mixed-endian EL0, and has
no switch.
AArch32 is on only when `darwin_uml_guest_compat_hwcap()` is non-zero. That
word always names the architecture's baseline, so it is the single source of
truth and no separate query exists. While it is zero, as in a run without an
AArch32 engine, a 32-bit ELF file fails with `ENOEXEC` and
`personality(PER_LINUX32)` with `EINVAL`, as on an arm64 CPU without 32-bit EL0,
and an AArch32 entry is `FATAL`. The word must not set `VFPD32`, which arm64
Linux's `hwcap_str` has no name for; `/proc/cpuinfo` would warn.

Under `PER_LINUX32`, whatever the reader's width, `uname()` reports `armv8l`
and `/proc/cpuinfo` is arm64 Linux's layout for a 32-bit task: each block
adds `model name : ARMv8 Processor rev N (v8l)` after `processor`, and
`Features` names the compat hardware capabilities.

Guest pages are 4 KiB, so 32-bit programs laid out for 4 KiB pages, as the
files of stock Debian armhf userspace are, run as they are.
`tests/integration/aarch32` runs one.

## Scheduling and interrupts

Darwin's cooperative scheduler services timer deadlines and pending IPI vectors
at safe kernel spin, retry, TLB-walk, and `cond_resched()` boundaries, and in
`schedule()` when the caller stays runnable. The uaccess retry and TLB-walk
safe points poll through `cond_resched()`, which does so only without
`CONFIG_PREEMPTION`; both assert that at build time. The TLB walk of
`flush_tlb_mm()` is a safe point only for its direct callers, `dup_mmap()` and
the soft-dirty clear, which hold no spinlock or RCU read lock. The mmu_gather
flushes under the PTE lock and never reaches it: the port's own `tlb_flush()`
walks no VMA, and `exit_mmap()`'s full-mm gather does no host work, because
`arch_exit_mmap()` has already emptied the host generation and no task is left
to map into it. A uaccess that may sleep
retries its fault until the fault handler refuses it or the task is dying, as
native uaccess re-executes the access, so a sibling's `fork()` that
write-protects the page again after the fault never fails the copy with
`-EFAULT`. That fault is the kernel's own access, so it carries no
`FAULT_FLAG_USER`: a userfaultfd made with `UFFD_USER_MODE_ONLY` refuses it
and the copy fails with `-EFAULT`, as it does natively, while a user-mode
access to the same range is still delivered. A uaccess write holds its page
with a pin taken under the page-table lock, so a `fork()` meanwhile gives the
child a copy and a store through the kernel's alias never reaches it.
Every path that changes a CPU's
logical IRQ state outside `local_irq_*()`, including interrupt work run from an
idle wait, reports the change to lockdep as interrupt entry would; without
`CONFIG_TRACE_IRQFLAGS` the reports compile away. The IPI signal is only a
wake edge, so a task that yields in a retry loop would otherwise stop its CPU
whenever nothing else is runnable there: `tty_open()` retries that way while
the last
close of a terminal is in progress, and a shutdown that restarted a console
getty stopped for minutes, with `rcu_sched` stall reports, waiting for a timer
and a queued wakeup that the looping CPU never serviced. A pagefault-disabled
uaccess
never reports mmap-lock contention as a fault: `mmap_read_trylock()` refuses to
barge past a queued writer while the faulting retry of the same task still
steals the lock, so answering the try with `-EFAULT` made `futex_wait_setup()`
alternate a failing atomic read with a succeeding faulting one and never yield
the CPU that writer needs. The contended atomic walk runs under RCU instead,
which `MMU_GATHER_RCU_TABLE_FREE` keeps safe, and the page-table lock still
samples the entry.

An idle virtual CPU never blocks while one of its IPI vectors is pending. A
remote sender writes the idle wake pipe only when it takes the target's pending
mask from zero to non-zero and sends no signal to a target marked idle, so a
bit that is already set when the wait starts has no wake edge behind it. The
local irq_work vector sets its bit without a pipe edge and can be left set with
nothing queued, and a CPU that blocked on it would absorb every later remote
vector until an unrelated descriptor or timer woke it. The idle wait reads the
mask after publishing the idle state and polls instead of blocking.

Every kernel product performs futex updates atomically against native
userspace running on other vCPUs. Compare-and-exchange reports the value
observed by that operation, and wake-operation arithmetic retries competing
updates instead of overwriting them. The optional `guest-futex-atomic` test
checks concurrent kernel/userspace updates and robust priority-inheritance
mutexes.

## Block devices and filesystems

Releasing an ext4 inode preallocation never allocates, so evicting a cached
inode from the inode shrinker, which runs under `PF_MEMALLOC`, does not trip
the page allocator's check against `__GFP_NOFAIL` requests there. While a
block group holds an inode preallocation, a shared topic keeps references to
the group's in-core bitmap and buddy folios and its on-disk block bitmap
buffer, so the release finds all three cached. The references cost at most
three 4 KiB pages per such group and keep those pages from being reclaimed or
migrated. Closed inodes keep preallocations, for example after enabling
fs-verity, so the release is not confined to open files.
`guest-ext4-prealloc-reclaim` evicts such inodes under memory pressure, on
4 KiB and 1 KiB blocks, and requires that reclaim release them with no
`__GFP_NOFAIL` page allocation.

The block allocator indexes each block group by its largest free order and its
average fragment size in xarrays, and moves a group between them under the
group lock, so the insert can use only the atomic reserves and fails when
memory is that short. A group left out is still found by the linear scan of
the later criteria, so a failed insert costs a worse choice of group and never
an allocation. A shared topic inserts with `__GFP_NOWARN`, because
`warn_alloc()` printed a stack trace from writeback in the middle of the
pressure that caused it, which the test above rightly rejects. It also checks,
when a group's order is unchanged, that the group is indexed and indexes it
again if not, where upstream returned early and left the group out until its
order changed. The extent status cache is filled the same way: the extents a
lookup reads are inserted under the inode's extent-cache lock with an atomic
allocation, a miss only means the extent tree is read again, and a second
shared topic adds `__GFP_NOWARN` to that allocation for the same reason.

Direct UBD images advertise discard ranges up to 128 MiB and service each range
with one host hole punch. A UBD request carries its host I/O state in the data
blk-mq allocates with the request, and the work item that runs it walks its
segments itself, so accepting or running a request allocates no memory, even
during writeback under memory pressure. `guest-ubd-request-memory` traces every
allocation while it drives each kind of request. The project adds no behavior
to upstream UML's COW implementation, and Apple launchers never construct a COW
image pair.

## Shutdown and device removal

UML runs its exitcalls, which tear its drivers down, while the other vCPUs
still run tasks, and `poweroff -f` leaves every task running. The UBD exitcall
therefore quiesces every disk before it destroys the workqueue that runs their
requests: the requests already handed to it complete, and a later one waits in
its queue until the run ends, as on a machine that has stopped.
`guest-ubd-poweroff` powers guests off with direct reads in flight on every
vCPU.

A management-console halt or reboot, an unbind and a lost vhost-user
backend reach a driver's removal without `device_shutdown()`. The Virtio
video driver's removal, for a decoder and an encoder alike, resets the device
and deletes its queues before it returns anything the device held, then
answers every pending command as failed, so a queued buffer returns to vb2 in
error, and flushes its event work. Its state lasts until the last open file is
closed, which no longer reaches the device. `guest-video-teardown` removes
each device by unbind, halt and forced power-off while it has open files,
queued buffers and commands in flight.

A synchronous video command waits five seconds for its answer. On Darwin its
answer is already in guest memory when the kick returns, because the backend
runs the queue before it acknowledges the kick and the device answers such a
command at once, so the timeout can only race the completion interrupt; a
device that queues its work may still answer later. A command therefore owns
its response memory and copies a timely answer to its caller, and a waiter
that gives up leaves the command allocated for the late answer, or the
removal, to free. An open that fails after the device made its stream
destroys that stream. `guest-video-late-answer` serves the decoder from the
production device behind a test-only shim that answers one parameter query
late and another never.

## Devices

The Darwin network receive path uses NAPI, and the display-mapped absolute
pointer is a direct touchscreen.

A virtio-gpu PRIME export is always read-write. The first export of a buffer
fixes the mode of every later one, and guest Mesa exports without `DRM_RDWR`,
so a buffer it exported first could otherwise never be mapped writable.

A virtio-gpu device with 3D refuses a dma-buf another driver exported (udmabuf,
a DMA-BUF heap) with `-ENODEV`: the driver has no way to import a foreign
scatter-gather table, and stable 6.18 reverted the change that routed such a
buffer to the generic importer. A buffer this device exported still imports as
its own object. Do not restore the change; a consumer that needs foreign
memory in the GPU must take it through the device's own allocation. No product
path imports foreign memory: gralloc, screencap, and hardware video encode and
decode import only the device's own exports.

The virtio-gpu resource-creation ioctl, for a 2D or classic 3D resource,
waits for the host's answer. The driver queues the fenced creation, kicks the
control queue and waits (at most 15 s) for the response before it queues the
attach-backing. A host that answers out of memory fails the ioctl with `-ENOMEM`
(any other error with `-EINVAL`) and the object is released without a
`RESOURCE_UNREF` and without a command the host would answer with an error, so
a host that bounds the memory it gives the guest refuses a texture or buffer
visibly: Mesa raises `GL_OUT_OF_MEMORY` and Android's gralloc fails the
allocation. The wait costs one control-queue round trip per creation; blob and
dumb creations are unchanged.

A classic 3D resource, a single-sampled 2D or rectangle Virgl texture, is
coherent with the GPU through its dma-buf around `DMA_BUF_IOCTL_SYNC`. Its
guest pages change only on an explicit `TRANSFER_FROM_HOST_3D` or
`TRANSFER_TO_HOST_3D`, which Mesa issues for its own maps and not for a
dma-buf. A sync start waits for the buffer's write fences, then reads level 0
from the host and waits for the transfer, and a sync end that wrote sends
level 0 to the host. Blob resources, 2D resources and every other shape keep
their behaviour, so Android, whose gralloc locks with its own transfers and
whose GPU-only buffers are blobs, is unaffected. Each sync is a synchronous
round trip that copies the whole level, because the ioctl carries no range,
and a write end adds a second one. A start also replaces guest-page writes no
transfer has taken to the host, so CPU access must be bracketed and a producer
must flush its commands before it hands a buffer over. The optional
`guest-gbm-dmabuf` test checks the export mode and both directions of
visibility.

virtio-gpu has no vertical blank, so the driver paces each CRTC with the DRM
vblank timer at its mode's refresh rate. A page flip's event and out-fence
signal at the next tick, not when the flip is queued, and the next commit waits
for them, so a client that flips faster is held to the display's rate.
Android's drm_hwcomposer schedules a frame against the present fence and so
presents it no earlier than it asked for. The timer runs only while a client
holds a vblank reference, so an idle display costs nothing. The commit tail
waits for the commit's own flip event instead of counting a vblank as the
default tail does: a loaded host can delay the timer past the 100 ms after
which the count warns, and a commit must still complete on its flip, or the
nonblocking flip a client queues right after a blocking one is refused with
`-EBUSY`. No scanout needs the wait, because the host copies the frame when the
driver flushes the resource. The optional `guest-drm-vblank` test checks the
vblank wait and the flip rate against the mode's refresh rate.


A Virtio-GPU file on a device with context initialization has no context until
it initializes one or its first transfer, submission, or resource creation
creates it. Every handle the file opened before then, typically a PRIME
import, is attached to the context when it is created, as a handle opened
afterwards is when it opens; closing one first drops it without a detach.
Upstream attaches only handles opened after the context exists, so the host
refused every transfer that named such a buffer. Android's gralloc mapper reads
an imported buffer exactly that way, which blanked every screenshot.

The Virtio-GPU primary plane accepts ARGB8888 as well as XRGB8888, so a GBM
client whose EGL config has alpha can set a mode. XRGB8888 stays the first
format, the one fbdev takes. The plane has no blend-mode property. The host
presents a scanout's colour channels and ignores its alpha byte, which is what
premultiplied alpha over the CRTC's black background shows. The
[Android product](../mainline-6.18/README.md#android-product) offers the other
32-bit RGB orders as well.

## Suspend, wake and the management console

Lifecycle quiesce uses full Linux system suspend. The management console
acknowledges only after the suspend entry boundary and remains the explicit
wake source, so monotonic guest uptime excludes time while an Apple host has
frozen the application. Multi-vCPU suspend parks secondary carriers through CPU
hotplug and restores them before guest tasks resume. The console socket is
`/tmp/uml-<umid>-mconsole`, or `<dir>/mconsole` when the host passes
`uml_dir=<dir>`, a directory owned by that machine alone; a path that does not
fit Darwin's 104-byte `sun_path` is refused, never truncated.

The console's `mem` replies `total=<n> available=<n> free=<n>`: the MemTotal,
MemAvailable and MemFree that `/proc/meminfo` prints, in decimal bytes, with
one space between the fields and nothing after them, as one reply and no
error. It reads `si_meminfo()` and `si_mem_available()` from the console's
interrupt, with no file and no process context, so it answers where a policy
keeps the console from `/proc/meminfo`, as on Android. A kernel without the
command refuses it as an unknown command.

Only a wake source ends the suspend. Some wake any suspend: the management
console, the UML RTC alarm, VNC input, and a vhost-user device whose wakeup is
enabled in sysfs (its backend channel and every queue's call descriptor
alike). The Darwin network interface wakes only a suspend the guest started
itself, such as Android's once its last wakelock goes away, as a phone's
wireless interface does: a received frame ends it and keeps the guest awake
for 500 ms through the `darwin-net-rx` wakeup source, so that the stack and
the task the frame is for answer before the next automatic suspend, and an
`adb` command to a guest whose display is off is answered at once instead of
at the next RTC alarm. A suspend the host asked for through the management
console ends only with the host's wake, and no frame fails it; the kernel
knows it from the host's request until the guest has replied that it is
suspended. `darwin_net_kern.guest_wake=0` turns the wake and the hold off. 

A Linux host clears O_ASYNC on every other descriptor for the suspend; Darwin,
whose IRQ notification thread stands in for SIGIO, takes those descriptors off
that thread instead, so console input, vhost-user requests and block
completions wait for the resume and are handled then, and so does network
input in a suspend the host asked for. Those descriptors stay nonblocking: a
vhost-user reply wait still serves its backend's requests during a suspend,
and the `irq_work` that delivers what one recorded drains the request socket
until it would block, so a blocking socket would hold that CPU in the host's
`recvmsg()` and the suspend would never end. The suspended CPU
sleeps until a wake source has input or a wakeup is pending, so neither a host
signal nor a notification collected before the suspend ends it. A wake request
that arrives while a suspend is still being entered, whether the host or the
guest started it, is answered at once rather than left on the console, and it
still ends that suspend: the PM core aborts at its next wakeup check, or the
CPU does not go to sleep. The PM core keeps every wakeup signaled since the
suspend began, including one signaled while it syncs file systems, and a
suspend whose sync saw one ends before it notifies or freezes anything; a
guest that is not suspending only answers the request, and its next suspend
discards that wakeup. A suspend the host asked for that ends with `-EBUSY`, as
when one of the guest's own wake sources such as an RTC alarm fired while it
was being entered, is entered again up to three more times; the host's wake
ends the request instead, and a request that gives up replies `Suspend
failed`.

Virtio input is not a wake source, because a suspend disables its rings, so a
host that forwards a key also sends the wake. `guest-suspend-holds`
keeps network, console and vhost-user traffic flowing through a suspend the
host asked for, sends early wakes, one of them while a frozen file system
holds the suspend's sync, fires an RTC alarm during a held sync of the host's
suspend, which must still hold, and wakes a suspend the guest entered by
itself with a wake, with a power key typed on the VNC display and with a
network connection, after which the host's own suspend must hold under
traffic on its first attempt. 

The Virtio sound driver starts a substream afresh
after a suspend, with no position and no pending message, whether it was
playing, paused or prepared, because the ALSA core restarts it at offset 0
and the driver releases on the device the stream of a substream the core did
not run, before the rings are disabled; UML runs no virtio freeze, so nothing
else would tell the device. The driver reports elapsed periods one at a time,
so a guest stall longer than the buffer xruns, or carries on when the stop
threshold is the boundary; it releases the periods queued before a start in
prepare, hw_params and hw_free; and it pads the last period of a drain with
silence. `guest-virtio-sound-suspend` plays across a suspend, a pause and a
reopen, across a 300 ms stop of the guest, and through the drains.

## BPF

The kernel exposes its BTF at `/sys/kernel/btf/vmlinux` for CO-RE programs.
BPF runs on the interpreter; no BPF code is generated at run time. Helpers
that take a callback, such as `bpf_loop`, `bpf_for_each_map_elem`, and
`bpf_timer_set_callback`, reach an interpreted callback through a fixed pool of
1024 built-in entry points. Each callback subprogram of a loaded program holds
one until the program is freed, and a load that finds none free fails with
`ENOSPC`. The interpreter still rejects kfunc calls, and tail calls in a
program that has callbacks or other subprograms; `bpf_wq`, which only kfuncs
reach, is therefore unavailable. Tracing program types (kprobe, uprobe,
tracepoint, raw tracepoint, iterator) need `CONFIG_BPF_EVENTS`, which UML does
not provide.
