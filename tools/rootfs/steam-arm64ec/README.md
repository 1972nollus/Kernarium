# Steam ARM64EC gaming rootfs

This producer builds a Debian 13 ARM64 Steam appliance with Gamescope,
Xwayland, the native ARM64 Steam client, and source-built Proton 11. It uses
the emulator's `xtajit64` ARM64EC and `xtajit` i386 WoW64 CPU bridges, including
ARM64EC DXVK and vkd3d-proton. It installs no desktop environment.

The default build retains Debian Mesa. An experimental Vulkan build imports
receipt-verified Mesa/Venus and Vulkan-Loader runtimes from the
[maintained Vulkan guest](../vulkan/README.md). The importer checks the source
epoch and each installed file, validates dynamic dependencies, and selects
only the Virtio ICD. Its receipt is included in the image provenance. This
qualifies neither game rendering nor Vulkan conformance.
The Vulkan gaming session selects Zink for Xwayland and OpenGL clients so
their native-heap buffers can be imported by Venus. VirGL buffers cannot cross
that sharing boundary. The native Steam UI requires both GTK 2 and GTK 3.

The image uses the existing dedicated 4 KiB ARM64EC Wine kernel and emulator.
There is no 16 KiB Steam product. Apple builds consume the accepted prebuilt
kernel; the Vulkan variant requires that current kernel source series built
with the Wine profile, including the shared-memory GPU transport. This
workflow changes neither maintained kernel line. iOS packaging
and automatic image discovery are not part of this producer.

## Build on native ARM64 Linux

Use an ordinary account with passwordless sudo on Debian 13. Preparation
installs host tools, retrieves the SDK and source closure, and prepares a
signed Debian package closure. Gamescope comes from `trixie-backports`.

```sh
dependencies/prepare.sh --product steam-arm64ec-rootfs --root /work/steam-inputs
tools/rootfs/steam-arm64ec/build-proton.sh /work/steam-inputs /work/proton-build 12
tools/rootfs/steam-arm64ec/build.sh --inputs /work/steam-inputs \
  --proton /work/proton-build/build/redist --work /work/steam-image \
  --output /work/steam-arm64ec-gaming.erofs
```

For the Vulkan variant, add `--vulkan-root /work/vulkan/stage/rootfs-kosmickrisp`
and `--vulkan-receipt /work/vulkan/output-kosmickrisp/rootfs-vulkan-venus.receipt.json`
to the image build. These inputs are read-only; only the runtime is imported,
without the CTS payload. Use fresh prepared inputs when the Debian package
closure changes.

Build work and output paths must be absent. Preparation reuses verified
inputs. `dependencies/prepare.sh --product steam-arm64ec-rootfs --root
/work/steam-inputs --verify` performs read-only source, payload, and SDK
verification; `--no-fetch` has the same verification-only behavior for this
product. The Proton and image builders run containers with networking
disabled and never pull images. Standard rootful Podman supports build hosts
where nested rootless containers cannot mount procfs; privileged containers
are not required.

[`sources.json`](sources.json) locks the SDK digest and Steam runtime snapshot.
[`downloads.json`](downloads.json) locks the additional build archives.
The unmodified upstream [`steam-client.manifest`](steam-client.manifest)
pins the Steam client package set and each package's SHA-256. The producer
installs the complete seed; Steam owns its subsequent updater state and
`.installed` manifest. No user account or credentials are included.
Debian package versions are resolved during preparation and recorded with
the base archive digest. Build timestamps and compiler identities remain
authentic.

Proton source identities and generated topics live in
[`dependencies/steam-arm64ec`](../../../dependencies/steam-arm64ec/README.md).
Its Wine 11 module adapter restores private CPU-feature names removed since
Wine 10 while retaining the shared bridge implementation. Wine supplies a
readable ARM64 signal restorer so libgcc can unwind native thread shutdown
without reading the kernel's synthetic signal-return address. Piper's unmodified
upstream dependencies use their own CMake projects and prepared private
prefixes. ARM64 prefixes are created lazily in the guest because the bridge
requires the emulator's hostcall service.

## Image and startup contract

The output includes SHA-256, package and binary inventories, and input
provenance sidecars. The audit requires native ARM64 Steam and Wine launchers,
both CPU bridge DLLs, ARM64EC DXVK and vkd3d DLLs, and both Windows execution
probes. It rejects guest FEX and foreign system ELF binaries. Steam's upstream
package set also contains x86 compatibility assets; these are retained only
under its explicit bundled paths and do not provide an x86 Linux execution
engine.

`steam-gaming.service` starts a Gamescope session as the image-owned `uml`
user. That account has passwordless local sudo and serial autologin, matching
the existing development appliances. Steam data and the registered local
Proton tool use the writable root overlay. The standard client and Proton
runtime downloads remain Steam's responsibility after account login.

## macOS qualification

Place the image at `.build/rootfs/steam-arm64ec-gaming.erofs`, configure the
accepted 4 KiB Wine kernel artifact and verified host prefixes through local
presets, then run:

```sh
cmake --workflow --preset check-steam-arm64ec-macos
```

The gate boots systemd with a disposable overlay and a virtio GPU, starts
Xvfb without a desktop, requires AMD64 exit code 42 and i386 exit code 43,
and repeats the AMD64 launch through Proton. It then starts the actual gaming
service and requires the known Vulkan limitation. Any illegal instruction,
missing runtime dependency, prefix failure, or other gaming-session failure
blocks the gate. This graphics-limitation check applies to the stock Mesa
image. Guest output is saved under the build's `reports/` directory.
The smoke uses a temporary sparse 16 GiB ext4 upper disk. After the guest
mounts it, the harness unlinks its host pathname; the emulator's open disk
descriptor retains the backing until exit, including a forced exit.

For experimental Venus, select `UML_APPLE_GPU_PRODUCT=apple-gpu-vulkan-experiment`,
its verified `UML_APPLE_GPU_DEPENDENCY_PREFIX`, and macOS deployment target
`26.0` in the local configure preset. Select the current Wine-profile kernel
bundle and the Vulkan Steam image as complete inputs. Launch the workflow or
interactive emulator with `UML_GPU_EXPERIMENTAL_VULKAN=1`. The smoke additionally
requires `vulkaninfo` to identify Venus, a Gamescope framebuffer with a valid
pitch, and a running native Steam bootstrap. These checks do not qualify
composed rendering. A running session still requires visual verification of
the login or setup screen.

For an interactive boot with the same development account:

```sh
scripts/boot-overlay-systemd.sh --emulator /path/to/uml-darwin-wine-arm64ec \
  --rootfs /path/to/steam-arm64ec-gaming.erofs --unit graphical.target \
  -- --gpu-display=native
```

## Current Venus startup boundary

On macOS with Mesa 26.2.1 and the paired KosmicKrisp renderer, Gamescope can
register its XR24 framebuffer through Mesa's private scanout-image path.
The native allocation remains optimal and carries a logical pitch for DRM.
Direct Vulkan color scanout reaches VNC through native Metal readback.
AR24 client buffers may still be rejected for direct KMS scanout and require
composition. Composed Vulkan-cube output is visibly corrupted; a nonblank VNC
frame alone is not evidence of correct rendering or a working Steam login UI.
Steam completes its client update and reaches UI-module loading, where
`vgui2_s` cannot load because `libopenal.so.1` is absent. The native client
module closure also lacks `libnm.so.0`. Login and game launch remain
unqualified.
