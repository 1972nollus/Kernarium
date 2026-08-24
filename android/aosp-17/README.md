# AOSP 17 UML guest producer

This directory owns the standalone AOSP 17 producer lane for the native ARM64
Darwin UML guest. It starts from the exact source identities in
[`metadata/aosp.env`](metadata/aosp.env) and
[`../../kernel/android-6.18/patches/metadata.env`](../../kernel/android-6.18/patches/metadata.env).

The first producer milestone prepares and validates existing builder-local Repo
workspaces:

```sh
android/aosp-17/build/prepare-sources.sh \
  --aosp-source /absolute/path/to/aosp \
  --ack-workspace /absolute/path/to/ack-workspace

android/aosp-17/build/preflight.sh \
  --aosp-source /absolute/path/to/aosp \
  --ack-workspace /absolute/path/to/ack-workspace \
  --darwin-sdk /absolute/path/to/MacOSX.sdk \
  --work-root /absolute/path/to/work \
  --release-root /absolute/path/to/releases
```

Materialize and build the tracked ACK series in a separate worktree:

```sh
android/aosp-17/build/apply-kernel-patches.sh \
  --ack-workspace /absolute/path/to/ack-workspace \
  --kernel-source /absolute/path/to/new-android-kernel-source

android/aosp-17/build/build-kernel.sh \
  --ack-workspace /absolute/path/to/ack-workspace \
  --kernel-source /absolute/path/to/new-android-kernel-source \
  --darwin-sdk /absolute/path/to/MacOSX.sdk \
  --build /absolute/path/to/kernel-build
```

After changing or appending commits on a clean linear ACK topic checkout,
regenerate the complete kernel mail series from that Git history on the Linux
builder:

```sh
android/aosp-17/tools/regenerate-kernel-patches.py \
  --android-root /absolute/path/to/copied/repository/android/aosp-17 \
  --line-root /absolute/path/to/copied/repository/kernel/android-6.18 \
  --topic-checkout /absolute/path/to/clean-ack-topic
```

The command is structurally limited to `kernel/android-6.18` in the same
copied repository, rewrites final-tree and patch-count metadata together with
the generated series, and runs the common manifest and topology verifiers
before exposing the result. New ACK topics require the
`UML-DARWIN-LOCAL:` subject prefix. Never edit generated kernel patch bytes.

Apply the generated per-project AOSP series and build the initial 16 KiB,
ARM64-only product composition:

The builder host must provide `zip` and `rsync` for AOSP packaging. Install
both before invoking the producer; the tracked producer scripts do not invoke
transport tools themselves.

```sh
android/aosp-17/build/apply-aosp-patches.sh \
  --aosp-source /absolute/path/to/aosp

android/aosp-17/build/build-aosp.sh \
  --aosp-source /absolute/path/to/aosp \
  --out /absolute/path/to/aosp/out-uml-android17 \
  --kernel-elf /absolute/path/to/kernel-build/vmlinux
```

Ninja is the canonical build backend for this producer. Do not switch build
systems when diagnosing a rebuild or continuing incremental work. Preserve the
output directory after the baseline build and pass the exact same `--out` path
to later invocations so Ninja reuses all valid Android outputs. A new builder
needs no special bootstrap option: a missing output path is created
automatically. The wrapper does not force `SOONG_INCREMENTAL_ANALYSIS`; the
pinned Android environment owns that setting so subsequent builds keep using
the analysis mode already associated with the preserved output tree.

[`metadata/base-manifest.xml`](metadata/base-manifest.xml) locks every resolved
Repo project. [`patches/projects.tsv`](patches/projects.tsv) routes each changed
project from its exact base commit and tree to its expected final tree. Files
destined for the AOSP checkout, including the UML product definition, are
introduced only by generated `git format-patch` series under `patches/projects`;
`support/` is never copied over an AOSP project.

After amending a clean, linear per-project topic branch, regenerate its patch
bytes and all routed final-tree/count/manifest identities together:

```sh
android/aosp-17/tools/regenerate-aosp-patches.py \
  --android-root /absolute/path/to/copied/repository/android/aosp-17 \
  --route-id p0001 \
  --topic-checkout /absolute/path/to/project-topic
```

The topic must descend directly from the route's exact base commit, every
subject must start with `UML-ANDROID-LOCAL:`, and the worktree must be clean.
Never edit the generated patch bytes or either identity table by hand.

