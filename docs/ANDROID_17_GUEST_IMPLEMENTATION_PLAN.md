# Android 17 guest implementation plan

## Status and objective

This document defines the implementation plan for a native ARM64 Android 17
guest running on the Darwin User-Mode Linux emulator with a 16 KiB Android
Common Kernel (ACK) 6.18 kernel.

Implementation started on 2026-08-20. **Android 17 now boots.** The accepted
release uses a 34-topic built-in-only 16 KiB ACK 6.18 Darwin UML kernel, a
fresh `aosp_kernarium_darwin` device overlay, a fixed ten-partition UBD image
with partition-backed normal and stock AOSP recovery ramdisks, pinned external
LineageOS Jelly source, an import-only Apple consumer, separate macOS
executable, Virtio console, and source-built Mesa Virgl userspace.
`20260822-28ce51ba-mm21` was the first
release to reach `sys.boot_completed=1`, in a ten-vCPU gadgets boot with
SELinux enforcing and hardware Virgl SurfaceFlinger. Six further accepted
releases added the touchscreen pointer and boot animation, NAPI receive, free
guest page reporting, virtually mapped task stacks, and correct mixed-COW UBD
request advancement, then removed a circular warm-boot theme wait. The accepted
release is `android17-ack6.18-16k-20260825-87eaee1e-artpreopt1`.

The guest also runs on iOS. Kernarium embeds a third signed runtime framework
carrying the ACK kernel and boots an Android guest from a `.kern` bundle in its
Documents, with a writable disk that persists across application launches. The
exact current state, its measurements, and the next actions are in the active
implementation handoff.

The Android product is additive. It does not replace, rename, or silently
change either of the current mainline Linux products. All project
changes remain in this repository and its private build workspaces. No phase
of this plan proposes, submits, pushes, or otherwise sends a change to AOSP,
ACK, or upstream Linux.

The initial source pins, verified on 2026-08-20, are:

- AOSP manifest tag `android-17.0.0_r1`, manifest commit
  `5bc9a7ce1cd78dd53613bbfd0ebf506e1e4adb0f`;
- ACK tag `android17-6.18-2026-06_r35`, commit
  `7942b4b6552dc2c5932ed2bd9a6f3981f6a04333`, tree
  `24e0546bc6c78e8e829f0a3657c8ba7c68faabda`; and
- Cuttlefish's 64-bit-only page-size-agnostic ARM64 product as the Android
  device and HAL donor, not as a directly bootable artifact.

These are exact starting identities, not floating branches. A future update
is a reviewed metadata change that records a new full commit and tree.

## Active implementation handoff (2026-08-25)

This section is the current entry point and supersedes the 2026-08-24 handoff
below wherever the two differ.

The fresh-only overlay is implemented under `android/aosp-17/overlay` and is
exact-synced into AOSP as `device/kernarium/darwin`. It keeps ThemePicker and
LatinIME, forces the software keyboard visible even when Android sees a host
keyboard, disables the first-boot screen lock and practical idle timeout, and
removes obsolete launcher applications and packages tied to hardware this UML
machine does not expose. Framework APIs, compatibility providers, WebView,
PackageInstaller, Virtio network and hardware Virgl remain.

Jelly is external source, not vendored code. `prepare-sources.sh` fetches
LineageOS branch `lineage-23.2` at commit
`767833067da7bbbb639782bbddec33f602215c3d` and replays exactly two generated
topics: SDK-37 build compatibility and geolocation disabled by default. The
resulting tree is `ce63d63703fbfd2e616e94513ea1e5c63ccae4ef` and the series
manifest SHA-256 is
`bb647d2db690f7bc3af85d0b8e243436fa0b59c1bb5c1a780e8861358bd48115`.
Jelly's upstream optional camera, microphone and location declarations remain
unchanged; no hardware-omission patch is retained.

The accepted release is
`android17-ack6.18-16k-20260825-87eaee1e-artpreopt1`, produced from clean outer
revision `87eaee1e433ce8db423cf48819084755ebd5c014`. AOSP projects-manifest
SHA-256 is
`c19af30b12aadb52c941c8555e16aa26d01330fa028c796fef946117e233ab50`.
ACK has 34 topics, final tree
`621a0ab42d8990aa0960bf5d341d89b8f1042723`, patch-manifest SHA-256
`8bc9771b8b7776adfae244f098ca9fabb4bbab8a914e0c5aa522eface71b8594`
and release `6.18.21-ga64f141f6266`; it remains built-in-only, 16 KiB and ABI
v8 / `0x3871ad612337ea76`.

The accepted release has now completed matched runtime qualification after
host fix `051260e3`. With gadgets, 4 vCPUs, 2 GiB, 16 KiB, normal boot,
point-mode 160 DPI, hardware Virgl, a fresh COW and then the exact persisted
COW, macOS reached `sys.boot_completed=1` in 72.858 seconds first boot and
54.594 seconds second boot. The iOS 26.5 iPad mini Simulator measured 77.500
and 38.940 seconds; the physical iPad mini 6 on iOS 18.5 measured 384.418 and
182.400 seconds. All six runs completed without timeout or forced termination,
and exact scans found no Darwin signal, panic, oops, RCU stall, lockup, OOM,
EROFS worker, userdata-recognition fallback, Virtio-GPU response error, or ART
crash.

The physical device remains the slowest host: its first-boot `artd` interval
was 150.368 seconds versus 40.261 on macOS and 49.097 on Simulator. Its warm
boot skipped `artd`, but codec-list generation took 56.252 seconds and the
codec-exit-to-completion tail took 32.895 seconds. The macOS first-boot console
shows 22 unique runtime dex2oat inputs, every one below `/apex`, and no
ordinary-partition runtime compilation. Full phase tables and preceding-release
comparisons are in
[`android-17-artpreopt-boot-timing-20260825.md`](android-17-artpreopt-boot-timing-20260825.md).

A subsequent fresh-COW cold-boot CPU profile used Release applications and the
same guest compute, memory, page-size, boot, density, and Virgl policy on the
physical iPad and Simulator. The iPad recorded 573.213 process CPU-s through
the profile boundary: real gadget bodies consumed 356.384 CPU-s (62.17%),
memory/MMU runtime 100.187 CPU-s (17.48%), and translation plus software
translation-cache lookup and maintenance 37.035 CPU-s (6.46%). Its I/O/device
paths consumed only 5.775 CPU-s (1.01%) on CPU. Simulator's fallback sampling
attribution is approximate, but again identifies real gadget execution as the
largest owner at 51.13%; its translation/cache subtotal is 14.33%, driven by
late program-cache maintenance rather than translation itself. From `artd`
start through boot completion accounts for 72.24% of iPad CPU and 72.71% of
Simulator CPU. Full whole-boot and per-stage tables, category definitions,
confidence gates, and evidence paths are in
[`android-17-cold-boot-cpu-profile-20260825.md`](android-17-cold-boot-cpu-profile-20260825.md).

The Simulator crash that invalidated the provisional timing was a host gadget
cache defect, not an Android image or ACK defect. A cache slot handed to a
different address space retained the prior owner's memoised `LinkRecord`, so a
translation could be published into the wrong address space and outlive the
retired owner's storage. `store()` now clears that memo and the deterministic
cross-address-space regression covers the failure, including under
ASan/UBSan. The separate older macOS signal at address `0x3` has a different
signature and remains an isolated open observation.

The schema-2 product has one 17,179,869,184-byte GPT with ten partitions. Its
normal `init_boot` ramdisk and stock AOSP `recovery` ramdisk are boot-image-v4
payloads selected as bounded byte ranges; writable `misc` carries the standard
`boot-recovery` request. The release and `.kern` guest bundle contain no
standalone ramdisk. Explicit normal and recovery modes are implemented on
macOS and Kernarium. OrangeFox remains intentionally deferred.

The first built overlay exposed one invalid inherited dependency: the Context
Hub APEX advertised `android.hardware.context_hub`, then
`ContextHubSystemService` attempted to use the deliberately absent Wi-Fi
framework service and repeatedly restarted `system_server`. The accepted
product excludes `com.android.hardware.contexthub`; the target-files verifier
and builder fixture reject its APEX, VINTF/permission payload and feature name.

The earlier overlay acceptance's corrected warm normal boot reached
`sys.boot_completed=1` at guest second 100.21, kept one `system_server` through
a greater-than-30-second dwell, passed the settings, package,
browser-resolution and feature checks, and powered down cleanly. Explicit
stock recovery started `/system/bin/recovery`, remained stable through its
dwell and powered down cleanly. One fresh run reached boot completion at
587.70 and then panicked at 594.44 on a kernel-mode Darwin signal 11. It did
not reproduce on the exact persisted state, is excluded from passing evidence,
has a different signature from the fixed gadget-cache crash, and remains an
isolated open stability observation.

Those earlier overlay and ext4-APEX descriptors passed producer,
copied-release and installed verification, all focused Android policy/series
tests, the macOS Android target, both Android iOS runtime slices and final
XCFramework packaging. The macOS Android product, both Android iOS runtime
slices, final XCFramework packaging, and the signed physical-iOS application
were rebuilt against the ext4-APEX release before its historical fresh runtime
qualification below.

A subsequent physical-iPad cold boot used 4 vCPUs, 2 GiB, the gadgets engine,
and a fresh custom-overlay bundle. It reached the normal Android first stage,
but at wall second 169 an `erofs_worker` running
`z_erofs_decompressqueue_work` triggered an RCU stall on one vCPU. The run did
not reach boot completion and is not timing evidence. The accepted ext4 image switches
the immutable `system` and `product` payloads from EROFS to ext4, matching the
already-ext4 `system_ext`, `vendor`, and `odm` partitions; all five remain
mounted read-only. This is an image and device-overlay change only. The ACK
kernel and its EROFS driver are intentionally unchanged. Its producer,
target-files, VINTF, GPT, copied-release and kernel-identity checks pass; the
first retry of that image then identified a second EROFS owner: all five GPT
partitions mounted as ext4 by wall second 1.10, but inherited EROFS APEX loop
images mounted at wall seconds 2.44--2.74 and reproduced an `erofs_worker`
stall at wall second 46.70. That run was stopped at 103.68 seconds and is also
not timing evidence.

The accepted replacement sets `PRODUCT_DEFAULT_APEX_PAYLOAD_TYPE := ext4`
after the Cuttlefish product inheritance and carries generated AOSP route
`p0008` for `platform/packages/modules/Virtualization`, changing the one
module-level `com.android.virt` EROFS override to ext4. The target-files
verifier checks every nested `apex_payload.img` magic and rejects EROFS,
compressed APEXes, and any non-ext4 payload. No ACK source, kernel
configuration, or EROFS driver change is part of this fix. Its incremental
AOSP build completed in 8 minutes 14 seconds; producer verification inspected
4,608 target-files entries, 1,147 target executables and all 69 nested APEX
payloads as ext4. The 17,179,869,184-byte GPT compressed to 704,869,601 bytes
and passed independent workstation verification.

The fresh physical-iPad retry passed. A wired iPad mini 6 running iOS 18.5
booted the new bundle with no pre-existing COW, the gadgets engine, 4 vCPUs,
2 GiB of RAM, 16 KiB guest pages, normal boot, and hardware Virgl. Host thermal
state was nominal. The five immutable GPT filesystems mounted read-only ext4 by
wall second 0.951; APEX bootstrap scanned 67 active packages and mounted its
payloads through ext4 loop devices, activating them by wall second 2.640.
Userdata followed the normal ext4 encryption path with no unrecognized
filesystem fallback. First-boot `artd` ran for 316.137 seconds, codec-list
generation ran for 30.170 seconds, boot animation exited at wall second
504.123, and init reached `sys.boot_completed=1` at wall second 525.731. The
guest remained stable beyond the 30-second dwell and was then terminated by
the host. Exact scans found no EROFS use, `erofs_worker`, RCU stall, vold
fallback, Virtio-GPU `0x1204`/`0x1205`, OOM, oops, panic, or Darwin-signal
signature. Evidence is retained under
`.build/android/measurements/ipad-ext4-apex-retry-20260825`.

The preceding pre-ART matched Apple-host baseline completed at outer revision
`c529aeaa1a520ba0c0b870e170e72c8967b31a98`. The standalone macOS product and
Kernarium's iOS 26.5 iPad mini Simulator product used that same accepted
release, gadgets, 4 vCPUs, 2 GiB, 16 KiB, normal boot, point-mode 160 DPI,
hardware Virgl, a fresh COW followed by its exact persisted state, and the
same host-launch-to-init completion boundary. macOS measured 788.926 seconds
cold and 171.793 seconds warm. Simulator measured 109.657 and 41.186 seconds.
The accepted Simulator pair acknowledged inactive and background storage
settlement before each termination; both macOS runs synced and powered down
through the guest console. The Simulator surface was 744 x 1081 in its native
portrait orientation versus the macOS/physical-iPad 1133 x 695 landscape
surface, but its pixel area was only 2.14% larger.

The early phases do not explain the difference: every run mounted the five GPT
payload filesystems by wall second 1.352, activated all 67 ext4 APEX payloads
by 3.198, and the two warm runs completed codec-list generation at 28.597 and
28.998 seconds. macOS then took 143.196 seconds from codec exit to warm boot
completion versus 12.188 on Simulator. Cold Simulator `artd` ran once and
exited cleanly after a 74.428-second window. The macOS cold observation is
flagged rather than presented as a clean baseline: its first `artd` aborted at
wall second 443.417 with the tombstone message
`getAndExecuteCommand(fd=4) returned unexpected error -14, aborting`, Android
restarted it, and the replacement exited normally at 513.657. That is an
`EFAULT` Binder-command failure, not an OOM, filesystem stall or timeout.

Exact scans of the four accepted logs found no EROFS worker, RCU stall, vold
fallback, Virtio-GPU `0x1204`/`0x1205`, OOM, oops, panic or Darwin-signal
signature. The sole matched failure was the recovered macOS cold `artd`
`SIGABRT`. Full phase tables, limits and retained evidence paths are in
[`android-17-boot-timing-20260825.md`](android-17-boot-timing-20260825.md).

### Build-time ART implementation and runtime gate

The first-boot ART cost was traced to a product-policy mismatch. Dexpreopt was
enabled, but the inherited mainline board set
`BOARD_USES_SYSTEM_OTHER_ODEX := true` while this product deliberately disables
the `system_other` image. Soong generated ordinary application artifacts for
an image that was never packaged.

