# Shared kernel contracts

This directory contains only source-family-independent inputs:

- [`abi/`](abi/README.md): canonical plain-C host/kernel bridge, identity,
  generated-copy authority, and import policy;
- `config/darwin-uml.fragment`: the kernel configuration policy every line
  shares. Each line applies its own overlay after it, which lists only the
  settings in which that line differs and whose README says why;
- `config/page-16k.fragment`: shared 16 KiB page and 128 KiB task-stack
  geometry;
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

Darwin's `sigprocmask()` sets the mask of every thread in the process, vCPU
carriers included, so host-side kernel code changes a signal mask only with
`pthread_sigmask()`. The bridge import check rejects an image that imports
`sigprocmask`, and the console PTY assertion checks that a console write
leaves another thread's mask unchanged.

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

A task's stack pointer is the task's own. A system call or a fault taken
with it at zero or outside the task's address space returns, or delivers the
signal, that native Linux does, however often it comes on one vCPU.
`guest-stack-pointer` checks this on arm64 and the x86 exception probe on
both x86 ABIs.

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
copies both, ptrace adds `NT_ARM_TLS`, `NT_ARM_SSVE`, `NT_ARM_ZA` and
`NT_ARM_ZT`, and `PR_SME_SET_VL` and `abi.sme_default_vector_length` accept
any length and settle on the SVL. Unlike native arm64, `AT_MINSIGSTKSZ`
counts the ZA data a frame can carry, so a stack of that size holds every
frame. `guest-sme-gadgets` and `guest-sme-jit` check the contract, and
`guest-sme-off-gadgets` and `guest-sme-off-jit` that none of it shows
without SME.

XNU delivers a signal to a thread in streaming mode without leaving it, and
returning from the handler loses the upper bits of the Z registers. With SME,
a vCPU carrier therefore blocks the IPI wake signal SIGUSR1 outside its idle
`pselect()` and blocks the process-directed SIGINT, SIGTERM, SIGHUP, SIGQUIT
and SIGCHLD throughout, so the process takes those on a thread that runs no
guest. Every other carrier wait polls the pending IPIs or does not depend on
EINTR. Without SME no carrier blocks these signals.

An mm is marked shared, sticky for its life, once `clone()` creates a task on
it without `CLONE_VFORK`; each later guest entry of a task on it carries
`DARWIN_UML_DISPATCH_MM_SHARED`, which the x86-64 JIT will use to keep x86's
store ordering only where another task can observe it. The arm64 host
ignores the flag. `tests/integration/x86_64/run_guest_x86_64_mm_shared.py`
checks it.

An arm64 task reads `/proc/cpuinfo` exactly as arm64 Linux's `c_show()`
prints it for a 64-bit task, and both engines give the same bytes. Each online
CPU has a block of `processor`, `BogoMIPS`, `Features`, `CPU implementer`,
`CPU architecture`, `CPU variant`, `CPU part` and `CPU revision` lines, ended
by a blank line. There is no `model name`: arm64 Linux prints one only to a
32-bit task. `BogoMIPS` is what arm64 Linux derives from its architected timer,
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

Both maintained kernel lines perform futex updates atomically against native
userspace running on other vCPUs. Compare-and-exchange reports the value
observed by that operation, and wake-operation arithmetic retries competing
updates instead of overwriting them. The optional `guest-futex-atomic` test
checks concurrent kernel/userspace updates and robust priority-inheritance
mutexes on both Linux page profiles.