After packaging, the builder verifies that `BOOT/kernel`, the packaged kernel
configuration, and the packaged kernel release exactly match the selected ACK
build. It rejects DLKM and loadable-module payloads, rejects 32-bit target ABIs,
checks 16 KiB load alignment for target executables, requires Android 17's
`memfd_class` SELinux policy capability, and runs AOSP's packaged target-files
VINTF checker against the exact kernel metadata. The companion `.config` and
`include/config/kernel.release` are read from the selected ELF's build
directory unless the verifier receives explicit paths.

`prepare-sources.sh` is the only producer command in this lane that may access
the public source network. It moves clean builder-local workspaces to the pinned
AOSP manifest and ACK commit. All later commands operate offline.

No tracked command connects to or transfers files to another machine. Copy a
reviewed outer-project snapshot to the builder before invoking these scripts,
then copy completed releases back through an operator-managed workflow outside
the repository.

The Android kernel has one profile: 16 KiB, ARM64-only, with
`CONFIG_MODULES=n`. The build rejects any `=m` configuration or `.ko` output
and verifies the built-in Android feature set, ELF geometry, and exact Darwin
bridge imports. The AOSP product disables system, vendor, and ODM DLKM images,
clears every inherited Cuttlefish module list, and rejects a module-bearing
configuration before compilation. A UML-specific vendor init file never starts
the Cuttlefish DLKM loader or invokes `modprobe`. It also disables Cuttlefish's
Trusty security VM before inheritance and retains only the explicitly nonsecure
software KeyMint and gatekeeper APEXes; the build rejects remote or
guest-Trusty alternatives. AOSP image assembly, release publication,
and Apple consumers remain separate from the host build. The current product is
still an initial page-size and ABI composition based on Cuttlefish; standalone
UML storage, boot, VINTF, SELinux, and HAL adaptation remain later milestones.

## Static UBD disk contract

`support/image/partition-layout.json` owns the first fixed GPT layout. It has
separate `system`, `system_ext`, `product`, `vendor`, `odm`, `metadata`, and
`userdata` partitions; it deliberately has no `super` or DLKM partition. The
kernel ELF and Android ramdisks remain separate release artifacts and are not
placed in this disk.

The routed UML product selects static storage before inheriting the shared
Cuttlefish products. This prevents their dynamic-partition and Virtual A/B
inheritance, properties, postinstall configuration, and OTA-only packages from
entering the resolved product. It also disables A/B OTA packaging and AVB.
The unused `system_other` image is disabled because this product has no second
slot and does not copy preoptimized artifacts from one.
`build-aosp.sh` rejects any regression in those switches and requires every
resolved partition capacity to match the JSON layout. AOSP core still assigns
its default `PRODUCT_VIRTUAL_AB_COW_VERSION` value after product evaluation;
that inert format default is not an enablement signal when
`PRODUCT_VIRTUAL_AB_OTA=false`.

Route `p0002` carries one narrow `platform/build` correction for this explicit
OTA-disabled composition: when `TARGET_SKIP_OTA_PACKAGE=true`, target-files no
longer depends on or copies the legacy non-A/B `updater` binary. This keeps the
useful target-files verification input without manufacturing an OTA path for a
product that cannot consume one.

Route `p0001` also omits the inherited BootControl APEX and recovery service for
this non-A/B product. Its fixed userdata fstab has no checkpoint flag, so vold
does not wait for a slot-based BootControl service that the static GPT cannot
provide. The builder and target-files verifier reject either package, the APEX
payload, or a checkpoint-bearing fstab if those contracts regress.

The same route disables project-quota provisioning and omits the userdata
`quota` fstab flag. Both a staged ext4 image and a freshly formatted encrypted
`dm-default` device produced invalid hidden quota inodes under the 16 KiB ACK
UML slice. The complete kernel quota subsystem remains built in, but this guest
does not activate filesystem quotas; Android falls back to non-quota storage
accounting. The target-files and initramfs verifiers pin that mount policy.

