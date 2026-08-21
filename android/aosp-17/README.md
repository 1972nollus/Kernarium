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

Route `p0004` carries the AOSP Mesa project changes for an ARM64 Virgl-only
Gallium build. It generates vendor `libEGL_mesa`, GLES 1 and GLES 2 libraries,
and `gallium_dri` from source with 16 KiB load alignment; it does not package
SwiftShader. Route `p0001` selects those modules only for the UML product and
installs `virtio_gpu_dri.so` as a symlink to `gallium_dri.so`, leaving other
Cuttlefish products on their existing graphics stack. The target-files
verifier requires all four AArch64 shared objects, checks every PT_LOAD segment
for 16 KiB alignment, and requires the exact DRI symlink before a candidate can
be published.

This accommodation is not a claim of stock Android or VTS eBPF-JIT
compliance. ACK's native arm64 JIT depends on native CPU-capability, `pt_regs`,
Spectre, and exception-table contracts that the arm64 UML architecture does
not share. The producer therefore requires built-in `BPF`, `BPF_SYSCALL`, and
`CGROUP_BPF`, rejects `BPF_JIT`, `BPF_JIT_ALWAYS_ON`, and `HAVE_EBPF_JIT`, and
keeps the slower interpreter choice visible in both the boot property and log.

The 2026-08-21 builder checkpoint produced an OTA-disabled target-files archive
with 4,571 entries and 1,162 verified target executables, a 4,375,040-byte raw
initramfs with 35 unique entries, and a reverified seven-partition
17,179,869,184-byte GPT disk. The compressed transfer artifact is 817,207,500
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
selects `android17-ack6.18-16k-20260821-5735514a-rcu-timer1`, published from
clean outer revision `5735514ae840ac4bbdca78fd0ba9afa295351bc4` after the
static kernel, target-files, VINTF, SELinux, initramfs, disk, DRM-HWC,
no-camera APEX, non-A/B storage, built-in userdata-encryption, multi-vCPU timer
service, and built-in single-port Virtio-console gates passed.
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

Accepted release `20260821-5735514a-rcu-timer1` compressed the
17,179,869,184-byte sparse GPT disk to 807,725,165 bytes. The workstation
installer restored a sparse disk with approximately 1.6 GiB allocated,
reverified all seven partitions, and exposed the completed local release only
after validation. Its built-in-only 16 KiB ACK ELF is
`6.18.21-g1edaf2cbd207` from final tree
`d1a4d62471db6569cfe5a11e842a5a433d8d297e`.

The accepted four-vCPU gadgets campaign used one fresh COW for a cold boot to
guest uptime 982 seconds and a warm boot to 284 seconds. Neither dedicated
`ttyS0` capture contained an RCU stall, missing timer wakeup, oops, or panic;
both ended with `result=0 exitcode=0`. The independent `hvc0` transcripts
reached their target markers and returned `logcat -b main` output. They contain
no kernel timestamp, Linux version, or kernel command-line record. Android's
`sys.boot_completed` property remained unset because SurfaceFlinger still
restarts with `SIGABRT`; this acceptance closes the SMP timer and console
transport gates, not graphical application readiness.

The current host default-JIT gate additionally uses commit `2a28713a`, which
defers a native atomic block's serialized cold fallback until its direct-write
publication has unwound. A fresh-COW, four-vCPU run of this same accepted
release kept the Unix-socket `hvc0` shell and main logcat buffer responsive
through guest second 319.98, then powered off normally at second 368.49 with no
observed RCU-stall, oops, or panic report on `ttyS0`. This closes the earlier
approximately-25-second JIT publication deadlock without changing the accepted
ACK or disk artifacts. SurfaceFlinger still prevents graphical boot completion.

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