The project-owned BoardConfig now clears that inherited route. Build guards
verify the resolved and generated dexpreopt policy, and target-files checks
require adjacent ARM64 artifacts for the 26 ordinary apps from the captured
first-boot workload. The accepted producer release was built incrementally
from the preserved Ninja output. Pinned AOSP deliberately does not dexpreopt
ordinary APEX-contained apps. The accepted macOS first-boot capture contains
22 unique runtime inputs, all within that APEX boundary, while the warm capture
starts neither `artd` nor dex2oat. The earlier ten-input observation was a
lower bound rather than a complete residual inventory. The evidence, source
trace, implementation boundary, rejected shortcuts, and completed matched
acceptance measurement are in
[`android-17-art-build-time-investigation-20260825.md`](android-17-art-build-time-investigation-20260825.md).

## Historical implementation handoff (2026-08-24)

This section was the entry point on 2026-08-24 and is retained as dated
history. The 2026-08-25 handoff above supersedes it wherever the two differ.
The architectural decisions and phase definitions later in this document
remain in force except where a superseding decision is recorded against them.

### Current device-overlay candidate

The new image is deliberately fresh-only; no migration or legacy product alias
is part of this stage. `android/aosp-17/overlay` is synchronized exactly to
`device/kernarium/darwin` and owns the `aosp_kernarium_darwin` product. It keeps
ThemePicker and LatinIME, forces the software keyboard visible even with a host
keyboard, disables the first-boot lock screen and practical idle timeout, and
removes obsolete launcher applications plus machine services for Bluetooth,
Wi-Fi, telephony, GNSS, NFC, camera, sensors, biometrics, UWB, Thread, and
secure elements. Framework APIs, providers, WebView, PackageInstaller, and the
real Virtio-net and Virtio-GPU paths remain.

LineageOS Jelly is fetched only by `prepare-sources.sh` from the exact external
commit recorded under `android/aosp-17/third_party/jelly`. This repository
stores only provenance and a two-topic generated patch series: SDK-37 build
compatibility and geolocation-disabled-by-default policy. Jelly's upstream
camera, microphone, and location declarations are unchanged.

The final GPT now carries `init_boot`, writable `misc`, and stock AOSP
`recovery` in addition to the seven filesystem partitions. Both boot
partitions are kernelless boot-image-v4 containers; the ACK ELF remains the
separately signed kernel. Automatic launch reads the standard
`boot-recovery` command from the COW-backed `misc` partition, while the macOS
and Kernarium launchers also expose explicit normal and recovery modes. The
release schema is version 2 and carries no standalone ramdisk. OrangeFox was
investigated but is intentionally deferred. Builder publication and boot
qualification of this candidate are the remaining acceptance steps.

### Durable repository and builder state

- The accepted release is
  `android17-ack6.18-16k-20260824-c3febf12-theme1`, built from outer revision
  `c3febf12` with ACK final tree
  `05c892f8175ca302ca869b01efc76f09ad14fa2c`, common ABI version 8 and
  fingerprint `0x3871ad612337ea76`, `CONFIG_MODULES=n`, and the 16 KiB-only
  configuration. The ACK 6.18 series remains 33 topics. AOSP now has seven
  routed local series; route `p0007` owns the framework theme-gate fix.
- The host Virtio-GPU accepts the Linux driver's device-selected context
  request and the gadget cache bulk-owns allocator storage for each bounded
  resident address space. These are host changes; they do not alter the
  accepted Android release bundle.
- The builder-local checkouts, the relative `OUT_DIR=out-uml-android17`, and the
  no-tracked-endpoint policy are unchanged from the 2026-08-22 handoff.

### Matched macOS and Simulator boot timing

The current accepted release was measured from host launch to Android init's
first `sys.boot_completed=1` action on macOS and the iOS 26.5 iPhone 17 Pro Max
Simulator. Both products used gadgets, 4 GiB, ten vCPUs, a 16 KiB ACK kernel,
440 x 860 at 160 DPI, and hardware Virgl through ANGLE/Metal. Boot 1 began
without a COW; boot 2 reopened the disk persisted by boot 1.

macOS completed at 786.169 s cold and 84.795 s warm. Simulator completed at
130.991 s cold and 42.780 s warm. Every run reached late mount by 7.866 s,
Zygote by 13.600 s, SurfaceFlinger by 15.386 s, and boot animation by 16.753 s.
The time is therefore after initial graphics startup: boot-animation-to-
completion was 771.291/74.103 s on macOS and 114.237/30.182 s on Simulator.

First-boot ART/package verification is the dominant cost. macOS `artd` lived
for 579.415 s; its captured first-boot `verify` dex2oat work used ten threads
per job, with a longest observed job of 117.997 s wall/443.086 s guest CPU.
Simulator's corresponding quiet ART/package interval was about 66 seconds.
The next costs are media codec-list generation, package overlay/idmap and
theme/user-unlock work. Neither second boot started `artd`.

The macOS/Simulator ratios in this single matched campaign are 6.00x cold and
1.98x warm, but they are not a general platform claim. Earlier accepted
macOS evidence for the same release measured 130.287/44.977 s. The current
long cold behavior did reproduce twice (`artd` 555.631 and 579.415 s), and an
extra foreground-priority warm control was not faster. The phase is identified
but the cross-campaign macOS variance is not explained. The full method,
milestones, interval table, limitations and retained paths are in
[`android-17-boot-timing-20260824.md`](android-17-boot-timing-20260824.md).

### Android completes boot

`26e208db` accepted `20260822-28ce51ba-mm21`, the first Android 17 UML release
to reach `sys.boot_completed`. Verified in a live ten-vCPU gadgets boot with
SELinux enforcing and a host `stdin` that cannot hang up: `sys.boot_completed=1`
at guest 953 s and still set at 1134 s, `getenforce` Enforcing, one zygote
start, zero watchdog dumps, zero abends, zero panics, RCU stalls, lockups, or
oopses, zero `ssl` interrupts, and 89k-96k timer ticks on every CPU `0-9`. That
even tick distribution is the direct contrast with the vCPU-0 serial storm that
invalidated every measurement taken before it. `dumpsys SurfaceFlinger` reports
`GLES: Mesa, virgl, OpenGL ES 3.0 Mesa 26.1.0-devel` over `hwc3-drm`.

Thirteen fixes stand behind that release, from the restored three-patch Mesa
route and the two graphics SELinux labels through the omitted RIL, NFC, and
factory reset protection storage, `CONFIG_NET_KEY`, `CONFIG_TRACE_GPU_MEM`, the
`DC CVAC` decode gap in all three CPU engines, the scaled guest timeouts, and
the libmeminfo `gpu_mem` guard. Five accepted releases follow it, each adding
one capability and each boot-verified before acceptance:

| release | topics | what it added |
| --- | --- | --- |
| `20260822-28ce51ba-mm21` | 28 | first `sys.boot_completed=1`, enforcing, Mesa/virgl SurfaceFlinger |
| `20260823-016033c1-input3` | 29 | touchscreen pointer (`Darwin UML VNC Tablet`), boot animation |
| `20260823-bb3cf26e-napi1` | 30 | NAPI receive; 512 MiB `adb push` at 14.3 MB/s with the hash matched |
| `20260823-4e793df3-pr2` | 31 | free-page reporting; 2.15 GiB freed, host RSS 7.89 to 6.85 GiB |
| `20260823-5292a9ce-vmap` | 32 | `CONFIG_VMAP_STACK=y` on the widened vmalloc window |
| `20260824-d6a6f91d-ubdcow1` | 33 | correct descriptor advancement across mixed backing/COW UBD runs |
| `20260824-c3febf12-theme1` | 33 | AOSP route `p0007` releases a missing-wallpaper theme gate at boot phase 600 |

The vmapped-stack release closes the order-3 task-stack fragmentation that had
been costing background applications: 97 free order-3 blocks, `compact_stall` 0,
zero OOM kills, and 350 MiB of vmalloc in use against a 4 GiB window at guest
second 1469. The same configuration on the former one-eighth auxiliary window
exhausted vmalloc at second 175 and panicked in `exit_mmap` at 203, which is why
the window change and the stack change had to land together.

### Fresh-userdata and device-selected Virgl contexts are fixed

The two warnings investigated after the first complete boots were real defects,
not harmless first-boot noise.

The Linux Virtio-GPU driver creates a context with the capset bits in
`context_init` set to zero when it wants the device to select the context type.
The host accepted only explicit Virgl capset IDs 1 and 2, so it rejected the
valid `CTX_CREATE` (`0x200`) with `0x1205`; the subsequent
`CTX_ATTACH_RESOURCE` commands (`0x202`) then reported the observed invalid
context response `0x1204`. The host now accepts zero and presents it to
virglrenderer as Virgl2, while preserving explicit IDs 1 and 2 and continuing
to reject reserved values. The GPU ownership test covers all four cases. A
fresh accepted-release boot emits neither `0x1205` nor `0x1204`.

The userdata warning was caused by ACK topic 33's UBD defect. A COW-backed block
descriptor can be divided into several homogeneous backing-image and COW-image
runs. `do_io()` correctly issued each run, but advanced the request by the
length of only the last run instead of by the complete descriptor. Once
`mke2fs` wrote enough COW extents, the next descriptor reached the wrong disk
offset; `e2fsck` then found an invalid journal and `vold` could not recognise
the filesystem, so it fell back to full-device encryption. Topic 33 advances by
`desc->length` and leaves the per-run I/O lengths unchanged.

This fallback did materially delay early boot, but it was not the main reason a
fresh emulated Android boot takes minutes. In a representative broken run the
warning appeared at guest second 4.87 and the encrypted filesystem mounted at
28.78, about 24 seconds later. With topic 33, `e2fsck` reports `data: clean`
both before and after `encryptFstab`, and `mount_all --late` completes in 925 ms
at guest second 5.97. The remaining first-boot time is dominated by Android's
normal package/ART preparation under emulation.

### Ten-vCPU gadget boot is stable again

The first ten-vCPU candidate containing the GPU and UBD fixes exposed a
separate host-side stall at guest seconds 234.334 and 449.900. A host sample
held `uml-vcpu-3-carrier` and `uml-vcpu-8-carrier` for every captured sample in
`ProgramCache::prepare -> retire -> Generation::~Generation -> free`. Android
process churn retires address spaces frequently, and ten private gadget caches
were returning thousands of individually allocated link/map nodes through the
system allocator at the same time. That starved guest vCPU carriers long enough
for RCU to report stalls even though the kernel's cooperative timer fixes were
present.

Each bounded gadget-cache `Generation` now owns an unsynchronised PMR pool for
its link maps, live lists, and slot list. Retirement still clears the same
address space and preserves stable node addresses, the eight-generation bound,
and exact survivor behavior, but returns allocator storage upstream in a small
number of bulk releases instead of serialising individual system frees.

A fresh-COW run of the preceding UBD-fix release with 4 GiB, ten online vCPUs
(`0-9`), and the gadgets engine set `sys.boot_completed=1` at guest second
307.5; the console observed it set at uptime 314.8 with boot animation stopped,
SELinux enforcing, and `nproc=10`. The independent `hvc0` Virtio console stayed
responsive for the Android shell and `logcat`, while `ttyS0` retained kernel and
init output. The guest powered down normally at second 366.290. Its complete
`ttyS0` capture contains no RCU stall, invalid-journal or unrecognised-userdata
warning, Virtio-GPU `0x1204`/`0x1205`, oops, or panic.

### Warm boot no longer waits for the hardware timeout

The long warm boot was not emulator slowness and reducing its timeout would
have hidden the defect. Before route `p0007`, WindowManager enabled the screen
at guest second 31.827 but did not receive boot-animation completion until
1531.950, a 1500.123-second interval. That is the 30-second WindowManager
emergency timeout multiplied by `androidboot.hw_timeout_multiplier=50`.

`ThemeInitializationObserver` registered a boot-screen blocker and called
`setThemeReady(false)` while it waited for initial wallpaper colors. On this
headless product no color callback arrived. Its fallback ran only at
`PHASE_BOOT_COMPLETED`, but WindowManager withheld boot completion until theme
readiness, so the fallback was behind the gate it was supposed to release.
Route `p0007` moves only that fallback to
`PHASE_THIRD_PARTY_APPS_CAN_START` (phase 600). The ordinary callback remains
unchanged, and the focused `FrameworksServicesTests` regression proves phase
550 leaves initialization pending while phase 600 forces it.

The accepted candidate was produced by the standalone x86_64 Linux lane using
the existing Ninja configuration. Its target-files archive has 4,548 entries
and 1,162 target executables; its verified raw initramfs is 4,374,784 bytes and
its seven-partition sparse GPT disk is 17,179,869,184 bytes, compressed to
826,612,380 bytes. The ACK ELF and 33-topic final tree are byte-for-byte
unchanged from the prior release.

On macOS with 4 GiB, ten vCPUs and gadgets, a fresh COW reached
`sys.boot_completed=1` at wall second 130.287, stayed live for another 30
seconds, and powered down normally at 164.436. A natural warm boot logged the
wallpaper wait at guest second 25.516, forced initialization at 28.279,
notified WindowManager at 28.474, enabled the screen at 37.048, and completed
the boot animation at 42.296. The host observed boot completion at wall second
44.977, retained the guest for another 30 seconds, and powered down normally at
78.615. A second warm run independently completed at 44.871 seconds. The three
successful logs contain no RCU stall, userdata/journal fallback, Virtio-GPU
`0x1204`/`0x1205`, oops, or panic.

One earlier fresh attempt reached boot completion at 125.773 seconds and then
panicked four seconds later on a kernel-mode Darwin signal 10. That signature
does not occur in any older retained Android measurement and did not reproduce
in the second fresh boot or either warm boot. It is retained as an isolated
observation, not counted as a successful stability run and not conflated with
the fixed theme wait.

### The guest runs in the iOS application

Kernarium embeds a third versioned runtime framework,
`KernariumRuntimeAndroid_<ack-release>_16K`, built by the
`kernarium-ios-{device,simulator}-android` presets with
`UML_KERNEL_PRODUCT=android-17`. The ACK ELF passes the signed carrier check
unchanged and rides in the same aligned `__TEXT,__uml_elf` section as the Linux
runtimes, with the release manifest in `__DATA,__uml_android`. The application
embeds all three runtime XCFrameworks, links none at launch, and loads exactly
one for the process lifetime.