Route `p0001` also replaces the inherited dynamic/virtio fstab with
`fstab.uml`. Android first-stage init discovers the fixed GPT partitions below
the `uml-blkdev.0` platform device and mounts the immutable filesystems
directly from `/dev/block/by-name`; there are no logical, slot-select, AVB, or
DLKM mount flags. The UML init and ueventd inputs keep the first-stage mount,
late writable-state mount, serial console, and bugreport paths while removing
Cuttlefish-only multiport HVC, vsock, Wi-Fi, custom-partition,
persistent-memory, and suspend actions. The UML product labels its sole
single-port `/dev/hvc0` as a serial device and grants the shell access through
its patch-owned ueventd policy. Soong disables the inherited stock fstabs and
serial-logging file even though the base product still contributes their
package names.
`verify-target-files.py` checks the installed result rather than trusting that
inherited package list.

Once the static AOSP product emits the declared image names, assemble and
immediately verify a raw UBD disk locally on the builder with:

```sh
android/aosp-17/build/assemble-image.sh \
  --target-files /absolute/path/to/aosp_uml_arm64_pgagnostic-target_files.zip \
  --kernel-elf /absolute/path/to/vmlinux \
  --output /absolute/path/to/release/android17-ubd.img
```

The wrapper revalidates target-files against the selected kernel, consumes its
adjacent `IMAGES` staging directory, and explicitly leaves `metadata` as the
zero-filled writable template that Cuttlefish normally creates at assembly
time. Every other partition requires its declared image. The disk assembler
accepts ordinary raw images and Android sparse images, expands payloads into a
sparse host file, rejects overflow and unsafe image paths, refuses to replace
an existing output, and writes matching primary and backup GPT metadata.
Recheck an existing image with the `verify` subcommand. The fixed partition
capacities are intentionally larger than the initial inherited Cuttlefish
dynamic groups; every concrete image must fit before publication.

## Standalone UML initramfs

The emulator consumes neither Android `boot.img` nor `vendor_boot.img`.
Compose the generic init-boot tree with only the patch-owned UML first-stage
fstab and emit an uncompressed newc archive:

```sh
android/aosp-17/build/assemble-ramdisk.sh \
  --target-files /absolute/path/to/aosp_uml_arm64_pgagnostic-target_files.zip \
  --kernel-elf /absolute/path/to/vmlinux \
  --mkbootfs /absolute/path/to/aosp/host/linux-x86/bin/mkbootfs \
  --output /absolute/path/to/release/ramdisk.img
```

The assembler removes generic `modprobe` and snapshot-userdata helpers before
packing because this guest has `CONFIG_MODULES=n`, no Virtual A/B snapshots,
and no loadable module path. `verify-initramfs.py` parses the raw newc stream,
requires executable `/init` and the exact first-stage fstab, and rejects
ambiguous duplicate files, unsafe paths, module payloads, and snapshot helpers.
The one harmless duplicate `dev` directory emitted by AOSP `mkbootfs` from its
tree and node list is accepted only when both directory records have identical
mode and zero size.

Current-candidate assembly requires exact byte equality with the current
patch-owned UML fstab. Accepted-runtime import uses the verifier's structural
mode instead: it still requires the regular first-stage fstab and every archive
safety/module exclusion, but preserves the fstab version reviewed in that
release rather than comparing it with a later producer revision.

`support/boot/kernel-command-line.txt` owns the machine-independent launch
arguments. It selects `/init`, the UML fstab suffix and boot device, enforcing
SELinux, and the DRM-HWC composer APEX for the Virtio-GPU path. That path uses
minigbm, Mesa OpenGL ES 3.0, DRM display discovery, RGBA framebuffers, and
SkiaGL HWUI. CPU and hardware Vulkan remain disabled because the emulator does
not implement the blob/shared-aperture and Venus facilities needed by Android
Vulkan. The profile disables both inherited camera-provider APEX variants
because UML exposes no camera and selects the single-port `hvc0` diagnostic
shell through `androidboot.console=hvc0`. The Linux console list contains only
`ttyS0`, so kernel and init diagnostics remain on the host log and never
contaminate the interactive shell or its `logcat` output. It also
sets the product-scoped
`androidboot.uml_bpf_interpreter=1` contract consumed by route `p0003` in
`platform/packages/modules/Connectivity`. Normal Android products retain
NetBpfLoad's eBPF-JIT requirement. This UML product may continue only when both
JIT sysctls are absent and the explicit boot property is present, using the
kernel's built-in verifier and generic interpreter instead. An inconsistent
lone kallsyms sysctl remains fatal. The same explicit property permits a zero
JIT-image length during post-load validation; program identity and translated
bytecode length remain mandatory. When the current loader would otherwise use
the callback/bpf-to-bpf `@funcs` objects, interpreter mode selects the shipped
inline `dscpPolicy.o` and `netd.o` variants instead; it does not omit those
networking programs. The runtime adds its local `ubd0=` disk and initrd paths;
no builder or workstation path is stored in this tree.

