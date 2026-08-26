# AOSP 17 UML guest producer

This directory owns the standalone AOSP 17 guest producer for Darwin UML. The
exact AOSP identity and product name live in
[`metadata/aosp.env`](metadata/aosp.env); the ACK identity and generated kernel
series live under
[`../../kernel/android-6.18`](../../kernel/android-6.18/README.md).

The producer runs locally on an x86_64 Linux builder. Repository scripts accept
builder-local paths only; transport, machines, accounts, and credentials are
outside this repository.

## Producer boundary

`build/prepare-sources.sh` is the only producer command allowed to use the
network. It pins the AOSP Repo workspace, ACK workspace, and external Jelly
source. All patching, builds, verification, image assembly, and publication are
offline.

The producer has four independent outputs:

1. a replayed and built ACK 16 KiB kernel;
2. the `aosp_kernarium_darwin-userdebug` target files;
3. one static GPT guest disk containing both boot ramdisks; and
4. a compact rootfs release installed by Apple consumers.

Use the scripts under `build/` in that order. Each script's `--help` is the
canonical interface for builder paths and optional inputs. Ninja and the
preserved AOSP output directory own incremental builds; the wrapper does not
introduce a second build system.

Kernel publication is independent of AOSP/rootfs publication. An already-built
clean ACK result is published with `tools/publish-kernel-artifact.py` and
installed below `.build/kernel/artifacts`. Updating this artifact must not
republish the GPT or invalidate a guest bundle.

## Source and patch ownership

`metadata/base-manifest.xml` pins every Repo project. `patches/projects.tsv`
maps each changed project from its exact base to final tree. Generated mail
series under `patches/projects/` are the only maintained modifications to
upstream AOSP projects.

`overlay/` owns the complete `device/kernarium/darwin` product tree and is
synchronized as one verified unit. External Jelly has its own pinned source
and generated series under `third_party/jelly/`.

Never edit generated patch files. Amend clean Git topic branches and run the
matching regenerator under `tools/`; it updates patch bytes and all related
identity tables together. New local topics use the repository's required
subject prefix.

## Android product

The product is ARM64-only, 16 KiB-only, userdebug, and built-in-kernel-only. It
removes Cuttlefish virtual hardware and services that have no Darwin UML
implementation while preserving Android framework APIs required by ordinary
applications.

The image provides Settings, Launcher/search, Files, Package Installer,
WebView, LatinIME, ThemePicker, and the patched Jelly browser. Unsupported
Bluetooth, Wi-Fi, telephony, GNSS, NFC, UWB, Thread, camera, biometric, sensor,
vibrator, secure-element, USB, and Trusty hardware is not advertised. Ethernet
uses the real Virtio-net/SLiRP path.

The product uses software KeyMint/gatekeeper, SELinux enforcing policy, build-
time artifacts for ordinary applications, no `system_other` image, and no
loadable modules or DLKM partitions. Applications inside updatable APEXes
remain subject to the pinned platform's own ART policy.

## Static disk and boot contract

`support/image/partition-layout.json` owns a fixed 16 GiB GPT with separate
Android payload, metadata, userdata, `init_boot`, `misc`, and recovery
partitions. There is no `super`, dynamic partition, Virtual A/B, AVB, OTA slot,
or DLKM partition.

The immutable Android payload filesystems and APEX payloads are ext4. Userdata
is writable through the host COW. The product does not enable filesystem quota
provisioning. First-stage init mounts fixed GPT partitions by name and does not
use logical, slot-select, or module mount flags.

`init_boot` and `recovery` contain kernelless boot-image-v4 ramdisks. The ACK
ELF remains outside the disk in the selected runtime artifact. The normal and
stock-recovery ramdisks are validated in place and are not duplicated as
standalone release files. Boot mode `auto` follows the standard command in the
COW-backed `misc` partition; explicit normal/recovery selection is diagnostic.

`build/assemble-ramdisk.sh` and `build/assemble-image.sh` own assembly and
verification. The assembler refuses unsafe paths, partition overflow, module
payloads, or replacement of an existing output.

## Release installation

`support/release/release-manifest.schema.json` defines the compact rootfs
release. It binds the fixed GPT, boot ranges, command line, layout, target-file
support, and source provenance. The checked
`metadata/accepted-release.env` selects one reviewed release without recording
a builder path.

After copying a reviewed candidate to the workstation, install it with:

```sh
android/aosp-17/tools/install-release.py \
  --source /absolute/local/path/to/release
```

The installer validates the candidate, expands the compressed GPT as a sparse
local file, revalidates the staged tree, and installs it atomically below
`.build/android/releases`. It never replaces an existing release. Apple CMake
performs read-only installed verification and does not invoke Repo, Soong,
Kbuild, image assembly, publication, or transport.

The release manifest may retain the ACK build that qualified its target files,
but Apple runtime selection uses the independently accepted ACK artifact. The
runtime compares the rootfs manifest identity only, so a kernel-only app update
continues to accept existing `.kern` bundles and their COWs.

## Apple consumers and guest bundles

The macOS Android product is configured with the `macos-android17` preset. It
requires the accepted rootfs release, accepted ACK artifact, and a dedicated
writable instance COW. It does not alter the normal Linux executable or search
for `rootfs.erofs`.

Kernarium represents one Android guest as a `.kern` directory containing:

- `release-manifest.json`;
- `boot/kernel-command-line.txt`;
- exactly one `guest/*.img`; and
- `userdata.cow` after first boot.

The kernel is supplied by the runtime framework and is not bundle payload. A
bundle is selectable only when complete and matching the installed rootfs
release. Android fixes Kernarium's page selection at 16 KiB and appends the
host-owned display density and tagged-pointer option at launch.

Do not recreate a bundle merely to relaunch it; that would discard persistent
guest state. When copying to physical iOS, target the complete
`Documents/<name>.kern` destination rather than the Documents directory. See
[`../../docs/testing.md`](../../docs/testing.md#apple-builds-and-kernarium).

## Validation boundary

Producer verification covers pinned source/patch identity, target files,
VINTF, SELinux policy, executable alignment, module exclusion, partition
layout, boot ramdisks, and release inventory. Runtime boot, persistence,
recovery, graphics, input, networking, connected-device memory limits, and
application compatibility are separate Apple-product gates.

Historical boot, ART, memory, and topology experiments are summarized in
[`../../docs/optimization-history.md`](../../docs/optimization-history.md).
