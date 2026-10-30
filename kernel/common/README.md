# Shared kernel contracts

This directory contains only source-family-independent inputs:

- [`abi/`](abi/README.md): canonical plain-C host/kernel bridge, identity,
  generated-copy authority, and import policy;
- `config/darwin-uml.fragment`: the kernel configuration policy every product
  shares, including the 4 KiB guest page size and the 128 KiB task-stack
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
can take UML's handlers too. Its SIGSEGV or SIGBUS is reported and left to the
host's default action. SIGALRM, which the process-directed ITIMER_REAL may hand
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

Every kernel describes the virtual machine in the SMBIOS tables firmware
writes on other machines, so `/sys/class/dmi/id`, `/sys/firmware/dmi` and the
programs that read them, such as dmidecode and systemd, find it there. The
system, baseboard, chassis, BIOS, processor and memory vendor is `Kernarium`,
the product, family and board are `Virtual Machine`, the BIOS version is the
kernel release, the chassis type is Other, and the BIOS characteristics mark a
virtual machine, so `systemd-detect-virt` reports `vm-other`. The processor is
the virtual CPU the host names for `/proc/cpuinfo`, with the possible CPUs as
its cores and threads, and one memory device is the RAM the guest was given.
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
while `darwin_uml_guest_top_byte_ignore()` is 1, that is only under
`--guest-top-byte-ignore`, because a tagged pointer faults otherwise; without
the flag enabling it fails with `EINVAL` through both interfaces. A fault's
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

With 4 KiB pages, a kernel alias outside the linear map is the same memory as
the frames' other views only where one 16 KiB host slot holds a whole aligned
group of four. Four shared topics make that so for memory the kernel shares:
the vmalloc group hook in `mm/vmalloc.c`, the host mapping of congruent
`init_mm` slots with the `flush_kernel_vmap_range()`,
`invalidate_kernel_vmap_range()` and `flush_cache_vunmap()` hooks in `arch/um`,
the io_uring region allocation, and the fbdev shmem flush for the virtio-gpu
console. `flush_cache_vunmap()` publishes the private slots of a range before
Linux clears its PTEs, so the stores of a `vm_map_ram()` alias, such as
EROFS's LZ4 output, reach the page cache before it is read or freed. The first
three are inert on any architecture that reports no vmap group, and the flush
costs a page-table check there. The x86_64 slice's
pinned-ring accounting below sees no change: a ring in one page of the
application's memory is one folio and stays valid, and a larger one that is
not whole aligned groups is refused with `EINVAL`.
[`tests/integration/kernel`](../../tests/integration/kernel) checks the sharing
in both directions and reads back an LZ4 EROFS image with 4 KiB clusters.

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

Every arm64 task has `NT_ARM_TLS`, as natively: TPIDR_EL0, then TPIDR2_EL0,
which reads as zero and ignores writes unless the run has SME.
`guest-sme-off-gadgets` and `guest-sme-off-jit` check it without SME.

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
instead.

The normal arm64 kernels run AArch32 tasks, armhf (ARMv7-A and Armv8 AArch32
with VFPv4-D32 and NEON, EABI hard-float) and Android's armeabi-v7a, beside
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

Both maintained kernel lines perform futex updates atomically against native
userspace running on other vCPUs. Compare-and-exchange reports the value
observed by that operation, and wake-operation arithmetic retries competing
updates instead of overwriting them. The optional `guest-futex-atomic` test
checks concurrent kernel/userspace updates and robust priority-inheritance
mutexes.

A virtio-gpu PRIME export is always read-write. The first export of a buffer
fixes the mode of every later one, and guest Mesa exports without `DRM_RDWR`,
so a buffer it exported first could otherwise never be mapped writable.

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