A guest bundle is a `.kern` directory in Documents holding one guest's complete
payload and its writable disk. An Android bundle carries
`release-manifest.json`, `boot/kernel-command-line.txt`, and exactly one
`guest/*.img`; its GPT owns kernelless `init_boot`, writable `misc`, and stock
AOSP `recovery` partitions. UBD creates `userdata.cow` beside them on first boot
and reopens it afterwards, so guest state accumulates inside the bundle rather
than in disposable storage. A bundle is recognised from its payload rather than
its name, so a half-copied directory is never offered for selection, and
selecting one fixes the page-size picker at the only profile its kernel exists
in. `Kernarium/scripts/make-guest-bundle.py` assembles a bundle from the
accepted release, and `Kernarium/scripts/run-ios-simulator.sh --guest-bundle
NAME` installs one into a booted Simulator.

The latest matched measurement on the iPhone 17 Pro Max Simulator used 4 GiB,
ten vCPUs and the gadget engine. A fresh bundle reached
`sys.boot_completed=1` at 130.991 s and the same persisted bundle reached it at
42.780 s on its second boot. Both lifecycle transitions settled the guest disk.

### Durability across an iOS reclaim

iOS can reclaim a suspended application without running any more of its code,
and a killed guest that was mid-write leaves a torn `/data`. The failure is
specific and was previously invisible: `vold` reports
`Version mismatch, expected 1 got 2`, `init_user0` fails, Android reboots into
recovery, and the legacy image fails because it has neither `/misc` nor a
recovery partition. The new device-overlay image supplies both partitions; the
quiesce contract remains necessary to avoid torn writable state.

The repair is to ask the guest before abandoning it. UML's management console is
the only channel that reaches a running guest kernel, and it had never worked:
the port's default umid was a constant, so only the first guest on a host could
bind its socket, and Android's init disables magic SysRq, which makes
`handle_sysrq` a silent no-op. The emulator now names each run and Kernarium
boots its guests with `sysrq_always_enabled`, so a refused request is reported
rather than assumed. `uml_emulator_sync()` asks the guest to write back;
`uml_emulator_quiesce()` writes back, freezes the guest through the same
console, then flushes the host's files; `uml_emulator_resume()` thaws it.
Kernarium flushes on losing focus, quiesces on backgrounding and termination,
and resumes on returning. The public API is version 6.

Measured on the same Simulator guest: backgrounding takes the process from 204%
CPU to 0.0% and stops the instance disk growing entirely, 170448 KiB unchanged
over thirty seconds. Killing it there and relaunching reopens the same disk with
`init_user0` exiting 0, no `Version mismatch`, and no reboot into recovery,
which is what every previous relaunch produced.

Guest-side write-through was evaluated as an alternative and rejected. It is
not reachable for this product: `/data`'s mount options come from
`support/fstab.uml` inside the vendor image, not from the host command line, so
a synchronous `/data` would require an image rebuild. It is also expensive.
Measured on a 2 GiB four-vCPU Linux guest with an ext4 scratch disk on the
gadget engine, two interleaved runs per arm, `-o sync` cost 3.4x on 300 file
creates (1.31 s to 4.49 s) and 1.41x on 1500 32 KiB overwrites (6.46 s to 9.10
s). The reachable half, bounding writeback through `sysctl.vm.*` boot
parameters, measured within 1% of the default but would be protecting a window
that quiesce has already closed: `sysrq m` on the booted Android guest reports
`dirty:0 writeback:0` at rest. Neither was adopted.

### Superseding application decisions

Two decisions recorded later in this document were made differently, and the
implementation is the current contract:

- The Android runtime framework is **not** a separate application. On explicit
  request it is embedded in the existing Kernarium app behind a guest-family
  selector, which the "Android runtime framework" section had deferred as a
  later explicit application decision. That decision has now been made.
- Its output name is `KernariumRuntimeAndroid_<release>_16K`, not
  `KernariumAndroidRuntime_17_ACK6_18_16K`. The guest flavour leads the name and
  the page profile trails it, so no name can match two selections; the loader
  matches `KernariumRuntimeAndroid_` against `KernariumRuntime_` and neither is
  a prefix of the other. Fixed decision 8's distinct-target-name requirement is
  met by output name, bundle identifier, module name, and binary directory; the
  CMake target `KernariumRuntime` is shared and parameterized by
  `UML_KERNEL_PRODUCT`.

### Open limitations

- **The sustained-transfer case has not been repeated with the current host
  allocator fix.** An earlier 2 GiB `adb push` transferred completely but
  degraded to 2.7 MB/s over 750 s and left the guest unresponsive; all eleven
  RCU stalls on that boot reported `0 ticks this GP` on an idle CPU 0. That
  signature is distinct from the now-fixed boot-time allocator stall. A 512 MiB
  push is clean, but the 2 GiB case remains unqualified rather than assumed
  fixed.
- **Physical iOS is unverified for Android.** Every Android application
  measurement above is from the Simulator. The device policy of 2 GiB of guest
  RAM has not been exercised against an Android guest.
- `artd` took one SIGSEGV through a null pointer, and `vendor.ril-daemon` exits
  1 in a restart loop. Both were observed on 2026-08-22 and neither is
  explained.
- One fresh boot of `20260824-c3febf12-theme1` panicked four seconds after
  boot completion with a kernel-mode Darwin signal 10. A second fresh boot and
  two warm boots did not reproduce it, including two 30-second post-completion
  dwells. Treat it as an open isolated stability observation until a repeat or
  a longer campaign gives it a disposition.
- **Matched macOS timing is highly variable across campaigns.** The accepted
  2026-08-24 run measured 786.169 s cold and 84.795 s warm, versus the earlier
  130.287/44.977 s evidence for the same release. The current cold delay
  reproduced in first-boot ART verification, but its host-side cause is not
  established. Do not present the 6.00x cold Simulator comparison as an
  intrinsic platform ratio.

### Required continuation order

The 2026-08-22 continuation order's release items are done: the Mesa route was
rebuilt and published, the ten-vCPU boot passed, and `accepted-release.env` has
advanced six times since. Its item 4 named the remaining plan phases, which
continue here:

1. Boot an Android `.kern` bundle on a physical iOS device at the 2 GiB device
   policy, and record what it reaches. This is the last unexercised product
   surface.
2. Repeat the 2 GiB `adb push` with the current bulk-retirement host. If its
   older idle-CPU timer signature still reproduces, close that distinct
   cooperative-timer starvation before claiming sustained-transfer stability.
3. Complete Phase 8's application acceptance: install and run one Java/Kotlin
   APK and one aligned `arm64-v8a` native APK, and register the Android
   integration workflow.
4. Explain or retire the `artd` SIGSEGV and the `vendor.ril-daemon` restart
   loop.

Commit after each focused change reaches a consistent, tested state.

## Superseded implementation handoff (2026-08-22)

This section is retained as history. It records the state on 2026-08-22, when
Android had not yet completed boot, and the three defects that were blocking it.
The 2026-08-24 handoff above supersedes it wherever the two differ; its
measurements remain valid observations of the builds they were taken on.

### Durable repository and builder state

- The outer repository was at `2275c9b4` (`gpu: restore Simulator GLES texture
  sampling`) on `main` when this handoff was written. Android-specific durable
  checkpoints immediately below it include `8f91b078` (GPU sysfs labeling),
  `c659cfc3` (Mesa zstd SP-HAL labeling), `577dbfea` (four-vCPU stall control),
  `8eeb714e` (Mesa DRI namespace release), `6afe4225` (incremental-analysis
  mode), and the earlier Android kernel, image, console, and product commits.
- The working tree also contains unrelated concurrent Apple-GPU/CTS and Xcode
  edits. At this checkpoint those include `Kernarium.xcodeproj`,
  `MAX_GL_GLES_SUPPORT_PLAN.md`, `REFACTOR_HANDOFF.md`, `testing.md`, and GPU
  integration/tooling files. Inspect `git status` and preserve those edits;
  they are not part of the Android CPU-correctness work or this handoff.
- The operator-configured build machine alias is intentionally not recorded in
  the repository. Its builder-local AOSP checkout is
  `/home/build/aosp-17`, its ACK Repo checkout is
  `/home/build/android-kernel-6.18`, and the ACK Git project is the `common/`
  child of that Repo checkout. Tracked scripts still contain no remote-shell or
  transport command, endpoint, or credential.
- The AOSP incremental output is
  `/home/build/aosp-17/out-uml-android17`. When invoking Android's build
  environment from the AOSP root, keep `OUT_DIR=out-uml-android17` relative.
  An attempted absolute `OUT_DIR` changed Soong's recorded environment and
  failed during bootstrap; restoring the relative value caused a broad
  dependency re-evaluation. That incremental build nevertheless completed
  successfully after 180,084 executed steps in 1 hour 55 minutes. Do not clean
  this output or repeat that build merely for preflight.
- The tested ACK ELF is
  `/home/build/uml-darwin-work/build-ack-android17-rcu-cond1/vmlinux`. It was
  built from the tracked 27-topic series with final tree
  `e675d73659f9f829c7e36db035f1652ce2a85408`, ABI version 8/fingerprint
  `0x3871ad612337ea76`, `CONFIG_MODULES=n`, and the 16 KiB-only configuration.
  Topic 27, `UML-DARWIN-LOCAL: poll timers before cond_resched`, is the
  maintained form of the successful cooperative-timer experiment; the ignored
  `cond-resched-timer.patch` is only its diagnostic precursor.

### Latest ten-vCPU gadgets boot

The latest run used the separate Android macOS executable, gadgets engine, a
fresh COW instance, the ACK ELF above, and ten vCPUs. The diagnostic release
root was
`.build/android/diagnostics/mesa-glapi/test-release/android17-ack6.18-16k-20260822-8f91b078-gpu-sysfs1`.
The corresponding files are:

- COW: `.build/android/instances/mesa-glapi-10vcpu-gadgets1.cow`;
- kernel/ttyS0 log:
  `.build/android/logs/mesa-glapi-10vcpu-gadgets1-ttyS0.log`;
- compressed diagnostic disk:
  `.build/android/diagnostics/mesa-glapi/android17-ubd.img.zst`; and
- raw diagnostic disk inside the release root:
  `guest/android-17-uml-arm64.img`.

The COW and raw disk each have a 16 GiB logical size and are sparse; the
compressed disk is approximately 868 MiB and the ttyS0 log is approximately
121 MiB. They are ignored diagnostics, not release inputs. The CPU defect is now
reproduced by a host unit test, so these exact stale artifacts may be removed
as part of the already-requested `.build` storage cleanup; the two `cubic-*`
payloads are still useful as an in-guest confirmation of the repair.

Confirmed results from this run:

- CPUs `0-9` came online. The 121 MiB ttyS0 log contains no RCU stall, kernel
  panic, oops, or hard-lockup report through guest second 23,844. The original
  multi-vCPU RCU/timer stall is therefore not reproduced with topic 27.
- `ttyS0` carried Linux printk only. The reconnectable Virtio console on
  `hvc0` carried the Android shell and `logcat`; the two streams remained
  separated as required.
- SurfaceFlinger remained running after the Mesa GLAPI correction. Its log
  identified vendor `Mesa`, renderer `virgl`, `OpenGL ES 3.0 Mesa
  26.1.0-devel`, a 16,384 maximum texture/viewport size, the AIDL composer3
  service, and display 0. This proves the hardware Virgl path reaches Android;
  SwiftShader remains packaged as the unchanged fallback.
- Android did not complete boot. `sys.boot_completed` never became `1` because
  zygote repeatedly died while preloading
  `android.graphics.drawable.RippleAnimationSession`. At guest second 23,844,
  after hours of service retry noise, `init` itself took signal 11 and requested
  a reboot; the emulator then stopped with result 1. The old emulator process
  and console socket are no longer live.

Repeated missing audio-core and RIL/NFC service messages are real product gaps,
but they are not the first blocker. Do not mask those services or change VINTF
until a boot with the repaired engine below shows that zygote starts reliably.

### Repaired gadgets-engine correctness defect

Zygote failed in `PathInterpolator.initPath()` because the native Skia cubic
path approximation reported an endpoint other than `(1,1)` for
`new PathInterpolator(0.4f, 0f, 0.2f, 1f)`. The current unstripped `libhwui`
compiled `SkPathGlue::cubicBezierCalculation` to a short scalar/SIMD sequence
whose final relevant operations include:

```text
ldp  d4, d6, [x0, #16]
fmul v2.2s, v4.2s, v2.s[0]
fadd v1.2s, v1.2s, v3.2s
fmul v0.2s, v6.2s, v0.s[0]
fadd v1.2s, v1.2s, v2.2s
fadd v0.2s, v1.2s, v0.2s
```

Two freestanding ARM64 payloads in the ignored diagnostics directory reproduced
the failure without Java, ART, Skia allocation, Mesa, or SurfaceFlinger:

- `.build/android/diagnostics/cubic-endpoints.S` executes the exact compiled
  cubic calculation at `t=0` and `t=1`. Under the gadgets engine it wrote four
  zero words; `t=1` should write `3f800000 3f800000` for `(1,1)`.
- `.build/android/diagnostics/cubic-steps.S` records the inputs around the final
  vector multiply. It observed `t=1`, `t^2=1`, and the first loaded pair
  `.2,1`, but the second destination of `ldp d4, d6, [x0,#16]` was zero instead
  of `1,1`; the following multiply was consequently zero.

The root cause is the tagged-address path of `vector_pair_fast` in
`darwin/cpu/arm64/gadgets/engine/load_store.inc`. Android userspace enables
top-byte-ignore, so `UML_GADGET_CPU_TAGGED_ADDRESS_STATE` is non-zero and every
guest vector pair leaves the direct fast path for
`uml_arm64_simd_structure_memory_transfer`. That helper moves the pair as one
contiguous range through a 32-byte stack buffer, but the gadget reloaded the
buffer with `ldp q0, q1, [sp]` and stored those two registers to the two
destinations. Only the 128-bit variant matches that split. For a 32-bit or
64-bit pair the whole range lands in `q0`, so the first destination kept the
second element in its upper half and the second destination became zero; the
store direction symmetrically wrote the first register's full width instead of
the two low halves. The repair splits and packs the narrow variants exactly as
the established cross-page path already did, so the fast, cross-page, and
tagged paths now agree. No other tagged transfer site was affected: the integer
pair path already reloads with `ldp w6, w7`/`ldp x6, x7`, and the SIMD
structure, replicate, and lane paths walk the buffer through the same shared
macros as their fast paths.

