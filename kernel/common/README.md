# Shared kernel contracts

This directory contains only source-family-independent inputs:

- [`abi/`](abi/README.md): canonical plain-C host/kernel bridge, identity,
  generated-copy authority, and import policy;
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
- `tooling/verify-console-input.py` and `tooling/console_input_probe.cpp`: the
  console PTY assertion described below; and
- [`video/`](video/README.md): the private Virtio video wire contract and the
  tool that syncs it into each kernel checkout.

Pinned commits, complete configurations, toolchains, source locations, the
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
`pthread_sigmask()`. Every kernel build rejects an image that imports
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
cancellation inside a system call rely on. `guest-signal-unwind` checks both
and the frame record.

Both maintained kernel lines perform futex updates atomically against native
userspace running on other vCPUs. Compare-and-exchange reports the value
observed by that operation, and wake-operation arithmetic retries competing
updates instead of overwriting them. The optional `guest-futex-atomic` test
checks concurrent kernel/userspace updates and robust priority-inheritance
mutexes on both Linux page profiles.
