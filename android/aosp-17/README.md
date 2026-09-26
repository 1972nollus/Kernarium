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

`build/prepare-sources.sh` and `build/prepare-orangefox-source.sh` are the only
producer commands allowed to use the network. The first pins AOSP, ACK, and
external Jelly; the second materializes the independently pinned OrangeFox
14.1/TWRP workspace. All patching, builds, verification, image assembly, and
publication are offline.

The producer has five independent outputs:

1. a replayed and built ACK 16 KiB kernel;
2. the `aosp_kernarium_darwin-user` target files;
3. a kernel-less OrangeFox recovery partition;
4. one static GPT guest disk containing both boot ramdisks; and
5. a compact rootfs release installed by Apple consumers.

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
maps each changed project from its exact base to final tree. Generated patch
series under `patches/projects/` are the only maintained modifications to
upstream AOSP projects.

`overlay/` owns the complete `device/kernarium/darwin` product tree and is
synchronized as one verified unit. External Jelly has its own pinned source
and generated series under `third_party/jelly/`, and the Vulkan HAL's Mesa
source has its own under `third_party/mesa-venus/`.

`third_party/orangefox/` records the official OrangeFox sync identity, the
fully resolved TWRP Repo manifest, the exact OrangeFox-owned project commits,
two generated integration topics, and a small independent Kernarium recovery
device tree. It does not vendor OrangeFox source. Source preparation needs
roughly 85 GiB, matching the upstream `fox_14.1` guidance. The device tree is
synced to the same `device/kernarium/darwin` path, but only inside the separate
OrangeFox workspace.

Every series here uses the shared
[`scripts/patch_series.py`](../../scripts/patch_series.py) format and is
identified by the tree it produces: replay applies it to the pinned base and
requires the final tree recorded in `projects.tsv` or the owning
`metadata.env`. Release manifests compute patch counts and digests from the
series files when they are published.

Never edit generated patch files. Amend clean Git topic branches and run the
matching regenerator under `tools/`; it exports the series and records its
final tree. OrangeFox routes have no dedicated regenerator: export a route with
`scripts/patch_series.py export --license GPL-3.0-or-later` and record the tree
it prints as that route's `final_tree`. Export refuses a topic whose author is
a placeholder; amend it as described in
[`kernel/mainline-6.18/README.md`](../../kernel/mainline-6.18/README.md).

## Android product

The product is ARM64-only, 16 KiB-only, `user`, and built-in-kernel-only. It
removes Cuttlefish virtual hardware and services that have no Darwin UML
implementation while preserving Android framework APIs required by ordinary
applications.

The image provides Settings, Launcher, Files, Package Installer, WebView,
LatinIME, ThemePicker, and the patched Jelly browser. QuickSearchBox and its
home-screen search surface are deliberately omitted. Bluetooth and Wi-Fi
device implementations and feature declarations are also omitted. The Network
dashboard exposes the stock Ethernet interface editor alongside Data Saver,
VPN, and Private DNS. On this radio-free product, the editor enables AOSP's
full Ethernet path and hides its inapplicable Wi-Fi groups. The radio-only
Connected devices entry is hidden, as is System update because the synced AOSP
tree has no activity implementing that intent. The modular framework API
containers remain part of base AOSP so ordinary applications can still resolve
the Android SDK classes; excluding them also removes required system-server
dexpreopt inputs. Telephony,
GNSS, NFC, UWB, Thread, camera, biometric, sensor, vibrator, secure-element,
USB, and Trusty hardware is not advertised. Ethernet uses the real
Virtio-net/SLiRP path.

The vendor audio policy exposes only the implemented 48 kHz stereo primary
playback path, supplies the AOSP software effects configuration, and disables
AAudio MMAP and exclusive modes. Settings omits its Bluetooth media-output
switcher so the remaining volume and vibration controls do not instantiate a
radio-backed media manager. A dedicated Virtio
battery device reports the Apple host's percentage and charging state through
the generic Android Health service; hosts without readable battery data report
an always-present battery at 100 percent and charging. The vendor policy labels
both the standalone macOS and Kernarium iOS Virtio enumeration paths so the
stock Health service can read the battery on every Apple product.