The defect was invisible to the existing suite because
`arm64-simd-load-store-pair` had no tagged-address coverage at all; the integer
pair suite owned a `top_byte_ignore_without_mte` matrix and its SIMD
counterpart did not. `tests/unit/arm64/arm64_simd_load_store_pair_tests.cpp`
now adds both a TBI matrix over the six allocated variants and four addressing
modes and an `EngineHarness::check_program` case carrying the exact
21-instruction compiled cubic sequence, the exact Skia control points, and a
tagged base pointer. Both new cases failed under gadgets and passed under the
interpreter before the engine change. The suite executes interpreter, gadgets,
and JIT over all 3,686 cases:

```sh
ctest --test-dir .build/presets/macos-unit-16k \
  -R '^arm64-simd-load-store-pair$' --output-on-failure
```

### Ten-vCPU verification boot (2026-08-22)

The repaired engine was verified in the guest. The run used an Android macOS
executable built from outer revision `eafe46af` in a throwaway worktree,
because the tracked tree's Apple GPU dependency prefix predates the committed
UTM WebKit patch 10 and no GPU-enabled preset configures until that prefix is
rebuilt. The executable was bound to the same
`android17-ack6.18-16k-20260822-8f91b078-gpu-sysfs1` Mesa-GLAPI diagnostic
release the previous run used, with ten vCPUs, the gadgets engine, and a fresh
COW:

- COW: `.build/android/instances/cpufix-10vcpu-gadgets1.cow`;
- kernel/ttyS0 log: `.build/android/logs/cpufix-10vcpu-gadgets1-ttyS0.log`;
- Virtio console socket: `/tmp/a17-cpufix.sock`.

Confirmed by this run:

- CPUs `0-9` came online and the guest reports `nproc` 10. The ttyS0 log
  contains no RCU stall, panic, oops, hard lockup, or soft lockup through guest
  second 345.
- SurfaceFlinger stayed up and `dumpsys SurfaceFlinger` reports
  `GLES: Mesa, virgl, OpenGL ES 3.0 Mesa 26.1.0-devel`, so the hardware Virgl
  path still reaches Android.
- **The vector-pair defect is gone.** Zygote is stable at `running` and 20,066
  lines of `logcat` contain zero occurrences of `Ripple` or `initPath`. The
  previous run died repeatedly at exactly that preload.

Android still does not complete boot, but the first blocker has moved and is
now a different, independent defect. `system_server` takes three fatal
exceptions and is not restarted:

```text
java.lang.RuntimeException: Failed to create service
    com.android.server.ConnectivityServiceInitializer
Caused by: java.lang.IllegalStateException: Failed to update uid permission
    chunk map
  at ...server.BpfNetMaps.setChunkPermListForUids(BpfNetMaps.java:1599)
  at ...connectivity.PermissionMonitor.<init>(PermissionMonitor.java:488)
  at ...server.ConnectivityService.<init>(ConnectivityService.java:2302)
Caused by: android.system.ErrnoException: nativeWriteToMapEntry failed:
    ENOMEM (Out of memory)
  at ...net.module.util.BpfMap.nativeWriteToMapEntry(Native Method)
```

This is not memory exhaustion and not a vmalloc shortage: the guest reports
`MemAvailable` 3,319,984 kB and `VmallocTotal` 516,016 kB against
`VmallocUsed` 13,376 kB when the write fails, and the kernel log carries no
allocation-failure or OOM record. `NetBpfLoad` loaded 5 of 5 programs first and
logged `UML kernel has no eBPF JIT; using its built-in interpreter`, which is
the expected `androidboot.uml_bpf_interpreter=1` contract.

The root cause was then identified exactly and is a UML kernel defect, not an
Android or product one. `bpftool` in the failing guest shows the map as a
`BPF_F_NO_PREALLOC` hash with a 4-byte key and a **1024-byte value**, and it
held exactly four entries. A non-preallocated hash map allocates elements from
`bpf_mem_alloc`, whose `prefill_mem_cache()` inserts exactly **one** element
per CPU once the unit size exceeds 256 bytes and refills the rest from
`irq_work`. UML never implemented `arch_irq_work_raise()`, so the generic weak
stub in `kernel/irq_work.c` left that refill waiting for the next timer tick:
`unit_alloc()` took the single prefilled element, found the free list empty on
the next insert, and returned `NULL`, which `htab_map_update_elem()` reports as
`-ENOMEM`. Any burst into such a map therefore accepts about one element per
CPU per tick.

Mainline topic 0022, `uml: raise irq work locally instead of at the tick`,
fixes this by publishing a self-directed IPI vector from
`arch_irq_work_raise()` and running `irq_work_run()` when it is dispatched. A
freestanding guest probe that fills a 1024-byte-value `BPF_F_NO_PREALLOC` hash
map inserted 1 of 256 elements before the topic and all 256 after it, on both
canonical page profiles.

ACK 6.18 topic 0028 ports the same change, and the builder produced release
`android17-ack6.18-16k-20260822-37b0ff5f-irqwork1` from it. Do not raise guest
memory or mask ConnectivityService to route around the defect.

### Withdrawn revert of the irq-work topic (2026-08-22)

Both irq-work topics were reverted once and have been restored. The revert was
based on a misattribution, and this section records it so the same mistake is
not repeated.

The `irqwork1` ten-vCPU boot ended with `rcu_sched` stalls at guest second 877,
CPU 0 a grace period behind in `default_idle_call`/`do_idle`, and
`vu_interrupt+0xa0` warning `read returned 0` from `swapper/0` until the ttyS0
log reached 240 MiB. That was attributed to the irq-work topic and blamed on a
livelock in the cooperative idle drain. **Both conclusions were wrong.**

Reading the log by line number rather than by summary shows the true order. The
`vu_interrupt` storm began at guest second **855.999**, twenty-one seconds
*before* the stall, and the three lines immediately above the first warning name
the real cause:

```text
Virtio console client detached
vhost-user connection failed: configuring no-signal console descriptor failed:
    Invalid argument
[  855.999467] WARNING: CPU: 0 PID: 0 at arch/um/drivers/virtio_uml.c:845
```

A console client that connects and closes again before the emulator accepts it
leaves a fully shut down socket, and Darwin rejects `F_SETNOSIGPIPE` on such a
socket with `EINVAL`. `UnixSocketChannel` threw on that, the exception escaped
`drainEventSource()` into the vhost-user connection loop, and the loop reset the
console device for the rest of the run. The guest was left holding a virtio call
descriptor at end of file, which stays readable forever, so `vu_interrupt()`
re-entered on every poll and pinned CPU 0 until RCU declared the stall.

The control that appeared to convict the topic did not: `mesa-glapi` saw only
**one** console detach in 23,844 guest seconds, so it barely exercised the race
at all. Counting detach events across the four ten-vCPU logs shows the real
distribution, and shows that the failure is a rare race rather than a property
of any kernel:

| run | console detaches | configure failures | `vu_interrupt` warns | RCU stalls |
| --- | --- | --- | --- | --- |
| `irqwork` (28 ACK topics) | 16 | 1 | 397,874 | 2 |
| `cpufix` (27 ACK topics) | 17 | 0 | 0 | 0 |
| `c659cfc3` (27 ACK topics) | 7 | 0 | 0 | 0 |
| `mesa-glapi` (27 ACK topics) | 1 | 0 | 0 | 0 |

The console defect is repaired in `93b4db99` and pinned by a host unit
regression that reproduced the exact production message. The lesson for this
plan is procedural: a per-run watcher that reconnects to the console socket on
every poll is itself a load generator, and a control run must be matched on the
host interactions the candidate run performed, not only on the kernel.

`sys.boot_completed` never became `1` in that run, so the ten-vCPU acceptance in
the continuation order below is still not met and the tracked accepted-release
descriptor still selects `android17-ack6.18-16k-20260822-6afe4225-virgl2`.

### vCPU-0 serial-line interrupt storm (2026-08-22)

Every ten-vCPU boot recorded in this plan ran with a host `stdin` that could
reach end of file. The UML serial line (`ssl`) polls that descriptor, sees it
permanently readable, reads zero bytes, and re-arms, so the guest takes an
unbounded level-triggered SIGIO storm on vCPU 0. One wedged run measured

    /proc/interrupts
      0: CPU0=367       CPU1=48514 ... CPU9=35666   SIGALRM hr timer
     19: CPU0=43851100  every other CPU=0           SIGIO   ssl

CPU 0 took 367 timer ticks while its peers took 32k-49k, `/proc/stat` shows its
accounting frozen near 408 jiffies against roughly 49,300 elsewhere, and RCU
reported `0-...0: (0 ticks this GP)` with `vdc` spinning on CPU 0. A host
`sample` placed `uml-vcpu-0-carrier` in `poll()` or `read()` for 1966 of 2087
samples. That boot died at 4.45 s, before SurfaceFlinger ever started.

Giving the emulator a stdin that never reaches end of file removes the storm
completely: `ssl` interrupts stay at zero, CPU 0 resumes ticking, and the same
release boots past `vdc cryptfs encryptFstab` into the framework. Every earlier
measurement in this plan was taken under the storm and must not be treated as a
clean control. The serial line should also refuse to re-arm on a hung-up
descriptor, which is the same defect class as the `vu_interrupt` end-of-file
storm repaired in `93b4db99`; that repair is not yet written.

### SurfaceFlinger EGL failure is in this repository (2026-08-22)

An earlier section of this plan called the SurfaceFlinger failure environmental.
That was wrong, and the evidence retiring it is recorded here. No run has ever
reached `sys.boot_completed=1`; the two runs previously described as healthy
stopped before SurfaceFlinger started, so their clean logs were never evidence
of a working graphics stack.

With the storm removed, the accepted release
`android17-ack6.18-16k-20260822-6afe4225-virgl2` fails in three separable
layers, each confirmed in a live guest:

1. `libEGL_mesa.so` does not load at all:

       E libEGL : load_driver(/vendor/lib64/egl/libEGL_mesa.so): unknown

   The kernel audit log carries the reason: SurfaceFlinger is denied `read`,
   `open`, `execute`, `map`, and `getattr` on `/vendor/lib64/libzstd.so`, which
   is labelled `vendor_file`. `dlopen` fails on that dependency, which is why
   `dlerror()` was empty and the loader printed `unknown`. `c659cfc3` labels
   Mesa's zstd as a same-process HAL and postdates this release.

2. SurfaceFlinger is also denied `read`, `open`, and `getattr` on
   `/sys/devices/virtio-uml-cmdline/virtio-uml.0/uevent`, labelled `sysfs`
   rather than `sysfs_gpu`. libdrm reads that file to identify the device.
   `8f91b078` and `fd2bc031` supply the label, so **`fd2bc031` is a real fix,
   not the non-fix an earlier permissive run appeared to show**; that run was
   taken under the vCPU-0 storm.

3. With SELinux set permissive live, `libEGL_mesa.so` loads and Mesa still
   fails, with the device present but never opened:

       W EGL-MAIN: failed to get driver name for fd -1
       W EGL-MAIN: MESA-LOADER: failed to retrieve device information
       W EGL-MAIN: egl: failed to create dri2 screen

   The guest DRM node is a platform device with no device-tree node
   (`device/subsystem` resolves to `/sys/bus/platform`, and its uevent carries
   only `DRIVER=virtio-uml` and `MODALIAS=platform:virtio-uml`), and the Mesa
   route was missing the pipe-loader descriptor that makes virgl selectable.

`a6bad438` cut route `p0004` from three patches to one on the inference that the
regeneration had caused a SurfaceFlinger crash. That inference was disproved in
the same session by hashing the two guest images, which were byte-identical;
only the manifest metadata differed. The two withdrawn commits are
`Enable Virgl pipe-loader descriptor`, which defines `GALLIUM_VIRGL` for the
Gallium pipe-loader so a virgl driver descriptor exists at all, and
`Share GLAPI dispatch state`, which leaves `gallium_dri.so` the sole GLAPI
owner. Both are restored, returning the route to three patches and final tree
`4295979a`.

### Graphics fixed and proven; boot now blocks on the audio HAL (2026-08-22)

Release `20260822-9db7004e-mesa3p1` carries the restored three-patch Mesa route
together with the `c659cfc3` zstd label and the `fd2bc031` GPU sysfs label. Booted
at ten vCPUs on the gadgets engine with SELinux enforcing and a host stdin that
cannot hang up, it settles the EGL question:

- SurfaceFlinger started once at guest 38.6 s and never restarted. No SIGABRT.
- `dumpsys SurfaceFlinger` reports a working renderer:

      GLES: Mesa, virgl, OpenGL ES 3.0 Mesa 26.1.0-devel
      EGL implementation : 1.5 Android META-EGL
      ------------RE GLES (Ganesh)------------
      android.hardware.graphics.composer3.IComposer version:5 ... hwc3-drm

- `ssl` interrupts stayed at zero, every CPU kept taking timer ticks, and no RCU
  stall, panic, oops, or lockup appeared.
- The only AVC denial in the entire run is the pre-existing, unrelated `init`
  write to sysfs `discard_max_bytes`. Both graphics denials are gone in
  enforcing mode, which confirms `c659cfc3` and `fd2bc031` together.

`sys.boot_completed` still does not become `1`, but the cause has moved out of
graphics and out of the CPU. `system_server`'s main thread blocks during
`AudioService` construction:

    SystemServer.startOtherServices
      -> SystemServiceManager.startService
      -> AudioService.<init> -> AudioDeviceBroker.<init>
      -> AudioDeviceBroker.getCommunicationDeviceInt
      -> AudioSystemAdapter.getDevicesForAttributes
      -> AudioSystem.getDevicesForAttributes (native)
      -> mediautils::getService<IAudioPolicyService>
      -> condition_variable::wait_until        blocked

`IAudioPolicyService` never arrives because `audioserver` cannot obtain
`android.hardware.audio.core.IModule/default`; servicemanager reports it as not
found and its lazy start fails. The release device VINTF manifest declares no
HALs at all, while the init service `vendor.audio-hal-aidl` is installed and does
start. Android's watchdog then kills the system process with "Blocked in handler
on main thread (main) for 69s", four times in this run, and zygote restarted five
times. That is a HAL declaration problem, not a renderer problem.