Route `p0007` breaks a framework boot dependency cycle without changing the
global WindowManager timeout. When the wallpaper service is available but has
not produced its initial color event, `ThemeInitializationObserver` marks the
theme unready and WindowManager consequently withholds boot completion. The
old no-color fallback ran only at `PHASE_BOOT_COMPLETED`, so it could never
release the gate it was waiting behind; a warm ten-vCPU UML boot therefore
continued only when WindowManager's 30-second emergency timeout, multiplied by
the product's hardware-timeout factor, forced the display on. The local route
moves that fallback to `PHASE_THIRD_PARTY_APPS_CAN_START` (phase 600), after the
ordinary color callback has had time to arrive but before boot completion can
depend on theme readiness. Its focused service test pins that phase 550 leaves
initialization pending and phase 600 releases it.

Route `p0004` carries the AOSP Mesa project changes for an ARM64 Virgl-only
Gallium build. It generates vendor `libEGL_mesa`, GLES 1 and GLES 2 libraries,
and `gallium_dri` from source with 16 KiB load alignment; it does not package
SwiftShader. Route `p0001` selects those modules only for the UML product and
installs `virtio_gpu_dri.so` as a symlink to `gallium_dri.so`, leaving other
Cuttlefish products on their existing graphics stack. The target-files
verifier requires all four AArch64 shared objects, checks every PT_LOAD segment
for 16 KiB alignment, and requires the exact DRI symlink before a candidate can
be published. Each Mesa EGL/GLES consumer must also retain a dynamic dependency
on `gallium_dri.so` and the unbraced `$ORIGIN/../dri` runpath so Android's
restricted `sphal` linker namespace can resolve that dependency from the DRI
directory.

This accommodation is not a claim of stock Android or VTS eBPF-JIT
compliance. ACK's native arm64 JIT depends on native CPU-capability, `pt_regs`,
Spectre, and exception-table contracts that the arm64 UML architecture does
not share. The producer therefore requires built-in `BPF`, `BPF_SYSCALL`, and
`CGROUP_BPF`, rejects `BPF_JIT`, `BPF_JIT_ALWAYS_ON`, and `HAVE_EBPF_JIT`, and
keeps the slower interpreter choice visible in both the boot property and log.

The 2026-08-22 builder checkpoint produced an OTA-disabled target-files archive
with 4,542 entries and 1,155 verified target executables, a 4,374,784-byte raw
initramfs with 35 unique entries, and a reverified seven-partition
17,179,869,184-byte GPT disk. The compressed transfer artifact is 830,866,283
bytes. AOSP's packaged VINTF checker accepted the target files and exact ACK
kernel metadata; `atest elf_alignment_test` built but correctly deferred because
it is a device-side test. It remains a post-boot gate. This proves the static
non-A/B build, VINTF, SELinux capability, fstab, initramfs, and image-layout
contracts; it does not yet claim an accepted HAL runtime.

## Local release installation contract

`support/release/release-manifest.schema.json` and `tools/release_common.py`
define the compact runtime release. The inventory is fixed to the ACK UML ELF
and built-in-module evidence, external initramfs and command line, compressed
raw GPT disk and layout, VINTF/fstab support, and compact source provenance.
The verifier rejects symlinks, special files, undeclared files or directories,
unsafe paths, wrong family/page/ABI/source identities, size changes, and a
kernel configuration with module support or any `=m` value.

After an operator copies a complete release directory to the workstation, use
only its local path:

```sh
android/aosp-17/tools/install-release.py \
  --source /absolute/local/path/android17-ack6.18-16k-release-id
```

