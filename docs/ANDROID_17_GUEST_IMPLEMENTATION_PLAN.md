# Android 17 guest implementation plan

## Status and objective

This document defines the implementation plan for a native ARM64 Android 17
guest running on the Darwin User-Mode Linux emulator with a 16 KiB Android
Common Kernel (ACK) 6.18 kernel.

The Android product is additive. It does not replace, rename, or silently
change either of the current upstream-stable Linux products. All project
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
   upstream-stable repository used by the current products.
6. Kernel sources of truth are organized under `kernel/common`,
   `kernel/upstream-6.18`, and `kernel/android-6.18`.
7. CMake, Ninja, and Xcode consume already-built local artifacts only. They
   never fetch sources, contact the Linux builder, replay patches, invoke
   Kbuild, run Soong, or assemble a disk image.
8. The Android macOS executable and Android runtime framework have distinct
   target names, output names, bundle identities, and output directories from
   the current products.

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
- automatic remote builds from an Apple configure, build, test, or packaging
  command.

## Architectural boundary

The result has one producer boundary and one consumer boundary:

```text
dedicated x86_64 Linux builder
    pinned outer-project revision
    pinned AOSP 17 manifest
    pinned ACK 6.18 commit
    local ACK and AOSP patch/support inputs
             |
             | standalone prepare/build/verify/publish
             v
versioned Android release bundle on the Linux builder
             |
             | explicit SSH import and local verification
             v
Git-ignored local prebuilt release under .build/android/releases
             |
             | read-only manifest import
             v
macOS executable / iOS Device framework / iOS Simulator framework
```

There is no reverse edge. The Apple product graph does not know the builder's
hostname, remote paths, AOSP checkout, ACK checkout, compiler invocation, or
image assembly commands.

The same prebuilt-only rule is applied to the current upstream-stable kernel
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
      release-manifest.schema.json
      verify-release.py
      KernelArtifact.cmake
    config/
      page-16k.fragment
    tooling/
      materialize-git.sh
      verify-series-policy.py
      verify-linux-imports.sh
      shared script libraries

  upstream-6.18/
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
    builder.conf.example
    source-location.conf.example
    metadata/
      aosp.env
      kernel.env
      accepted-release.env
    config/
      android.fragment
    patches/
      metadata.env
      extraction-decisions.tsv
      local-change-policy.tsv
      path-ownership.tsv
      topic-path-overlaps.tsv
      series/
    support/
      aosp/
        device/uml/darwin/
        sepolicy/
        init/
        fstab/
        patches/
          manifest.tsv
          projects/
      image/
        partition-layout.json
    build/
      prepare-source.sh
      build-kernel.sh
      build-aosp.sh
      assemble-image.sh
      build-release.sh
      verify-release.sh
      publish-release.sh
    remote/
      invoke-build.sh
      import-release.sh
