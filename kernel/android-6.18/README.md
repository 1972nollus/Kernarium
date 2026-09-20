# Android Common Kernel 6.18 source policy

This line is the ACK 6.18 kernel for the AOSP 17 guest. Exact upstream and
final-tree identities, ABI, patch count, and delta limits live in
[`patches/metadata.env`](patches/metadata.env); ordered generated patch identity
lives in [`patches/series/manifest.tsv`](patches/series/manifest.tsv).

It is independent of both mainline page profiles. Mainline patch bytes are not
this line's source of truth, even when a topic implements the same Darwin
contract.

The shared stateful Virtio video decoder is built in for every product in this
line. Its [wire contract](../common/video/README.md) is synchronized from the
canonical header and its implementation is maintained as a local media topic.
The host exposes hardware decoding through V4L2 independently of the GPU.

## Product contract

The line has one 16 KiB ARM64 configuration in
[`config/android.fragment`](config/android.fragment). It is built-in-only:
`CONFIG_MODULES=n`, no `.ko` or DLKM payloads, and no 4 KiB variant.

The configuration includes the Android Binder, SELinux/audit, cgroups, storage,
DMA-BUF heaps, networking, BPF interpreter, Virtio devices, and Darwin UML
scheduling support required by the product. The current rootfs uses unencrypted
ext4 userdata, but the fscrypt, inline-encryption fallback, HCTR2, and
dm-default-key stack remains built in for ordinary Android application and
framework compatibility. The
native arm64 eBPF JIT is not exposed because its execution contracts do not
match UML; the AOSP product uses the generic BPF interpreter.

Android's Ethernet IpClient acquires the SLiRP lease, default route, and DNS
configuration after init starts, so the kernel does not perform IP
autoconfiguration. Recovery independently assigns its fixed local endpoint for
the host-forwarded ADB contract; external recovery routing is not a product
requirement. This Android product has no 4 KiB profile.

The local series provides the durable Android-specific behavior:

- cooperative timer, reschedule, IPI, irq-work, and RCU progress on persistent
  Darwin vCPU carriers;
- direct touchscreen classification compatible with Android input;
- NAPI receive processing for sustained user networking;
- free-page reporting for file-backed guest RAM where supported;
- virtually mapped 128 KiB task stacks and the common RAM-scaled auxiliary
  address window;
- 128 MiB batched discard for directly attached UBD images without modifying
  the upstream UML COW implementation;
- validated initramfs ranges inside the static GPT;
- a Kernarium Virtio battery power-supply driver for host percentage and
  charging-state passthrough;
- system suspend and wake through the management console, including secondary
  vCPU hotplug across an Apple host background interval; and
- anonymous host VM for Android physical memory, with page reporting disabled
  when no file extent exists.

Pure touchscreen input is specific to this Android line because Android and
recovery must not enter pointer mode. The mainline kernel continues to expose
desktop mouse buttons, hover, and wheel input for non-Android guests.

Physical iOS wires the complete anonymous RAM root before guest start. macOS
and Simulator retain demand allocation. Normal image descriptors remain
file-backed.

A pagefault-disabled uaccess never reports mmap-lock contention as a fault.
`mmap_read_trylock()` refuses to barge past a queued writer while the faulting
retry of the same task still steals the lock, so answering the try with a fault
made `futex_wait_setup()` alternate a failing atomic read with a succeeding
faulting one and never yield the CPU that writer needs. The contended atomic
walk runs under RCU, which `MMU_GATHER_RCU_TABLE_FREE` keeps safe, and the
page-table lock still samples the entry. This is the same source change the
mainline line carries, and it is independent of the cooperative-checkpoint safe
point this line already has.

A synchronous host fault is interpreted only by its owner. Signal dispositions
are process-wide, so the emulator's device and renderer threads share UML's
`SIGSEGV` and `SIGBUS` handlers even though they own no Linux task; such a fault
now leaves the guest path, is reported with its real host thread and literal
addresses, and terminates under the default host disposition. This too is the
same source change the mainline line carries.

A guest data-alignment exception is not a translation fault and cannot be
repaired by mapping a page. It crosses the versioned bridge as its own event
and reaches the faulting task as `SIGBUS` with `BUS_ADRALN` at the effective
data address, so a guest handler can repair or emulate the access before
`sigreturn`. This is again the same source change the mainline line carries.

An idle virtual CPU never blocks while one of its IPI vectors is pending: the
idle wait reads the pending mask after publishing the idle state and polls
instead of blocking, because a bit that is already set has no wake edge behind
it and a CPU that blocked on one would absorb every later remote vector.
`schedule()` is a cooperative polling boundary when its caller stays runnable,
so a task that yields in a retry loop, as `tty_open()` does during the last
close of a terminal, no longer stops the timer and the queued wakeups of its
CPU. Both are the same source changes the mainline line carries.

Threads of one process replay their mappings to the host side by side: a user
`mm` has no replay lock, the host's invalidation epoch rejects a batch that
sampled a PTE before Linux changed it, a replay is published in bounded batches
with a scheduling checkpoint between them, user ranges are exact pages, and a
fault replays ahead of Linux only when the PTE already grants the access. This
is the same source change the mainline line carries; its
[contract](../mainline-6.18/README.md) is described there.

The standardized vhost-user shared-memory-region transport the mainline line
adds for host-visible Virtio-GPU blobs is carried here as well. The Android
product's Vulkan HAL is Mesa's Venus driver, which refuses to initialize
without `VIRTGPU_PARAM_HOST_VISIBLE` or guest VRAM, so this line negotiates
`VHOST_USER_PROTOCOL_F_SHMEM_MAP` and reserves the same guarded guest-physical
aperture under the same mapping-capacity policy as the mainline topic.

## Build and artifact boundary

The standalone ACK replay and build are owned by
[`../../android/aosp-17`](../../android/aosp-17/README.md) and run on the Linux
builder. That producer verifies the generated series, configuration, ELF, and
exact bridge imports.

`android/aosp-17/tools/publish-kernel-artifact.py` publishes an already-built
clean ACK result into the common compact artifact format.
[`artifacts/accepted-16k.env`](artifacts/accepted-16k.env) selects the reviewed
local bundle below `.build/kernel/artifacts`.

Kernel publication is independent of AOSP/rootfs publication. Advancing the
accepted ACK artifact must not rebuild or rename the rootfs release, recreate a
`.kern` bundle, or invalidate its persistent disk. Apple consumers validate the
bridge ABI, 16 KiB page profile, Android family, and `6.18` series at build
time; patch releases and commit identities are provenance, not runtime gates.

## Patch maintenance

Patch files are generated from the clean ACK topic history. Change commits in
Git and run the regenerator documented by the AOSP producer; never edit patch
bytes, metadata, or manifest rows independently. A changed final tree requires
a fresh replay, build, artifact verification, and the affected Android runtime
qualification before acceptance.
