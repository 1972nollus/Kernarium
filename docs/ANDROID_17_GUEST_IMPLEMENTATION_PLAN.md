# Android 17 guest implementation plan

## Status and objective

This document defines the implementation plan for a native ARM64 Android 17
guest running on the Darwin User-Mode Linux emulator with a 16 KiB Android
Common Kernel (ACK) 6.18 kernel.

Implementation started on 2026-08-20 with the versioned AOSP 17/ACK 6.18
source metadata and builder-local preparation/preflight lane. That checkpoint
does not yet add a Darwin UML ACK patch series, Android product, build artifact,
disk image, or Apple runtime target and does not by itself complete a phase
below.

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
   target names, output names, bundle identities, and output directories from
   the current products.
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

The first release uses one raw GPT UBD disk containing statically addressed
partitions sufficient for:

- `system`;
- `system_ext`;
- `product`;
- `vendor`;
- `odm` only if a retained HAL requires it;
- `metadata`; and
- writable `userdata`.

Immutable partitions may use 16 KiB-block EROFS; writable state uses ext4.
Avoid F2FS in the first release. Disable dynamic-super, Virtual A/B, OTA, and
production AVB policy until basic boot, application execution, and clean
shutdown are accepted.

Publish the generic and vendor ramdisk content needed for Android first-stage
init separately from `vmlinux`. The runtime passes the verified ramdisk and
`init=/init`; it does not ask the emulator to parse Android `boot.img`.

The published disk is a pristine base with empty writable-state templates. An
explicit runtime provisioning command copies or filesystem-clones that
already-built disk into a per-instance location before first boot; it does not
repartition or rebuild the image. The imported release is never opened writable.
Subsequent boots use the instance disk, whose receipt binds it to the base
release ID and partition layout. Resetting an instance is an explicit operation
that replaces only that instance from the same accepted base.

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
    ramdisk.img
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
- the current packaging scripts continue to require both current page
  profiles.

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

Add a distinct target and output family:

```text
CMake target:  KernariumAndroid17Runtime
framework:     KernariumAndroidRuntime_17_ACK6_18_16K
presets:       kernarium-android17-ios-device
               kernarium-android17-ios-simulator
package root:  .build/kernarium-android17-xcframeworks
```

The different `KernariumAndroidRuntime_` prefix is required. The current loader
scans `KernariumRuntime_*_4K` and `KernariumRuntime_*_16K`; reusing that prefix
would create an ambiguous candidate set when both families are embedded.

The Android framework:

- has Device and Simulator slices, both 16 KiB only;
- embeds the exact imported Android `vmlinux` in its aligned signed
  `__TEXT,__uml_elf` carrier;
- runs the existing signed-carrier safety verifier;
- packages the verified ramdisk/support resource without rebuilding it;
- uses a dedicated Android runtime composition source and identity contract;
- has its own bundle identifier and module map; and
- is packaged by a new script that requires exactly the two Android 16 KiB
  slices.

Do not broaden the current four-slice packaging script or silently add this
framework to the existing Kernarium app. Integrating a guest-family selector
or a separate Android app target is a later explicit application decision.

### Android boot and management

The Android launch profile supplies the verified ramdisk, `init=/init`, the
UBD disk, `tty0`, `ttyS0`, the project-owned Android boot properties, and the
selected SELinux enforcing policy. It does not run the current Linux desktop
overlay shell.

Initial diagnostics use `ttyS0`. Add a local, Android-product-only SLiRP host
forwarding path for ADB over TCP before application-install acceptance. Do not
claim ADB availability merely because `adbd` starts inside the guest.

The raw disk is a runtime resource and is not linked into the executable or
framework. On iOS an instance is provisioned from the accepted base into the
app's Documents space; on macOS the Android launcher receives a validated
per-instance disk and its release receipt. Neither product boots the imported
base disk writable.

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
- `aosp_uml_arm64_pgagnostic` product;
- patch-owned UBD fstab/ueventd/init/VINTF/SELinux source;
- selected software HALs and accurate feature declarations;
- external image/release/verification inputs under `android/aosp-17/support`;
- standalone ramdisk and GPT disk assembly; and
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
- the disk and ramdisk validate against the checked support contract.

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

Deliverables:

- Device and Simulator `KernariumAndroid17Runtime` slices;
- `KernariumAndroidRuntime_17_ACK6_18_16K.xcframework`;
- dedicated Android runtime API identity and loader tests; and
- signed ELF carrier and packaged support resources.

Exit criteria:

- both framework slices contain the exact accepted ELF;
- carrier verification and package-shape tests pass;
- physical-iOS execution requires no JIT entitlement, anonymous executable
  allocation, RW-to-RX publication, or module-text relocation outside the
  signed immutable carrier;
- framework discovery cannot collide with current `KernariumRuntime_*` names;
- no Android 4 KiB framework exists; and
- current dual-profile framework packaging still passes independently.

### Phase 8: Android boot and application acceptance

Deliverables:

- enforcing Android boot;
- functional SurfaceFlinger/launcher on Virgl GLES;
- ADB-over-TCP host forwarding;
- APK install/run automation; and
- Android-specific integration workflow.

Exit criteria:

- Android reports major version 17;
- `getconf PAGE_SIZE` reports `16384`;
- SELinux reports enforcing and `memfd_class` enabled;
- Binder service discovery works;
- boot-complete, clean shutdown, and restart pass;
- Virtio GPU/input, output-only sound, and Ethernet are exercised;
- one Java/Kotlin APK and one properly aligned `arm64-v8a` native APK install
  and run; and
- the same accepted disk boots in the dedicated macOS product and, after
  application integration is authorized, the Android runtime framework.

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
| An ELF is mixed with another release's ramdisk or disk | Accept one manifest/release root, derive all paths internally, and embed/check the release identity. |
| Current and Android frameworks collide in loader discovery | Use the distinct `KernariumAndroidRuntime_` prefix and a dedicated identity contract. |
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
   `KernariumAndroidRuntime_17_ACK6_18_16K` are separate accepted products;
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