Two further observations from the same run are recorded but not yet explained:
`artd` took a SIGSEGV at fault address 0 with backtrace `#00 pc 0000000000000000`,
a call through a null pointer, once; and `vendor.ril-daemon` exits with status 1
in a tight restart loop.

### AOSP diagnostic and patch-series state

`device/google/cuttlefish` is clean on builder-local branch
`uml-android17-no-userdata-quota`; its current tree is the already-recorded
`ed279eb3c267bb8480d734f8219adcb2506dce3a` for route `p0001`. It includes the
Virtio console, Mesa packaging, zstd SP-HAL label, and GPU sysfs uevent label.
No p0001 regeneration is currently required unless a reviewed product property
is added later.

`external/mesa3d` is clean on branch
`uml-android17-virgl-descriptor`. Its current three local commits are:

```text
e85639a2878 UML-ANDROID-LOCAL: Build 16 KiB Mesa Virgl GLES stack
6d0d5ab94fa UML-ANDROID-LOCAL: Enable Virgl pipe-loader descriptor
470d1a2b657 UML-ANDROID-LOCAL: Share GLAPI dispatch state
```

The current Mesa tree is
`4295979a36e8b4650af5e98bb9fd0c6a9dd12bcd`. Route `p0004` was regenerated from
that clean three-commit topic branch in `37b0ff5f` and now records all three
patches. Never edit its patch bytes manually.

The GLAPI commit is required. Before it, `libEGL_mesa`, both GLES libraries,
and `gallium_dri.so` each statically defined their own GLAPI dispatch state and
SurfaceFlinger crashed. The fixed layout leaves `gallium_dri.so` as the sole
GLAPI owner: EGL/GLES reference it dynamically, and `_glapi_new_nop_table` is
exported from the DRI library. Preserve this dependency architecture when
regenerating p0004.

`frameworks/base` is detached at base commit
`94b4c163b7dfe5ce3607f7bb8456f9573f7de57d` and has one uncommitted diagnostic
change to `PathInterpolator.java` that appends the actual endpoint values to
the exception. The completed target-files output contains that diagnostic.
It is not a product fix and must never become an AOSP route. The host
regression has now captured the needed evidence, so restore that one file on
the build machine and use the existing relative `OUT_DIR` for the incremental
rebuild.

The tracked accepted-release descriptor intentionally still selects
`android17-ack6.18-16k-20260822-6afe4225-virgl2`. The installed
`20260822-8f91b078-gpu-sysfs1` directory and the Mesa-GLAPI test release are
diagnostic candidates. Do not advance `accepted-release.env` until a clean,
patch-replayed image reaches full boot acceptance.

### Continuation order as of 2026-08-22

Items 1 to 3 were carried out and the boot passed; item 4's remaining phases
continue in the 2026-08-24 handoff above. Recorded as written:

The gadgets vector-pair repair, the BPF `ENOMEM` kernel repair, and the console
accept repair are all in the tree, and the console repair is confirmed in a live
guest: sixty deliberate connect-and-close races produced sixty rejected clients,
zero device resets, zero `vu_interrupt` warnings, and a console that still
answered afterwards. The Apple GPU dependency prefix has been reinstalled, so
the tracked tree configures again. The remaining order is:

1. Rebuild `external/mesa3d` incrementally on the builder from the restored
   three-patch `p0004` route, reusing the existing Soong output, and publish a
   release that also carries the `c659cfc3` zstd label and the `fd2bc031` GPU
   sysfs label.
2. Boot that release at ten vCPUs on the gadgets engine with SELinux enforcing
   and a host stdin that cannot reach end of file. The JIT engine is not used
   for Android boot. Require `ssl` interrupts to stay at zero and every CPU
   `0-9` to keep taking timer ticks, then `sys.boot_completed=1`, stable
   zygote/system_server and SurfaceFlinger processes, working
   `dumpsys SurfaceFlinger`, the Mesa/virgl renderer report, and no RCU stall,
   panic, oops, lockup, or mixed ttyS0/hvc0 logs. A long retry loop is not a
   successful boot. The BPF `ENOMEM` repair is so far supported only by the
   freestanding probe; this boot is what confirms it in the guest.
3. Select the new immutable release in `accepted-release.env` only after that
   boot passes. Keep SwiftShader packaged. The `frameworks/base` diagnostic is
   already reverted on the builder and saved as
   `pathinterpolator-diagnostic.patch`.
4. Remove superseded diagnostic disks, COWs, logs, and builder-local images by
   exact path, including the 240 MiB `irqwork-10vcpu-gadgets1-ttyS0.log` once
   its evidence is no longer needed. Then continue the remaining plan phases:
   Android application acceptance, product HAL cleanup, and the separate signed
   16 KiB iOS runtime framework.

Commit after each focused change reaches a consistent, tested state. Do not
fold the CPU fix, Mesa patch regeneration, AOSP diagnostic cleanup, and release
promotion into one unreviewable commit.

## Fixed decisions

The following decisions are requirements, not optional implementation ideas:

1. All Darwin UML, Android product, and build-system changes are maintained as
   local project changes. There is no contribution or submission workflow.
2. The guest is AOSP 17 on ACK 6.18.
3. A dedicated SSH-reachable x86_64 Linux machine builds AOSP, the guest disk,
   the ramdisk/support files, and the Android ACK UML kernel ELF.
4. The Android kernel line has exactly one page profile: 16 KiB. It has no 4
   KiB configuration, artifact, preset, framework, executable, or test product.
5. The Android UML kernel is built from the pinned ACK repository, not from the
   Linux stable repository used by the current mainline products.
6. Kernel sources of truth are organized under `kernel/common`,
   `kernel/mainline-6.18`, and `kernel/android-6.18`.
7. CMake, Ninja, and Xcode consume already-built local artifacts only. They
   never fetch sources, contact the Linux builder, replay patches, invoke
   Kbuild, run Soong, or assemble a disk image.
8. The Android macOS executable and Android runtime framework have distinct
   output names, bundle identities, and output directories from the current
   products. The macOS executable also has a distinct target name. The runtime
   framework shares the `KernariumRuntime` CMake target, parameterized by
   `UML_KERNEL_PRODUCT`, because both flavours build the same composition
   against a different accepted artifact; its distinctness is carried by the
   `KernariumRuntimeAndroid_` output name, bundle identifier, module name, and
   binary directory, which is what loader discovery and packaging consume.
9. Every change made inside an AOSP Git project is represented by a generated
   per-project patch series under `android/aosp-17/patches`; copying an overlay
   into an AOSP checkout is not an accepted source-management workflow.
10. Android 17 build scripts, image-layout inputs, release tooling, and other
    non-kernel support files live under `android/aosp-17`. Android ACK patches,
    configuration, and line metadata remain under `kernel/android-6.18`.
11. The repository stores no build-machine SSH hostname, user, port,
    credential, key path, or connection option. Tracked scripts contain no
    SSH, scp, sftp, rsync, or equivalent command for executing on or
    transferring files to or from the build machine. An operator copies the
    required tracked files to the build machine before running its local build
    scripts and copies the finished release back outside those scripts.
12. The Android ACK kernel sets `CONFIG_MODULES=n`. Every kernel facility the
    Android guest requires is linked into `vmlinux`; no `.ko`, DLKM partition,
    module loader, or late executable-code path exists. This is required by
    the iOS runtime, which executes the signed immutable kernel carrier without
    a JIT entitlement and cannot publish loadable module text at runtime.

## Non-goals

The first accepted product does not include:

- a 4 KiB Android kernel or a page-size fallback;
- Google Mobile Services, Play Store certification, Play Integrity, or device
  attestation;
- an unchanged Cuttlefish image, a Waydroid container image, or a GSI used as
  a complete guest;
- loadable guest kernel modules;
- Android Virtual A/B, OTA, or production AVB enablement during initial
  bring-up;
- Vulkan, camera, microphone, telephony, Wi-Fi, Bluetooth, GNSS, UWB, or sensor
  claims; or
- automatic builder invocation from an Apple configure, build, test, or
  packaging command.

## Architectural boundary

The result has one producer boundary and one consumer boundary:

```text
reviewed outer-project revision on the operator workstation
    android/aosp-17 build scripts, AOSP patch series, and support files
    kernel/android-6.18 ACK patch series and configuration
             |
             | operator-managed copy outside tracked scripts
             v
dedicated x86_64 Linux builder staging tree
    pinned outer-project revision
    pinned AOSP 17 manifest
    pinned ACK 6.18 commit
             |
             | builder-local prepare/build/verify/publish
             v
versioned Android release bundle on the Linux builder
             |
             | operator-managed copy outside tracked scripts
             v
Git-ignored incoming release on the workstation
             |
             | local-only install and verification
             v
validated prebuilt release under .build/android/releases
             |
             | read-only manifest import
             v
macOS executable / iOS Device framework / iOS Simulator framework
```

There is no build edge across the copy boundaries. The Apple product graph
does not know a builder hostname, remote path, AOSP checkout, ACK checkout,
compiler invocation, image assembly command, or transport command.

The same prebuilt-only rule is applied to the current mainline kernel
products during the directory migration. Standalone kernel workflows produce
artifacts; emulator workflows import them.

## Target repository layout

The target ownership is:

```text
kernel/
  README.md
  CMakeLists.txt

  common/
    README.md
    abi/
    artifacts/
      kernel-artifact-manifest.schema.json
      verify-kernel-artifact.py
      KernelArtifact.cmake
    config/
      page-16k.fragment
    tooling/
      materialize-git.sh
      verify-series-policy.py
      verify-linux-imports.sh
      shared script libraries

  mainline-6.18/
    README.md
    source-location.conf.example
    config/
      common.fragment
      page-4k.fragment
    patches/
      metadata.env
      extraction-decisions.tsv
      outside-port-justification.tsv
      path-ownership.tsv
      topic-path-overlaps.tsv
      series/
    build/
      build-kernel.sh
      KernelToolchain.cmake
      CMakeBuildLayout.cmake
      toolchain/
    scripts/
      prepare-source.sh
      materialize-git.sh
      audit-delta.sh
      rebase-patches.sh
      verify-replay.sh
      check-next-version.sh
      source-location.sh

  android-6.18/
    README.md
    config/
      android.fragment
    patches/
      metadata.env
      extraction-decisions.tsv
      local-change-policy.tsv
      path-ownership.tsv
      topic-path-overlaps.tsv
      series/

android/
  README.md
  aosp-17/
    README.md
    metadata/
      aosp.env
      accepted-release.env
    patches/
      projects.tsv
      projects/
        <route-id>/
          metadata.env
          path-ownership.tsv
          series/
            manifest.tsv
            0001-*.patch
    support/
      boot/
        kernel-command-line.txt
      image/
        partition-layout.json
      release/
        release-manifest.schema.json
      verification/
        required-kernel-builtins.txt
        forbidden-kernel-config.txt
    build/
      prepare-sources.sh
      apply-aosp-patches.sh
      build-kernel.sh
      build-aosp.sh
      assemble-image.sh
      build-release.sh
      verify-release.sh
      publish-release.sh
    tools/
      regenerate-aosp-series.sh
      verify-aosp-patches.py
      install-release.py
```

`kernel/README.md` becomes a short index. It explains the two kernel lines,
the shared ABI and artifact contracts, and the fact that the emulator build is
an artifact consumer. It does not contain either line's source or patch policy.

`kernel/android-6.18` owns only ACK line documentation, configuration, and
generated kernel patches. `android/aosp-17/build/build-kernel.sh` is the
versioned Android release orchestrator that consumes those inputs; it does not
duplicate or modify their source of truth. All other Android producer scripts
and non-kernel support stay beside it under `android/aosp-17`.

### Current-path migration

Move the current kernel material as follows:

| Current path | Target path |
| --- | --- |
| `kernel/patches/**` | `kernel/mainline-6.18/patches/**` |
| `kernel/config/common.fragment` | `kernel/mainline-6.18/config/common.fragment` |
| `kernel/config/page-4k.fragment` | `kernel/mainline-6.18/config/page-4k.fragment` |
| `kernel/config/page-16k.fragment` | `kernel/common/config/page-16k.fragment` |
| `kernel/toolchain/**` | `kernel/mainline-6.18/build/toolchain/**` |
| `kernel/build-kernel.sh` | `kernel/mainline-6.18/build/build-kernel.sh` |
| `kernel/KernelToolchain.cmake` | `kernel/mainline-6.18/build/KernelToolchain.cmake` |
| `kernel/CMakeBuildLayout.cmake` | `kernel/mainline-6.18/build/CMakeBuildLayout.cmake` if still needed by the standalone workflow |
| `kernel/abi/**` | `kernel/common/abi/**` |
| current source/replay scripts | fixed-line wrappers under `kernel/mainline-6.18/scripts/` plus parameterized engines under `kernel/common/tooling/` where genuinely shared |
| `kernel/README.md` | `kernel/mainline-6.18/README.md`, followed by a new root index |

The ignored `kernel/source-location.conf` is user-owned local data and must not
be deleted or moved automatically. Migration documents copying its value to
`kernel/mainline-6.18/source-location.conf`; the new per-line files remain
ignored. Android and ACK checkout paths are supplied only to the copied build
scripts on the Linux machine, through builder-local command-line arguments or
an untracked file created there. The repository contains no build-machine
endpoint configuration.

Move the current patch series as one byte-preserving operation before changing
its scripts. Do not combine the path move with a patch rebase or configuration
change. Update all tracked references and tests in the same checkpoint so no
supported command depends on compatibility symlinks.

### Common versus version-specific ownership

Only behavior proven identical belongs in `kernel/common`:

- the canonical plain-C Darwin host/kernel ABI;
- the 16 KiB page and 128 KiB task-stack fragment;
- the kernel-artifact schema and read-only artifact verifier;
- ELF/import validation that accepts an explicit kernel-line policy; and
- source/patch helpers parameterized by a required line root.

The following remain version-specific:

- pinned source identity and final-tree identity;
- generated patch bytes, stable patch IDs, hashes, order, and overlap policy;
- complete product configuration fragments;
- source checkout and build commands;
- delta ceilings and path ownership;
- compiler/sysroot policy; and
- guest support files tied to Android 17 or the current desktop guests.

Do not share one generated mail patch between mainline 6.18 and ACK
6.18. Port the intended behavior as ACK-based topic commits and regenerate a
separate Android series.

## Local-only change policy