The installer requires `metadata/accepted-release.env`, validates the copied
directory before writing, copies only declared regular files into a fresh
staging directory under `.build/android/releases`, expands the GPT archive as
a sparse local file, revalidates it, and atomically installs the result. It
refuses to replace an existing release. The acceptance descriptor is added only
when a clean builder-local publication has been reviewed. The current descriptor
selects `android17-ack6.18-16k-20260824-c3febf12-theme1`, published from
clean outer revision `c3febf1273e129f43305f3c7cfb9f0bf0a03c8ca` after the
static kernel, target-files, VINTF, SELinux, initramfs, disk, DRM-HWC,
no-camera APEX, non-A/B storage, built-in userdata-encryption, multi-vCPU timer
service, built-in single-port Virtio-console, and source-built 16 KiB Mesa
Virgl payload, DRI namespace-resolution, mixed-COW UBD, and warm-boot theme
gates passed.
This is artifact acceptance for Apple-product integration; HAL and application
acceptance remain separate later gates.

Producer verification and accepted-runtime verification are deliberately
separate. A new clean producer revision must match the current AOSP route table,
ACK series, kernel policy, boot command line, and partition layout before it can
publish a candidate. An already accepted runtime instead matches its checked-in
descriptor and the current common host/kernel ABI. It remains importable while
the next source candidate is built and reviewed; changing the producer route
table alone neither silently promotes that candidate nor invalidates the last
accepted runtime.

Apple consumers validate that installed directory without a builder or transfer
tool. The installed mode requires and checks the expanded GPT disk and all other
runtime payloads, while leaving the compressed transfer archive's integrity test
at the installer boundary:

```sh
android/aosp-17/tools/verify-release.py \
  --release /absolute/path/to/.build/android/releases/accepted-directory \
  --acceptance android/aosp-17/metadata/accepted-release.env \
  --installed
```

When the dedicated Android macOS product is selected, CMake performs this
installed-release validation during configure and exposes the accepted files
through one read-only `uml_android17_release` interface target. It never
declares a release payload as an output and has no source, Soong, Kbuild, or
transport operation.

Publish only from the clean copied Git checkout on the builder, after rebuilding
the disk and initramfs from the current target-files staging directory:

```sh
android/aosp-17/build/publish-release.sh \
  --aosp-source /absolute/path/to/aosp \
  --kernel-source /absolute/path/to/replayed-ack \
  --kernel-build /absolute/path/to/kernel-build \
  --target-files /absolute/path/to/target-files.zip \
  --ramdisk /absolute/path/to/ramdisk.img \
  --disk /absolute/path/to/android17-ubd.img \
  --darwin-sdk /absolute/path/to/MacOSX.sdk \
  --clang /absolute/path/to/pinned/clang \
  --linker /absolute/path/to/pinned/ld.lld \
  --vintf-checker /absolute/path/to/check_target_files_vintf \
  --zstd /absolute/path/to/zstd \
  --release-root /absolute/path/to/releases \
  --release-id reviewed-release-id
```

The publisher accepts only builder-local paths. It repeats ACK series/source,
kernel, target-files, VINTF, initramfs, and GPT checks, rejects any module output,
records only role-based input descriptions rather than builder paths, compresses
the disk with a frame checksum, verifies the completed staging tree, and then
atomically exposes the new release directory. It never changes the checked-in
acceptance descriptor.

The first source-built Virgl release, `20260822-6afe4225-virgl2`, compressed the
17,179,869,184-byte sparse GPT disk to 830,866,283 bytes. Its target-files
archive contains the source-built 16 KiB Mesa EGL/GLES and Gallium Virgl
libraries plus the exact `virtio_gpu_dri.so -> gallium_dri.so` link. All three
Mesa EGL/GLES consumers retain `DT_NEEDED=gallium_dri.so` and
`RUNPATH=$ORIGIN/../dri`, allowing the dependency to resolve inside Android's
restricted `sphal` linker namespace. The
workstation verifier accepted the copied compact bundle before installation.
Its built-in-only 16 KiB ACK ELF is `6.18.21-g6385729901c5` from the 27-topic
final tree `e675d73659f9f829c7e36db035f1652ce2a85408`. Hardware-renderer runtime
qualification remains separate from this static artifact acceptance.

The current accepted release is
`android17-ack6.18-16k-20260824-c3febf12-theme1`. It retains the 33-topic ACK
tree `05c892f8175ca302ca869b01efc76f09ad14fa2c`; topic 33 corrects COW-backed
UBD request advancement, while AOSP route `p0007` supplies the framework
theme-gate fix. The built-in-only 16 KiB ELF reports
`6.18.21-gc345c80dbb03`, and the sparse GPT disk compresses to 826,612,380
bytes. `metadata/accepted-release.env` owns the exact release, patch-manifest,
and source identities.