Both the framework and SettingsProvider defaults disable the non-secure
keyguard, set the screen-off timeout to the maximum supported value, and keep
the display awake while the host supplies virtual AC power. LatinIME remains
eligible even while a host keyboard is attached. Fresh settings databases seed
every user-facing audio stream at its maximum and disable UI, lock, dial-pad,
charging, and power sounds; subsequent volume and sound settings remain owned
by the user.

Fresh settings databases also enable Developer options and ADB. The radio-free
product starts adbd on TCP port 5555 and retains the stock authorization prompt
for a newly connected host. The immutable `ro.boot.kernarium.adb_auth` launch
property can disable that check for host-side diagnostics: iOS fixes it to one,
while the standalone macOS product fixes it to zero. Bundle command lines may
not override this host security choice. Android's wireless-debugging setting
uses the active Virtio-net Ethernet interface as its trusted-network identity,
monitors that transport for reconnect and disconnect, and publishes its mDNS
services without requiring a Wi-Fi manager or multicast lock. Fresh
notification state shows silent notification icons in the status bar. Active
DownloadProvider transfers use a silent low-importance channel so their
progress icon remains visible there; later notification changes remain owned
by the user.

The product uses software KeyMint/gatekeeper, SELinux enforcing policy, build-
time artifacts for ordinary applications, no `system_other` image, and no
loadable modules or DLKM partitions. Applications inside updatable APEXes
remain subject to the pinned platform's own ART policy.

## Graphics

The GLES stack is the AOSP `external/mesa3d` Virgl port routed through `p0004`.
The Vulkan HAL is separate: it is Mesa's Venus driver, `VK_DRIVER_ID_MESA_VENUS`
over Virtio-GPU, built from this project's own guest Mesa source rather than
from the AOSP Mesa snapshot, so the Android guest and the Linux Venus guest run
the same driver against the same renderer.

`third_party/mesa-venus/` pins that source as three independent inputs: the
upstream Mesa revision, the guest Vulkan series under
[`../../tools/rootfs/vulkan/`](../../tools/rootfs/vulkan/README.md), and the
generated Android port series beside the metadata. A guest replay assigns new
commit identities each time it is recreated, so the guest half is pinned by
tree. Moving to a newer guest epoch is a metadata bump, a replay, and a
regeneration; the verifier refuses a guest series that changed underneath the
recorded pin.

The port itself is generated. `tools/generate-mesa-venus-port.py` runs AOSP's
`meson2hermetic` converter with the checked `soong-port.toml`, then scopes every
generated module name with a `venus_` prefix because `external/mesa3d` already
defines the same Mesa module names and Soong module names are global at the Make
layer. The installed HAL module keeps the name `vulkan.virtio`, which is the
file name the Android Vulkan loader resolves through `ro.hardware.vulkan`.
Soong has no equivalent of the guest build's out-of-tree
`-Dvenus-protocol-dir`, so the port carries the paired generated Venus protocol
codec at the tree path the upstream snapshot reserves for it.

`vulkan.virtio.so` is a same-process HAL: it is loaded into every application
that uses Vulkan, so the product labels it `same_process_hal_file` beside the
Mesa EGL/GLES libraries.

Mesa reads a buffer's DRM format, modifier and plane layout through gralloc, so
the product installs the allocator's own `gralloc.minigbm` module and sets
`ro.hardware.gralloc=minigbm`: the passthrough module answers none of those
queries and the driver would have to guess the layout of the buffers the window
system hands it. That module is labelled `same_process_hal_file` too. minigbm
allocates a GPU-only buffer as a virtio-gpu blob, which is the form the renderer
can share with the Vulkan render server, and keeps a buffer the guest itself
reads or writes on the classic transfer path, which a CPU mapping still needs.
The compositor's framebuffers are such GPU-only blobs; the host presents each
through the Metal texture that stores its GL texture, as
[`../../docs/graphics.md`](../../docs/graphics.md) describes.