Both kernel lines and every routed AOSP project remain private project
replays. Active policy and command output use local dispositions such as:

- `carry-locally`;
- `drop-locally-if-contained`;
- `rebase-locally-and-retest`; and
- `not-applicable-to-this-line`.

Do not add a submit, upload, push, review-server, or contribution command.
Project-authored ACK topics use a clear local prefix such as
`UML-DARWIN-LOCAL:`; project-authored AOSP topics use a distinct prefix such as
`UML-ANDROID-LOCAL:`. A commit copied from a pinned source retains its original
authorship and provenance, but its presence in this project does not imply a
submission plan.

Patch files remain generated artifacts. Each kernel line and AOSP route owns a
clean linear local topic branch. Changes are made with Git
commit/rebase/amend operations and regenerated with `git format-patch`; patch
bytes are never edited manually.

The active series tooling must take an explicit line root and fail if that root
does not match its metadata. A regeneration command for one line must be
structurally unable to write the other line's metadata or patch directory.

### AOSP multi-project patch contract

`android/aosp-17/patches/projects.tsv` is the routing authority for every AOSP
project changed by this product. Each row records an opaque stable route ID,
manifest project name, exact checkout path, manifest revision, base commit and
tree, expected final tree, patch count, series-manifest identity, and the
relative series directory.
Opaque route IDs prevent nested Repo project paths from aliasing one another.
Each corresponding `projects/<route-id>/metadata.env` repeats the
project-specific identity needed by its verifier.

All source-tree changes are generated patches, including newly added product
makefiles, BoardConfig files, SELinux policy, init and ueventd rules, fstab,
VINTF declarations, feature XML, and HAL configuration. The initial product
places those additions within explicitly selected projects already present in
the pinned AOSP manifest, so each series has a real pinned base. If a future
change requires a new Git project, that project first needs a separately
reviewed baseline and manifest entry in this patch contract; copying an
untracked directory into the checkout is never the fallback.

Each AOSP project is maintained as its own clean local topic branch. Modify the
commits with Git rebase/amend and regenerate its `git format-patch` series and
metadata; never edit patch bytes. Each series manifest records patch order,
filename, stable patch ID, hash, and subject. Generate with full indexes and
binary support so additions, deletions, renames, modes, symlinks, and any
necessary binary changes remain Git-owned. The builder verifies every project
at its declared base, applies only that project's ordered series, checks the
expected final tree, and rejects unmanifested AOSP changes. Applying or
regenerating one project's series must be structurally unable to write another
project or either kernel line.

Application tooling resolves the route against the pinned Repo manifest and
rejects checkout-path mismatches, dirty bases, symlink escapes, and patch paths
that are absolute, contain `..`, or enter `.git` or `.repo`.

`android/aosp-17/support` contains only inputs consumed outside AOSP source,
such as disk assembly, release-schema, and verification policy. A file copied
into an AOSP worktree is source-tree content and therefore belongs in a patch
series, not in `support`.

## Shared host/kernel ABI

Move the canonical bridge and its declaration history to
`kernel/common/abi`. The first Android kernel must use the same ABI version and
fingerprint as the current host implementation.

Each kernel line separately verifies that its replayed source contains exact
generated copies of the common ABI. Each published kernel release records the
ABI version and fingerprint. Every Apple product rejects an artifact whose ABI
identity differs from the host build.

Do not introduce an Android-only callback as an undeclared side channel. If an
unavoidable bridge extension is discovered, make it backward-compatible,
advance the common ABI once, update both kernel lines, and requalify both
current products before consuming it from Android.

## Dedicated Linux builder

### Builder staging and configuration

The Linux machine remains reachable through the operator's normal SSH setup,
but that setup is wholly outside this repository. Do not add a hostname, user,
port, key or credential path, connection option, remote work path, or example
endpoint to a tracked configuration, preset, script, or test fixture.

Before a build, the operator copies a clean snapshot of the selected
outer-project revision to a staging directory on the Linux machine using an
external administrative workflow. At minimum, the snapshot contains
`android/aosp-17`, `kernel/android-6.18`, `kernel/common`, and the outer source
needed to build and validate the shared host/kernel bridge. The copy operation
is not implemented or wrapped by any repository script.

After the copy, every producer script runs locally on the Linux machine. It
accepts builder-local AOSP, ACK, work, output, release, toolchain, and Darwin
SDK/sysroot paths through command-line arguments or an untracked data file
created on that machine. None of those values describes how to connect to the
machine.

The preparer proves that its current host reports `Linux` and `x86_64`. It also
checks available disk, RAM, JDK, `repo`, Git, Python, build tools, LLVM, and the
configured Darwin SDK/sysroot before starting an expensive build. A tooling
test rejects build-machine remote-execution/file-transfer commands and
endpoint/credential fields anywhere under `android/aosp-17/build`.
Builder-local public source synchronization by `repo` and Git during the
prepare phase is allowed; it is not a workstation-to-builder transport path
and must not embed authentication data in tracked source URLs.

### Compiler domains

The ACK UML build has two compiler domains:

1. `HOSTCC` builds native x86_64 Linux Kbuild utilities.
2. `CC`/`LD` build the AArch64 ELF kernel and Darwin-facing UML objects.

The Darwin UML backend keeps ordinary kernel objects as AArch64 ELF. Its
host-facing objects retain the Darwin SDK ABI while remaining ELF so the final
image can be linked as the relocatable `vmlinux` consumed by the Apple loader.
The Linux builder therefore needs a separately provisioned, version-recorded
Darwin SDK/sysroot for headers; it does not use the Linux host headers for
Darwin-facing objects. The plan does not redistribute that SDK.

Before any AOSP build is coupled to the kernel, a Phase 0 proof must show that
the pinned ACK source plus the local Darwin UML series can be cross-built on
the designated Linux machine into an AArch64, ELF64, ET_DYN `vmlinux` with
16 KiB executable boundaries and the exact common ABI imports. This is a
go/no-go gate, not a late integration task.

Do not assume ACK Kleaf already understands this nonstandard Darwin UML
target. A project-local wrapper may invoke Kbuild directly with the pinned ACK
toolchain. The command and tool versions are recorded in the build receipt.

### Builder-local phases and copy boundary

After the tracked files have been copied to the builder, the workflow has four
phases:

1. **Prepare.** `android/aosp-17/build/prepare-sources.sh` is the only
   network-enabled producer step. It synchronizes the exact AOSP manifest and
   ACK commit into builder-local storage, verifies commits and trees, creates
   clean worktrees, replays the ACK-local series, and applies every routed AOSP
   project series from `android/aosp-17/patches/projects.tsv`.
2. **Build.** The offline scripts build the ACK UML kernel, AOSP product,
   ramdisk/support files, and composite guest disk. They write only to
   builder-local work and output directories.
3. **Verify and publish.** Verification runs against a staged builder-local
   release directory. Publication atomically renames it to a new release ID
   and refuses to replace an existing release.
4. **Copy and install.** After publication is complete, the operator copies the
   entire release directory to a fresh incoming directory on the workstation
   through an external workflow. The local-only
   `android/aosp-17/tools/install-release.py` validates and installs that
   already-copied directory; it never contacts or names the builder.

The build, verify, and publish steps must succeed with source-network access
disabled. No builder preparation, build, copy, publication, or installation
command is a CMake target, build preset, CTest, Xcode build phase, or dependency
of an Apple target.

## ACK 6.18 kernel product

### Base and local series

`kernel/android-6.18/patches/metadata.env` records:

- ACK repository URL;
- source branch and exact tag;
- base commit and tree;
- local replay branch;
- expected final tree;
- patch count and delta ceilings;
- common ABI version and fingerprint; and
- the Android kernel release identity exported to the artifact manifest.

Re-evaluate every current Darwin UML topic against ACK instead of copying the
current patch bytes:

- drop a topic locally when ACK already contains the required behavior;
- omit the current Smack-specific correction because Android uses SELinux;
- omit or reshape Darwin-host-tool portability patches that are unnecessary on
  an x86_64 Linux build host;
- retain the Darwin software-MMU, AArch64 UML, fixed-vCPU-carrier, timer,
  signal, device, relocatable-ELF, and host-bridge behavior that the Apple
  runtime requires; and
- record exact overlap and path ownership for the ACK final tree.

### One 16 KiB configuration

The Android builder has no `--page-size` choice. Its only accepted
configuration combines:

```text
kernel/common/config/page-16k.fragment
kernel/android-6.18/config/android.fragment
```

The build fails if the resolved configuration does not select 16 KiB pages or
if a 4 KiB fragment, build directory, artifact name, or manifest value appears
in the Android lane.

`android/aosp-17/support/verification/required-kernel-builtins.txt` owns the
exact Kconfig symbol set required by the selected Android product. Every entry
must resolve to `y`; resolving one to `m` or enabling module support to satisfy
a dependency is a hard build failure.

The resolved policy must provide at least:

- AArch64-only UML with no 32-bit compatibility ABI;
- built-in Binder/BinderFS and `binder`, `hwbinder`, and `vndbinder` devices;
- Audit and SELinux as the exclusive major LSM, with Smack disabled;
- Android 17 `memfd_class` support and the ACK ashmem-on-memfd compatibility
  behavior;
- DMA-BUF and the allocator heap required by the Android graphics stack;
- built-in Virtio GPU, Virtio input, Virtio Sound, UBD, Ethernet, ext4, EROFS,
  GPT partition parsing, loop, device mapper, dm-verity, fscrypt, and
  fs-verity support needed by the selected product;
- all cgroup, namespace, PSI, and memory-controller support required by
  Android init and system services;
- `CONFIG_MODULES=n`; and
- `CONFIG_HOSTFS=n`, preserving the existing rule that a guest cannot select
  an arbitrary host directory as a filesystem.

`CONFIG_MODULES=n` is a carrier-safety boundary, not an optimization. The
signed Android `vmlinux` must contain the complete executable kernel closure
before Apple packaging. Android init, first-stage ramdisk, vendor configuration,
and release support files must not invoke `modprobe`, `insmod`, `rmmod`, or an
equivalent module loader, and no driver may rely on deferred `.ko` loading.

Use the conventional built-in C Binder for initial bring-up and remove the
Cuttlefish `binder.impl=rust` selection. Rust Binder can be evaluated later as
a separately justified local product change after the native Android guest is
accepted; it is not part of the first kernel port.

### Kernel verification

The builder-local kernel gate requires:

- exact ACK base and final replay tree;
- clean ordered local series and policy metadata;
- exact resolved fragment values;
- `getconf`-independent ELF inspection proving AArch64, ELF64, ET_DYN, the
  expected entry point, relocation policy, and 16 KiB executable boundaries;
- no writable-executable segment;
- undefined `darwin_uml_*` imports exactly matching the common 16 KiB ABI;
- exact `CONFIG_MODULES=n`, every required kernel feature resolved to `y`, no
  Kconfig value resolved to `m`, and no `.ko`, module-dependency inventory,
  DLKM image, or module-loading init action;
- no Android 4 KiB output; and
- generation of the compact kernel support set used by release verification.

## AOSP 17 product

### Product ownership

Create a project-owned, 64-bit-only product such as
`aosp_uml_arm64_pgagnostic`. Its source of truth is the set of routed
per-project series below `android/aosp-17/patches`; there is no product-source
overlay under either `android/aosp-17/support` or `kernel/android-6.18`.
Product makefiles and every device, policy, init, fstab, VINTF, feature, and
HAL file added to an AOSP checkout are introduced by the series for the AOSP
Git project that owns their destination. The prepared AOSP worktree is never a
source of truth.

Derive the product from Cuttlefish's page-size-agnostic ARM64 definitions and
reuse only the portions matching this emulator. The product must retain:

```make
PRODUCT_NO_BIONIC_PAGE_SIZE_MACRO := true
PRODUCT_MAX_PAGE_SIZE_SUPPORTED := 16384
PRODUCT_CHECK_PREBUILT_MAX_PAGE_SIZE := true
TARGET_BOOTS_16K := true
```

Do not select a 32-bit secondary architecture.

### Device adaptation

Replace Cuttlefish host assumptions with emulator-owned interfaces:

| Cuttlefish assumption | UML Android product |
| --- | --- |
| GKI kernel in `boot.img` | separately published ACK UML `vmlinux`; any AOSP boot image is an intermediate only |
| virtio-blk/by-name devices | GPT partitions on UBD with Android ueventd/by-name support |
| virtio-console | `ttyS0` host console and `tty0` framebuffer console |
| vsock host services | ordinary Ethernet plus explicit local host forwarding where required |
| loadable `*_dlkm` modules | required drivers built into the kernel; no DLKM partitions or module loader services |
| Cuttlefish Rust Binder selection | conventional built-in C Binder for first bring-up |
| simulated camera/radio/sensors | absent feature declarations and no matching HAL services |
| Vulkan-capable virtual GPU modes | Mesa/minigbm/DRM-HWC Virgl GLES path; no Vulkan feature |

Reuse the Cuttlefish/Mesa software paths for minigbm allocation, mapper,
DRM-HWC, and Virgl. Reuse Virtio input and output-only Virtio Sound support.
Declare no microphone, camera, Vulkan, Wi-Fi, Bluetooth, GNSS, telephony, UWB,
or sensors.

Use software/nonsecure implementations only where Android requires a service
to boot, such as the initial gatekeeper, KeyMint, health, power, and thermal
stubs. Document their non-production security properties and do not expose
hardware-security feature declarations.

### Disk and boot artifacts

The accepted schema-2 release uses one raw GPT UBD disk containing statically
addressed partitions for:

- `system`;
- `system_ext`;
- `product`;
- `vendor`;
- `odm`;
- `metadata`;
- writable `userdata`;
- `init_boot`;
- writable `misc`; and
- stock AOSP `recovery`.

Immutable partitions and APEX payloads use read-only ext4; writable state also
uses ext4. EROFS and F2FS are not used by this Android guest image. Disable
dynamic-super, Virtual A/B, OTA, and production AVB policy until basic boot,
application execution, and clean shutdown are accepted.

Keep `vmlinux` separate from Android boot containers. The producer composes
the generic and vendor first-stage content, wraps the normal ramdisk in a
kernelless boot-image-v4 `init_boot` partition, and places AOSP's stock recovery
ramdisk in a kernelless boot-image-v4 `recovery` partition. The runtime verifies
the selected header and passes its bounded GPT byte range through
`initrd_offset=` and `initrd_size=` with `init=/init`; no standalone ramdisk is
published and the kernel is not asked to parse a complete Android boot image.

