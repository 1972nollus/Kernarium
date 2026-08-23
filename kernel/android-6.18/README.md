# Android Common Kernel 6.18 source policy

This line starts from ACK tag `android17-6.18-2026-06_r35` at the exact commit
and tree recorded in [`patches/metadata.env`](patches/metadata.env). It is the
kernel source for the AOSP 17 guest only and does not replace either current
mainline page profile.

The checked series contains 29 ACK-based topic commits and resolves to the
exact final tree recorded in the metadata. The first 16 topics port the
applicable Darwin UML behavior; the final thirteen are ACK-only local topics
discovered by compiling and booting the 16 KiB Android product. Mainline patch
bytes are never installed as this line's source of truth.

The Android producer is owned by
[`../../android/aosp-17/`](../../android/aosp-17/README.md). Its scripts execute
locally on an x86_64 Linux builder and receive the ACK Repo workspace as a local
path. This directory contains no builder endpoint or transfer workflow.

The product has exactly one 16 KiB configuration in
[`config/android.fragment`](config/android.fragment). Loadable modules are
forbidden: `CONFIG_MODULES=n`, Binder, ashmem compatibility, SELinux, audit,
DMA-BUF heaps, storage, networking, and guest devices resolve built-in, and no
`.ko` or DLKM output is accepted. Android userdata metadata encryption also
requires the built-in block inline-encryption core, its software fallback,
fscrypt inline hooks, `dm-default-key`, and the software `hctr2(aes)` transform
used by the product's `aes-256-xts:aes-256-hctr2` file-encryption policy; UBD
has no hardware crypto offload. These remain built in because the iOS product
cannot recover a missing storage or crypto feature by loading a module.

Topic 24 closes a fixed-carrier scheduler deadlock exposed by Android's
multi-vCPU `apexd` workload. Darwin switches to cooperative per-vCPU timers
after bootstrap, so an optimistic kernel lock spinner could remain in
`cpu_relax()` without receiving the tick that sets `need_resched()`. That
starved the lock owner and eventually produced an RCU stall. The fix keeps IPI
service immediate, then runs a throttled timer checkpoint only from an
IRQ-enabled kernel spin boundary, recomputing the logical CPU after IPI work in
case the Linux task migrated. A four-vCPU gadgets run advanced continuously to
guest second 198 without an RCU stall; the former kernel reproduced the stall
at guest second 56–58.

Topic 28 implements `arch_irq_work_raise()`, which UML never provided. The
generic weak stub left queued work waiting for the next timer tick, and
`bpf_mem_alloc` prefills exactly one element per CPU once the unit size exceeds
256 bytes and refills the rest from irq work. A non-preallocated BPF hash map
with a value larger than a couple of hundred bytes therefore accepted one
insert per CPU per tick, which made `ConnectivityService` fail its
1024-byte-value uid permission chunk map with `-ENOMEM` and crash-loop
`system_server`. The topic publishes a self-directed IPI vector that the
cooperative drain already reaches long before the tick, and leaves
`arch_irq_work_has_interrupt()` false so the tick keeps its backstop.

Topics 25 and 26 cover longer non-spinning kernel retry paths. Topic 25 adds
explicit PREEMPT_NONE quiescent boundaries after fault-enabled uaccess and
completed host TLB invalidations, once their mmap/page-table or host mapping
locks are no longer live. Topic 26 first polls the cooperative per-vCPU timer
at those IRQ-enabled boundaries, then calls `cond_resched()`. The ordering is
required because the timer raises `need_resched` and wakes the RCU grace-period
thread; a reschedule check alone cannot recover a timer deadline that was never
serviced. Atomic futex operations remain excluded. Release
`20260821-5735514a-rcu-timer1` passed that gate: its four-vCPU gadgets cold
boot reached guest second 982 and a same-COW warm boot reached second 284 with
no RCU-stall, timer-wakeup, oops, or panic signature. The former late warning
occurred at guest second 816. Both runs shut down normally. The accepted ACK
ELF is `6.18.21-g1edaf2cbd207`, final tree
`d1a4d62471db6569cfe5a11e842a5a433d8d297e`.

Topic 27 extends the same cooperative timer rule to the scheduler's generic
`__cond_resched()` boundary. Android's default JIT engine can execute a long
PREEMPT_NONE `execve` path while other vCPUs advance the cooperative clock; in
that case no UML userspace dispatch occurs on the busy CPU soon enough to raise
`need_resched`. When interrupts are enabled, `__cond_resched()` now polls the
per-vCPU Darwin timer before testing the reschedule flag. The checkpoint is a
no-op before cooperative timers become active and is deliberately excluded
from IRQ-disabled call sites. The resulting 27-topic tree builds as a 16 KiB
AArch64 ET_DYN image with an exact bridge-import match and no module output;
multi-vCPU runtime qualification remains a separate gate before release
acceptance.

Topic 29 reshapes the Darwin pointer into a direct touchscreen. Topic 10
exposes the host frontend's absolute window coordinates as an
`INPUT_PROP_POINTER` tablet with `ABS_X`/`ABS_Y` and mouse buttons, which
libinput accepts, so the Linux desktop guests are unaffected and that topic is
unchanged. Android is stricter: `EventHub` recognises an absolute device only
when it also reports `BTN_TOUCH`, and a device matching no input class is
dropped before `InputReader` ever opens it. A fully booted Android 17 guest
therefore listed only the keyboard in `dumpsys input` and ignored every click.
The topic reports `BTN_TOUCH` alongside `BTN_LEFT` and marks the device
`INPUT_PROP_DIRECT`, which selects `TouchInputMapper`'s direct mode and maps
the absolute range straight onto the display. `REL_WHEEL` is no longer consumed
under Android; touch drag is that platform's native scrolling gesture.

The BPF syscall ABI, verifier, cgroup hooks, and generic interpreter are also
built in. This arm64 UML line deliberately has no eBPF JIT: ACK's native arm64
backend assumes CPU-capability, Spectre, register-frame, and exception-table
contracts that do not match UML. The AOSP product carries a narrowly scoped
NetBpfLoad patch and boot property for interpreter operation; other products
retain Android's normal JIT requirement. This is a bring-up accommodation, not
a claim of eBPF-JIT or VTS compliance.
