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

Apply the generated per-project AOSP series, exactly synchronize the checked
device overlay to `device/kernarium/darwin`, and build the 16 KiB ARM64-only
product:

The builder host must provide `zip` for AOSP packaging. Source preparation is
the sole transport-enabled producer step; patch application, compilation,
packaging, publication, and verification remain offline.

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
upstream project from its exact base commit and tree to its expected final
tree. Generated `git format-patch` series under `patches/projects` remain the
authority for upstream-project changes. The standalone product is instead
owned by [`overlay/`](overlay/) and
[`tools/sync-device-overlay.py`](tools/sync-device-overlay.py), which replaces
and byte-verifies exactly one destination, `device/kernarium/darwin`. It does
not copy `support/`, the Jelly source, or modify any other AOSP path.

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
AOSP manifest and ACK commit, fetches Jelly's exact external commit into
`packages/apps/Jelly`, and replays the checked generated Jelly series. All later
commands operate offline.

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
guest-Trusty alternatives. AOSP image assembly, release publication, and Apple
consumers remain separate from the host build.

## Dedicated emulator device overlay

`overlay/aosp_kernarium_darwin.mk` defines a fresh-only product named
`aosp_kernarium_darwin`. There is no legacy product alias, payload migration,
or compatibility copy: the final Cuttlefish topic retires the preceding UML
product registration and device tree, and the exact overlay is the only
supported device tree for new images. It retains the Cuttlefish ARM64
page-agnostic board foundation for Android framework compatibility, then
removes machine services and applications that have no useful emulator
implementation.

The user-facing baseline keeps Settings, Launcher, Files/DocumentsUI,
PackageInstaller, the system WebView, LatinIME, and ThemePicker. It deliberately
removes the obsolete Calendar, Messages, Contacts, Dialer, Camera, Gallery,
DeskClock, Music, photo-table, telephony, satellite, and hardware-demo packages.
Framework services and providers that ordinary third-party applications depend
on remain present even when no corresponding launcher application is shipped.

