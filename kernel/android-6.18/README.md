# Android Common Kernel 6.18 source policy

This line starts from ACK tag `android17-6.18-2026-06_r35` at the exact commit
and tree recorded in [`patches/metadata.env`](patches/metadata.env). It is the
kernel source for the AOSP 17 guest only and does not replace either current
mainline page profile.

The checked series contains 35 ACK-based topic commits and resolves to the
exact final tree recorded in the metadata. The first 16 topics port the
applicable Darwin UML behavior; the remaining nineteen are ACK-only local
topics discovered by compiling and booting the 16 KiB Android product.
Mainline patch bytes are never installed as this line's source of truth.

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

Topic 30 moves the Darwin user-network receive path onto NAPI. Topic 10's
driver drained up to 64 frames straight from the interrupt handler, and the
host readiness descriptor is level triggered, so a sustained inbound stream
re-armed the interrupt as soon as the handler returned and the CPU never left
interrupt context long enough for the backlog softirq to drain. An `adb push`
of a 512 MiB file over a forwarded port wedged a booted guest every time,
while a 3 GiB guest-local write through the same block stack completed in 10 s
at 293 MB/s. The wedged guest still took timer ticks - RCU reported "2515
ticks this GP" rather than the zero that marks Darwin timer starvation - and
its carriers were spinning in `darwin_cpu_relax` and `_raw_spin_lock_bh`, the
per-CPU backlog lock. `arch/um/drivers/vector_kern.c` already runs NAPI on
this architecture; the topic follows it, so repeated interrupts collapse into
one `NAPI_STATE_SCHED` test and draining happens under a budget that
`ksoftirqd` can defer.

Topic 31 returns free guest pages to the host. `mem=` is a ceiling rather than
a reservation - `setup_physmem()` backs physical memory with a sparse unlinked
temp file mapped `MAP_SHARED`, so host pages appear on first touch - but
nothing gave them back, so a guest that peaked once stayed expensive for its
whole life. The topic registers a free-page reporting driver that punches the
reported range out of that file with `F_PUNCHHOLE`. A measured probe on APFS
took a fully touched 1 GiB `MAP_SHARED` mapping from 1025 MiB resident to
513 MiB by punching half of it, with the punched range reading back as zero and
re-materializing on the next write. `phys_mapping()` already returns the file
offset for a physical address, and for this file that offset is the physical
address, so no new address arithmetic is involved. The minimum order is 5
(512 KiB) rather than the `pageblock_order` default, because a 16 KiB profile
makes a pageblock 32 MiB and a busy guest rarely has a whole one free;
`page_reporting.page_reporting_order=` overrides it.

The punch goes through `darwin_uml_fcntl()` rather than calling `fcntl()`
directly. `fcntl()` is variadic, and Apple's arm64 ABI passes variadic
arguments on the stack while this translation unit passes them in registers, so
a direct call hands libc a garbage third argument; the same code returned
`EINVAL` on one build and `EFAULT` on the next. Every other `fcntl()` in
`os-Darwin` that carries a real argument already goes through the fixed-arity
bridge for this reason, and only `F_GETFD`, which takes no argument, calls
`fcntl()` directly.

Measured on a booted guest: freeing 2.15 GiB inside the guest took the
emulator's resident set from 7.89 GiB to 6.85 GiB, and it stayed there. Roughly
half the freed memory comes back; the remainder is free in blocks below the
reporting order. This bounds host residency for an idle guest; it does not
address fragmentation, since a fragmented zone has no high-order free blocks
left to report.

Topic 32 and `CONFIG_VMAP_STACK=y` close the task-stack fragmentation that
this product carried until now. `CONFIG_KERNEL_STACK_ORDER=3` still yields the
required 128 KiB stack, but as a physically contiguous order-3 allocation, and
a booted guest holds roughly 1400 of them. That pinned about 176 MiB of the
zone in unmovable blocks and fragmented the buddy allocator past what
compaction could recover: an observed boot held 147 MiB free and 2.6 GiB
`MemAvailable` with no free block at order 3 or above, 188 of 230 compaction
attempts failing, and six global OOM kills of healthy applications.

Virtually mapped stacks are backed by order-0 pages and remove the contiguity
requirement. They need address space, though, and the native hole's auxiliary
allowance was one eighth of RAM - 504 MiB on a 4 GiB guest, of which a booted
guest already uses about 350 MiB. Enabling `CONFIG_VMAP_STACK` alone therefore
exhausted vmalloc at guest second 175 and panicked in `exit_mmap` at 203.
Topic 32 tracks the allowance to RAM one-for-one between a 1 GiB floor and a
4 GiB ceiling, and gives vmalloc the whole window rather than
`min(window, RAM)`, because `end_vm` is what the kernel fault handler uses to
recognise a vmalloc address.

Together they hold. A boot on the widened window reached guest second 1469
with zero vmalloc refusals, zero OOM kills, zero RCU stalls, and no panic, and
its free-area breakdown read `0 1 1 97 20 3 0 0 0 0 0 0` - 97 free order-3
blocks where there had been none - with `compact_stall` still 0, meaning
compaction had not once been needed.

Topic 33 fixes COW-backed UBD request advancement. One block descriptor can
cross several backing-image and COW-image extents, so `do_io()` divides it into
homogeneous runs. The loop's `len` variable describes only the current run and
therefore holds the final run length after the loop; using it to advance the
request offset makes the next descriptor read or write the wrong disk region.
The first writable Android userdata mount exposes the bug because journal and
bitmap writes create a mixed backing/COW view, after which `e2fsck` can observe
an invalid journal and `vold` falls back to full-device encryption. The request
now advances by the complete `desc->length`; individual host I/O operations
continue to use their per-run lengths.

Topic 34 lets the Darwin UML loader pass a bounded host-file range as the
kernel initrd. Android uses that mechanism for the kernelless boot-image-v4
`init_boot` and recovery partitions held inside its static GPT rather than
shipping a duplicate standalone ramdisk.

Topic 35 aligns Linux scheduling with Apple performance levels without
claiming physical-core affinity. The host constructs a versioned, immutable
capacity vector from public performance-level sysctls. ACK accepts it only
when its length matches `ncpus`, its first value is 1024, every value is in
`1...1024`, and the vector is nonincreasing; malformed or duplicate input
leaves every vCPU at the existing uniform capacity. Before exposing an accepted
profile to scheduler-domain construction, the Darwin host preflights every
requested pthread QoS attribute and applies `QOS_CLASS_USER_INITIATED` to
1024-capacity carriers and `QOS_CLASS_UTILITY` to lower-capacity carriers at
creation. A preflight or CPU0 failure discards the complete profile, while a
later carrier failure rejects that CPU bring-up. XNU may still migrate any
carrier between physical cores.

The BPF syscall ABI, verifier, cgroup hooks, and generic interpreter are also
built in. This arm64 UML line deliberately has no eBPF JIT: ACK's native arm64
backend assumes CPU-capability, Spectre, register-frame, and exception-table
contracts that do not match UML. The AOSP product carries a narrowly scoped
NetBpfLoad patch and boot property for interpreter operation; other products
retain Android's normal JIT requirement. This is a bring-up accommodation, not
a claim of eBPF-JIT or VTS compliance.
