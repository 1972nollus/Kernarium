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

After packaging, the builder verifies that `BOOT/kernel` exactly matches the
selected ACK ELF, rejects DLKM and loadable-module payloads, rejects 32-bit
target ABIs, and checks 16 KiB load alignment for target executables.

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

Route `p0001` also replaces the inherited dynamic/virtio fstab with
`fstab.uml`. Android first-stage init discovers the fixed GPT partitions below
the `uml-blkdev.0` platform device and mounts the immutable filesystems
directly from `/dev/block/by-name`; there are no logical, slot-select, AVB, or
DLKM mount flags. The UML init and ueventd inputs keep the first-stage mount,
late writable-state mount, serial console, and bugreport paths while removing
Cuttlefish-only HVC, vsock, Wi-Fi, custom-partition, persistent-memory, and
suspend actions. Soong disables the inherited stock fstabs and serial-logging
file even though the base product still contributes their package names.
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

`support/boot/kernel-command-line.txt` owns the machine-independent launch
arguments. It selects `/init`, the UML fstab suffix and boot device, enforcing
SELinux, and the `ttyS0` diagnostic shell. The runtime adds its local `ubd0=`
disk and initrd paths; no builder or workstation path is stored in this tree.

The 2026-08-21 builder checkpoint produced an OTA-disabled target-files archive
with 4,571 entries and 1,162 verified target executables, a 4,374,784-byte raw
initramfs with 35 unique entries, and a reverified seven-partition
17,179,869,184-byte GPT disk. The sparse disk allocated 1,677,684,736 bytes in
that run. This proves the build, fstab, initramfs, and image-layout contracts;
it does not yet claim a UML boot or accepted VINTF, SELinux, and HAL runtime.