Jelly comes from LineageOS
[`android_packages_apps_Jelly`](https://github.com/LineageOS/android_packages_apps_Jelly)
at commit `767833067da7bbbb639782bbddec33f602215c3d`. The full upstream source is
not stored in this repository or copied through the device overlay.
[`third_party/jelly/metadata.env`](third_party/jelly/metadata.env) pins the
repository, branch, commit, base tree, final topic identity, destination, and
license. [`third_party/jelly/series/manifest.tsv`](third_party/jelly/series/manifest.tsv)
binds the two generated `git format-patch` files by order, stable patch ID,
SHA-256, and subject. Source preparation fetches only the pinned commit and
replays that series deterministically before any offline AOSP build begins.

The local topic binds Jelly to SDK 37 and disables WebView geolocation by
default. Jelly's upstream optional camera, microphone, and location declarations
remain intact for browser compatibility; they do not cause this product to
advertise absent system hardware. The framework overlay selects
`org.lineageos.jelly` as the default browser. After amending the clean
`kernarium-android17` Jelly topic, regenerate its tracked identities and patch
bytes on the Linux builder:

```sh
android/aosp-17/tools/regenerate-jelly-patches.py \
  --android-root /absolute/path/to/copied/repository/android/aosp-17 \
  --topic-checkout /absolute/path/to/clean-jelly-topic
```

Never edit the generated Jelly patch bytes by hand.

The product disables Bluetooth, Wi-Fi, telephony, GNSS, NFC, UWB, Thread,
camera, biometric, sensor, light, vibrator, secure-element, and remote-Trusty
payloads, and removes their PackageManager feature declarations. Unsupported
microphone and USB hardware, plus verified boot while AVB is disabled, are not
advertised either. This keeps feature queries truthful while preserving the
standard Android APIs and the real Virtio-net Ethernet path. Bluetooth and
Wi-Fi framework API components remain where the platform classpath requires
them; no corresponding HAL, Cuttlefish service APEX, permission feature, or
radio is advertised.

The SettingsProvider resource overlay disables the lock screen, sets the screen
timeout to the largest signed 32-bit value, keeps the display awake while
powered, and always shows the soft keyboard even when Android sees a host
keyboard. These are first-boot defaults for a new image, not a migration of an
existing data partition. The recovery partition uses AOSP's default recovery.
OrangeFox is intentionally not part of this implementation.

## Static UBD disk contract

`support/image/partition-layout.json` owns the fixed 16 GiB GPT layout. It has
separate `system`, `system_ext`, `product`, `vendor`, `odm`, `metadata`,
`userdata`, `init_boot`, `misc`, and `recovery` partitions; it deliberately has
no `super` or DLKM partition. `init_boot` and `recovery` are kernelless Android
boot-image-v4 containers. The independently built and signed ACK ELF remains
outside the disk, while both ramdisks are partition payloads in the one final
guest image.

The routed UML product selects static storage before inheriting the shared
Cuttlefish products. This prevents their dynamic-partition and Virtual A/B
inheritance, properties, postinstall configuration, and OTA-only packages from
entering the resolved product. It also disables A/B OTA packaging and AVB.
The product declares its dedicated recovery partition as the full recovery
image. AOSP derives `TARGET_OTA_ALLOW_NON_AB=true` for every non-A/B product,
so route `p0002` makes `TARGET_SKIP_OTA_PACKAGE=true` suppress the otherwise
automatic recovery resource and reconstruction steps. Target-files therefore
does not install a legacy `recovery-from-boot.p`, `install-recovery.sh`, or
redundant `recovery.img` payload under `vendor`; the release assembler embeds
the one partition image directly in the GPT instead.
The unused `system_other` image is disabled because this product has no second
slot. The device BoardConfig also clears the inherited
`BOARD_USES_SYSTEM_OTHER_ODEX` switch, so Soong installs ordinary application
`odex`/`vdex` outputs beside their APKs instead of routing them to an image that
is not built. `build-aosp.sh` rejects any regression in both switches, verifies
the generated dexpreopt policy after the incremental build, and requires every
resolved partition capacity to match the JSON layout. The target-files verifier
forbids the `SYSTEM_OTHER` namespace, requires the version-scoped ordinary app
artifact set, and rejects incomplete `odex`/`vdex` pairs. Ordinary applications
inside updatable APEXes remain outside pinned AOSP 17's supported dexpreopt
path and are verified at first boot instead. AOSP core still assigns
its default `PRODUCT_VIRTUAL_AB_COW_VERSION` value after product evaluation;
that inert format default is not an enablement signal when
`PRODUCT_VIRTUAL_AB_OTA=false`.

Route `p0002` carries three narrow `platform/build` corrections. When
`TARGET_SKIP_OTA_PACKAGE=true`, target-files no longer depends on or copies the
legacy non-A/B `updater` binary, builds no recovery resource bonus, and does
not reconstruct recovery into a vendor-side install payload. The route also
makes inherited package and copy-file exclusions explicit product variables,
allowing this leaf product to remove unwanted Cuttlefish payloads after
inheritance without changing other Android products.

Route `p0001` also omits the inherited A/B BootControl APEX and BootControl
recovery service for this non-A/B product. This is separate from the stock AOSP
recovery user interface built into the `recovery` partition. Its fixed userdata
fstab has no checkpoint flag, so vold
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
DLKM mount flags. All five immutable Android payload partitions are ext4 images
mounted with `ro`; the host maps the release GPT as an immutable backing image
behind the per-instance COW. The product also overrides the inherited APEX
payload default to ext4, while route `p0008` replaces the one explicit EROFS
selection in `com.android.virt`. The target-files verifier reads every nested
`apex_payload.img` filesystem magic and rejects EROFS, compressed APEXes, and
non-ext4 payloads. This avoids EROFS execution in the guest without changing
the ACK EROFS driver or removing its built-in configuration.
The overlay-owned UML init and ueventd inputs keep the first-stage mount,
late writable-state mount, serial console, and bugreport paths while removing
Cuttlefish-only multiport HVC, vsock, Wi-Fi, custom-partition,
persistent-memory, and suspend actions. The UML product labels its sole
single-port `/dev/hvc0` as a serial device and grants the shell access through
its device-overlay ueventd policy. Soong disables the inherited stock fstabs and
serial-logging file even though the base product still contributes their
package names.
`verify-target-files.py` checks the installed result rather than trusting that
inherited package list.

Once the static AOSP product emits the declared image names, assemble and
immediately verify a raw UBD disk locally on the builder with:

```sh
android/aosp-17/build/assemble-image.sh \
  --target-files /absolute/path/to/aosp_kernarium_darwin-target_files.zip \
  --kernel-elf /absolute/path/to/vmlinux \
  --ramdisk /absolute/path/to/work/normal-ramdisk.img \
  --mkbootimg /absolute/path/to/aosp/out/host/linux-x86/bin/mkbootimg \
  --output /absolute/path/to/release/android17-ubd.img
```

The wrapper revalidates target-files against the selected kernel, consumes its
adjacent `IMAGES` staging directory, wraps the verified normal ramdisk in a
kernelless boot-image-v4 `init_boot`, embeds AOSP's kernelless v4
`recovery.img`, and explicitly leaves `metadata` and `misc` as zero-filled
writable templates. Every other partition requires its declared image. The
blank `misc` partition makes automatic boot select normal mode until Android or
the host writes a standard `boot-recovery` BCB command. The disk assembler
accepts ordinary raw images and Android sparse images, expands payloads into a
sparse host file, rejects overflow and unsafe image paths, refuses to replace
an existing output, and writes matching primary and backup GPT metadata.
Recheck an existing image with the `verify` subcommand. The fixed partition
capacities are intentionally larger than the initial inherited Cuttlefish
dynamic groups; every concrete image must fit before publication.

## Partition-backed boot ramdisks

The emulator does not use `boot.img` or `vendor_boot.img`, and the release no
longer carries a standalone initramfs. Compose the generic init-boot tree with
only the overlay-owned UML first-stage fstab as an intermediate raw newc
archive:

```sh
android/aosp-17/build/assemble-ramdisk.sh \
  --target-files /absolute/path/to/aosp_kernarium_darwin-target_files.zip \
  --kernel-elf /absolute/path/to/vmlinux \
  --mkbootfs /absolute/path/to/aosp/out/host/linux-x86/bin/mkbootfs \
  --output /absolute/path/to/work/normal-ramdisk.img
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
overlay-owned UML fstab. `assemble-image.sh` verifies the intermediate against
the bytes embedded in `init_boot`; only the final GPT enters the release.
Accepted-runtime import reads that bounded partition range and uses the
initramfs verifier's structural mode: it still requires the regular first-stage
fstab and every archive safety/module exclusion while preserving the reviewed
fstab version inside that release.

The common command line does not set `androidboot.force_normal_boot`. The
selected `init_boot` ramdisk is intrinsically a normal boot, while the stock
recovery ramdisk is detected by its `/system/bin/recovery`; forcing normal boot
would make that recovery partition unusable.

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
networking programs. The runtime points `initrd=` at the immutable base GPT and
passes the verified `initrd_offset=` and `initrd_size=` for either `init_boot`
or `recovery`; no builder or workstation path is stored in this tree.

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
and built-in-module evidence, command line, compressed raw GPT disk and layout,
VINTF/fstab support, and compact source provenance. Normal and recovery
ramdisks are verified in place inside that GPT and are not duplicated as
release files.
The manifest separately binds the external Jelly repository, pinned upstream
commit and tree, deterministic final topic commit and tree, patch count, and
series-manifest SHA-256 because that checkout is intentionally outside Repo's
resolved AOSP manifest.
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
when a clean builder-local publication has been reviewed. The current
descriptor selects
`android17-ack6.18-16k-20260825-87eaee1e-artpreopt1`, published from clean
outer revision `87eaee1e433ce8db423cf48819084755ebd5c014`.
The static kernel, target-files, VINTF, SELinux, both partition-backed
ramdisks, ten-partition GPT, source-built 16 KiB Mesa Virgl payload, mixed-COW
UBD, warm-boot theme, minimal application/feature set, external Jelly, Context
Hub exclusion, read-only ext4 partition policy, and 69 ext4 APEX payloads
passed producer verification. Physical-iOS boot qualification remains a
separate runtime gate and is recorded with the current release below.

This is artifact acceptance for Apple-product integration. The bounded normal
boot, stock-recovery and bundled-application checks below do not claim broad
third-party application or HAL qualification.

Producer verification and accepted-runtime verification are deliberately
separate. A new clean producer revision must match the current AOSP route table,
ACK series, kernel policy, boot command line, and partition layout before it can
publish a candidate. The schema-2 runtime is fresh-only: its checked acceptance
descriptor, installed directory, and consumers advance together. There is no
schema-1 fallback or migration path.

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
the partition-backed disk from the current target-files staging directory:

```sh
android/aosp-17/build/publish-release.sh \
  --aosp-source /absolute/path/to/aosp \
  --kernel-source /absolute/path/to/replayed-ack \
  --kernel-build /absolute/path/to/kernel-build \
  --target-files /absolute/path/to/target-files.zip \
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
kernel, target-files, VINTF, GPT, both boot-image-v4 headers, and bounded
`init_boot` initramfs checks, rejects any module output, records only role-based
input descriptions rather than builder paths, compresses the disk with a frame
checksum, verifies the completed staging tree, and then atomically exposes the
new release directory. It never changes the checked-in acceptance descriptor.

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
`android17-ack6.18-16k-20260825-87eaee1e-artpreopt1`. It uses the 34-topic ACK
tree `621a0ab42d8990aa0960bf5d341d89b8f1042723`; topic 34 loads a bounded
initrd range from the GPT host file. The built-in-only 16 KiB ELF reports
`6.18.21-ga64f141f6266`. The schema-2 release contains a ten-partition
17,179,869,184-byte GPT compressed to 706,631,372 bytes and no standalone
ramdisk. Its normal and stock-recovery ramdisks are verified in place inside
`init_boot` and `recovery`. All five immutable GPT payload filesystems and all
69 packaged APEX payloads are ext4. Route `p0008` changes the sole explicit
Virtualization APEX override; the product default covers the remaining APEXes,
and the target-files verifier checks their nested filesystem magic. The build
also clears the inherited `system_other` odex route and packages adjacent
ARM64 `odex`/`vdex` artifacts for the 26 ordinary applications observed in
the prior first-boot ART workload. The ACK
kernel is byte-identical to the preceding accepted overlay release, including
its unchanged built-in EROFS driver. `metadata/accepted-release.env` owns the
exact release, ACK, AOSP, Jelly and common-ABI identities.

The preceding `20260825-8c5881ab-ext4-apex` release passed a fresh physical-iOS
boot on a wired iPad mini 6 running iOS 18.5. Kernarium used gadgets, 4 vCPUs,
2 GiB, 16 KiB guest pages,
normal boot and hardware Virgl from a bundle with no pre-existing COW. The five
GPT payload filesystems mounted read-only ext4 by wall second 0.951 and APEX
bootstrap activated 67 ext4-backed packages by wall second 2.640. Init reached
`sys.boot_completed=1` at wall second 525.731 and remained stable beyond the
30-second dwell. Runtime scans found no EROFS worker, RCU stall, userdata
fallback, Virtio-GPU `0x1204`/`0x1205`, OOM, oops, panic, or Darwin-signal
signature. That result is the matched pre-ART-packaging baseline; the current
release requires fresh and persisted-state remeasurement on every Apple host.

LineageOS Jelly is fetched at upstream commit
`767833067da7bbbb639782bbddec33f602215c3d` and replays exactly two local
topics to final tree `ce63d63703fbfd2e616e94513ea1e5c63ccae4ef`: SDK-37
build compatibility and geolocation disabled by default. The upstream optional
camera, microphone and location declarations remain unchanged. The product
keeps ThemePicker and LatinIME, forces the soft keyboard visible with a host
keyboard, removes lock/idle friction, and omits unused first-party apps and
unsupported hardware feature declarations.

The first minimal-overlay image inherited the Context Hub APEX even though the
product intentionally omitted Wi-Fi. That APEX advertised
`android.hardware.context_hub`; `ContextHubSystemService` then dereferenced a
missing `WifiManager` and restarted `system_server`. The accepted product
excludes `com.android.hardware.contexthub`, and static regressions forbid its
APEX, permission/VINTF payload and feature name. A warm normal boot holds one
stable `system_server`, passes the settings, application, browser-resolution
and feature checks, dwells for more than 30 guest seconds and powers down
cleanly. Explicit recovery mode starts the stock `/system/bin/recovery`,
survives its dwell and powers down cleanly. One preceding fresh run reached
boot completion and then panicked on an isolated kernel-mode Darwin signal 11;
it is not counted as passing stability evidence and remains an open runtime
observation.

The previous accepted `20260824-c3febf12-theme1` release established the
mixed-COW and theme-gate fixes retained by the current image. Its evidence is
kept below as dated history.

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

A later matched-platform campaign used the same accepted release, gadgets,
4 GiB, ten vCPUs, 16 KiB pages, and 440 x 860 at 160 DPI on both the standalone
macOS product and Kernarium's iOS 26.5 Simulator product. macOS reached init's
first `sys.boot_completed=1` action at 786.169 s from a fresh COW and 84.795 s
from the persisted disk. Simulator reached it at 130.991 s and 42.780 s.
Initial startup remained fast on every run: late mount completed by 7.866 s
and boot animation started by 16.753 s. First-boot ART/package verification is
the dominant cost; macOS `artd` reported 579.415 s, while Simulator's
corresponding quiet compilation interval was about 66 seconds. Neither second
boot started `artd`.

Those 6.00x cold and 1.98x warm platform ratios apply only to that campaign.
The earlier 130.287/44.977-second macOS evidence above remains valid, and the
cause of the cross-campaign macOS variance is not established. The long ART
interval did reproduce twice in the new campaign, and an extra scheduling-
policy control did not remove it. See
[`../../docs/android-17-boot-timing-20260824.md`](../../docs/android-17-boot-timing-20260824.md)
for the exact boundary, phase tables, error scan, and retained raw paths.

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
  --android-boot-mode=auto \
  --virtio-console-socket=/absolute/path/to/android17-console.sock \
  --execution-engine=gadgets
```

The executable embeds the accepted ACK ELF and release-manifest identity. It
rejects another release, protects release-owned boot arguments, maps the large
installed GPT image only as the COW backing file, and binds each instance to
the accepted release with an adjacent manifest receipt. `auto` follows the
standard `boot-recovery` command in the COW-backed `misc` partition; `normal`
and `recovery` are explicit diagnostic overrides. Both choices load a bounded
ramdisk range from the immutable GPT rather than a standalone file. Attach to
the console socket with a local Unix-stream client such as `nc -U`; the userdebug shell on
`hvc0` accepts `logcat` directly. Linux printk is deliberately restricted to
`ttyS0`; `hvc0` is selected only through `androidboot.console=hvc0`, so the
interactive Android shell and logcat output cannot be mixed with kernel logs.
Press Enter after attaching to obtain the shell prompt. The current
`uml-darwin` executable and Linux desktop rootfs discovery remain separate.