```

`kernel/README.md` becomes a short index. It explains the two kernel lines,
the shared ABI and artifact contracts, and the fact that the emulator build is
an artifact consumer. It does not contain either line's source or patch policy.

### Current-path migration

Move the current kernel material as follows:

| Current path | Target path |
| --- | --- |
| `kernel/patches/**` | `kernel/upstream-6.18/patches/**` |
| `kernel/config/common.fragment` | `kernel/upstream-6.18/config/common.fragment` |
| `kernel/config/page-4k.fragment` | `kernel/upstream-6.18/config/page-4k.fragment` |
| `kernel/config/page-16k.fragment` | `kernel/common/config/page-16k.fragment` |
| `kernel/toolchain/**` | `kernel/upstream-6.18/build/toolchain/**` |
| `kernel/build-kernel.sh` | `kernel/upstream-6.18/build/build-kernel.sh` |
| `kernel/KernelToolchain.cmake` | `kernel/upstream-6.18/build/KernelToolchain.cmake` |
| `kernel/CMakeBuildLayout.cmake` | `kernel/upstream-6.18/build/CMakeBuildLayout.cmake` if still needed by the standalone workflow |
| `kernel/abi/**` | `kernel/common/abi/**` |
| current source/replay scripts | fixed-line wrappers under `kernel/upstream-6.18/scripts/` plus parameterized engines under `kernel/common/tooling/` where genuinely shared |
| `kernel/README.md` | `kernel/upstream-6.18/README.md`, followed by a new root index |

The ignored `kernel/source-location.conf` is user-owned local data and must not
be deleted or moved automatically. Migration documents copying its value to
`kernel/upstream-6.18/source-location.conf`; the new per-line files remain
ignored. The actual Android source-location configuration exists only in the
outer-project checkout on the Linux builder. A workstation that imports a
release may have an ignored SSH/import configuration derived from
`builder.conf.example`, but it has no Android or ACK source path.

Move the current patch series as one byte-preserving operation before changing
its scripts. Do not combine the path move with a patch rebase or configuration
change. Update all tracked references and tests in the same checkpoint so no
supported command depends on compatibility symlinks.

### Common versus version-specific ownership

Only behavior proven identical belongs in `kernel/common`:

- the canonical plain-C Darwin host/kernel ABI;
- the 16 KiB page and 128 KiB task-stack fragment;
- the release-manifest schema and read-only artifact verifier;
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

Do not share one generated mail patch between upstream-stable 6.18 and ACK
6.18. Port the intended behavior as ACK-based topic commits and regenerate a
separate Android series.

## Local-only change policy

Both kernel lines remain private project replays. Active policy and command
output use local dispositions such as:

- `carry-locally`;
- `drop-locally-if-contained`;
- `rebase-locally-and-retest`; and
- `not-applicable-to-this-line`.

Do not add a submit, upload, push, review-server, or contribution command.
Project-authored ACK topics use a clear local prefix such as
`UML-DARWIN-LOCAL:`. A commit copied from a pinned source retains its original
authorship and provenance, but its presence in this project does not imply a
submission plan.

Patch files remain generated artifacts. Each line owns a clean linear local
topic branch. Changes are made with Git commit/rebase/amend operations and
regenerated with `git format-patch`; patch bytes are never edited manually.

The active series tooling must take an explicit line root and fail if that root
does not match its metadata. A regeneration command for one line must be
structurally unable to write the other line's metadata or patch directory.

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

### Builder configuration

Use a Git-ignored configuration file parsed as data rather than sourced by a
shell, for example:

```text
ANDROID_BUILDER_HOST=android-builder
ANDROID_BUILDER_WORK_ROOT=/srv/uml-android/work
ANDROID_BUILDER_RELEASE_ROOT=/srv/uml-android/releases
ANDROID_BUILDER_DARWIN_SDKROOT=/opt/darwin-sdk/MacOSX.sdk
```

`ANDROID_BUILDER_HOST` names an entry in the user's normal SSH configuration.
Passwords, keys, tokens, private host options, and options that disable host
verification never enter the repository.

The preparer proves that the remote host reports `Linux` and `x86_64`. It also
checks available disk, RAM, JDK, `repo`, Git, Python, build tools, LLVM, and the
configured Darwin SDK/sysroot before starting an expensive build.

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

### Remote phases

The remote workflow has four separate phases:

1. **Prepare.** The only network-enabled phase synchronizes the exact AOSP
   manifest and ACK commit, verifies their commits/trees, creates clean local
   worktrees, replays the ACK-local patch series, and installs the
   project-owned Android support tree into a disposable AOSP worktree. If an
   existing AOSP repository must be changed, it also replays the explicitly
   routed per-project series below `support/aosp/patches`.
2. **Build.** The offline build phase builds the ACK UML kernel, AOSP product,
   ramdisk/support files, and composite guest disk. It writes only to remote
   out/work directories.
3. **Verify and publish.** Verification runs against a staged release
   directory. Publication atomically renames the verified directory to a new
   release ID and refuses to replace an existing release.
4. **Import.** A workstation operator explicitly retrieves one completed
   release. Import never starts, resumes, repairs, or deletes a remote build.

`prepare-source.sh` is the only remote step allowed to access source networks.
The build, verify, and publish steps must succeed with source-network access
disabled.

No remote command is a CMake target, build preset, CTest, Xcode build phase, or
dependency of an Apple target.

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

Use the conventional built-in C Binder for initial bring-up and remove the
Cuttlefish `binder.impl=rust` selection. Rust Binder can be evaluated later as
a separately justified local product change after the native Android guest is
accepted; it is not part of the first kernel port.

### Kernel verification

The remote kernel gate requires:

- exact ACK base and final replay tree;
- clean ordered local series and policy metadata;
- exact resolved fragment values;
- `getconf`-independent ELF inspection proving AArch64, ELF64, ET_DYN, the
  expected entry point, relocation policy, and 16 KiB executable boundaries;
- no writable-executable segment;
- undefined `darwin_uml_*` imports exactly matching the common 16 KiB ABI;
- no loadable modules and no Android 4 KiB output; and
- generation of the compact kernel support set used by release verification.

## AOSP 17 product

### Product ownership

Create a project-owned, 64-bit-only product such as
`aosp_uml_arm64_pgagnostic`. Keep its source of truth below
`kernel/android-6.18/support/aosp`. The remote preparer stages that tree into a
disposable AOSP checkout. Changes to existing AOSP repositories, if required,
are maintained as separate local Git topic commits and generated per-project
patch series below `kernel/android-6.18/support/aosp/patches`; the prepared
AOSP worktree is never the source of truth.

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
manifests, compatibility matrices, HAL declarations, and kernel command line
are versioned support inputs under `kernel/android-6.18` and published in the
release bundle. The Android property set selects the ACK memfd path with
`sys.use_memfd=true`; the ACK ashmem compatibility behavior remains available
for legacy applications.

### Android build verification

The remote AOSP gate requires:

- `atest elf_alignment_test`;
- AOSP's 16 KiB prebuilt-library checker;
- no 32-bit ABI or 32-bit native library in the selected product;
- a complete VINTF match for the project-owned UML vendor definition and ACK
  6.18 kernel identity;
- compiled SELinux policy with `policycap memfd_class`;
- filesystem and partition-layout checks against the checked support file;
- no undeclared hardware feature XML; and
- a target-files/image inventory containing no DLKM or 4 KiB kernel artifact.

## Published release contract

### Remote release layout

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
are not emulator dependencies and are not part of routine SSH import.

### Manifest contents

The versioned manifest records:

- release ID and product family `android-17-ack-6.18-16k`;
- Android major, build ID, build variant, fingerprint, and vendor API level;
- AOSP manifest tag and pinned `repo manifest -r` identity;
- ACK repository, tag, base commit/tree, local final tree, and patch-manifest
  identity;
- outer-project revision and dirty-state observation;
- page size `16384`;
- common host/kernel ABI version and fingerprint;
- kernel release and ELF role;
- selected product and configuration identities;
- an exact inventory of published relative paths, roles, expected formats, and
  observed byte sizes;
- exact producer command; and
- builder OS, JDK, LLVM, linker, SDK/sysroot, and Android build-tool versions.

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

### SSH import

The explicit importer:

1. reads the ignored builder configuration;
2. validates the release ID;
3. retrieves the manifest first through normal host-key-verified SSH;
4. requires the manifest identity and semantic fields to match the checked-in
   acceptance descriptor;
5. transfers the exact declared payload into a fresh staging directory under
   `.build/android/releases`;
6. rejects undeclared files, symlinks, special files, absolute paths, and path
   traversal;
7. verifies the declared inventory, observed sizes, and transport completion;
8. repeats portable ELF, ABI, config, ramdisk, and image-layout checks;
9. expands the disk image into the final local release only after its archive
   integrity and declared filesystem/image structure validate; and
10. atomically renames the completed staging directory and refuses to replace
    an existing release.

The importer does not use a broad `--delete`, modify another local release, or
make a partial release visible. Deleting or replacing an imported release is a
separate explicit maintenance operation.

The trust boundary is the controlled builder reached through authenticated
SSH. SSH supplies host authentication and transport integrity; the importer
supplies release-ID, inventory, format, ABI, page-size, and image-structure
validation. In accordance with the repository's lean workflow policy, this
plan does not add a product hash manifest. References to an "exact" ELF below
mean that the Apple carrier is compared byte-for-byte with the already-imported
local ELF it was configured to consume, not that independent producer builds
must be byte-identical.

## Apple build consumption

### Import-only kernel CMake

Replace the current in-CMake Kbuild graph with an import-only artifact owner.
`kernel/CMakeLists.txt` and `kernel/common/artifacts/KernelArtifact.cmake`
perform only local, read-only validation and expose target-specific imported
artifact dependencies.

Use one release root or manifest cache variable per family rather than
independently overridable ELF and support paths, for example:

```text
UML_UPSTREAM_618_KERNEL_ARTIFACT_MANIFEST
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
- SSH, scp, rsync, or remote path discovery; and
- AOSP/Soong/image construction.

A missing, stale, wrong-family, wrong-page-size, wrong-architecture, or
ABI-incompatible artifact is a clear configuration error.

Machine-specific artifact paths belong in ignored local configuration or
`CMakeUserPresets.json`, not in checked presets. Checked presets select a
product family and enforce its properties.

### Current products

Preserve the existing output names and behavior:

- `uml-darwin` continues to embed the current upstream-stable kernel artifact;
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
- has no dependency on `uml_kernel`, an ACK checkout, or a remote build target.

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
- explicit Darwin SDK/sysroot input; and
- one standalone 16 KiB ET_DYN `vmlinux` plus config/import verification.

Exit criteria:

- build occurs on the dedicated Linux machine;
- no AOSP build has yet been required;
- ELF architecture, type, alignment, signed-carrier layout, and ABI imports
  pass;
- the resolved kernel page size is 16 KiB; and
- the build system has no Android 4 KiB path.

Stop here if the designated Linux toolchain cannot reproduce the Darwin UML
ELF contract. Resolve that locally before starting the multi-hour AOSP lane.

### Phase 1: split kernel ownership without changing products

Deliverables:

- `kernel/common`, `kernel/upstream-6.18`, and `kernel/android-6.18` layout;
- current patch/config/build material moved to the upstream line;
- common ABI and parameterized tooling moved to common ownership;
- per-line ignored source configurations and cache roots;
- updated tests, `.gitignore`, and `.gitattributes` paths;
- updates to `AGENTS.md`, `docs/architecture.md`, `docs/testing.md`,
  `docs/REFACTOR_HANDOFF.md`, `docs/REFACTOR_PROGRESS.md`, `darwin/README.md`,
  the new `kernel/README.md` index, and each line's README so active guidance
  names the new paths and the staged transition; and
- preservation of explicitly dated historical sections as history rather than
  rewriting them into current instructions.

Exit criteria:

- current upstream replay and both standalone page builds pass;
- generated current patch bytes and final tree are unchanged by the move;
- one line's replay/regenerator cannot touch the other line;
- ABI generated copies verify from the new common path; and
- no old tracked path remains as a second source of truth.

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
  Kbuild toolchain, Git network, or SSH command available;
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
- standalone remote kernel replay/build/verification.

Exit criteria:

- exact ACK final tree and series policy pass;
- only one 16 KiB kernel artifact is produced;
- Binder, SELinux, `memfd_class`, ashmem compatibility, DMA-BUF, UBD, Virtio
  devices, namespaces, cgroups, and required filesystems verify built-in;
- HostFS and loadable modules verify disabled; and
- common ABI import verification passes.

### Phase 4: build the AOSP 17 UML product and disk

Deliverables:

- `aosp_uml_arm64_pgagnostic` product;
- UBD fstab/ueventd/init/VINTF/SELinux support;
- selected software HALs and accurate feature declarations;
- standalone ramdisk and GPT disk assembly; and
- Android page-alignment and image-layout verification.

Exit criteria:

- AOSP build passes on the dedicated machine;
- all selected userspace native ELF files pass 16 KiB alignment checks;
- no 32-bit ABI, DLKM partition, module-loading service, or 4 KiB kernel
  artifact is present; and
- the disk and ramdisk validate against the checked support contract.

### Phase 5: publish and import a complete Android release

Deliverables:

- atomic remote publisher;
- versioned release manifest and checked-in acceptance descriptor;
- explicit SSH importer; and
- local semantic verifier.

Exit criteria:

- a complete release imports into `.build/android/releases`;
- partial, structurally or semantically altered, wrong-family, wrong-page,
  path-traversal, symlink, and undeclared-file fixtures fail;
- a second import cannot overwrite the first; and
- CMake can validate the local release with the Linux builder offline.

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
- per-line source configuration and cache isolation;
- parameterized patch-series verification and write isolation;
- common ABI path, identity, generated-copy, and per-line import checks;
- Android lane references to the shared 16 KiB fragment and no reference to a
  4 KiB fragment;
- release-manifest schema, inventory, path safety, size, and semantic
  validation;
- accepted-release descriptor validation;
- CMake graph inspection proving no Git, Kbuild, Soong, SSH, scp, rsync,
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
- local patch replay/final tree;
- resolved kernel configuration;
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
| An ELF is mixed with another release's ramdisk or disk | Accept one manifest/release root, derive all paths internally, and embed/check the release identity. |
| Current and Android frameworks collide in loader discovery | Use the distinct `KernariumAndroidRuntime_` prefix and a dedicated identity contract. |
| Cuttlefish declares devices the emulator does not implement | Own feature XML and VINTF locally; omit unsupported services and declarations. |
| Android native apps contain 4 KiB-only libraries | Enforce AOSP alignment checks and include a native `arm64-v8a` APK in acceptance. |
| ADB starts but is unreachable from the host | Add and test an explicit local SLiRP forwarding path before APK-install acceptance. |
| Large remote artifacts leak into Git or routine builds | Publish only the compact manifest inventory, import under `.build`, and keep producer object trees on the Linux builder. |

## Completion definition

The plan is complete when:

1. the versioned kernel directory split is the only source of truth;
2. all emulator and framework builds consume validated prebuilt artifacts and
   contain no kernel/image producer edge;
3. the dedicated x86_64 Linux builder reproducibly prepares pinned AOSP 17 and
   ACK 6.18 sources, applies only local project changes, and publishes one
   verified 16 KiB Android release;
4. `uml-darwin-android17` and
   `KernariumAndroidRuntime_17_ACK6_18_16K` are separate accepted products;
5. Android 17 boots enforcing with a real 16 KiB page size and runs both
   managed and native ARM64 applications; and
6. the existing upstream-stable 16 KiB and 4 KiB products retain their names,
   artifacts, workflows, and accepted behavior.

## Primary external references

- [AOSP Android 17.0.0 release manifest](https://android.googlesource.com/platform/manifest/+/refs/tags/android-17.0.0_r1)
- [ACK Android 17 / 6.18 release builds](https://source.android.com/docs/core/architecture/kernel/gki-android17-6_18-release-builds)
- [ACK `android17-6.18-2026-06_r35` source](https://android.googlesource.com/kernel/common/+/refs/tags/android17-6.18-2026-06_r35)
- [AOSP 16 KiB page-size guidance](https://source.android.com/docs/core/architecture/16kb-page-size/16kb)
- [Cuttlefish Android 17 source tree](https://android.googlesource.com/device/google/cuttlefish/+/refs/heads/android17-release/)
- [AOSP build-host requirements](https://source.android.com/docs/setup/start/requirements)