Presentation is `VK_ANDROID_native_buffer`: the platform loader allocates the
swapchain images from gralloc and the driver imports them. Those blobs belong to
the renderer's GL context, and an Apple host has no dma-buf to hand across, so
the renderer gives such a resource a host shared-memory segment and exports
that; the contract is in
[`../../docs/architecture.md`](../../docs/architecture.md). SurfaceFlinger then
takes the presented layer on the scanout as a device composition. The product
ships `kernarium_vk_present`, which owns a layer, wraps it through
`VK_KHR_android_surface` and presents a swapchain, as the pass-or-fail check of
that path.

The product declares `android.hardware.vulkan.version` 1.4, `level` 1 and
`compute` 0, and deliberately declares no `android.software.vulkan.deqp.level`:
this driver has not been qualified against Android's Vulkan dEQP must-pass list.

The Android hardware buffer handle type a swapchain needs is guest Mesa work,
so it arrives with the guest Vulkan epoch pinned in
`third_party/mesa-venus/metadata.env`. The HAL and the host renderer must also
speak the same Venus protocol revision: a guest epoch that changes the paired
protocol leaves an older release's HAL unable to talk to a newer renderer, so
such an epoch needs a port replay and a new release.

## Payload reduction

Payload reduction is done through product configuration and regenerated AOSP
patches, never by deleting files from a completed image. The product omits the
AuthSecret, reboot-escrow, AuthGraph, Secretkeeper, remote Identity Credential,
confirmation-UI, consumer-IR, USB, thermal, tether-offload, input-processor,
conditional-access, netlink-interceptor, and inactive Ranchu-composer HAL
APEXes. None has corresponding Darwin UML hardware. The active DRM composer,
power and memtrack integration, ClearKey DRM, dumpstate diagnostics, software
KeyMint/gatekeeper, and Credential Manager remain.

The fixed image also omits CompOS and other AVF guest payloads, legacy VNDK
31--34 APEXes, OTA and DSU executables, the dynamic-system installer,
Cuttlefish host-monitoring services, process-memory guardian, and unused
vsock, suspend, and tombstone forwarding helpers. These are excluded from the
resolved product package set before image construction. The build and
target-files verifiers reject any excluded module that reappears.

DeviceLock and Virtualization are retained with their boot and system-server
classpath fragments even though their hardware-backed functions are not
advertised. `SystemServiceRegistry` directly links their framework initializer
classes, so removing either APEX breaks Zygote and the platform SDK class
contract. Bluetooth, Wi-Fi, NFC, UWB, telephony, tethering, and RKPD modular
framework containers are retained for the same SDK/API reason. WebView, fonts,
ART and dexpreopt outputs, media codecs, SwiftShader, Mesa/virgl, Health
Connect, AdServices, and OnDevicePersonalization also remain.

Qualification starts with a fresh base expanded to 8 GiB and verifies direct
unencrypted ext4 userdata, absence of `/metadata/vold/metadata_encryption`, a truthful feature
inventory, VINTF and SELinux health, PIN/password changes, Android Keystore key
persistence across reboot, package installation, WebView, audio, networking,
and GLES. Only then are image size, resident processes, and cold and warm boot
time compared and the release accepted.

## Static disk and boot contract

`support/image/partition-layout.json` owns a compact 1,983 MiB seed GPT with
separate Android payload, metadata, `init_boot`, `misc`, recovery, and a
128 MiB userdata filesystem. Userdata is the final partition. Before its first
boot, the Apple app's generic Swift `DiskUtil` expands the bundle-local raw
file to the selected whole-GiB capacity from 4 through 64 GiB and uses the
statically linked GPT fdisk core to relocate the backup GPT and extend
userdata. The standalone macOS wrapper performs the same transactional
host-side operation with the system `truncate` utility and Homebrew `sgdisk`.
Runtime launch has no disk-capacity parameter and rejects a GPT that does not
match the file. The resized image is opened directly as writable storage;
Android launch never constructs or accepts a UBD COW pair. Normal Android asks
the ext4 `resize2fs` tool to fill userdata before mounting `/data`; the tool is
idempotent when the host has not enlarged the partition. OrangeFox compares
the ext4 and partition extents before running `e2fsck` plus `resize2fs`. Neither
guest rewrites or refreshes a live partition table. There is no `super`,
dynamic partition, Virtual A/B, AVB, OTA slot, or DLKM partition.