The published disk is a pristine base with empty writable-state templates.
Runtime provisioning creates a per-instance COW file whose immutable backing
is that already-built disk; it does not repartition or rebuild the image. The
imported release is never opened writable. Subsequent boots reopen the same
COW, whose receipt binds it to the base release ID and partition layout.
Resetting an instance explicitly discards and recreates only that COW from the
same accepted base.

The UBD-aware fstab, init properties, ueventd rules, SELinux policy, VINTF
manifests, compatibility matrices, HAL declarations, and Android property set
are patch-owned AOSP source under `android/aosp-17/patches`. The external disk
layout, release-time kernel command line, release schema, and verification
policy live under `android/aosp-17/support`. Nothing in this list is stored as
Android support under `kernel/android-6.18`. The Android property set selects
the ACK memfd path with `sys.use_memfd=true`; the ACK ashmem compatibility
behavior remains available for legacy applications.

### Android build verification

The builder-local AOSP gate requires:

- `atest elf_alignment_test`;
- AOSP's 16 KiB prebuilt-library checker;
- no 32-bit ABI or 32-bit native library in the selected product;
- a complete VINTF match for the project-owned UML vendor definition and ACK
  6.18 kernel identity;
- compiled SELinux policy with `policycap memfd_class`;
- filesystem and partition-layout checks against the checked support file;
- no undeclared hardware feature XML;
- a target-files/image inventory containing no DLKM or 4 KiB kernel artifact;
- exact replayed final trees for every route in
  `android/aosp-17/patches/projects.tsv`; and
- no untracked Repo-root file, no changed project absent from `projects.tsv`,
  and no source file copied from `android/aosp-17/support`.

## Published release contract

### Builder-local release layout

Publish a compact runtime release, not the AOSP or kernel object trees:

```text
android17-ack6.18-16k-<release-id>/
  release-manifest.json
  kernel/
    vmlinux
    config
    System.map
    modules.builtin
    modules.builtin.modinfo
  boot/
    kernel-command-line.txt
  guest/
    android-17-uml-arm64.img.zst
    partition-layout.json
  support/
    fstab.uml
    vintf-manifest.xml
    compatibility-matrix.xml
  provenance/
    aosp-repo-manifest.xml
    build-receipt.tsv
```

Keep target-files archives, symbols, object directories, AOSP source trees,
ACK source trees, intermediate boot images, and full logs on the builder. They
are not emulator dependencies and are not part of the release directory copied
back to the workstation.

`modules.builtin` and `modules.builtin.modinfo` are read-only evidence of code
already linked into `vmlinux`; they are not loadable-module artifacts. The
publisher rejects `.ko` or `.ko.*` files, `modules.dep*`, `modules.alias*`,
`modules.load*`, DLKM images, and any other input intended for runtime module
discovery or loading.

### Manifest contents

The versioned manifest records:

- release ID and product family `android-17-ack-6.18-16k`;
- Android major, build ID, build variant, fingerprint, and vendor API level;
- AOSP manifest tag and pinned `repo manifest -r` identity;
- `android/aosp-17/patches/projects.tsv` identity plus every routed AOSP
  project's base commit/tree, ordered-series identity, and final tree;
- external Jelly repository, branch, pinned commit/tree, local final
  commit/tree and ordered-series identity;
- ACK repository, tag, base commit/tree, local final tree, and patch-manifest
  identity;
- outer-project revision and dirty-state observation;
- page size `16384`;
- common host/kernel ABI version and fingerprint;
- kernel release and ELF role;
- selected product and configuration identities;
- an exact inventory of published relative paths, roles, expected formats, and
  observed byte sizes;
- exact builder-local producer entry point and functional options, with local
  filesystem paths recorded only by role; and
- builder OS, JDK, LLVM, linker, SDK/sysroot, and Android build-tool versions.

The manifest and receipt do not record a builder hostname, user, port, home
directory, connection option, credential, or transport command.

Git commits, patch identities, configuration, and procedure are the source
reproducibility contract. The manifest records real build IDs and file sizes as
observations, but does not turn the product into a recursive hash inventory or
require an independent rebuild to be byte-identical.

An accepted publication requires clean outer-project, AOSP, and ACK source
states matching the recorded identities. Developer builds may record a dirty
state for diagnosis, but the publisher labels them non-acceptance releases and
cannot update the checked-in acceptance descriptor to select one.

A checked-in acceptance descriptor records the accepted release ID, source
identities, page size, product family, and common ABI identity, not binary
digests or the large artifacts. Publishing a release and accepting it into the
Apple product are separate review steps.

### Operator transfer and local installation

Once builder-local publication succeeds, the operator copies the complete
release directory to a fresh workstation-local incoming directory. The
repository neither chooses nor implements that transport. No tracked script
accepts a hostname, remote URI, account, port, SSH configuration, or credential,
and no tracked script starts a remote shell or file-transfer process.

The explicit local installer then:

1. accepts only a local filesystem source path and validates the release ID;
2. reads the already-copied manifest from that directory;
3. requires its identity and semantic fields to match the checked-in acceptance
   descriptor;
4. copies the exact declared payload into a fresh staging directory under
   `.build/android/releases`;
5. rejects undeclared files, symlinks, special files, absolute paths, path
   traversal, remote-URI syntax, and incomplete input;
6. verifies the declared inventory and observed sizes;
7. repeats portable ELF, ABI, config, ramdisk, and image-layout checks;
8. expands the disk image into the final local release only after its archive
   integrity and declared filesystem/image structure validate; and
9. atomically renames the completed staging directory and refuses to replace an
   existing release.

The installer does not modify another local release or make a partial release
visible. Deleting or replacing an installed release is a separate explicit
maintenance operation. Transport authentication and integrity belong to the
operator-managed environment outside this repository; the repository trust
boundary begins with the selected local incoming directory.

In accordance with the repository's lean workflow policy, this plan does not
add a product hash manifest. References to an "exact" ELF below mean that the
Apple carrier is compared byte-for-byte with the already-installed local ELF
it was configured to consume, not that independent producer builds must be
byte-identical.

## Apple build consumption

### Import-only kernel CMake

Replace the current in-CMake Kbuild graph with an import-only artifact owner.
`kernel/CMakeLists.txt` and `kernel/common/artifacts/KernelArtifact.cmake`
perform only local, read-only validation and expose target-specific imported
artifact dependencies.

Use one release root or manifest cache variable per family rather than
independently overridable ELF and support paths, for example:

```text
UML_MAINLINE_618_KERNEL_ARTIFACT_MANIFEST
UML_ANDROID17_RELEASE_DIR
```

The helper derives every input path from the validated manifest. It never
declares `vmlinux`, ramdisk, disk, or support files as outputs of a custom
command.

Remove these operations from every Apple configure/build:

- source-location parsing;
- Git source verification;
- patch replay and delta audit;
- compiler/toolchain selection for Kbuild;
- kernel configuration and Kbuild;
- SSH, scp, sftp, rsync, or remote path discovery; and
- AOSP/Soong/image construction.

A missing, stale, wrong-family, wrong-page-size, wrong-architecture, or
ABI-incompatible artifact is a clear configuration error.

Machine-specific artifact paths belong in ignored local configuration or
`CMakeUserPresets.json`, not in checked presets. Checked presets select a
product family and enforce its properties.

### Current products

Preserve the existing output names and behavior:

- `uml-darwin` continues to embed the current mainline kernel artifact;
- `KernariumRuntime_<release>_16K` remains the current 16 KiB framework;
- `KernariumRuntime_<release>_4K` remains the current compatibility framework;
  and
- the packaging script continues to require both current page profiles. It now
  also requires the two Android 16 KiB slices, because the application embeds
  all three runtime families; a missing Linux slice still fails packaging
  exactly as before.

Their standalone kernel producer emits the same manifest contract after
building. Moving kernel sources and decoupling CMake must pass the current 16
KiB and 4 KiB workflows before Android product work is merged into the Apple
graph.

### Android macOS executable

Add a distinct application target and output:

```text
CMake target:  uml-darwin-android17
binary:        bin/uml-darwin-android17
preset:        macos-android17
```

The target:

- hard-requires `UML_GUEST_16K_PAGES=ON`;
- imports only the accepted Android release;
- embeds `kernel/vmlinux` in its own Mach-O ELF carrier;
- uses an Android-specific launcher/composition source rather than inheriting
  the current `rootfs.erofs` discovery and desktop OverlayFS bootstrap;
- records the Android release identity in its build metadata;
- accepts the imported disk/ramdisk release at runtime and refuses a mismatched
  release; and
- has no dependency on `uml_kernel`, an ACK checkout, or a builder build target.

Factor common executable construction only after preserving target-specific
boot policy. Host GPU, input, audio, network, CPU, memory, and emulator
libraries may be reused; Android disk discovery, kernel arguments, serial/ADB
policy, and shutdown handling remain owned by the Android application.

### Android runtime framework

Delivered, with the naming and packaging decisions made differently from this
section's original proposal. The accepted contract is:

```text
CMake target:  KernariumRuntime, with UML_KERNEL_PRODUCT=android-17
framework:     KernariumRuntimeAndroid_<ack-release>_16K
presets:       kernarium-ios-device-android
               kernarium-ios-simulator-android
binary dirs:   .build/kernarium-runtime-ios-{device,simulator}-android-16k
package root:  .build/kernarium-xcframeworks, shared with the Linux runtimes
```

The guest flavour leads the framework name and the page profile trails it.
A trailing marker was rejected because it would leave the Android framework
matching the Linux page-profile suffix as well; with the flavour leading,
neither `KernariumRuntimeAndroid_` nor `KernariumRuntime_` is a prefix of the
other, so the loader's candidate set stays unambiguous with all three frameworks
embedded.

The Android framework:

- has Device and Simulator slices, both 16 KiB only, and refuses to configure
  against a 4 KiB guest profile;
- embeds the exact imported Android `vmlinux` in its aligned signed
  `__TEXT,__uml_elf` carrier, and its release manifest in
  `__DATA,__uml_android`;
- runs the existing signed-carrier safety verifier;
- consumes the verified base GPT from the accepted release and selects its
  verified normal or recovery ramdisk range rather than rebuilding or
  embedding either payload;
- shares the runtime composition source with the Linux runtimes, branching on
  the guest flavour rather than duplicating it; and
- has its own output name, bundle identifier, and module map.

`Kernarium/scripts/package-ios-frameworks.sh` now requires six runtime slices
rather than four and publishes three runtime XCFrameworks. The application
embeds all three and links none at launch, loading exactly one for the process
lifetime. The guest-family selector this section had deferred is implemented:
the rootfs picker offers `.kern` bundles alongside Linux images, and selecting
an Android bundle fixes the page-size picker at 16 KiB. A separate Android app
target was not created and is not planned.

### Android boot and management

The Android launch profile supplies the GPT as both the UBD backing and initrd
host file, the selected verified `init_boot` or `recovery` byte range,
`init=/init`, `tty0`, `ttyS0`, the project-owned Android boot properties, and
the selected SELinux enforcing policy. It does not run the current Linux
desktop overlay shell.

Initial diagnostics use `ttyS0`. ADB over TCP reaches the guest through a
local SLiRP host forwarding path, which was required before application-install
acceptance precisely because `adbd` starting inside the guest is not evidence
that ADB is reachable. `--net-forward=5555-5555` carried a verified 512 MiB
`adb push` at 14.3 MB/s with the SHA-256 matched.

The raw disk is a runtime resource and is not linked into the executable or
framework. On iOS it lives inside a `.kern` guest bundle in the app's Documents
space, and UBD layers a per-bundle `userdata.cow` over it; on macOS the Android
launcher receives a validated per-instance disk and its release receipt. Neither
product boots the imported base disk writable.

An iOS guest must survive being abandoned. `uml_emulator_sync()` asks the guest
to write back through its management console and then flushes the host's files;
`uml_emulator_quiesce()` additionally freezes the guest between the two, and
`uml_emulator_resume()` thaws it. Android's init disables magic SysRq, so
Kernarium boots its guests with `sysrq_always_enabled` and reports a refused
request rather than assuming it succeeded. Do not substitute a synchronous
`/data` mount for this: the mount options are owned by `support/fstab.uml`
inside the vendor image, and the measurements in the 2026-08-24 handoff record
what write-through would cost.

## Implementation phases

### Phase 0: prove the Linux-hosted ACK UML cross-build

Deliverables:

- pinned ACK metadata;
- minimal ACK-based Darwin UML local topic series;
- x86_64-host/AArch64-target compiler separation;
- explicit Darwin SDK/sysroot input;
- builder-local kernel command owned by
  `android/aosp-17/build/build-kernel.sh`; and
- one standalone 16 KiB ET_DYN `vmlinux` plus config/import verification.

Exit criteria:

- build occurs on the dedicated Linux machine;
- no AOSP build has yet been required;
- ELF architecture, type, alignment, signed-carrier layout, and ABI imports
  pass;
- the resolved kernel page size is 16 KiB;
- the build system has no Android 4 KiB path;
- the reviewed scripts and kernel inputs were copied to the machine before
  execution; and
- no tracked script knows or contacts the build-machine endpoint.

Stop here if the designated Linux toolchain cannot reproduce the Darwin UML
ELF contract. Resolve that locally before starting the multi-hour AOSP lane.

### Phase 1: split kernel and Android ownership without changing products

Deliverables:

- `kernel/common`, `kernel/mainline-6.18`, `kernel/android-6.18`, and
  `android/aosp-17` layout;
- current patch/config/build material moved to the mainline family;
- common ABI and parameterized tooling moved to common ownership;
- the mainline family's ignored source configuration, isolated per-line cache
  roots, and builder-local Android/ACK path inputs;
- updated tests, `.gitignore`, and `.gitattributes` paths;
- updates to `AGENTS.md`, `docs/architecture.md`, `docs/testing.md`,
  `docs/REFACTOR_HANDOFF.md`, `docs/REFACTOR_PROGRESS.md`, `darwin/README.md`,
  the new `kernel/README.md` and `android/README.md` indexes, and each versioned
  line's README so active guidance names the new paths and staged transition;
  and
- preservation of explicitly dated historical sections as history rather than
  rewriting them into current instructions.

Exit criteria:

- current upstream replay and both standalone page builds pass;
- generated current patch bytes and final tree are unchanged by the move;
- one line's replay/regenerator cannot touch the other line;
- ABI generated copies verify from the new common path;
- no old tracked path remains as a second source of truth;
- `kernel/android-6.18` contains no AOSP patch, Android build script, image
  layout, release tool, or non-kernel support file; and
- `android/aosp-17` contains every AOSP patch series and all Android-specific
  build and support files.

### Phase 2: make every emulator build prebuilt-only

Deliverables:

- common artifact schema and verifier;
- standalone current-kernel artifact publication;
- import-only root kernel CMake;
- current presets converted from `UML_KERNEL_BUILD_DIR` to validated artifact
  manifests;
- source-dependent kernel policy tests moved out of Apple configure/build; and
- the authoritative documents named in Phase 1 updated again to describe the
  final standalone-producer/import-only-consumer model.

Exit criteria:

- current macOS and framework builds succeed without a kernel source checkout,
  Kbuild toolchain, Git network, builder endpoint, or transport tool available;
- build graphs contain no command that produces a kernel ELF;
- current 16 KiB and 4 KiB products embed their matching imported artifacts;
- the architecture and documentation-path audits find no active instruction
  that still requires root `kernel/source-location.conf`, in-CMake Kbuild, or
  `/Volumes/<case-sensitive-volume>/build-{16k,4k}` as an Apple product input; and
- existing current workflows and packaging acceptance remain green.

### Phase 3: complete the ACK Android kernel line

Deliverables:

- full ACK-local Darwin UML series;
- Android-only kernel configuration;
- built-in Android subsystems and SELinux policy capability support;
- local delta/ownership/overlap metadata; and
- standalone builder-local kernel replay/build/verification invoked from the
  copied `android/aosp-17/build` workflow.

Exit criteria:

- exact ACK final tree and series policy pass;
- only one 16 KiB kernel artifact is produced;
- Binder, SELinux, `memfd_class`, ashmem compatibility, DMA-BUF, UBD, Virtio
  devices, namespaces, cgroups, and required filesystems verify built-in;
- `CONFIG_MODULES=n`, every required feature is `y`, no feature is `m`, and
  the build and release trees contain no loadable-module or DLKM payload;
- HostFS verifies disabled; and
- common ABI import verification passes.

### Phase 4: build the AOSP 17 UML product and disk

Deliverables:

- routed per-project patch metadata and generated series under
  `android/aosp-17/patches`;
- fresh-only `aosp_kernarium_darwin` product synchronized from the checked
  device overlay;
- overlay-owned UBD fstab/ueventd/init/feature/SELinux source;
- selected software HALs and accurate feature declarations;
- external image/release/verification inputs under `android/aosp-17/support`;
- partition-backed normal and stock recovery ramdisks in one ten-partition GPT
  disk; and
- Android page-alignment and image-layout verification.

Exit criteria:

- AOSP build passes on the dedicated machine;
- all selected userspace native ELF files pass 16 KiB alignment checks;
- no 32-bit ABI, DLKM partition, module-loading service, or 4 KiB kernel
  artifact is present;
- every routed AOSP project starts at its pinned base and finishes at its
  expected clean final tree;
- no changed AOSP project is absent from `projects.tsv` and no support overlay
  is copied into the source tree;
- no Android build/support ownership remains under `kernel/android-6.18`;
- the disk and both bounded ramdisks validate against the checked support
  contract.

### Phase 5: publish, copy, and locally install an Android release

Deliverables:

- atomic builder-local publisher;
- versioned release manifest and checked-in acceptance descriptor;
- an operator-performed copy outside repository scripts; and
- a local-path semantic installer with no network or builder-endpoint input.

Exit criteria:

- a complete already-copied incoming release installs into
  `.build/android/releases`;
- partial, structurally or semantically altered, wrong-family, wrong-page,
  path-traversal, symlink, and undeclared-file fixtures fail;
- a second installation cannot overwrite the first;
- CMake can validate the local release with the Linux builder offline; and
- tracked scripts contain no workstation-to-builder remote-shell/file-transfer
  command, endpoint, account, credential, or remote URI handling.

### Phase 6: add the dedicated macOS executable

Deliverables:

- `uml-darwin-android17` target and preset;
- Android launch profile and bundle validation;
- Android disk, ramdisk, serial, display, input, sound, and network wiring; and
- product-isolation tests.

Exit criteria:

- executable embeds the accepted Android ACK ELF and no current upstream ELF;
- current `uml-darwin` embeds no Android ELF;
- Android configuration fails when 4 KiB is selected;
- Android reaches first-stage init and a serial shell; and
- current integration products remain unchanged.

### Phase 7: add the dedicated runtime framework

Complete as of 2026-08-24, under the naming recorded in the "Android runtime
framework" section rather than this phase's original proposal.

Deliverables:

- Device and Simulator Android runtime slices;
- `KernariumRuntimeAndroid_<ack-release>_16K.xcframework`;
- Android runtime API identity and loader flavour matching; and
- signed ELF carrier and release-resource consumption.

Exit criteria:

- both framework slices contain the exact accepted ELF;
- carrier verification and package-shape tests pass;
- physical-iOS execution requires no JIT entitlement, anonymous executable
  allocation, RW-to-RX publication, or module-text relocation outside the
  signed immutable carrier. This is structurally true because the ACK ELF uses
  the same signed carrier as the Linux runtimes, but it has not been exercised
  on a physical device with an Android guest;
- framework discovery cannot collide with current `KernariumRuntime_*` names;
- no Android 4 KiB framework exists; and
- current dual-profile framework packaging still passes independently.

### Phase 8: Android boot and application acceptance

Partly complete. Boot, graphics, input, and ADB are delivered; APK acceptance
and the registered integration workflow are not.

Deliverables:

- enforcing Android boot - **done**, first at `20260822-28ce51ba-mm21`;
- functional SurfaceFlinger/launcher on Virgl GLES - **done**, with the
  launcher rendered on the Simulator under Kernarium;
- ADB-over-TCP host forwarding - **done**, verified by a 512 MiB push;
- APK install/run automation - **outstanding**; and
- Android-specific integration workflow - **outstanding**; no Android guest
  test is registered in `tests/test-manifest.tsv`, whose Android entries are
  all builder-lane and release-contract tooling.

Exit criteria:

- Android reports major version 17 - met;
- `getconf PAGE_SIZE` reports `16384` - met;
- SELinux reports enforcing and `memfd_class` enabled - enforcing is confirmed
  in every accepted boot;
- Binder service discovery works - met; servicemanager, zygote64,
  surfaceflinger, and system_server all stay at their first pids;
- boot-complete, clean shutdown, and restart pass - boot-complete and restart
  are met, including restart from a persistent `.kern` instance disk;
- Virtio GPU/input, output-only sound, and Ethernet are exercised - GPU, input,
  and Ethernet are met; sound is not exercised;
- one Java/Kotlin APK and one properly aligned `arm64-v8a` native APK install
  and run - not attempted; and
- the same accepted disk boots in the dedicated macOS product and the Android
  runtime framework - met on macOS and on the iOS Simulator, not on a physical
  device.

## Verification matrix

### Host-only tooling

Add or update tooling tests for:

- exact directory ownership and absence of duplicate old paths;
- `kernel/android-6.18` containing only ACK documentation, configuration,
  metadata, and patches, with Android build/support ownership confined to
  `android/aosp-17`;
- upstream source-configuration, builder-local Android/ACK path, and cache
  isolation;
- parameterized patch-series verification and write isolation;
- AOSP `projects.tsv` routing, exact per-project base commit/tree, ordered
  series manifest, final tree, path ownership, and cross-route write isolation;
- proof that every changed AOSP project is routed, that no product source is
  copied from `support`, and that the Repo checkout has no untracked root file;
- common ABI path, identity, generated-copy, and per-line import checks;
- Android lane references to the shared 16 KiB fragment and no reference to a
  4 KiB fragment;
- release-manifest schema, inventory, path safety, size, and semantic
  validation;
- accepted-release descriptor validation;
- structural scans proving tracked Android scripts contain no
  workstation-to-builder remote-shell, SSH-family, or file-transfer invocation
  and accept no hostname, account, credential, remote URI, port, or connection
  option;
- local-installer tests proving it accepts only an already-copied filesystem
  directory and performs no network or endpoint discovery;
- CMake graph inspection proving no Git, Kbuild, Soong, SSH, scp, sftp, rsync,
  remote path, or artifact-producing command is reachable from an Apple
  target;
- distinct current and Android executable/framework target names;
- current 4 KiB/16 KiB and Android 16 KiB artifact-to-target binding; and
- negative cases for wrong family, Android major, ACK line, architecture, page
  size, ABI, release ID, and carrier layout.

Update `tests/test-manifest.tsv` and keep these checks in the `tooling` suite.
Actual guest boots remain `integration`.

### Standalone producer gates

The upstream line retains its 4 KiB and 16 KiB replay/build gates. The Android
line has a separate 16 KiB-only producer gate. Neither is registered as an
Apple CMake build dependency.

The Android producer records and checks:

- AOSP and ACK source identities;
- outer-project revision and dirty observation;
- ACK patch replay/final tree;
- every routed AOSP project's base identity, ordered patch series, clean final
  tree, and absence of changes outside `projects.tsv`;
- resolved kernel configuration;
- `CONFIG_MODULES=n`, an all-`y` required-builtins set, absence of every `=m`
  value, and absence of `.ko`, module-loader, and DLKM release inputs;
- kernel ELF and common ABI imports;
- AOSP 16 KiB native-library alignment;
- VINTF and SELinux policy;
- disk/ramdisk structure; and
- exact published runtime inventory.

### Apple product gates

For `uml-darwin-android17` and both framework slices:

- inspect the embedded carrier and compare it with the imported release;
- verify release identity and common ABI;
- run the signed-carrier verifier for frameworks;
- verify runtime dependency closure and package shape;
- assert the current and Android products do not cross-embed artifacts; and
- run architecture audit and focused loader sanitizer coverage.

Current acceptance continues to include both current page profiles. Android
acceptance includes only its one 16 KiB profile.

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| ACK 6.18 conflicts materially with the current Darwin UML series | Phase 0 ports the smallest executable slice first and records a separate ACK series; do not copy current patch bytes. |
| Linux-hosted Darwin UML cross-build accidentally uses Linux host ABI | Separate `HOSTCC` and AArch64 target compilers, require an explicit Darwin SDK/sysroot, and inspect every final undefined import and ELF property. |
| AOSP build time hides a kernel/toolchain failure | Complete Phase 0 before starting AOSP. |
| CMake regains a hidden source/build dependency | File-API and source audits reject Kbuild, SSH, Git, Soong, or artifact-producing commands in Apple targets. |
| A required Android driver silently becomes a module | Resolve every symbol in `required-kernel-builtins.txt` to `y`, require `CONFIG_MODULES=n`, reject every `=m` value and module-loading artifact/action, and prove the signed `vmlinux` is the complete executable kernel closure. |
| An AOSP patch is applied to the wrong Repo project | Route patches by opaque ID, verify the manifest project/path and exact base before replay, and require the recorded clean final tree. |
| Product files bypass patch ownership through a copied overlay | Structural and post-build audits reject support-tree staging, untracked Repo-root files, and changed projects absent from `projects.tsv`. |
| The operator copies a partial or wrong release from the builder | Publish atomically on the builder and require the local installer to reject incomplete, undeclared, wrong-family, and semantically invalid incoming directories. |
| An ELF is mixed with another release's boot payload or disk | Accept one manifest/release root, derive the GPT and verified ramdisk ranges internally, and embed/check the release identity. |
| Current and Android frameworks collide in loader discovery | Lead the framework name with the guest flavour, `KernariumRuntimeAndroid_` against `KernariumRuntime_`, so neither is a prefix of the other, and match the loaded runtime's reported flavour and name before using it. |
| Cuttlefish declares devices the emulator does not implement | Own feature XML and VINTF through the routed AOSP patches; omit unsupported services and declarations. |
| Android native apps contain 4 KiB-only libraries | Enforce AOSP alignment checks and include a native `arm64-v8a` APK in acceptance. |
| ADB starts but is unreachable from the host | Add and test an explicit local SLiRP forwarding path before APK-install acceptance. |
| Large builder artifacts leak into Git or routine builds | Publish only the compact runtime inventory, copy it through the operator-managed workflow, install under `.build`, and keep producer object trees on the Linux builder. |

## Completion definition

The plan is complete when:

1. `kernel/common`, `kernel/mainline-6.18`, and `kernel/android-6.18` are the
   only kernel sources of truth, while `android/aosp-17` exclusively owns AOSP
   patch series and Android build/support files;
2. all emulator and framework builds consume validated prebuilt artifacts and
   contain no kernel/image producer edge;
3. the dedicated x86_64 Linux builder reproducibly prepares pinned AOSP 17 and
   ACK 6.18 sources, replays the separate routed AOSP and ACK patch series, and
   publishes one verified 16 KiB Android release using only scripts copied to
   and executed locally on that machine;
4. no tracked file contains a build-machine endpoint or credential and no
   tracked script initiates execution on or transport to or from that machine;
5. `uml-darwin-android17` and
   `KernariumRuntimeAndroid_<ack-release>_16K` are separate accepted products;
6. the Android ACK configuration has `CONFIG_MODULES=n`, every required kernel
   feature is built in, and the release/runtime has no loadable-module, DLKM,
   or JIT-dependent kernel-code path;
7. Android 17 boots enforcing with a real 16 KiB page size and runs both
   managed and native ARM64 applications; and
8. the existing mainline 16 KiB and 4 KiB products retain their names,
   artifacts, workflows, and accepted behavior.

## Primary external references

- [AOSP Android 17.0.0 release manifest](https://android.googlesource.com/platform/manifest/+/refs/tags/android-17.0.0_r1)
- [ACK Android 17 / 6.18 release builds](https://source.android.com/docs/core/architecture/kernel/gki-android17-6_18-release-builds)
- [ACK `android17-6.18-2026-06_r35` source](https://android.googlesource.com/kernel/common/+/refs/tags/android17-6.18-2026-06_r35)
- [AOSP 16 KiB page-size guidance](https://source.android.com/docs/core/architecture/16kb-page-size/16kb)
- [Cuttlefish Android 17 source tree](https://android.googlesource.com/device/google/cuttlefish/+/refs/heads/android17-release/)
- [AOSP build-host requirements](https://source.android.com/docs/setup/start/requirements)
