# Steam ARM64EC gaming rootfs

This producer builds a Debian 13 ARM64 Steam appliance with Gamescope,
Xwayland, the native ARM64 Steam client, and source-built Proton 11. It uses
the emulator's `xtajit64` ARM64EC and `xtajit` i386 WoW64 CPU bridges, including
ARM64EC DXVK and vkd3d-proton. It installs no desktop environment.

Standard Debian Mesa GL and Vulkan packages are placeholders. The current
macOS renderer does not expose the Vulkan device required by Gamescope.
Qualification therefore requires working systemd and Proton CPU execution,
followed by an observed Vulkan initialization failure from the gaming
session. This image does not qualify game rendering or the unfinished Vulkan
driver.

The image uses the existing dedicated 4 KiB ARM64EC Wine kernel and emulator.
There is no 16 KiB Steam product. Apple builds consume the accepted prebuilt
kernel; this workflow changes neither maintained kernel line. iOS packaging
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
blocks the gate. Guest output is saved under the build's `reports/` directory.

For an interactive boot with the same development account:

```sh
scripts/boot-overlay-systemd.sh --emulator /path/to/uml-darwin-wine-arm64ec \
  --rootfs /path/to/steam-arm64ec-gaming.erofs --unit graphical.target \
  -- --gpu-display=native
```