The immutable Android payload filesystems and APEX payloads are ext4. Userdata
is an ordinary unencrypted ext4 filesystem in the directly writable image; this
emulator does not expose a hardware-backed KeyMint service and the guest bundle
is already protected by the host application sandbox. The product does not
enable filesystem quota provisioning. First-stage init mounts fixed GPT
partitions by name and does not use logical, slot-select, or module mount flags.
Writable metadata and userdata use continuous ext4 discard so deleted guest
extents are promptly returned to the sparse host file through UBD's batched
discard path.

`init_boot` and `recovery` contain kernelless boot-image-v4 ramdisks. The ACK
ELF remains outside the disk in the selected runtime artifact. `init_boot`
comes from the AOSP target files; the final recovery partition is the separately
built and verified OrangeFox image, never the stock target-files copy. Neither
ramdisk is duplicated as a standalone release file. Boot mode `auto` follows
the standard command in the directly writable `misc` partition; explicit
normal/recovery selection is diagnostic.

OrangeFox is built page-size agnostic with 64 KiB ELF alignment. Its two
upstream 4 KiB-only helper prebuilts are not shipped: toybox supplies `ps`, and
MagiskBoot image repacking is intentionally absent because the signed ACK ELF
is app-owned rather than stored in an Android boot partition. Mount, format,
backup/restore, file-manager, terminal, ADB, and reboot paths remain available.
Its BGRA minui path writes the BGRX byte order required by Virtio-GPU's fixed
XRGB legacy dumb-buffer resources. The virtual-DRM topic also labels NanoSVG
and generated RGBA resources as BGRA so pixelflinger converts their channels
instead of leaving image and shaped-control colors transposed.
Recovery screen blanking, its separate idle lock timeout, haptics, and USB-only
device paths are disabled for the virtual hardware. The pinned recovery cgroup
and task-profile maps keep OrangeFox 14.1's logd/logcat path functional.

`build/build-orangefox.sh`, `build/assemble-ramdisk.sh`, and
`build/assemble-image.sh` own recovery construction, disk assembly, and
verification. The assembler requires an explicit verified OrangeFox image and
refuses unsafe paths, partition overflow, module payloads, or replacement of an
existing output.

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
does not import the rootfs release and does not invoke Repo, Soong, Kbuild,
image assembly, publication, or transport.

The release manifest may retain the ACK build that qualified its target files,
but that value is historical provenance. Apple runtime selection uses an
independent compatible ACK artifact and performs no rootfs, kernel commit, or
emulator release comparison at launch. A bundle's expanded disk remains usable
across compatible kernel updates within the `6.18` line.

## Apple consumers and guest bundles

The macOS Android product is configured with the `macos-android17` preset. Its
build requires a compatible ACK artifact; launch takes any rootfs directory
with the expected payload layout and one directly writable GPT image. It
does not alter the normal Linux executable or search for `rootfs.erofs`.

[`../../scripts/boot_android.py`](../../scripts/boot_android.py) is the ordinary
macOS launcher. It selects the accepted installed release, copies its seed GPT
to a persistent writable image on first boot, expands that copy to 8 GiB,
exposes guest ADB only on host loopback, waits for the transport, and disconnects
it when the guest stops. Later boots reuse the same image directly. When the
selected release identity changes, the launcher replaces the persistent image
with a newly expanded copy of that release. The default session uses
the existing `macos-android17` executable with native Metal display, 10 vCPUs,
8 GiB of RAM, the gadgets engine, and a 720-by-1280 portrait display. Pass
`--build` to refresh that executable incrementally or `--recovery` to boot
OrangeFox; run `scripts/boot_android.py --help` for other overrides.

Kernarium represents one Android guest as a `.kern` directory containing:

- `release-manifest.json`;
- `boot/kernel-command-line.txt`;
- exactly one directly writable `guest/*.img`.

The kernel is supplied by the runtime framework and is not bundle payload. The
manifest is provenance rather than a runtime compatibility token, and legacy
bundles without it remain selectable. Missing files and malformed disk images
fail when opened or parsed. Android fixes Kernarium's page selection at 16 KiB
and appends the host-owned display density, ADB authorization policy, and
tagged-pointer option at launch. Simulator launches write persistent guest
changes directly into the image inside the `.kern` bundle.

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