The defect was visible only after the first writes to a fresh COW. UBD divided
one descriptor into backing-image and COW-image runs, then advanced the next
descriptor by the final run length rather than by the complete descriptor.
That corrupted the newly created ext4 journal: `e2fsck` deleted it and `vold`
reported `No recognized filesystem found`, adding about 24 seconds of
full-device-encryption fallback in a representative boot. With topic 33,
`e2fsck` reports `data: clean` before and after `encryptFstab`, and
`mount_all --late` completes in 925 ms at guest second 5.97. The fallback was
therefore a real, avoidable delay, but normal first-boot package and ART work
still dominates total boot time under emulation.

The host also accepts a Virtio-GPU `CTX_CREATE` whose `context_init` capset bits
are zero, the Linux driver's device-selected form, and normalises it to Virgl2
for virglrenderer. Previously it rejected that valid command with `0x1205`, so
the following resource attachments produced the observed `0x1204` invalid-
context errors. Explicit Virgl capsets 1 and 2 remain unchanged and reserved
values remain errors.

A fresh-COW acceptance run used this release with 4 GiB, ten online vCPUs and
the gadgets engine. The first candidate exposed two host allocator stalls while
several private gadget caches destroyed thousands of address-space link nodes;
the host now gives each bounded cache generation its own PMR pool and releases
that storage in bulk. The corrected run set `sys.boot_completed=1` at guest
second 307.5, retained an enforcing SELinux state and a responsive independent
`hvc0` shell/logcat console, and powered down normally at second 366.290. Its
`ttyS0` log contains no RCU stall, UBD/journal failure, unrecognised-userdata
fallback, Virtio-GPU `0x1204`/`0x1205`, oops, or panic.

The preceding image's natural warm boot enabled the screen at guest second
31.827 but completed the boot animation only at 1531.950. That 1500.123-second
interval was WindowManager's 30-second emergency timeout multiplied by the
product factor 50. `ThemeInitializationObserver` had marked the theme unready
while waiting for wallpaper colors, but its no-color fallback ran only at
`PHASE_BOOT_COMPLETED`; WindowManager withheld that phase on the same theme
readiness flag. Route `p0007` moves only the fallback to phase 600. Its focused
test preserves the normal callback and proves phase 550 does not force the
observer while phase 600 does.

On the accepted release, a ten-vCPU gadgets warm run forces the fallback at
guest second 28.279, notifies WindowManager at 28.474, enables the screen at
37.048, completes animation at 42.296, and is observed complete at wall second
44.977 with the multiplier unchanged. It remains live for a 30-second dwell
and powers down normally at 78.615; a second warm run completes at 44.871.
A fresh-COW rerun completes at 130.287, remains live for 30 seconds, and powers
down at 164.436. Exact scans of those passing runs find no RCU stall,
userdata/journal fallback, Virtio-GPU context error, oops, or panic. One prior
fresh attempt panicked four seconds after already completing boot on a
kernel-mode Darwin signal 10; it did not reproduce and is retained as an
isolated observation rather than counted as passing evidence.

## Separate macOS Android executable

Configure and build the Android-only 16 KiB product after installing the
accepted release:

```sh
cmake --preset macos-android17
cmake --build --preset macos-android17
```

Launch it with the accepted release directory and a dedicated writable COW
path whose parent already exists:

```sh
.build/presets/macos-android17/bin/uml-darwin-android17 \
  --android-release=/absolute/path/to/.build/android/releases/accepted-directory \
  --android-instance=/absolute/path/to/android17-instance.cow \
  --virtio-console-socket=/absolute/path/to/android17-console.sock \
  --execution-engine=gadgets
```

The executable embeds the accepted ACK ELF and release-manifest identity. It
rejects another release, protects release-owned boot arguments, maps the large
installed GPT image only as the COW backing file, and binds each instance to
the accepted release with an adjacent manifest receipt. Attach to the console
socket with a local Unix-stream client such as `nc -U`; the userdebug shell on
`hvc0` accepts `logcat` directly. Linux printk is deliberately restricted to
`ttyS0`; `hvc0` is selected only through `androidboot.console=hvc0`, so the
interactive Android shell and logcat output cannot be mixed with kernel logs.
Press Enter after attaching to obtain the shell prompt. The current
`uml-darwin` executable and Linux desktop rootfs discovery remain separate.
