# Android Common Kernel 6.18 source policy

This line is the ACK 6.18 kernel for the AOSP 17 guest. Exact upstream
identity, the ordered series, and the final tree they produce live in
[`patches/metadata.env`](patches/metadata.env); the bridge ABI identity is
[`../common/abi/identity.env`](../common/abi/identity.env).

It is independent of both mainline page profiles. It applies the shared
[`kernel/common/patches`](../common/patches) series to its ACK base and then
its own overlay in `patches/series`: build fixes for code only ACK carries (its
PM callback tracepoint exports, its `asm/simd.h` callers, and its Rust ashmem
driver's personality binding), a touchscreen-only input presentation for
Android and recovery, and ACK ports of the balloon, drm-attach and BPF ring
buffer fixes whose context differs on this base. The ring buffer variant lays
the ring out for the vmap group that the shared series reports on this line's
4 KiB profile, so a BPF ring buffer that user space maps (Android's
connectivity and network-event consumers) stays coherent with the kernel's
view. ACK's `ringbuf.c` carries the page-size compatibility include and
macros, which is why the shared series cannot hold it. A kernel CPU mapping of
a udmabuf (`dma_buf_vmap()`) is not bracketed with the kernel-vmap hooks; no
importer in the product maps one, and user space reaches udmabuf memory
through its own mapping, which is coherent.

The shared stateful Virtio video decoder is built in for every product in this
line. Its [wire contract](../common/video/README.md) is synchronized from the
canonical header and its implementation is a topic in the shared series.
The host exposes hardware decoding through V4L2 independently of the GPU.

## Product contract

The line has one 4 KiB ARM64 configuration: the shared
[`kernel/common/config/darwin-uml.fragment`](../common/config/darwin-uml.fragment)
policy, this line's overlay [`config/android.fragment`](config/android.fragment),
and the shared 4 KiB page fragment. It is built-in-only:
`CONFIG_MODULES=n`, no `.ko` or DLKM payloads, and no 16 KiB variant. The
kernel build and the release verifier require every symbol in
[`config/required-builtins.txt`](config/required-builtins.txt) to be `y` and
every symbol in [`config/forbidden-options.txt`](config/forbidden-options.txt)
to be absent.

`CONFIG_COMPAT` and `CONFIG_KUSER_HELPERS` are required builtins: the kernel
carries the shared AArch32 task support described in
[`kernel/common`](../common/README.md), so an armeabi-v7a process runs once the
host's engine implements AArch32. With 16 KiB pages it runs only when its
segments are 16 KiB congruent, and any other fails cleanly at exec or `dlopen`.

The configuration includes the Android Binder, SELinux/audit, cgroups, storage,
DMA-BUF heaps, networking, BPF interpreter, Virtio devices, and Darwin UML
scheduling support required by the product. The overlay holds only what
differs from the mainline line:

- SELinux with audit syscall records is the exclusive LSM, with the matching
  `CONFIG_LSM` order, because Android owns the SELinux policy. Mainline uses
  Smack.
- `CHECKPOINT_RESTORE` is off, as in GKI, so that `MSEAL_SYSTEM_MAPPINGS` can
  seal userspace's system mappings.
- DWARF is zstd-compressed, as in GKI, for the ACK kernel-build-tools pahole.
- Android's own facilities: Binder and binderfs, Rust ashmem with memfd, the
  DMA-BUF heaps and udmabuf, bootconfig, the metadata-encryption stack, PF_KEY
  for the network statistics path, socket destruction for netd, further
  xtables matches and targets, the gpu_mem tracepoints, per-UID statistics,
  the Kernarium battery, and hvc for the single-port Virtio console. ACK's
  `WERROR` default is turned off.
- Settings the shared policy leaves to their Kconfig defaults, which this line
  pins: seccomp, delay accounting, utilization clamping (off), policy routing,
  XFRM interfaces and statistics, conntrack events and netlink, and Unicode.

The current rootfs uses unencrypted ext4 userdata, but the fscrypt,
inline-encryption fallback, HCTR2, and dm-default-key stack remains built in
for ordinary Android application and framework compatibility. The native arm64
eBPF JIT is not exposed because its execution contracts do not
match UML; the AOSP product uses the generic BPF interpreter, which runs
callbacks through the entry-point pool the
[mainline README](../mainline-6.18/README.md) describes. The kernel
carries BTF type information, exposed at `/sys/kernel/btf/vmlinux`, as GKI
does. The build emits DWARF 5 only as pahole's input. Frame pointers are on
for the stack walker, as the [common README](../common/README.md) describes.

Android's Ethernet IpClient acquires the SLiRP lease, default route, and DNS
configuration after init starts, so the kernel does not perform IP
autoconfiguration. Recovery independently assigns its fixed local endpoint for
the host-forwarded ADB contract; external recovery routing is not a product
requirement. This Android product has no 16 KiB profile: Android's 32-bit
libraries are 4 KiB aligned, so the kernel page size is 4 KiB, while the
64-bit userspace stays page-agnostic.

The shared series supplies the Darwin carrier, memory, device, and lifecycle
behaviour this line runs on, with the same contracts as the mainline line:
cooperative progress on persistent vCPU carriers, on-demand guest RAM with
page-cache trim, anonymous host memory for guest RAM, NAPI user networking,
batched UBD discard, initrds from host file ranges, the Kernarium battery,
system suspend through the management console, host-fault ownership, the
uaccess and fault-replay changes, data-alignment fault delivery, and the
vhost-user shared-memory aperture. [`../common/README.md`](../common/README.md)
and the [mainline README](../mainline-6.18/README.md) describe them.

Two of them matter to Android for reasons of its own. Pure touchscreen input is
specific to this line because Android and recovery must not enter pointer mode;
the mainline kernel keeps desktop mouse buttons, hover, and wheel input for
non-Android guests. The aperture is required here because the Android Vulkan
HAL is Mesa's Venus driver, which refuses to initialize without
`VIRTGPU_PARAM_HOST_VISIBLE` or guest VRAM. The drm-attach port in the overlay
exists because Android's gralloc mapper imports a buffer and then locks it for
reading, so a buffer imported before its context must be attached when the
context is created.

## Build and artifact boundary

The standalone ACK replay and build are owned by
[`../../android/lineage-24`](../../android/lineage-24/README.md) and run on the Linux
builder. That producer verifies the generated series, configuration, ELF, and
exact bridge imports. As in Kleaf, its host tools build against the pinned
kernel-build-tools musl sysroot and libraries, including the `libelf` that
`resolve_btfids` needs, and BTF comes from that package's `pahole`.

`android/lineage-24/tools/publish-kernel-artifact.py` publishes an already-built
clean ACK result into the common compact artifact format. It strips DWARF from
the published `vmlinux`. That drops only the `STT_FILE` symbols and
debug-section locals, and keeps `.BTF`, every function and data symbol,
`.dynsym`, and `System.map`.
[`artifacts/accepted-4k.env`](artifacts/accepted-4k.env) selects the reviewed
local bundle below `.build/kernel/artifacts`.

Kernel publication is independent of AOSP/rootfs publication. Advancing the
accepted ACK artifact must not rebuild or rename the rootfs release, recreate a
`.kern` bundle, or invalidate its persistent disk. Apple consumers validate the
bridge ABI, 4 KiB page profile, Android family, and `6.18` series at build
time; patch releases and commit identities are provenance, not runtime gates.

## Patch maintenance

Shared topics are edited on the mainline replay. Android-only topics are
edited on the ACK topic branch named in the metadata. After either, rebase the
overlay onto the current shared series and export it with its final tree:

```sh
python3 kernel/common/tooling/kernel_series.py refresh \
  --metadata kernel/android-6.18/patches/metadata.env --source ACK_CHECKOUT
python3 kernel/common/tooling/kernel_series.py regenerate \
  --metadata kernel/android-6.18/patches/metadata.env --source ACK_CHECKOUT
```

Every shared patch applies to this base with plain `git am`. A shared topic
that cannot becomes a variant in each line's overlay. Never edit patch files,
`series.tsv`, or the final tree by hand. A changed final tree requires a fresh
replay, build, artifact verification, and the affected Android runtime
qualification before acceptance.
